#pragma once

#include "Preset.h"
#include "../dsp/filters/FilterSlot.h"
#include "../engine/DrumVoice.h"
#include "../engine/ArchitecturePreset.h"
#include "../engine/SampleBank.h"
#include "../engine/WavetableEngine.h"
#include <cmath>
#include <vector>

// Forward declaration so callers can wire this up
class PresetManager;

namespace FactoryPresets
{
    // ------------------------------------------------------------------
    //  UnifiedVoice parameter IDs (from UnifiedVoiceEngine.h):
    //    0  = Osc1 Shape       1  = Osc2 Shape       2  = Osc Mix
    //    3  = Osc1 Detune      5  = Osc2 Detune      7  = Portamento
    //   10  = Filter Freq (Hz) 11 = Filter Reso      12 = Filter Key Track
    //   14  = Filter Env Amt   16 = Filter Drive     17 = Filter Model
    //   18  = Filter Routing   19 = Filter Model B
    //   20  = Amp Attack (s)   21 = Amp Decay (s)    22 = Amp Sustain (0-1)
    //   23  = Amp Release (s)  24 = Volume (0-1)
    //   60  = FEnv Attack (s)  61 = FEnv Decay (s)   62 = FEnv Sustain (0-1)
    //   63  = FEnv Release (s) 70 = Architecture Preset
    //
    //  FM parameter IDs (from FMEngine.h):
    //    OpN*10 + 0 = Ratio,  +1 = Level,  +2 = Detune,  +3 = Feedback
    //    OpN*10 + 4 = Attack, +5 = Decay,  +6 = Sustain, +7 = Release
    //    60 = Algorithm, 61 = Master Level
    //
    //  Drum parameter IDs (from DrumEngine.h):
    //    VoiceIdx * 8 + offset:  0=Tune(Hz), 1=Decay(s), 2=Tone(0-1),
    //    3=Drive(0-1), 4=Snap(0-1), 5=Level(0-1), 6=Pan(0-1), 7=Type(enum)
    //
    //  DrumSynthType values:
    //    FMKick=0, AnalogKick=1, ModCyclesKick=2, NoiseSnare=3, FMSnare=4,
    //    Clap=5, ClosedHat=6, OpenHat=7, Cymbal=8, Tom=9, Rim=10,
    //    Shaker=11, Clave=12, SubBass=13
    //
    //  MacroOsc parameter IDs (from MacroOscEngine.h):
    //    0 = Model (0=VA, 1=FM2Op, 2=WaveFold, 3=Formant, 4=Harmonic,
    //        5=PluckedString, 6=ParticleNoise, 7=BowedString)
    //    1 = Timbre, 2 = Color, 3 = Harmonics, 4 = Morph,
    //    5 = Decay (s), 6 = Level (0-1), 7 = Detune (cents),
    //    11 = AmpAttack, 12 = AmpDecay, 13 = AmpSustain, 14 = AmpRelease
    //
    //  Wavetable parameter IDs (from WavetableEngine.h):
    //    0 = Position (0-1), 1 = InterpMode (0=Linear, 2=Stepped, 3=Vintage, 4=Classic),
    //    2 = Detune, 3 = UnisonSpread, 4 = SubLevel, 5 = MorphSpeed (Hz),
    //    6 = Level, 7 = Octave, 8 = TableSelect (0=Saw, 1=Square,
    //        2=Triangle, 3=Sine, 4=PWM, 5=Formant, 6=Harmonics,
    //        7=Spectral, 8=Digital, 9=USER, 10=KEYS, 11-24 the newer tables:
    //        see appendWavetableTablePresets, which uses the engine's names),
    //    9 = AmpAttack, 10 = AmpDecay, 11 = AmpSustain, 12 = AmpRelease
    //
    //  WaveSeq parameter IDs (from WaveSeqEngine.h):
    //    0 = SeqLength (1-16), 1 = Crossfade (0-1), 2 = LoopMode (0-4),
    //    3 = Speed, 4 = PitchRange (semitones),
    //    5 = ShapeMix (0=Sine..5=Noise), 6 = Level, 7 = GateLength
    //
    //  Sampler parameter IDs (from SamplerEngine.h):
    //    0 = SliceSelect, 1 = StartPoint, 2 = EndPoint, 3 = LoopStart,
    //    4 = LoopEnd, 5 = PitchSemi, 6 = Mode (0-5), 7 = FineTune,
    //    8 = BitReduce (0-1), 9 = RateReduce (0-1), 10 = DriveAmt (0-1),
    //    11 = Volume, 12 = Pan, 13 = Attack, 14 = Decay, 15 = Release,
    //    16 = VelToFilter, 17 = FilterCutoff (0-1), 18 = SliceCount
    // ------------------------------------------------------------------

    // The unified-voice presets below are written in an older parameter layout (continuous
    // oscillator "shape", mix where 1 = oscillator 1, filter envelope on 60-63, ...). This maps
    // them onto the engine's real parameters (UnifiedParam in UnifiedVoiceEngine.h).
    inline ParamMap convertUnified(const ParamMap& in)
    {
        auto wave = [](float shape)   // 0 sine .. 0.3 triangle .. 0.5 square .. 0.8 saw
        {
            if (shape < 0.15f) return 0.0f;   // SIN
            if (shape < 0.40f) return 3.0f;   // TRI
            if (shape < 0.65f) return 2.0f;   // SQR
            return 1.0f;                      // SAW
        };
        auto get = [&](int id, float d) { auto it = in.find(id); return it != in.end() ? it->second : d; };

        ParamMap out;
        for (auto& [id, v] : in)
        {
            switch (id)
            {
                case 0:  out[0] = wave(v); break;
                case 1:  out[1] = wave(v); break;
                case 2:  out[2] = 1.0f - v; break;                 // MIX: 0 = oscillator 1
                case 3:  break;                                    // folded into oscillator 2 detune below
                case 5:  break;
                case 12: out[15] = v; break;                       // key tracking
                case 17: case 19:                                  // filter model: old enum value -> type index
                    out[id] = (float) FilterSlot::indexFromModel((FilterModel) juce::jlimit(0, 3, (int) v));
                    break;
                case 60: out[90] = v; break;                       // filter envelope
                case 61: out[91] = v; break;
                case 62: out[92] = v; break;
                case 63: out[93] = v; break;
                case 70: out[110] = v + 1.0f; break;               // architecture (0 = init)
                default: out[id] = v; break;
            }
        }
        // Detune between the oscillators, in cents.
        float cents = (get(5, 0.0f) - get(3, 0.0f)) * 100.0f;
        if (cents != 0.0f)
            out[8] = juce::jlimit(-50.0f, 50.0f, cents);
        return out;
    }

    // Helper to build a factory preset
    inline Preset make(const juce::String& name, const juce::String& category,
                       EngineType type, ParamMap params)
    {
        Preset p;
        p.name = name;
        p.category = category;
        p.engineType = type;
        p.soundParams = (type == EngineType::UnifiedVoice) ? convertUnified(params) : std::move(params);
        p.isFactory = true;
        return p;
    }

    // Helper for presets captured from the app in the engine's native parameter
    // format (bypasses the old-layout conversion that make() applies).
    inline Preset makeNative(const juce::String& name, const juce::String& category,
                             EngineType type, ParamMap params)
    {
        Preset p;
        p.name = name;
        p.category = category;
        p.engineType = type;
        p.soundParams = std::move(params);
        p.isFactory = true;
        return p;
    }

    // Helper to set drum voice params in a ParamMap
    inline void setDrumVoice(ParamMap& params, int voiceIdx,
                             float tune, float decay, float tone,
                             float drive, float snap, float level,
                             float pan, int type)
    {
        int base = voiceIdx * 8;
        params[base + 0] = tune;
        params[base + 1] = decay;
        params[base + 2] = tone;
        params[base + 3] = drive;
        params[base + 4] = snap;
        params[base + 5] = level;
        params[base + 6] = pan;
        params[base + 7] = (float)type;
    }

    // ------------------------------------------------------------------
    //  Mod strip
    //
    //  Every factory preset says here what the mod strip does to it (the
    //  "Mod Wheel" tests fail for a preset the strip does not change).
    //
    //  Unified voice: through LFO WHL amounts (WheelRoute, MOD / MOD2 pages);
    //  nothing is heard of these LFOs with the strip at rest. A preset
    //  without an entry keeps the engine's built-in strip vibrato.
    //
    //  The other engines have a fixed strip route each (FM: modulation
    //  index and vibrato; macro: fourth macro and vibrato; wavetable:
    //  position and vibrato; wave sequencer: sequence rate; sampler: drive
    //  and rate reduction; drums: decay and drive) plus, where they have
    //  the voice filter, its WHL cell (F ENV page, id 115; cutoff is 102).
    //  The entries below give those routes something to work on.
    // ------------------------------------------------------------------
    inline void addWheelRoute(Preset& preset)
    {
        using D = UnifiedVoice::LfoDest;
        using S = LFO::Shape;
        auto& p = preset.soundParams;
        const juce::String& n = preset.name;

        // ---- Unified voice: basses - tone, not vibrato
        if (n == "Sub Bass")            // the sine folds into harmonics and the filter lets them through
        {
            WheelRoute::to(p, 0, D::PulseWidth, -0.8f);
            WheelRoute::to(p, 1, D::Cutoff, 0.55f);
        }
        else if (n == "Acid Bass")      // cutoff and resonance together
        {
            WheelRoute::to(p, 0, D::Cutoff, 0.5f);
            WheelRoute::to(p, 1, D::Resonance, 0.3f);
        }
        else if (n == "Deep Sub")       // opens up and leans towards the triangle
        {
            WheelRoute::to(p, 0, D::Cutoff, 0.3f);
            WheelRoute::to(p, 1, D::OscMix, 0.6f);
        }
        else if (n == "Growl Bass")     // cross-modulation growl, filter opening
        {
            WheelRoute::to(p, 0, D::CrossMod, 0.5f);
            WheelRoute::to(p, 1, D::Cutoff, 0.4f);
        }
        else if (n == "Reese")          // filter opens, the two saws beat faster
        {
            WheelRoute::to(p, 0, D::Cutoff, 0.6f);
            WheelRoute::to(p, 1, D::Osc2Pitch, 0.02f);
        }
        // ---- pads - filter and slow movement
        else if (n == "Warm Pad 2")     // filter opens, slow vibrato
        {
            WheelRoute::to(p, 0, D::Cutoff, 0.4f);
            WheelRoute::vibrato(p, 1, 4.5f, 0.2f);
        }
        else if (n == "Glass Pad")      // slow wavefolding shimmer, light vibrato
        {
            WheelRoute::motion(p, 0, 0.3f, S::Sine, D::PulseWidth, 0.8f);
            WheelRoute::vibrato(p, 1, 5.0f, 0.15f);
        }
        else if (n == "Dark Pad")       // filter opens, slow pulse-width motion
        {
            WheelRoute::to(p, 0, D::Cutoff, 0.6f);
            WheelRoute::motion(p, 1, 0.2f, S::Sine, D::PulseWidth, 0.5f);
        }
        else if (n == "Shimmer")        // flutter in the waveshape, tremolo
        {
            WheelRoute::motion(p, 0, 8.0f, S::SmoothRandom, D::PulseWidth, 0.7f);
            WheelRoute::motion(p, 1, 5.5f, S::Sine, D::Amp, 0.35f);
        }
        // ---- leads - vibrato and brightness
        else if (n == "Classic Lead")
        {
            WheelRoute::vibrato(p, 0, 5.5f, 0.4f);
            WheelRoute::to(p, 1, D::Cutoff, 0.3f);
        }
        else if (n == "Sync Lead")      // cross-modulation bite, vibrato
        {
            WheelRoute::to(p, 0, D::CrossMod, 0.7f);
            WheelRoute::vibrato(p, 1, 5.5f, 0.3f);
        }
        else if (n == "Screamer")       // the resonant filter sweeps up, wide vibrato
        {
            WheelRoute::to(p, 0, D::Cutoff, 0.45f);
            WheelRoute::vibrato(p, 1, 6.0f, 0.5f);
        }
        else if (n == "Soft Lead")      // the triangle squares up, gentle vibrato
        {
            WheelRoute::to(p, 0, D::PulseWidth, 0.5f);
            WheelRoute::vibrato(p, 1, 5.0f, 0.3f);
        }
        // ---- keys
        else if (n == "Organ")          // rotary speaker: tremolo and a little pitch wobble at one rate
        {
            WheelRoute::motion(p, 0, 6.5f, S::Sine, D::Amp, 0.5f);
            WheelRoute::motion(p, 1, 6.5f, S::Sine, D::Pitch, 0.025f);
        }
        else if (n == "Clav")           // wah: narrower pulse, filter up
        {
            WheelRoute::to(p, 0, D::PulseWidth, -0.6f);
            WheelRoute::to(p, 1, D::Cutoff, 0.35f);
        }
        // ---- effects
        else if (n == "Noise Sweep")    // resonance up, the filter flutters
        {
            WheelRoute::to(p, 0, D::Resonance, 0.6f);
            WheelRoute::motion(p, 1, 10.0f, S::SmoothRandom, D::Cutoff, 0.4f);
        }
        else if (n == "Laser")          // repeating downward zaps
        {
            WheelRoute::motion(p, 0, 8.0f, S::RampDown, D::Pitch, 0.6f);
        }
        else if (n == "Wind")           // gusts, and the filter starts to whistle
        {
            WheelRoute::motion(p, 0, 1.5f, S::SmoothRandom, D::Cutoff, 0.6f);
            WheelRoute::to(p, 1, D::Resonance, 0.5f);
        }

        else if (n == "Narptune")       // opens the dark filter, gentle vibrato
        {
            WheelRoute::to(p, 0, D::Cutoff, 0.5f);
            WheelRoute::vibrato(p, 1, 5.0f, 0.2f);
        }

        // ---- FM: the strip triples the modulation index. Where a modulator dies away at once
        // (or the preset has none), give the strip something to raise.
        else if (n == "FM Bass 2")      // the modulator keeps a little level while the note is held
            p[16] = 0.12f;
        else if (n == "Rhodes")         // the same: the bark stays in reach of the strip
            p[16] = 0.25f;
        else if (n == "FM Bell Pad")    // operator 6 modulates the three carriers very lightly; the strip makes it shimmer
        {
            p[50] = 2.0f;  p[51] = 0.10f;
            p[54] = 0.4f;  p[55] = 3.0f;  p[56] = 0.6f;  p[57] = 3.0f;
        }
        else if (n == "FM Metallic")    // an inharmonic operator 6 for the strip to turn up
        {
            p[50] = 2.76f; p[51] = 0.10f;
            p[54] = 0.01f; p[55] = 2.0f;  p[56] = 0.5f;  p[57] = 1.0f;
        }
        else if (n == "Metallic Stab")
        {
            p[50] = 7.0f;  p[51] = 0.10f;
            p[54] = 0.001f; p[55] = 0.3f; p[56] = 0.4f;  p[57] = 0.1f;
        }
        else if (n == "FM Pad")         // pads: the filter opens three octaves (it starts lower than before)
        {
            p[102] = 1800.0f;
            p[115] = 0.6f;
        }

        // ---- Wave sequencer: the strip doubles the sequence rate; the pad also opens its filter.
        else if (n == "Morphing Texture")
        {
            p[102] = 2500.0f;
            p[115] = 0.5f;
        }
    }

    // ===================== DRUM BREAKS (sampler) - begin =====================
    //  Two presets for every drum break of the SampleBank (B100, B170 rendered from the drum
    //  voices; the others recordings credited in docs/sample-credits.md):
    //
    //    "Break 90 Loop"    a held key plays the break round and round in time with the tempo,
    //                       whatever tempo it was played at: MODE = LOOP, SYNC on with LEN = the
    //                       break's bars, ALGO = SLCE on transient slices (every drum hit stays
    //                       as it was recorded, only its place in time moves). One voice, so a
    //                       new key starts the break again. Other keys play it higher or lower.
    //    "Break 90 Slices"  the sixteen strongest hits on sixteen keys (PLAY = KEYS: the root key
    //                       C4 plays slice 1), each to its end at its own speed and pitch.
    //
    //  A preset names its sample (Preset::sampleName); the place stored in parameter 24 is the one
    //  the break has in this run and is looked up again by name when the preset is applied.
    //  Sampler ids used: 6 MODE (2 = 1SHT, 4 = LOOP), 18 SLCS, 22 VOX, 24 SMPL, 40 slice MODE
    //  (1 = TRNS), 43 PLAY (1 = KEYS), 44 FADE, 46 ROOT, 48 ALGO (4 = SLCE), 50 SYNC, 51 LEN
    //  (index: 4 = one bar, 5 = two, 6 = four). The mod strip keeps the sampler's own routes
    //  (drive, sample-rate reduction).
    inline void appendBreakPresets(std::vector<Preset>& presets)
    {
        auto& bank = SampleBank::get();
        for (int n = 0; n < bank.breakCount(); ++n)
        {
            const int index = bank.breakIndex(n);
            const auto* e = bank.entry(index);
            if (e == nullptr || e->bars <= 0.0f)
                continue;
            const juce::String sample(e->name);                                  // "B090"
            const juce::String title = "Break " + sample.substring(1).trimCharactersAtStart("0");
            const float length = (float) juce::jlimit(0, 9, 4 + (int) std::lround(std::log2((double) e->bars)));

            auto loop = make(title + " Loop", "PERC", EngineType::Sampler, {
                {24, (float) index},  // SMPL: the break
                {6, 4.0f},            // MODE: LOOP
                {40, 1.0f},           // slice MODE: TRNS - cut at the drum hits
                {48, 4.0f},           // ALGO: SLCE - the hits are moved, not stretched
                {50, 1.0f},           // SYNC: on
                {51, length},         // LEN: the break's own length in bars
                {22, 1.0f},           // VOX: one - a new key starts the break again
                {11, 0.8f},           // LEVL
            });
            loop.sampleName = sample;
            presets.push_back(std::move(loop));

            auto slices = make(title + " Slices", "PERC", EngineType::Sampler, {
                {24, (float) index},  // SMPL: the break
                {6, 2.0f},            // MODE: 1SHT - a tap plays the whole hit
                {40, 1.0f},           // slice MODE: TRNS
                {18, 16.0f},          // SLCS: the sixteen strongest hits (one for every pad)
                {43, 1.0f},           // PLAY: KEYS
                {46, 60.0f},          // ROOT: C4 plays the first slice
                {44, 0.005f},         // FADE: 5 ms at the end of a slice
                {11, 0.8f},           // LEVL
            });
            slices.sampleName = sample;
            presets.push_back(std::move(slices));
        }
    }
    // ===================== DRUM BREAKS (sampler) - end =====================

    // ===================== WAVETABLE: THE NEWER TABLES - begin =====================
    //  At least one preset for each of the wavetable engine's newer built-in tables (VOWL ... COMB,
    //  src/dsp/WaveTables.h), so that they are found from PRESET, and three that play a table
    //  through INTP = CLSC (the raw 8-bit read-out, not band-limited: it aliases on high notes).
    //
    //  Ids are the wavetable engine's own (WavetableEngine::Param and the voice filter from
    //  FilterBase; nothing above 115, which tests/factory_preset_coverage_tests.h holds a
    //  wavetable preset to, so movement comes from SCAN, MSPD / MDPT, ENVP and VELP). A table is named by its TABL label and looked
    //  up in WavetableEngine::tableLabels(), so a preset says which table it plays.
    //
    //  The mod strip keeps the engine's own route (POS +60, a little vibrato) and no preset sets
    //  a STRIP cell: every preset here rests low enough in its table, or is still on its way
    //  through it, for the strip to have somewhere to go.
    //
    //  LEVL is set by measurement, not by ear: the loudest 100 ms of two held notes is brought to
    //  about -12 dB, between the older wavetable presets (-8.9 to -18.8 dB, -13.4 on average);
    //  tests/wavetable_embed_tests.h logs every one and holds them within 4 dB of that average.
    //  (The frames of the newer tables all have an RMS of 0.35; pads with two copies and a full
    //  sustain come out louder than plucks, hence the different values.)
    //
    //  No preset uses the KEYS table: its key frames are cut from samples, and a preset keeps
    //  key samples by their place in the bank only (see .ralph/progress.md).
    inline float wavetableByLabel(const char* label)
    {
        const char* const* labels = WavetableEngine::tableLabels();
        for (int i = 0; i < WavetableEngine::NUM_TABLES; ++i)
            if (juce::String(labels[i]) == label)
                return (float) i;
        jassertfalse;
        return 0.0f;
    }

    inline void appendWavetableTablePresets(std::vector<Preset>& presets)
    {
        using W = WavetableEngine;
        const int F = W::FilterBase;                        // + VoiceFilterSection::Offset
        using VF = VoiceFilterSection;
        const float CLSC = (float) (int) WavetableInterpMode::Classic;
        const float RUN = (float) (int) WavetableScanMode::Run, PING = (float) (int) WavetableScanMode::Ping,
                    STEP = (float) (int) WavetableScanMode::Step;
        auto add = [&presets](const char* name, const char* category, const char* table, ParamMap params)
        {
            params[W::TableSelect] = wavetableByLabel(table);
            presets.push_back(make(name, category, EngineType::Wavetable, std::move(params)));
        };

        // VOWL - a lead that speaks: the note starts on "ah", harder notes start further along,
        // and the strip walks it on through "eh" and "ee".
        add("Vowel Lead", "LEAD", "VOWL", {
            {W::Position, 0.0f}, {W::VelToPosition, 0.2f}, {W::Glide, 0.05f}, {W::SubLevel, 0.15f},
            {W::AmpAttack, 0.01f}, {W::AmpDecay, 0.4f}, {W::AmpSustain, 0.85f}, {W::AmpRelease, 0.25f},
            {W::Level, 0.8f},
        });

        // VOWL - a slow choir: two detuned copies drifting between the first three vowels.
        add("Vowel Choir", "PAD", "VOWL", {
            {W::Position, 0.3f}, {W::MorphSpeed, 0.13f}, {W::MorphDepth, 0.5f}, {W::UnisonSpread, 0.5f},
            {W::AmpAttack, 0.9f}, {W::AmpDecay, 2.0f}, {W::AmpSustain, 0.8f}, {W::AmpRelease, 2.8f},
            {W::Level, 0.65f},
        });

        // SYNC - the classic sync sweep: the envelope pulls the restarted sine up and lets it fall.
        add("Sync Sweep Lead", "LEAD", "SYNC", {
            {W::Position, 0.12f}, {W::EnvToPosition, 0.55f}, {W::UnisonSpread, 0.2f},
            {W::AmpAttack, 0.004f}, {W::AmpDecay, 0.7f}, {W::AmpSustain, 0.45f}, {W::AmpRelease, 0.3f},
            {W::Level, 1.0f},
        });

        // FOLD - a bass that folds over at the start of each note and more when played hard.
        add("Fold Bass", "BASS", "FOLD", {
            {W::Octave, -1.0f}, {W::Position, 0.06f}, {W::EnvToPosition, 0.4f}, {W::VelToPosition, 0.25f},
            {W::SubLevel, 0.3f},
            {W::AmpAttack, 0.003f}, {W::AmpDecay, 0.35f}, {W::AmpSustain, 0.4f}, {W::AmpRelease, 0.15f},
            {W::Level, 0.76f},
        });

        // FM1 - an electric-piano kind of key: the FM index follows the envelope and the velocity.
        add("FM Table Keys", "KEYS", "FM1", {
            {W::Position, 0.04f}, {W::EnvToPosition, 0.22f}, {W::VelToPosition, 0.2f},
            {W::AmpAttack, 0.002f}, {W::AmpDecay, 1.4f}, {W::AmpSustain, 0.2f}, {W::AmpRelease, 0.5f},
            {W::Level, 1.0f},
        });

        // FM3 - a glassy pluck: a bright strike that falls back to a sine.
        add("Glass Pluck", "KEYS", "FM3", {
            {W::Position, 0.05f}, {W::EnvToPosition, 0.5f}, {W::VelToPosition, 0.2f},
            {W::AmpAttack, 0.001f}, {W::AmpDecay, 0.6f}, {W::AmpSustain, 0.0f}, {W::AmpRelease, 0.6f},
            {W::Level, 1.0f},
        });

        // BELL - a struck bar: the harder the strike the more partials, and they die away first.
        add("Struck Bar", "KEYS", "BELL", {
            {W::Position, 0.1f}, {W::EnvToPosition, 0.4f}, {W::VelToPosition, 0.35f},
            {W::AmpAttack, 0.001f}, {W::AmpDecay, 2.5f}, {W::AmpSustain, 0.0f}, {W::AmpRelease, 2.0f},
            {W::Level, 0.9f},
        });

        // ORGN - four drawbars out, no dynamics, a key click's worth of attack; the strip pulls the
        // rest of the stops.
        add("Drawbar Organ", "KEYS", "ORGN", {
            {W::Position, 0.4f}, {W::VelSens, 0.0f}, {W::Vibrato, 0.08f},
            {W::AmpAttack, 0.003f}, {W::AmpDecay, 0.1f}, {W::AmpSustain, 1.0f}, {W::AmpRelease, 0.06f},
            {W::Level, 0.64f},
        });

        // NOIS - every note is a breath of noise that settles into a tone in a second and a half
        // (SCAN RUN); through a low-pass that closes with it.
        add("Noise To Tone", "FX", "NOIS", {
            {W::Position, 0.0f}, {W::ScanMode, RUN}, {W::ScanTime, 1.5f},
            {W::AmpAttack, 0.005f}, {W::AmpDecay, 1.0f}, {W::AmpSustain, 0.8f}, {W::AmpRelease, 0.8f},
            {F + VF::Cutoff, 2500.0f}, {F + VF::EnvAmount, 0.35f}, {F + VF::Decay, 1.2f},
            {W::Level, 0.8f},
        });

        // RESO - the resonant sweep as a bass line: the "filter" in the table snaps shut after
        // each note's start.
        add("Reso Sweep Bass", "BASS", "RESO", {
            {W::Octave, -1.0f}, {W::Position, 0.12f}, {W::EnvToPosition, 0.5f}, {W::VelToPosition, 0.15f},
            {W::SubLevel, 0.2f},
            {W::AmpAttack, 0.003f}, {W::AmpDecay, 0.3f}, {W::AmpSustain, 0.35f}, {W::AmpRelease, 0.12f},
            {W::Level, 0.9f},
        });

        // RESO - the same sweep slowly (MSPD: nine seconds there and back), on two detuned copies.
        add("Reso Sweep Pad", "PAD", "RESO", {
            {W::Position, 0.3f}, {W::UnisonSpread, 0.6f}, {W::MorphSpeed, 0.11f}, {W::MorphDepth, 0.5f},
            {W::AmpAttack, 1.2f}, {W::AmpDecay, 2.0f}, {W::AmpSustain, 0.8f}, {W::AmpRelease, 3.0f},
            {W::Level, 0.75f},
        });

        // WLSH - a bar-long staircase through the Walsh functions, sixteen steps in time with the
        // tempo (SCAN STEP, SYNC on): square-cornered and rhythmic.
        add("Walsh Stepper", "LEAD", "WLSH", {
            {W::Position, 0.0f}, {W::ScanMode, STEP}, {W::ScanSync, 1.0f}, {W::ScanLength, 4.0f}, {W::ScanSteps, 2.0f},
            {W::AmpAttack, 0.003f}, {W::AmpDecay, 0.3f}, {W::AmpSustain, 0.8f}, {W::AmpRelease, 0.2f},
            {W::Level, 0.85f},
        });

        // PRIM - the prime harmonics come and go over eight seconds (SCAN PING).
        add("Prime Shimmer", "PAD", "PRIM", {
            {W::Position, 0.05f}, {W::ScanMode, PING}, {W::ScanTime, 8.0f}, {W::UnisonSpread, 0.45f},
            {W::AmpAttack, 0.7f}, {W::AmpDecay, 2.0f}, {W::AmpSustain, 0.85f}, {W::AmpRelease, 3.0f},
            {W::Level, 0.6f},
        });

        // OCTV - only octaves: the weight climbs to the top ones and back down in six seconds.
        add("Octave Climb", "PAD", "OCTV", {
            {W::Position, 0.0f}, {W::ScanMode, PING}, {W::ScanTime, 6.0f}, {W::UnisonSpread, 0.3f},
            {W::AmpAttack, 0.4f}, {W::AmpDecay, 1.5f}, {W::AmpSustain, 0.9f}, {W::AmpRelease, 2.5f},
            {W::Level, 0.6f},
        });

        // PDST - phase distortion's best-known use: a round bass with a bright, velocity-shaped start.
        add("Phase Bass", "BASS", "PDST", {
            {W::Octave, -1.0f}, {W::Position, 0.1f}, {W::EnvToPosition, 0.5f}, {W::VelToPosition, 0.3f},
            {W::AmpAttack, 0.002f}, {W::AmpDecay, 0.28f}, {W::AmpSustain, 0.4f}, {W::AmpRelease, 0.15f},
            {W::Level, 0.8f},
        });

        // COMB - a phasing pad: the comb's notches wander down and up through two detuned saws.
        add("Comb Phase Pad", "PAD", "COMB", {
            {W::Position, 0.3f}, {W::MorphSpeed, 0.1f}, {W::MorphDepth, 0.5f}, {W::UnisonSpread, 0.4f},
            {W::AmpAttack, 0.5f}, {W::AmpDecay, 1.5f}, {W::AmpSustain, 0.8f}, {W::AmpRelease, 2.2f},
            {W::Level, 0.68f},
        });

        // ---- INTP = CLSC: 64 positions, 128 held samples of 8 bits, nothing band-limited ---------

        // The vowels an octave up, stepping from one to the next: the top folds back and rings.
        add("Eight Bit Vowels", "LEAD", "VOWL", {
            {W::InterpMode, CLSC}, {W::Octave, 1.0f}, {W::Position, 0.15f},
            {W::MorphSpeed, 0.5f}, {W::MorphDepth, 0.3f},
            {W::AmpAttack, 0.005f}, {W::AmpDecay, 0.4f}, {W::AmpSustain, 0.8f}, {W::AmpRelease, 0.3f},
            {W::Level, 0.9f},
        });

        // The struck bar an octave up: the aliasing gives it the glassy top of an early digital bell.
        add("Glassy Bell", "KEYS", "BELL", {
            {W::InterpMode, CLSC}, {W::Octave, 1.0f}, {W::Position, 0.3f}, {W::EnvToPosition, 0.35f},
            {W::AmpAttack, 0.001f}, {W::AmpDecay, 1.8f}, {W::AmpSustain, 0.0f}, {W::AmpRelease, 1.5f},
            {W::Level, 0.9f},
        });

        // The sync sweep read raw: gritty steps where the smooth one glides.
        add("Raw Sync Lead", "LEAD", "SYNC", {
            {W::InterpMode, CLSC}, {W::Position, 0.2f}, {W::EnvToPosition, 0.45f},
            {W::AmpAttack, 0.003f}, {W::AmpDecay, 0.9f}, {W::AmpSustain, 0.5f}, {W::AmpRelease, 0.25f},
            {W::Level, 0.9f},
        });
    }
    // ===================== WAVETABLE: THE NEWER TABLES - end =====================

    // Returns all factory presets. Called by PresetManager::loadFactoryPresets()
    // and usable independently in tests.
    inline std::vector<Preset> getAll()
    {
        std::vector<Preset> presets;
        presets.reserve(46);

        // ===================== BASS (6) =====================

        // Sub Bass - pure sine, low cutoff, clean sub
        presets.push_back(make("Sub Bass", "BASS", EngineType::UnifiedVoice, {
            {0, 0.0f},             // Osc1 sine
            {1, 0.0f},             // Osc2 sine
            {2, 1.0f},             // Osc1 only
            {10, 200.0f},          // Filter freq low
            {11, 0.0f},            // No resonance
            {20, 0.005f},          // Fast attack
            {21, 0.3f},            // Medium decay
            {22, 0.8f},            // High sustain
            {23, 0.15f},           // Short release
            {24, 0.9f},            // Volume
        }));

        // FM Bass 2 - simple 2-op FM bass
        presets.push_back(make("FM Bass 2", "BASS", EngineType::FM, {
            {0, 1.0f},             // Op1 ratio = 1
            {1, 1.0f},             // Op1 level = 1
            {4, 0.005f},           // Op1 attack
            {5, 0.3f},             // Op1 decay
            {6, 0.3f},             // Op1 sustain
            {7, 0.15f},            // Op1 release
            {10, 2.0f},            // Op2 ratio = 2
            {11, 0.7f},            // Op2 level
            {14, 0.005f},          // Op2 attack
            {15, 0.15f},           // Op2 decay
            {16, 0.0f},            // Op2 sustain
            {17, 0.1f},            // Op2 release
            {60, 0.0f},            // Algorithm 0 (serial)
            {61, 0.9f},            // Master level
        }));

        // Acid Bass - saw, resonant ladder filter, low cutoff
        presets.push_back(make("Acid Bass", "BASS", EngineType::UnifiedVoice, {
            {0, 0.8f},             // Osc1 saw-ish
            {1, 0.0f},             // Osc2 off
            {2, 1.0f},             // Osc1 only
            {10, 400.0f},          // Filter freq low
            {11, 0.75f},           // High resonance
            {14, 0.6f},            // Filter env amount
            {16, 0.2f},            // Some drive
            {17, 1.0f},            // Ladder filter
            {20, 0.005f},          // Fast attack
            {21, 0.2f},            // Medium decay
            {22, 0.0f},            // Zero sustain for pluck
            {23, 0.1f},            // Short release
            {24, 0.85f},           // Volume
            {60, 0.005f},          // Filter env attack
            {61, 0.3f},            // Filter env decay
            {62, 0.1f},            // Filter env sustain
            {63, 0.1f},            // Filter env release
        }));

        // Deep Sub - sine + triangle, very low cutoff
        presets.push_back(make("Deep Sub", "BASS", EngineType::UnifiedVoice, {
            {0, 0.0f},             // Osc1 sine
            {1, 0.3f},             // Osc2 triangle-ish
            {2, 0.7f},             // Mostly Osc1
            {5, 0.02f},            // Osc2 slight detune
            {10, 120.0f},          // Very low filter
            {11, 0.1f},            // Slight reso
            {20, 0.01f},           // Fast attack
            {21, 0.5f},            // Medium decay
            {22, 0.9f},            // High sustain
            {23, 0.2f},            // Short release
            {24, 0.9f},            // Volume
        }));

        // Growl Bass - 2x saw detuned, drive, medium filter
        presets.push_back(make("Growl Bass", "BASS", EngineType::UnifiedVoice, {
            {0, 0.8f},             // Osc1 saw
            {1, 0.8f},             // Osc2 saw
            {2, 0.5f},             // Equal mix
            {3, 0.1f},             // Osc1 detune
            {5, -0.1f},            // Osc2 detune opposite
            {10, 800.0f},          // Medium filter
            {11, 0.5f},            // Medium resonance
            {14, 0.3f},            // Filter env amount
            {16, 0.5f},            // Drive
            {17, 1.0f},            // Ladder filter
            {20, 0.005f},          // Fast attack
            {21, 0.4f},            // Medium decay
            {22, 0.5f},            // Medium sustain
            {23, 0.15f},           // Short release
            {24, 0.85f},           // Volume
        }));

        // Reese - 2x saw, slow detune drift, low filter
        presets.push_back(make("Reese", "BASS", EngineType::UnifiedVoice, {
            {0, 0.8f},             // Osc1 saw
            {1, 0.8f},             // Osc2 saw
            {2, 0.5f},             // Equal mix
            {3, 0.05f},            // Slow detune
            {5, -0.05f},           // Opposite detune
            {10, 400.0f},          // Low filter
            {11, 0.3f},            // Moderate resonance
            {16, 0.15f},           // Slight drive
            {17, 1.0f},            // Ladder filter
            {20, 0.01f},           // Fast attack
            {21, 0.5f},            // Medium decay
            {22, 0.7f},            // Good sustain
            {23, 0.3f},            // Medium release
            {24, 0.85f},           // Volume
        }));

        // ===================== PAD (5) =====================

        // Warm Pad 2 - 2 saws detuned, slow attack, low filter
        presets.push_back(make("Warm Pad 2", "PAD", EngineType::UnifiedVoice, {
            {0, 0.7f},             // Osc1 saw-ish
            {1, 0.7f},             // Osc2 saw-ish
            {2, 0.5f},             // Equal mix
            {5, 0.08f},            // Detune for width
            {10, 2000.0f},         // Low-mid filter
            {11, 0.15f},           // Gentle reso
            {20, 1.2f},            // Slow attack
            {21, 2.0f},            // Long decay
            {22, 0.7f},            // Good sustain
            {23, 2.5f},            // Long release
            {24, 0.6f},            // Moderate volume
        }));

        // Glass Pad - triangle + sine, high cutoff, long release
        presets.push_back(make("Glass Pad", "PAD", EngineType::UnifiedVoice, {
            {0, 0.3f},             // Osc1 triangle-ish
            {1, 0.0f},             // Osc2 sine
            {2, 0.6f},             // Mostly osc1
            {5, 0.03f},            // Slight detune
            {10, 12000.0f},        // High cutoff
            {11, 0.05f},           // Minimal reso
            {20, 0.8f},            // Medium-slow attack
            {21, 1.5f},            // Long decay
            {22, 0.5f},            // Medium sustain
            {23, 4.0f},            // Very long release
            {24, 0.55f},           // Moderate volume
        }));

        // FM Bell Pad - FM bell-like ratios, long release
        presets.push_back(make("FM Bell Pad", "PAD", EngineType::FM, {
            {0, 1.0f},             // Op1 ratio 1
            {1, 0.5f},             // Op1 level
            {4, 0.01f},            // Op1 attack
            {5, 3.0f},             // Op1 long decay
            {6, 0.2f},             // Op1 sustain
            {7, 4.0f},             // Op1 long release
            {10, 3.5f},            // Op2 ratio 3.5 (bell-like)
            {11, 0.4f},            // Op2 level
            {14, 0.01f},           // Op2 attack
            {15, 2.0f},            // Op2 long decay
            {16, 0.1f},            // Op2 sustain
            {17, 3.0f},            // Op2 release
            {20, 7.0f},            // Op3 ratio 7 (harmonic shimmer)
            {21, 0.2f},            // Op3 level
            {24, 0.001f},          // Op3 attack
            {25, 1.5f},            // Op3 decay
            {26, 0.0f},            // Op3 sustain
            {27, 2.0f},            // Op3 release
            {60, 4.0f},            // Algorithm 4
            {61, 0.6f},            // Master level
        }));

        // Dark Pad - square + saw, very low cutoff
        presets.push_back(make("Dark Pad", "PAD", EngineType::UnifiedVoice, {
            {0, 0.5f},             // Osc1 square-ish
            {1, 0.8f},             // Osc2 saw
            {2, 0.5f},             // Equal mix
            {5, 0.06f},            // Detune
            {10, 600.0f},          // Very low cutoff
            {11, 0.2f},            // Gentle reso
            {14, 0.1f},            // Slight filter env
            {20, 1.5f},            // Slow attack
            {21, 2.5f},            // Long decay
            {22, 0.6f},            // Medium sustain
            {23, 3.0f},            // Long release
            {24, 0.6f},            // Volume
        }));

        // Shimmer - high cutoff, chorus-like detune, long envelope
        presets.push_back(make("Shimmer", "PAD", EngineType::UnifiedVoice, {
            {0, 0.3f},             // Osc1 triangle-ish
            {1, 0.3f},             // Osc2 triangle-ish
            {2, 0.5f},             // Equal mix
            {5, 0.12f},            // Chorus-like detune
            {10, 15000.0f},        // High cutoff
            {11, 0.1f},            // Gentle reso
            {20, 1.0f},            // Slow attack
            {21, 3.0f},            // Long decay
            {22, 0.5f},            // Medium sustain
            {23, 5.0f},            // Very long release
            {24, 0.5f},            // Volume
        }));

        // ===================== LEAD (5) =====================

        // Classic Lead - mono, saw, medium filter, portamento
        presets.push_back(make("Classic Lead", "LEAD", EngineType::UnifiedVoice, {
            {0, 0.8f},             // Osc1 saw
            {1, 0.0f},             // Osc2 off
            {2, 1.0f},             // Osc1 only
            {7, 0.05f},            // Portamento
            {10, 4000.0f},         // Medium filter
            {11, 0.3f},            // Moderate reso
            {14, 0.4f},            // Filter env
            {20, 0.01f},           // Fast attack
            {21, 0.3f},            // Medium decay
            {22, 0.7f},            // Good sustain
            {23, 0.2f},            // Short release
            {24, 0.8f},            // Volume
        }));

        // Sync Lead - 2 oscs with pitch offset
        presets.push_back(make("Sync Lead", "LEAD", EngineType::UnifiedVoice, {
            {0, 0.8f},             // Osc1 saw
            {1, 0.8f},             // Osc2 saw
            {2, 0.5f},             // Equal mix
            {3, 0.0f},             // No detune on osc1
            {5, 0.5f},             // Osc2 pitched up (fifth-like)
            {10, 6000.0f},         // Medium-high filter
            {11, 0.4f},            // Moderate reso
            {14, 0.3f},            // Filter env
            {20, 0.01f},           // Fast attack
            {21, 0.2f},            // Short decay
            {22, 0.8f},            // High sustain
            {23, 0.15f},           // Short release
            {24, 0.8f},            // Volume
        }));

        // FM Pluck - short decay, bright
        presets.push_back(make("FM Pluck", "LEAD", EngineType::FM, {
            {0, 1.0f},             // Op1 ratio 1
            {1, 0.9f},             // Op1 level
            {4, 0.001f},           // Op1 instant attack
            {5, 0.4f},             // Op1 medium decay
            {6, 0.0f},             // Op1 no sustain
            {7, 0.1f},             // Op1 short release
            {10, 3.0f},            // Op2 ratio 3
            {11, 0.6f},            // Op2 level
            {14, 0.001f},          // Op2 instant attack
            {15, 0.15f},           // Op2 short decay
            {16, 0.0f},            // Op2 no sustain
            {17, 0.05f},           // Op2 short release
            {60, 0.0f},            // Algorithm 0 (serial)
            {61, 0.8f},            // Master level
        }));

        // Screamer - mono, saw, high resonance, drive
        presets.push_back(make("Screamer", "LEAD", EngineType::UnifiedVoice, {
            {0, 0.9f},             // Osc1 saw
            {1, 0.0f},             // Osc2 off
            {2, 1.0f},             // Osc1 only
            {10, 3000.0f},         // Medium filter
            {11, 0.85f},           // High resonance
            {14, 0.5f},            // Strong filter env
            {16, 0.6f},            // Drive
            {17, 1.0f},            // Ladder filter
            {20, 0.01f},           // Fast attack
            {21, 0.15f},           // Short decay
            {22, 0.8f},            // High sustain
            {23, 0.1f},            // Short release
            {24, 0.75f},           // Volume (lower to compensate drive)
        }));

        // Soft Lead - triangle, gentle filter
        presets.push_back(make("Soft Lead", "LEAD", EngineType::UnifiedVoice, {
            {0, 0.3f},             // Osc1 triangle
            {1, 0.0f},             // Osc2 off
            {2, 1.0f},             // Osc1 only
            {10, 5000.0f},         // Medium-high filter
            {11, 0.1f},            // Gentle reso
            {20, 0.05f},           // Gentle attack
            {21, 0.5f},            // Medium decay
            {22, 0.6f},            // Medium sustain
            {23, 0.3f},            // Medium release
            {24, 0.75f},           // Volume
        }));

        // ===================== KEYS (4) =====================

        // Electric Piano - DX7-like EP ratios
        presets.push_back(make("Electric Piano", "KEYS", EngineType::FM, {
            {0, 1.0f},             // Op1 ratio 1 (carrier)
            {1, 0.85f},            // Op1 level
            {4, 0.01f},            // Op1 attack
            {5, 1.0f},             // Op1 decay
            {6, 0.3f},             // Op1 sustain
            {7, 0.5f},             // Op1 release
            {10, 14.0f},           // Op2 ratio 14 (modulator - bell partial)
            {11, 0.25f},           // Op2 level
            {14, 0.005f},          // Op2 attack
            {15, 0.5f},            // Op2 decay
            {16, 0.0f},            // Op2 sustain
            {17, 0.3f},            // Op2 release
            {20, 1.0f},            // Op3 ratio 1 (second carrier)
            {21, 0.4f},            // Op3 level
            {24, 0.01f},           // Op3 attack
            {25, 0.8f},            // Op3 decay
            {26, 0.2f},            // Op3 sustain
            {27, 0.4f},            // Op3 release
            {60, 2.0f},            // Algorithm 2
            {61, 0.7f},            // Master level
        }));

        // Organ - sine + drawbar harmonics
        presets.push_back(make("Organ", "KEYS", EngineType::UnifiedVoice, {
            {0, 0.0f},             // Osc1 sine (fundamental)
            {1, 0.0f},             // Osc2 sine (octave up)
            {2, 0.6f},             // Mostly osc1
            {5, 1.0f},             // Osc2 up an octave
            {10, 8000.0f},         // Medium-high filter
            {11, 0.05f},           // Minimal reso
            {20, 0.005f},          // Fast attack
            {21, 0.01f},           // Instant decay
            {22, 1.0f},            // Full sustain
            {23, 0.05f},           // Short release
            {24, 0.7f},            // Volume
        }));

        // Clav - pulse wave, short decay
        presets.push_back(make("Clav", "KEYS", EngineType::UnifiedVoice, {
            {0, 0.5f},             // Osc1 square/pulse
            {1, 0.0f},             // Osc2 off
            {2, 1.0f},             // Osc1 only
            {10, 3500.0f},         // Medium-high filter
            {11, 0.4f},            // Moderate reso
            {14, 0.5f},            // Filter env
            {20, 0.001f},          // Instant attack
            {21, 0.3f},            // Short decay
            {22, 0.1f},            // Low sustain
            {23, 0.08f},           // Short release
            {24, 0.8f},            // Volume
            {60, 0.001f},          // Filter env fast attack
            {61, 0.2f},            // Filter env short decay
            {62, 0.0f},            // Filter env no sustain
            {63, 0.05f},           // Filter env short release
        }));

        // Rhodes - FM, 2-op with gentle ratios
        presets.push_back(make("Rhodes", "KEYS", EngineType::FM, {
            {0, 1.0f},             // Op1 ratio 1
            {1, 0.8f},             // Op1 level
            {4, 0.01f},            // Op1 attack
            {5, 1.2f},             // Op1 decay
            {6, 0.2f},             // Op1 sustain
            {7, 0.6f},             // Op1 release
            {10, 1.0f},            // Op2 ratio 1 (same as carrier)
            {11, 0.3f},            // Op2 moderate level
            {14, 0.005f},          // Op2 attack
            {15, 0.4f},            // Op2 decay
            {16, 0.0f},            // Op2 no sustain
            {17, 0.2f},            // Op2 release
            {60, 0.0f},            // Algorithm 0 (serial)
            {61, 0.7f},            // Master level
        }));

        // ===================== FX (4) =====================

        // Noise Sweep - noise osc, filter sweep
        presets.push_back(make("Noise Sweep", "FX", EngineType::UnifiedVoice, {
            {0, 1.0f},             // Osc1 noise-ish (high shape value)
            {1, 0.0f},             // Osc2 off
            {2, 1.0f},             // Osc1 only
            {10, 500.0f},          // Low filter start
            {11, 0.6f},            // Moderate reso
            {14, 0.8f},            // Strong filter env
            {20, 0.01f},           // Fast attack
            {21, 2.0f},            // Long decay
            {22, 0.0f},            // No sustain
            {23, 1.0f},            // Long release
            {24, 0.6f},            // Volume
            {60, 0.01f},           // Filter env attack
            {61, 3.0f},            // Filter env long decay (sweep)
            {62, 0.0f},            // Filter env no sustain
            {63, 0.5f},            // Filter env release
        }));

        // FM Metallic - inharmonic ratios, feedback
        presets.push_back(make("FM Metallic", "FX", EngineType::FM, {
            {0, 1.0f},             // Op1 ratio 1
            {1, 0.8f},             // Op1 level
            {3, 1.5f},             // Op1 feedback
            {4, 0.01f},            // Op1 attack
            {5, 2.0f},             // Op1 long decay
            {6, 0.1f},             // Op1 low sustain
            {7, 1.5f},             // Op1 long release
            {10, 1.41f},           // Op2 ratio sqrt(2) (inharmonic)
            {11, 0.7f},            // Op2 level
            {14, 0.01f},           // Op2 attack
            {15, 1.5f},            // Op2 decay
            {16, 0.05f},           // Op2 sustain
            {17, 1.0f},            // Op2 release
            {20, 3.14f},           // Op3 ratio pi (inharmonic)
            {21, 0.5f},            // Op3 level
            {24, 0.01f},           // Op3 attack
            {25, 1.0f},            // Op3 decay
            {26, 0.0f},            // Op3 sustain
            {27, 0.8f},            // Op3 release
            {60, 4.0f},            // Algorithm 4
            {61, 0.5f},            // Master level
        }));

        // Laser - pitch env sweep
        presets.push_back(make("Laser", "FX", EngineType::UnifiedVoice, {
            {0, 0.8f},             // Osc1 saw
            {1, 0.0f},             // Osc2 off
            {2, 1.0f},             // Osc1 only
            {10, 10000.0f},        // High filter
            {11, 0.3f},            // Moderate reso
            {14, 0.9f},            // Strong filter env (sweeps pitch effect)
            {20, 0.001f},          // Instant attack
            {21, 0.5f},            // Medium decay
            {22, 0.0f},            // No sustain
            {23, 0.1f},            // Short release
            {24, 0.7f},            // Volume
            {60, 0.001f},          // Filter env instant attack
            {61, 0.8f},            // Filter env sweep
            {62, 0.0f},            // No sustain
            {63, 0.05f},           // Short release
        }));

        // Wind - noise, bandpass filter
        presets.push_back(make("Wind", "FX", EngineType::UnifiedVoice, {
            {0, 1.0f},             // Osc1 noise
            {1, 0.0f},             // Osc2 off
            {2, 1.0f},             // Osc1 only
            {10, 2000.0f},         // Medium filter
            {11, 0.5f},            // Moderate reso
            {14, 0.2f},            // Gentle filter env
            {20, 0.5f},            // Slow attack
            {21, 3.0f},            // Long decay
            {22, 0.3f},            // Low sustain
            {23, 3.0f},            // Long release
            {24, 0.5f},            // Volume
        }));

        // ===================== DRUMS (6) =====================

        // 808 Kit 2 - AnalogKick, NoiseSnare, ClosedHat, OpenHat
        {
            ParamMap p;
            setDrumVoice(p, 8,  52.0f, 0.6f, 0.5f, 0.0f, 0.5f, 0.9f, 0.5f,
                         (int)DrumSynthType::AnalogKick);
            setDrumVoice(p, 9,  52.0f, 0.8f, 0.5f, 0.0f, 0.5f, 0.85f, 0.5f,
                         (int)DrumSynthType::AnalogKick);
            setDrumVoice(p, 10, 120.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.7f, 0.5f,
                         (int)DrumSynthType::Tom);
            setDrumVoice(p, 11, 100.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.7f, 0.4f,
                         (int)DrumSynthType::Tom);
            setDrumVoice(p, 12, 200.0f, 0.2f, 0.5f, 0.2f, 0.6f, 0.8f, 0.5f,
                         (int)DrumSynthType::NoiseSnare);
            setDrumVoice(p, 13, 1200.0f, 0.3f, 0.6f, 0.5f, 0.5f, 0.7f, 0.55f,
                         (int)DrumSynthType::Clap);
            setDrumVoice(p, 14, 8000.0f, 0.04f, 0.5f, 0.0f, 0.5f, 0.6f, 0.6f,
                         (int)DrumSynthType::ClosedHat);
            setDrumVoice(p, 15, 8000.0f, 0.5f, 0.5f, 0.0f, 0.5f, 0.6f, 0.6f,
                         (int)DrumSynthType::OpenHat);
            presets.push_back(make("808 Kit 2", "KIT", EngineType::Drum, std::move(p)));
        }

        // FM Kit - FMKick, FMSnare, Cymbal, Rim
        {
            ParamMap p;
            setDrumVoice(p, 8,  48.0f, 0.8f, 0.5f, 0.3f, 0.7f, 0.9f, 0.5f,
                         (int)DrumSynthType::FMKick);
            setDrumVoice(p, 9,  48.0f, 0.6f, 0.5f, 0.2f, 0.5f, 0.85f, 0.5f,
                         (int)DrumSynthType::FMKick);
            setDrumVoice(p, 10, 800.0f, 0.005f, 0.5f, 0.0f, 0.5f, 0.7f, 0.6f,
                         (int)DrumSynthType::Rim);
            setDrumVoice(p, 11, 150.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.7f, 0.45f,
                         (int)DrumSynthType::Tom);
            setDrumVoice(p, 12, 200.0f, 0.25f, 0.6f, 0.4f, 0.6f, 0.8f, 0.5f,
                         (int)DrumSynthType::FMSnare);
            setDrumVoice(p, 13, 1200.0f, 0.3f, 0.5f, 0.6f, 0.5f, 0.7f, 0.55f,
                         (int)DrumSynthType::Clap);
            setDrumVoice(p, 14, 7000.0f, 1.0f, 0.6f, 0.0f, 0.5f, 0.5f, 0.35f,
                         (int)DrumSynthType::Cymbal);
            setDrumVoice(p, 15, 9000.0f, 1.5f, 0.7f, 0.0f, 0.5f, 0.5f, 0.65f,
                         (int)DrumSynthType::Cymbal);
            presets.push_back(make("FM Kit", "KIT", EngineType::Drum, std::move(p)));
        }

        // Electro Kit - ModCyclesKick, Clap, ClosedHat, Shaker
        {
            ParamMap p;
            setDrumVoice(p, 8,  55.0f, 0.7f, 0.6f, 0.4f, 0.6f, 0.9f, 0.5f,
                         (int)DrumSynthType::ModCyclesKick);
            setDrumVoice(p, 9,  55.0f, 0.5f, 0.4f, 0.3f, 0.5f, 0.85f, 0.5f,
                         (int)DrumSynthType::ModCyclesKick);
            setDrumVoice(p, 10, 100.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.7f, 0.45f,
                         (int)DrumSynthType::Tom);
            setDrumVoice(p, 11, 5000.0f, 0.15f, 0.5f, 0.2f, 0.5f, 0.5f, 0.55f,
                         (int)DrumSynthType::Shaker);
            setDrumVoice(p, 12, 180.0f, 0.2f, 0.5f, 0.3f, 0.6f, 0.8f, 0.5f,
                         (int)DrumSynthType::NoiseSnare);
            setDrumVoice(p, 13, 1500.0f, 0.25f, 0.7f, 0.7f, 0.5f, 0.75f, 0.55f,
                         (int)DrumSynthType::Clap);
            setDrumVoice(p, 14, 8500.0f, 0.03f, 0.6f, 0.0f, 0.5f, 0.6f, 0.6f,
                         (int)DrumSynthType::ClosedHat);
            setDrumVoice(p, 15, 8000.0f, 0.5f, 0.5f, 0.0f, 0.5f, 0.55f, 0.6f,
                         (int)DrumSynthType::OpenHat);
            presets.push_back(make("Electro Kit", "KIT", EngineType::Drum, std::move(p)));
        }

        // Minimal Kit - SubBass, Clap, ClosedHat, Rim
        {
            ParamMap p;
            setDrumVoice(p, 8,  36.0f, 0.1f, 0.5f, 0.0f, 0.5f, 0.85f, 0.5f,
                         (int)DrumSynthType::SubBass);
            setDrumVoice(p, 9,  42.0f, 0.1f, 0.5f, 0.0f, 0.5f, 0.8f, 0.5f,
                         (int)DrumSynthType::SubBass);
            setDrumVoice(p, 10, 800.0f, 0.005f, 0.5f, 0.0f, 0.5f, 0.65f, 0.5f,
                         (int)DrumSynthType::Rim);
            setDrumVoice(p, 11, 120.0f, 0.25f, 0.5f, 0.0f, 0.5f, 0.65f, 0.4f,
                         (int)DrumSynthType::Tom);
            setDrumVoice(p, 12, 200.0f, 0.15f, 0.5f, 0.1f, 0.6f, 0.75f, 0.5f,
                         (int)DrumSynthType::NoiseSnare);
            setDrumVoice(p, 13, 1200.0f, 0.2f, 0.5f, 0.3f, 0.5f, 0.7f, 0.55f,
                         (int)DrumSynthType::Clap);
            setDrumVoice(p, 14, 9000.0f, 0.03f, 0.4f, 0.0f, 0.5f, 0.5f, 0.6f,
                         (int)DrumSynthType::ClosedHat);
            setDrumVoice(p, 15, 8000.0f, 0.4f, 0.5f, 0.0f, 0.5f, 0.5f, 0.6f,
                         (int)DrumSynthType::OpenHat);
            presets.push_back(make("Minimal Kit", "KIT", EngineType::Drum, std::move(p)));
        }

        // Industrial Kit - FMKick, NoiseSnare, Cymbal, Clave
        {
            ParamMap p;
            setDrumVoice(p, 8,  42.0f, 0.9f, 0.5f, 0.6f, 0.8f, 0.9f, 0.5f,
                         (int)DrumSynthType::FMKick);
            setDrumVoice(p, 9,  42.0f, 0.7f, 0.5f, 0.5f, 0.7f, 0.85f, 0.5f,
                         (int)DrumSynthType::FMKick);
            setDrumVoice(p, 10, 2500.0f, 0.02f, 0.5f, 0.0f, 0.5f, 0.7f, 0.6f,
                         (int)DrumSynthType::Clave);
            setDrumVoice(p, 11, 150.0f, 0.3f, 0.6f, 0.2f, 0.5f, 0.7f, 0.45f,
                         (int)DrumSynthType::Tom);
            setDrumVoice(p, 12, 250.0f, 0.3f, 0.7f, 0.6f, 0.7f, 0.8f, 0.5f,
                         (int)DrumSynthType::NoiseSnare);
            setDrumVoice(p, 13, 1500.0f, 0.25f, 0.7f, 0.8f, 0.5f, 0.75f, 0.55f,
                         (int)DrumSynthType::Clap);
            setDrumVoice(p, 14, 6000.0f, 1.5f, 0.8f, 0.0f, 0.5f, 0.55f, 0.4f,
                         (int)DrumSynthType::Cymbal);
            setDrumVoice(p, 15, 8000.0f, 0.5f, 0.6f, 0.0f, 0.5f, 0.5f, 0.6f,
                         (int)DrumSynthType::OpenHat);
            presets.push_back(make("Industrial Kit", "KIT", EngineType::Drum, std::move(p)));
        }

        // Percussion Kit - Tom, Rim, Shaker, Clave
        {
            ParamMap p;
            setDrumVoice(p, 8,  80.0f, 0.35f, 0.5f, 0.0f, 0.5f, 0.8f, 0.45f,
                         (int)DrumSynthType::Tom);
            setDrumVoice(p, 9,  120.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.75f, 0.5f,
                         (int)DrumSynthType::Tom);
            setDrumVoice(p, 10, 800.0f, 0.005f, 0.5f, 0.0f, 0.5f, 0.7f, 0.55f,
                         (int)DrumSynthType::Rim);
            setDrumVoice(p, 11, 4000.0f, 0.12f, 0.5f, 0.1f, 0.5f, 0.55f, 0.55f,
                         (int)DrumSynthType::Shaker);
            setDrumVoice(p, 12, 2500.0f, 0.02f, 0.5f, 0.0f, 0.5f, 0.7f, 0.5f,
                         (int)DrumSynthType::Clave);
            setDrumVoice(p, 13, 1200.0f, 0.25f, 0.6f, 0.4f, 0.5f, 0.7f, 0.55f,
                         (int)DrumSynthType::Clap);
            setDrumVoice(p, 14, 8000.0f, 0.04f, 0.5f, 0.0f, 0.5f, 0.5f, 0.6f,
                         (int)DrumSynthType::ClosedHat);
            setDrumVoice(p, 15, 8000.0f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.6f,
                         (int)DrumSynthType::OpenHat);
            presets.push_back(make("Percussion Kit", "KIT", EngineType::Drum, std::move(p)));
        }

        // ===================== MACRO OSCILLATOR (2) =====================

        // Grain Cloud - particle noise model, dense colored texture
        presets.push_back(make("Grain Cloud", "FX", EngineType::MacroOsc, {
            {0, 6.0f},             // Model: ParticleNoise
            {1, 0.7f},            // Timbre: high density
            {2, 0.8f},            // Color: colored/filtered
            {3, 0.5f},            // Ring: none (the cell did nothing in this model when the preset was made)
            {4, 0.6f},            // Morph: blended output
            {6, 0.7f},            // Level
            {11, 0.5f},           // Amp Attack: slow fade in
            {12, 2.0f},           // Amp Decay: long
            {13, 0.4f},           // Amp Sustain
            {14, 3.0f},           // Amp Release: very long tail
        }));

        // Resonator Pluck - Karplus-Strong physical model, bright attack
        presets.push_back(make("Resonator Pluck", "KEYS", EngineType::MacroOsc, {
            {0, 5.0f},             // Model: PluckedString
            {1, 0.3f},            // Timbre: moderate damping (brighter)
            {2, 0.4f},            // Color: pick position
            {3, 0.8f},            // Harmonics: long sustain feedback
            {6, 0.8f},            // Level
            {11, 0.001f},         // Amp Attack: instant
            {12, 1.5f},           // Amp Decay: long ring
            {13, 0.0f},           // Amp Sustain: pluck envelope
            {14, 1.0f},           // Amp Release
        }));

        // ===================== WAVETABLE (3) =====================

        // Digital Choir - formant wavetable, slow morph, pad envelope
        presets.push_back(make("Digital Choir", "PAD", EngineType::Wavetable, {
            {0, 0.3f},            // Position: vocal-like area of formant table
            {1, 0.0f},            // InterpMode: Linear (smooth crossfade)
            {4, 0.15f},           // SubLevel: subtle low octave
            {5, 0.4f},            // MorphSpeed: slow LFO scan through formants
            {6, 0.7f},            // Level
            {8, 5.0f},            // TableSelect: Formant
            {9, 1.0f},            // Amp Attack: slow pad-like
            {10, 2.0f},           // Amp Decay: long
            {11, 0.6f},           // Amp Sustain
            {12, 3.0f},           // Amp Release: long tail
        }));

        // Frozen Sweep - spectral wavetable, static position, spacious
        presets.push_back(make("Frozen Sweep", "PAD", EngineType::Wavetable, {
            {0, 0.5f},            // Position: mid-spectrum
            {1, 0.0f},            // InterpMode: Linear
            {4, 0.2f},            // SubLevel: gentle sub
            {5, 0.0f},            // MorphSpeed: frozen (no auto-scan)
            {6, 0.7f},            // Level
            {8, 7.0f},            // TableSelect: Spectral Sweep
            {9, 0.8f},            // Amp Attack: medium-slow
            {10, 1.5f},           // Amp Decay
            {11, 0.5f},           // Amp Sustain
            {12, 4.0f},           // Amp Release: very long
        }));

        // Glitch Table - digital wavetable, stepped interpolation, fast morph
        presets.push_back(make("Glitch Table", "FX", EngineType::Wavetable, {
            {0, 0.7f},            // Position: into the crunchy zone
            {1, 2.0f},            // InterpMode: Stepped (lo-fi jumps)
            {5, 3.0f},            // MorphSpeed: fast scanning
            {6, 0.65f},           // Level
            {8, 8.0f},            // TableSelect: Digital (bitcrush)
            {9, 0.01f},           // Amp Attack: fast
            {10, 0.3f},           // Amp Decay: short
            {11, 0.7f},           // Amp Sustain
            {12, 0.15f},          // Amp Release: quick
        }));

        // ===================== WAVE SEQUENCER (2) =====================

        // Sequenced Bells - short sine sequence, bell-like pitch jumps
        presets.push_back(make("Sequenced Bells", "KEYS", EngineType::WaveSeq, {
            {0, 8.0f},            // SeqLength: 8 steps
            {1, 0.3f},            // Crossfade: moderate glide between steps
            {2, 0.0f},            // LoopMode: Forward
            {3, 1.5f},            // Speed: moderate tempo
            {4, 12.0f},           // PitchRange: full octave range
            {5, 0.0f},            // ShapeMix: Sine (pure, bell-like)
            {6, 0.7f},            // Level
            {7, 0.6f},            // GateLength: slightly detached
        }));

        // Morphing Texture - long ping-pong sequence, high crossfade
        presets.push_back(make("Morphing Texture", "PAD", EngineType::WaveSeq, {
            {0, 16.0f},           // SeqLength: maximum steps
            {1, 0.8f},            // Crossfade: very smooth transitions
            {2, 2.0f},            // LoopMode: PingPong
            {3, 0.5f},            // Speed: slow, evolving
            {4, 7.0f},            // PitchRange: fifth range
            {5, 1.0f},            // ShapeMix: Saw (harmonically rich)
            {6, 0.65f},           // Level
            {7, 0.9f},            // GateLength: nearly legato
        }));

        // ===================== FM EXTENDED (3) =====================

        // Glass Marimba - bright percussive FM, quick harmonic decay
        presets.push_back(make("Glass Marimba", "KEYS", EngineType::FM, {
            {0, 1.0f},            // Op1 ratio 1 (carrier)
            {1, 0.9f},            // Op1 level
            {4, 0.001f},          // Op1 instant attack
            {5, 0.8f},            // Op1 medium-long decay
            {6, 0.0f},            // Op1 no sustain (percussive)
            {7, 0.3f},            // Op1 short release
            {10, 4.0f},           // Op2 ratio 4 (bright harmonic)
            {11, 0.5f},           // Op2 level
            {14, 0.001f},         // Op2 instant attack
            {15, 0.25f},          // Op2 short decay (transient brightness)
            {16, 0.0f},           // Op2 no sustain
            {17, 0.1f},           // Op2 short release
            {20, 10.0f},          // Op3 ratio 10 (sparkle)
            {21, 0.15f},          // Op3 low level
            {24, 0.001f},         // Op3 instant attack
            {25, 0.1f},           // Op3 very short decay
            {26, 0.0f},           // Op3 no sustain
            {27, 0.05f},          // Op3 tiny release
            {60, 0.0f},           // Algorithm 0 (serial)
            {61, 0.7f},           // Master level
        }));

        // Metallic Stab - inharmonic FM hit, aggressive transient
        presets.push_back(make("Metallic Stab", "LEAD", EngineType::FM, {
            {0, 1.0f},            // Op1 ratio 1
            {1, 1.0f},            // Op1 level: full
            {4, 0.001f},          // Op1 instant attack
            {5, 0.15f},           // Op1 short decay
            {6, 0.3f},            // Op1 some sustain
            {7, 0.08f},           // Op1 short release
            {10, 1.41f},          // Op2 ratio sqrt(2) (inharmonic)
            {11, 0.8f},           // Op2 level: high
            {14, 0.001f},         // Op2 instant attack
            {15, 0.08f},          // Op2 very short decay
            {16, 0.0f},           // Op2 no sustain
            {17, 0.05f},          // Op2 tiny release
            {20, 5.0f},           // Op3 ratio 5
            {21, 0.4f},           // Op3 moderate level
            {23, 0.5f},           // Op3 feedback: some grit
            {24, 0.001f},         // Op3 instant attack
            {25, 0.2f},           // Op3 short decay
            {26, 0.0f},           // Op3 no sustain
            {27, 0.1f},           // Op3 release
            {60, 4.0f},           // Algorithm 4
            {61, 0.75f},          // Master level
        }));

        // FM Pad - warm slow FM with gentle modulation
        presets.push_back(make("FM Pad", "PAD", EngineType::FM, {
            {0, 1.0f},            // Op1 ratio 1 (carrier)
            {1, 0.6f},            // Op1 level
            {4, 1.2f},            // Op1 slow attack (pad)
            {5, 2.0f},            // Op1 long decay
            {6, 0.5f},            // Op1 medium sustain
            {7, 3.0f},            // Op1 long release
            {10, 2.0f},           // Op2 ratio 2 (octave harmonic)
            {11, 0.25f},          // Op2 gentle modulation
            {14, 1.5f},           // Op2 slow attack
            {15, 2.5f},           // Op2 long decay
            {16, 0.2f},           // Op2 low sustain
            {17, 2.0f},           // Op2 long release
            {20, 3.0f},           // Op3 ratio 3 (fifth harmonic)
            {21, 0.1f},           // Op3 subtle level
            {24, 2.0f},           // Op3 very slow attack
            {25, 3.0f},           // Op3 very long decay
            {26, 0.1f},           // Op3 low sustain
            {27, 2.5f},           // Op3 long release
            {60, 2.0f},           // Algorithm 2
            {61, 0.65f},          // Master level
        }));

        // ===================== SAMPLER (3) =====================

        // Lo-Fi Tape - bit-crushed, rate-reduced, driven warmth
        presets.push_back(make("Lo-Fi Tape", "FX", EngineType::Sampler, {
            {6, 4.0f},            // Mode: Loop
            {8, 0.3f},            // BitReduce: noticeable crunch
            {9, 0.15f},           // RateReduce: mild aliasing
            {10, 0.2f},           // DriveAmt: warm saturation
            {11, 0.8f},           // Volume
            {13, 0.01f},          // Attack: quick
            {14, 0.5f},           // Decay: medium
            {15, 0.3f},           // Release: medium
            {17, 0.6f},           // FilterCutoff: darkened top end
        }));

        // Granular Pad - looped sample with slow attack, filtered
        presets.push_back(make("Granular Pad", "PAD", EngineType::Sampler, {
            {6, 4.0f},            // Mode: Loop
            {11, 0.7f},           // Volume
            {13, 1.0f},           // Attack: slow pad-like
            {14, 2.0f},           // Decay: long
            {15, 3.0f},           // Release: very long
            {16, 0.3f},           // VelToFilter: velocity opens filter
            {17, 0.5f},           // FilterCutoff: mid-range warmth
        }));

        // Vinyl Scratch - ping-pong playback with lo-fi character
        presets.push_back(make("Vinyl Scratch", "FX", EngineType::Sampler, {
            {6, 5.0f},            // Mode: PingPong
            {8, 0.15f},           // BitReduce: subtle crunch
            {9, 0.2f},            // RateReduce: more aliasing
            {10, 0.15f},          // DriveAmt: mild saturation
            {11, 0.75f},          // Volume
            {13, 0.001f},         // Attack: instant
            {14, 0.4f},           // Decay: medium
            {15, 0.2f},           // Release: short
            {17, 0.7f},           // FilterCutoff: slightly opened
        }));

        // ===================== DRUM BREAKS (sampler): a loop and a sliced preset each =====================
        appendBreakPresets(presets);

        // ===================== ADDITIONAL DRUMS (2) =====================

        // Lo-Fi Kit - driven sounds, short decays, gritty character
        {
            ParamMap p;
            setDrumVoice(p, 8,  50.0f, 0.5f, 0.4f, 0.6f, 0.7f, 0.9f, 0.5f,
                         (int)DrumSynthType::AnalogKick);
            setDrumVoice(p, 9,  45.0f, 0.4f, 0.3f, 0.5f, 0.6f, 0.85f, 0.5f,
                         (int)DrumSynthType::AnalogKick);
            setDrumVoice(p, 10, 100.0f, 0.2f, 0.4f, 0.4f, 0.5f, 0.7f, 0.45f,
                         (int)DrumSynthType::Tom);
            setDrumVoice(p, 11, 3000.0f, 0.08f, 0.3f, 0.5f, 0.5f, 0.5f, 0.55f,
                         (int)DrumSynthType::Shaker);
            setDrumVoice(p, 12, 180.0f, 0.15f, 0.4f, 0.5f, 0.7f, 0.8f, 0.5f,
                         (int)DrumSynthType::NoiseSnare);
            setDrumVoice(p, 13, 1000.0f, 0.2f, 0.5f, 0.6f, 0.5f, 0.7f, 0.55f,
                         (int)DrumSynthType::Clap);
            setDrumVoice(p, 14, 7000.0f, 0.02f, 0.3f, 0.3f, 0.5f, 0.5f, 0.6f,
                         (int)DrumSynthType::ClosedHat);
            setDrumVoice(p, 15, 6000.0f, 0.3f, 0.3f, 0.3f, 0.5f, 0.45f, 0.6f,
                         (int)DrumSynthType::OpenHat);
            presets.push_back(make("Lo-Fi Kit", "KIT", EngineType::Drum, std::move(p)));
        }

        // Minimal Techno Kit - clean, tight, electronic
        {
            ParamMap p;
            setDrumVoice(p, 8,  40.0f, 0.15f, 0.5f, 0.0f, 0.3f, 0.9f, 0.5f,
                         (int)DrumSynthType::SubBass);
            setDrumVoice(p, 9,  38.0f, 0.12f, 0.5f, 0.0f, 0.3f, 0.85f, 0.5f,
                         (int)DrumSynthType::SubBass);
            setDrumVoice(p, 10, 700.0f, 0.005f, 0.5f, 0.0f, 0.5f, 0.7f, 0.55f,
                         (int)DrumSynthType::Rim);
            setDrumVoice(p, 11, 2500.0f, 0.015f, 0.5f, 0.0f, 0.5f, 0.65f, 0.5f,
                         (int)DrumSynthType::Clave);
            setDrumVoice(p, 12, 190.0f, 0.12f, 0.5f, 0.0f, 0.5f, 0.8f, 0.5f,
                         (int)DrumSynthType::NoiseSnare);
            setDrumVoice(p, 13, 1300.0f, 0.15f, 0.5f, 0.0f, 0.5f, 0.7f, 0.55f,
                         (int)DrumSynthType::Clap);
            setDrumVoice(p, 14, 9500.0f, 0.025f, 0.5f, 0.0f, 0.5f, 0.55f, 0.6f,
                         (int)DrumSynthType::ClosedHat);
            setDrumVoice(p, 15, 8500.0f, 0.35f, 0.5f, 0.0f, 0.5f, 0.5f, 0.6f,
                         (int)DrumSynthType::OpenHat);
            presets.push_back(make("Minimal Techno Kit", "KIT", EngineType::Drum, std::move(p)));
        }

        // ===================== WAVETABLE: THE NEWER TABLES (16) AND THE CLSC READ-OUT (3) =====================
        appendWavetableTablePresets(presets);

        // ===================== SAVED FROM THE APP (1) =====================

        // Narptune - the owner's sound, exactly as PRESET's SV stored it (2026-11-11): two saws
        // 8 cents apart with the third oscillator's pitch an octave down, ladder filter A at 32 Hz
        // with resonance 0.9 and full drive, filter envelope 0.64, 10 s release, poly 8.
        // Ids are UnifiedParam's. Left out, as for every factory preset: the track's mixer values
        // it was saved with (volume 0.86, sends 0.20 / 0.53, send bus 1 effect type 7).
        presets.push_back(makeNative("Narptune", "BASS", EngineType::UnifiedVoice, {
            {0, 1.0f}, {1, 1.0f}, {2, 0.5f}, {3, 0.0f}, {4, 0.0f},
            {5, 0.0f}, {6, 0.0f}, {7, 0.0f}, {8, 8.0f}, {9, 0.5f},
            {10, 32.4360695f}, {11, 0.9000001f}, {12, 0.0f}, {13, 0.05f}, {14, 0.6399994f},
            {15, 0.0f}, {16, 1.0f}, {17, 1.0f}, {18, 2.0f}, {19, 0.0f},
            {20, 0.01f}, {21, 0.3f}, {22, 1.0f}, {23, 10.0f}, {24, 0.8f},
            {26, 0.7f}, {27, 0.0f}, {90, 0.01f}, {91, 0.5f}, {92, 0.0f},
            {93, 0.3f}, {94, 0.0f}, {95, 0.0f}, {96, 0.0f}, {97, 0.0f},
            {98, -12.0f}, {99, 0.0f}, {100, 0.0f}, {101, 2.0f}, {102, 0.0f},
            {103, 0.0f}, {104, 0.2f}, {105, 0.5f}, {106, 0.0f}, {107, 0.8599995f},
            {108, 0.0f}, {109, 1.0f}, {110, 0.0f}, {111, 2.0f}, {112, 8.0f},
            {113, 0.0f}, {114, 0.0f}, {115, 0.1f}, {116, 0.03f}, {117, 0.0f},
            {118, 0.9059854f}, {119, 2.4999998f}, {120, 1.0f}, {121, 0.0f}, {122, 30.5f},
            {123, 0.5000001f}, {124, 0.7299998f}, {125, 0.0f},
        }));

        for (auto& preset : presets)
            addWheelRoute(preset);

        return presets;
    }

    // Convenience: register all factory presets into a PresetManager.
    // Requires PresetManager to be fully defined (include after PresetManager.h).
    template <typename PM>
    void registerAll(PM& pm)
    {
        for (auto& preset : getAll())
            pm.addPreset(preset);
    }

} // namespace FactoryPresets
