#pragma once

#include "EffectBase.h"
#include <cmath>
#include <algorithm>
#include <array>

// Brickwall lookahead limiter.
// Uses a pre-allocated circular delay buffer for lookahead (1-5ms).
// Output is guaranteed to never exceed the ceiling.
// No heap allocation in process. All buffers are fixed-size member arrays.
class Limiter : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "CEIL", 0, -3, 0, -0.3f, K::Linear, "dB" };
            case 1: return { "REL", 1, 50, 500, 100, K::Log, "ms" };
            case 2: return { "LOOK", 2, 1, 5, 2, K::Linear, "ms" };
            case 3: return {};
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case 0: setCeiling(value); break;
            case 1: setRelease(value); break;
            case 2: setLookahead(value); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case 0: return ceilingDb;
            case 1: return releaseMs;
            case 2: return lookaheadMs;
            default: return 0.0f;
        }
    }

    Limiter()
    {
        delayBufferL.fill(0.0f);
        delayBufferR.fill(0.0f);
        gainBuffer.fill(1.0f);
    }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        recalcParams();
        reset();
    }

    void reset() override
    {
        delayBufferL.fill(0.0f);
        delayBufferR.fill(0.0f);
        gainBuffer.fill(1.0f);
        writePos = 0;
        currentGain = 1.0f;
    }

    // Ceiling in dB (-3 to 0)
    void setCeiling(float db)
    {
        ceilingDb = std::clamp(db, -3.0f, 0.0f);
        ceilingLinear = std::pow(10.0f, ceilingDb / 20.0f);
    }

    // Lookahead time in ms (1-5)
    void setLookahead(float ms)
    {
        lookaheadMs = std::clamp(ms, 1.0f, 5.0f);
        recalcParams();
    }

    // Release time in ms (50-500)
    void setRelease(float ms)
    {
        releaseMs = std::clamp(ms, 50.0f, 500.0f);
        recalcParams();
    }

    float getCeiling() const { return ceilingDb; }
    float getLookahead() const { return lookaheadMs; }
    float getRelease() const { return releaseMs; }

    // Current gain reduction in dB (for metering)
    float getGainReductionDb() const
    {
        if (currentGain <= 0.000001f) return -120.0f;
        return 20.0f * std::log10(currentGain);
    }

    void process(float& left, float& right) override
    {
        if (!enabled)
            return;

        // Write input to delay buffer
        delayBufferL[writePos] = left;
        delayBufferR[writePos] = right;

        // Detect peak of current input
        float peak = std::max(std::abs(left), std::abs(right));

        // Compute needed gain for this sample
        float neededGain = 1.0f;
        if (peak > ceilingLinear)
            neededGain = ceilingLinear / peak;

        // Write the gain target into the gain buffer at the current write position.
        // We apply gain reduction ahead of time using the lookahead window.
        gainBuffer[writePos] = neededGain;

        // Find the minimum gain in the lookahead window.
        // This ensures we start reducing gain before the peak arrives.
        float minGain = 1.0f;
        for (int i = 0; i < lookaheadSamples; ++i)
        {
            int idx = (writePos - i + MAX_DELAY) % MAX_DELAY;
            minGain = std::min(minGain, gainBuffer[idx]);
        }

        // Smooth gain transitions
        if (minGain < currentGain)
        {
            // Attack: fast (instant within lookahead)
            currentGain = minGain;
        }
        else
        {
            // Release: slow exponential
            currentGain = releaseCoeff * currentGain + (1.0f - releaseCoeff) * minGain;
        }

        // Clamp gain to never exceed 1
        currentGain = std::min(currentGain, 1.0f);

        // Read from delayed position
        int readPos = (writePos - lookaheadSamples + MAX_DELAY) % MAX_DELAY;
        left = delayBufferL[readPos] * currentGain;
        right = delayBufferR[readPos] * currentGain;

        // Final hard clip safety net (should never activate if logic is correct)
        left = std::clamp(left, -ceilingLinear, ceilingLinear);
        right = std::clamp(right, -ceilingLinear, ceilingLinear);

        writePos = (writePos + 1) % MAX_DELAY;
    }

private:
    // Max 5ms at 192kHz = 960 samples. Round up to power of 2 for safety.
    static constexpr int MAX_DELAY = 1024;

    // Parameters
    float ceilingDb = -0.3f;
    float ceilingLinear = 0.966f;  // ~-0.3 dB
    float lookaheadMs = 3.0f;
    float releaseMs = 200.0f;

    // Derived
    int lookaheadSamples = 132;    // 3ms at 44100
    float releaseCoeff = 0.0f;

    // Pre-allocated buffers (no heap in process)
    std::array<float, MAX_DELAY> delayBufferL {};
    std::array<float, MAX_DELAY> delayBufferR {};
    std::array<float, MAX_DELAY> gainBuffer {};

    // State
    int writePos = 0;
    float currentGain = 1.0f;

    void recalcParams()
    {
        if (sampleRate <= 0.0) return;
        lookaheadSamples = std::clamp(
            (int)(lookaheadMs * 0.001f * (float)sampleRate),
            1, MAX_DELAY - 1);
        releaseCoeff = std::exp(-1.0f / (float)(releaseMs * 0.001 * sampleRate));
    }
};
