#pragma once

#include "EffectBase.h"
#include <vector>
#include <memory>
#include <algorithm>
#include <cstring>

/**
 * EffectChain — ordered chain of effects with configurable routing.
 *
 * Routing modes:
 *   Series   — each effect feeds the next in sequence.
 *   Parallel — each effect processes a copy of the dry input; outputs are summed.
 *   Mixed    — first effect in series, then remaining effects in parallel (summed).
 *
 * All internal buffers are pre-allocated in prepare(). No heap allocation on the audio thread.
 */
class EffectChain
{
public:
    enum class Routing { Series, Parallel, Mixed };

    EffectChain() = default;

    void prepare(double sampleRate, int blockSize)
    {
        currentSampleRate = sampleRate;
        currentBlockSize = blockSize;

        // Pre-allocate scratch buffers for parallel routing
        // Two scratch buffers per effect slot (L + R) plus accumulator
        scratchL.resize((size_t) blockSize);
        scratchR.resize((size_t) blockSize);
        accumL.resize((size_t) blockSize);
        accumR.resize((size_t) blockSize);

        for (auto& fx : effects)
            fx->prepare(sampleRate, blockSize);
    }

    void processStereo(float* left, float* right, int numSamples)
    {
        if (effects.empty())
            return;

        switch (routing)
        {
            case Routing::Series:
                processSeriesRouting(left, right, numSamples);
                break;
            case Routing::Parallel:
                processParallelRouting(left, right, numSamples);
                break;
            case Routing::Mixed:
                processMixedRouting(left, right, numSamples);
                break;
        }
    }

    void addEffect(std::unique_ptr<EffectBase> fx)
    {
        if (currentSampleRate > 0.0 && currentBlockSize > 0)
            fx->prepare(currentSampleRate, currentBlockSize);

        effects.push_back(std::move(fx));
    }

    // Swap the effect in a slot, keeping its position in the chain.
    void replaceEffect(int index, std::unique_ptr<EffectBase> fx)
    {
        if (index < 0 || index >= (int) effects.size() || !fx)
            return;
        if (currentSampleRate > 0.0 && currentBlockSize > 0)
            fx->prepare(currentSampleRate, currentBlockSize);
        effects[(size_t) index] = std::move(fx);
    }

    void removeEffect(int index)
    {
        if (index >= 0 && index < (int) effects.size())
            effects.erase(effects.begin() + index);
    }

    void setRouting(Routing r) { routing = r; }
    Routing getRouting() const { return routing; }

    int getEffectCount() const { return (int) effects.size(); }

    EffectBase* getEffect(int index)
    {
        if (index >= 0 && index < (int) effects.size())
            return effects[(size_t) index].get();
        return nullptr;
    }

    const EffectBase* getEffect(int index) const
    {
        if (index >= 0 && index < (int) effects.size())
            return effects[(size_t) index].get();
        return nullptr;
    }

    void reset()
    {
        for (auto& fx : effects)
            fx->reset();
    }

private:
    void processSeriesRouting(float* left, float* right, int numSamples)
    {
        for (auto& fx : effects)
        {
            if (!fx->isBypassed())
                fx->processStereo(left, right, numSamples);
        }
    }

    void processParallelRouting(float* left, float* right, int numSamples)
    {
        // Clear accumulator
        std::memset(accumL.data(), 0, sizeof(float) * (size_t) numSamples);
        std::memset(accumR.data(), 0, sizeof(float) * (size_t) numSamples);

        int activeCount = 0;

        for (auto& fx : effects)
        {
            if (fx->isBypassed())
                continue;

            // Copy dry input into scratch
            std::memcpy(scratchL.data(), left, sizeof(float) * (size_t) numSamples);
            std::memcpy(scratchR.data(), right, sizeof(float) * (size_t) numSamples);

            // Process the copy
            fx->processStereo(scratchL.data(), scratchR.data(), numSamples);

            // Sum into accumulator
            for (int i = 0; i < numSamples; ++i)
            {
                accumL[(size_t) i] += scratchL[(size_t) i];
                accumR[(size_t) i] += scratchR[(size_t) i];
            }

            ++activeCount;
        }

        if (activeCount > 0)
        {
            // Copy accumulated result back
            std::memcpy(left, accumL.data(), sizeof(float) * (size_t) numSamples);
            std::memcpy(right, accumR.data(), sizeof(float) * (size_t) numSamples);
        }
        // If all bypassed, input passes through unchanged
    }

    void processMixedRouting(float* left, float* right, int numSamples)
    {
        if (effects.empty())
            return;

        // First effect processes in series (in place)
        if (!effects[0]->isBypassed())
            effects[0]->processStereo(left, right, numSamples);

        if (effects.size() == 1)
            return;

        // Remaining effects process in parallel from the series output
        std::memset(accumL.data(), 0, sizeof(float) * (size_t) numSamples);
        std::memset(accumR.data(), 0, sizeof(float) * (size_t) numSamples);

        int activeCount = 0;

        for (size_t idx = 1; idx < effects.size(); ++idx)
        {
            if (effects[idx]->isBypassed())
                continue;

            std::memcpy(scratchL.data(), left, sizeof(float) * (size_t) numSamples);
            std::memcpy(scratchR.data(), right, sizeof(float) * (size_t) numSamples);

            effects[idx]->processStereo(scratchL.data(), scratchR.data(), numSamples);

            for (int i = 0; i < numSamples; ++i)
            {
                accumL[(size_t) i] += scratchL[(size_t) i];
                accumR[(size_t) i] += scratchR[(size_t) i];
            }

            ++activeCount;
        }

        if (activeCount > 0)
        {
            std::memcpy(left, accumL.data(), sizeof(float) * (size_t) numSamples);
            std::memcpy(right, accumR.data(), sizeof(float) * (size_t) numSamples);
        }
        // If all remaining are bypassed, series output passes through
    }

    std::vector<std::unique_ptr<EffectBase>> effects;
    Routing routing = Routing::Series;

    double currentSampleRate = 0.0;
    int currentBlockSize = 0;

    // Pre-allocated scratch buffers (sized in prepare())
    std::vector<float> scratchL, scratchR;
    std::vector<float> accumL, accumR;
};
