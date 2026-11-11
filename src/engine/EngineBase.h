#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <map>
#include "ParamSpec.h"
#include "AftertouchRouter.h"

using ParamId = int;
using ParamMap = std::map<ParamId, float>;

enum class EngineType
{
    UnifiedVoice,
    FM,
    MacroOsc,
    Wavetable,
    WaveSeq,
    Sampler,
    Drum,
    MIDITrack
};

// Modulation parameter ids (LFOs and extra envelopes) of engines that support the MOD pages.
//   40..63    LFO 1-4, six parameters each (LFO_BASE + lfo * LFO_STRIDE + offset)
//   64..87    extra envelopes 1-4, six each
//   88, 89    how many LFOs / extra envelopes are running
//   130..145  LFO 1-4, four more parameters each (LFO_EXT_BASE + lfo * LFO_EXT_STRIDE + offset)
//   174..189  LFO 1-4, performance controls (LFO_EXT2_BASE + lfo * LFO_EXT2_STRIDE + offset)
// 146..149 are free. Values that are indices (shape, trigger mode, division, destination) are
// stored in projects, so the lists behind them only ever grow at the end.
namespace ModParam
{
    static constexpr int LFO_BASE = 40;       // + lfoIdx*STRIDE + offset
    static constexpr int LFO_STRIDE = 6;
    static constexpr int ENV_BASE = 64;       // + envIdx*STRIDE + offset
    static constexpr int ENV_STRIDE = 6;
    static constexpr int LFO_COUNT = 88;
    static constexpr int ENV_COUNT = 89;
    static constexpr int LFO_EXT_BASE = 130;  // + lfoIdx*EXT_STRIDE + ext offset
    static constexpr int LFO_EXT_STRIDE = 4;
    static constexpr int LFO_EXT_END = 146;   // one past the last id
    static constexpr int LFO_EXT2_BASE = 174; // + lfoIdx*EXT2_STRIDE + ext2 offset
    static constexpr int LFO_EXT2_STRIDE = 4;
    static constexpr int LFO_EXT2_END = 190;  // one past the last id
    static constexpr int MAX_LFOS = 4;
    static constexpr int MAX_ENVS = 4;

    // Per-LFO offsets
    static constexpr int LFO_RATE = 0;        // Hz, used while SYNC is off
    static constexpr int LFO_DEPTH = 1;       // -1..1, negative inverts
    static constexpr int LFO_SHAPE = 2;       // LFO::Shape index
    static constexpr int LFO_TRIG = 3;        // LFO::TriggerMode index
    static constexpr int LFO_SYNC = 4;        // 0 / 1
    static constexpr int LFO_DEST = 5;        // destination index (engine specific list)

    // Per-LFO extended offsets (with lfoExtParamId)
    static constexpr int LFO_DIV = 0;         // LFO division index, used while SYNC is on
    static constexpr int LFO_PHASE = 1;       // start phase, 0..1 of a cycle
    static constexpr int LFO_DELAY = 2;       // seconds before the LFO starts after a note-on
    static constexpr int LFO_FADE = 3;        // seconds of fade-in after the delay

    // Per-LFO performance offsets (with lfoExt2ParamId), all -1..1, 0 = off.
    // Depth used = clamp(DEPTH + WHEEL * wheel + VEL * velocity + PRES * pressure, -1, 1).
    static constexpr int LFO_WHEEL = 0;       // mod strip (0..1) -> depth
    static constexpr int LFO_VEL = 1;         // note velocity (0..1, taken at note-on) -> depth
    static constexpr int LFO_KTRK = 2;        // key -> rate: x 2^(KTRK * (note - 60) / 12); not while SYNC is on
    static constexpr int LFO_PRES = 3;        // the voice's own pressure (0..1) -> depth

    // Per-ENV offsets
    static constexpr int ENV_ATK = 0;
    static constexpr int ENV_DEC = 1;
    static constexpr int ENV_SUS = 2;
    static constexpr int ENV_REL = 3;
    static constexpr int ENV_DEPTH = 4;
    static constexpr int ENV_TARGET = 5;

    static constexpr int lfoParamId(int lfoIdx, int offset) { return LFO_BASE + lfoIdx * LFO_STRIDE + offset; }
    static constexpr int lfoExtParamId(int lfoIdx, int offset) { return LFO_EXT_BASE + lfoIdx * LFO_EXT_STRIDE + offset; }
    static constexpr int lfoExt2ParamId(int lfoIdx, int offset) { return LFO_EXT2_BASE + lfoIdx * LFO_EXT2_STRIDE + offset; }
    static constexpr int envParamId(int envIdx, int offset) { return ENV_BASE + envIdx * ENV_STRIDE + offset; }

    static constexpr bool isModParam(int id)
    {
        return (id >= LFO_BASE && id <= ENV_COUNT) || (id >= LFO_EXT_BASE && id < LFO_EXT_END)
            || (id >= LFO_EXT2_BASE && id < LFO_EXT2_END);
    }
} // namespace ModParam

class EngineBase
{
public:
    virtual ~EngineBase() = default;

    virtual EngineType getType() const = 0;
    virtual juce::String getName() const = 0;

    virtual void prepareToPlay(double sampleRate, int samplesPerBlock) = 0;
    virtual void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) = 0;
    virtual void releaseResources() {}

    virtual void noteOn(int note, float velocity, int voiceIndex = -1) = 0;
    virtual void noteOff(int note, int voiceIndex = -1) = 0;
    virtual void allNotesOff() = 0;

    virtual void setParameter(ParamId id, float value) = 0;
    virtual float getParameter(ParamId id) const = 0;
    virtual juce::String getParameterName(ParamId id) const = 0;
    virtual int getParameterCount() const = 0;

    virtual void aftertouch(int note, float pressure) { juce::ignoreUnused(note, pressure); }
    // amount is -1..1 (the pitch strip); each engine applies its own bend range.
    virtual void pitchBend(float amount) { juce::ignoreUnused(amount); }
    virtual void modWheel(float value) { juce::ignoreUnused(value); }
    // The mod strip, 0..1, as the app sends it: the engine also remembers the position, so that
    // whoever replaces or reselects the engine can see what it was last given.
    void setModWheel(float value)
    {
        modWheelPosition = juce::jlimit(0.0f, 1.0f, value);
        modWheel(modWheelPosition);
    }
    float getModWheelPosition() const { return modWheelPosition; }
    virtual void setTempo(float bpm) { juce::ignoreUnused(bpm); }

    // The 8-encoder pages of this engine, in order. Every parameter on them must be applied by
    // setParameter and read back by getParameter (which returns the true current value, defaults
    // included).
    virtual ParamPages getParamPages() const { return {}; }

    // A parameter that replaces many others when set (e.g. an architecture preset) and so has to
    // be applied before the rest when a stored sound is loaded; -1 if there is none.
    virtual int getMasterParamId() const { return -1; }

    // Parameters whose change rebuilds data (a sample swap, re-slicing, re-cutting a wavetable):
    // too heavy to run from a step lock or pad pressure, so the screens offer neither for them.
    virtual bool isRebuildParam(ParamId) const { return false; }

    // The stage-curve parameter (StageCurveParam, ParamSpec.h) behind an envelope's ATK, DEC or
    // REL cell: what Shift + turning that cell edits. -1 = the cell is not such a stage.
    virtual int getStageCurveParam(ParamId cellId) const { juce::ignoreUnused(cellId); return -1; }

    // True when the engine implements the ModParam ids (LFOs / extra envelopes), i.e. the MOD page.
    virtual bool supportsModulation() const { return false; }

    // Pressure routing shared by the whole instrument (edited on the AT ROUTE screen).
    void setAftertouchRouter(const AftertouchRouter* router) { aftertouchRouter = router; }

    virtual float getCpuLoad() const { return 0.0f; }

protected:
    const AftertouchRouter* aftertouchRouter = nullptr;

    // The strip arrives in 7-bit steps; an engine that routes it to something heard at that
    // resolution moves its own copy towards the target once per block (about 20 ms to settle).
    static float smoothWheel(float current, float target, int numSamples, double sampleRate)
    {
        if (current == target)
            return current;
        const float k = 1.0f - std::exp(-(float) numSamples / (0.02f * (float) std::max(sampleRate, 1.0)));
        const float next = current + (target - current) * k;
        return std::abs(target - next) < 1.0e-4f ? target : next;
    }

private:
    float modWheelPosition = 0.0f;

};
