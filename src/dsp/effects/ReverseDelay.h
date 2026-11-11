#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>
#include <cmath>
#include <algorithm>

// Reverse delay effect.
// Records input into a buffer, then plays chunks back in reverse.
// Uses two alternating grains with crossfade for seamless reverse playback.
// All buffers pre-allocated. No heap on audio thread.
class ReverseDelay : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "TIME", 0, 0.1f, 2, 0.5f, K::Log, "s" };
            case 1: return { "FDBK", 1, 0, 0.95f, 0.3f };
            case 2: return { "HCUT", 2, 500, 20000, 8000, K::Log, "Hz" };
            case 3: return { "MIX", 3, 0, 1, 0.5f };
            default: return {};
        }
    }

    enum Param
    {
        Time = 0,       // 0.1 - 2.0 seconds (reverse chunk length)
        Feedback,       // 0 - 0.95
        HighCut,        // 500 - 20000 Hz
        Mix             // 0 - 1
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        recalcFilterCoeff();
        reset();
    }

    void reset() override
    {
        bufferL.fill(0.0f);
        bufferR.fill(0.0f);
        writePos = 0;
        grainCounter = 0;
        activeGrain = 0;
        for (auto& g : grains)
        {
            g.readPos = 0;
            g.length = 0;
            g.active = false;
        }
        lpStateL = 0.0f;
        lpStateR = 0.0f;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        int chunkSamples = std::clamp((int)(time * (float)sampleRate), 1, MAX_SAMPLES / 2);

        // Write input (plus feedback) to circular buffer
        bufferL[(size_t)writePos] = left;
        bufferR[(size_t)writePos] = right;

        // Manage grain triggering
        grainCounter++;
        if (grainCounter >= chunkSamples)
        {
            grainCounter = 0;
            // Start a new reverse grain from current write position
            activeGrain = 1 - activeGrain;
            grains[activeGrain].readPos = writePos;
            grains[activeGrain].length = chunkSamples;
            grains[activeGrain].active = true;
            grains[activeGrain].progress = 0;
        }

        float wetL = 0.0f;
        float wetR = 0.0f;

        // Process both grains with crossfade
        for (int g = 0; g < 2; ++g)
        {
            if (!grains[g].active) continue;

            float progress = (float)grains[g].progress / (float)grains[g].length;
            if (progress >= 1.0f)
            {
                grains[g].active = false;
                continue;
            }

            // Reverse read: start from grain's start position, go backwards
            int reverseOffset = (int)((1.0f - progress) * (float)grains[g].length);
            int readIdx = grains[g].readPos - (grains[g].length - reverseOffset);
            if (readIdx < 0) readIdx += MAX_SAMPLES;
            if (readIdx >= MAX_SAMPLES) readIdx -= MAX_SAMPLES;

            float sampleL = bufferL[(size_t)readIdx];
            float sampleR = bufferR[(size_t)readIdx];

            // Crossfade envelope: half-cosine window
            float fade = 1.0f;
            float fadeIn = 0.05f;   // 5% fade in
            float fadeOut = 0.05f;  // 5% fade out
            if (progress < fadeIn)
                fade = 0.5f - 0.5f * std::cos(progress / fadeIn * dsp::PI_F);
            else if (progress > (1.0f - fadeOut))
                fade = 0.5f - 0.5f * std::cos((1.0f - progress) / fadeOut * dsp::PI_F);

            wetL += sampleL * fade;
            wetR += sampleR * fade;

            grains[g].progress++;
        }

        // Filter the wet signal
        lpStateL += cachedLpCoeff * (wetL - lpStateL);
        lpStateR += cachedLpCoeff * (wetR - lpStateR);
        wetL = lpStateL;
        wetR = lpStateR;

        // Write feedback back into buffer
        bufferL[(size_t)writePos] += wetL * feedback;
        bufferR[(size_t)writePos] += wetR * feedback;

        // Clamp buffer to prevent runaway
        bufferL[(size_t)writePos] = std::clamp(bufferL[(size_t)writePos], -4.0f, 4.0f);
        bufferR[(size_t)writePos] = std::clamp(bufferR[(size_t)writePos], -4.0f, 4.0f);

        writePos = (writePos + 1) % MAX_SAMPLES;

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Time:     time     = std::clamp(value, 0.1f, 2.0f);     break;
            case Feedback: feedback = std::clamp(value, 0.0f, 0.95f);    break;
            case HighCut:
                highCut = std::clamp(value, 500.0f, 20000.0f);
                recalcFilterCoeff();
                break;
            case Mix:      mix      = std::clamp(value, 0.0f, 1.0f);     break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Time:     return time;
            case Feedback: return feedback;
            case HighCut:  return highCut;
            case Mix:      return mix;
            default:       return 0.0f;
        }
    }

private:
    static constexpr int MAX_SAMPLES = 96000 * 2; // 2 seconds at 96kHz

    float time     = 0.5f;
    float feedback = 0.3f;
    float highCut  = 8000.0f;
    float cachedLpCoeff = 0.5f;

    struct Grain
    {
        int readPos = 0;
        int length = 0;
        int progress = 0;
        bool active = false;
    };

    std::array<Grain, 2> grains {};
    int activeGrain = 0;
    int grainCounter = 0;

    std::array<float, MAX_SAMPLES> bufferL {};
    std::array<float, MAX_SAMPLES> bufferR {};
    int writePos = 0;
    float lpStateL = 0.0f;
    float lpStateR = 0.0f;

    void recalcFilterCoeff()
    {
        if (sampleRate > 0.0)
            cachedLpCoeff = 1.0f - std::exp(-dsp::TWO_PI_F * highCut / (float)sampleRate);
    }
};
