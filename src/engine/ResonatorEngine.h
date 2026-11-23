#pragma once

#include "EngineBase.h"
#include "VoiceAllocator.h"
#include "VoiceFilterSection.h"
#include "ModRouter.h"
#include "VoiceSpread.h"
#include "SampleBank.h"
#include "../dsp/Envelope.h"
#include "../dsp/EnvelopeChain.h"
#include "../dsp/DSPUtils.h"
#include "../dsp/Resonator.h"
#include "ResonatorVoice.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// ResonatorEngine — VoiceAllocator + ResonatorVoice behind EngineBase.
//
//   RSN    MRPH XMPH DCAY ENRG | TUNE FINE VSEN LEVL
//   BODY   STIF SHT  SHEL ODD  | DAMP TILT HUM  POS      (added to what MRPH gives)
//   BOD2   MEMB DROP WIRE MEET | SPRD PAIR HAND DIR      (the body's second eight, added likewise)
//   EXC    CONT GRAN COUP REED | HARD CURV IN   LAG      (the first five added to what XMPH gives)
//   GRAIN  SMPL GPOS SCAN SPRY | SIZE GPIT PJIT SHAP     (what GRAN's impulses are read from)
//   AMP    ATK DEC SUS REL | CHAN STGS COMB BEND
//   FILTR, F ENV (VoiceFilterSection), then the modulation router's (mod::Router, ModRouter.h):
//   XIN, SLT1 .., LSHP, STRIP, LFO1, LFO2
//
// [U4-RSN] Everything that moves the sound goes through the router: the two LFOs, the strip's
// routes and its vibrato, the sixteen free slots, the track input at audio rate (XIN) and the
// track's pressure routes (AT ROUTE) - the pad's pressure as the exciter's energy is the hidden
// destination EXC (mod::Steps: it follows the pad as it is; the voice has a lag of its own, LAG).
// Per sample: PTCH (a ratio), AMP (a gain), PAN, CUT (octaves); everything else once a sub-block
// (16 samples at 48 kHz: the resonator itself takes its cells every 16, Resonator::controlPeriod).
// RSN has no ANLG page: the per-note randoms (RAND, RND2 ..) and the exciter's noise (stream
// spread::Exciter: where a note's noise, grains and wires start) are draws of one generator a
// voice, seeded with 0 and the voice's number, so a render repeats.
// ---------------------------------------------------------------------------
class ResonatorEngine : public EngineBase
{
public:
    enum Param
    {
        MorphParam      = 0,   // the body's position, 0..5
        ExciterMorphParam = 1, // the exciter's position, 0..3
        DecayParam      = 2,   // seconds
        EnergyParam     = 3,
        TuneParam       = 4,   // semitones
        FineParam       = 5,   // cents
        VelSenseParam   = 6,
        LevelParam      = 7,
        BodyBase        = 8,   // 8-15: STIF SHT SHEL ODD DAMP TILT HUM POS (res::Body), -1..+1
        ExciteBase      = 16,  // 16-20: CONT GRAN COUP REED HARD (res::Excite), -1..+1
        CurveParam      = 21,  // 0 = PHYS, n = curve n - 1 of dsp/Saturation.h
        InputParam      = 22,
        LagParam        = 23,  // seconds
        AmpAttack       = 24,
        AmpDecay        = 25,
        AmpSustain      = 26,
        AmpRelease      = 27,
        EnvChainMode    = 28,  // 0=off (single ADSR), 1=series, 2=parallel, 3=RING (the envelope is the energy alone)
        EnvChainStages  = 29,
        EnvChainCombine = 30,
        BendRangeParam  = 31,  // semitones
        GrainSample     = 32,  // 0 = OFF (noise impulses), n = sample n - 1 of the SampleBank
        GrainPosParam   = 33,  // where in the sample grains start, 0..1
        GrainScanParam  = 34,  // how fast that place moves on, x the sample's own speed (0 = it stays)
        GrainSprayParam = 35,  // each grain up to +-250 ms from there
        GrainSizeParam  = 36,  // seconds
        GrainPitchParam = 37,  // semitones
        GrainJitterParam = 38, // each grain up to +-12 semitones off (squared)
        GrainShapeParam = 39,  // grn::Window
        MoreBase        = 40,  // 40-47: MEMB DROP WIRE MEET SPRD PAIR HAND DIR (res::More), -1..+1
        NumOwnParams    = 48,
        FilterBase      = 100  // 100-115: the per-voice filter (VoiceFilterSection offsets)
    };

    static constexpr int NUM_BODIES = res::NumBodyPoints;
    static constexpr int NUM_EXCITERS = res::NumExcitePoints;
    static constexpr int NUM_CURVES = (int) sat::Count + 1;

    static const char* bodyName(int point)
    {
        static const char* const n[NUM_BODIES] = { "STRG", "TUBE", "COMB", "PLAT", "BAR", "BELL", "HEAD", "GONG",
                                                   "TAM", "CRSH", "RIDE", "CUP", "CHNA", "SPLS" };
        return n[juce::jlimit(0, NUM_BODIES - 1, point)];
    }
    static const char* exciterName(int point)
    {
        static const char* const n[NUM_EXCITERS] = { "STRK", "BOW", "BRTH", "SCRP", "BRSH" };
        return n[juce::jlimit(0, NUM_EXCITERS - 1, point)];
    }

    explicit ResonatorEngine(int maxVoices = 8)
        : allocator(maxVoices)
    {
        voices.resize((size_t) maxVoices);
        mods.resize((size_t) maxVoices);
        spreads.resize((size_t) maxVoices);
        cutMoved.assign((size_t) maxVoices, 0);
        // The per-note randoms and the exciter's noise repeat from run to run: the same notes in
        // the same order on a new engine draw the same values (no ANLG page, so no SEED cell).
        for (int v = 0; v < maxVoices; ++v)
            spreads[(size_t) v].seed(0, v);

        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            auto& g = spreads[(size_t) voiceIdx];
            g.advance();
            mods[(size_t) voiceIdx].noteOn(router, v.note, v.velocity, g);
            voices[(size_t) voiceIdx].setNoiseSeed(noteSeeds ? 0u : exciterSeed(g));
            voices[(size_t) voiceIdx].setNote(v.note, v.velocity);
            voices[(size_t) voiceIdx].noteOn();
        });
        allocator.setNoteOffCallback([this](int voiceIdx, const Voice&) { voices[(size_t) voiceIdx].noteOff(); });
        // A new note takes a silent voice while there is one: release tails ring out.
        allocator.setVoiceSoundingQuery([this](int voiceIdx) { return voices[(size_t) voiceIdx].isActive(); });

        // Seed every parameter so getParameter is always the true value.
        for (auto& page : getParamPages())
            for (auto& p : page.params)
                if (p.isValid())
                    setParameter(p.id, p.defaultVal);
        filterSection.applyAll(VoiceFilterSection::each(voices));
        res::series();      // built now, never on the audio thread
    }

    // The exciter's noise from the note's number instead of the per-note draw: the same note, the
    // same noise, as the kit's resonator drum makes it - so that a sound on the keys gives the very
    // samples of the same sound on a pad ("Resonator Drum" compares the two hosts). Off by default.
    void setNoteSeededNoise(bool on) { noteSeeds = on; }

    // Starts every voice's per-note draws again from this seed (0 is the engine's own). Not on the
    // audio thread. (What a SEED cell will call when RSN gets the ANLG page; the tests use it.)
    void seedVoices(uint32_t engineSeed)
    {
        for (int v = 0; v < (int) spreads.size(); ++v)
            spreads[(size_t) v].seed(engineSeed, v);
    }

    // Where a note's noise starts (the exciter's grain, the cloud of a sample's grains, the wires):
    // two draws of the voice's generator on the stream spread::Exciter, as it stands for this note.
    static uint32_t exciterSeed(const spread::Generator& g)
    {
        const uint32_t a = (uint32_t) (g.unipolar(spread::Exciter, 0) * 16777216.0f);
        const uint32_t b = (uint32_t) (g.unipolar(spread::Exciter, 1) * 16777216.0f);
        return ((a << 8) ^ b) | 1u;
    }

    // What can be moved (the modulation router's table), in the order the LFO and slot DEST cells
    // stored before the port (a stored DEST keeps its meaning): pitch, level, pan, cutoff and
    // resonance, the continuous cells of the pages, LRAT / LDPT, the grains, the body's second
    // eight; then the envelopes' times ETIM / FTIM, and hidden (a pressure route's only) the
    // exciter's energy EXC.
    // Per sample (the track input at audio rate reaches them): PTCH, AMP, PAN, CUT. The rest once a
    // sub-block: the resonator takes its latents, positions and decay every 16 samples at 48 kHz
    // and glides to them by itself, so a per-sample value would be read at that rate all the same.
    // Not offered: CURV, CHAN, STGS, COMB, SMPL, SHAP (choices and counts), TUNE, FINE and BEND
    // (PTCH is the pitch), VSEN and LAG (how a note starts), and what the filter leaves out.
    enum Dest { DestOff = 0, DestPitch, DestAmp, DestPan, DestCutoff, DestResonance, NumFixedDests };
    static const mod::DestTable& lfoDests()
    {
        static const mod::DestTable table = []
        {
            const ParamPages p = ownPages();
            mod::DestTable t;
            t.group();
            t.input("PTCH", 1.0f, 1.0f, 1.0f / 1024.0f, 1024.0f, mod::Pitch);      // a ratio: one octave x the amount squared
            t.input("AMP", 1.0f, 1.0f, 0.0f, 4.0f, mod::PerSample).audioFloor(-4.0f);  // (below 0: ring modulation)
            t.input("PAN", 0.0f, 1.0f, -1.0f, 1.0f, mod::PerSample);
            t.input("CUT", 0.0f, VoiceFilter::PRESSURE_OCTAVES, -16.0f, 16.0f, mod::PerSample);     // octaves
            t.input("RESO", 0.0f, 0.5f, -1.0f, 1.0f);
            t.group();
            for (int id : { (int) MorphParam, (int) ExciterMorphParam, (int) DecayParam, (int) EnergyParam })
                t.add(p, id);
            t.group();
            for (int i = 0; i < res::NumBody; ++i)
                t.add(p, BodyBase + i);
            t.group();
            for (int i = 0; i < res::NumExcite; ++i)
                t.add(p, ExciteBase + i);
            t.add(p, InputParam);
            t.group();
            for (int id : { (int) LevelParam, (int) AmpAttack, (int) AmpDecay, (int) AmpSustain, (int) AmpRelease })
                t.add(p, id);
            VoiceFilterSection::addLfoDests(t, p, FilterBase);
            t.addCross();
            // The grains. Not SMPL and SHAP (choices); SIZE is GSIZ here, as PJIT is the sampler's
            // name for the same thing.
            t.group();
            t.add(p, GrainPosParam);
            t.add(p, GrainScanParam);
            t.add(p, GrainSprayParam);
            t.add(p, GrainSizeParam, "GSIZ");
            t.add(p, GrainPitchParam);
            t.add(p, GrainJitterParam);
            // [U2b] The body's second eight.
            t.group();
            for (int i = 0; i < res::NumMore; ++i)
                t.add(p, MoreBase + i);
            // [U4-RSN] The envelopes' times (after what was there before).
            t.group();
            t.envTimes();
            // The pad's pressure as the exciter's energy (the pressure route EXC): it follows the pad
            // as it is (LAG is the voice's own smoothing), up to what the voice takes.
            t.hidden("EXC", 0.0f, 1.0f, 0.0f, ResonatorVoice::MAX_ENERGY, mod::Steps);
            // The mod strip until the STRIP page says otherwise: more energy (bow, breath and
            // scrape harder), less damping, a quarter-tone of vibrato.
            t.stripRoute(0, "ENRG", 1.0f);
            t.stripRoute(1, "DAMP", -0.1f);
            t.stripVibrato(0.25f);
            // Pad pressure until AT ROUTE says otherwise: the exciter's energy (the instrument is
            // played by it: a bow, a breath, a scrape as hard as the pad is pressed).
            t.pressureRoute(0, "EXC", 1.0f);
            t.finish(2);
            return t;
        }();
        static const bool registered = [] { table.registerGroups(); return true; }();
        juce::ignoreUnused(registered);
        return table;
    }
    static int envTimeDest()    { return lfoDests().ampTime(); }
    static int filterTimeDest() { return lfoDests().filterTime(); }
    static int energyDest()     { static const int d = lfoDests().indexOfLabel("EXC"); return d; }

    // What an old pressure route (AftertouchRouter::Destination) is here, as a slot's DEST.
    // (TIMB, FM and WPOS do nothing on RSN.)
    int pressureDestOf(AftertouchRouter::Destination d) const override
    {
        using D = AftertouchRouter::Destination;
        switch (d)
        {
            case D::Volume:         return DestAmp;
            case D::FilterCutoff:   return DestCutoff;
            case D::PanSpread:      return DestPan;
            case D::Excite:         return energyDest();
            default:                return 0;
        }
    }

    // SMPL is a pointer to a sample that stays: nothing is rebuilt, but it is found (and measured,
    // the first time) where it is set, which is no work for a step lock on the audio thread.
    bool isRebuildParam(ParamId id) const override { return id == GrainSample; }

    // OFF, then the bank's samples as they stand now (recordings are added while the app runs).
    static const char* const* grainSampleNames(int& count)
    {
        static std::array<const char*, SampleBank::MAX_ENTRIES + 1> names {};
        auto& bank = SampleBank::get();
        const int n = std::min(bank.size(), (int) SampleBank::MAX_ENTRIES);
        names[0] = "OFF";
        for (int i = 0; i < n; ++i)
            names[(size_t) i + 1] = bank.names()[i];
        count = n + 1;
        return names.data();
    }

    // A bank sample as the exciter reads it: its left channel where it lies (entries are never
    // moved or freed) and what brings it to the level GRAIN_LEVEL (a recording has most of its
    // power low, where the body's strong partials are: at the level of the noise it stands in for
    // it would drive the body some 10 dB harder). Message thread; measured once a sample.
    static constexpr double GRAIN_LEVEL = 0.35;
    static const res::GrainSource* grainSource(int bankIndex)
    {
        static std::array<res::GrainSource, SampleBank::MAX_ENTRIES> table {};
        const auto* e = SampleBank::get().entry(bankIndex);
        if (e == nullptr || bankIndex >= (int) SampleBank::MAX_ENTRIES || e->left.size() < 2)
            return nullptr;
        auto& g = table[(size_t) bankIndex];
        if (g.data == nullptr)
        {
            double sum = 0.0;
            for (float x : e->left)
                sum += (double) x * (double) x;
            const double rms = std::sqrt(sum / (double) e->left.size());
            g.frames = (int) e->left.size();
            g.rate = e->sampleRate >= 1.0 ? e->sampleRate : 44100.0;
            g.gain = rms > 1.0e-4 && std::isfinite(rms) ? (float) std::min(GRAIN_LEVEL / rms, 32.0) : 1.0f;
            g.data = e->left.data();
        }
        return &g;
    }

    mod::Router* modRouter() override { return &router; }
    const mod::Router* modRouter() const override { return &router; }

    ParamPages getParamPages() const override
    {
        ParamPages out = ownPages();
        filterSection.updateModeCell(out);
        router.appendPages(out);        // XIN, SLT1 .., LSHP, STRIP, LFO1, LFO2
        return out;
    }

    // The pages without the modulation router's.
    static ParamPages ownPages()
    {
        static const char* const chain[] = { "OFF", "SER", "PAR", "RING" };
        static const char* const combine[] = { "SUM", "MAX" };
        static const char* const* const curves = []
        {
            static const char* names[NUM_CURVES];
            names[0] = "PHYS";
            for (int i = 0; i < (int) sat::Count; ++i)
                names[i + 1] = sat::names()[i];
            return (const char* const*) names;
        }();
        static const char* const bodyCells[res::NumBody] = { "STIF", "SHT", "SHEL", "ODD", "DAMP", "TILT", "HUM", "POS" };
        static const char* const exciteCells[res::NumExcite] = { "CONT", "GRAN", "COUP", "REED", "HARD" };
        static const char* const moreCells[res::NumMore] = { "MEMB", "DROP", "WIRE", "MEET", "SPRD", "PAIR", "HAND", "DIR" };
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;

        static const char* const windows[grn::NumWindows] = { "HANN", "TRI", "PERC", "SWEL" };
        int numSamples = 0;
        const char* const* samples = grainSampleNames(numSamples);

        ParamPages out(6);
        out[0].name = "RSN"; out[0].section = "SRC";
        out[0].params = {{
            { "MRPH", MorphParam, 0, (float) (NUM_BODIES - 1), 0, L, "x", nullptr, 0, 0.05f },
            { "XMPH", ExciterMorphParam, 0, (float) (NUM_EXCITERS - 1), 0, L, "x", nullptr, 0, 0.05f },
            { "DCAY", DecayParam, 0.05f, 20, 2.0f, G, "s" },
            { "ENRG", EnergyParam, 0, 1, 0.5f, L },
            { "TUNE", TuneParam, -24, 24, 0, I, "st" },
            { "FINE", FineParam, -100, 100, 0, L, "ct" },
            { "VSEN", VelSenseParam, 0, 1, 1, L },
            { "LEVL", LevelParam, 0, 1, 0.8f, L } }};
        out[1].name = "BODY"; out[1].section = "SRC";
        for (int i = 0; i < res::NumBody; ++i)
            out[1].params[(size_t) i] = { bodyCells[i], BodyBase + i, -1, 1, 0, L };
        out[2].name = "BOD2"; out[2].section = "SRC";
        for (int i = 0; i < res::NumMore; ++i)
            out[2].params[(size_t) i] = { moreCells[i], MoreBase + i, -1, 1, 0, L };
        out[3].name = "EXC"; out[3].section = "SRC";
        for (int i = 0; i < res::NumExcite; ++i)
            out[3].params[(size_t) i] = { exciteCells[i], ExciteBase + i, -1, 1, 0, L };
        out[3].params[5] = { "CURV", CurveParam, 0, (float) (NUM_CURVES - 1), 0, C, "", curves, NUM_CURVES };
        out[3].params[6] = { "IN",   InputParam, 0, 1, 0, L };
        out[3].params[7] = { "LAG",  LagParam, 0.001f, 0.5f, 0.015f, G, "s" };
        out[4].name = "GRAIN"; out[4].section = "SRC";
        out[4].params = {{
            { "SMPL", GrainSample, 0, (float) (numSamples - 1), 0, C, "", samples, numSamples },
            { "GPOS", GrainPosParam, 0, 1, 0, L },
            { "SCAN", GrainScanParam, -2, 2, 1, L, "x", nullptr, 0, 0.05f },
            { "SPRY", GrainSprayParam, 0, 1, 0, L },
            { "SIZE", GrainSizeParam, res::Resonator::MIN_GRAIN, res::Resonator::MAX_GRAIN, 0.03f, G, "s" },
            { "GPIT", GrainPitchParam, -24, 24, 0, L, "st", nullptr, 0, 1.0f },
            { "PJIT", GrainJitterParam, 0, 1, 0, L },
            { "SHAP", GrainShapeParam, 0, (float) (grn::NumWindows - 1), 0, C, "", windows, grn::NumWindows } }};
        out[5].name = "AMP"; out[5].section = "AMP";
        out[5].params = {{
            { "ATK",  AmpAttack, 0.001f, 10, 0.002f, G, "s" },
            { "DEC",  AmpDecay, 0.001f, 10, 0.3f, G, "s" },
            { "SUS",  AmpSustain, 0, 1, 0.8f, L },
            { "REL",  AmpRelease, 0.001f, 10, 0.5f, G, "s" },
            { "CHAN", EnvChainMode, 0, 3, 0, C, "", chain, 4 },
            { "STGS", EnvChainStages, 1, 4, 2, I },
            { "COMB", EnvChainCombine, 0, 1, 0, C, "", combine, 2 },
            { "BEND", BendRangeParam, 0, 24, 2, I, "st" } }};
        VoiceFilterSection::appendPages(out, FilterBase);
        return out;
    }

    void setTempo(float bpm) override { router.setTempo(bpm); }
    void pitchBend(float amount) override { bend = std::isfinite(amount) ? juce::jlimit(-1.0f, 1.0f, amount) : 0.0f; }
    void modWheel(float value) override { wheel = std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : 0.0f; }

    EngineType getType() const override { return EngineType::Resonator; }
    juce::String getName() const override { return "Resonator"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr >= 1.0 ? sr : 44100.0;
        router.prepare(sampleRate);
        for (auto& v : voices)
            v.setSampleRate(sampleRate);
        for (auto& m : mods)
            m.prepare(sampleRate);
        inputLevelK = (float) (1.0 - std::exp(-1.0 / (0.005 * sampleRate)));
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        const int numSamples = buffer.getNumSamples();

        // Once per block: tuning and bend, the strip (the cutoff by the filter's WHL amount; its
        // routes are the router's), the track's pressure routes and the track input.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, sampleRate);
        const float basePitch = tune + fine / 100.0f + bend * bendRange;
        const float wheelOctaves = filterSection.externalOctaves(wheelNow, 0.0f);
        for (auto& voice : voices)
        {
            voice.setPitchOffset(basePitch);
            voice.getFilter().setExternalOctaves(wheelOctaves);
        }
        const bool hasInput = audioInput.connected();
        router.beginBlock(audioInput.left, audioInput.right, hasInput ? audioInput.frames : 0, audioInput.envelope, wheelNow, sampleRate);
        // The pressure route EXC: the pad's pressure is the exciter's energy.
        const bool exciteRouted = router.routed(energyDest());

        // The track's input, as one channel; its follower is given once a block and glided to.
        const float* inL = hasInput ? audioInput.left : nullptr;
        const float* inR = hasInput && audioInput.right != nullptr ? audioInput.right : inL;
        const int inFrames = hasInput ? audioInput.frames : 0;
        const float levelTarget = hasInput && std::isfinite(audioInput.envelope) ? juce::jlimit(0.0f, 1.0f, audioInput.envelope) : 0.0f;

        // Sub-blocks: the router's control rate. Inside one the voices are rendered one after the
        // other (they share the router's per-sample buffers).
        for (int offset = 0; offset < numSamples;)
        {
            const int n = router.beginChunk(offset, numSamples);
            for (int i = 0; i < n; ++i)
            {
                const int at = offset + i;
                inNow[(size_t) i] = at < inFrames ? 0.5f * (inL[at] + inR[at]) : 0.0f;
                inputLevel += (levelTarget - inputLevel) * inputLevelK;
                levelNow[(size_t) i] = inputLevel;
            }
            std::fill(mixL.begin(), mixL.begin() + n, 0.0f);
            std::fill(mixR.begin(), mixR.begin() + n, 0.0f);
            for (int v = 0; v < (int) voices.size(); ++v)
            {
                auto& voice = voices[(size_t) v];
                auto& m = mods[(size_t) v];
                if (! voice.isActive())
                {
                    // Its LFOs keep time; a cell it was left holding goes back to its set value.
                    if (m.idle(router, n))
                        m.restore(router, [&](int dest, float value) { setControl(v, dest, value); });
                    continue;
                }
                const float pressure = v < allocator.getVoiceCount() ? allocator.getAftertouch(v) : 0.0f;
                renderVoice(v, n, pressure, exciteRouted);
            }
            for (int i = 0; i < n; ++i)
            {
                const float sampleL = mixL[(size_t) i] * MASTER_LEVEL, sampleR = mixR[(size_t) i] * MASTER_LEVEL;
                if (right)
                {
                    left[offset + i] += sampleL;
                    right[offset + i] += sampleR;
                }
                else
                {
                    left[offset + i] += 0.5f * (sampleL + sampleR);
                }
            }
            offset += n;
        }
    }

    void releaseResources() override
    {
        for (auto& v : voices)
            v.reset();
    }

    void noteOn(int note, float velocity, int /*voiceIndex*/) override { allocator.noteOn(note, velocity); }
    void noteOff(int note, int /*voiceIndex*/) override { allocator.noteOff(note); }

    void allNotesOff() override
    {
        soundCut();
        allocator.allNotesOff();
        for (auto& v : voices)
            v.reset();
    }

    void aftertouch(int note, float pressure) override
    {
        allocator.setAftertouch(note, std::isfinite(pressure) ? juce::jlimit(0.0f, 1.0f, pressure) : 0.0f);
    }

    // ---- Per-stage envelope curves (StageCurveParam): the amp envelope is envelope slot 0 -------
    int getStageCurveParam(ParamId cellId) const override
    {
        if (cellId == AmpAttack)  return StageCurveParam::id(0, StageCurveParam::Attack);
        if (cellId == AmpDecay)   return StageCurveParam::id(0, StageCurveParam::Decay);
        if (cellId == AmpRelease) return StageCurveParam::id(0, StageCurveParam::Release);
        return filterSection.stageCurveParamFor(cellId);
    }

    void setParameter(ParamId id, float value) override
    {
        if (filterSection.owns(id))
        {
            if (filterSection.swaps(id, value))
                soundSwapped();
            filterSection.set(id, value, VoiceFilterSection::each(voices));
            router.setBase(id, filterSection.get(id));
            return;
        }
        if (mod::Router::owns(id))
        {
            router.set(id, value);
            return;
        }
        if (StageCurveParam::owns(id))
        {
            setStageCurve(id, value);
            return;
        }
        if (id < 0 || id >= NumOwnParams || ! std::isfinite(value))
            return;
        const ParamSpec& spec = specOf(id);
        // (SMPL's list grows while the app runs: it is held to the bank as it stands now.)
        value = id == GrainSample ? juce::jlimit(0.0f, (float) SampleBank::get().size(), value) : spec.clamp(value);
        if (spec.isDiscrete())
            value = std::round(value);
        if ((id == CurveParam || id == EnvChainMode || id == EnvChainStages || id == EnvChainCombine) && own[(size_t) id] != value)
            soundSwapped();
        own[(size_t) id] = value;
        applyParameter(id, value);
        router.setBase(id, value);
    }

    float getParameter(ParamId id) const override
    {
        if (filterSection.owns(id))
            return filterSection.get(id);
        if (mod::Router::owns(id))
            return router.get(id);
        if (StageCurveParam::owns(id))
            return stageCurves.get(id);
        return id >= 0 && id < NumOwnParams ? own[(size_t) id] : 0.0f;
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (filterSection.owns(id))
            return VoiceFilterSection::name(id - FilterBase);
        if (mod::Router::owns(id))
            return mod::Router::name(id);
        static const char* const body[res::NumBody] = { "Stiffness", "Sheet", "Shell", "Odd", "Damping", "Tilt", "Hum", "Position" };
        static const char* const excite[res::NumExcite] = { "Continuity", "Grain", "Coupling", "Shape", "Hardness" };
        static const char* const more[res::NumMore] = { "Membrane", "Pitch Drop", "Wires", "Bow Meets", "Bow Spread", "Pairs",
                                                        "Hand-Placed", "Direct" };
        if (id >= MoreBase && id < MoreBase + res::NumMore)
            return more[id - MoreBase];
        if (id >= BodyBase && id < BodyBase + res::NumBody)
            return body[id - BodyBase];
        if (id >= ExciteBase && id < ExciteBase + res::NumExcite)
            return excite[id - ExciteBase];
        switch (id)
        {
            case MorphParam:      return "Body Morph";
            case ExciterMorphParam: return "Exciter Morph";
            case DecayParam:      return "Decay";
            case EnergyParam:     return "Energy";
            case LevelParam:      return "Level";
            case CurveParam:      return "Junction Curve";
            case InputParam:      return "Input";
            case AmpAttack:       return "Attack";
            case AmpDecay:        return "Amp Decay";
            case AmpSustain:      return "Sustain";
            case AmpRelease:      return "Release";
            case TuneParam:       return "Tune";
            case BendRangeParam:  return "Bend Range";
            case VelSenseParam:   return "Velocity";
            case LagParam:        return "Energy Lag";
            case GrainSample:     return "Grain Sample";
            case GrainPosParam:   return "Grain Position";
            case GrainScanParam:  return "Grain Scan";
            case GrainSprayParam: return "Grain Spray";
            case GrainSizeParam:  return "Grain Size";
            case GrainPitchParam: return "Grain Pitch";
            case GrainJitterParam: return "Grain Pitch Jitter";
            case GrainShapeParam: return "Grain Shape";
            case FineParam:       return "Fine";
            case EnvChainMode:    return "Env Chain";
            case EnvChainStages:  return "Env Stages";
            case EnvChainCombine: return "Env Combine";
            default:              return "Param " + juce::String(id);
        }
    }

    int getParameterCount() const override { return NumOwnParams; }

    ResonatorVoice& getVoice(int index) { return voices[(size_t) index]; }
    int getNumVoices() const { return (int) voices.size(); }
    // This voice's side of the modulation router (its LFOs, its sources as of the last sub-block).
    mod::Voice& getMod(int index) { return mods[(size_t) index]; }
    const mod::Router& getRouter() const { return router; }
    VoiceAllocator& getAllocator() { return allocator; }

    // --- [U2b] One voice set from a stored sound --------------------------------------
    // A cell of a sound (the ids above, and the filter's) given to one voice alone, wherever
    // that voice lives: the kit's resonator drum is set from the same presets this way. What
    // belongs to the engine and not to a voice (TUNE, FINE, BEND, the envelope chain, the mod
    // slots and LFOs) is passed over; SMPL is found in the bank (not on the audio thread).
    static void applySound(ResonatorVoice& voice, ParamId id, float value)
    {
        if (! std::isfinite(value))
            return;
        if (id >= FilterBase && id < FilterBase + 16)
        {
            // (The filter's type and mode are the section's to give out: here to one filter alone.)
            if (id == FilterBase + VoiceFilterSection::Type)
                voice.getFilter().setModel(FilterSlot::modelFromIndex(std::clamp((int) std::lround(value), 0, FilterSlot::NUM_MODELS - 1)));
            else if (id == FilterBase + VoiceFilterSection::Mode)
                voice.getFilter().setMode(std::max(0, (int) std::lround(value)));
            else
                VoiceFilterSection::applyTo(voice.getFilter(), id - FilterBase, value);
            return;
        }
        if (StageCurveParam::owns(id))      // (the amp envelope's own three curves)
        {
            if (StageCurveParam::envelopeOf(id) == 0)
            {
                auto p = voice.getAmpEnvelope().getParams();
                const float curve = juce::jlimit(-1.0f, 1.0f, value);
                const int stage = (id - StageCurveParam::BASE) % StageCurveParam::STAGES;
                if (stage == StageCurveParam::Attack)       p.attackCurve = curve;
                else if (stage == StageCurveParam::Decay)   p.decayCurve = curve;
                else                                        p.releaseCurve = curve;
                voice.getAmpEnvelope().setParams(p);
            }
            return;
        }
        if (id < 0 || id >= NumOwnParams)
            return;
        value = id == GrainSample ? juce::jlimit(0.0f, (float) SampleBank::get().size(), value) : specOf(id).clamp(value);
        if (id == CurveParam)
            voice.setCurve((int) std::lround(value));
        else if (id == EnvChainMode)
            voice.setRingOut((int) std::lround(value) == 3);
        else if (id == GrainSample)
            voice.setGrainSource(value >= 1.0f ? grainSource((int) value - 1) : nullptr);
        else if (id == GrainShapeParam)
            voice.setGrainWindow((int) std::lround(value));
        else
            applyVoiceParam(voice, id, value);
    }
    // The cell with this id: its range and default; and the voice's cell behind it (-1: none).
    static const ParamSpec& cellSpec(int id) { return specOf(id); }
    static int voiceCell(ParamId id) { return cellOf(id); }

private:
    static constexpr float MASTER_LEVEL = 0.5f;

    // The cell with this id (its range and kind are the same for every model and exciter).
    static const ParamSpec& specOf(int id)
    {
        static const std::array<ParamSpec, NumOwnParams> specs = []
        {
            std::array<ParamSpec, NumOwnParams> s;
            for (auto& page : ownPages())
                for (auto& p : page.params)
                    if (p.isValid() && p.id < NumOwnParams)
                        s[(size_t) p.id] = p;
            return s;
        }();
        return specs[(size_t) juce::jlimit(0, (int) NumOwnParams - 1, id)];
    }

    // The voice's cell behind a parameter id, -1 if it is none.
    static int cellOf(ParamId id)
    {
        if (id >= BodyBase && id < BodyBase + res::NumBody)
            return ResonatorVoice::BodyFirst + (id - BodyBase);
        if (id >= ExciteBase && id < ExciteBase + res::NumExcite)
            return ResonatorVoice::ExciteFirst + (id - ExciteBase);
        if (id >= MoreBase && id < MoreBase + res::NumMore)
            return ResonatorVoice::MoreFirst + (id - MoreBase);
        switch (id)
        {
            case MorphParam:        return ResonatorVoice::Morph;
            case ExciterMorphParam: return ResonatorVoice::ExciterMorph;
            case DecayParam:        return ResonatorVoice::Decay;
            case EnergyParam:       return ResonatorVoice::Energy;
            case InputParam:        return ResonatorVoice::Input;
            case VelSenseParam:     return ResonatorVoice::VelSense;
            case LagParam:          return ResonatorVoice::Lag;
            case GrainPosParam:     return ResonatorVoice::GrainPos;
            case GrainScanParam:    return ResonatorVoice::GrainScan;
            case GrainSprayParam:   return ResonatorVoice::GrainSpray;
            case GrainSizeParam:    return ResonatorVoice::GrainSize;
            case GrainPitchParam:   return ResonatorVoice::GrainPitch;
            case GrainJitterParam:  return ResonatorVoice::GrainJitter;
            default:                return -1;
        }
    }

    StageCurves stageCurves;

    void setStageCurve(ParamId id, float value)
    {
        stageCurves.set(id, value);
        if (StageCurveParam::envelopeOf(id) != 0)
            return;
        for (auto& v : voices)
        {
            auto p = v.getAmpEnvelope().getParams();
            stageCurves.applyTo(p, 0);
            v.getAmpEnvelope().setParams(p);
        }
        refreshChainStages();
    }

    // One sub-block of voice v (n samples, added to mixL / mixR): the router's tick, then the plain
    // path when nothing moves the voice, else its control-rate destinations and the per-sample ones.
    void renderVoice(int v, int n, float pressure, bool exciteRouted)
    {
        auto& voice = voices[(size_t) v];
        auto& m = mods[(size_t) v];
        const bool moved = m.tick(router, n, voice.getAmpEnvelope().getCurrentLevel(), voice.getFilter().getEnvelope().getCurrentLevel(), pressure);
        const int exc = energyDest();
        voice.setPressure(exciteRouted, pressure, m.moved(exc) ? m.value(exc) : 0.0f);
        if (! moved)
        {
            // Nothing moves this voice: the resonator as it is without any modulation.
            if (cutMoved[(size_t) v])
            {
                voice.getFilter().setLfoOctaves(0.0f);
                cutMoved[(size_t) v] = 0;
            }
            voice.setPitchMod(1.0f);
            voice.setAmpMod(1.0f);
            for (int i = 0; i < n && voice.isActive(); ++i)
            {
                const float s = voice.process(inNow[(size_t) i], levelNow[(size_t) i]);
                mixL[(size_t) i] += s;
                mixR[(size_t) i] += s;
            }
            return;
        }

        m.forEachControl([&](int dest, float value) { setControl(v, dest, value); });
        const float* pitch = m.buffer(DestPitch);
        const float* amp = m.buffer(DestAmp);
        const float* pan = m.buffer(DestPan);
        const float* cutoff = m.buffer(DestCutoff);
        if (cutoff == nullptr && cutMoved[(size_t) v])
            voice.getFilter().setLfoOctaves(0.0f);
        cutMoved[(size_t) v] = cutoff != nullptr ? 1 : 0;
        if (pitch == nullptr) voice.setPitchMod(1.0f);
        if (amp == nullptr)   voice.setAmpMod(1.0f);
        for (int i = 0; i < n && voice.isActive(); ++i)
        {
            if (pitch)  voice.setPitchMod(pitch[i]);
            if (amp)    voice.setAmpMod(amp[i]);
            if (cutoff) voice.getFilter().setLfoOctaves(cutoff[i]);
            const float s = voice.process(inNow[(size_t) i], levelNow[(size_t) i]);
            // PAN moves its own voice between the channels (balance law: the centre is unchanged).
            const float p = pan ? pan[i] : 0.0f;
            mixL[(size_t) i] += p > 0.0f ? s * (1.0f - p) : s;
            mixR[(size_t) i] += p < 0.0f ? s * (1.0f + p) : s;
        }
    }

    // A control-rate destination of the router on voice v: the resonance, the envelopes' times, or
    // one of the voice's page cells. (EXC is read where the energy is given: renderVoice.)
    void setControl(int v, int dest, float value)
    {
        auto& voice = voices[(size_t) v];
        if (dest == DestResonance)
            voice.getFilter().setResonanceMod(value);
        else if (dest == envTimeDest())
        {
            // ETIM: every stage time of the amp envelope, and of the chain's stages, times this.
            voice.getAmpEnvelope().setTimeScale(value);
            auto& chain = voice.getEnvelopeChain();
            for (int s = 0; s < chain.getStageCount(); ++s)
                chain.getEnvelopeMut(s).setTimeScale(value);
        }
        else if (dest == filterTimeDest())
            voice.getFilter().setEnvTimeScale(value);
        else if (dest != energyDest())
        {
            const int id = router.dests()[dest].id;
            if (id >= 0)
                applyVoiceParam(voice, id, value);
        }
    }

    // One voice's own value of a page cell: what a slot routed to that cell sets, once a
    // sub-block (and what the drum kit gives its resonator drum).
    static void applyVoiceParam(ResonatorVoice& voice, ParamId id, float value)
    {
        if (id >= FilterBase)
        {
            VoiceFilterSection::applyTo(voice.getFilter(), id - FilterBase, value);
            return;
        }
        const int cell = cellOf(id);
        if (cell >= 0)
        {
            voice.setCell(cell, value);
            return;
        }
        if (id == LevelParam)
        {
            voice.setLevel(value);
            return;
        }
        auto setEnvelope = [id, value](Envelope& e)
        {
            switch (id)
            {
                case AmpAttack:  e.setAttack(value); break;
                case AmpDecay:   e.setDecay(value); break;
                case AmpSustain: e.setSustain(value); break;
                case AmpRelease: e.setRelease(value); break;
                default: break;
            }
        };
        setEnvelope(voice.getAmpEnvelope());
        auto& chain = voice.getEnvelopeChain();
        for (int s = 0; s < chain.getStageCount(); ++s)
            setEnvelope(chain.getEnvelopeMut(s));
    }

    void applyParameter(ParamId id, float value)
    {
        const int cell = cellOf(id);
        if (cell >= 0)
        {
            for (auto& v : voices) v.setCell(cell, value);
            return;
        }
        switch (id)
        {
            case CurveParam:
                for (auto& v : voices) v.setCurve((int) value);
                break;
            case GrainSample:
            {
                const res::GrainSource* source = value >= 1.0f ? grainSource((int) value - 1) : nullptr;
                for (auto& v : voices) v.setGrainSource(source);
                break;
            }
            case GrainShapeParam:
                for (auto& v : voices) v.setGrainWindow((int) value);
                break;
            case LevelParam:
                for (auto& v : voices) v.setLevel(value);
                break;
            case TuneParam:       tune = value; break;
            case FineParam:       fine = value; break;
            case BendRangeParam:  bendRange = value; break;
            case EnvChainMode:
            {
                const int modeVal = (int) value;
                // (RING: one envelope, which is the energy alone; the body is heard until it has died.)
                for (auto& v : voices)
                    v.setRingOut(modeVal == 3);
                if (modeVal == 0 || modeVal == 3)
                {
                    for (auto& v : voices)
                        v.setEnvelopeChainEnabled(false);
                }
                else
                {
                    const auto chainMode = modeVal == 1 ? EnvelopeChain::ChainMode::Series : EnvelopeChain::ChainMode::Parallel;
                    for (auto& v : voices)
                    {
                        v.setEnvelopeChainEnabled(true);
                        v.getEnvelopeChain().setMode(chainMode);
                    }
                    // A chain needs stages: build them from the current envelope if there are none.
                    if (voices.empty() || voices[0].getEnvelopeChain().getStageCount() == 0)
                        rebuildEnvelopeChains(juce::jmax(1, envChainStageCount));
                }
                break;
            }
            case EnvChainStages:
                rebuildEnvelopeChains((int) value);
                break;
            case EnvChainCombine:
                for (auto& v : voices)
                    v.getEnvelopeChain().setCombineMode((int) value == 0 ? EnvelopeChain::CombineMode::Sum : EnvelopeChain::CombineMode::Max);
                break;
            case AmpAttack:
            case AmpDecay:
            case AmpSustain:
            case AmpRelease:
                for (auto& v : voices)
                {
                    auto p = v.getAmpEnvelope().getParams();
                    if (id == AmpAttack)       p.attack = value;
                    else if (id == AmpDecay)   p.decay = value;
                    else if (id == AmpSustain) p.sustain = value;
                    else                       p.release = value;
                    v.getAmpEnvelope().setParams(p);
                }
                refreshChainStages();
                break;
            default:
                break;
        }
    }

    // The chain's stages are copies of the amp envelope (as on the macro oscillator). [U4-RSN] This
    // plumbing - CHAN / STGS / COMB, rebuildEnvelopeChains, refreshChainStages, the chain in
    // applyVoiceParam and in ETIM - is the macro oscillator's and the wave sequencer's, copied: one
    // shared envelope-chain section is due (spec-unify U4), not folded here (the macro is another
    // agent's while this was written).
    void rebuildEnvelopeChains(int stages)
    {
        envChainStageCount = stages;
        for (auto& v : voices)
        {
            auto& chain = v.getEnvelopeChain();
            EnvelopeChain newChain;
            newChain.setSampleRate(sampleRate);
            newChain.setMode(chain.getMode());
            newChain.setCombineMode(chain.getCombineMode());
            const auto envParams = v.getAmpEnvelope().getParams();
            for (int s = 0; s < stages; ++s)
                newChain.addEnvelope(envParams);
            v.getEnvelopeChain() = newChain;
        }
    }

    void refreshChainStages()
    {
        for (auto& v : voices)
        {
            auto& chain = v.getEnvelopeChain();
            const auto p = v.getAmpEnvelope().getParams();
            for (int s = 0; s < chain.getStageCount(); ++s)
                chain.getEnvelopeMut(s).setParams(p);
        }
    }

    double sampleRate = 44100.0;
    float tune = 0.0f, fine = 0.0f, bend = 0.0f, bendRange = 2.0f, wheel = 0.0f;
    float wheelNow = 0.0f;                              // the strip, smoothed per block
    float inputLevel = 0.0f, inputLevelK = 0.005f;      // the input's follower, glided to
    int envChainStageCount = 2;
    VoiceFilterSection filterSection { FilterBase };
    mod::Router router { lfoDests(), 2 };
    VoiceAllocator allocator;
    std::vector<ResonatorVoice> voices;
    std::vector<mod::Voice> mods;                       // each voice's side of the router
    std::vector<spread::Generator> spreads;             // each voice's per-note draws
    std::vector<uint8_t> cutMoved;                      // the voice's filter holds a cutoff from the router
    bool noteSeeds = false;                             // setNoteSeededNoise
    std::array<float, mod::Router::MAX_CHUNK> mixL {}, mixR {}, inNow {}, levelNow {};
    std::array<float, NumOwnParams> own {};
};
