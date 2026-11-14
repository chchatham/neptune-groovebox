#pragma once

#include "EngineBase.h"
#include "../dsp/DSPUtils.h"
#include "../dsp/Envelope.h"
#include "../dsp/EnvelopeChain.h"
#include "../dsp/LFO.h"
#include "../dsp/filters/FilterSlot.h"
#include "../dsp/ModMatrix.h"
#include "WavetableEngine.h"       // Wavetable, WavetableFactory: the tables an oscillator slot can play
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

// Parameter ids of the unified voice. Every id listed here is applied to the sound and appears on
// one of the pages returned by getParamPages().
namespace UnifiedParam
{
    // Oscillators / mixer
    static constexpr int WAVE1 = 0;      // waveform choice
    static constexpr int WAVE2 = 1;
    static constexpr int MIX = 2;        // 0 = oscillator 1 only, 1 = oscillator 2 only
    static constexpr int SUB = 3;        // oscillator 3 (sub) level
    static constexpr int TUNE = 4;       // whole voice, semitones
    static constexpr int FINE = 5;       // whole voice, cents
    static constexpr int NOISE = 6;
    static constexpr int GLIDE = 7;      // seconds
    static constexpr int DETUNE2 = 8;    // oscillator 2 fine detune, cents
    static constexpr int PULSE_WIDTH = 9;

    // Filter
    static constexpr int CUTOFF = 10;    // Hz
    static constexpr int RESONANCE = 11;
    static constexpr int FILTER_VEL = 12;   // velocity -> cutoff
    static constexpr int MORPH = 13;        // SVF: LP -> notch -> HP -> BP
    static constexpr int FILTER_ENV = 14;   // -1..1
    static constexpr int KEYTRACK = 15;
    static constexpr int FILTER_DRIVE = 16;
    static constexpr int MODEL_A = 17;      // filter type, as an index into FilterSlot's display order
    static constexpr int ROUTING = 18;
    static constexpr int MODEL_B = 19;

    // Amp
    static constexpr int AMP_ATTACK = 20;
    static constexpr int AMP_DECAY = 21;
    static constexpr int AMP_SUSTAIN = 22;
    static constexpr int AMP_RELEASE = 23;
    static constexpr int LEVEL = 24;        // engine output level (track volume lives in the mixer)
    static constexpr int VEL_SENS = 26;
    static constexpr int AMP_CURVE = 27;

    // 40..63 LFOs, 64..87 extra envelopes, 88 / 89 counts, 130..145 and 174..189 more LFO
    // settings: see ModParam (EngineBase.h)

    // Filter envelope and second filter
    static constexpr int FENV_ATTACK = 90;
    static constexpr int FENV_DECAY = 91;
    static constexpr int FENV_SUSTAIN = 92;
    static constexpr int FENV_RELEASE = 93;
    static constexpr int FENV_CURVE = 94;
    static constexpr int FILTER_B_OFFSET = 95;   // semitones relative to filter A

    // More oscillator controls
    static constexpr int TUNE2 = 96;        // semitones
    static constexpr int WAVE3 = 97;
    static constexpr int TUNE3 = 98;        // semitones
    static constexpr int CROSS_MOD = 99;
    static constexpr int MIX_DRIVE = 100;
    static constexpr int BEND_RANGE = 101;  // semitones

    // Filter modes and the second filter
    static constexpr int MODE_A = 102;       // mode within filter A's type
    static constexpr int MODE_B = 103;
    static constexpr int RESONANCE_B = 104;
    static constexpr int BALANCE = 105;      // parallel routing: A <-> B
    static constexpr int FILTER_FM = 106;    // oscillator 2 -> cutoff
    static constexpr int DRIVE_B = 107;
    static constexpr int POLY_MOD = 108;     // filter envelope -> oscillator 2 pitch
    static constexpr int B_TRACK = 109;      // how much filter B follows the cutoff modulation

    // Voice architecture
    static constexpr int ARCH = 110;        // 0 = init patch, 1.. = architecture presets
    static constexpr int VOICE_MODE = 111;
    static constexpr int VOICE_COUNT = 112;
    static constexpr int LEGATO = 113;
    static constexpr int STACK = 114;
    static constexpr int SPREAD = 115;
    static constexpr int DRIFT = 116;
    static constexpr int GLIDE_MODE = 117;

    // Analog character (the ANALG page; DRIFT above belongs to it too)
    static constexpr int DRIFT_RATE = 118;     // Hz
    static constexpr int SLOP = 119;           // per-note pitch error, cents
    static constexpr int RANDOM_PHASE = 120;   // oscillators start at random phases
    static constexpr int WAVE_VAR = 121;       // per-note waveshape tolerance
    static constexpr int SOFT_START = 122;     // pitch overshoot at note start, cents
    static constexpr int FILTER_VAR = 123;     // per-note cutoff error
    static constexpr int ENV_VAR = 124;        // per-note envelope-time error
    static constexpr int NOISE_COLOR = 125;

    // The two controls of an oscillator slot that plays a wavetable, FM, a formant or harmonics
    // (the SRC page). What they mean depends on the slot's WAV choice; see UnifiedVoice::OscWaveform.
    static constexpr int SRC_A1 = 126, SRC_B1 = 127, SRC_A2 = 128, SRC_B2 = 129, SRC_A3 = 146, SRC_B3 = 147;
    static constexpr int srcA(int osc) { return osc == 0 ? SRC_A1 : (osc == 1 ? SRC_A2 : SRC_A3); }
    static constexpr int srcB(int osc) { return osc == 0 ? SRC_B1 : (osc == 1 ? SRC_B2 : SRC_B3); }

    // One past the last id (EngineParamRange: 0..149 and 158..189). Unused: 148, 149,
    // 158..173; 150..157 are not engine ids at all.
    static constexpr int MAX_ID = 190;
}

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
    }

    void noteOn()
    {
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

    // Feed all modulation sources into the ModMatrix and read modulated values
    // for key voice parameters. Called once per sample inside process().
    void processModulation(float& pitchOffsetSemitones, float& cutoffMod,
                           float& resonanceMod, float& oscMixMod, float& ampMod)
    {
        // Feed LFO outputs into ModMatrix sources
        int lfoCount = lfoBank.getActiveCount();
        for (int i = 0; i < lfoCount && i < 5; ++i)
            modMatrix.setSourceValue(static_cast<ModSource>(static_cast<int>(ModSource::LFO1) + i),
                                     lfoBank.getOutput(i));

        // Feed envelope chain output
        float envOutput = envChain.getLastOutput();
        modMatrix.setSourceValue(ModSource::Env1, envOutput);

        // Set performance sources
        modMatrix.setSourceValue(ModSource::Velocity, currentVelocity);
        modMatrix.setSourceValue(ModSource::KeyTrack, (currentNote - 60) / 60.0f);

        // Read modulated values: base is 0 so we get pure modulation offset
        // ParamId 3 = Osc Pitch (semitone offset)
        pitchOffsetSemitones = modMatrix.getModulatedValue(3, 0.0f);
        // ParamId 10 = Filter Cutoff
        cutoffMod = modMatrix.getModulatedValue(10, 0.0f);
        // ParamId 11 = Filter Resonance
        resonanceMod = modMatrix.getModulatedValue(11, 0.0f);
        // ParamId 2 = Osc Mix
        oscMixMod = modMatrix.getModulatedValue(2, 0.0f);
        // ParamId 24 = Amp Level
        ampMod = modMatrix.getModulatedValue(24, 0.0f);

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
                case LfoDest::Amp:        ampMod += v; break;
                case LfoDest::PulseWidth: lfoPwMod += v * 0.4f; break;
                case LfoDest::CrossMod:   lfoXmodMod += v; break;
                default: break;          // off, or a page cell (applyParamRoutes)
            }
        };
        for (int i = 0; i < lfoCount && i < LFOBank::MAX_LFOS; ++i)
            route(lfoDest[(size_t) i], lfoBank.getOutput(i));
        // The extra envelopes sent to one of the LFOs' destinations.
        if (envDest != LfoDest::Off && envChain.getStageCount() > 0)
            route(envDest, envOutput * envChain.getDepth());
        lfoPan = std::clamp(lfoPan, -1.0f, 1.0f);

        // LFOs modulating LFOs take effect on the next sample.
        for (int i = 0; i < LFOBank::MAX_LFOS; ++i)
        {
            auto& lfo = lfoBank.getLFO(i);
            lfo.setRateMod(rateMod[(size_t) i]);
            lfo.setDepthMod(depthMod[(size_t) i]);
        }
    }

    float process()
    {
        if (!active)
            return 0.0f;

        // Wheel, velocity and this voice's pressure move each LFO's depth.
        for (int i = 0; i < LFOBank::MAX_LFOS; ++i)
        {
            const auto& c = lfoControl[(size_t) i];
            lfoBank.getLFO(i).setDepthOffset(c.wheel * lfoWheel + c.velocity * lfoNoteVelocity + c.pressure * lfoPressure);
        }

        // Process LFO bank
        lfoBank.processAll();

        // Process envelope chain (needed for mod matrix feed)
        float chainMod = envChain.process();

        // Feed mod sources and read modulated offsets
        float modPitchOffset = 0.0f, modCutoffOffset = 0.0f;
        float modResonanceOffset = 0.0f, modOscMixOffset = 0.0f, modAmpOffset = 0.0f;
        processModulation(modPitchOffset, modCutoffOffset, modResonanceOffset,
                          modOscMixOffset, modAmpOffset);
        if (anyParamRoute)
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

        const float pwMod = lfoPwMod + atTimbre * 0.4f;
        const float xmodNow = std::clamp(crossMod + lfoXmodMod + atFm, 0.0f, 1.0f);
        modOscMixOffset += atWavePos * 0.5f;

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

        float oscMix = 0.0f;
        for (int i = 0; i < MAX_OSCS; ++i)
        {
            if (!oscs[i].enabled)
                continue;

            // Apply osc mix modulation: scale level of osc 1 up / osc 0 down (or vice versa)
            float levelScale = oscs[i].level;
            if (MAX_OSCS >= 2 && modOscMixOffset != 0.0f)
            {
                if (i == 0)
                    levelScale = std::clamp(oscs[i].level - modOscMixOffset, 0.0f, 2.0f);
                else if (i == 1)
                    levelScale = std::clamp(oscs[i].level + modOscMixOffset, 0.0f, 2.0f);
            }
            levelScale *= lfoOscGain[(size_t) i];

            int numStackVoices = waveStack.voiceCount();

            if (numStackVoices <= 1)
            {
                // No stacking: single oscillator
                double freq = baseFrequency * std::exp2((oscs[i].detune + driftSemitones
                                                         + (i == 1 ? polyModSemis : 0.0f)) / 12.0);
                double phaseInc = freq / sampleRate;
                oscs[i].phase += phaseInc;
                if (oscs[i].phase >= 1.0)
                    oscs[i].phase -= 1.0;

                OscState shaped = oscs[i];
                shaped.pulseWidth = std::clamp(shaped.pulseWidth + pwMod, 0.05f, 0.95f);
                shaped.aMod = pwMod;
                if (i == 0 && xmodNow > 0.0f)
                {
                    double pm = shaped.phase + xmodPhase;
                    shaped.phase = pm - std::floor(pm);
                }

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

                    stackPhases[i][v] += phaseInc;
                    if (stackPhases[i][v] >= 1.0)
                        stackPhases[i][v] -= 1.0;

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
                    float sample = generateOsc(tempOsc, oscWaveshapeOffset[i]);
                    stackMix += sample * gain;
                }

                if (i == 1)
                    lastOsc2 = stackMix;
                oscMix += stackMix * levelScale;
            }
        }

        // Noise source and mixer drive (soft saturation before the filter).
        if (noiseLevel > 0.0f)
        {
            // Colour: a one-pole low-pass on the noise, white at 0 and dark at 1.
            float n = dsp::xorshiftNoise(noiseSeed);
            noiseState += noiseCoeff * (n - noiseState);
            oscMix += noiseState * noiseLevel * noiseMakeup * lfoNoiseGain;
        }

        if (mixDrive > 0.0f)
        {
            const float g = 1.0f + mixDrive * 5.0f;
            oscMix = dsp::fastTanh(oscMix * g) / dsp::fastTanh(g) ;
        }

        // Filter cutoff: envelope, key tracking, velocity, LFOs and pressure all move it in octaves
        // around the base frequency; mod-matrix routings add Hz on top.
        float filterEnvVal = filterEnv.process();
        lastFilterEnv = filterEnvVal;
        const float filterFmNow = lfoFilterFmMod != 0.0f ? std::clamp(filterFm + lfoFilterFmMod, 0.0f, 1.0f) : filterFm;
        float cutoffOct = filterFmNow * lastOsc2 * 2.0f     // audio-rate cutoff modulation from oscillator 2
                        + filterEnvVal * filterEnvAmount * FILTER_ENV_OCTAVES
                        + filterKeyTrack * (currentPitch - 60.0f) / 12.0f
                        + filterVelAmount * currentVelocity * 4.0f
                        + lfoCutoffOct
                        + atCutoff * 4.0f
                        + noteFilterOct;
        float cutoff = filterBaseCutoff * (cutoffOct != 0.0f ? std::exp2(cutoffOct) : 1.0f) + modCutoffOffset;
        // Filter B follows that modulation fully, partly or not at all.
        float cutoffB = filterBaseCutoff * (cutoffOct != 0.0f ? std::exp2(cutoffOct * filterBTrack) : 1.0f)
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
            filterSlot.setResonance(0, std::clamp(filterBaseResonance + modResonanceOffset, 0.0f, 1.0f));
            filterSlot.setResonance(1, std::clamp(filterResonanceB + modResonanceOffset, 0.0f, 1.0f));
            resonanceModulated = true;
        }
        else if (resonanceModulated)
        {
            filterSlot.setResonance(0, filterBaseResonance);
            filterSlot.setResonance(1, filterResonanceB);
            resonanceModulated = false;
        }

        float filtered = filterSlot.process(oscMix);

        float ampVal = ampEnv.process();

        // Apply chain modulation to amplitude if targeted there
        if (envChain.getStageCount() > 0 && envChain.getTarget() == EnvelopeChain::Target::Amplitude)
            ampVal = envChain.getModulatedValue(ampVal);

        if (!ampEnv.isActive())
            active = false;

        // Apply amp modulation (mod matrix, LFOs, pressure) AFTER filter
        float ampScale = std::clamp(1.0f + modAmpOffset + atVolume, 0.0f, 2.0f);

        // Velocity sensitivity: 0 = every note at full level, 1 = level follows velocity.
        float velGain = 1.0f - velSensitivity + velSensitivity * currentVelocity;

        return filtered * ampVal * velGain * ampScale;
    }

    void reset()
    {
        ampEnv.reset();
        filterEnv.reset();
        filterSlot.reset();
        envChain.reset();
        lfoBank.resetAll();
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
    ModMatrix& getModMatrix() { return modMatrix; }
    const ModMatrix& getModMatrix() const { return modMatrix; }
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
    void setLfoDest(int lfoIndex, LfoDest d)
    {
        if (lfoIndex >= 0 && lfoIndex < LFOBank::MAX_LFOS)
            lfoDest[(size_t) lfoIndex] = d;
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
    void setFilterResonance(float q) { filterBaseResonance = q; filterResonanceB = q; filterSlot.setResonance(q); }
    void setFilterResonanceA(float q) { filterBaseResonance = q; filterSlot.setResonance(0, q); }
    void setFilterResonanceB(float q) { filterResonanceB = q; filterSlot.setResonance(1, q); }
    void setFilterModel(int slot, FilterModel model)
    {
        filterSlot.setModel(slot, model);
    }
    void setFilterMode(int slot, int mode) { filterSlot.setMode(slot, mode); }
    void setFilterRouting(FilterRouting r) { filterSlot.setRouting(r); }
    void setFilterBalance(float b) { filterSlot.setBalance(b); }
    void setFilterDrive(float d) { filterSlot.setDrive(d); }
    void setFilterDrive(int slot, float d) { filterSlot.setDrive(slot, d); }
    void setFilterFM(float amount) { filterFm = std::clamp(amount, 0.0f, 1.0f); }
    void setFilterBTrack(float amount) { filterBTrack = std::clamp(amount, 0.0f, 1.0f); }
    void setPolyMod(float amount) { polyMod = std::clamp(amount, -1.0f, 1.0f); }

private:
    // One sample of the sources routed to page cells: every routed cell takes its set value moved
    // by the sum of its sources (in the cell's own scale, see PARAM_DESTS).
    void applyParamRoutes(float envValue)
    {
        std::array<float, NUM_MOD_SOURCES> amount {};
        const int lfoCount = lfoBank.getActiveCount();
        for (int s = 0; s < LFOBank::MAX_LFOS && s < lfoCount; ++s)
            amount[(size_t) s] = lfoBank.getOutput(s);
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
            const float moved = r.log ? r.base * std::exp2(total * r.span) : r.base + total * r.span;
            setParam(r.id, std::clamp(moved, r.lo, r.hi));
        }
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

    ModMatrix modMatrix;
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
    std::array<float, MAX_OSCS> lfoOscGain { 1.0f, 1.0f, 1.0f };

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

// -------------------------------------------------------------------------
// CPUMeter: simple per-track CPU cost measurement.
// Measures wall-clock time spent in processBlock vs. buffer duration.
// Reports as a percentage. Name bar warning when >80%.
// Audio-thread safe: uses only atomics and pre-allocated state.
// -------------------------------------------------------------------------

class CPUMeter
{
public:
    static constexpr int MAX_TRACKS = 16;
    static constexpr float WARNING_THRESHOLD = 80.0f; // percent

    void configure(double sampleRate, int blockSize)
    {
        bufferDurationUs = (blockSize / sampleRate) * 1e6;
    }

    // Call at the start of processing a track
    void beginTrack(int trackIndex)
    {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
            return;
        trackStartUs[trackIndex] = nowMicroseconds();
    }

    // Call at the end of processing a track
    void endTrack(int trackIndex)
    {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
            return;
        int64_t elapsed = nowMicroseconds() - trackStartUs[trackIndex];
        // Exponential moving average (alpha = 0.1)
        float pct = (bufferDurationUs > 0.0)
            ? (float)(elapsed / bufferDurationUs) * 100.0f
            : 0.0f;
        trackCpuPercent[trackIndex] = trackCpuPercent[trackIndex] * 0.9f + pct * 0.1f;
    }

    // Get CPU percentage for a track
    float getTrackCPU(int trackIndex) const
    {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
            return 0.0f;
        return trackCpuPercent[trackIndex];
    }

    // Get total CPU across all tracks
    float getTotalCPU() const
    {
        float total = 0.0f;
        for (int i = 0; i < MAX_TRACKS; ++i)
            total += trackCpuPercent[i];
        return total;
    }

    // Check if any track or total is above warning threshold
    bool isOverloaded() const
    {
        return getTotalCPU() > WARNING_THRESHOLD;
    }

    // Get a short status string for display
    // Returns "CPU XX%" or empty if below threshold
    juce::String getWarningText() const
    {
        float total = getTotalCPU();
        if (total > WARNING_THRESHOLD)
            return "CPU " + juce::String((int)total) + "%";
        return {};
    }

    // Get formatted percentage for settings screen
    juce::String getFormattedTotal() const
    {
        return juce::String((int)getTotalCPU()) + "%";
    }

    juce::String getFormattedTrack(int trackIndex) const
    {
        return "T" + juce::String(trackIndex + 1) + ":" + juce::String((int)getTrackCPU(trackIndex)) + "%";
    }

private:
    static int64_t nowMicroseconds()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    double bufferDurationUs = 0.0;
    std::array<int64_t, MAX_TRACKS> trackStartUs{};
    std::array<float, MAX_TRACKS> trackCpuPercent{};
};
