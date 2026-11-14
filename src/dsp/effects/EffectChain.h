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

        fx->setChained();
        effects.push_back(std::move(fx));
    }

    // Swap the effect in a slot, keeping its position in the chain.
    void replaceEffect(int index, std::unique_ptr<EffectBase> fx)
    {
        if (index < 0 || index >= (int) effects.size() || !fx)
            return;
        if (currentSampleRate > 0.0 && currentBlockSize > 0)
            fx->prepare(currentSampleRate, currentBlockSize);
        fx->setChained();
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
            // Switched off or on under sound: the effect is faded out or in over 3 ms (a cut
            // from the effect's output to its input, or back, is a click).
            const float target = fx->isBypassed() ? 0.0f : 1.0f;
            float presence = fx->presence < 0.0f ? target : fx->presence;
            if (presence == target || (int) scratchL.size() < numSamples || currentSampleRate <= 0.0)
            {
                fx->presence = target;
                fx->hold(target > 0.0f);
                if (target > 0.0f)
                    fx->processStereo(left, right, numSamples);
                continue;
            }
            fx->hold(true);         // it runs on while it fades
            std::memcpy(scratchL.data(), left, sizeof(float) * (size_t) numSamples);
            std::memcpy(scratchR.data(), right, sizeof(float) * (size_t) numSamples);
            fx->processStereo(scratchL.data(), scratchR.data(), numSamples);
            const float step = (float) (1.0 / (0.003 * currentSampleRate));
            for (int i = 0; i < numSamples; ++i)
            {
                const float d = target - presence;
                presence = std::abs(d) <= step ? target : presence + (d > 0.0f ? step : -step);
                left[i] += presence * (scratchL[(size_t) i] - left[i]);
                right[i] += presence * (scratchR[(size_t) i] - right[i]);
            }
            fx->presence = presence;
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
            fx->hold(! fx->isBypassed());
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
        effects[0]->hold(! effects[0]->isBypassed());
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
            effects[idx]->hold(! effects[idx]->isBypassed());
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
