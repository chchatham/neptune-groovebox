#pragma once

#include "Step.h"
#include "../engine/EngineBase.h"
#include <juce_core/juce_core.h>
#include <vector>

struct Track
{
    EngineType engineType = EngineType::UnifiedVoice;
    ParamMap soundParams;
    std::vector<Step> steps;
    int length = 16;
    float speedMultiplier = 1.0f;
    int midiChannel = 1;
    bool muted = false;
    bool soloed = false;

    Track()
    {
        steps.resize(16);
    }

    void setLength(int newLength)
    {
        length = juce::jlimit(1, 1024, newLength);
        if ((int) steps.size() < length)
            steps.resize(length);
    }

    // True if a step at or after `from` holds anything (a trig, notes or locks).
    bool anythingFrom(int from) const
    {
        for (size_t i = (size_t) juce::jmax(0, from); i < steps.size(); ++i)
            if (steps[i].trig || !steps[i].notes.empty() || !steps[i].paramLocks.empty())
                return true;
        return false;
    }

    // Longer, with the first `period` steps looping through the new ones (step i = step i - period).
    // The new steps are written before the length takes them in.
    void lengthenLooping(int newLength, int period)
    {
        newLength = juce::jlimit(1, 1024, newLength);
        period = juce::jmax(1, period);
        if ((int) steps.size() < newLength)
            steps.resize((size_t) newLength);
        for (int i = juce::jmax(length, period); i < newLength; ++i)
            steps[(size_t) i] = steps[(size_t) (i - period)];
        length = newLength;
    }

    // A step for ANY index: wrapped into the track's length, which is held to the steps there are
    // (length and steps are separate fields; a length set by hand, a negative index or a length
    // of 0 once read outside the vector or divided by zero).
    int stepIndex(int index) const
    {
        const int n = juce::jmax(1, juce::jmin(length, (int) steps.size()));
        const int i = index % n;
        return i < 0 ? i + n : i;
    }

    Step& getStep(int index)
    {
        return steps[(size_t) stepIndex(index)];
    }

    const Step& getStep(int index) const
    {
        return steps[(size_t) stepIndex(index)];
    }

    void clearAllSteps()
    {
        for (auto& step : steps)
            step.clear();
    }

    void copyStepRange(int fromStart, int count, std::vector<Step>& buffer) const
    {
        buffer.clear();
        for (int i = 0; i < count; ++i)
        {
            buffer.push_back(steps[(size_t) stepIndex(fromStart + i)]);
        }
    }

    void pasteStepRange(int toStart, const std::vector<Step>& buffer)
    {
        for (int i = 0; i < (int) buffer.size(); ++i)
        {
            steps[(size_t) stepIndex(toStart + i)] = buffer[(size_t) i];
        }
    }
};

// The length knob (LN on STEP, on TRK FX): a track made longer plays on as it was, its steps
// looping through the new ones. One turn of the knob - clicks no more than 1.5 s apart, on one
// track - loops the track as it was when the turn began, so 16 turned up to 64 is the 16 four
// times however many clicks it took. A track that already holds something beyond its length
// (it was longer once) is not written over: those steps come back as they were.
struct LengthTurn
{
    static constexpr juce::int64 GAP_MS = 1500;

    void turn(Track& track, int newLength, juce::int64 nowMs)
    {
        if (&track != turning || nowMs - lastMs > GAP_MS || period <= 0)
        {
            turning = &track;
            period = track.length;
            loops = !track.anythingFrom(track.length);
        }
        lastMs = nowMs;
        if (loops && newLength > track.length)
            track.lengthenLooping(newLength, period);
        else
            track.setLength(newLength);
    }

private:
    const Track* turning = nullptr;
    int period = 0;
    bool loops = false;
    juce::int64 lastMs = 0;
};
