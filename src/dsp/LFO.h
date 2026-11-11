#pragma once

#include "DSPUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// -------------------------------------------------------------------------
// LFO: one low-frequency oscillator, stepped once per sample.
//
// Polarity. Each shape has a natural polarity, and the depth is signed:
//   Sine, Triangle, Square, Saw, Random, SmoothRandom, RampDown
//       bipolar, -1..+1 (they swing around the destination's set value);
//   ExpUp, ExpDown
//       unipolar, 0..1. ExpDown starts at 1 and decays to 0, ExpUp is its
//       mirror in time (0, slow at first, then up to 1). A one-shot ExpDown
//       therefore ends at "no modulation", which is what a decay should do.
// process() returns raw * depth * fade, so a negative depth inverts a
// bipolar shape and makes a unipolar one push downwards.
//
// The depth can also be moved by performance controls (setDepthOffset) and
// the Hz rate can follow the keyboard (setKeyTrack / setKeyNote).
//
// Rate. Free: setRate() in Hz. Tempo-synced: one cycle lasts the selected
// division (setSyncDivision) at the tempo given by setTempo; the Hz value is
// kept and comes back when sync is switched off. Sync locks the cycle LENGTH
// to the tempo; where the cycle starts is decided by the trigger mode (use
// KeySync to start it with the note).
//
// Trigger modes (what trigger(), i.e. a note-on, does):
//   Free     nothing: the LFO keeps running
//   KeySync  restarts the cycle
//   Single   restarts, runs exactly one cycle, then stops and holds the
//            value at the end of the cycle
//   Half     as Single but stops after half a cycle
//   Hold     the LFO keeps running in the background; its value at the
//            moment of the trigger is held until the next trigger
//
// Start phase is an offset into the cycle (0..1), applied in every mode, so
// a restarted cycle begins there and free LFOs can be set apart from each
// other. The random shapes ignore it.
//
// Delay and fade-in restart at every trigger, in every mode: the output is
// zero for the delay time and then rises linearly over the fade time. In the
// restarting modes (KeySync, Single, Half) the cycle waits for the delay to
// pass, so a delayed one-shot still plays its whole cycle.
//
// All times are in seconds / Hz and scaled by setSampleRate, nothing here
// allocates or locks.
// -------------------------------------------------------------------------
class LFO
{
public:
    // Stored in projects by index: only ever append.
    enum class Shape
    {
        Sine,
        Triangle,
        Square,
        Saw,            // ramp up
        Random,         // a new value every cycle
        SmoothRandom,   // glides from one random value to the next
        RampDown,
        ExpUp,
        ExpDown,
        COUNT   // sentinel for shape cycling
    };

    // Stored in projects by index: only ever append.
    enum class TriggerMode
    {
        Free,
        KeySync,
        Single,
        Half,
        Hold,
        COUNT
    };

    // Tempo-sync divisions, longest first (so a higher index is faster, like a higher rate).
    // A bar is four beats. Stored in projects by index: only ever append.
    static constexpr int NUM_DIVISIONS = 17;
    static constexpr int DEFAULT_DIVISION = 8;     // 1/4

    static const char* const* divisionLabels()
    {
        static const char* const names[NUM_DIVISIONS] = {
            "8BAR", "4BAR", "2BAR", "1BAR", "1/2D", "1/2", "1/4D", "1/2T", "1/4",
            "1/8D", "1/4T", "1/8", "1/16D", "1/8T", "1/16", "1/16T", "1/32" };
        return names;
    }

    // Length of one cycle in beats (quarter notes).
    static float divisionBeats(int index)
    {
        static const float beats[NUM_DIVISIONS] = {
            32.0f, 16.0f, 8.0f, 4.0f, 3.0f, 2.0f, 1.5f, 4.0f / 3.0f, 1.0f,
            0.75f, 2.0f / 3.0f, 0.5f, 0.375f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f };
        return beats[std::clamp(index, 0, NUM_DIVISIONS - 1)];
    }

    static const char* divisionName(int index)
    {
        return divisionLabels()[std::clamp(index, 0, NUM_DIVISIONS - 1)];
    }

    // Cell texts (up to four characters), in enum order.
    static const char* const* shapeLabels()
    {
        static const char* const names[(int) Shape::COUNT] = {
            "SIN", "TRI", "SQR", "SAW", "RND", "SRND", "RAMP", "EXPU", "EXPD" };
        return names;
    }

    static const char* const* triggerLabels()
    {
        static const char* const names[(int) TriggerMode::COUNT] = { "FREE", "KEY", "ONE", "HALF", "HOLD" };
        return names;
    }

    static bool isUnipolar(Shape s) { return s == Shape::ExpUp || s == Shape::ExpDown; }

    void setSampleRate(double sr) { sampleRate = sr > 0.0 ? sr : 44100.0; }

    void setRate(float hz) { rate = hz; updateBaseHz(); }
    float getRate() const { return rate; }
    void setShape(Shape s) { shape = s; }
    Shape getShape() const { return shape; }
    void setDepth(float d) { depth = d; }       // -1..1; negative inverts
    float getDepth() const { return depth; }

    // Changing the mode lets a stopped or held LFO run again; setting the same mode does nothing.
    void setTriggerMode(TriggerMode m)
    {
        if (m == triggerMode)
            return;
        triggerMode = m;
        stopped = false;
        oneShotArmed = false;
        holding = false;
        if (phase >= 1.0)
            phase = 0.0;
    }
    TriggerMode getTriggerMode() const { return triggerMode; }

    void setTempoSync(bool sync) { tempoSynced = sync; updateBaseHz(); }
    bool isTempoSynced() const { return tempoSynced; }
    void setTempo(float bpm) { tempoBpm = bpm > 1.0f ? bpm : 1.0f; updateBaseHz(); }
    float getTempo() const { return tempoBpm; }
    void setSyncDivision(int index) { division = std::clamp(index, 0, NUM_DIVISIONS - 1); updateBaseHz(); }
    int getSyncDivision() const { return division; }

    // Cycles per second right now (Hz value or tempo division), before rate modulation.
    float getEffectiveRate() const { return baseHz; }

    void setStartPhase(float cycles) { startPhase = (double) cycles - std::floor((double) cycles); }   // 0..1
    float getStartPhase() const { return (float) startPhase; }
    void setDelayTime(float seconds) { delayTime = std::max(0.0f, seconds); }
    float getDelayTime() const { return (float) delayTime; }
    void setFadeTime(float seconds) { fadeTime = std::max(0.0f, seconds); }
    float getFadeTime() const { return (float) fadeTime; }

    // Modulation of this LFO by something else (another LFO): rate in octaves, depth as a
    // factor offset (depth * clamp(1 + amount, 0, 2)). Zero switches them off.
    void setRateMod(float octaves)
    {
        if (octaves == rateModOct)
            return;
        rateModOct = octaves;
        rateScale = octaves == 0.0f ? 1.0f : std::exp2(std::clamp(octaves, -6.0f, 6.0f));
    }
    void setDepthMod(float amount) { depthScale = std::clamp(1.0f + amount, 0.0f, 2.0f); }

    // Performance control of the depth (mod wheel, velocity, pressure, each already multiplied by
    // its amount): the depth becomes clamp(depth + offset, -1, 1). Zero leaves the depth alone.
    void setDepthOffset(float offset) { depthOffset = offset; }
    float getEffectiveDepth() const { return std::clamp(depth + depthOffset, -1.0f, 1.0f) * depthScale; }

    // Key tracking of the rate: the Hz rate is multiplied by 2^(amount * (note - 60) / 12), so at
    // amount 1 it doubles with every octave above middle C. Ignored while tempo-synced.
    void setKeyTrack(float amount) { keyTrack = amount; updateBaseHz(); }
    float getKeyTrack() const { return keyTrack; }
    void setKeyNote(float note) { keyNote = note; updateBaseHz(); }

    // Cycle shape forward/backward (for Shift+turn gesture on MOD page)
    void cycleShape(int direction)
    {
        int s = static_cast<int>(shape);
        int count = static_cast<int>(Shape::COUNT);
        s += direction;
        if (s < 0) s = count - 1;
        if (s >= count) s = 0;
        shape = static_cast<Shape>(s);
    }

    // Get human-readable shape name (for display)
    static const char* shapeName(Shape s)
    {
        switch (s)
        {
            case Shape::Sine:         return "SIN";
            case Shape::Triangle:     return "TRI";
            case Shape::Square:       return "SQR";
            case Shape::Saw:          return "SAW";
            case Shape::Random:       return "RND";
            case Shape::SmoothRandom: return "SRN";
            case Shape::RampDown:     return "RMP";
            case Shape::ExpUp:        return "EXU";
            case Shape::ExpDown:      return "EXD";
            default:                  return "---";
        }
    }

    void setEnabled(bool e) { enabled = e; }
    bool isEnabled() const { return enabled; }

    // Note-on. See the trigger modes above.
    void trigger()
    {
        fading = delayTime > 0.0 || fadeTime > 0.0;
        sinceTrigger = 0.0;

        switch (triggerMode)
        {
            case TriggerMode::KeySync:
                restartCycle();
                break;
            case TriggerMode::Single:
            case TriggerMode::Half:
                restartCycle();
                oneShotArmed = true;
                break;
            case TriggerMode::Hold:
                heldValue = shapeValue();
                holding = true;
                break;
            default:
                break;
        }
    }

    // True once a one-shot (Single / Half) has finished its cycle.
    bool hasStopped() const { return stopped; }

    float process()
    {
        if (!enabled)
            return 0.0f;

        float gain = 1.0f;
        bool waiting = false;
        if (fading)
        {
            const double t = sinceTrigger;
            sinceTrigger += 1.0 / sampleRate;
            if (t < delayTime)
            {
                gain = 0.0f;
                waiting = triggerMode == TriggerMode::KeySync || oneShotArmed;
            }
            else if (t < delayTime + fadeTime)
                gain = (float) ((t - delayTime) / fadeTime);
            else
                fading = false;
        }

        if (!stopped && !waiting)
            advance((double) (baseHz * rateScale) / sampleRate);

        const float raw = holding ? heldValue : shapeValue();
        lastOutput = raw * gain * getEffectiveDepth();
        return lastOutput;
    }

    // Move the cycle on by a whole block without producing output (a voice that is not sounding
    // keeps its free-running LFOs in time this way).
    void advanceIdle(int numSamples)
    {
        if (enabled && !stopped && numSamples > 0)
            advance((double) (baseHz * rateScale) * (double) numSamples / sampleRate);
    }

    float getCurrentValue() const { return lastOutput; }

    // Position in the cycle, 0..1 (1 = a finished one-shot), without the start-phase offset.
    double getPhase() const { return phase; }

    void reset()
    {
        phase = 0.0;
        lastOutput = 0.0f;
        randomValue = 0.0f;
        prevRandom = 0.0f;
        nextRandom = 0.0f;
        stopped = false;
        oneShotArmed = false;
        holding = false;
        heldValue = 0.0f;
        fading = false;
        sinceTrigger = 0.0;
    }

private:
    void updateBaseHz()
    {
        if (tempoSynced)
            baseHz = tempoBpm / 60.0f / divisionBeats(division);
        else if (keyTrack != 0.0f)
            baseHz = rate * std::exp2(keyTrack * (keyNote - 60.0f) / 12.0f);
        else
            baseHz = rate;
    }

    void restartCycle()
    {
        phase = 0.0;
        stopped = false;
        newCycle();
    }

    void newCycle()
    {
        randomValue = random.nextFloat() * 2.0f - 1.0f;
        prevRandom = nextRandom;
        nextRandom = random.nextFloat() * 2.0f - 1.0f;
    }

    void advance(double inc)
    {
        phase += inc;

        if (oneShotArmed)
        {
            const double end = triggerMode == TriggerMode::Half ? 0.5 : 1.0;
            if (phase >= end)
            {
                phase = end;
                stopped = true;
            }
            return;
        }

        if (phase >= 1.0)
        {
            phase -= std::floor(phase);
            newCycle();
        }
    }

    // exp(-5 p) rescaled to run exactly from 1 (p = 0) to 0 (p = 1).
    static float expDecay(float p)
    {
        constexpr float floorVal = 0.006737947f;     // exp(-5)
        return (std::exp(-5.0f * p) - floorVal) / (1.0f - floorVal);
    }

    // The shape at the current position, in its natural polarity, before depth and fade.
    float shapeValue() const
    {
        // A finished one-shot sits at the END of its cycle (position 1, not wrapped back to 0).
        double pos = phase + startPhase;
        if (stopped ? pos > 1.0 : pos >= 1.0)
            pos -= 1.0;
        const float p = (float) pos;

        switch (shape)
        {
            case Shape::Sine:         return std::sin(p * dsp::TWO_PI_F);
            case Shape::Triangle:     return p < 0.5f ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
            case Shape::Square:       return p < 0.5f ? 1.0f : -1.0f;
            case Shape::Saw:          return 2.0f * p - 1.0f;
            case Shape::RampDown:     return 1.0f - 2.0f * p;
            case Shape::ExpUp:        return expDecay(1.0f - p);
            case Shape::ExpDown:      return expDecay(p);
            case Shape::Random:       return randomValue;
            case Shape::SmoothRandom: return prevRandom + (nextRandom - prevRandom) * (float) std::min(phase, 1.0);
            default:                  return 0.0f;
        }
    }

    double sampleRate = 44100.0;
    double phase = 0.0;
    double startPhase = 0.0;
    float rate = 1.0f;
    float baseHz = 1.0f;
    float depth = 1.0f;
    Shape shape = Shape::Sine;
    TriggerMode triggerMode = TriggerMode::Free;
    bool enabled = true;

    bool tempoSynced = false;
    float tempoBpm = 120.0f;
    int division = DEFAULT_DIVISION;

    float rateModOct = 0.0f;
    float rateScale = 1.0f;
    float depthScale = 1.0f;
    float depthOffset = 0.0f;
    float keyTrack = 0.0f;
    float keyNote = 60.0f;

    // One-shot / hold state
    bool oneShotArmed = false;   // a Single / Half cycle was started by a trigger
    bool stopped = false;        // ... and has reached its end
    bool holding = false;
    float heldValue = 0.0f;

    // Delay / fade-in
    double delayTime = 0.0;
    double fadeTime = 0.0;
    double sinceTrigger = 0.0;
    bool fading = false;

    float lastOutput = 0.0f;
    float randomValue = 0.0f;
    float prevRandom = 0.0f;
    float nextRandom = 0.0f;

    struct SimpleRandom
    {
        uint32_t state = 12345;
        float nextFloat()
        {
            state = state * 1103515245 + 12345;
            return ((state >> 16) & 0x7FFF) / 32767.0f;
        }
    } random;
};

// -------------------------------------------------------------------------
// LFOBank: pre-allocated pool of up to MAX_LFOS per voice.
// Audio-thread safe: fixed-size array, no heap allocation.
// Shift+turn on MOD page encoder cycles through available mod sources.
// -------------------------------------------------------------------------

class LFOBank
{
public:
    static constexpr int MAX_LFOS = 4;

    LFOBank()
    {
        // First LFO enabled by default
        lfos[0].setEnabled(true);
        for (int i = 1; i < MAX_LFOS; ++i)
            lfos[i].setEnabled(false);
        activeCount = 1;
    }

    void setSampleRate(double sr)
    {
        for (auto& lfo : lfos)
            lfo.setSampleRate(sr);
    }

    // Add an LFO. Returns index or -1 if full.
    int addLFO()
    {
        if (activeCount >= MAX_LFOS)
            return -1;
        int idx = activeCount++;
        lfos[idx].setEnabled(true);
        lfos[idx].reset();
        return idx;
    }

    // Remove the last active LFO. Returns new count.
    int removeLFO()
    {
        if (activeCount <= 1)
            return activeCount;
        --activeCount;
        lfos[activeCount].setEnabled(false);
        lfos[activeCount].reset();
        return activeCount;
    }

    int getActiveCount() const { return activeCount; }

    LFO& getLFO(int index)
    {
        return lfos[std::clamp(index, 0, MAX_LFOS - 1)];
    }

    const LFO& getLFO(int index) const
    {
        return lfos[std::clamp(index, 0, MAX_LFOS - 1)];
    }

    void triggerAll()
    {
        for (int i = 0; i < activeCount; ++i)
            lfos[i].trigger();
    }

    void setTempo(float bpm)
    {
        for (auto& lfo : lfos)
            lfo.setTempo(bpm);
    }

    // Keep the cycles moving while the owner is not being processed (see LFO::advanceIdle).
    void advanceIdle(int numSamples)
    {
        for (int i = 0; i < activeCount; ++i)
            lfos[i].advanceIdle(numSamples);
    }

    void resetAll()
    {
        for (auto& lfo : lfos)
            lfo.reset();
    }

    // Process all active LFOs, storing outputs for mod matrix readback
    void processAll()
    {
        for (int i = 0; i < activeCount; ++i)
            outputs[i] = lfos[i].process();
        for (int i = activeCount; i < MAX_LFOS; ++i)
            outputs[i] = 0.0f;
    }

    float getOutput(int index) const
    {
        if (index < 0 || index >= MAX_LFOS)
            return 0.0f;
        return outputs[index];
    }

    // Mod source cycling: given a current source index, cycle to next/prev
    // available source. Sources: LFO1..LFO4, Env1..Env4 (8 total).
    // direction: +1 forward, -1 backward
    static int cycleModSource(int currentSource, int direction, int numLFOs, int numEnvs)
    {
        int totalSources = numLFOs + numEnvs;
        if (totalSources == 0) return 0;
        int next = currentSource + direction;
        if (next < 0) next = totalSources - 1;
        if (next >= totalSources) next = 0;
        return next;
    }

    // Get name for a mod source index (0..3 = LFO1..4, 4..7 = ENV1..4)
    static const char* modSourceName(int sourceIndex)
    {
        switch (sourceIndex)
        {
            case 0: return "LFO1";
            case 1: return "LFO2";
            case 2: return "LFO3";
            case 3: return "LFO4";
            case 4: return "ENV1";
            case 5: return "ENV2";
            case 6: return "ENV3";
            case 7: return "ENV4";
            default: return "----";
        }
    }

private:
    std::array<LFO, MAX_LFOS> lfos;
    std::array<float, MAX_LFOS> outputs{};
    int activeCount = 1;
};
