#pragma once

// The unified voice as it was before phase U4-UV moved it onto the modulation router (mod::Router):
// UnifiedVoice.h and UnifiedVoiceEngine.h at commit 46fedbc, copied into a namespace of their own
// (the class bodies are unchanged; UnifiedParam, ArchitecturePreset and the shared sections are the
// product's) so that "U4 Unified" can render old against new. Nothing in the product includes this.

#include "../../src/engine/UnifiedVoiceEngine.h"
#include "../../src/engine/EngineModSection.h"

namespace olduv
{

class UnifiedVoice
{
public:
    static constexpr int MAX_OSCS = 3;

    // Wave stacking: layer multiple detuned copies of each oscillator
    // 0=off (1 voice), 1=2 voices, 2=4 voices, 3=8 voices
    static constexpr int MAX_STACK_MODE = 3;
    static constexpr int MAX_STACK_VOICES = 8;

    // What an oscillator slot plays. The first six are the analog waves; the rest are other kinds
    // of oscillator altogether, each with two controls of its own (OscState::a, b):
    //   Wavetable   a = position in the table 0..1,        b = which built-in table (slotTables())
    //   FM          a = modulation index 0..1,             b = modulator ratio (whole number 1..12)
    //   Formant     a = formant frequency 0..1 (1x..16x),  b = width of the formant 0..1
    //   Harmonic    a = tilt 0..1 (bright .. dark),        b = odd 0..1 (all harmonics .. odd only)
    // Every kind is computed from the slot's phase, so tuning, detune, cross-mod, the stack and
    // the mixer treat all of them alike. Whatever modulates PW (an LFO, pad pressure) moves `a`.
    enum class OscWaveform { Sine, Saw, Square, Triangle, Pulse, Noise, Wavetable, FM, Formant, Harmonic };
    static constexpr int NUM_WAVEFORMS = 10;
    static bool isAnalog(OscWaveform w) { return (int) w <= (int) OscWaveform::Noise; }

    // The tables a WT slot plays: the wavetable engine's built-in set, where it is (nothing is
    // copied or made here), read with the wavetable engine's read-out (WavetableRead). 0-8 are
    // the nine the slots have always had, the newer ones follow; the index is stored in projects.
    // Built on first use: call it once from the message thread (the engine's constructor does)
    // before audio runs.
    static const std::vector<const Wavetable*>& sharedTables()
    {
        return WavetableFactory::builtInTables();
    }
    // An LFO or envelope sent to a slot's table cell at full depth has always moved it by the
    // eight steps of the first nine tables; it still does, however long the list grows.
    static constexpr float TABLE_ROUTE_SPAN = (float) (WavetableFactory::NUM_FIRST_TABLES - 1);

    // Wave stacking configuration
    struct WaveStackConfig
    {
        int mode = 0;               // 0=off, 1=2 voices, 2=4 voices, 3=8 voices
        float detuneSpread = 0.1f;  // detune spread in semitones (0.0 to 1.0)
        float stereoSpread = 0.0f;  // stereo width (0.0 mono to 1.0 wide)

        int voiceCount() const
        {
            switch (mode)
            {
                case 1: return 2;
                case 2: return 4;
                case 3: return 8;
                default: return 1;
            }
        }
    };

    struct OscState
    {
        OscWaveform waveform = OscWaveform::Saw;
        float level = 1.0f;
        float detune = 0.0f;
        float pulseWidth = 0.5f;
        float a = 0.0f, b = 0.0f;       // the two controls of the non-analog kinds (see OscWaveform)
        float aMod = 0.0f;              // added to `a` for this sample (LFO / pressure on PW)
        double phase = 0.0;
        bool enabled = true;
    };

    struct AnalogCharacter
    {
        float driftAmount = 0.15f;     // max drift in cents (subtle: ~2.5 cents peak)
        float driftRate = 0.3f;        // Hz — slow wandering
        float waveshapeVar = 0.003f;   // per-osc waveshape offset (tiny component tolerance)
        float softStartCents = 8.0f;   // initial pitch overshoot in cents
        float softStartDecay = 80.0f;  // decay rate (samples to ~37%)
        bool enabled = true;

        // Per-note randomisation (new values are drawn at every note-on)
        bool randomPhase = true;       // oscillators start at random phases
        float slopCents = 0.0f;        // each note lands up to this far off pitch
        float filterVariance = 0.0f;   // each note's cutoff is off by up to this many octaves
        float envVariance = 0.0f;      // each note's envelope times are scaled by up to 2^this either way
    };

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        ampEnv.setSampleRate(sr);
        filterEnv.setSampleRate(sr);
        filterSlot.setSampleRate(sr);
        envChain.setSampleRate(sr);
        lfoBank.setSampleRate(sr);
        mods.setSampleRate(sr);
        lfoAmpSlew.setSampleRate(sr);
        lfoPanSlew.setSampleRate(sr);
        declick.setSampleRate(sr);
        gainDeclick.setSampleRate(sr);
        lfoStep = (float) (2.0 / (0.004 * sr));         // an LFO's whole swing in 4 ms
        levelStep = (float) (1.0 / (0.004 * sr));       // a level's whole range in 4 ms
        octaveRatio = (float) std::exp2(8.0 / (0.004 * sr));   // 8 octaves in 4 ms
    }

    // ---- Destinations that are page cells ------------------------------------------------------
    // After the original destinations (LfoDest below) the list goes on with every other
    // continuous cell of the pages, by page. An LFO (or the extra envelopes) routed to one moves
    // this voice's own value of that cell in the cell's own scale: a linear cell by +-(its whole
    // range) at full depth, a logarithmic one (times, Hz) by +-(half its range in octaves),
    // clamped to the range (ParamRoute, applyParamRoutes, setParam). The engine fills in the
    // range from the cell's ParamSpec when DEST is set.
    //   1A 1B .. 3B  the two SRC cells of each oscillator slot, whatever kind it plays
    //   ATK2 .. REL5 the extra envelopes 1-4 (numbered 2-5, as on MENV); EDPT their depth
    // Not offered: the choices and counts (WAVn, ARCH, MODE, VOX, LEGA, STCK, GLMD, PHAS, TYPE,
    // MODE, ROUT), TUNE / FINE / BEND (PTCH is the pitch), OFSB (CUTB is filter B's cutoff), LEVL
    // (the level of the mixed voices; AMP is each voice's), CURV (would re-bend the envelope
    // every sample), SLOP / WSHP / SOFT / FVAR / EVAR (drawn once per note), and the LFOs' own
    // PHAS / DLY / FADE / WHL / VEL / KTRK / PRES.
    struct ParamDest
    {
        const char* label;
        int id;                 // UnifiedParam / ModParam id
        bool group;             // the first of a page's cells (Func + turn on DEST stops here)
    };
    static constexpr ParamDest PARAM_DESTS[] = {
        { "TUN2", UnifiedParam::TUNE2, true }, { "DET2", UnifiedParam::DETUNE2, false }, { "DRIV", UnifiedParam::MIX_DRIVE, false },
        { "TUN3", UnifiedParam::TUNE3, false }, { "NCOL", UnifiedParam::NOISE_COLOR, false },
        { "1A", UnifiedParam::SRC_A1, true }, { "1B", UnifiedParam::SRC_B1, false }, { "2A", UnifiedParam::SRC_A2, false },
        { "2B", UnifiedParam::SRC_B2, false }, { "3A", UnifiedParam::SRC_A3, false }, { "3B", UnifiedParam::SRC_B3, false },
        { "SPRD", UnifiedParam::SPREAD, true }, { "GLID", UnifiedParam::GLIDE, false },
        { "DRFT", UnifiedParam::DRIFT, false }, { "DRAT", UnifiedParam::DRIFT_RATE, false },
        { "ENV", UnifiedParam::FILTER_ENV, true }, { "FDRV", UnifiedParam::FILTER_DRIVE, false }, { "MORP", UnifiedParam::MORPH, false },
        { "RESB", UnifiedParam::RESONANCE_B, false }, { "BAL", UnifiedParam::BALANCE, false },
        { "DRVB", UnifiedParam::DRIVE_B, false }, { "TRKB", UnifiedParam::B_TRACK, false },
        { "FATK", UnifiedParam::FENV_ATTACK, true }, { "FDEC", UnifiedParam::FENV_DECAY, false },
        { "FSUS", UnifiedParam::FENV_SUSTAIN, false }, { "FREL", UnifiedParam::FENV_RELEASE, false },
        { "KEY", UnifiedParam::KEYTRACK, false }, { "FVEL", UnifiedParam::FILTER_VEL, false }, { "PMOD", UnifiedParam::POLY_MOD, false },
        { "ATK", UnifiedParam::AMP_ATTACK, true }, { "DEC", UnifiedParam::AMP_DECAY, false },
        { "SUS", UnifiedParam::AMP_SUSTAIN, false }, { "REL", UnifiedParam::AMP_RELEASE, false }, { "VEL", UnifiedParam::VEL_SENS, false },
        { "ATK2", ModParam::envParamId(0, ModParam::ENV_ATK), true }, { "DEC2", ModParam::envParamId(0, ModParam::ENV_DEC), false },
        { "SUS2", ModParam::envParamId(0, ModParam::ENV_SUS), false }, { "REL2", ModParam::envParamId(0, ModParam::ENV_REL), false },
        { "ATK3", ModParam::envParamId(1, ModParam::ENV_ATK), true }, { "DEC3", ModParam::envParamId(1, ModParam::ENV_DEC), false },
        { "SUS3", ModParam::envParamId(1, ModParam::ENV_SUS), false }, { "REL3", ModParam::envParamId(1, ModParam::ENV_REL), false },
        { "ATK4", ModParam::envParamId(2, ModParam::ENV_ATK), true }, { "DEC4", ModParam::envParamId(2, ModParam::ENV_DEC), false },
        { "SUS4", ModParam::envParamId(2, ModParam::ENV_SUS), false }, { "REL4", ModParam::envParamId(2, ModParam::ENV_REL), false },
        { "ATK5", ModParam::envParamId(3, ModParam::ENV_ATK), true }, { "DEC5", ModParam::envParamId(3, ModParam::ENV_DEC), false },
        { "SUS5", ModParam::envParamId(3, ModParam::ENV_SUS), false }, { "REL5", ModParam::envParamId(3, ModParam::ENV_REL), false },
        { "EDPT", ModParam::envParamId(0, ModParam::ENV_DEPTH), false },
    };
    static constexpr int NUM_PARAM_DESTS = (int) (sizeof(PARAM_DESTS) / sizeof(PARAM_DESTS[0]));

    // Where an LFO goes. One destination per LFO; its depth parameter sets the amount (ranges at
    // full depth are given with each one). The index is stored in projects: only ever append.
    enum class LfoDest
    {
        Off,
        Pitch,          // all oscillators, +-12 semitones
        Cutoff,         // +-4 octaves (filter B follows as far as its tracking says)
        Resonance,      // +-0.5, both filters
        OscMix,         // oscillator 1 <-> 2 balance, +-0.5
        Amp,            // level, x0..x2
        PulseWidth,     // +-0.4
        CrossMod,       // oscillator 2 -> 1 through-zero FM amount, +-1
        CutoffB,        // filter B only, +-4 octaves
        Pan,            // the voice's place in the stereo field, +-1 (full left / right)
        Osc1Level,      // x0..x2
        Osc2Level,
        Osc3Level,
        NoiseLevel,     // x0..x2
        FilterFM,       // oscillator 2 -> cutoff amount, +-1
        Osc2Pitch,      // oscillator 2 only, +-12 semitones
        Lfo1Rate, Lfo2Rate, Lfo3Rate, Lfo4Rate,        // that LFO's rate, +-2 octaves
        Lfo1Depth, Lfo2Depth, Lfo3Depth, Lfo4Depth,    // that LFO's depth, x0..x2
        FirstParam,                                    // PARAM_DESTS[0], and so on to the end
        Count = FirstParam + NUM_PARAM_DESTS
    };
    static constexpr int NUM_FIXED_DESTS = (int) LfoDest::FirstParam;

    // The page cell behind a destination; -1 for the original ones.
    static int paramOfDest(int dest)
    {
        return dest >= NUM_FIXED_DESTS && dest < (int) LfoDest::Count ? PARAM_DESTS[dest - NUM_FIXED_DESTS].id : -1;
    }

    // Cell texts for the destinations, in enum order.
    static const char* const* lfoDestLabels()
    {
        struct Names
        {
            std::array<const char*, (size_t) LfoDest::Count> text {};
            std::array<int, (size_t) LfoDest::Count> starts {};
            int numStarts = 0;
        };
        static const Names names = []
        {
            static const char* const fixed[NUM_FIXED_DESTS] = {
                "OFF", "PTCH", "CUT", "RESO", "MIX", "AMP", "PW", "XMOD",
                "CUTB", "PAN", "LVL1", "LVL2", "LVL3", "NOIS", "FFM", "PIT2",
                "RAT1", "RAT2", "RAT3", "RAT4", "DPT1", "DPT2", "DPT3", "DPT4" };
            Names n;
            n.starts[(size_t) n.numStarts++] = 0;
            n.starts[(size_t) n.numStarts++] = 1;
            for (int i = 0; i < NUM_FIXED_DESTS; ++i)
                n.text[(size_t) i] = fixed[i];
            for (int i = 0; i < NUM_PARAM_DESTS; ++i)
            {
                n.text[(size_t) (NUM_FIXED_DESTS + i)] = PARAM_DESTS[i].label;
                if (PARAM_DESTS[i].group)
                    n.starts[(size_t) n.numStarts++] = NUM_FIXED_DESTS + i;
            }
            return n;
        }();
        // Func + turn on a DEST cell with these labels moves page by page (ParamSpec::turned).
        static const bool registered = (ChoiceGroups::add(names.text.data(), names.starts.data(), names.numStarts), true);
        juce::ignoreUnused(registered);
        return names.text.data();
    }

    // ---- What the extra envelopes (MENV) can be sent to -----------------------------------------
    // Their DEST keeps its four original values (AMP, CUT, PTCH as they always worked, and OFF)
    // and goes on with the LFOs' list without the three it already has: the envelopes then act
    // like an LFO whose output is the envelope (0..1) times DPTH.
    static constexpr int NUM_ENV_TARGETS = 4;
    static constexpr int NUM_ENV_DESTS = (int) LfoDest::Count;      // 4 + (Count - OFF, PTCH, CUT, AMP)
    static LfoDest lfoDestOfEnvDest(int envDest)
    {
        if (envDest < NUM_ENV_TARGETS || envDest >= NUM_ENV_DESTS)
            return LfoDest::Off;
        int d = envDest - NUM_ENV_TARGETS + 3;          // 3 = RESO, the first one after OFF PTCH CUT
        if (d >= (int) LfoDest::Amp)
            ++d;
        return (LfoDest) d;
    }
    static const char* const* envDestLabels()
    {
        struct Names
        {
            std::array<const char*, (size_t) NUM_ENV_DESTS> text {};
            std::array<int, (size_t) NUM_ENV_DESTS> starts {};
            int numStarts = 0;
        };
        static const Names names = []
        {
            Names n;
            n.text[0] = "AMP"; n.text[1] = "CUT"; n.text[2] = "PTCH"; n.text[3] = "OFF";
            for (int i = NUM_ENV_TARGETS; i < NUM_ENV_DESTS; ++i)
                n.text[(size_t) i] = lfoDestLabels()[(int) lfoDestOfEnvDest(i)];
            // Groups for Func + turn: the four original values, the LFOs' original destinations,
            // then page by page (a page cell has the same index here as in the LFOs' list).
            n.starts[(size_t) n.numStarts++] = 0;
            n.starts[(size_t) n.numStarts++] = NUM_ENV_TARGETS;
            for (int i = 0; i < NUM_PARAM_DESTS; ++i)
                if (PARAM_DESTS[i].group)
                    n.starts[(size_t) n.numStarts++] = NUM_FIXED_DESTS + i;
            return n;
        }();
        static const bool registered = (ChoiceGroups::add(names.text.data(), names.starts.data(), names.numStarts), true);
        juce::ignoreUnused(registered);
        return names.text.data();
    }

    // True for destinations that work through oscillator 2 even when it is out of the mix.
    static bool lfoDestNeedsOsc2(LfoDest d)
    {
        return d == LfoDest::CrossMod || d == LfoDest::FilterFM;
    }

    enum class GlideMode { Always, Legato };

    void setNote(int note, float velocity)
    {
        currentNote = note;
        currentVelocity = velocity;
        targetPitch = (float) note;

        // Glide starts from wherever the voice is now. Without glide, or for a voice that is not
        // sounding in legato-only mode (or has never played), the pitch jumps.
        const bool sounding = active && ampEnv.getStage() != Envelope::Stage::Release;
        if (glideTime <= 0.0005f || !hasPlayed || (glideMode == GlideMode::Legato && !sounding))
            currentPitch = targetPitch;

        hasPlayed = true;
        baseFrequency = pitchToHz(currentPitch);
        for (int i = 0; i < LFOBank::MAX_LFOS; ++i)
            lfoBank.getLFO(i).setKeyNote((float) note);
    }

    // Change pitch without retriggering envelopes (mono legato).
    void setNoteLegato(int note)
    {
        currentNote = note;
        targetPitch = (float) note;
        if (glideTime <= 0.0005f)
            currentPitch = targetPitch;
        baseFrequency = pitchToHz(currentPitch);
        for (int i = 0; i < LFOBank::MAX_LFOS; ++i)
            lfoBank.getLFO(i).setKeyNote((float) note);
        mods.setNote(note, currentVelocity);    // a mod slot's KEY follows the legato note
    }

    void noteOn()
    {
        if (! active)
        {
            lfoAmpSlew.snap();      // from silence: nothing to step from
            lfoPanSlew.snap();
        }
        else
        {
            declick.arm();          // struck again while sounding: new phases, a new velocity
            gainDeclick.arm();
        }
        finishLfoClosings();        // a new note takes every destination as it stands
        mods.finishClosing(true);
        mods.setNote(currentNote, currentVelocity);
        mods.noteOn();
        struck = true;              // ... and every glided value (glide)
        // Per-note component spread: pitch, cutoff and envelope times are each a little different
        // on every note, as on a polysynth whose voices are never identical.
        noteSlopSemis = 0.0f;
        noteFilterOct = 0.0f;
        if (analog.enabled)
        {
            auto& rng = juce::Random::getSystemRandom();
            noteSlopSemis = (rng.nextFloat() * 2.0f - 1.0f) * analog.slopCents / 100.0f;
            noteFilterOct = (rng.nextFloat() * 2.0f - 1.0f) * analog.filterVariance;
            if (haveBaseEnvelopes && (analog.envVariance > 0.0f || envelopesScaled))
            {
                float scale = std::exp2((rng.nextFloat() * 2.0f - 1.0f) * analog.envVariance);
                ampEnv.setParams(scaledEnvelope(baseAmpEnv, scale));
                filterEnv.setParams(scaledEnvelope(baseFilterEnv, scale));
                envelopesScaled = analog.envVariance > 0.0f;
            }
        }

        ampEnv.noteOn();
        filterEnv.noteOn();
        envChain.noteOn();
        lfoNoteVelocity = currentVelocity;      // velocity -> LFO depth is fixed for the note
        lfoBank.triggerAll();
        active = true;

        if (analog.enabled)
        {
            auto& rng = juce::Random::getSystemRandom();

            // Randomize oscillator starting phases (or start them together for a fixed attack)
            for (auto& osc : oscs)
                osc.phase = analog.randomPhase ? rng.nextDouble() : 0.0;

            // Randomize stacked voice phases
            for (auto& sp : stackPhases)
                for (auto& p : sp)
                    p = rng.nextDouble();

            // Set per-oscillator waveshape variation (component tolerance)
            for (int i = 0; i < MAX_OSCS; ++i)
                oscWaveshapeOffset[i] = (rng.nextFloat() * 2.0f - 1.0f) * analog.waveshapeVar;

            // Reset soft-start pitch overshoot
            softStartSamples = 0.0f;
        }
        else
        {
            // Reset stacked voice phases deterministically
            for (auto& sp : stackPhases)
                for (int v = 0; v < MAX_STACK_VOICES; ++v)
                    sp[v] = (double)v / MAX_STACK_VOICES;
        }
    }

    void noteOff()
    {
        ampEnv.noteOff();
        filterEnv.noteOff();
        envChain.noteOff();
    }

    bool isActive() const { return active; }
    bool isGateOpen() const { return active && ampEnv.getStage() != Envelope::Stage::Release; }

    // One sample of what the LFOs, the mod slots and the extra envelopes add to the voice's fixed
    // destinations (the five outputs start at zero). Called once per sample inside process().
    void processModulation(float& pitchOffsetSemitones, float& cutoffMod,
                           float& resonanceMod, float& oscMixMod, float& ampMod)
    {
        juce::ignoreUnused(cutoffMod);
        int lfoCount = lfoBank.getActiveCount();
        float envOutput = envChain.getLastOutput();

        // LFO destinations (each LFO's output is already scaled by its depth).
        lfoCutoffOct = 0.0f;
        lfoCutoffBOct = 0.0f;
        lfoPwMod = 0.0f;
        lfoXmodMod = 0.0f;
        lfoFilterFmMod = 0.0f;
        lfoOsc2Semis = 0.0f;
        lfoNoiseGain = 1.0f;
        lfoPan = 0.0f;
        lfoOscGain.fill(1.0f);
        std::array<float, LFOBank::MAX_LFOS> rateMod {}, depthMod {};
        float routedAmp = 0.0f;
        auto route = [&](LfoDest d, float v)
        {
            switch (d)
            {
                case LfoDest::CutoffB:    lfoCutoffBOct += v * 4.0f; break;
                case LfoDest::Pan:        lfoPan += v; break;
                case LfoDest::Osc1Level:  lfoOscGain[0] *= std::clamp(1.0f + v, 0.0f, 2.0f); break;
                case LfoDest::Osc2Level:  lfoOscGain[1] *= std::clamp(1.0f + v, 0.0f, 2.0f); break;
                case LfoDest::Osc3Level:  lfoOscGain[2] *= std::clamp(1.0f + v, 0.0f, 2.0f); break;
                case LfoDest::NoiseLevel: lfoNoiseGain *= std::clamp(1.0f + v, 0.0f, 2.0f); break;
                case LfoDest::FilterFM:   lfoFilterFmMod += v; break;
                case LfoDest::Osc2Pitch:  lfoOsc2Semis += v * 12.0f; break;
                case LfoDest::Lfo1Rate: case LfoDest::Lfo2Rate: case LfoDest::Lfo3Rate: case LfoDest::Lfo4Rate:
                    rateMod[(size_t) ((int) d - (int) LfoDest::Lfo1Rate)] += v * 2.0f;
                    break;
                case LfoDest::Lfo1Depth: case LfoDest::Lfo2Depth: case LfoDest::Lfo3Depth: case LfoDest::Lfo4Depth:
                    depthMod[(size_t) ((int) d - (int) LfoDest::Lfo1Depth)] += v;
                    break;
                case LfoDest::Pitch:      pitchOffsetSemitones += v * 12.0f; break;
                case LfoDest::Cutoff:     lfoCutoffOct += v * 4.0f; break;
                case LfoDest::Resonance:  resonanceMod += v * 0.5f; break;
                case LfoDest::OscMix:     oscMixMod += v * 0.5f; break;
                case LfoDest::Amp:        routedAmp += v; break;
                case LfoDest::PulseWidth: lfoPwMod += v * 0.4f; break;
                case LfoDest::CrossMod:   lfoXmodMod += v; break;
                default: break;          // off, or a page cell (applyParamRoutes)
            }
        };
        // Pitch and the other LFOs' rate and depth take an LFO's output as it is (a pitch step is no
        // click); everything else takes it on a lead that does not jump (lfoHeard, see glide).
        for (int i = 0; i < LFOBank::MAX_LFOS; ++i)
        {
            const LfoDest d = lfoDest[(size_t) i];
            const bool asItIs = d == LfoDest::Pitch || d == LfoDest::Osc2Pitch
                                || ((int) d >= (int) LfoDest::Lfo1Rate && (int) d <= (int) LfoDest::Lfo4Depth);
            if (asItIs ? i < lfoCount : lfoHeard[(size_t) i] != 0.0f)
                route(d, asItIs ? lfoBank.getOutput(i) : lfoHeard[(size_t) i]);
        }
        // The mod slots on the same destinations, summed with the LFOs (a slot aimed at a page cell
        // is applied with the cells: applyParamRoutes).
        // The slots on one oscillator's (or the noise's) level are summed first and are one factor
        // of 0..2 together: sixteen factors of 2 would be a gain of 65536.
        if (mods.live())
        {
            std::array<float, 4> slotLevel {};
            bool anyLevel = false;
            for (int a = 0; a < mods.count(); ++a)
            {
                const int slot = mods.slotAt(a);
                const ModSlotTarget& t = mods.target(slot);
                if (t.kind != ModSlotTarget::Fixed)
                    continue;
                const LfoDest d = (LfoDest) t.index;
                const int level = d == LfoDest::Osc1Level ? 0 : (d == LfoDest::Osc2Level ? 1
                                : (d == LfoDest::Osc3Level ? 2 : (d == LfoDest::NoiseLevel ? 3 : -1)));
                if (level >= 0)
                {
                    slotLevel[(size_t) level] += mods.heardOf(slot);
                    anyLevel = true;
                    continue;
                }
                const bool asItIs = d == LfoDest::Pitch || d == LfoDest::Osc2Pitch
                                    || ((int) d >= (int) LfoDest::Lfo1Rate && (int) d <= (int) LfoDest::Lfo4Depth);
                route(d, asItIs ? mods.outputOf(slot) : mods.heardOf(slot));
            }
            if (anyLevel)
            {
                for (int o = 0; o < 3; ++o)
                    lfoOscGain[(size_t) o] *= std::clamp(1.0f + slotLevel[(size_t) o], 0.0f, 2.0f);
                lfoNoiseGain *= std::clamp(1.0f + slotLevel[3], 0.0f, 2.0f);
            }
        }
        // A ramp, square or random LFO may not step the level or the pan (dsp::GainSlew); the
        // envelopes' own share of the level is left as it is.
        ampMod += lfoAmpSlew.process(routedAmp);
        routedAmp = 0.0f;
        // The extra envelopes sent to one of the LFOs' destinations.
        if (envDest != LfoDest::Off && envChain.getStageCount() > 0)
            route(envDest, envOutput * envChain.getDepth());
        ampMod += routedAmp;
        lfoPan = std::clamp(lfoPanSlew.process(lfoPan), -1.0f, 1.0f);

        // LFOs modulating LFOs take effect on the next sample.
        for (int i = 0; i < LFOBank::MAX_LFOS; ++i)
        {
            auto& lfo = lfoBank.getLFO(i);
            lfo.setRateMod(rateMod[(size_t) i]);
            lfo.setDepthMod(depthMod[(size_t) i]);
        }
    }

    // ---- The track's input at audio rate (XIN) --------------------------------------------------
    // What the input does to this voice, sample by sample (the engine hands every voice the mono
    // sum of the input, held to +-1). XDPT is "how much" in every mode, and 0 is the voice alone:
    //   PM    the input moves oscillator 1's phase along the cross-modulation path (through-zero
    //         FM: the phase offset is the leaky running sum of the input, in carrier cycles). A
    //         full-scale sine at the oscillator's own pitch is an index of pi at full XDPT, as
    //         XMOD from oscillator 2 is; a lower input moves it further (index ~ carrier / input).
    //   RING  the mixer's output (the three slots and the noise, before DRIV) is multiplied by
    //         the input: out = mix * ((1 - XDPT) + XDPT * input).
    //   SYNC  oscillator 1 (and its stack) starts its cycle again when the input rises through
    //         +threshold after having been below -threshold; XDPT is the sensitivity
    //         (threshold = 0.005 + 0.995 * (1 - XDPT)^2 of full scale: -12 dB at 50, -46 dB at 100).
    // The numbers are stored: append only.
    enum XinMode { XinOff = 0, XinPm, XinRing, XinSync, NUM_XIN_MODES };
    void setXinMode(int mode) { xinMode = std::clamp(mode, 0, (int) NUM_XIN_MODES - 1); }
    void setXinDepth(float d) { xinDepth = d > 0.0f ? std::min(d, 1.0f) : 0.0f; }     // (not a number: 0)
    static float xinSyncThreshold(float depth)
    {
        const float away = 1.0f - std::clamp(depth, 0.0f, 1.0f);
        return 0.005f + 0.995f * away * away;
    }

    float process() { return process(0.0f, false); }

    // `xin`: this sample of the track's input (mono, finite, within +-1); `xinConnected`: the track
    // has an input at all. Without one, or with XIN OFF, the voice is what it is without the cells.
    float process(float xin, bool xinConnected)
    {
        if (!active)
            return 0.0f;

        const int xm = xinConnected ? xinMode : (int) XinOff;
        if (xm != xinModeNow)
        {
            // Another mode (or the input came or went): nothing is carried over from the last one.
            xinModeNow = xm;
            xinSum = 0.0;
            xinArmed = false;
            xinPrev = xin;
            xinDepthNow = xinDepth;
        }
        if (xm == XinOff || struck)                 xinDepthNow = xinDepth;
        else if (xinDepthNow != xinDepth)           xinDepthNow = dsp::towards(xinDepthNow, xinDepth, levelStep);

        // Wheel, velocity and this voice's pressure move each LFO's depth.
        for (int i = 0; i < LFOBank::MAX_LFOS; ++i)
        {
            const auto& c = lfoControl[(size_t) i];
            lfoBank.getLFO(i).setDepthOffset(c.wheel * lfoWheel + c.velocity * lfoNoteVelocity + c.pressure * lfoPressure);
        }

        // Process LFO bank
        lfoBank.processAll();
        const bool firstSample = struck;
        glide();
        if (mods.live())
            runSlots(firstSample);

        // Process envelope chain (needed for mod matrix feed)
        float chainMod = envChain.process();

        // Feed mod sources and read modulated offsets
        float modPitchOffset = 0.0f, modCutoffOffset = 0.0f;
        float modResonanceOffset = 0.0f, modOscMixOffset = 0.0f, modAmpOffset = 0.0f;
        processModulation(modPitchOffset, modCutoffOffset, modResonanceOffset,
                          modOscMixOffset, modAmpOffset);
        if (anyParamRoute || mods.keyed())
            applyParamRoutes(chainMod);

        // Update analog drift (slow filtered noise — one-pole lowpass on white noise)
        float driftSemitones = 0.0f;
        if (analog.enabled)
        {
            double driftPhaseInc = analog.driftRate / sampleRate;
            driftPhase += driftPhaseInc;
            if (driftPhase >= 1.0)
                driftPhase -= 1.0;

            // Two slow LFOs at incommensurate rates for natural-sounding wander
            float drift1 = std::sin((float) driftPhase * dsp::TWO_PI_F);
            float drift2 = std::sin((float) driftPhase * dsp::TWO_PI_F * 1.618f); // golden ratio
            float rawDrift = (drift1 * 0.6f + drift2 * 0.4f) * analog.driftAmount;

            // One-pole lowpass for smooth wandering
            driftFiltered += 0.0001f * (rawDrift - driftFiltered);
            driftSemitones = driftFiltered / 100.0f; // cents to semitones

            // Soft-start pitch overshoot (decaying exponential)
            float softStartOffset = 0.0f;
            if (softStartSamples < analog.softStartDecay * 6.0f)
            {
                softStartOffset = (analog.softStartCents / 100.0f)
                    * std::exp(-softStartSamples / analog.softStartDecay);
                softStartSamples += 1.0f;
            }
            driftSemitones += softStartOffset;
        }

        // Apply mod matrix pitch offset (semitones)
        driftSemitones += modPitchOffset;

        // Glide towards the target note.
        if (currentPitch != targetPitch)
        {
            currentPitch += (targetPitch - currentPitch) * glideCoeff;
            if (std::abs(targetPitch - currentPitch) < 0.002f)
                currentPitch = targetPitch;
            baseFrequency = pitchToHz(currentPitch);
        }

        // Extra envelopes aimed at pitch.
        if (envChain.getStageCount() > 0 && envChain.getTarget() == EnvelopeChain::Target::Pitch)
            driftSemitones += chainMod * envChain.getDepth() * 24.0f;

        // Pressure vibrato (about 5.5 Hz, up to one semitone at full depth).
        if (atVibrato != 0.0f)
        {
            vibratoPhase += 5.5 / sampleRate;
            if (vibratoPhase >= 1.0)
                vibratoPhase -= 1.0;
            driftSemitones += std::sin((float) vibratoPhase * dsp::TWO_PI_F) * atVibrato;
        }

        // Global tuning, pitch bend and unison detune apply to every oscillator.
        driftSemitones += tuneSemitones + bendSemitones + unisonDetune + noteSlopSemis;

        // Poly-mod: the filter envelope bends oscillator 2 (its value from the previous sample).
        const float polyModSemis = polyMod * lastFilterEnv * 24.0f + lfoOsc2Semis;

        const float pwMod = lfoPwMod + atTimbreNow * 0.4f + pwLag;
        const float xmodNow = std::clamp(crossModNow + lfoXmodMod + atFmNow, 0.0f, 1.0f);
        modOscMixOffset += atWavePosNow * 0.5f;

        // Cross-modulation is through-zero linear FM: oscillator 2 moves oscillator 1's FREQUENCY in
        // proportion to that frequency, and far enough the phase runs backwards. Its effect on the
        // phase is the running sum of oscillator 2 (so a bright modulator is smoothed, not
        // differentiated as with plain phase modulation, and whole-number tunings stay harmonic at
        // every depth); the sum leaks below about 8 Hz, so an offset in the modulator (a narrow
        // pulse) shifts the phase and never the pitch. With a sine at the same pitch, full XMOD is
        // an FM index of pi, as before.
        double xmodPhase = 0.0;
        if (xmodNow > 0.0f || xmodSum != 0.0)
        {
            const double carrierInc = baseFrequency * std::exp2((oscs[0].detune + driftSemitones) / 12.0) / sampleRate;
            xmodSum = xmodSum * (1.0 - 50.3 / sampleRate) + (double) lastOsc2 * carrierInc;
            if (std::abs(xmodSum) < 1.0e-12) xmodSum = 0.0;
            xmodPhase = (double) xmodNow * XMOD_INDEX * xmodSum;
        }

        // XIN PM: the input on the same path (its own running sum, so XMOD and XIN add).
        double xinPhase = 0.0;
        if (xm == XinPm)
        {
            const double carrierInc = baseFrequency * std::exp2((oscs[0].detune + driftSemitones) / 12.0) / sampleRate;
            xinSum = xinSum * (1.0 - 50.3 / sampleRate) + (double) xin * carrierInc;
            if (! (std::abs(xinSum) < 1.0e6)) xinSum = 0.0;         // (also what is not a number)
            else if (std::abs(xinSum) < 1.0e-12) xinSum = 0.0;
            xinPhase = (double) xinDepthNow * XMOD_INDEX * xinSum;
        }
        // XIN SYNC: armed below -threshold, fired on the way up through +threshold. `xinSince` is
        // how much of this sample had passed when the input crossed it. The cycle starts again
        // just past the stretch where the band-limited waves round off their own wrap (the first
        // step of the cycle as generateOsc counts it, `xinStart`: about one sample late, the same
        // for every restart): half of that rounding on a jump of another size is a click of its own.
        bool xinFired = false;
        double xinSince = 0.0, xinStart = 0.0;
        if (xm == XinSync)
        {
            const float threshold = xinSyncThreshold(xinDepthNow);
            if (xinDepthNow > 0.0f)
            {
                if (xin <= -threshold)
                    xinArmed = true;
                else if (xinArmed && xin >= threshold)
                {
                    xinArmed = false;
                    xinFired = true;
                    const float rise = xin - xinPrev;
                    xinSince = rise > 0.0f ? (double) std::clamp((xin - threshold) / rise, 0.0f, 1.0f) : 0.0;
                    xinStart = baseFrequency * std::exp2(oscs[0].detune / 12.0) / sampleRate;
                }
            }
            else
                xinArmed = false;
            xinPrev = xin;
        }

        float oscMix = 0.0f;
        for (int i = 0; i < MAX_OSCS; ++i)
        {
            // (An oscillator that has just been taken out of the mix is heard until it has faded.)
            const float setLevel = oscLevelNow[(size_t) i];
            if (!oscs[i].enabled && setLevel <= 0.0f)
                continue;

            // Apply osc mix modulation: scale level of osc 1 up / osc 0 down (or vice versa)
            float levelScale = setLevel;
            if (MAX_OSCS >= 2 && modOscMixOffset != 0.0f)
            {
                if (i == 0)
                    levelScale = std::clamp(setLevel - modOscMixOffset, 0.0f, 2.0f);
                else if (i == 1)
                    levelScale = std::clamp(setLevel + modOscMixOffset, 0.0f, 2.0f);
            }
            levelScale *= lfoOscGain[(size_t) i];

            int numStackVoices = waveStack.voiceCount();

            if (numStackVoices <= 1)
            {
                // No stacking: single oscillator
                double freq = baseFrequency * std::exp2((oscs[i].detune + driftSemitones
                                                         + (i == 1 ? polyModSemis : 0.0f)) / 12.0);
                double phaseInc = freq / sampleRate;
                // (wrapped for any step: one subtraction let the phase run away once a pitch passed
                // the sample rate - a high key with PMOD or TUNE and BEND up - and the level with it)
                oscs[i].phase = dsp::wrapPhase(oscs[i].phase + phaseInc);
                if (i == 0 && xinFired)
                    oscs[i].phase = dsp::wrapPhase(xinStart + phaseInc * xinSince);

                OscState shaped = oscs[i];
                shaped.pulseWidth = std::clamp(shaped.pulseWidth + pwMod, 0.05f, 0.95f);
                shaped.aMod = pwMod;
                if (i == 0 && xmodNow > 0.0f)
                {
                    double pm = shaped.phase + xmodPhase;
                    shaped.phase = pm - std::floor(pm);
                }
                if (i == 0 && xinPhase != 0.0)
                    shaped.phase = dsp::wrapPhase(shaped.phase + xinPhase);

                float sample = generateOsc(shaped, oscWaveshapeOffset[i]);
                if (i == 1)
                    lastOsc2 = sample;
                oscMix += sample * levelScale;
            }
            else
            {
                // Wave stacking: layer detuned copies
                float stackMix = 0.0f;
                float gain = cachedStackGain;

                for (int v = 0; v < numStackVoices; ++v)
                {
                    // Spread detune symmetrically around center
                    float voiceDetune = 0.0f;
                    if (numStackVoices > 1)
                    {
                        float pos = ((float)v / (float)(numStackVoices - 1)) * 2.0f - 1.0f; // -1 to +1
                        voiceDetune = pos * waveStack.detuneSpread;
                    }

                    double freq = baseFrequency * std::exp2(
                        (oscs[i].detune + driftSemitones + voiceDetune + (i == 1 ? polyModSemis : 0.0f)) / 12.0);
                    double phaseInc = freq / sampleRate;

                    stackPhases[i][v] = dsp::wrapPhase(stackPhases[i][v] + phaseInc);
                    if (i == 0 && xinFired)
                        stackPhases[i][v] = dsp::wrapPhase(xinStart + phaseInc * xinSince);

                    // Create a temporary OscState for generation with the stacked phase
                    OscState tempOsc = oscs[i];
                    tempOsc.phase = stackPhases[i][v];
                    tempOsc.pulseWidth = std::clamp(tempOsc.pulseWidth + pwMod, 0.05f, 0.95f);
                    tempOsc.aMod = pwMod;
                    if (i == 0 && xmodNow > 0.0f)
                    {
                        double pm = tempOsc.phase + xmodPhase;
                        tempOsc.phase = pm - std::floor(pm);
                    }
                    if (i == 0 && xinPhase != 0.0)
                        tempOsc.phase = dsp::wrapPhase(tempOsc.phase + xinPhase);
                    float sample = generateOsc(tempOsc, oscWaveshapeOffset[i]);
                    stackMix += sample * gain;
                }

                if (i == 1)
                    lastOsc2 = stackMix;
                oscMix += stackMix * levelScale;
            }
        }

        // Noise source and mixer drive (soft saturation before the filter).
        if (noiseLevelNow > 0.0f)
        {
            // Colour: a one-pole low-pass on the noise, white at 0 and dark at 1.
            float n = dsp::xorshiftNoise(noiseSeed);
            noiseState += noiseCoeff * (n - noiseState);
            oscMix += noiseState * noiseLevelNow * noiseMakeup * lfoNoiseGain;
        }

        // XIN RING: the mixer's output times the input, from none of it (XDPT 0) to all of it.
        if (xm == XinRing)
            oscMix *= 1.0f + xinDepthNow * (xin - 1.0f);

        if (mixDriveNow > 0.0f)
        {
            const float g = 1.0f + mixDriveNow * 5.0f;
            oscMix = dsp::fastTanh(oscMix * g) / dsp::fastTanh(g) ;
        }

        // Filter cutoff: envelope, key tracking, velocity, LFOs and pressure all move it in octaves
        // around the base frequency; mod-matrix routings add Hz on top.
        float filterEnvVal = filterEnv.process();
        lastFilterEnv = filterEnvVal;
        const float filterFmNow = lfoFilterFmMod != 0.0f ? std::clamp(filterFm + lfoFilterFmMod, 0.0f, 1.0f) : filterFm;
        float cutoffOct = filterFmNow * lastOsc2 * 2.0f     // audio-rate cutoff modulation from oscillator 2
                        + filterEnvVal * filterEnvAmountNow * FILTER_ENV_OCTAVES
                        + filterKeyTrack * (currentPitch - 60.0f) / 12.0f
                        + filterVelAmount * currentVelocity * 4.0f
                        + lfoCutoffOct
                        + atCutoffNow * 4.0f
                        + noteFilterOct;
        float cutoff = filterCutoffNow * (cutoffOct != 0.0f ? std::exp2(cutoffOct) : 1.0f) + modCutoffOffset;
        // Filter B follows that modulation fully, partly or not at all.
        float cutoffB = filterCutoffNow * (cutoffOct != 0.0f ? std::exp2(cutoffOct * filterBTrack) : 1.0f)
                        * filterBRatio + modCutoffOffset * filterBTrack;
        if (lfoCutoffBOct != 0.0f)
            cutoffB *= std::exp2(lfoCutoffBOct);

        // Apply chain modulation to filter cutoff if targeted there
        if (envChain.getStageCount() > 0 && envChain.getTarget() == EnvelopeChain::Target::FilterCutoff)
            cutoff = envChain.getModulatedValue(cutoff, 10000.0f);

        cutoff = juce::jlimit(20.0f, 20000.0f, cutoff);
        filterSlot.setCutoff(0, cutoff);
        if (filterSlot.getModelB() != FilterModel::Bypass)
            filterSlot.setCutoff(1, juce::jlimit(20.0f, 20000.0f, cutoffB));

        // Apply resonance modulation only when there is actual mod contribution (and put the base
        // values back when it stops).
        if (modResonanceOffset != 0.0f)
        {
            filterSlot.setResonance(0, std::clamp(resonanceNow + modResonanceOffset, 0.0f, 1.0f));
            filterSlot.setResonance(1, std::clamp(resonanceBNow + modResonanceOffset, 0.0f, 1.0f));
            resonanceModulated = true;
        }
        else if (resonanceModulated || resonanceGlides)
        {
            filterSlot.setResonance(0, resonanceNow);
            filterSlot.setResonance(1, resonanceBNow);
            resonanceModulated = false;
        }

        float filtered = filterSlot.process(declick.process(oscMix));

        float ampVal = ampEnv.process();

        // Apply chain modulation to amplitude if targeted there
        if (envChain.getStageCount() > 0 && envChain.getTarget() == EnvelopeChain::Target::Amplitude)
            ampVal = envChain.getModulatedValue(ampVal);

        if (!ampEnv.isActive())
            active = false;

        // Apply amp modulation (mod matrix, LFOs, pressure) AFTER filter
        float ampScale = std::clamp(1.0f + modAmpOffset + atVolumeNow, 0.0f, 2.0f);

        // Velocity sensitivity: 0 = every note at full level, 1 = level follows velocity (velGainNow).
        return gainDeclick.process(filtered * ampVal * velGainNow * ampScale * levelNow);
    }

    // The engine's LEVL (each voice applies it, so that a note takes it whole).
    void setLevel(float l) { level = l; }

    void reset()
    {
        ampEnv.reset();
        filterEnv.reset();
        filterSlot.reset();
        envChain.reset();
        lfoBank.resetAll();
        declick.reset();
        gainDeclick.reset();
        finishLfoClosings();
        mods.reset();
        active = false;
        for (auto& osc : oscs)
            osc.phase = 0.0;
        for (auto& sp : stackPhases)
            sp.fill(0.0);
        driftPhase = 0.0;
        driftFiltered = 0.0f;
        softStartSamples = 0.0f;
        lastOsc2 = 0.0f;
        xmodSum = 0.0;
        xinSum = 0.0;
        xinArmed = false;
        xinPrev = 0.0f;
        lastFilterEnv = 0.0f;
        vibratoPhase = 0.0;
        currentPitch = targetPitch;
    }

    OscState& getOsc(int index) { return oscs[(size_t) index]; }
    Envelope& getAmpEnvelope() { return ampEnv; }
    Envelope& getFilterEnvelope() { return filterEnv; }
    FilterSlot& getFilterSlot() { return filterSlot; }
    AnalogCharacter& getAnalogCharacter() { return analog; }
    EnvelopeChain& getEnvelopeChain() { return envChain; }
    LFOBank& getLFOBank() { return lfoBank; }
    VoiceModSlots& getModSlots() { return mods; }
    const VoiceModSlots& getModSlots() const { return mods; }
    WaveStackConfig& getWaveStack() { return waveStack; }
    const WaveStackConfig& getWaveStack() const { return waveStack; }

    void setWaveStackMode(int mode)
    {
        waveStack.mode = std::clamp(mode, 0, MAX_STACK_MODE);
        cachedStackGain = 1.0f / std::sqrt((float) waveStack.voiceCount());
    }

    void setWaveStackDetune(float spread)
    {
        waveStack.detuneSpread = std::clamp(spread, 0.0f, 1.0f);
    }

    void setFilterBaseCutoff(float hz) { filterBaseCutoff = hz; }
    // -1..1: the filter envelope sweeps the cutoff by up to FILTER_ENV_OCTAVES octaves either way.
    void setFilterEnvAmount(float amount) { filterEnvAmount = std::clamp(amount, -1.0f, 1.0f); }
    void setFilterKeyTrack(float amount) { filterKeyTrack = std::clamp(amount, -1.0f, 1.0f); }
    void setFilterVelAmount(float amount) { filterVelAmount = std::clamp(amount, 0.0f, 1.0f); }
    void setFilterMorph(float m) { filterSlot.setMorph(std::clamp(m, 0.0f, 1.0f)); }
    void setFilterBOffset(float semitones)
    {
        filterBOffsetSemis = semitones;
        filterBRatio = std::exp2(semitones / 12.0f);
    }

    void setNoiseLevel(float l) { noiseLevel = std::clamp(l, 0.0f, 1.0f); }
    void setNoiseColor(float c)
    {
        c = std::clamp(c, 0.0f, 1.0f);
        noiseCoeff = 1.0f - 0.985f * c;
        noiseMakeup = 1.0f + 5.0f * c;      // darker noise is quieter: keep it usable
    }

    // The envelopes as set by the user; per-note variation is applied on top of these.
    void setAmpEnvParams(const Envelope::Params& p) { baseAmpEnv = p; ampEnv.setParams(p); haveBaseEnvelopes = true; }
    void setFilterEnvParams(const Envelope::Params& p) { baseFilterEnv = p; filterEnv.setParams(p); haveBaseEnvelopes = true; }
    void setMixDrive(float d) { mixDrive = std::clamp(d, 0.0f, 1.0f); }
    void setCrossMod(float x) { crossMod = std::clamp(x, 0.0f, 1.0f); }
    void setPulseWidth(float pw)
    {
        for (auto& o : oscs)
            o.pulseWidth = std::clamp(pw, 0.05f, 0.95f);
    }
    void setTune(float semitones) { tuneSemitones = semitones; }
    void setPitchBend(float semitones) { bendSemitones = semitones; }
    void setUnisonDetune(float semitones) { unisonDetune = semitones; }
    void setVelocitySensitivity(float s) { velSensitivity = std::clamp(s, 0.0f, 1.0f); }
    void setGlideMode(GlideMode m) { glideMode = m; }
    void setGlideTime(float seconds)
    {
        glideTime = std::max(0.0f, seconds);
        // One-pole approach: about 95 % of the way after glideTime.
        glideCoeff = glideTime > 0.0005f
            ? 1.0f - std::exp(-3.0f / (glideTime * (float) sampleRate))
            : 1.0f;
    }
    // An LFO taken off a destination under a sounding note lets go of it first: its output glides
    // to zero over a few milliseconds, and only then does it move (a snap back would be a click).
    void setLfoDest(int lfoIndex, LfoDest d)
    {
        if (lfoIndex < 0 || lfoIndex >= LFOBank::MAX_LFOS)
            return;
        const auto i = (size_t) lfoIndex;
        if (lfoClosing[i])
        {
            if (d == lfoDest[i])        // back where it was: it stays
            {
                lfoClosing[i] = false;
                routePending[i] = false;
                lfoCatchUp[i] = true;
            }
            else
                pendingDest[i] = d;
        }
        else if (active && d != lfoDest[i] && lfoHeard[i] != 0.0f)
        {
            pendingDest[i] = d;
            lfoClosing[i] = true;
            routePending[i] = false;
        }
        else
            lfoDest[i] = d;
    }
    // The extra envelopes as a source for one of the LFOs' destinations (Off: their own AMP / CUT
    // / PTCH target applies, EnvelopeChain::Target).
    void setEnvDest(LfoDest d) { envDest = d; }
    LfoDest getEnvDest() const { return envDest; }

    // ---- Page cells as destinations -------------------------------------------------------------
    // Sources: the four LFOs, then the extra envelopes.
    static constexpr int NUM_MOD_SOURCES = LFOBank::MAX_LFOS + 1;
    static constexpr int ENV_SOURCE = LFOBank::MAX_LFOS;

    // Source `source` moves page cell `id` (-1: nothing): `base` is the cell's set value, lo..hi
    // its range, log whether it is a logarithmic cell.
    // `span` (when above 0) is how far full depth moves a linear cell, where that is not hi - lo.
    void setParamRoute(int source, int id, float base = 0.0f, float lo = 0.0f, float hi = 1.0f, bool log = false, float span = 0.0f)
    {
        if (source < 0 || source >= NUM_MOD_SOURCES)
            return;
        if (source < LFOBank::MAX_LFOS && lfoClosing[(size_t) source])
        {
            // The LFO is still letting go of where it was: its new cell waits with its destination.
            ParamRoute p;
            p.id = id; p.base = base; p.lo = lo; p.hi = hi;
            p.log = log && lo > 0.0f && hi > lo;
            p.span = p.log ? 0.5f * std::log2(hi / lo) : (span > 0.0f ? span : hi - lo);
            pendingRoute[(size_t) source] = p;
            routePending[(size_t) source] = true;
            return;
        }
        auto& r = paramRoutes[(size_t) source];
        r.id = id;
        r.base = base;
        r.lo = lo;
        r.hi = hi;
        r.log = log && lo > 0.0f && hi > lo;
        r.span = r.log ? 0.5f * std::log2(hi / lo) : (span > 0.0f ? span : hi - lo);
        anyParamRoute = false;
        for (auto& p : paramRoutes)
            anyParamRoute = anyParamRoute || p.id >= 0;
    }
    int getParamRoute(int source) const
    {
        return source >= 0 && source < NUM_MOD_SOURCES ? paramRoutes[(size_t) source].id : -1;
    }
    // The set value of a routed cell changed.
    void setParamRouteBase(int id, float base)
    {
        for (auto& r : paramRoutes)
            if (r.id == id)
                r.base = base;
        for (auto& r : pendingRoute)
            if (r.id == id)
                r.base = base;
    }

    // This voice's own value of a page cell: the cells PARAM_DESTS lists (the engine uses it for
    // them too, so the knob and an LFO go the same way). Envelope times act on the stage that is
    // running.
    void setParam(int id, float value)
    {
        using namespace UnifiedParam;
        switch (id)
        {
            case TUNE2:        osc2Semis = value; oscs[1].detune = osc2Semis + osc2Cents / 100.0f; break;
            case DETUNE2:      osc2Cents = value; oscs[1].detune = osc2Semis + osc2Cents / 100.0f; break;
            case TUNE3:        oscs[2].detune = value; break;
            case MIX_DRIVE:    setMixDrive(value); break;
            case NOISE_COLOR:  setNoiseColor(value); break;
            case SRC_A1:       oscs[0].a = value; break;
            case SRC_B1:       oscs[0].b = value; break;
            case SRC_A2:       oscs[1].a = value; break;
            case SRC_B2:       oscs[1].b = value; break;
            case SRC_A3:       oscs[2].a = value; break;
            case SRC_B3:       oscs[2].b = value; break;
            case SPREAD:       setWaveStackDetune(value); break;
            case GLIDE:        setGlideTime(value); break;
            case DRIFT:        analog.driftAmount = value * 20.0f; break;   // 0..1 = up to 20 cents of slow wander
            case DRIFT_RATE:   analog.driftRate = value; break;
            case FILTER_ENV:   setFilterEnvAmount(value); break;
            case FILTER_DRIVE: setFilterDrive(0, value); break;
            case MORPH:        setFilterMorph(value); break;
            case RESONANCE_B:  setFilterResonanceB(value); break;
            case BALANCE:      setFilterBalance(value); break;
            case DRIVE_B:      setFilterDrive(1, value); break;
            case B_TRACK:      setFilterBTrack(value); break;
            case FENV_ATTACK:  filterEnv.setAttack(value); break;
            case FENV_DECAY:   filterEnv.setDecay(value); break;
            case FENV_SUSTAIN: filterEnv.setSustain(value); break;
            case FENV_RELEASE: filterEnv.setRelease(value); break;
            case KEYTRACK:     setFilterKeyTrack(value); break;
            case FILTER_VEL:   setFilterVelAmount(value); break;
            case POLY_MOD:     setPolyMod(value); break;
            case AMP_ATTACK:   ampEnv.setAttack(value); break;
            case AMP_DECAY:    ampEnv.setDecay(value); break;
            case AMP_SUSTAIN:  ampEnv.setSustain(value); break;
            case AMP_RELEASE:  ampEnv.setRelease(value); break;
            case VEL_SENS:     setVelocitySensitivity(value); break;
            default:
                if (id >= ModParam::ENV_BASE && id < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
                {
                    const int env = (id - ModParam::ENV_BASE) / ModParam::ENV_STRIDE;
                    const int cell = (id - ModParam::ENV_BASE) % ModParam::ENV_STRIDE;
                    if (cell == ModParam::ENV_DEPTH)
                        envChain.setDepth(value);
                    else if (env < envChain.getStageCount())
                    {
                        auto& e = envChain.getEnvelopeMut(env);
                        if (cell == ModParam::ENV_ATK)      e.setAttack(value);
                        else if (cell == ModParam::ENV_DEC) e.setDecay(value);
                        else if (cell == ModParam::ENV_SUS) e.setSustain(value);
                        else if (cell == ModParam::ENV_REL) e.setRelease(value);
                    }
                }
                break;
        }
    }
    LfoDest getLfoDest(int lfoIndex) const
    {
        return (lfoIndex >= 0 && lfoIndex < LFOBank::MAX_LFOS) ? lfoDest[(size_t) lfoIndex] : LfoDest::Off;
    }

    // How much the mod wheel, the note's velocity and the voice's pressure add to an LFO's depth
    // (each -1..1), and the current wheel position / pressure of this voice (0..1).
    void setLfoWheelAmount(int lfoIndex, float a)    { if (validLfo(lfoIndex)) lfoControl[(size_t) lfoIndex].wheel = a; }
    void setLfoVelocityAmount(int lfoIndex, float a) { if (validLfo(lfoIndex)) lfoControl[(size_t) lfoIndex].velocity = a; }
    void setLfoPressureAmount(int lfoIndex, float a) { if (validLfo(lfoIndex)) lfoControl[(size_t) lfoIndex].pressure = a; }
    void setLfoPerformance(float wheel, float pressure) { lfoWheel = wheel; lfoPressure = pressure; }

    // ---- The mod slots (EngineModSection.h) ------------------------------------------------------
    // A slot's settings: its target is a destination of the LFOs' list (ModSlotTarget::Fixed, index
    // = LfoDest), a page cell (Keyed, index = the cell's id) or another slot's amount.
    void applyModSlot(int slot, const VoiceModSlots::Settings& s) { mods.apply(slot, s, active); }
    // Once a block: what the slots' PRES, STRP and IN sources read (each 0..1).
    void setSlotSources(float pressure, float strip, float input) { mods.setBlockSources(pressure, strip, input); }

    // -1..1 from LFOs routed to the pan (as of the last processed sample); the engine places the voice.
    float getPanMod() const { return lfoPan; }

    // Called once per block while the voice is silent, so free-running LFOs stay in time.
    void advanceIdleLfos(int numSamples) { lfoBank.advanceIdle(numSamples); }

    // Per-voice pressure (polyphonic aftertouch) contributions, already scaled by the router.
    void setPressureMods(float volume, float cutoff, float vibrato, float timbre, float wavePos, float fm)
    {
        atVolume = volume;
        atCutoff = cutoff;
        atVibrato = vibrato;
        atTimbre = timbre;
        atWavePos = wavePos;
        atFm = fm;
    }

    float getCurrentPitch() const { return currentPitch; }

    static constexpr float FILTER_ENV_OCTAVES = 7.0f;

    // Sets both filters; setFilterResonanceB then gives filter B its own.
    // (Under a sounding note the resonances and the balance are moved to, not jumped to: glide.)
    void setFilterResonance(float q) { filterBaseResonance = q; filterResonanceB = q; if (! active) filterSlot.setResonance(q); }
    void setFilterResonanceA(float q) { filterBaseResonance = q; if (! active) filterSlot.setResonance(0, q); }
    void setFilterResonanceB(float q) { filterResonanceB = q; if (! active) filterSlot.setResonance(1, q); }
    void setFilterModel(int slot, FilterModel model)
    {
        filterSlot.setModel(slot, model);
    }
    void setFilterMode(int slot, int mode) { filterSlot.setMode(slot, mode); }
    void setFilterRouting(FilterRouting r) { filterSlot.setRouting(r); }
    void setFilterBalance(float b) { filterBalance = b; if (! active) filterSlot.setBalance(b); }
    void setFilterDrive(float d) { filterSlot.setDrive(d); }
    void setFilterDrive(int slot, float d) { filterSlot.setDrive(slot, d); }
    void setFilterFM(float amount) { filterFm = std::clamp(amount, 0.0f, 1.0f); }
    void setFilterBTrack(float amount) { filterBTrack = std::clamp(amount, 0.0f, 1.0f); }
    void setPolyMod(float amount) { polyMod = std::clamp(amount, -1.0f, 1.0f); }

private:
    // One sample of the mod slots: what they read from this voice (the LFOs' shapes without their
    // depth, the two envelopes as of the last sample), then what each gives.
    void runSlots(bool firstSample)
    {
        for (int i = 0; i < LFOBank::MAX_LFOS; ++i)
            if (mods.uses(ModSrc::ofLfo(i)))
            {
                const auto& lfo = lfoBank.getLFO(i);
                mods.setLfo(i, i < lfoBank.getActiveCount() ? lfo.getShapeOutput() : 0.0f, LFO::isUnipolar(lfo.getShape()));
            }
        if (mods.usesEnvelopes())
            mods.setEnvelopes(ampEnv.getCurrentLevel(), lastFilterEnv);
        mods.run(firstSample);
    }

    // One sample of the sources routed to page cells: every routed cell takes its set value moved
    // by the sum of its sources (in the cell's own scale, see PARAM_DESTS).
    void applyParamRoutes(float envValue)
    {
        const bool slotCells = mods.keyed();
        std::array<float, NUM_MOD_SOURCES> amount {};
        for (int s = 0; s < LFOBank::MAX_LFOS; ++s)
            amount[(size_t) s] = lfoHeard[(size_t) s];
        amount[ENV_SOURCE] = envChain.getStageCount() > 0 ? envValue * envChain.getDepth() : 0.0f;

        for (int s = 0; s < NUM_MOD_SOURCES; ++s)
        {
            const auto& r = paramRoutes[(size_t) s];
            if (r.id < 0)
                continue;
            bool first = true;
            float total = amount[(size_t) s];
            for (int t = 0; t < NUM_MOD_SOURCES; ++t)
                if (t != s && paramRoutes[(size_t) t].id == r.id)
                {
                    if (t < s) first = false;
                    else       total += amount[(size_t) t];
                }
            if (! first)
                continue;           // summed into the first source that has this cell
            if (slotCells)
                total += mods.keyedAmount(r.id);
            const float moved = r.log ? r.base * std::exp2(total * r.span) : r.base + total * r.span;
            setParam(r.id, std::clamp(moved, r.lo, r.hi));
        }
        if (! slotCells)
            return;
        // The cells only slots move (one that an LFO or the envelopes also move was done above).
        for (int a = 0; a < mods.count(); ++a)
        {
            const int slot = mods.slotAt(a);
            const ModSlotTarget& t = mods.target(slot);
            if (t.kind != ModSlotTarget::Keyed)
                continue;
            bool done = false;
            for (auto& r : paramRoutes)
                done = done || r.id == t.index;
            for (int b = 0; b < a && ! done; ++b)
                done = mods.keyOf(mods.slotAt(b)) == t.index;
            if (done)
                continue;
            const float total = mods.keyedAmount(t.index);
            const float moved = t.log ? t.base * std::exp2(total * t.span) : t.base + total * t.span;
            setParam(t.index, std::clamp(moved, t.lo, t.hi));
        }
    }

    // The shapes that jump: at the turn of the cycle, or (the two decays) when it starts again.
    static bool hasEdge(LFO::Shape s)
    {
        return s != LFO::Shape::Sine && s != LFO::Shape::Triangle && s != LFO::Shape::SmoothRandom;
    }

    // The LFOs that were letting go of a destination take their new one now.
    void finishLfoClosings()
    {
        for (int i = 0; i < LFOBank::MAX_LFOS; ++i)
            if (lfoClosing[(size_t) i])
                finishLfoClosing(i);
    }

    void finishLfoClosing(int i)
    {
        const auto s = (size_t) i;
        lfoClosing[s] = false;
        lfoHeard[s] = 0.0f;
        lfoCatchUp[s] = true;
        lfoDest[s] = pendingDest[s];
        if (routePending[s])
        {
            routePending[s] = false;
            // The cell it was moving goes back to its set value, unless another source still moves it.
            const ParamRoute old = paramRoutes[s];
            paramRoutes[s] = pendingRoute[s];
            bool held = false;
            for (auto& r : paramRoutes)
                held = held || (old.id >= 0 && r.id == old.id);
            if (old.id >= 0 && ! held)
                setParam(old.id, old.base);
            anyParamRoute = false;
            for (auto& r : paramRoutes)
                anyParamRoute = anyParamRoute || r.id >= 0;
        }
    }

    // Once per sample, after the LFOs have run. Nothing that jumps under a sounding note is heard
    // as a jump (a click), except what the player asked to be one:
    //   - an LFO's output reaches every destination but pitch on a lead (lfoHeard): it follows
    //     exactly while it moves smoothly, and at no more than the whole swing in 4 ms where it
    //     jumps - at the edge of a square, ramp, random or decay shape, where its depth or shape
    //     was changed (a knob, the strip, pressure, another LFO), and while it lets go of a
    //     destination. The sound still changes abruptly to the ear;
    //   - what is set from outside between two blocks (a step lock, a knob, pad pressure): the
    //     oscillator and noise levels, cross-mod, pulse width, the cutoff, the filter envelope's
    //     amount, velocity's share and the LEVL cell are moved to over a few milliseconds.
    // Envelopes are not touched: an attack is as fast as it was set. The first sample of a note
    // takes everything as it stands.
    void glide()
    {
        const int lfoCount = lfoBank.getActiveCount();
        for (int i = 0; i < LFOBank::MAX_LFOS; ++i)
        {
            const auto s = (size_t) i;
            if (lfoClosing[s] && lfoHeard[s] == 0.0f)
                finishLfoClosing(i);
            auto& lfo = lfoBank.getLFO(i);
            const bool on = i < lfoCount;
            const float depth = on ? lfo.getEffectiveDepth() : 0.0f;
            const LFO::Shape shape = lfo.getShape();
            if (depth != lfoDepthSeen[s] || shape != lfoShapeSeen[s])
            {
                lfoDepthSeen[s] = depth;
                lfoShapeSeen[s] = shape;
                lfoCatchUp[s] = true;
            }
            const float raw = on && ! lfoClosing[s] ? lfoBank.getOutput(i) : 0.0f;
            const float d = raw - lfoHeard[s];
            if (struck || std::abs(d) <= lfoStep || ! (hasEdge(shape) || lfoCatchUp[s] || lfoClosing[s]))
            {
                lfoHeard[s] = raw;
                lfoCatchUp[s] = false;
            }
            else
                lfoHeard[s] += d > 0.0f ? lfoStep : -lfoStep;
        }

        const float velGain = 1.0f - velSensitivity + velSensitivity * currentVelocity;
        if (struck)
        {
            struck = false;
            for (int i = 0; i < MAX_OSCS; ++i)
                oscLevelNow[(size_t) i] = oscs[i].enabled ? oscs[i].level : 0.0f;
            noiseLevelNow = noiseLevel;
            mixDriveNow = mixDrive;
            resonanceGlides = resonanceNow != filterBaseResonance || resonanceBNow != filterResonanceB;
            resonanceNow = filterBaseResonance;
            resonanceBNow = filterResonanceB;
            if (balanceNow != filterBalance)
            {
                balanceNow = filterBalance;
                filterSlot.setBalance(balanceNow);
            }
            crossModNow = crossMod;
            pulseWidthNow = oscs[0].pulseWidth;
            filterCutoffNow = filterBaseCutoff;
            filterEnvAmountNow = filterEnvAmount;
            velGainNow = velGain;
            levelNow = level;
            atVolumeNow = atVolume; atCutoffNow = atCutoff; atTimbreNow = atTimbre; atWavePosNow = atWavePos; atFmNow = atFm;
        }
        else
        {
            for (int i = 0; i < MAX_OSCS; ++i)
            {
                const float target = oscs[i].enabled ? oscs[i].level : 0.0f;
                if (oscLevelNow[(size_t) i] != target)
                    oscLevelNow[(size_t) i] = dsp::towards(oscLevelNow[(size_t) i], target, levelStep);
            }
            if (noiseLevelNow != noiseLevel)            noiseLevelNow = dsp::towards(noiseLevelNow, noiseLevel, levelStep);
            if (mixDriveNow != mixDrive)                mixDriveNow = dsp::towards(mixDriveNow, mixDrive, levelStep);
            resonanceGlides = resonanceNow != filterBaseResonance || resonanceBNow != filterResonanceB;
            if (resonanceGlides)
            {
                resonanceNow = dsp::towards(resonanceNow, filterBaseResonance, levelStep);
                resonanceBNow = dsp::towards(resonanceBNow, filterResonanceB, levelStep);
            }
            if (balanceNow != filterBalance)
            {
                balanceNow = dsp::towards(balanceNow, filterBalance, levelStep);
                filterSlot.setBalance(balanceNow);
            }
            if (crossModNow != crossMod)                crossModNow = dsp::towards(crossModNow, crossMod, levelStep);
            if (pulseWidthNow != oscs[0].pulseWidth)    pulseWidthNow = dsp::towards(pulseWidthNow, oscs[0].pulseWidth, levelStep);
            if (filterCutoffNow != filterBaseCutoff)
                filterCutoffNow = filterCutoffNow < filterBaseCutoff ? std::min(filterBaseCutoff, filterCutoffNow * octaveRatio)
                                                                     : std::max(filterBaseCutoff, filterCutoffNow / octaveRatio);
            if (filterEnvAmountNow != filterEnvAmount)  filterEnvAmountNow = dsp::towards(filterEnvAmountNow, filterEnvAmount, levelStep);
            if (velGainNow != velGain)                  velGainNow = dsp::towards(velGainNow, velGain, levelStep);
            if (levelNow != level)                      levelNow = dsp::towards(levelNow, level, levelStep);
            if (atVolumeNow != atVolume)                atVolumeNow = dsp::towards(atVolumeNow, atVolume, levelStep);
            if (atCutoffNow != atCutoff)                atCutoffNow = dsp::towards(atCutoffNow, atCutoff, 2.0f * levelStep);
            if (atTimbreNow != atTimbre)                atTimbreNow = dsp::towards(atTimbreNow, atTimbre, levelStep);
            if (atWavePosNow != atWavePos)              atWavePosNow = dsp::towards(atWavePosNow, atWavePos, levelStep);
            if (atFmNow != atFm)                        atFmNow = dsp::towards(atFmNow, atFm, levelStep);
        }
        pwLag = pulseWidthNow - oscs[0].pulseWidth;
    }

    static Envelope::Params scaledEnvelope(Envelope::Params p, float scale)
    {
        p.attack *= scale;
        p.decay *= scale;
        p.release *= scale;
        return p;
    }

    static double pitchToHz(float pitch)
    {
        return 440.0 * std::exp2(((double) pitch - 69.0) / 12.0);
    }

    float generateOsc(const OscState& osc, float waveOffset) const
    {
        float p = (float) osc.phase;
        double dt = baseFrequency * std::exp2((osc.detune) / 12.0) / sampleRate;
        if (dt <= 0.0) dt = 1e-9;

        // PW shapes every waveform, more the further it is from 0.5, and the two directions are
        // two different sounds that meet at the plain wave (at exactly 0.5 every wave is computed
        // as it always was). Below 0.5 thins or sharpens the wave; above 0.5 thickens it:
        //             below 0.5                                above 0.5
        //   SQR, PLS  the pulse narrows (classic PWM)          a square an octave up is mixed in
        //   SIN, TRI  wavefolding: driven up to six times      saturation: pushed into a limiter
        //             too hard, the excess folded back         towards a square
        //   SAW       a second saw is added, PW of a cycle     a saw half a cycle behind is taken
        //             behind (doubled, phasing saw)            away: the saw hollows into a square
        // All the pulses and saws are band-limited (polyBLEP).
        const float pwSet = std::clamp(osc.pulseWidth, 0.05f, 0.95f);
        const bool shaped = pwSet != 0.5f;
        const bool upper = pwSet > 0.5f;
        const float shapeAmount = std::abs(pwSet - 0.5f) / 0.45f;           // 0..1
        auto fold = [shapeAmount](float x)
        {
            float t = (x * (1.0f + 5.0f * shapeAmount) + 1.0f) * 0.25f;
            t -= std::floor(t);
            return 1.0f - std::abs(4.0f * t - 2.0f);                        // reflects at +-1
        };
        // tanh(g x) / tanh(g): the identity as g goes to 0, nearly a square at g = 8.
        auto saturate = [shapeAmount](float x)
        {
            const float g = 0.02f + 8.0f * shapeAmount;
            return std::tanh(g * x) / std::tanh(g);
        };
        auto bend = [&](float x) { return !shaped ? x : (upper ? saturate(x) : fold(x)); };
        // A band-limited pulse that is high for `width` of the cycle starting at `phase`.
        auto pulseAt = [](double phase, double step, float width)
        {
            float v = (float) phase < width ? 1.0f : -1.0f;
            v += dsp::polyBLEP(phase, step);
            double shifted = phase + (double) width;
            if (shifted >= 1.0) shifted -= 1.0;
            v -= dsp::polyBLEP(shifted, step);
            return v;
        };
        auto shapedPulse = [&](float plainWidth)
        {
            if (!shaped)
                return pulseAt(osc.phase, dt, plainWidth);
            if (!upper)
                return pulseAt(osc.phase, dt, std::clamp(pwSet + waveOffset, 0.05f, 0.95f));
            double twice = osc.phase * 2.0;
            if (twice >= 1.0) twice -= 1.0;
            const float octave = pulseAt(twice, dt * 2.0, 0.5f);
            return (1.0f - 0.5f * shapeAmount) * pulseAt(osc.phase, dt, 0.5f + waveOffset) + 0.5f * shapeAmount * octave;
        };

        switch (osc.waveform)
        {
            case OscWaveform::Sine:
                return bend(std::sin(p * dsp::TWO_PI_F));
            case OscWaveform::Saw:
            {
                float saw = 2.0f * p - 1.0f;
                saw -= dsp::polyBLEP(osc.phase, dt);
                if (shaped)
                {
                    double phase2 = osc.phase + (upper ? 0.5 : (double) pwSet);
                    if (phase2 >= 1.0) phase2 -= 1.0;
                    float saw2 = 2.0f * (float) phase2 - 1.0f;
                    saw2 -= dsp::polyBLEP(phase2, dt);
                    saw = upper ? saw - shapeAmount * saw2
                                : (saw + shapeAmount * saw2) / (1.0f + 0.5f * shapeAmount);
                }
                return saw + waveOffset * saw * saw;
            }
            case OscWaveform::Square:
                return shapedPulse(0.5f + waveOffset);
            case OscWaveform::Triangle:
            {
                float tri = p < 0.5f ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
                tri = bend(tri);
                return tri + waveOffset * tri * tri;
            }
            case OscWaveform::Pulse:
                // PLS is the same wave as SQR now that PW is the pulse width of both.
                return shapedPulse(std::clamp(0.5f + waveOffset, 0.05f, 0.95f));
            case OscWaveform::Noise:
                return dsp::xorshiftNoise(noiseSeed);

            case OscWaveform::Wavetable:
            {
                // The wavetable engine's read-out on the wavetable engine's tables: a function
                // of this slot's phase, so the stack, detune and cross-mod act on it as on any kind.
                const auto& tables = sharedTables();
                const Wavetable& table = *tables[(size_t) std::clamp((int) std::lround(osc.b), 0, (int) tables.size() - 1)];
                return WavetableRead::sample(table, WavetableRead::mipLevel(table, dt),
                                             std::clamp(osc.a + osc.aMod, 0.0f, 1.0f), osc.phase);
            }
            case OscWaveform::FM:
            {
                // A sine whose phase is pushed by a second sine at a whole multiple of its pitch
                // (whole, so the pair repeats exactly once per cycle whatever the phase does).
                const float ratio = (float) std::clamp((int) std::lround(osc.b), 1, 12);
                const float index = std::clamp(osc.a + osc.aMod, 0.0f, 1.0f) * 8.0f;        // radians
                return std::sin(p * dsp::TWO_PI_F + index * std::sin(p * ratio * dsp::TWO_PI_F));
            }
            case OscWaveform::Formant:
            {
                // A burst of a higher sine once per cycle under a window that is zero at both ends:
                // a resonance at `formant` times the pitch that stays put as its width changes.
                const float formant = 1.0f + std::clamp(osc.a + osc.aMod, 0.0f, 1.0f) * 15.0f;
                const float window = 0.5f - 0.5f * std::cos(p * dsp::TWO_PI_F);
                const float sharp = 1.0f + (1.0f - std::clamp(osc.b, 0.0f, 1.0f)) * 7.0f;     // narrow window = wide formant
                return std::sin(p * formant * dsp::TWO_PI_F) * std::pow(window, sharp) * 1.4f;
            }
            case OscWaveform::Harmonic:
            {
                // Up to twelve harmonics added together, never above 0.45 of the sample rate.
                const int count = std::clamp((int) (0.45 / dt), 1, 12);
                const float tilt = std::clamp(osc.a + osc.aMod, 0.0f, 1.0f) * 3.0f;
                const float even = 1.0f - std::clamp(osc.b, 0.0f, 1.0f);
                const float theta = p * dsp::TWO_PI_F;
                const float twoCos = 2.0f * std::cos(theta);
                float prev = 0.0f, cur = std::sin(theta);          // sin(0), sin(theta)
                float sum = 0.0f, weights = 0.0f;
                for (int h = 1; h <= count; ++h)
                {
                    const float w = ((h & 1) != 0 ? 1.0f : even) / (1.0f + tilt * (float) (h - 1));
                    sum += w * cur;
                    weights += w;
                    const float next = twoCos * cur - prev;        // sin((h + 1) theta)
                    prev = cur;
                    cur = next;
                }
                return sum / (0.6f * weights + 0.4f);
            }
        }
        return 0.0f;
    }

    double sampleRate = 44100.0;
    double baseFrequency = 440.0;
    int currentNote = 60;
    float currentVelocity = 1.0f;
    bool active = false;

    std::array<OscState, MAX_OSCS> oscs;
    Envelope ampEnv;
    Envelope filterEnv;
    FilterSlot filterSlot;
    EnvelopeChain envChain;
    LFOBank lfoBank;

    VoiceModSlots mods;
    float filterBaseCutoff = 20000.0f;
    float filterBaseResonance = 0.0f;
    float filterEnvAmount = 0.0f;
    float filterKeyTrack = 0.0f;
    float filterVelAmount = 0.0f;
    float filterBOffsetSemis = 0.0f;
    float filterBRatio = 1.0f;
    float filterResonanceB = 0.0f;
    float filterBTrack = 1.0f;
    float filterFm = 0.0f;
    float polyMod = 0.0f;
    float lastFilterEnv = 0.0f;
    bool resonanceModulated = false;

    // Per-note analog spread
    float noteSlopSemis = 0.0f;
    float noteFilterOct = 0.0f;
    Envelope::Params baseAmpEnv, baseFilterEnv;
    bool haveBaseEnvelopes = false;
    bool envelopesScaled = false;
    float noiseState = 0.0f;
    float noiseCoeff = 1.0f;
    float noiseMakeup = 1.0f;

    // Mixer extras
    float noiseLevel = 0.0f;
    float mixDrive = 0.0f;
    float crossMod = 0.0f;
    float lastOsc2 = 0.0f;
    double xmodSum = 0.0;                       // leaky running sum of oscillator 2, in carrier cycles
    static constexpr double XMOD_INDEX = 6.283185307179586 * 0.5;   // full XMOD, sine at unison: index pi

    // The track's input at audio rate (XIN, see process).
    int xinMode = XinOff, xinModeNow = XinOff;
    float xinDepth = 0.5f, xinDepthNow = 0.5f;
    double xinSum = 0.0;                        // PM: leaky running sum of the input, in carrier cycles
    bool xinArmed = false;                      // SYNC: the input has been below -threshold
    float xinPrev = 0.0f;                       // SYNC: the input one sample ago

    // Pitch
    float tuneSemitones = 0.0f;
    float bendSemitones = 0.0f;
    float unisonDetune = 0.0f;
    float targetPitch = 60.0f;
    float currentPitch = 60.0f;
    float glideTime = 0.0f;
    float glideCoeff = 1.0f;
    GlideMode glideMode = GlideMode::Always;
    bool hasPlayed = false;

    float velSensitivity = 1.0f;

    // LFO routing and per-sample LFO contributions that are not part of processModulation's outputs
    std::array<LfoDest, LFOBank::MAX_LFOS> lfoDest { LfoDest::Off, LfoDest::Off, LfoDest::Off, LfoDest::Off };
    LfoDest envDest = LfoDest::Off;
    struct ParamRoute { int id = -1; float base = 0.0f, lo = 0.0f, hi = 1.0f, span = 1.0f; bool log = false; };
    std::array<ParamRoute, NUM_MOD_SOURCES> paramRoutes {};
    bool anyParamRoute = false;
    float osc2Semis = 0.0f, osc2Cents = 0.0f;       // TUN2 and DET2 (oscillator 2's detune is their sum)
    struct LfoControl { float wheel = 0.0f, velocity = 0.0f, pressure = 0.0f; };
    std::array<LfoControl, LFOBank::MAX_LFOS> lfoControl {};
    float lfoWheel = 0.0f, lfoPressure = 0.0f, lfoNoteVelocity = 0.0f;
    static bool validLfo(int i) { return i >= 0 && i < LFOBank::MAX_LFOS; }
    float lfoCutoffOct = 0.0f;
    float lfoCutoffBOct = 0.0f;
    float lfoPwMod = 0.0f;
    float lfoXmodMod = 0.0f;
    float lfoFilterFmMod = 0.0f;
    float lfoOsc2Semis = 0.0f;
    float lfoNoiseGain = 1.0f;
    float lfoPan = 0.0f;
    dsp::GainSlew lfoAmpSlew, lfoPanSlew;
    std::array<float, MAX_OSCS> lfoOscGain { 1.0f, 1.0f, 1.0f };

    // What is heard (see glide): the LFOs on their leads, the set values on theirs.
    bool struck = true;
    float lfoStep = 0.01f, levelStep = 0.005f, octaveRatio = 1.03f;
    std::array<float, LFOBank::MAX_LFOS> lfoHeard {}, lfoDepthSeen {};
    std::array<LFO::Shape, LFOBank::MAX_LFOS> lfoShapeSeen {};
    std::array<bool, LFOBank::MAX_LFOS> lfoCatchUp {}, lfoClosing {}, routePending {};
    std::array<LfoDest, LFOBank::MAX_LFOS> pendingDest { LfoDest::Off, LfoDest::Off, LfoDest::Off, LfoDest::Off };
    std::array<ParamRoute, LFOBank::MAX_LFOS> pendingRoute {};
    std::array<float, MAX_OSCS> oscLevelNow {};
    float noiseLevelNow = 0.0f, crossModNow = 0.0f, pulseWidthNow = 0.5f, pwLag = 0.0f, mixDriveNow = 0.0f;
    float resonanceNow = 0.0f, resonanceBNow = 0.0f, filterBalance = 0.5f, balanceNow = 0.5f;
    bool resonanceGlides = false;
    float filterCutoffNow = 20000.0f, filterEnvAmountNow = 0.0f, velGainNow = 1.0f;
    float level = 0.8f, levelNow = 0.8f;
    float atVolumeNow = 0.0f, atCutoffNow = 0.0f, atTimbreNow = 0.0f, atWavePosNow = 0.0f, atFmNow = 0.0f;
    dsp::Declick declick;       // on the mixer's output, before the filter (whose state smears a jump)
    dsp::Declick gainDeclick;   // on the voice's output

    // Pressure contributions
    float atVolume = 0.0f, atCutoff = 0.0f, atVibrato = 0.0f, atTimbre = 0.0f, atWavePos = 0.0f, atFm = 0.0f;
    double vibratoPhase = 0.0;

    // Wave stacking state
    WaveStackConfig waveStack;
    float cachedStackGain = 1.0f;
    std::array<std::array<double, MAX_STACK_VOICES>, MAX_OSCS> stackPhases{};

    // Analog character state
    AnalogCharacter analog;
    double driftPhase = 0.0;
    float driftFiltered = 0.0f;
    float softStartSamples = 0.0f;
    std::array<float, MAX_OSCS> oscWaveshapeOffset = {0.0f, 0.0f, 0.0f};
    mutable uint32_t noiseSeed = 42;
};

class UnifiedVoiceEngine : public EngineBase
{
public:
    static constexpr int MAX_VOICES = 16;

    UnifiedVoiceEngine(int maxVoices = 8)
        : allocator(juce::jlimit(1, MAX_VOICES, maxVoices))
    {
        // The voice pool never changes size, so the audio thread never sees a reallocation; the
        // allocator decides how many of them are in use.
        voices.resize((size_t) MAX_VOICES);
        // The track's custom LFO shape (USR / USRS): every LFO reads the engine's eight levels.
        for (auto& v : voices)
            for (int l = 0; l < LFOBank::MAX_LFOS; ++l)
                v.getLFOBank().getLFO(l).setUserShape(modSlots.shape());
        UnifiedVoice::sharedTables();        // built here, on the message thread, not at the first note
        installAllocatorCallbacks();

        initialVoiceCount = juce::jlimit(1, MAX_VOICES, maxVoices);
        loadInitPatch();
    }

    EngineType getType() const override { return EngineType::UnifiedVoice; }
    juce::String getName() const override { return "Unified Voice"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr;
        for (auto& v : voices)
            v.setSampleRate(sr);
        // Glide coefficients depend on the sample rate.
        applyParameter(UnifiedParam::GLIDE, flatParams[UnifiedParam::GLIDE]);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        int numSamples = buffer.getNumSamples();

        modWheelNow = smoothWheel(modWheelNow, modWheelValue, numSamples, sampleRate);
        updatePressure();

        const float gain = HEADROOM * unisonGain;       // LEVL is applied by each voice (UnifiedVoice::setLevel)

        // XIN: the track's input at audio rate, while the cell is on and the track has an input.
        const bool xinLive = xinMode != UnifiedVoice::XinOff && audioInput.connected();
        const float* xinRight = audioInput.right != nullptr ? audioInput.right : audioInput.left;
        if (xinLive != xinWasLive)
        {
            xinWasLive = xinLive;
            if (xinMode != UnifiedVoice::XinOff)
                soundSwapped();                         // the input came or went under held notes
        }

        for (int i = 0; i < numSamples; ++i)
        {
            // An LFO routed to PAN moves its own voice between the channels (balance law: the
            // centre is unchanged, so a patch without that routing sounds exactly as before).
            float sampleL = 0.0f, sampleR = 0.0f;
            // The track's input for XIN: mono, held to +-1, silence after its last frame.
            float xin = 0.0f;
            if (xinLive && i < audioInput.frames)
            {
                xin = 0.5f * (audioInput.left[i] + xinRight[i]);
                if (! (std::abs(xin) <= 1.0f))
                    xin = std::isnan(xin) ? 0.0f : (xin > 0.0f ? 1.0f : -1.0f);
            }
            for (auto& voice : voices)
            {
                if (!voice.isActive())
                    continue;
                const float s = voice.process(xin, xinLive);
                const float pan = voice.getPanMod();
                sampleL += pan > 0.0f ? s * (1.0f - pan) : s;
                sampleR += pan < 0.0f ? s * (1.0f + pan) : s;
            }

            if (right)
            {
                left[i] += sampleL * gain;
                right[i] += sampleR * gain;
            }
            else
            {
                left[i] += 0.5f * (sampleL + sampleR) * gain;
            }
        }

        // Voices that are not sounding still keep their free-running LFOs in time, so FREE and
        // HOLD behave like one LFO that never stops.
        for (auto& voice : voices)
            if (!voice.isActive())
                voice.advanceIdleLfos(numSamples);
    }

    void releaseResources() override
    {
        for (auto& v : voices)
            v.reset();
    }

    void noteOn(int note, float velocity, int /*voiceIndex*/) override
    {
        if (isMono())
        {
            monoNoteOn(note, velocity);
            return;
        }
        allocator.noteOn(note, velocity);
    }

    void noteOff(int note, int /*voiceIndex*/) override
    {
        if (isMono())
        {
            monoNoteOff(note);
            return;
        }
        allocator.noteOff(note);
    }

    void allNotesOff() override
    {
        soundCut();
        monoStackCount = 0;
        monoNote = -1;
        monoPressure = 0.0f;
        allocator.allNotesOff();
        for (auto& v : voices)
            v.reset();
    }

    void aftertouch(int note, float pressure) override
    {
        if (isMono())
        {
            if (note == monoNote)
                monoPressure = pressure;
            return;
        }
        allocator.setAftertouch(note, pressure);
    }

    // -1..1 from the pitch strip; the range is the BEND parameter.
    void pitchBend(float amount) override
    {
        bendAmount = juce::jlimit(-1.0f, 1.0f, amount);
        float semis = bendAmount * flatParams[UnifiedParam::BEND_RANGE];
        for (auto& v : voices)
            v.setPitchBend(semis);
    }

    // 0..1 from the mod strip. It goes to every LFO whose WHL amount is set (MOD2 page); while no
    // LFO in use listens to it, it is vibrato (up to half a semitone), so the strip always does
    // something.
    void modWheel(float value) override
    {
        modWheelValue = juce::jlimit(0.0f, 1.0f, value);
    }

    // ---- Per-stage envelope curves (StageCurveParam) --------------------------------------------
    // Envelope slots: 0 amp, 1 filter envelope, 2-5 the extra envelopes 1-4 (MENV). On the amp and
    // filter envelopes they add to the CURV cell.
    static int stageCurveParamFor(ParamId cellId)
    {
        using namespace UnifiedParam;
        switch (cellId)
        {
            case AMP_ATTACK:   return StageCurveParam::id(0, StageCurveParam::Attack);
            case AMP_DECAY:    return StageCurveParam::id(0, StageCurveParam::Decay);
            case AMP_RELEASE:  return StageCurveParam::id(0, StageCurveParam::Release);
            case FENV_ATTACK:  return StageCurveParam::id(1, StageCurveParam::Attack);
            case FENV_DECAY:   return StageCurveParam::id(1, StageCurveParam::Decay);
            case FENV_RELEASE: return StageCurveParam::id(1, StageCurveParam::Release);
            default: break;
        }
        if (cellId >= ModParam::ENV_BASE && cellId < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
        {
            const int env = (cellId - ModParam::ENV_BASE) / ModParam::ENV_STRIDE;
            const int cell = (cellId - ModParam::ENV_BASE) % ModParam::ENV_STRIDE;
            if (cell == ModParam::ENV_ATK) return StageCurveParam::id(2 + env, StageCurveParam::Attack);
            if (cell == ModParam::ENV_DEC) return StageCurveParam::id(2 + env, StageCurveParam::Decay);
            if (cell == ModParam::ENV_REL) return StageCurveParam::id(2 + env, StageCurveParam::Release);
        }
        return -1;
    }
    int getStageCurveParam(ParamId cellId) const override { return stageCurveParamFor(cellId); }

private:
    StageCurves stageCurves;

    // The curves of extra envelope `env` (0-3), when it is running.
    void applyExtraEnvCurves(int env)
    {
        for (auto& v : voices)
        {
            auto& chain = v.getEnvelopeChain();
            if (env >= chain.getStageCount())
                continue;
            auto p = chain.getEnvelopeMut(env).getParams();
            stageCurves.applyTo(p, 2 + env);
            chain.getEnvelopeMut(env).setParams(p);
        }
    }

    void setStageCurve(ParamId id, float value)
    {
        stageCurves.set(id, value);
        const int slot = StageCurveParam::envelopeOf(id);
        if (slot < 2)                               applyEnvelope(slot == 1);
        else if (slot < 2 + ModParam::MAX_ENVS)     applyExtraEnvCurves(slot - 2);
    }

public:
    void setParameter(ParamId id, float value) override
    {
        if (StageCurveParam::owns(id))
        {
            setStageCurve(id, value);
            return;
        }
        if (EngineModSlots::owns(id))
        {
            setModSlotParameter(id, value);
            return;
        }
        if (!EngineParamRange::contains(id) || id >= UnifiedParam::MAX_ID)
            return;
        if (id == UnifiedParam::MODE_A || id == UnifiedParam::MODE_B)
        {
            auto model = FilterSlot::modelFromIndex(
                (int) flatParams[id == UnifiedParam::MODE_A ? UnifiedParam::MODEL_A : UnifiedParam::MODEL_B]);
            value = (float) juce::jlimit(0, FilterSlot::modeCount(model) - 1, (int) std::lround(value));
        }
        else if (const ParamSpec* spec = findSpec(id))
        {
            value = spec->isDiscrete() ? std::round(spec->clamp(value)) : spec->clamp(value);
            // A selector (waveform, filter type, routing, stack ...) turned under a held note.
            if (spec->kind == ParamKind::Choice && flatParams[(size_t) id] != value)
                soundSwapped();
        }
        if ((id == UnifiedParam::MODE_A || id == UnifiedParam::MODE_B) && flatParams[(size_t) id] != value)
            soundSwapped();
        flatParams[(size_t) id] = value;
        applyParameter(id, value);

        // Page cells an LFO or the extra envelopes are routed to: the voices move them around
        // their set value, so they are told when it changes (and when an oscillator's kind, and
        // with it the range of its SRC cells, does).
        if (id == UnifiedParam::ARCH || id == UnifiedParam::WAVE1 || id == UnifiedParam::WAVE2 || id == UnifiedParam::WAVE3)
            refreshRoutes();
        else if (isRouted(id))
            for (auto& v : voices)
                v.setParamRouteBase(id, flatParams[(size_t) id]);
        // The same for the mod slots (a slot keeps the set value and range of the cell it moves).
        if (anySlotOn && (id == UnifiedParam::ARCH || id == UnifiedParam::WAVE1 || id == UnifiedParam::WAVE2
                          || id == UnifiedParam::WAVE3 || slotMoves(id)))
            pushModSlots();
    }

    float getParameter(ParamId id) const override
    {
        if (id >= 0 && id < UnifiedParam::MAX_ID)
            return flatParams[(size_t) id];
        if (EngineModSlots::owns(id))
            return modSlots.get(id);
        return stageCurves.get(id);     // 0 for anything else
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (const ParamSpec* spec = findSpec(id))
            return spec->label;
        if (EngineModSlots::owns(id))
            return EngineModSlots::name(id);
        return "Param " + juce::String(id);
    }

    int getParameterCount() const override { return UnifiedParam::MAX_ID; }

    bool supportsModulation() const override { return true; }
    int getMasterParamId() const override { return UnifiedParam::ARCH; }

    // ---- Pages -------------------------------------------------------------------------------

    ParamPages getParamPages() const override
    {
        ParamPages pages = basePages();
        // The third oscillator is a sub in the mono architecture and a full oscillator otherwise.
        const bool subStyle = (currentPresetId == ArchitecturePresetId::MonoVA && archIndex() == 1);
        pages[1].name = subStyle ? "SUB" : "OSC3";
        pages[1].params[2].label = subStyle ? "SUB" : "LVL3";

        // SRC: two cells for each oscillator that plays something other than an analog wave.
        for (auto& page : pages)
            if (page.name == "SRC")
                for (int osc = 0; osc < 3; ++osc)
                {
                    const int kind = (int) std::lround(flatParams[osc == 0 ? UnifiedParam::WAVE1 : (osc == 1 ? UnifiedParam::WAVE2 : UnifiedParam::WAVE3)]);
                    page.params[(size_t) (osc * 2)] = srcSpec(osc, kind, false);
                    page.params[(size_t) (osc * 2 + 1)] = srcSpec(osc, kind, true);
                }

        // The MODE cells offer the modes of the filter type currently chosen.
        for (auto& page : pages)
            for (auto& p : page.params)
                if (p.id == UnifiedParam::MODE_A || p.id == UnifiedParam::MODE_B)
                {
                    auto model = FilterSlot::modelFromIndex(
                        (int) flatParams[p.id == UnifiedParam::MODE_A ? UnifiedParam::MODEL_A : UnifiedParam::MODEL_B]);
                    p.choices = FilterSlot::modeNames(model);
                    p.numChoices = FilterSlot::modeCount(model);
                    p.maxVal = (float) (p.numChoices - 1);
                }
        // The mod slots and the custom LFO shape: SLT1 ..., LSHP (the screen puts the LFO and
        // envelope pages MOD, MOD2 and MENV after them).
        const auto& names = slotDestNames();
        modSlots.insertPages(pages, nullptr, names.text.data(), names.count, ModSrc::COUNT);
        return pages;
    }

    // ---- Mod slots (EngineModSection.h) ----------------------------------------------------------
    // A slot's DEST: the LFOs' list at the LFOs' indices (it already has every LFO's rate and
    // depth), then AMT1 .. AM16, the amount of another slot.
    static constexpr int NUM_SLOT_DESTS = (int) UnifiedVoice::LfoDest::Count + EngineModSlots::MAX;
    static const EngineModSlots::DestNames& slotDestNames()
    {
        return EngineModSlots::destNames(UnifiedVoice::lfoDestLabels(), (int) UnifiedVoice::LfoDest::Count, nullptr, 0);
    }
    const EngineModSlots& slots() const { return modSlots; }
    bool slotsOn() const { return anySlotOn; }

    // The SRC cell of oscillator `osc` (0..2) for WAV choice `kind`; invalid for the analog waves.
    // Labels carry the oscillator's number: 1POS 1TBL, 2IDX 2RAT, 3FRM 3WID, 1TLT 1ODD.
    static ParamSpec srcSpec(int osc, int kind, bool second)
    {
        static const char* const labels[4][2][3] = {
            { { "1POS", "2POS", "3POS" }, { "1TBL", "2TBL", "3TBL" } },
            { { "1IDX", "2IDX", "3IDX" }, { "1RAT", "2RAT", "3RAT" } },
            { { "1FRM", "2FRM", "3FRM" }, { "1WID", "2WID", "3WID" } },
            { { "1TLT", "2TLT", "3TLT" }, { "1ODD", "2ODD", "3ODD" } } };
        // nTBL: the wavetable engine's built-in set, by its names (the first nine, then the newer ones).
        const int numTables = WavetableFactory::builtInCount();
        const int k = kind - (int) UnifiedVoice::OscWaveform::Wavetable;
        if (k < 0 || k > 3 || osc < 0 || osc > 2)
            return {};
        const int id = second ? UnifiedParam::srcB(osc) : UnifiedParam::srcA(osc);
        const char* label = labels[k][second ? 1 : 0][osc];
        const float d = srcDefault(kind, second);
        if (second && k == 0) return { label, id, 0, (float) (numTables - 1), d, ParamKind::Choice, "", WavetableFactory::builtInLabels(), numTables };
        if (second && k == 1) return { label, id, 1, 12, d, ParamKind::Int };
        return { label, id, 0, 1, d, ParamKind::Linear };
    }
    static float srcDefault(int kind, bool second)
    {
        switch ((UnifiedVoice::OscWaveform) kind)
        {
            case UnifiedVoice::OscWaveform::Wavetable: return second ? 0.0f : 0.0f;      // SAW table, first frame
            case UnifiedVoice::OscWaveform::FM:        return second ? 2.0f : 0.3f;
            case UnifiedVoice::OscWaveform::Formant:   return second ? 0.5f : 0.3f;
            case UnifiedVoice::OscWaveform::Harmonic:  return second ? 0.0f : 0.4f;
            default:                                   return 0.0f;
        }
    }

    static const ParamPages& basePages()
    {
        using namespace UnifiedParam;
        static const char* const waves[] = { "SIN", "SAW", "SQR", "TRI", "PLS", "NOIS", "WT", "FM", "FORM", "HARM" };
        static const char* const routes[] = { "SNGL", "SER", "PAR" };
        static const char* const modes[] = { "MONO", "DUO", "POLY", "UNI" };
        static const char* const archs[] = { "INIT", "MONO", "HYBR", "POLY" };
        static const char* const stacks[] = { "OFF", "2", "4", "8" };
        static const char* const glideModes[] = { "ALL", "LEGA" };
        static const char* const xins[] = { "OFF", "PM", "RING", "SYNC" };

        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;

        static const ParamPages pages = {
            { "OSC", "SRC", {{
                { "WAV1", WAVE1, 0, 9, 1, C, "", waves, 10 },
                { "WAV2", WAVE2, 0, 9, 1, C, "", waves, 10 },
                { "MIX",  MIX, 0, 1, 0.5f, L },
                { "PW",   PULSE_WIDTH, 0.05f, 0.95f, 0.5f, L },
                { "TUN2", TUNE2, -24, 24, 0, I, "st" },
                { "DET2", DETUNE2, -50, 50, 8, L, "ct" },
                { "XMOD", CROSS_MOD, 0, 1, 0, L },
                { "DRIV", MIX_DRIVE, 0, 1, 0, L } }} },
            { "OSC3", "SRC", {{
                { "WAV3", WAVE3, 0, 9, 0, C, "", waves, 10 },
                { "TUN3", TUNE3, -24, 24, -12, I, "st" },
                { "LVL3", SUB, 0, 1, 0, L },
                { "NOIS", NOISE, 0, 1, 0, L },
                { "NCOL", NOISE_COLOR, 0, 1, 0, L },
                { "TUNE", TUNE, -24, 24, 0, I, "st" },
                { "FINE", FINE, -100, 100, 0, L, "ct" },
                { "BEND", BEND_RANGE, 0, 24, 2, I, "st" } }} },
            // Filled in getParamPages(): two cells per oscillator that is a wavetable, FM, formant
            // or harmonic oscillator (empty for the analog waves).
            { "SRC", "SRC", {{ {}, {}, {}, {}, {}, {}, {}, {} }} },
            { "VOICE", "SRC", {{
                { "ARCH", ARCH, 0, 3, 0, C, "", archs, 4 },
                { "MODE", VOICE_MODE, 0, 3, 2, C, "", modes, 4 },
                { "VOX",  VOICE_COUNT, 1, MAX_VOICES, 8, I },
                { "LEGA", LEGATO, 0, 1, 0, C, "", ParamChoices::offOn, 2 },
                { "STCK", STACK, 0, 3, 0, C, "", stacks, 4 },
                { "SPRD", SPREAD, 0, 1, 0.1f, L },
                { "GLID", GLIDE, 0, 2, 0, L, "s" },
                { "GLMD", GLIDE_MODE, 0, 1, 0, C, "", glideModes, 2 } }} },
            // Everything that makes the voices imperfect, in one place: slow drift (amount, rate),
            // and what is drawn afresh for every note (pitch, start phase, waveshape, pitch
            // overshoot, cutoff, envelope times).
            { "ANALG", "SRC", {{
                { "DRFT", DRIFT, 0, 1, 0.15f, L },
                { "DRAT", DRIFT_RATE, 0.05f, 5, 0.3f, G, "Hz" },
                { "SLOP", SLOP, 0, 50, 2, L, "ct" },
                { "PHAS", RANDOM_PHASE, 0, 1, 1, C, "", ParamChoices::offOn, 2 },
                { "WSHP", WAVE_VAR, 0, 1, 0.1f, L },
                { "SOFT", SOFT_START, 0, 50, 8, L, "ct" },
                { "FVAR", FILTER_VAR, 0, 1, 0, L },
                { "EVAR", ENV_VAR, 0, 1, 0, L } }} },
            // The track's input (TRK FX page IN) at audio rate, in every voice: what it does and
            // how much (UnifiedVoice::XinMode). Nothing without an input.
            { "XIN", "SRC", {{
                { "XIN",  XIN, 0, UnifiedVoice::NUM_XIN_MODES - 1, 0, C, "", xins, UnifiedVoice::NUM_XIN_MODES },
                { "XDPT", XIN_DEPTH, 0, 1, 0.5f, L },
                {}, {}, {}, {}, {}, {} }} },
            { "FILTR", "FLTR", {{
                { "CUT",  CUTOFF, 20, 20000, 8000, G, "Hz" },
                { "RESO", RESONANCE, 0, 1, 0.2f, L },
                { "ENV",  FILTER_ENV, -1, 1, 0, L },
                { "DRIV", FILTER_DRIVE, 0, 1, 0, L },
                { "TYPE", MODEL_A, 0, FilterSlot::NUM_MODELS - 1, 0, C, "", FilterSlot::modelNames(), FilterSlot::NUM_MODELS },
                { "MODE", MODE_A, 0, 5, 0, C, "", FilterSlot::modeNames(FilterModel::SVF), 4 },
                { "MORP", MORPH, 0, 1, 0, L },
                { "FFM",  FILTER_FM, 0, 1, 0, L } }} },
            { "FLT B", "FLTR", {{
                { "TYPB", MODEL_B, 0, FilterSlot::NUM_MODELS - 1, FilterSlot::NUM_MODELS - 1, C, "", FilterSlot::modelNames(), FilterSlot::NUM_MODELS },
                { "MODB", MODE_B, 0, 5, 0, C, "", FilterSlot::modeNames(FilterModel::Bypass), 1 },
                { "OFSB", FILTER_B_OFFSET, -48, 48, 0, I, "st" },
                { "RESB", RESONANCE_B, 0, 1, 0.2f, L },
                { "ROUT", ROUTING, 0, 2, 0, C, "", routes, 3 },
                { "BAL",  BALANCE, 0, 1, 0.5f, L },
                { "DRVB", DRIVE_B, 0, 1, 0, L },
                { "TRKB", B_TRACK, 0, 1, 1, L } }} },
            { "F ENV", "FLTR", {{
                { "ATK",  FENV_ATTACK, 0.001f, 10, 0.01f, G, "s" },
                { "DEC",  FENV_DECAY, 0.001f, 10, 0.5f, G, "s" },
                { "SUS",  FENV_SUSTAIN, 0, 1, 0, L },
                { "REL",  FENV_RELEASE, 0.001f, 10, 0.3f, G, "s" },
                { "CURV", FENV_CURVE, -1, 1, 0, L },
                { "KEY",  KEYTRACK, -1, 1, 0, L },
                { "VEL",  FILTER_VEL, 0, 1, 0, L },
                { "PMOD", POLY_MOD, -1, 1, 0, L } }} },
            { "AMP", "AMP", {{
                { "ATK",  AMP_ATTACK, 0.001f, 10, 0.01f, G, "s" },
                { "DEC",  AMP_DECAY, 0.001f, 10, 0.3f, G, "s" },
                { "SUS",  AMP_SUSTAIN, 0, 1, 0.7f, L },
                { "REL",  AMP_RELEASE, 0.001f, 10, 0.3f, G, "s" },
                { "CURV", AMP_CURVE, -1, 1, 0, L },
                { "VEL",  VEL_SENS, 0, 1, 0.7f, L },
                { "LEVL", LEVEL, 0, 1, 0.8f, L },
                {} }} },
        };
        return pages;
    }

    VoiceAllocator& getAllocator() { return allocator; }
    UnifiedVoice& getVoice(int index) { return voices[(size_t) juce::jlimit(0, MAX_VOICES - 1, index)]; }

    // ---- Architecture presets ----------------------------------------------------------------

    void applyArchitecturePreset(ArchitecturePresetId presetId)
    {
        const auto& preset = getArchitecturePreset(presetId);
        currentPresetId = presetId;
        flatParams[UnifiedParam::ARCH] = (float) ((int) presetId + 1);

        using namespace UnifiedParam;

        set(VOICE_MODE, (float) (int) preset.voiceMode);
        set(VOICE_COUNT, (float) preset.voiceCount);
        set(LEGATO, preset.legato ? 1.0f : 0.0f);
        set(GLIDE, preset.glideTime);
        set(GLIDE_MODE, preset.legato ? 1.0f : 0.0f);
        set(STACK, preset.unisonEnabled ? (preset.unisonCount >= 4 ? 2.0f : 1.0f) : 0.0f);
        set(SPREAD, preset.unisonEnabled ? preset.unisonSpread : 0.1f);
        set(DRIFT, preset.analogEnabled ? preset.driftAmount : 0.0f);

        set(WAVE1, (float) (int) preset.oscWaveforms[0]);
        set(WAVE2, (float) (int) preset.oscWaveforms[1]);
        set(WAVE3, (float) (int) preset.oscWaveforms[2]);
        const float l1 = preset.oscEnabled[0] ? preset.oscLevels[0] : 0.0f;
        const float l2 = preset.oscEnabled[1] ? preset.oscLevels[1] : 0.0f;
        set(MIX, (l1 + l2) > 0.0f ? l2 / (l1 + l2) : 0.0f);
        set(SUB, preset.oscEnabled[2] ? preset.oscLevels[2] : 0.0f);
        const float semis2 = std::round(preset.oscDetune[1]);
        set(TUNE2, semis2);
        set(DETUNE2, (preset.oscDetune[1] - semis2) * 100.0f);
        set(TUNE3, std::round(preset.oscDetune[2]));
        set(NOISE, preset.noiseEnabled ? preset.noiseLevel : 0.0f);
        set(PULSE_WIDTH, 0.5f);
        set(CROSS_MOD, 0.0f);
        set(MIX_DRIVE, 0.0f);
        set(TUNE, 0.0f);
        set(FINE, 0.0f);

        set(MODEL_A, (float) FilterSlot::indexFromModel(preset.filterModel));
        set(MODEL_B, (float) FilterSlot::indexFromModel(preset.filter2Model));
        set(MODE_A, 0.0f);
        set(MODE_B, 0.0f);
        set(BALANCE, 0.5f);
        set(FILTER_FM, 0.0f);
        set(DRIVE_B, 0.0f);
        set(POLY_MOD, 0.0f);
        set(B_TRACK, 1.0f);
        set(ROUTING, (float) (int) preset.filterRouting);
        set(CUTOFF, preset.filterCutoff);
        set(RESONANCE, juce::jmin(preset.filterResonance, 1.0f));
        set(RESONANCE_B, juce::jmin(preset.filterResonance, 1.0f));
        set(FILTER_DRIVE, preset.filterDrive);
        // Presets give the envelope amount in Hz above the cutoff; the parameter is in octaves.
        float octaves = std::log2(juce::jmax(20.0f, preset.filterCutoff + preset.filterEnvAmount)
                                  / juce::jmax(20.0f, preset.filterCutoff));
        set(FILTER_ENV, octaves / UnifiedVoice::FILTER_ENV_OCTAVES);
        set(KEYTRACK, preset.voiceMode == VoiceMode::Poly ? 0.5f : 0.3f);
        set(FILTER_VEL, 0.0f);
        set(MORPH, 0.0f);
        set(FILTER_B_OFFSET, 0.0f);

        set(AMP_ATTACK, preset.ampEnv.attack);
        set(AMP_DECAY, preset.ampEnv.decay);
        set(AMP_SUSTAIN, preset.ampEnv.sustain);
        set(AMP_RELEASE, preset.ampEnv.release);
        set(FENV_ATTACK, preset.filterEnv.attack);
        set(FENV_DECAY, preset.filterEnv.decay);
        set(FENV_SUSTAIN, preset.filterEnv.sustain);
        set(FENV_RELEASE, preset.filterEnv.release);

        // The architecture's mod strip route (LFO cells on MOD / MOD2).
        for (auto& [id, value] : preset.wheelRoute)
            set(id, value);
    }

    ArchitecturePresetId getCurrentPresetId() const { return currentPresetId; }

    const ArchitecturePreset& getCurrentPreset() const
    {
        return getArchitecturePreset(currentPresetId);
    }

private:
    // One full-level voice peaks about where a drum hit does (measured on the device: 0.7 left the
    // synth clearly quieter than the kit).
    static constexpr float HEADROOM = 1.0f;
    static constexpr int MONO_STACK_SIZE = 16;

    void set(ParamId id, float value) { setParameter(id, value); }

    int archIndex() const { return (int) flatParams[UnifiedParam::ARCH]; }

    static const ParamSpec* findSpec(int id)
    {
        for (auto& page : basePages())
            if (auto* p = page.find(id))
                return p;
        return nullptr;
    }

    void installAllocatorCallbacks()
    {
        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            if (voiceIdx >= 0 && voiceIdx < (int) voices.size())
            {
                voices[(size_t) voiceIdx].setNote(v.note, v.velocity);
                voices[(size_t) voiceIdx].noteOn();
            }
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            if (voiceIdx >= 0 && voiceIdx < (int) voices.size())
                voices[(size_t) voiceIdx].noteOff();
        });
    }

    // Every page parameter at its default, with the voice count the engine was built with.
    void loadInitPatch()
    {
        for (auto& page : basePages())
            for (auto& p : page.params)
                if (p.isValid() && p.id != UnifiedParam::ARCH)
                    setParameter(p.id, p.id == UnifiedParam::VOICE_COUNT ? (float) initialVoiceCount : p.defaultVal);

        flatParams[UnifiedParam::ARCH] = 0.0f;

        // Modulation defaults: one LFO, no extra envelopes, nothing routed.
        flatParams[ModParam::LFO_COUNT] = 1.0f;
        flatParams[ModParam::ENV_COUNT] = 0.0f;
        for (int l = 0; l < ModParam::MAX_LFOS; ++l)
        {
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_RATE), 2.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_DEPTH), 0.3f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_SHAPE), 0.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_TRIG), 0.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_SYNC), 0.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_DEST), 0.0f);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_DIV), (float) LFO::DEFAULT_DIVISION);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_PHASE), 0.0f);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_DELAY), 0.0f);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_FADE), 0.0f);
            for (int off : { ModParam::LFO_WHEEL, ModParam::LFO_VEL, ModParam::LFO_KTRK, ModParam::LFO_PRES })
                setParameter(ModParam::lfoExt2ParamId(l, off), 0.0f);
        }
        for (int e = 0; e < ModParam::MAX_ENVS; ++e)
        {
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_ATK)] = 0.01f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_DEC)] = 0.1f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_SUS)] = 0.7f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_REL)] = 0.3f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_DEPTH)] = 1.0f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_TARGET)] = 3.0f;
        }
        applyModParameter(ModParam::ENV_COUNT, 0.0f);
        applyModParameter(ModParam::LFO_COUNT, 1.0f);

        // No mod slot set, two shown, the custom shape a rising staircase.
        modSlots = EngineModSlots();
        pushModSlots();
    }

    bool isMono() const { return allocator.getMode() == VoiceMode::Mono; }

    // ---- Mono: last-note priority with a held-note stack, optional legato -----------------------

    void monoNoteOn(int note, float velocity)
    {
        const bool wasHeld = monoStackCount > 0 && voices[0].isGateOpen();
        monoRemove(note);
        if (monoStackCount < MONO_STACK_SIZE)
            monoStack[(size_t) monoStackCount++] = note;

        monoVelocity = velocity;
        monoPressure = 0.0f;
        monoNote = note;

        if (legato && wasHeld)
        {
            voices[0].setNoteLegato(note);
        }
        else
        {
            voices[0].setNote(note, velocity);
            voices[0].noteOn();
        }
    }

    void monoNoteOff(int note)
    {
        monoRemove(note);
        if (note != monoNote)
            return;

        if (monoStackCount > 0)
        {
            // Fall back to the most recent key still held.
            monoNote = monoStack[(size_t) (monoStackCount - 1)];
            if (legato)
            {
                voices[0].setNoteLegato(monoNote);
            }
            else
            {
                voices[0].setNote(monoNote, monoVelocity);
                voices[0].noteOn();
            }
        }
        else
        {
            monoNote = -1;
            voices[0].noteOff();
        }
    }

    void monoRemove(int note)
    {
        int w = 0;
        for (int r = 0; r < monoStackCount; ++r)
            if (monoStack[(size_t) r] != note)
                monoStack[(size_t) w++] = monoStack[(size_t) r];
        monoStackCount = w;
    }

    // True while an LFO in use has a WHL amount: the strip then does what those say and nothing else.
    bool wheelIsRouted() const
    {
        const int lfos = juce::jlimit(1, ModParam::MAX_LFOS, (int) flatParams[ModParam::LFO_COUNT]);
        for (int l = 0; l < lfos; ++l)
            if (flatParams[(size_t) ModParam::lfoExt2ParamId(l, ModParam::LFO_WHEEL)] != 0.0f)
                return true;
        return false;
    }

    // Per-voice pressure through the shared router, plus the mod wheel: to the LFOs' WHL amounts,
    // or as vibrato on every voice while none is set.
    void updatePressure()
    {
        const int count = juce::jmin(allocator.getVoiceCount(), (int) voices.size());
        const float wheelVibrato = wheelIsRouted() ? 0.0f : modWheelNow * 0.5f;
        for (int i = 0; i < (int) voices.size(); ++i)
        {
            float pressure = 0.0f;
            if (isMono())
                pressure = (i == 0) ? monoPressure : 0.0f;
            else if (i < count)
                pressure = allocator.getAftertouch(i);

            AftertouchRouter::ModValues m;
            if (pressure > 0.0f)
                m = (aftertouchRouter != nullptr ? *aftertouchRouter : allocator.getAftertouchRouter())
                        .computeModulation(pressure);

            voices[(size_t) i].setPressureMods(m.volumeMod, m.cutoffMod, m.vibratoMod + wheelVibrato,
                                               m.timbreMod, m.wavePosMod, m.fmDepthMod);
            // The LFOs' own WHL / PRES amounts take the raw wheel and this voice's raw pressure.
            voices[(size_t) i].setLfoPerformance(modWheelNow, pressure);
            if (anySlotOn)      // ... and so do the mod slots' PRES and STRP, with the input's follower (IN)
                voices[(size_t) i].setSlotSources(pressure, modWheelNow, audioInput.envelope);
        }
    }

    void applyOscLevels()
    {
        const float mix = flatParams[UnifiedParam::MIX];
        const float sub = flatParams[UnifiedParam::SUB];
        const float xmod = flatParams[UnifiedParam::CROSS_MOD];
        for (auto& v : voices)
        {
            v.getOsc(0).level = 1.0f - mix;
            v.getOsc(0).enabled = mix < 0.999f;
            v.getOsc(1).level = mix;
            // Still needed as the cross-mod / filter-FM source when it is out of the mix.
            v.getOsc(1).enabled = mix > 0.001f || xmod > 0.0f || flatParams[UnifiedParam::FILTER_FM] > 0.0f
                                  || lfoNeedsOsc2();
            v.getOsc(2).level = sub;
            v.getOsc(2).enabled = sub > 0.001f;
        }
    }

    // An LFO routed to the cross-mod or filter-FM amount needs oscillator 2 running.
    bool lfoNeedsOsc2() const
    {
        const int count = juce::jlimit(1, ModParam::MAX_LFOS, (int) flatParams[ModParam::LFO_COUNT]);
        for (int l = 0; l < count; ++l)
            if (UnifiedVoice::lfoDestNeedsOsc2(voices[0].getLfoDest(l)))
                return true;
        for (auto& s : slotSettings)
            if (s.on() && s.target.kind == ModSlotTarget::Fixed
                && UnifiedVoice::lfoDestNeedsOsc2((UnifiedVoice::LfoDest) s.target.index))
                return true;
        return UnifiedVoice::lfoDestNeedsOsc2(voices[0].getEnvDest());
    }

    // ---- The mod slots ---------------------------------------------------------------------------

    // Slot `slot` as the voices should have it: nothing when it is beyond the slots in use.
    VoiceModSlots::Settings resolveSlot(int slot) const
    {
        VoiceModSlots::Settings r;
        if (slot >= modSlots.acting())
            return r;
        const auto& m = modSlots.slot(slot);
        r.src = m.src;
        r.via = m.via;
        r.amount = m.amount;
        const int count = (int) UnifiedVoice::LfoDest::Count;
        if (m.dest > 0 && m.dest < count)
        {
            const int id = UnifiedVoice::paramOfDest(m.dest);
            if (id >= 0)
            {
                bool log = false;
                destRange(id, r.target.lo, r.target.hi, log);
                r.target.kind = ModSlotTarget::Keyed;
                r.target.index = id;
                r.target.log = log && r.target.lo > 0.0f && r.target.hi > r.target.lo;
                const float span = tableRouteSpan(id);
                r.target.span = r.target.log ? 0.5f * std::log2(r.target.hi / r.target.lo)
                                             : (span > 0.0f ? span : r.target.hi - r.target.lo);
                r.target.base = flatParams[(size_t) id];
            }
            else
            {
                r.target.kind = ModSlotTarget::Fixed;
                r.target.index = m.dest;
            }
        }
        else if (m.dest >= count && m.dest - count != slot && m.dest - count < modSlots.acting())
        {
            r.target.kind = ModSlotTarget::SlotAmount;
            r.target.index = m.dest - count;
        }
        for (int other = 0; other < modSlots.acting(); ++other)
            if (other != slot && modSlots.slot(other).src != ModSrc::Off && modSlots.slot(other).dest == count + slot)
                r.modulated = true;
        return r;
    }

    static int slotCell(const VoiceModSlots::Settings& r)
    {
        return r.on() && r.target.kind == ModSlotTarget::Keyed ? r.target.index : -1;
    }

    // True while a slot in use moves page cell `id`.
    bool slotMoves(int id) const
    {
        for (auto& s : slotSettings)
            if (slotCell(s) == id)
                return true;
        return false;
    }

    // Every slot whose settings are no longer what the voices have goes to them (one cell can
    // change two slots: its own and the one whose amount that slot moves); a page cell no slot
    // moves any more goes back to its set value.
    void pushModSlots()
    {
        bool on = false, changed = false;
        std::array<int, EngineModSlots::MAX> freed;
        int numFreed = 0;
        for (int slot = 0; slot < EngineModSlots::MAX; ++slot)
        {
            const VoiceModSlots::Settings now = resolveSlot(slot);
            auto& held = slotSettings[(size_t) slot];
            on = on || now.on();
            if (now.sameAs(held))
                continue;
            const int before = slotCell(held);
            held = now;
            changed = true;
            for (auto& v : voices)
                v.applyModSlot(slot, now);
            if (before >= 0 && before != slotCell(now))
                freed[(size_t) numFreed++] = before;
        }
        anySlotOn = on;
        for (int i = 0; i < numFreed; ++i)
            if (! slotMoves(freed[(size_t) i]) && ! isRouted(freed[(size_t) i]))
                applyParameter(freed[(size_t) i], flatParams[(size_t) freed[(size_t) i]]);
        if (changed)
            applyOscLevels();   // oscillator 2 may now be needed (or no longer)
    }

    void setModSlotParameter(ParamId id, float value)
    {
        const int change = modSlots.set(id, value, NUM_SLOT_DESTS, ModSrc::COUNT);
        if (change == EngineModSlots::SlotChanged)
            pushModSlots();
    }

    // ---- Page cells as LFO / envelope destinations (UnifiedVoice::PARAM_DESTS) ------------------

    bool isRouted(int id) const
    {
        for (int r : routeId)
            if (r == id && id >= 0)
                return true;
        return false;
    }

    // A slot's table cell: full depth moves it as far as it did when the list was nine tables long
    // (UnifiedVoice::TABLE_ROUTE_SPAN). 0 for every other cell: its whole range.
    float tableRouteSpan(int id) const
    {
        for (int osc = 0; osc < 3; ++osc)
            if (id == UnifiedParam::srcB(osc))
            {
                const int kind = (int) std::lround(flatParams[osc == 0 ? UnifiedParam::WAVE1 : (osc == 1 ? UnifiedParam::WAVE2 : UnifiedParam::WAVE3)]);
                return kind == (int) UnifiedVoice::OscWaveform::Wavetable ? UnifiedVoice::TABLE_ROUTE_SPAN : 0.0f;
            }
        return 0.0f;
    }

    // The range and scale of a cell an LFO can be sent to, from its ParamSpec.
    void destRange(int id, float& lo, float& hi, bool& log) const
    {
        lo = 0.0f; hi = 1.0f; log = false;
        ParamSpec src;
        const ParamSpec* spec = findSpec(id);
        for (int osc = 0; osc < 3 && spec == nullptr; ++osc)
            if (id == UnifiedParam::srcA(osc) || id == UnifiedParam::srcB(osc))
            {
                const int kind = (int) std::lround(flatParams[osc == 0 ? UnifiedParam::WAVE1 : (osc == 1 ? UnifiedParam::WAVE2 : UnifiedParam::WAVE3)]);
                src = srcSpec(osc, kind, id == UnifiedParam::srcB(osc));
                if (! src.isValid())
                    return;             // an analog wave: the cells do nothing
                spec = &src;
            }
        if (spec != nullptr)
        {
            lo = spec->minVal;
            hi = spec->maxVal;
            log = spec->kind == ParamKind::Log;
            return;
        }
        // The extra envelopes' cells are built by the screen: times 1 ms .. 10 s, the rest 0..1.
        if (id >= ModParam::ENV_BASE && id < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
        {
            const int cell = (id - ModParam::ENV_BASE) % ModParam::ENV_STRIDE;
            if (cell == ModParam::ENV_ATK || cell == ModParam::ENV_DEC || cell == ModParam::ENV_REL)
            {
                lo = 0.001f; hi = 10.0f; log = true;
            }
        }
    }

    // Source 0-3 = the LFOs (their DEST), 4 = the extra envelopes: tells every voice which page
    // cell the source moves, and puts a cell it has let go of back to its set value.
    void updateRoute(int source)
    {
        const int dest = source < ModParam::MAX_LFOS
            ? (int) std::lround(flatParams[(size_t) ModParam::lfoParamId(source, ModParam::LFO_DEST)])
            : envDestIndex;
        const int id = UnifiedVoice::paramOfDest(dest);
        const int before = routeId[(size_t) source];
        routeId[(size_t) source] = id;
        if (id >= 0)
        {
            float lo, hi; bool log;
            destRange(id, lo, hi, log);
            for (auto& v : voices)
                v.setParamRoute(source, id, flatParams[(size_t) id], lo, hi, log, tableRouteSpan(id));
        }
        else
        {
            for (auto& v : voices)
                v.setParamRoute(source, -1);
        }
        if (before >= 0 && before != id && ! isRouted(before))
            applyParameter(before, flatParams[(size_t) before]);
    }

    void refreshRoutes()
    {
        for (int s = 0; s < UnifiedVoice::NUM_MOD_SOURCES; ++s)
            if (routeId[(size_t) s] >= 0 || s < ModParam::MAX_LFOS)
                updateRoute(s);
    }

    void applyUnison()
    {
        const bool unison = allocator.getMode() == VoiceMode::Unison;
        const int n = allocator.getVoiceCount();
        const float spread = flatParams[UnifiedParam::SPREAD];
        unisonGain = (unison && n > 1) ? 1.0f / std::sqrt((float) n) : 1.0f;
        for (int i = 0; i < (int) voices.size(); ++i)
        {
            float detune = 0.0f;
            if (unison && n > 1 && i < n)
                detune = (((float) i / (float) (n - 1)) * 2.0f - 1.0f) * spread * 0.5f;
            voices[(size_t) i].setUnisonDetune(detune);
        }
    }

    void applyEnvelope(bool filter)
    {
        using namespace UnifiedParam;
        Envelope::Params p;
        p.attack = flatParams[filter ? FENV_ATTACK : AMP_ATTACK];
        p.decay = flatParams[filter ? FENV_DECAY : AMP_DECAY];
        p.sustain = flatParams[filter ? FENV_SUSTAIN : AMP_SUSTAIN];
        p.release = flatParams[filter ? FENV_RELEASE : AMP_RELEASE];
        stageCurves.applyTo(p, filter ? 1 : 0, flatParams[filter ? FENV_CURVE : AMP_CURVE]);
        for (auto& v : voices)
        {
            if (filter) v.setFilterEnvParams(p);
            else        v.setAmpEnvParams(p);
        }
    }

    void applyParameter(ParamId id, float value)
    {
        using namespace UnifiedParam;
        const int iv = (int) std::lround(value);

        switch (id)
        {
            case WAVE1: case WAVE2: case WAVE3:
            {
                int osc = (id == WAVE1) ? 0 : (id == WAVE2 ? 1 : 2);
                const int kind = juce::jlimit(0, UnifiedVoice::NUM_WAVEFORMS - 1, iv);
                auto w = static_cast<UnifiedVoice::OscWaveform>(kind);
                // A different kind of oscillator starts from that kind's own two settings.
                if (voices[0].getOsc(osc).waveform != w && !UnifiedVoice::isAnalog(w))
                {
                    flatParams[(size_t) srcA(osc)] = srcDefault(kind, false);
                    flatParams[(size_t) srcB(osc)] = srcDefault(kind, true);
                }
                for (auto& v : voices)
                {
                    v.getOsc(osc).waveform = w;
                    v.getOsc(osc).a = flatParams[(size_t) srcA(osc)];
                    v.getOsc(osc).b = flatParams[(size_t) srcB(osc)];
                }
                break;
            }
            // The cells that are simply one value in every voice: UnifiedVoice::setParam (which is
            // also what an LFO routed to the cell calls, for its own voice).
            case SRC_A1: case SRC_A2: case SRC_A3: case SRC_B1: case SRC_B2: case SRC_B3:
            case TUNE3: case MIX_DRIVE: case NOISE_COLOR: case GLIDE: case DRIFT: case DRIFT_RATE:
            case FILTER_ENV: case FILTER_DRIVE: case MORPH: case RESONANCE_B: case BALANCE: case DRIVE_B:
            case B_TRACK: case KEYTRACK: case FILTER_VEL: case POLY_MOD: case VEL_SENS:
                for (auto& v : voices) v.setParam(id, value);
                break;
            case MIX: case SUB: case CROSS_MOD:
                applyOscLevels();
                if (id == CROSS_MOD)
                    for (auto& v : voices) v.setCrossMod(value);
                break;
            case TUNE: case FINE:
                for (auto& v : voices) v.setTune(flatParams[TUNE] + flatParams[FINE] / 100.0f);
                break;
            case TUNE2: case DETUNE2:
                for (auto& v : voices)
                {
                    v.setParam(TUNE2, flatParams[TUNE2]);
                    v.setParam(DETUNE2, flatParams[DETUNE2]);
                }
                break;
            case XIN:
                xinMode = juce::jlimit(0, (int) UnifiedVoice::NUM_XIN_MODES - 1, iv);
                for (auto& v : voices) v.setXinMode(xinMode);
                break;
            case XIN_DEPTH:    for (auto& v : voices) v.setXinDepth(value); break;
            case NOISE:        for (auto& v : voices) v.setNoiseLevel(value); break;
            case PULSE_WIDTH:  for (auto& v : voices) v.setPulseWidth(value); break;
            case GLIDE_MODE:
                for (auto& v : voices)
                    v.setGlideMode(iv > 0 ? UnifiedVoice::GlideMode::Legato : UnifiedVoice::GlideMode::Always);
                break;
            case BEND_RANGE:   pitchBend(bendAmount); break;

            case CUTOFF:       for (auto& v : voices) v.setFilterBaseCutoff(value); break;
            case RESONANCE:    for (auto& v : voices) v.setFilterResonanceA(value); break;
            case FILTER_B_OFFSET: for (auto& v : voices) v.setFilterBOffset(value); break;
            case FILTER_FM:
                for (auto& v : voices) v.setFilterFM(value);
                applyOscLevels();      // oscillator 2 must run while it modulates the filter
                break;
            case MODEL_A: case MODEL_B:
            {
                const int slot = (id == MODEL_A) ? 0 : 1;
                const auto model = FilterSlot::modelFromIndex(iv);
                // Keep the stored mode inside the new type's range so the cell and the sound agree.
                const int modeId = (slot == 0) ? MODE_A : MODE_B;
                flatParams[(size_t) modeId] = (float) juce::jmin((int) flatParams[(size_t) modeId],
                                                                 FilterSlot::modeCount(model) - 1);
                for (auto& v : voices)
                {
                    v.setFilterModel(slot, model);
                    v.setFilterMode(slot, (int) flatParams[(size_t) modeId]);
                }
                break;
            }
            case MODE_A: case MODE_B:
                for (auto& v : voices) v.setFilterMode(id == MODE_A ? 0 : 1, iv);
                break;
            case ROUTING:
                for (auto& v : voices) v.setFilterRouting(static_cast<FilterRouting>(juce::jlimit(0, 2, iv)));
                break;

            case AMP_ATTACK: case AMP_DECAY: case AMP_SUSTAIN: case AMP_RELEASE: case AMP_CURVE:
                applyEnvelope(false);
                break;
            case FENV_ATTACK: case FENV_DECAY: case FENV_SUSTAIN: case FENV_RELEASE: case FENV_CURVE:
                applyEnvelope(true);
                break;
            case LEVEL:
                outputLevel = value;
                for (auto& v : voices)
                    v.setLevel(outputLevel);
                break;

            case ARCH:
                if (iv <= 0)
                {
                    loadInitPatch();
                }
                else
                {
                    applyArchitecturePreset(static_cast<ArchitecturePresetId>(
                        juce::jlimit(0, (int) ArchitecturePresetId::Count - 1, iv - 1)));
                }
                break;
            case VOICE_MODE:
            {
                auto m = static_cast<VoiceMode>(juce::jlimit(0, 3, iv));
                if (m != allocator.getMode())
                {
                    allNotesOff();
                    allocator.setMode(m);
                }
                applyUnison();
                break;
            }
            case VOICE_COUNT:
            {
                int n = juce::jlimit(1, MAX_VOICES, iv);
                if (n != allocator.getVoiceCount())
                {
                    allNotesOff();
                    allocator.setVoiceCount(n);
                }
                applyUnison();
                break;
            }
            case LEGATO: legato = iv > 0; break;
            case STACK:  for (auto& v : voices) v.setWaveStackMode(iv); break;
            case SPREAD:
                for (auto& v : voices) v.setWaveStackDetune(value);
                applyUnison();
                break;
            case SLOP:         for (auto& v : voices) v.getAnalogCharacter().slopCents = value; break;
            case RANDOM_PHASE: for (auto& v : voices) v.getAnalogCharacter().randomPhase = iv > 0; break;
            // A component tolerance over the first part of the cell (0.3 % at the default, as it
            // always was), growing into a difference that is heard from note to note (15 % at the top:
            // up to 0.03 the whole cell could not be heard at all).
            case WAVE_VAR:     for (auto& v : voices) v.getAnalogCharacter().waveshapeVar = value * (0.03f + 0.12f * value * value); break;
            case SOFT_START:   for (auto& v : voices) v.getAnalogCharacter().softStartCents = value; break;
            case FILTER_VAR:   for (auto& v : voices) v.getAnalogCharacter().filterVariance = value; break;
            case ENV_VAR:      for (auto& v : voices) v.getAnalogCharacter().envVariance = value; break;
            default:
                applyModParameter(id, value);
                break;
        }
    }

    void applyModParameter(ParamId id, float value)
    {
        // LFO params: 40 + lfoIdx*6 + offset
        if (id >= ModParam::LFO_BASE
            && id < ModParam::LFO_BASE + ModParam::MAX_LFOS * ModParam::LFO_STRIDE)
        {
            int offset = id - ModParam::LFO_BASE;
            int lfoIdx = offset / ModParam::LFO_STRIDE;
            int param = offset % ModParam::LFO_STRIDE;

            for (auto& v : voices)
            {
                auto& lfo = v.getLFOBank().getLFO(lfoIdx);
                switch (param)
                {
                    case ModParam::LFO_RATE:  lfo.setRate(value); break;
                    case ModParam::LFO_DEPTH: lfo.setDepth(value); break;
                    case ModParam::LFO_SHAPE:
                        lfo.setShape(static_cast<LFO::Shape>(juce::jlimit(0, (int) LFO::Shape::COUNT - 1, (int) std::lround(value))));
                        break;
                    case ModParam::LFO_TRIG:
                        lfo.setTriggerMode(static_cast<LFO::TriggerMode>(
                            juce::jlimit(0, (int) LFO::TriggerMode::COUNT - 1, (int) std::lround(value))));
                        break;
                    case ModParam::LFO_SYNC:
                        lfo.setTempo(tempoBpm);
                        lfo.setTempoSync(value > 0.5f);
                        break;
                    case ModParam::LFO_DEST:
                        v.setLfoDest(lfoIdx, static_cast<UnifiedVoice::LfoDest>(
                            juce::jlimit(0, (int) UnifiedVoice::LfoDest::Count - 1, (int) std::lround(value))));
                        break;
                    default: break;
                }
            }
            if (param == ModParam::LFO_DEST)
            {
                flatParams[(size_t) id] = (float) juce::jlimit(0, (int) UnifiedVoice::LfoDest::Count - 1, (int) std::lround(value));
                updateRoute(lfoIdx);    // a page cell as the destination (or no longer)
                applyOscLevels();       // oscillator 2 may now be needed (or no longer)
            }
            return;
        }

        // More LFO params: 130 + lfoIdx*4 + offset
        if (id >= ModParam::LFO_EXT_BASE && id < ModParam::LFO_EXT_END)
        {
            int offset = id - ModParam::LFO_EXT_BASE;
            int lfoIdx = offset / ModParam::LFO_EXT_STRIDE;
            int param = offset % ModParam::LFO_EXT_STRIDE;

            for (auto& v : voices)
            {
                auto& lfo = v.getLFOBank().getLFO(lfoIdx);
                switch (param)
                {
                    case ModParam::LFO_DIV:
                        lfo.setTempo(tempoBpm);
                        lfo.setSyncDivision((int) std::lround(value));
                        break;
                    case ModParam::LFO_PHASE: lfo.setStartPhase(juce::jlimit(0.0f, 1.0f, value)); break;
                    case ModParam::LFO_DELAY: lfo.setDelayTime(value); break;
                    case ModParam::LFO_FADE:  lfo.setFadeTime(value); break;
                    default: break;
                }
            }
            return;
        }

        // Extra ENV params: 64 + envIdx*6 + offset
        if (id >= ModParam::ENV_BASE
            && id < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
        {
            int offset = id - ModParam::ENV_BASE;
            int envIdx = offset / ModParam::ENV_STRIDE;
            int param = offset % ModParam::ENV_STRIDE;

            for (auto& v : voices)
            {
                auto& chain = v.getEnvelopeChain();
                // Depth and target belong to the whole chain; the stage settings need the stage.
                if (param == ModParam::ENV_DEPTH) { chain.setDepth(value); continue; }
                if (param == ModParam::ENV_TARGET)
                {
                    // 0-3: the envelopes' own targets. Beyond: one of the LFOs' destinations.
                    const int t = juce::jlimit(0, UnifiedVoice::NUM_ENV_DESTS - 1, (int) std::lround(value));
                    chain.setTarget(t < UnifiedVoice::NUM_ENV_TARGETS ? static_cast<EnvelopeChain::Target>(t)
                                                                      : EnvelopeChain::Target::Custom);
                    v.setEnvDest(UnifiedVoice::lfoDestOfEnvDest(t));
                    continue;
                }
                if (envIdx >= chain.getStageCount())
                    continue;
                auto& env = chain.getEnvelopeMut(envIdx);
                auto p = env.getParams();
                switch (param)
                {
                    case ModParam::ENV_ATK:    p.attack = value; env.setParams(p); break;
                    case ModParam::ENV_DEC:    p.decay = value; env.setParams(p); break;
                    case ModParam::ENV_SUS:    p.sustain = value; env.setParams(p); break;
                    case ModParam::ENV_REL:    p.release = value; env.setParams(p); break;
                    default: break;
                }
            }
            if (param == ModParam::ENV_TARGET)
            {
                envDestIndex = (int) voices[0].getEnvDest();
                updateRoute(UnifiedVoice::ENV_SOURCE);
                applyOscLevels();
            }
            return;
        }

        // LFO performance controls: 174 + lfoIdx*4 + offset
        if (id >= ModParam::LFO_EXT2_BASE && id < ModParam::LFO_EXT2_END)
        {
            int offset = id - ModParam::LFO_EXT2_BASE;
            int lfoIdx = offset / ModParam::LFO_EXT2_STRIDE;
            int param = offset % ModParam::LFO_EXT2_STRIDE;
            const float amount = juce::jlimit(-1.0f, 1.0f, value);

            for (auto& v : voices)
            {
                switch (param)
                {
                    case ModParam::LFO_WHEEL: v.setLfoWheelAmount(lfoIdx, amount); break;
                    case ModParam::LFO_VEL:   v.setLfoVelocityAmount(lfoIdx, amount); break;
                    case ModParam::LFO_KTRK:  v.getLFOBank().getLFO(lfoIdx).setKeyTrack(amount); break;
                    case ModParam::LFO_PRES:  v.setLfoPressureAmount(lfoIdx, amount); break;
                    default: break;
                }
            }
            return;
        }

        // LFO count change
        if (id == ModParam::LFO_COUNT)
        {
            int count = juce::jlimit(1, ModParam::MAX_LFOS, (int)value);
            for (auto& v : voices)
            {
                auto& bank = v.getLFOBank();
                while (bank.getActiveCount() < count)
                    bank.addLFO();
                while (bank.getActiveCount() > count)
                    bank.removeLFO();
            }
            applyOscLevels();
            return;
        }

        // ENV count change
        if (id == ModParam::ENV_COUNT)
        {
            int count = juce::jlimit(0, ModParam::MAX_ENVS, (int)value);
            for (auto& v : voices)
            {
                auto& chain = v.getEnvelopeChain();
                while (chain.getStageCount() < count)
                {
                    Envelope::Params p;
                    chain.addEnvelope(p);
                }
                while (chain.getStageCount() > count)
                    chain.removeLastEnvelope();
            }
            // New stages take the stored settings.
            for (int e = 0; e < count; ++e)
            {
                for (int p = ModParam::ENV_ATK; p <= ModParam::ENV_TARGET; ++p)
                    applyModParameter(ModParam::envParamId(e, p), flatParams[(size_t) ModParam::envParamId(e, p)]);
                applyExtraEnvCurves(e);
            }
            return;
        }
    }

public:
    void setTempo(float bpm) override
    {
        tempoBpm = bpm;
        for (auto& v : voices)
            v.getLFOBank().setTempo(bpm);
    }

private:
    double sampleRate = 44100.0;
    float outputLevel = 0.8f;
    float unisonGain = 1.0f;
    float tempoBpm = 120.0f;
    float bendAmount = 0.0f;
    float modWheelValue = 0.0f;
    float modWheelNow = 0.0f;       // smoothed per block
    bool legato = false;
    int initialVoiceCount = 8;
    int xinMode = UnifiedVoice::XinOff;     // the XIN cell
    bool xinWasLive = false;                // ... and whether it had an input to work on last block

    VoiceAllocator allocator;
    std::vector<UnifiedVoice> voices;
    std::array<float, UnifiedParam::MAX_ID> flatParams {};
    ArchitecturePresetId currentPresetId = ArchitecturePresetId::MonoVA;
    // The page cell each modulation source (LFO 1-4, the extra envelopes) is routed to, -1 = none.
    std::array<int, UnifiedVoice::NUM_MOD_SOURCES> routeId { { -1, -1, -1, -1, -1 } };
    int envDestIndex = 0;       // the extra envelopes' destination as a UnifiedVoice::LfoDest index
    // The mod slots: the stored cells, and each slot as the voices have it.
    EngineModSlots modSlots;
    std::array<VoiceModSlots::Settings, EngineModSlots::MAX> slotSettings {};
    bool anySlotOn = false;

    std::array<int, MONO_STACK_SIZE> monoStack {};
    int monoStackCount = 0;
    int monoNote = -1;
    float monoVelocity = 1.0f;
    float monoPressure = 0.0f;
};

} // namespace olduv
