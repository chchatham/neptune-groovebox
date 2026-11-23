#pragma once

// THE OLD CLASS, FOR THE TESTS ONLY. The resonator engine RSN (ResonatorEngine.h) as it was at
// 46fedbc, before phase U4-RSN of .ralph/spec-unify.md moved its modulation onto mod::Router;
// only the class names and the include paths were changed, and one test hook added: sameSeeds(true) gives each
// voice's note the noise seed the new engine draws (spread::Exciter of a generator seeded with
// the engine's seed, 0 unless given, and the voice's number), so that the suite "U4 RSN" can compare old and new sample by sample
// where the exciter makes noise. It is not part of the product. Do not edit it: it is the reference.

#include "../../src/engine/EngineBase.h"
#include "../../src/engine/VoiceAllocator.h"
#include "../../src/engine/VoiceFilterSection.h"
#include "../../src/engine/EngineLfoSection.h"
#include "../../src/engine/VoiceSpread.h"
#include "../../src/engine/SampleBank.h"
#include "../../src/dsp/Envelope.h"
#include "../../src/dsp/EnvelopeChain.h"
#include "../../src/dsp/DSPUtils.h"
#include "../../src/dsp/Resonator.h"
#include "OldResonatorVoice.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// ResonatorEngine — VoiceAllocator + OldResonatorVoice behind EngineBase.
//
//   RSN    MRPH XMPH DCAY ENRG | TUNE FINE VSEN LEVL
//   BODY   STIF SHT  SHEL ODD  | DAMP TILT HUM  POS      (added to what MRPH gives)
//   BOD2   MEMB DROP WIRE MEET | SPRD PAIR HAND DIR      (the body's second eight, added likewise)
//   EXC    CONT GRAN COUP REED | HARD CURV IN   LAG      (the first five added to what XMPH gives)
//   GRAIN  SMPL GPOS SCAN SPRY | SIZE GPIT PJIT SHAP     (what GRAN's impulses are read from)
//   AMP    ATK DEC SUS REL | CHAN STGS COMB BEND
//   FILTR, F ENV (VoiceFilterSection), SLT1 ..., LSHP (the mod slots and the custom LFO shape:
//   EngineModSection.h), STRIP, LFO1, LFO2 (EngineLfoSection)
// ---------------------------------------------------------------------------
class OldResonatorEngine : public EngineBase
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

    explicit OldResonatorEngine(int maxVoices = 8)
        : allocator(maxVoices)
    {
        voices.resize((size_t) maxVoices);
        generators.resize((size_t) maxVoices);
        for (int v = 0; v < maxVoices; ++v)
            generators[(size_t) v].seed(0, v);

        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            if (seeds)      // (the test hook: the new engine's draw, from the same generator)
            {
                auto& g = generators[(size_t) voiceIdx];
                g.advance();
                const uint32_t a = (uint32_t) (g.unipolar(spread::Exciter, 0) * 16777216.0f);
                const uint32_t b = (uint32_t) (g.unipolar(spread::Exciter, 1) * 16777216.0f);
                voices[(size_t) voiceIdx].setNoiseSeed(((a << 8) ^ b) | 1u);
            }
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
        lfoSection.applyAll(EngineLfoSection::each(voices));
        res::series();      // built now, never on the audio thread
    }

    // The LFO destinations: pitch, level, pan, cutoff and resonance, then the continuous cells of
    // the pages and the other LFO.
    // Not offered: CURV, CHAN, STGS, COMB (choices and counts), TUNE, FINE and BEND (PTCH is
    // the pitch), VSEN and LAG (how a note starts), and what the filter leaves out.
    static const LfoDestList& lfoDests()
    {
        static const char* const fixed[OldResonatorVoice::NumLfoDests] = { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO" };
        static const LfoDestList list = []
        {
            const ParamPages p = ownPages();
            LfoDestList l(fixed, OldResonatorVoice::NumLfoDests);
            l.group();
            for (int id : { (int) MorphParam, (int) ExciterMorphParam, (int) DecayParam, (int) EnergyParam })
                l.add(p, id);
            l.group();
            for (int i = 0; i < res::NumBody; ++i)
                l.add(p, BodyBase + i);
            l.group();
            for (int i = 0; i < res::NumExcite; ++i)
                l.add(p, ExciteBase + i);
            l.add(p, InputParam);
            l.group();
            for (int id : { (int) LevelParam, (int) AmpAttack, (int) AmpDecay, (int) AmpSustain, (int) AmpRelease })
                l.add(p, id);
            VoiceFilterSection::addLfoDests(l, p, FilterBase);
            l.addCross();
            // The grains (last: what was in the list before them keeps its place). Not SMPL and
            // SHAP (choices); SIZE is GSIZ here, as PJIT is the sampler's name for the same thing.
            l.group();
            l.add(p, GrainPosParam);
            l.add(p, GrainScanParam);
            l.add(p, GrainSprayParam);
            l.add(p, GrainSizeParam, "GSIZ");
            l.add(p, GrainPitchParam);
            l.add(p, GrainJitterParam);
            // [U2b] The body's second eight (last again).
            l.group();
            for (int i = 0; i < res::NumMore; ++i)
                l.add(p, MoreBase + i);
            // The mod strip until the STRIP page says otherwise: more energy (bow, breath and
            // scrape harder), less damping, a quarter-tone of vibrato.
            l.stripRoute(0, l.indexOfKey(EnergyParam), 1.0f);
            l.stripRoute(1, l.indexOfKey(BodyBase + res::Damp), -0.1f);
            l.stripVibrato(0.25f);
            return l;
        }();
        static const bool registered = [] { list.registerGroups(); return true; }();
        juce::ignoreUnused(registered);
        return list;
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

    ParamPages getParamPages() const override
    {
        ParamPages out = ownPages();
        filterSection.updateModeCell(out);
        lfoSection.appendPages(out, lfoDests().labels());
        lfoSection.insertModPages(out, lfoDests().labels());
        return out;
    }

    // The pages without the mod slots, STRIP and the two LFO pages.
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

    void setTempo(float bpm) override { lfoSection.setTempo(bpm, EngineLfoSection::each(voices)); }
    void pitchBend(float amount) override { bend = std::isfinite(amount) ? juce::jlimit(-1.0f, 1.0f, amount) : 0.0f; }
    void modWheel(float value) override { wheel = std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : 0.0f; }

    EngineType getType() const override { return EngineType::Resonator; }
    juce::String getName() const override { return "Resonator"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr >= 1.0 ? sr : 44100.0;
        for (auto& v : voices)
            v.setSampleRate(sampleRate);
        inputLevelK = (float) (1.0 - std::exp(-1.0 / (0.005 * sampleRate)));
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        const int numSamples = buffer.getNumSamples();

        // Once per block: the strip's routes (STRIP page), tuning, bend and vibrato, and each
        // voice's pressure: the exciter's energy (an EXC route), level, cutoff, vibrato.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, sampleRate);
        lfoSection.stripBlock(wheelNow, numSamples, sampleRate, EngineLfoSection::each(voices),
                              [this](int key) { setParameter(key, getParameter(key)); });
        vibratoPhase += 5.5 * numSamples / sampleRate;
        vibratoPhase -= std::floor(vibratoPhase);
        const float vib = std::sin((float) vibratoPhase * dsp::TWO_PI_F);
        const float basePitch = tune + fine / 100.0f + bend * bendRange;
        const AftertouchRouter& router = aftertouchRouter ? *aftertouchRouter : allocator.getAftertouchRouter();
        const bool exciteRouted = router.hasRoute(AftertouchRouter::Destination::Excite);
        const bool slotsOn = lfoSection.slotsOn();
        for (int v = 0; v < (int) voices.size(); ++v)
        {
            const float pressure = v < allocator.getVoiceCount() ? allocator.getAftertouch(v) : 0.0f;
            if (slotsOn)        // what the mod slots read once a block: this voice's pad, the strip, the input's follower
                voices[(size_t) v].getLfos().setBlockSources(pressure, wheelNow, audioInput.envelope);
            AftertouchRouter::ModValues m;
            if (pressure > 0.0f)
                m = router.computeModulation(pressure);
            auto& voice = voices[(size_t) v];
            voice.setPressure(exciteRouted, pressure, m.exciteMod);
            voice.setGainMod(juce::jlimit(0.0f, 2.0f, 1.0f + m.volumeMod));
            voice.setPitchOffset(basePitch + vib * m.vibratoMod);
            voice.getFilter().setExternalOctaves(filterSection.externalOctaves(wheelNow, m.cutoffMod));
        }

        // The track's input, as one channel; its follower is given once a block and glided to.
        const bool hasInput = audioInput.connected();
        const float* inL = hasInput ? audioInput.left : nullptr;
        const float* inR = hasInput && audioInput.right != nullptr ? audioInput.right : inL;
        const int inFrames = hasInput ? audioInput.frames : 0;
        const float levelTarget = hasInput && std::isfinite(audioInput.envelope) ? juce::jlimit(0.0f, 1.0f, audioInput.envelope) : 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float in = i < inFrames ? 0.5f * (inL[i] + inR[i]) : 0.0f;
            inputLevel += (levelTarget - inputLevel) * inputLevelK;

            // An LFO routed to PAN moves its own voice between the channels (balance law).
            float sampleL = 0.0f, sampleR = 0.0f;
            for (auto& voice : voices)
            {
                if (! voice.isActive())
                    continue;
                // LFOs on page cells: this voice's own value of each (as of its last sample).
                if (voice.getLfos().keyed())
                    voice.getLfos().forEachKeyed([&voice](int key, float value) { applyVoiceParam(voice, key, value); });
                const float s = voice.process(in, inputLevel);
                const float pan = voice.getLfoPan();
                sampleL += pan > 0.0f ? s * (1.0f - pan) : s;
                sampleR += pan < 0.0f ? s * (1.0f + pan) : s;
            }
            sampleL *= MASTER_LEVEL;
            sampleR *= MASTER_LEVEL;

            if (right)
            {
                left[i] += sampleL;
                right[i] += sampleR;
            }
            else
            {
                left[i] += 0.5f * (sampleL + sampleR);
            }
        }

        lfoSection.endBlock(numSamples, EngineLfoSection::each(voices));
        lfoSection.afterLetGo([this](int key) { setParameter(key, getParameter(key)); });
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
            lfoSection.baseChanged(id, filterSection.get(id), EngineLfoSection::each(voices));
            return;
        }
        if (lfoSection.owns(id))
        {
            // A cell an LFO has let go of goes back to its set value on every voice.
            const int released = lfoSection.set(id, value, EngineLfoSection::each(voices),
                                                [this](int key) { return getParameter(key); });
            if (released >= 0)
                setParameter(released, getParameter(released));
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
        lfoSection.baseChanged(id, value, EngineLfoSection::each(voices));
    }

    float getParameter(ParamId id) const override
    {
        if (filterSection.owns(id))
            return filterSection.get(id);
        if (lfoSection.owns(id))
            return lfoSection.get(id);
        if (StageCurveParam::owns(id))
            return stageCurves.get(id);
        return id >= 0 && id < NumOwnParams ? own[(size_t) id] : 0.0f;
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (filterSection.owns(id))
            return VoiceFilterSection::name(id - FilterBase);
        if (EngineLfoSection::owns(id))
            return EngineLfoSection::name(id);
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

    void sameSeeds(bool on, uint32_t engineSeed = 0)      // (the test hook)
    {
        seeds = on;
        for (int v = 0; v < (int) generators.size(); ++v)
            generators[(size_t) v].seed(engineSeed, v);
    }
    OldResonatorVoice& getVoice(int index) { return voices[(size_t) index]; }
    int getNumVoices() const { return (int) voices.size(); }
    VoiceAllocator& getAllocator() { return allocator; }

    // --- [U2b] One voice set from a stored sound --------------------------------------
    // A cell of a sound (the ids above, and the filter's) given to one voice alone, wherever
    // that voice lives: the kit's resonator drum is set from the same presets this way. What
    // belongs to the engine and not to a voice (TUNE, FINE, BEND, the envelope chain, the mod
    // slots and LFOs) is passed over; SMPL is found in the bank (not on the audio thread).
    static void applySound(OldResonatorVoice& voice, ParamId id, float value)
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
            return OldResonatorVoice::BodyFirst + (id - BodyBase);
        if (id >= ExciteBase && id < ExciteBase + res::NumExcite)
            return OldResonatorVoice::ExciteFirst + (id - ExciteBase);
        if (id >= MoreBase && id < MoreBase + res::NumMore)
            return OldResonatorVoice::MoreFirst + (id - MoreBase);
        switch (id)
        {
            case MorphParam:        return OldResonatorVoice::Morph;
            case ExciterMorphParam: return OldResonatorVoice::ExciterMorph;
            case DecayParam:        return OldResonatorVoice::Decay;
            case EnergyParam:       return OldResonatorVoice::Energy;
            case InputParam:        return OldResonatorVoice::Input;
            case VelSenseParam:     return OldResonatorVoice::VelSense;
            case LagParam:          return OldResonatorVoice::Lag;
            case GrainPosParam:     return OldResonatorVoice::GrainPos;
            case GrainScanParam:    return OldResonatorVoice::GrainScan;
            case GrainSprayParam:   return OldResonatorVoice::GrainSpray;
            case GrainSizeParam:    return OldResonatorVoice::GrainSize;
            case GrainPitchParam:   return OldResonatorVoice::GrainPitch;
            case GrainJitterParam:  return OldResonatorVoice::GrainJitter;
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

    // One voice's own value of a page cell: what an LFO or a strip route on that cell sets,
    // sample by sample.
    static void applyVoiceParam(OldResonatorVoice& voice, ParamId id, float value)
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

    // The chain's stages are copies of the amp envelope (as on the macro oscillator).
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
    double vibratoPhase = 0.0;
    int envChainStageCount = 2;
    VoiceFilterSection filterSection { FilterBase };
    EngineLfoSection lfoSection { lfoDests() };
    VoiceAllocator allocator;
    std::vector<OldResonatorVoice> voices;
    std::array<float, NumOwnParams> own {};
    std::vector<spread::Generator> generators;
    bool seeds = false;
};
