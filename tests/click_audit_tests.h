#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"

// -------------------------------------------------------------------------------------------------
// The click audit. A click is a jump nobody asked for: the output moving from one sample to the next
// by much more than the sound's own waveform does. The rule the engines keep (owner, 2026-11-14):
//
//     smooth only the jumps nobody asked for, over about 1-4 ms; never soften an attack the
//     player set.
//
// Every test here plays a steady, sine-like note on an engine (so the waveform's own steps are small
// and a jump stands out), makes something change abruptly under it, and compares the largest
// sample-to-sample step in the few milliseconds after the change with the largest anywhere else in
// the same render:
//
//     step at the change  <=  REL x step elsewhere  +  ABS x peak
//
// What changes abruptly:
//   - an LFO with an edge in its shape (square, ramp up, ramp down, random), on EVERY destination
//     the engine offers (the list is read from the engine's own DEST cell, so a new destination is
//     audited the day it is added), and an LFO switched off under a held note (DEST to OFF, DPTH
//     to 0);
//   - the mod strip jumping 0 -> 1 -> 0 through a route, and pad pressure jumping through every
//     pressure destination;
//   - a step lock (a parameter set under a held note) on every continuous cell of the engine's
//     pages, and - on a track, as the app plays it - a selector turned on every choice cell;
//   - what steps by design: the wavetable's STEP, RAND and LOOP scans, a wave sequence without a
//     crossfade (they have to step to the ear, not jump in one sample);
//   - the same key struck again while it sounds, and a note that steals a voice at full polyphony:
//     here the render is compared with the same last note played from silence, so the test also
//     proves that the new note starts at full strength at once (the difference between the two is
//     the old sound going away, and it has to be smooth). The sampler plays a cosine and the kick
//     machine its knock: attacks that start on a value of their own;
//   - the engine replaced or parked under a held note, and in the mixer: volume, pan, send level,
//     an insert switched off and on, exchanged and removed, the TRK FX blocks, Shift + Play's
//     silence.
//
// A destination where a step is the sound that was asked for, or a cell that is set at once on
// purpose, is named in an allow-list below with its reason; everything else has to pass. A
// destination that does nothing at all in the audit's patch has to be named too (lfoInert), so
// that a new one cannot slip through unheard.
//
// Kept short (about half a minute without optimisation): the square is tried on every destination;
// the other shapes, DPTH to 0 and the strip on the first twelve and every fifth after (sampled());
// renders are a quarter to half a second.
//
// What this cannot see: anything that is not a jump (zipper noise below the threshold, a thump, a
// pitch step, a level that moves in 4 ms where the ear wanted 20), and sounds that are noise
// already (a snare, a hat and its choke, the cymbal machine): there a jump hides among the
// waveform's own. The metronome, the MASTR page's knobs and the sampler's slice and stretch
// destinations are not played here.
//
// NEPTUNE_CLICK_AUDIT_VERBOSE=1 prints every measurement.
// -------------------------------------------------------------------------------------------------
class ClickAuditTests : public juce::UnitTest
{
public:
    ClickAuditTests() : UnitTest("Click Audit") {}

    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;
    static constexpr float REL = 1.5f;      // times the largest step elsewhere in the render
    static constexpr float ABS = 0.03f;     // plus this share of the render's peak

    using S = EngineLfoSection;

    struct Audio
    {
        std::vector<float> l, r;
    };

    // ------------------------------------------------------------------------------- the engines
    struct Subject
    {
        const char* name = "";
        EngineType type = EngineType::UnifiedVoice;
        std::function<std::unique_ptr<EngineBase>()> make;      // prepared; a steady, sine-like, sustaining sound
        int low = 46, high = 70;    // (not A: the events fall on whole blocks, which an A's cycles would line up with)
        int voices = 8;             // polyphony; 0 = one voice per note (drum pads)
        bool unified = false;       // its LFOs are the ModParam ones
        std::vector<const char*> only;      // when not empty: only these destinations are audited on it
        bool everything = true;     // false: an extra patch of an engine above, for its LFO destinations only

        bool audits(const juce::String& dest) const
        {
            if (only.empty())
                return true;
            for (auto* o : only)
                if (dest == o)
                    return true;
            return false;
        }
    };

    // A filter that is doing something (FM, macro, wavetable, wave sequencer): partly closed, with
    // some envelope on it.
    static void voiceFilter(EngineBase& e, int base)
    {
        using F = VoiceFilterSection;
        e.setParameter(base + F::Cutoff, 2500.0f);
        e.setParameter(base + F::EnvAmount, 0.3f);
        e.setParameter(base + F::Sustain, 0.6f);
    }

    static std::vector<Subject> subjects()
    {
        std::vector<Subject> out;
        {
            Subject s; s.name = "Unified"; s.type = EngineType::UnifiedVoice; s.unified = true;
            s.make = []
            {
                auto e = std::make_unique<UnifiedVoiceEngine>(8);
                e->prepareToPlay(SR, BLOCK);
                e->setParameter(UnifiedParam::DRIFT, 0.0f);
                for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                    e->getVoice(v).getAnalogCharacter().enabled = false;
                // Two sines an octave apart, a sub and a little dark noise: every mixer level has
                // something to scale.
                e->setParameter(UnifiedParam::WAVE1, (float) (int) UnifiedVoice::OscWaveform::Sine);
                e->setParameter(UnifiedParam::WAVE2, (float) (int) UnifiedVoice::OscWaveform::Sine);
                e->setParameter(UnifiedParam::MIX, 0.3f);
                e->setParameter(UnifiedParam::SUB, 0.3f);
                e->setParameter(UnifiedParam::NOISE, 0.1f);
                e->setParameter(UnifiedParam::NOISE_COLOR, 1.0f);
                e->setParameter(UnifiedParam::CUTOFF, 4000.0f);
                e->setParameter(UnifiedParam::FILTER_ENV, 0.2f);
                e->setParameter(UnifiedParam::FENV_SUSTAIN, 0.6f);
                e->setParameter(UnifiedParam::MODEL_B, 2.0f);          // a second filter beside the first
                e->setParameter(UnifiedParam::ROUTING, 2.0f);
                e->setParameter(UnifiedParam::AMP_ATTACK, 0.001f);
                e->setParameter(UnifiedParam::AMP_SUSTAIN, 1.0f);
                e->setParameter(UnifiedParam::VEL_SENS, 0.0f);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            Subject s; s.name = "FM"; s.type = EngineType::FM;
            s.make = []
            {
                auto e = std::make_unique<FMEngine>();
                e->prepareToPlay(SR, BLOCK);
                e->setParameter(FMEngine::Op2Level, 0.25f);     // a mild two-operator tone
                e->setParameter(FMEngine::Op1Sustain, 1.0f);
                e->setParameter(FMEngine::Op1Attack, 0.001f);
                voiceFilter(*e, FMEngine::FilterBase);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            // The operators' positions need operators that read a table: six carriers, each on the
            // vowel table (every place along it another wave), from the middle of it.
            Subject s; s.name = "FM WAVE"; s.type = EngineType::FM;
            s.only = { "POS1", "POS2", "POS3", "POS4", "POS5", "POS6" };
            s.make = []
            {
                auto e = std::make_unique<FMEngine>();
                e->prepareToPlay(SR, BLOCK);
                e->setParameter(FMEngine::AlgorithmParam, 5.0f);
                for (int op = 0; op < 6; ++op)
                {
                    e->setParameter(op * 10 + 1, 0.25f);
                    e->setParameter(op * 10 + 4, 0.001f);
                    e->setParameter(op * 10 + 6, 1.0f);
                    e->setParameter(op * 10 + FMEngine::OP_WAVE, (float) (phaseosc::FIRST_TABLE_WAVE + 9));
                    e->setParameter(op * 10 + FMEngine::OP_POSITION, 0.5f);
                }
                voiceFilter(*e, FMEngine::FilterBase);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            Subject s; s.name = "Macro"; s.type = EngineType::MacroOsc;
            s.make = []
            {
                auto e = std::make_unique<MacroOscEngine>();
                e->prepareToPlay(SR, BLOCK);
                e->setParameter(MacroOscEngine::ModelParam, 1.0f);      // two-operator FM: a sine at index 0
                e->setParameter(MacroOscEngine::TimbreParam, 0.1f);
                e->setParameter(MacroOscEngine::AmpAttack, 0.001f);
                e->setParameter(MacroOscEngine::AmpSustain, 1.0f);
                voiceFilter(*e, MacroOscEngine::FilterBase);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            // (U4-MACRO) BODY's destinations need the body in: the same steady tone through a plate.
            Subject s; s.name = "Macro BODY"; s.type = EngineType::MacroOsc; s.everything = false;
            s.only = { "BMIX", "BMRP", "BDCY", "BCUP", "BDMP", "BPOS", "BTUN", "BKTK",
                        "BCNT", "BGRN", "BRED", "BHRD", "BRDM", "BKDC", "BDIR", "BWIR" };
            const auto plain = out.back().make;
            s.make = [plain]
            {
                auto e = plain();
                e->setParameter(MacroOscEngine::BodyBase + VoiceBody::Mix, 0.7f);
                e->setParameter(MacroOscEngine::BodyBase + VoiceBody::Morph, (float) res::Plate);
                return e;
            };
            out.push_back(s);
        }
        {
            Subject s; s.name = "Resonator"; s.type = EngineType::Resonator;
            s.make = []
            {
                auto e = std::make_unique<ResonatorEngine>();
                e->prepareToPlay(SR, BLOCK);
                e->setParameter(ResonatorEngine::MorphParam, (float) res::Tube);            // bowed: a steady tone
                e->setParameter(ResonatorEngine::ExciterMorphParam, (float) res::Bow);
                e->setParameter(ResonatorEngine::AmpAttack, 0.001f);
                e->setParameter(ResonatorEngine::AmpSustain, 1.0f);
                voiceFilter(*e, ResonatorEngine::FilterBase);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            // The GRAIN page's destinations need grains: a dense scrape read from the bank's held tone.
            Subject s; s.name = "Resonator GRAIN"; s.type = EngineType::Resonator; s.everything = false;
            s.only = { "GPOS", "SCAN", "SPRY", "GSIZ", "GPIT", "PJIT" };
            s.make = []
            {
                auto e = std::make_unique<ResonatorEngine>();
                e->prepareToPlay(SR, BLOCK);
                e->setParameter(ResonatorEngine::ExciterMorphParam, (float) res::Scrape);
                e->setParameter(ResonatorEngine::ExciteBase + res::Grain, 0.4f);
                e->setParameter(ResonatorEngine::GrainSample, (float) (SampleBank::get().defaultIndex() + 1));
                e->setParameter(ResonatorEngine::GrainSizeParam, 0.1f);
                e->setParameter(ResonatorEngine::AmpAttack, 0.001f);
                e->setParameter(ResonatorEngine::AmpSustain, 1.0f);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            Subject s; s.name = "Wavetable"; s.type = EngineType::Wavetable;
            s.make = []
            {
                auto e = std::make_unique<WavetableEngine>();
                e->prepareToPlay(SR, BLOCK);
                e->setParameter(WavetableEngine::TableSelect, 2.0f);    // sine to triangle: no edge in any frame
                e->setParameter(WavetableEngine::Position, 0.5f);
                e->setParameter(WavetableEngine::AmpAttack, 0.001f);
                e->setParameter(WavetableEngine::AmpSustain, 1.0f);
                voiceFilter(*e, WavetableEngine::FilterBase);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            // The wavetable engine with filter B in the path (a band-pass two octaves below
            // filter A, beside it): the cells of the filter section's dual block.
            Subject s; s.name = "Wavetable FLTB"; s.type = EngineType::Wavetable; s.everything = false;
            s.only = { "BAL", "OFSB", "RESB", "DRVB" };
            s.make = []
            {
                auto e = std::make_unique<WavetableEngine>();
                e->prepareToPlay(SR, BLOCK);
                e->setParameter(WavetableEngine::TableSelect, 2.0f);
                e->setParameter(WavetableEngine::Position, 0.5f);
                e->setParameter(WavetableEngine::AmpAttack, 0.001f);
                e->setParameter(WavetableEngine::AmpSustain, 1.0f);
                voiceFilter(*e, WavetableEngine::FilterBase);
                const int dual = WavetableEngine::FilterDualBase - (int) VoiceFilterSection::Count;
                e->setParameter(dual + VoiceFilterSection::ModeB, 1.0f);
                e->setParameter(dual + VoiceFilterSection::OffsetB, -24.0f);
                e->setParameter(dual + VoiceFilterSection::Routing, 2.0f);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            Subject s; s.name = "WaveSeq"; s.type = EngineType::WaveSeq;
            s.make = []
            {
                auto e = std::make_unique<WaveSeqEngine>();
                e->prepareToPlay(SR, BLOCK);
                // One step at the played pitch: a steady tone.
                e->setParameter(WaveSeqEngine::SeqLength, 1.0f);
                e->setParameter(WaveSeqEngine::TimingLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::PitchLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::ShapeLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::LevelLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::StepPitch, 0.0f);
                e->setParameter(WaveSeqEngine::StepShape, 0.0f);
                e->setParameter(WaveSeqEngine::StepLevel, 1.0f);
                e->setParameter(WaveSeqEngine::Crossfade, 0.0f);
                e->setParameter(WaveSeqEngine::AmpAttack, 0.001f);
                e->setParameter(WaveSeqEngine::AmpSustain, 1.0f);
                voiceFilter(*e, WaveSeqEngine::FilterBase);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            // BODY's destinations need the body in: the same steady tone through a plate.
            Subject s; s.name = "WaveSeq BODY"; s.type = EngineType::WaveSeq; s.everything = false;
            s.only = { "BMIX", "BMRP", "BDCY", "BCUP", "BDMP", "BPOS", "BTUN", "BKTK",
                        "BCNT", "BGRN", "BRED", "BHRD", "BRDM", "BKDC", "BDIR", "BWIR" };
            const auto plain = out.back().make;
            s.make = [plain]
            {
                auto e = plain();
                e->setParameter(WaveSeqEngine::BodyBase + VoiceBody::Mix, 0.7f);
                e->setParameter(WaveSeqEngine::BodyBase + VoiceBody::Morph, (float) res::Plate);
                return e;
            };
            out.push_back(s);
        }
        {
            Subject s; s.name = "Sampler"; s.type = EngineType::Sampler; s.low = 49; s.high = 68;
            s.make = [] { return sampler(SamplerEngine::Loop); };
            out.push_back(s);
        }
        {
            // The grain destinations need the granular mode: a cloud of long, dense grains of the same tone.
            Subject s; s.name = "Sampler GRAN"; s.type = EngineType::Sampler; s.low = 49; s.high = 68; s.everything = false;
            s.only = { "GPOS", "SIZE", "DENS", "SPD", "SCAN", "SPRY", "PJIT", "SPRD", "RVRS" };
            s.make = []
            {
                auto e = sampler(SamplerEngine::Granular);
                static_cast<SamplerEngine&>(*e).setGrainSeed(0x434C4943u);
                e->setParameter(SamplerEngine::GrainSize, 0.2f);
                e->setParameter(SamplerEngine::GrainDensity, 100.0f);
                return e;
            };
            out.push_back(s);
        }
        {
            Subject s; s.name = "Drum"; s.type = EngineType::Drum; s.low = 0; s.high = 1; s.voices = 0;
            s.make = []
            {
                auto e = std::make_unique<DrumEngine>();
                e->prepareToPlay(SR, BLOCK);
                // Pads 1 and 2 are held sines (the sub bass), tuned into easy reach; pad 9 is the kick machine.
                e->setParameter(0 * DrumEngine::PARAMS_PER_VOICE + DrumEngine::TUNE, 116.54f);
                e->setParameter(1 * DrumEngine::PARAMS_PER_VOICE + DrumEngine::TUNE, 466.16f);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            // The resonator drum's cells, on a pad that is one (pad 9 as a gong, which rings).
            Subject s; s.name = "Drum RSN"; s.type = EngineType::Drum; s.low = 8; s.high = 8; s.voices = 0; s.everything = false;
            s.only = { "MRPH", "XMPH", "ENRG", "STIF", "SHT", "SHEL", "ODD", "DAMP", "TILT", "HUM", "POS", "MEMB", "DROP", "WIRE",
                       "PAIR", "HAND", "HARD" };
            s.make = []
            {
                auto e = std::make_unique<DrumEngine>();
                e->prepareToPlay(SR, BLOCK);
                e->setParameter(8 * DrumEngine::PARAMS_PER_VOICE + DrumEngine::TYPE, (float) (int) DrumSynthType::Resonator);
                e->setParameter(DrumEngine::SOUND_BASE + 8 * DrumEngine::SOUND_CELLS + DrumEngine::RsnSound, (float) resonatorSoundIndex("GONG"));
                e->setParameter(8 * DrumEngine::PARAMS_PER_VOICE + DrumEngine::TUNE, 300.0f);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            // The resonator's second eight latents that need a body and an exciter the main patch
            // has not: a scraped plate, with a grip (where a junction meets, pairs, the exciter itself).
            Subject s; s.name = "Resonator BOD2"; s.type = EngineType::Resonator; s.everything = false;
            s.only = { "MEET", "SPRD", "PAIR", "DIR" };
            s.make = []
            {
                auto e = std::make_unique<ResonatorEngine>();
                e->prepareToPlay(SR, BLOCK);
                e->setParameter(ResonatorEngine::MorphParam, (float) res::Plate);
                e->setParameter(ResonatorEngine::ExciterMorphParam, (float) res::Scrape);
                e->setParameter(ResonatorEngine::ExciteBase + res::Couple, 0.75f);
                e->setParameter(ResonatorEngine::AmpAttack, 0.001f);
                e->setParameter(ResonatorEngine::AmpSustain, 1.0f);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        {
            // The kick machine's own cells, on the pad that has it (most are read at the strike).
            Subject s; s.name = "Drum KICK"; s.type = EngineType::Drum; s.low = 8; s.high = 8; s.voices = 0; s.everything = false;
            s.only = { "SWEP", "SWPT", "SCRV", "SHAP", "KNCK", "KTON", "HOLD", "ACRV" };
            s.make = []
            {
                auto e = std::make_unique<DrumEngine>();
                e->prepareToPlay(SR, BLOCK);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            out.push_back(s);
        }
        return out;
    }

    // A drum kit has its pressure destination in its voices (as DrumEngine::applyKitPressure sets it from the kit's pressure slot).
    static void drumPressure(DrumEngine& drum, AftertouchRouter::Destination dest)
    {
        using D = AftertouchRouter::Destination;
        DrumATDest to = DrumATDest::Off;
        switch (dest)
        {
            case D::DrumPitch:    to = DrumATDest::Pitch; break;
            case D::DrumDecay:    to = DrumATDest::Decay; break;
            case D::FilterCutoff: to = DrumATDest::Filter; break;
            case D::Volume:       to = DrumATDest::Level; break;
            case D::DrumDrive:    to = DrumATDest::Drive; break;
            case D::DrumSnap:     to = DrumATDest::Snap; break;
            case D::PanSpread:    to = DrumATDest::Pan; break;
            default: break;
        }
        drum.setSlotAftertouchDest(0, to);
        drum.setSlotAftertouchDepth(0, 1.0f);
    }

    // The sampler on a cosine: the sample starts at its peak, so a note's first sample is an attack
    // of the kind that may not be softened (441 Hz at 44.1 kHz: a period of exactly 100 samples,
    // so the loop is seamless).
    static std::unique_ptr<EngineBase> sampler(SamplerEngine::PlayMode mode)
    {
        auto e = std::make_unique<SamplerEngine>();
        e->prepareToPlay(SR, BLOCK);
        std::vector<float> d(44100);
        for (size_t i = 0; i < d.size(); ++i)
            d[i] = 0.6f * std::cos((float) (i % 100) / 100.0f * juce::MathConstants<float>::twoPi);
        e->loadSample(d.data(), (int) d.size(), 1, 44100.0);
        e->setParameter(SamplerEngine::Mode, (float) mode);
        return std::unique_ptr<EngineBase>(std::move(e));
    }

    // ------------------------------------------------------------------------------- rendering
    struct Event
    {
        int block;
        std::function<void(EngineBase&)> act;
    };

    static Audio render(EngineBase& e, int blocks, const std::vector<Event>& events, int blockSize = BLOCK)
    {
        Audio out;
        out.l.reserve((size_t) (blocks * blockSize));
        out.r.reserve((size_t) (blocks * blockSize));
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> buf(2, blockSize);
        for (int b = 0; b < blocks; ++b)
        {
            for (auto& ev : events)
                if (ev.block == b)
                    ev.act(e);
            buf.clear();
            e.processBlock(buf, midi);
            out.l.insert(out.l.end(), buf.getReadPointer(0), buf.getReadPointer(0) + blockSize);
            out.r.insert(out.r.end(), buf.getReadPointer(1), buf.getReadPointer(1) + blockSize);
        }
        return out;
    }

    static Event noteOn(int block, int note, float velocity = 0.8f)
    {
        return { block, [note, velocity](EngineBase& e) { e.noteOn(note, velocity, -1); } };
    }

    static Event setParam(int block, int id, float value)
    {
        return { block, [id, value](EngineBase& e) { e.setParameter(id, value); } };
    }

    static int blocksFor(double seconds, int blockSize = BLOCK) { return (int) std::ceil(seconds * SR / blockSize); }

    // ------------------------------------------------------------------------------- measuring
    struct Result
    {
        float atEdges = 0.0f, elsewhere = 0.0f, peak = 0.0f;
        int where = 0;      // the sample of the largest step at a change
        bool clicks() const { return atEdges > REL * elsewhere + ABS * peak; }
        juce::String text() const
        {
            return juce::String(atEdges, 4) + " at the change (sample " + juce::String(where) + "), " + juce::String(elsewhere, 4)
                 + " elsewhere, peak " + juce::String(peak, 3);
        }
    };

    // The largest step from one sample to the next inside the windows that start at `edges` (and
    // last `window` samples) against the largest outside them, from `from` on.
    static Result measure(const Audio& a, const std::vector<int>& edges, int window, int from)
    {
        Result res;
        for (auto* ch : { &a.l, &a.r })
        {
            const auto& x = *ch;
            for (int i = std::max(1, from); i < (int) x.size(); ++i)
            {
                const float step = std::abs(x[(size_t) i] - x[(size_t) i - 1]);
                bool atEdge = false;
                for (int e : edges)
                    atEdge = atEdge || (i >= e - 4 && i < e + window);
                if (atEdge && step > res.atEdges)
                    res.where = i;
                float& worst = atEdge ? res.atEdges : res.elsewhere;
                worst = std::max(worst, step);
                res.peak = std::max(res.peak, std::abs(x[(size_t) i]));
            }
        }
        return res;
    }

    // For a sound whose changes are not at known places: the largest step anywhere in it against
    // the largest in a steady reference.
    static Result everywhere(const Audio& a, const std::vector<Audio>& references)
    {
        Result res = measure(a, { SETTLE + 4 }, (int) a.l.size(), SETTLE);
        res.elsewhere = 0.0f;
        for (auto& r : references)
            res.elsewhere = std::max(res.elsewhere, measure(r, {}, 0, SETTLE).elsewhere);
        return res;
    }

    static float maxAbsDiff(const Audio& a, const Audio& b)
    {
        float m = 0.0f;
        for (size_t i = 0; i < std::min(a.l.size(), b.l.size()); ++i)
            m = std::max({ m, std::abs(a.l[i] - b.l[i]), std::abs(a.r[i] - b.r[i]) });
        return m;
    }

    static Audio difference(const Audio& a, const Audio& b)
    {
        Audio d;
        const size_t n = std::min(a.l.size(), b.l.size());
        d.l.resize(n);
        d.r.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            d.l[i] = a.l[i] - b.l[i];
            d.r[i] = a.r[i] - b.r[i];
        }
        return d;
    }

    static float peakOf(const Audio& a, int from = 0, int to = 1 << 30)
    {
        float p = 0.0f;
        for (int i = std::max(0, from); i < std::min(to, (int) a.l.size()); ++i)
            p = std::max({ p, std::abs(a.l[(size_t) i]), std::abs(a.r[(size_t) i]) });
        return p;
    }

    // ------------------------------------------------------------------------------- allow-lists
    struct Allowed
    {
        const char* engine;     // Subject::name, or "*" for every engine
        const char* what;       // the destination or cell label
        const char* reason;
    };

    static const Allowed* find(const std::vector<Allowed>& list, const char* engine, const juce::String& what)
    {
        for (auto& a : list)
        {
            if (std::strcmp(a.engine, "*") != 0 && std::strcmp(a.engine, engine) != 0)
                continue;
            const juce::String pattern(a.what);     // "ATK*": every label that starts with ATK
            if (pattern.endsWithChar('*') ? what.startsWith(pattern.dropLastCharacters(1)) : what == pattern)
                return &a;
        }
        return nullptr;
    }

    bool verbose = false;

    // One measurement: logged when asked for, failed unless it passes or is allowed.
    void judge(const juce::String& test, const Subject& s, const juce::String& what, const Result& res,
               const std::vector<Allowed>& allowed)
    {
        const auto* a = find(allowed, s.name, what);
        if (verbose)
            std::cout << "  " << test << " | " << s.name << " | " << what << " | " << res.text()
                      << (res.clicks() ? (a != nullptr ? "  ALLOWED" : "  CLICK") : "") << std::endl;
        if (a == nullptr)
            expect(! res.clicks(), test + ", " + s.name + " " + what + ": " + res.text());
    }

    // ------------------------------------------------------------------------------- LFOs
    static constexpr float LFO_HZ = 12.0f;                 // a cycle of 4000 samples
    static constexpr int LFO_PERIOD = 4000;
    static constexpr int SETTLE = 1440;                     // 30 ms: the note's own attack is not judged
    static constexpr int EDGE_WINDOW = 480;                 // 10 ms after a change

    static int destCellId(const Subject& s) { return s.unified ? ModParam::lfoParamId(0, ModParam::LFO_DEST) : S::id(0, S::Dest); }

    // The engine's own destination labels (index 0 = OFF).
    static std::vector<juce::String> destinations(const Subject& s, EngineBase& e)
    {
        std::vector<juce::String> out;
        if (s.unified)
        {
            // Its DEST cell is on the screen-built MOD page: the voice's own list.
            for (int i = 0; i < (int) UnifiedVoice::LfoDest::Count; ++i)
                out.push_back(UnifiedVoice::lfoDestLabels()[i]);
            return out;
        }
        for (auto& page : e.getParamPages())
            if (auto* cell = page.find(destCellId(s)))
            {
                // (The engine's own. The cells of the track's effects that follow, E4's
                // mod::TrackDest, sound through an AudioEngine only: "Effect Destinations" sweeps
                // them under a held sound, with this audit's measure.)
                for (int i = 0; i < mod::TrackDest::firstIn(cell->choices, cell->numChoices); ++i)
                    out.push_back(cell->choices[i]);
                break;
            }
        return out;
    }

    static void setLfo(const Subject& s, EngineBase& e, int dest, float depth, LFO::Shape shape, float hz)
    {
        if (s.unified)
        {
            e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_RATE), hz);
            e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_SHAPE), (float) (int) shape);
            e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_TRIG), (float) (int) LFO::TriggerMode::KeySync);
            e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_SYNC), 0.0f);
            e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_DEST), (float) dest);
            e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_DEPTH), depth);
            return;
        }
        e.setParameter(S::id(0, S::Rate), hz);
        e.setParameter(S::id(0, S::Shape), (float) (int) shape);
        e.setParameter(S::id(0, S::Trigger), (float) (int) LFO::TriggerMode::KeySync);
        e.setParameter(S::id(0, S::Sync), 0.0f);
        e.setParameter(S::id(0, S::Fade), 0.0f);
        e.setParameter(S::id(0, S::Dest), (float) dest);
        e.setParameter(S::id(0, S::Depth), depth);
    }

    static int depthCellId(const Subject& s) { return s.unified ? ModParam::lfoParamId(0, ModParam::LFO_DEPTH) : S::id(0, S::Depth); }

    static const char* shapeName(LFO::Shape s) { return LFO::shapeLabels()[(int) s]; }

    // Where the shape has its edges in a render that starts with the note (KEY trigger).
    static std::vector<int> lfoEdges(LFO::Shape shape, int samples)
    {
        std::vector<int> edges;
        const int every = shape == LFO::Shape::Square ? LFO_PERIOD / 2 : LFO_PERIOD;
        for (int at = every; at < samples; at += every)
            edges.push_back(at);
        return edges;
    }

    // Destinations where the step is the sound that was asked for. (Pitch is one - the LFO reaches it
    // as it is, and a pitch step is no click because the phase goes on from where it is - but it
    // passes the measurement by itself and needs no entry.)
    static std::vector<Allowed> lfoAllowed()
    {
        return {};
    }

    // The strip moving a destination all the way within 20 ms.
    static std::vector<Allowed> stripAllowed()
    {
        return { { "Resonator", "MRPH", "the strip takes the bowed tube through the comb to the bell: on the way the body has every "
                                        "harmonic, and a bowed comb's own steps are fifty times the tube's - brightness, not a click "
                                        "(the 'Resonator' suite judges a morph against the brightest body on its way)" } };
    }

    // Destinations that do nothing to the audit's patch: nothing was heard, so nothing is proven.
    // Each is here with the reason; a new destination that is inert has to be given a patch that
    // uses it, or a line here.
    static std::vector<Allowed> lfoInert()
    {
        const char* const stage = "an envelope time: read by the stage that is running, and a time cannot jump a level";
        const char* const otherLfo = "moves another LFO, which is off here; an LFO that is moved is audited as itself";
        const char* const digital = "a cell of a digital oscillator kind; the audit's oscillators are sines";
        const char* const silentOp = "operators 3-6 are silent in the audit's patch (the same code as 1 and 2)";
        const char* const oneStep = "the audit's sequence has one step (see the wave sequence test for the steps)";
        const char* const granular = "granular mode only: audited on 'Sampler GRAN'";
        const char* const notHere = "needs slices or a time stretch, which the audit's patch has not (not audited)";
        const char* const atStart = "read once, when the note starts";
        const char* const filterB = "a cell of filter B, which is out of the path here (ROUT SNGL): audited on 'Wavetable FLTB'";
        const char* const kickCell = "a cell of the kick machine: audited on 'Drum KICK' (pad 9)";
        const char* const rsnCell = "a cell of the resonator drum: the body's are audited on 'Drum RSN' (pad 9 as a gong), and all of "
                                    "them on the RSN engine, whose voice it is ('Resonator', 'Resonator BOD2')";
        const char* const bod2 = "needs a heavy body under a grip, even partials, or an exciter that is heard (the audit's patch is a "
                                 "bowed tube): audited on 'Resonator BOD2'";
        const char* const bodyOff = "BODY is off in the audit's patch: audited on 'WaveSeq BODY'";
        const char* const macroBodyOff = "BODY is off in the audit's patch: audited on 'Macro BODY'";
        const char* const noSample = "the exciter's grains have no sample in the audit's patch: audited on 'Resonator GRAIN'";
        std::vector<Allowed> a = {
            { "*", "ATK*", stage }, { "*", "DEC*", stage }, { "*", "REL*", stage },
            { "*", "FATK", stage }, { "*", "FDEC", stage }, { "*", "FREL", stage },
            { "*", "LRAT", otherLfo }, { "*", "LDPT", otherLfo }, { "Unified", "RAT*", otherLfo }, { "Unified", "DPT*", otherLfo },
            { "*", "MORP", "the SEM filter's cell; the audit's filter is the SVF" },
            { "*", "FMOR", "the SEM filter's cell; the audit's filter is the SVF" },
            { "*", "GLID", "glide: only heard between two notes" },
            { "Unified", "1A", digital }, { "Unified", "1B", digital }, { "Unified", "2A", digital },
            { "Unified", "2B", digital }, { "Unified", "3A", digital }, { "Unified", "3B", digital },
            { "Unified", "SPRD", "the stack's spread; the audit's patch has no stack" },
            { "Unified", "DRFT", "analog drift, switched off for the audit (nothing random)" },
            { "Unified", "DRAT", "analog drift, switched off for the audit (nothing random)" },
            { "Unified", "SUS2", "the extra envelopes are off" }, { "Unified", "SUS3", "the extra envelopes are off" },
            { "Unified", "SUS4", "the extra envelopes are off" }, { "Unified", "SUS5", "the extra envelopes are off" },
            { "Unified", "EDPT", "the extra envelopes are off" },
            { "Wavetable", "MDPT", "the morph's depth; the morph is off" },
            { "Wavetable", "TIME", "the scan's time; the scan is off (see the scan test)" },
            { "Wavetable", "SPD", "the scan's speed; the scan is off (see the scan test)" },
            { "WaveSeq", "RATE", oneStep }, { "WaveSeq", "XFAD", oneStep }, { "WaveSeq", "PRNG", oneStep },
            { "WaveSeq", "P*", oneStep }, { "WaveSeq", "L*", oneStep }, { "WaveSeq", "T*", oneStep },
            { "WaveSeq", "BMRP", bodyOff }, { "WaveSeq", "BDCY", bodyOff }, { "WaveSeq", "BCUP", bodyOff }, { "WaveSeq", "BDMP", bodyOff },
            { "WaveSeq", "BPOS", bodyOff }, { "WaveSeq", "BTUN", bodyOff }, { "WaveSeq", "BKTK", bodyOff },
            { "WaveSeq", "BCNT", bodyOff }, { "WaveSeq", "BGRN", bodyOff }, { "WaveSeq", "BRED", bodyOff }, { "WaveSeq", "BHRD", bodyOff },
            { "WaveSeq", "BRDM", bodyOff }, { "WaveSeq", "BKDC", bodyOff }, { "WaveSeq", "BDIR", bodyOff }, { "WaveSeq", "BWIR", bodyOff },
            { "Macro", "BMRP", macroBodyOff }, { "Macro", "BDCY", macroBodyOff }, { "Macro", "BCUP", macroBodyOff }, { "Macro", "BDMP", macroBodyOff },
            { "Macro", "BPOS", macroBodyOff }, { "Macro", "BTUN", macroBodyOff }, { "Macro", "BKTK", macroBodyOff },
            { "Macro", "BCNT", macroBodyOff }, { "Macro", "BGRN", macroBodyOff }, { "Macro", "BRED", macroBodyOff }, { "Macro", "BHRD", macroBodyOff },
            { "Macro", "BRDM", macroBodyOff }, { "Macro", "BKDC", macroBodyOff }, { "Macro", "BDIR", macroBodyOff }, { "Macro", "BWIR", macroBodyOff },
            { "Resonator", "GPOS", noSample }, { "Resonator", "SCAN", noSample }, { "Resonator", "SPRY", noSample },
            { "Resonator", "GSIZ", noSample }, { "Resonator", "GPIT", noSample }, { "Resonator", "PJIT", noSample },
            { "Sampler", "GPOS", granular }, { "Sampler", "SIZE", granular }, { "Sampler", "DENS", granular },
            { "Sampler", "SPD", granular }, { "Sampler", "SCAN", granular }, { "Sampler", "SPRY", granular },
            { "Sampler", "PJIT", granular }, { "Sampler", "SPRD", granular }, { "Sampler", "RVRS", granular },
            { "Sampler", "SLIC", notHere }, { "Sampler", "FADE", notHere }, { "Sampler", "SLEN", notHere },
            { "Sampler", "SSIZ", notHere }, { "Sampler", "XFAD", notHere }, { "Sampler", "JIT", notHere }, { "Sampler", "SSPR", notHere },
            { "Sampler", "END", atStart }, { "Sampler", "LPST", atStart }, { "Sampler", "LPEN", atStart },
            { "Sampler", "VELF", atStart }, { "Sampler", "SUS", atStart },
            { "Drum", "SWEP", kickCell }, { "Drum", "SWPT", kickCell }, { "Drum", "SCRV", kickCell }, { "Drum", "SHAP", kickCell },
            { "Drum", "KNCK", kickCell }, { "Drum", "KTON", kickCell }, { "Drum", "HOLD", kickCell }, { "Drum", "ACRV", kickCell },
            { "Drum", "WPOS", "the place in a pad's WAVE: the audit's pads read the sine, which has no place (per sample, ramped between sub-blocks: \"U4 Drum\" hears it on a table)" },
            { "Drum", "FTIM", "the time of a drum's second envelope: the audit's pads are SUB, whose second envelope does nothing (\"U4 Drum\" hears it on the kick machine's sweep)" },
            { "Resonator", "MEET", bod2 }, { "Resonator", "SPRD", bod2 }, { "Resonator", "PAIR", bod2 }, { "Resonator", "DIR", bod2 },
            { "Drum RSN", "*", "a struck gong: the cell is heard at the strike or moves a body that rings on (the LFO's edges are judged all the same)" },
            // BDY2: cells that need a configuration the audit's body patch does not provide.
            // BDIR needs GRAN > 0 (the direct output is from grains; without them only the onset click).
            // BCNT, BGRN, BRED, BRDM: need specific excitation/release conditions the bowed-plate audit does not reach.
            { "Macro BODY", "BCNT", "CONT vs burst: the audit's bowed body is driven continuously by the voice; switching to burst is too subtle" },
            { "Macro BODY", "BGRN", "GRAN: grain impulses beside the voice; on a bowed plate the effect is below the threshold" },
            { "Macro BODY", "BRED", "REED: the friction curve; needs COUP > 0 (the audit's plate has none)" },
            { "Macro BODY", "BRDM", "RDAM: release damping; only heard on note-off, the LFO does not cross a note-off" },
            { "Macro BODY", "BDIR", "DIRE needs grain impulses (GRAN > 0); the audit's body has none" },
            { "WaveSeq BODY", "BDIR", "DIRE needs grain impulses (GRAN > 0); the audit's body has none" },
            { "WaveSeq BODY", "BCNT", "CONT vs burst: the audit's bowed body is driven continuously by the voice; switching to burst is too subtle" },
            { "WaveSeq BODY", "BGRN", "GRAN: grain impulses beside the voice; on a bowed tube the effect is below the threshold" },
            { "WaveSeq BODY", "BRED", "REED: the friction curve; needs COUP > 0 and a specific excitation for the LFO's edges to be heard" },
            { "WaveSeq BODY", "BRDM", "RDAM: release damping; only heard on note-off, the LFO does not cross a note-off" },
        };
        for (const char* cell : { "MRPH", "XMPH", "ENRG", "STIF", "SHT", "SHEL", "ODD", "DAMP", "TILT", "HUM", "POS", "MEMB", "DROP", "WIRE",
                                  "MEET", "SPRD", "PAIR", "HAND", "DIR", "CONT", "GRAN", "COUP", "REED", "HARD" })
            a.push_back({ "Drum", cell, rsnCell });
        for (const char* cell : { "LVL3", "RAT3", "DET3", "FDB3", "SUS3", "LVL4", "RAT4", "DET4", "FDB4", "SUS4",
                                  "LVL5", "RAT5", "DET5", "FDB5", "SUS5", "LVL6", "RAT6", "DET6", "FDB6", "SUS6",
                                  "PM3", "PM4", "PM5", "PM6" })     // (the phases of the same four: U4-FM)
            a.push_back({ "FM", cell, silentOp });
        a.push_back({ "FM", "POS*", "a table's position, and the audit's operators are sines: audited on 'FM WAVE'" });
        for (const char* cell : { "BAL", "OFSB", "RESB", "DRVB" })
            a.push_back({ "Wavetable", cell, filterB });
        return a;
    }

    void runTest() override
    {
        verbose = juce::SystemStats::getEnvironmentVariable("NEPTUNE_CLICK_AUDIT_VERBOSE", {}).isNotEmpty();
        const auto all = subjects();

        beginTest("an LFO with an edge in its shape does not step any destination");
        for (auto& s : all)
        {
            auto probe = s.make();
            const auto dests = destinations(s, *probe);
            expect(dests.size() > 1, juce::String(s.name) + " has LFO destinations");
            const int blocks = blocksFor(0.27);
            const auto off = render(*s.make(), blocks, { noteOn(0, s.low) });
            const auto allowed = lfoAllowed(), inertAllowed = lfoInert();
            for (int d = 1; d < (int) dests.size(); ++d)
            {
                if (! s.audits(dests[(size_t) d]))
                    continue;
                bool heard = false;
                for (auto shape : { LFO::Shape::Square, LFO::Shape::Random, LFO::Shape::Saw, LFO::Shape::RampDown })
                {
                    // The square on a low note for every destination. Random (low), and the two
                    // ramps on a high note (the same edge at other points of a faster wave), for a
                    // sample of the list: every shape reaches a destination by the same road.
                    const bool ramp = shape == LFO::Shape::Saw || shape == LFO::Shape::RampDown;
                    if (shape != LFO::Shape::Square && ! sampled(d))
                        continue;
                    auto e = s.make();
                    setLfo(s, *e, d, 1.0f, shape, LFO_HZ);
                    const auto a = render(*e, blocks, { noteOn(0, ramp ? s.high : s.low) });
                    heard = heard || (! ramp && maxAbsDiff(a, off) > 1.0e-4f);
                    judge(juce::String("LFO ") + shapeName(shape), s, dests[(size_t) d],
                          measure(a, lfoEdges(shape, (int) a.l.size()), EDGE_WINDOW, SETTLE), allowed);
                }
                if (verbose && ! heard)
                    std::cout << "  INERT | " << s.name << " | " << dests[(size_t) d] << std::endl;
                expect(heard || find(inertAllowed, s.name, dests[(size_t) d]) != nullptr,
                       juce::String(s.name) + " " + dests[(size_t) d] + ": the LFO changes nothing in the audit's patch (not audited)");
            }
        }

        beginTest("an LFO switched off under a held note lets go without a step");
        for (auto& s : all)
        {
            if (! s.everything)
                continue;
            const auto dests = destinations(s, *s.make());
            const int at = blocksFor(0.1), blocks = blocksFor(0.2);
            for (int d = 1; d < (int) dests.size(); ++d)
                for (int how = 0; how < 2; ++how)
                {
                    if (how == 1 && ! sampled(d))
                        continue;       // DPTH to 0 takes the same road as DEST to OFF: a sample of the list
                    auto e = s.make();
                    setLfo(s, *e, d, 1.0f, LFO::Shape::Square, 0.02f);      // at +1 for the whole render
                    const auto a = render(*e, blocks, { noteOn(0, s.low),
                                                        how == 0 ? setParam(at, destCellId(s), 0.0f) : setParam(at, depthCellId(s), 0.0f) });
                    judge(how == 0 ? "LFO DEST to OFF" : "LFO DPTH to 0", s, dests[(size_t) d],
                          measure(a, { at * BLOCK }, EDGE_WINDOW, SETTLE), {});
                }
        }

        beginTest("the mod strip jumping through a route does not step its destination");
        for (auto& s : all)
        {
            if (! s.everything)
                continue;
            // 256-sample blocks: the strip is taken once per block, so a larger block is the harder case.
            const int bs = 256;
            const auto dests = destinations(s, *s.make());
            const int up = blocksFor(0.06, bs), down = blocksFor(0.32, bs), blocks = blocksFor(0.56, bs);
            const int window = (int) (0.2 * SR);
            for (int d = 1; d < (int) dests.size(); ++d)
            {
                if (! sampled(d))
                    continue;
                auto e = s.make();
                e->prepareToPlay(SR, bs);
                if (s.unified)
                {
                    // The strip is an LFO's depth here: an LFO that stands at +1, brought in by the strip.
                    for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                        static_cast<UnifiedVoiceEngine&>(*e).getVoice(v).getAnalogCharacter().enabled = false;
                    setLfo(s, *e, d, 0.0f, LFO::Shape::Square, 0.02f);
                    e->setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 1.0f);
                }
                else
                {
                    for (int r = 0; r < S::NUM_STRIP; ++r)
                    {
                        e->setParameter(S::stripDestId(r), r == 0 ? (float) d : 0.0f);
                        e->setParameter(S::stripAmountId(r), r == 0 ? 1.0f : 0.0f);
                    }
                    e->setParameter(S::STRIP_VIBRATO, 0.0f);
                }
                const auto a = render(*e, blocks, { noteOn(0, s.low),
                                                    { up, [](EngineBase& x) { x.setModWheel(1.0f); } },
                                                    { down, [](EngineBase& x) { x.setModWheel(0.0f); } } }, bs);
                judge("strip 0 -> 1 -> 0", s, dests[(size_t) d], measure(a, { up * bs, down * bs }, window, SETTLE), stripAllowed());
            }
        }

        beginTest("pad pressure jumping does not step what it is routed to");
        for (auto& s : all)
        {
            if (! s.everything)
                continue;
            const AudioEngine::PressureDestInfo* list = nullptr;
            const int n = AudioEngine::pressureDestinations(s.type, list);
            const int press = blocksFor(0.08), lift = blocksFor(0.2), blocks = blocksFor(0.32);
            for (int i = 0; i < n; ++i)
            {
                AftertouchRouter router;
                router.clear();
                router.setRoute(0, { list[i].dest, 1.0f, false });
                auto e = s.make();
                if (s.type == EngineType::Drum)
                    drumPressure(static_cast<DrumEngine&>(*e), list[i].dest);
                else
                    e->setAftertouchRouter(&router);
                const int note = s.low;
                const auto a = render(*e, blocks, { noteOn(0, note),
                                                    { press, [note](EngineBase& x) { x.aftertouch(note, 1.0f); } },
                                                    { lift, [note](EngineBase& x) { x.aftertouch(note, 0.0f); } } });
                judge("pressure 0 -> 1 -> 0", s, list[i].label, measure(a, { press * BLOCK, lift * BLOCK }, 4 * EDGE_WINDOW, SETTLE), {});
            }
        }

        beginTest("a step lock landing on a held note does not step it (every continuous cell)");
        for (auto& s : all)
        {
            if (! s.everything)
                continue;
            auto probe = s.make();
            const int first = blocksFor(0.08), second = blocksFor(0.14), back = blocksFor(0.2), blocks = blocksFor(0.26);
            const auto allowed = lockAllowed();
            for (auto& page : probe->getParamPages())
                for (auto& cell : page.params)
                {
                    if (! cell.isValid() || cell.isDiscrete() || cell.maxVal <= cell.minVal || probe->isRebuildParam(cell.id)
                        || S::owns(cell.id) || (s.unified && ModParam::isModParam(cell.id)))
                        continue;
                    auto e = s.make();
                    const float set = e->getParameter(cell.id);
                    const auto a = render(*e, blocks, { noteOn(0, s.low), setParam(first, cell.id, cell.maxVal),
                                                        setParam(second, cell.id, cell.minVal), setParam(back, cell.id, set) });
                    judge("step lock", s, juce::String(page.name) + " " + cell.label,
                          measure(a, { first * BLOCK, second * BLOCK, back * BLOCK }, EDGE_WINDOW, SETTLE), allowed);
                }
        }

        beginTest("a key struck again, and a stolen voice: the old sound goes without a step, the new note is whole at once");
        for (auto& s : all)
        {
            if (! s.everything)
                continue;
            const int at = blocksFor(0.15) + 1, blocks = blocksFor(0.3);
            struct Case { const char* name; std::vector<Event> before; Event last; };
            std::vector<Case> cases;
            cases.push_back({ "same key again", { noteOn(0, s.low) }, noteOn(at, s.low) });
            cases.push_back({ "same key again, softer", { noteOn(0, s.low, 1.0f) }, noteOn(at, s.low, 0.35f) });
            cases.push_back({ "same key again in its release", { noteOn(0, s.low), { at - 30, [n = s.low](EngineBase& e) { e.noteOff(n, -1); } } },
                              noteOn(at, s.low) });
            if (s.voices > 0)
            {
                // Every voice on a low cluster (small steps of its own, so one voice cut off stands out).
                Case steal { "a note steals a voice", {}, noteOn(at, s.low + 19) };
                for (int i = 0; i < s.voices; ++i)
                    steal.before.push_back(noteOn(i, s.low - 6 + i, 0.8f));
                cases.push_back(steal);
            }
            for (auto& c : cases)
            {
                auto events = c.before;
                events.push_back(c.last);
                const auto both = render(*s.make(), blocks, events);
                const auto alone = render(*s.make(), blocks, { c.last });
                // What is left when the new note is taken away is the old sound going: it may not jump,
                // and neither may the whole.
                judge(juce::String(c.name) + " (old sound)", s, "", measure(difference(both, alone), { at * BLOCK }, EDGE_WINDOW, SETTLE), {});
                // The new note's own first step is its attack: the old sound may add no more than its
                // own steps to it.
                auto whole = measure(both, { at * BLOCK }, EDGE_WINDOW, SETTLE);
                whole.elsewhere += measure(alone, {}, 0, 0).elsewhere;
                judge(juce::String(c.name) + " (whole)", s, "", whole, {});
            }
        }

        beginTest("a drum struck again while it rings, and the held bass let go");
        for (auto& s : all)
        {
            if (s.type != EngineType::Drum || ! s.everything)
                continue;
            const int at = blocksFor(0.15) + 1, blocks = blocksFor(0.3);
            // The kick machine (pad 9): its knock is an attack that starts on a value of its own.
            for (float second : { 0.8f, 0.35f })
            {
                const auto both = render(*s.make(), blocks, { noteOn(0, 8, 0.8f), noteOn(at, 8, second) });
                const auto alone = render(*s.make(), blocks, { noteOn(at, 8, second) });
                judge("kick struck again (old sound)", s, second < 0.5f ? "softer" : "", measure(difference(both, alone), { at * BLOCK }, EDGE_WINDOW, SETTLE), {});
            }
            const auto letGo = render(*s.make(), blocks, { noteOn(0, s.low), { at, [n = s.low](EngineBase& e) { e.noteOff(n, -1); } } });
            judge("held bass let go", s, "", measure(letGo, { at * BLOCK }, EDGE_WINDOW, SETTLE), {});
        }

        beginTest("the main synth with its analog character on (random phases): struck again, and stolen");
        {
            // Nothing here can be compared with a second render (the phases are drawn at random):
            // the whole is judged, several times over.
            Subject s = all.front();
            const int at = blocksFor(0.15) + 1, blocks = blocksFor(0.3);
            for (int round = 0; round < 6; ++round)
            {
                auto live = [&s]
                {
                    auto e = s.make();
                    auto& u = static_cast<UnifiedVoiceEngine&>(*e);
                    for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                        u.getVoice(v).getAnalogCharacter().enabled = true;
                    e->setParameter(UnifiedParam::NOISE, 0.0f);
                    return e;
                };
                const auto again = render(*live(), blocks, { noteOn(0, s.low), noteOn(at, s.low) });
                judge("analog: same key again", s, "", measure(again, { at * BLOCK }, EDGE_WINDOW, SETTLE), {});
                std::vector<Event> events;
                for (int i = 0; i < s.voices; ++i)
                    events.push_back(noteOn(i, s.low - 6 + i));
                events.push_back(noteOn(at, s.low + 19));
                judge("analog: a note steals a voice", s, "", measure(render(*live(), blocks, events), { at * BLOCK }, EDGE_WINDOW, SETTLE), {});
                auto mono = live();
                mono->setParameter(UnifiedParam::VOICE_MODE, 0.0f);
                judge("analog: mono, the next key", s, "",
                      measure(render(*mono, blocks, { noteOn(0, s.low), noteOn(at, s.low + 5) }), { at * BLOCK }, EDGE_WINDOW, SETTLE), {});
            }
        }

        beginTest("what steps by design still does not jump: the wavetable's STEP, RAND and LOOP scans, a wave sequence");
        for (auto& s : all)
        {
            if (s.type == EngineType::Wavetable && s.everything)
            {
                struct Scan { const char* name; WavetableScanMode mode; float steps; };
                // One pass of 0.2 s: four steps of 50 ms, eight random frames of 25 ms, a wrap every 0.2 s.
                for (auto& scan : { Scan { "STEP", WavetableScanMode::Step, 0.0f }, Scan { "RAND", WavetableScanMode::Rand, 1.0f },
                                    Scan { "LOOP", WavetableScanMode::Loop, 0.0f } })
                {
                    auto e = s.make();
                    e->setParameter(WavetableEngine::Position, 0.0f);
                    e->setParameter(WavetableEngine::ScanMode, (float) (int) scan.mode);
                    e->setParameter(WavetableEngine::ScanTime, 0.2f);
                    e->setParameter(WavetableEngine::ScanSteps, scan.steps);
                    // Against the table's two ends held still.
                    auto first = s.make(), last = s.make();
                    first->setParameter(WavetableEngine::Position, 0.0f);
                    last->setParameter(WavetableEngine::Position, 1.0f);
                    judge("scan", s, scan.name, everywhere(render(*e, blocksFor(0.5), { noteOn(0, s.low) }),
                                                           { render(*first, blocksFor(0.1), { noteOn(0, s.low) }),
                                                             render(*last, blocksFor(0.1), { noteOn(0, s.low) }) }), {});
                }
            }
            if (s.type == EngineType::WaveSeq && s.everything)
            {
                // Four steps of 50 ms without a crossfade: sine, triangle, sine, triangle at levels 1, 0.3, 0.8, 0.5.
                auto e = s.make();
                auto& w = static_cast<WaveSeqEngine&>(*e);
                for (int lane : { WaveSeqEngine::SeqLength, WaveSeqEngine::TimingLaneLen, WaveSeqEngine::PitchLaneLen,
                                  WaveSeqEngine::ShapeLaneLen, WaveSeqEngine::LevelLaneLen })
                    e->setParameter(lane, 4.0f);
                static const float levels[4] = { 1.0f, 0.3f, 0.8f, 0.5f };
                for (int v = 0; v < 8; ++v)
                    for (int step = 0; step < 4; ++step)
                    {
                        auto& st = w.getVoice(v).getStep(step);
                        st.duration = 0.05f;
                        st.pitchOffset = 0.0f;
                        st.level = levels[step];
                        st.shape = step % 2 == 0 ? WaveSeqVoice::Waveform::Sine : WaveSeqVoice::Waveform::Triangle;
                    }
                // Against the one-step sine at full level.
                judge("wave sequence", s, "steps without a crossfade",
                      everywhere(render(*e, blocksFor(0.5), { noteOn(0, s.low) }), { render(*s.make(), blocksFor(0.1), { noteOn(0, s.low) }) }), {});
            }
        }

        mixerTests(all);
    }

    // The effect blocks belong to the whole track, not to a note: a lock on one of their cells
    // lands with its own note and has to be whole from that note's first sample, so these cells
    // are set at once - and step under a note that is held across the change.
    static std::vector<Allowed> trackFxAllowed()
    {
        const char* const whole = "an effect block is the track's, not a note's: a lock on it lands with its note and is whole at once";
        return { { "TRK FX", "DIRT DRIV", whole }, { "TRK FX", "DIRT FDBK", whole }, { "TRK FX", "DIRT MIX", whole },
                 { "TRK FX", "TALK MIX", whole }, { "TRK FX", "MODFX CH.D", whole }, { "TRK FX", "MODFX FL.D", whole },
                 { "TRK FX", "MODFX PH.D", whole }, { "TRK FX", "MODFX RO.D", whole } };
    }

    // Selectors whose change under a held note is allowed to be heard as one.
    static std::vector<Allowed> selectorAllowed()
    {
        return {};
    }

    // Every destination up to the twelfth, then every fifth: the strip and DPTH take the same road
    // through the voice as the LFO's own output, which is audited on every destination.
    static bool sampled(int dest) { return dest <= 12 || dest % 5 == 0; }

    // Cells whose step under a held note is what was asked for, or cannot be a click.
    static std::vector<Allowed> lockAllowed()
    {
        return {};
    }

    // ------------------------------------------------------------------------------- the mixer
    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;

        Rig()
        {
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::FM));
            audio.prepare(SR, BLOCK);
            audio.setEngineType(0, EngineType::FM);
            voice();
        }

        // A track playing one of the audit's own patches.
        explicit Rig(const Subject& s)
        {
            audio.setSequencer(&seq);
            audio.prepare(SR, BLOCK);
            audio.addEngine(0, std::shared_ptr<EngineBase>(s.make().release()));
        }

        void voice()
        {
            auto* e = audio.getEngine(0);
            e->setParameter(FMEngine::Op2Level, 0.25f);
            e->setParameter(FMEngine::Op1Sustain, 1.0f);
            e->setParameter(FMEngine::Op1Attack, 0.001f);
        }

        Audio render(int blocks, const std::vector<std::pair<int, std::function<void(AudioEngine&)>>>& events)
        {
            Audio out;
            float l[BLOCK], r[BLOCK];
            for (int b = 0; b < blocks; ++b)
            {
                for (auto& ev : events)
                    if (ev.first == b)
                        ev.second(audio);
                audio.renderBlock(l, r, BLOCK);
                out.l.insert(out.l.end(), l, l + BLOCK);
                out.r.insert(out.r.end(), r, r + BLOCK);
            }
            return out;
        }
    };

    void mixerJudge(const juce::String& what, const Result& res)
    {
        if (verbose)
            std::cout << "  mixer | " << what << " | " << res.text() << (res.clicks() ? "  CLICK" : "") << std::endl;
        expect(! res.clicks(), "mixer, " + what + ": " + res.text());
    }

    void mixerTests(const std::vector<Subject>& all)
    {
        using Act = std::function<void(AudioEngine&)>;

        // On a track, as the app plays it: an engine that swaps or cuts its sound says so, and the
        // track carries the jump off (EngineBase::takeJump).
        beginTest("a selector turned under a held note (every choice and count cell)");
        for (auto& s : all)
        {
            if (! s.everything)
                continue;
            auto probe = s.make();
            const int first = blocksFor(0.12), second = blocksFor(0.19), back = blocksFor(0.26), blocks = blocksFor(0.33);
            const auto allowed = selectorAllowed();
            for (auto& page : probe->getParamPages())
                for (auto& cell : page.params)
                {
                    if (! cell.isValid() || ! cell.isDiscrete() || cell.maxVal <= cell.minVal || probe->isRebuildParam(cell.id)
                        || S::owns(cell.id) || (s.unified && ModParam::isModParam(cell.id)))
                        continue;
                    Rig rig(s);
                    const float set = rig.audio.getEngine(0)->getParameter(cell.id);
                    const int id = cell.id, note = s.low;
                    const float lo = cell.minVal, hi = cell.maxVal;
                    const auto a = rig.render(blocks, { { 0, [note](AudioEngine& x) { x.getEngine(0)->noteOn(note, 0.8f, -1); } },
                                                        { first, [id, hi](AudioEngine& x) { x.setTrackParameter(0, id, hi); } },
                                                        { second, [id, lo](AudioEngine& x) { x.setTrackParameter(0, id, lo); } },
                                                        { back, [id, set](AudioEngine& x) { x.setTrackParameter(0, id, set); } } });
                    judge("selector", s, juce::String(page.name) + " " + cell.label,
                          measure(a, { first * BLOCK, second * BLOCK, back * BLOCK }, 2 * EDGE_WINDOW, (int) (0.08 * SR)), allowed);
                }
        }

        const int first = blocksFor(0.2), second = blocksFor(0.35), blocks = blocksFor(0.5);
        const int settle = (int) (0.1 * SR);        // the master compressor has taken hold by then
        const Act play = [](AudioEngine& a) { a.getEngine(0)->noteOn(46, 0.8f, -1); };
        auto trackParam = [](int id, float v) { return Act([id, v](AudioEngine& a) { a.setTrackParameter(0, id, v); }); };

        beginTest("mixer: volume, pan and send level jumping under a held note");
        {
            Rig rig;
            mixerJudge("volume 0.8 -> 0.1 -> 1", measure(rig.render(blocks, { { 0, play }, { first, trackParam(MixParam::VOLUME, 0.1f) },
                                                                             { second, trackParam(MixParam::VOLUME, 1.0f) } }),
                                                        { first * BLOCK, second * BLOCK }, 2 * EDGE_WINDOW, settle));
        }
        {
            Rig rig;
            mixerJudge("pan centre -> left -> right", measure(rig.render(blocks, { { 0, play }, { first, trackParam(MixParam::PAN, -1.0f) },
                                                                                  { second, trackParam(MixParam::PAN, 1.0f) } }),
                                                             { first * BLOCK, second * BLOCK }, 2 * EDGE_WINDOW, settle));
        }
        {
            // A delay on the bus gives the jump back a moment later, as it came in.
            Rig rig;
            while (rig.audio.getSendCount() < 2)
                rig.audio.addSendBus(rig.audio.getSendCount() == 0 ? EffectType::ReverbEffect : EffectType::DelayEffect);
            rig.audio.setInsertEffect(AudioEngine::sendTrack(1), 0, EffectType::DelayEffect);
            const int end = blocksFor(1.6);
            const auto a = rig.render(end, { { 0, play }, { first, trackParam(MixParam::SEND_BASE + 1, 1.0f) },
                                             { blocksFor(0.9), trackParam(MixParam::SEND_BASE + 1, 0.0f) } });
            // Judged everywhere after the first jump against the dry note before it; the echoes add
            // to the level, so the dry note's steps count twice.
            auto res = measure(a, { first * BLOCK }, (int) a.l.size(), settle);
            res.elsewhere *= 2.0f;
            mixerJudge("send level 0 -> 1 -> 0 into a delay", res);
        }

        beginTest("mixer: an insert switched off and on, replaced and removed under a held note");
        {
            Rig rig;
            rig.audio.addInsertEffect(0, EffectType::DriveEffect);
            mixerJudge("insert off -> on", measure(rig.render(blocks, { { 0, play }, { first, trackParam(AudioEngine::INSERT_ON_BASE, 0.0f) },
                                                                       { second, trackParam(AudioEngine::INSERT_ON_BASE, 1.0f) } }),
                                                  { first * BLOCK, second * BLOCK }, 2 * EDGE_WINDOW, settle));
        }
        {
            Rig rig;
            rig.audio.addInsertEffect(0, EffectType::DriveEffect);
            mixerJudge("insert replaced, then removed",
                       measure(rig.render(blocks, { { 0, play }, { first, [](AudioEngine& a) { a.setInsertEffect(0, 0, EffectType::WaveshaperEffect); } },
                                                    { second, [](AudioEngine& a) { a.removeInsertEffect(0, 0); } } }),
                               { first * BLOCK, second * BLOCK }, 2 * EDGE_WINDOW, settle));
        }

        beginTest("the track's effect blocks (DIRT, TALK, MODFX): every cell jumped under a held note");
        {
            const auto allowed = trackFxAllowed();
            Subject track;
            track.name = "TRK FX";
            for (auto& page : { DirtBlock::page(), TalkBlock::page(), ModFxBlock::page() })
                for (auto& cell : page.params)
                {
                    if (! cell.isValid() || cell.maxVal <= cell.minVal)
                        continue;
                    // The block switched on first (its own first cell up), so that the cell has something to do.
                    Rig rig;
                    const int on = page.params[0].id, id = cell.id;
                    rig.audio.setTrackParameter(0, on, page.params[0].maxVal * 0.5f);
                    const float set = rig.audio.getTrackParameter(0, id), lo = cell.minVal, hi = cell.maxVal;
                    const auto a = rig.render(blocksFor(0.5), { { 0, play }, { first, trackParam(id, hi) }, { blocksFor(0.28), trackParam(id, lo) },
                                                                 { second, trackParam(id, set) } });
                    judge("effect block", track, juce::String(page.name) + " " + cell.label,
                          measure(a, { first * BLOCK, blocksFor(0.28) * BLOCK, second * BLOCK }, 2 * EDGE_WINDOW, settle), allowed);
                }
        }

        beginTest("the engine replaced under a held note, and Shift + Play's silence");
        {
            Rig rig;
            mixerJudge("engine replaced", measure(rig.render(blocks, { { 0, play }, { first, [](AudioEngine& a) { a.setEngineType(0, EngineType::Wavetable); } } }),
                                                 { first * BLOCK }, 2 * EDGE_WINDOW, settle));
        }
        {
            Rig rig;
            std::shared_ptr<EngineBase> parked;
            mixerJudge("preset audition (engine parked and returned)",
                       measure(rig.render(blocks, { { 0, play }, { first, [&parked](AudioEngine& a) { parked = a.parkEngine(0, EngineType::FM); } },
                                                    { first + 1, [&rig, play](AudioEngine& a) { rig.voice(); play(a); } },
                                                    { second, [&parked](AudioEngine& a) { a.unparkEngine(0, parked); } } }),
                               { first * BLOCK, second * BLOCK }, 2 * EDGE_WINDOW, settle));
        }
        {
            Rig rig;
            mixerJudge("silence", measure(rig.render(blocks, { { 0, play }, { first, [](AudioEngine& a) { a.requestSilence(); } } }),
                                         { first * BLOCK }, 2 * EDGE_WINDOW, settle));
        }
    }
};

static ClickAuditTests clickAuditTests;
