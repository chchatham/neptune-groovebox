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
