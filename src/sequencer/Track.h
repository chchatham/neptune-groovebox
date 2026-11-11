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

    Step& getStep(int index)
    {
        return steps[index % length];
    }

    const Step& getStep(int index) const
    {
        return steps[index % length];
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
            int idx = (fromStart + i) % length;
            buffer.push_back(steps[(size_t) idx]);
        }
    }

    void pasteStepRange(int toStart, const std::vector<Step>& buffer)
    {
        for (int i = 0; i < (int) buffer.size(); ++i)
        {
            int idx = (toStart + i) % length;
            steps[(size_t) idx] = buffer[(size_t) i];
        }
    }
};
