#pragma once

#include "EngineBase.h"
#include "UnifiedVoice.h"
#include "VoiceAllocator.h"
#include "../dsp/filters/FilterSlot.h"
#include "../dsp/Envelope.h"
#include "../dsp/LFO.h"
#include <juce_core/juce_core.h>
#include <array>
#include <vector>

// -----------------------------------------------------------------------------
// Mod strip routes for the unified voice, as parameter values.
//
// The unified voice has no cell "strip -> destination": the strip reaches a
// destination through an LFO's WHL amount (MOD2 page), which adds strip x WHL to
// that LFO's depth. These helpers write the LFO cells for the three useful
// cases; everything they set is on the MOD and MOD2 pages and can be changed
// there. While an LFO in use has a WHL amount, the engine's built-in strip
// vibrato is off.
// -----------------------------------------------------------------------------
namespace WheelRoute
{
    inline void lfo(ParamMap& p, int index, float hz, LFO::Shape shape, LFO::TriggerMode trig,
                    UnifiedVoice::LfoDest dest, float wheelAmount)
    {
        p[ModParam::lfoParamId(index, ModParam::LFO_RATE)] = hz;
        p[ModParam::lfoParamId(index, ModParam::LFO_DEPTH)] = 0.0f;      // nothing with the strip at rest
        p[ModParam::lfoParamId(index, ModParam::LFO_SHAPE)] = (float) (int) shape;
        p[ModParam::lfoParamId(index, ModParam::LFO_TRIG)] = (float) (int) trig;
        p[ModParam::lfoParamId(index, ModParam::LFO_SYNC)] = 0.0f;
        p[ModParam::lfoParamId(index, ModParam::LFO_DEST)] = (float) (int) dest;
        p[ModParam::lfoExt2ParamId(index, ModParam::LFO_WHEEL)] = wheelAmount;
        auto count = p.find(ModParam::LFO_COUNT);
        if (count == p.end() || count->second < (float) (index + 1))
            p[ModParam::LFO_COUNT] = (float) (index + 1);
    }

    // The strip brings in vibrato of up to this many semitones.
    inline void vibrato(ParamMap& p, int index, float hz, float semitones)
    {
        lfo(p, index, hz, LFO::Shape::Sine, LFO::TriggerMode::Free, UnifiedVoice::LfoDest::Pitch, semitones / 12.0f);
    }

    // The strip brings in a moving LFO on a destination (amount in the destination's LFO range).
    inline void motion(ParamMap& p, int index, float hz, LFO::Shape shape, UnifiedVoice::LfoDest dest, float amount)
    {
        lfo(p, index, hz, shape, LFO::TriggerMode::Free, dest, amount);
    }

    // The strip moves a destination directly: a one-shot rising LFO at the fastest rate reaches 1
    // within 25 ms of the note and stays there, so the destination gets strip x amount of its LFO
    // range (cutoff: 4 octaves, pitch: 12 semitones, PW: 0.4, resonance and mix: 0.5).
    inline void to(ParamMap& p, int index, UnifiedVoice::LfoDest dest, float amount)
    {
        lfo(p, index, 40.0f, LFO::Shape::ExpUp, LFO::TriggerMode::Single, dest, amount);
    }
}

enum class ArchitecturePresetId
{
    MonoVA,
    HybridAggressive,
    VirtualPolysynth,
    Count = 3
};

struct ModRouting
{
    int sourceId;
    ParamId destId;
    float depth;
};

struct ArchitecturePreset
{
    ArchitecturePresetId id;
    juce::String name;
    juce::String description;

    // Oscillator configuration
    int oscCount = 2;
    std::array<UnifiedVoice::OscWaveform, 3> oscWaveforms = {
        UnifiedVoice::OscWaveform::Saw,
        UnifiedVoice::OscWaveform::Saw,
        UnifiedVoice::OscWaveform::Sine
    };
    std::array<float, 3> oscLevels = { 1.0f, 0.0f, 0.0f };
    std::array<float, 3> oscDetune = { 0.0f, 0.0f, 0.0f };
    std::array<bool, 3> oscEnabled = { true, false, false };
    bool subOscEnabled = false;
    bool noiseEnabled = false;
    float noiseLevel = 0.0f;

    // Filter configuration
    FilterModel filterModel = FilterModel::SVF;
    FilterModel filter2Model = FilterModel::Bypass;
    FilterRouting filterRouting = FilterRouting::Single;
    float filterCutoff = 20000.0f;
    float filterResonance = 0.707f;
    float filterEnvAmount = 0.0f;
    float filterDrive = 0.0f;

    // Voice configuration
    VoiceMode voiceMode = VoiceMode::Poly;
    int voiceCount = 8;
    bool legato = false;
    float glideTime = 0.0f;
    bool unisonEnabled = false;
    int unisonCount = 1;
    float unisonSpread = 0.0f;

    // Analog character
    bool analogEnabled = true;
    float driftAmount = 0.15f;

    // Envelope defaults
    Envelope::Params ampEnv = { 0.01f, 0.3f, 0.7f, 0.5f };
    Envelope::Params filterEnv = { 0.01f, 0.5f, 0.0f, 0.3f };
    bool envLooping = false;

    // Default mod routings
    std::vector<ModRouting> defaultMods;

    // What the mod strip does in this architecture: LFO cells written by WheelRoute.
    ParamMap wheelRoute;

    static ArchitecturePreset getMonoVA()
    {
        ArchitecturePreset p;
        p.id = ArchitecturePresetId::MonoVA;
        p.name = "Mono VA";
        p.description = "Classic mono/duo virtual analog with ladder filter";

        p.oscCount = 3;
        p.oscWaveforms = { UnifiedVoice::OscWaveform::Saw,
                           UnifiedVoice::OscWaveform::Square,
                           UnifiedVoice::OscWaveform::Sine };
        p.oscLevels = { 0.8f, 0.6f, 0.4f }; // osc1, osc2, sub
        p.oscDetune = { 0.0f, 0.0f, -12.0f }; // sub is 1 octave down
        p.oscEnabled = { true, true, true };
        p.subOscEnabled = true;
        p.noiseEnabled = true;
        p.noiseLevel = 0.0f;

        p.filterModel = FilterModel::Ladder;
        p.filter2Model = FilterModel::Bypass;
        p.filterRouting = FilterRouting::Single;
        p.filterCutoff = 2000.0f;
        p.filterResonance = 0.3f;
        p.filterEnvAmount = 4000.0f;
        p.filterDrive = 0.2f;

        p.voiceMode = VoiceMode::Mono;
        p.voiceCount = 1;
        p.legato = true;
        p.glideTime = 0.05f;

        p.analogEnabled = true;
        p.driftAmount = 0.2f;

        p.ampEnv = { 0.002f, 0.4f, 0.8f, 0.3f };
        p.filterEnv = { 0.005f, 0.6f, 0.2f, 0.4f };
        p.envLooping = false;


        p.defaultMods = {
            { 0, 10, 0.3f },   // LFO1 → filter cutoff
            { 1, 5, 0.1f }     // LFO2 → osc2 detune (vibrato)
        };
        // Strip: the ladder opens two octaves and vibrato comes in.
        WheelRoute::vibrato(p.wheelRoute, 0, 5.5f, 0.35f);
        WheelRoute::to(p.wheelRoute, 1, UnifiedVoice::LfoDest::Cutoff, 0.5f);

        return p;
    }

    static ArchitecturePreset getHybridAggressive()
    {
        ArchitecturePreset p;
        p.id = ArchitecturePresetId::HybridAggressive;
        p.name = "Hybrid Aggressive";
        p.description = "Digital + analog oscillators with diode filter and cross-mod";

        p.oscCount = 3;
        p.oscWaveforms = { UnifiedVoice::OscWaveform::Pulse,   // digital wavetable-ish
                           UnifiedVoice::OscWaveform::Saw,     // analog 1
                           UnifiedVoice::OscWaveform::Square }; // analog 2
        p.oscLevels = { 0.7f, 0.7f, 0.5f };
        p.oscDetune = { 0.0f, 0.0f, 7.0f }; // osc3 detuned a fifth
        p.oscEnabled = { true, true, true };
        p.subOscEnabled = false;
        p.noiseEnabled = false;

        p.filterModel = FilterModel::Diode;
        p.filter2Model = FilterModel::Bypass;
        p.filterRouting = FilterRouting::Single;
        p.filterCutoff = 3000.0f;
        p.filterResonance = 0.4f;
        p.filterEnvAmount = 5000.0f;
        p.filterDrive = 0.5f;

        p.voiceMode = VoiceMode::Mono;
        p.voiceCount = 1;
        p.legato = false;
        p.glideTime = 0.0f;

        p.analogEnabled = true;
        p.driftAmount = 0.1f;

        p.ampEnv = { 0.001f, 0.2f, 0.9f, 0.2f };
        p.filterEnv = { 0.001f, 0.4f, 0.3f, 0.3f };


        p.defaultMods = {
            { 0, 10, 0.5f },   // LFO1 → filter cutoff (aggressive sweep)
            { 2, 4, 0.2f }     // Env3 → cross-mod depth
        };
        // Strip: cross-modulation and the diode filter opening.
        WheelRoute::to(p.wheelRoute, 0, UnifiedVoice::LfoDest::CrossMod, 0.6f);
        WheelRoute::to(p.wheelRoute, 1, UnifiedVoice::LfoDest::Cutoff, 0.35f);

        return p;
    }

    static ArchitecturePreset getVirtualPolysynth()
    {
        ArchitecturePreset p;
        p.id = ArchitecturePresetId::VirtualPolysynth;
        p.name = "Virtual Polysynth";
        p.description = "Polyphonic with SEM SVF, unison spread, and per-voice drift";

        p.oscCount = 2;
        p.oscWaveforms = { UnifiedVoice::OscWaveform::Saw,
                           UnifiedVoice::OscWaveform::Square,
                           UnifiedVoice::OscWaveform::Sine };
        p.oscLevels = { 0.7f, 0.5f, 0.0f };
        p.oscDetune = { 0.0f, 0.1f, 0.0f };
        p.oscEnabled = { true, true, false };
        p.subOscEnabled = false;
        p.noiseEnabled = false;

        p.filterModel = FilterModel::SEM;
        p.filter2Model = FilterModel::Bypass;
        p.filterRouting = FilterRouting::Single;
        p.filterCutoff = 8000.0f;
        p.filterResonance = 0.5f;
        p.filterEnvAmount = 3000.0f;
        p.filterDrive = 0.0f;

        p.voiceMode = VoiceMode::Poly;
        p.voiceCount = 5;
        p.legato = false;
        p.glideTime = 0.0f;
        p.unisonEnabled = true;
        p.unisonCount = 3;
        p.unisonSpread = 0.15f;

        p.analogEnabled = true;
        p.driftAmount = 0.25f;

        p.ampEnv = { 0.01f, 0.5f, 0.6f, 0.8f };
        p.filterEnv = { 0.02f, 0.7f, 0.3f, 0.6f };


        p.defaultMods = {
            { 0, 10, 0.2f },    // LFO1 → filter cutoff (gentle)
            { 1, 5, 0.05f }     // LFO2 → osc2 detune (subtle motion)
        };
        // Strip: slow pulse-width motion and a gentle opening of the filter.
        WheelRoute::motion(p.wheelRoute, 0, 0.4f, LFO::Shape::Sine, UnifiedVoice::LfoDest::PulseWidth, 0.7f);
        WheelRoute::to(p.wheelRoute, 1, UnifiedVoice::LfoDest::Cutoff, 0.3f);

        return p;
    }

    static ArchitecturePreset get(ArchitecturePresetId presetId)
    {
        switch (presetId)
        {
            case ArchitecturePresetId::MonoVA: return getMonoVA();
            case ArchitecturePresetId::HybridAggressive: return getHybridAggressive();
            case ArchitecturePresetId::VirtualPolysynth: return getVirtualPolysynth();
        }
        return getMonoVA();
    }

    static constexpr int PRESET_COUNT = 3;

    static juce::String getPresetName(int index)
    {
        switch (index)
        {
            case 0: return "Mono VA";
            case 1: return "Hybrid Aggressive";
            case 2: return "Virtual Polysynth";
            default: return "Unknown";
        }
    }

    static ArchitecturePresetId getPresetId(int index)
    {
        switch (index)
        {
            case 0: return ArchitecturePresetId::MonoVA;
            case 1: return ArchitecturePresetId::HybridAggressive;
            case 2: return ArchitecturePresetId::VirtualPolysynth;
            default: return ArchitecturePresetId::MonoVA;
        }
    }
};

inline const ArchitecturePreset& getArchitecturePreset(ArchitecturePresetId id)
{
    static const ArchitecturePreset presets[] = {
        ArchitecturePreset::getMonoVA(),
        ArchitecturePreset::getHybridAggressive(),
        ArchitecturePreset::getVirtualPolysynth()
    };
    return presets[juce::jlimit(0, 2, (int)id)];
}
