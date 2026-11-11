#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <cmath>
#include <algorithm>

// Sample rate reduction + bit depth reduction effect.
// Provides independent control over sample rate decimation (1x-64x)
// and bit depth quantization (16-bit down to 1-bit).
// Includes a pre-filter to control aliasing character.
// No buffers needed - pure computation with sample-hold state.
// No heap on audio thread.
class RateReducer : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "RATE", 0, 0, 1, 0.3f };
            case 1: return { "BITS", 1, 0, 1, 0 };
            case 2: return { "FILT", 2, 0, 1, 0 };
            case 3: return { "MIX", 3, 0, 1, 1 };
            default: return {};
        }
    }

    enum Param
    {
        RateReduction = 0,  // 0 - 1 (maps to 1x - 64x decimation)
        BitDepth,           // 0 - 1 (maps to 16-bit down to 1-bit)
        PreFilter,          // 0 - 1 (anti-aliasing filter amount)
        Mix                 // 0 - 1
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        recalcFilterCoeff();
        reset();
    }

    void reset() override
    {
        holdL = 0.0f;
        holdR = 0.0f;
        holdCounter = 0.0f;
        preFilterL = 0.0f;
        preFilterR = 0.0f;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        float inL = left;
        float inR = right;

        // Pre-filter: lowpass to control aliasing character
        // At preFilter=1, aggressive filtering before decimation (clean)
        // At preFilter=0, no filtering (harsh aliasing, lo-fi character)
        if (preFilter > 0.001f)
        {
            preFilterL += cachedPreFilterCoeff * (inL - preFilterL);
            preFilterR += cachedPreFilterCoeff * (inR - preFilterR);
            inL = lerp(inL, preFilterL, preFilter);
            inR = lerp(inR, preFilterR, preFilter);
        }

        // Sample rate reduction via sample-and-hold
        // Map rateReduction 0-1 to decimation factor 1-64
        float decimation = 1.0f + rateReduction * 63.0f;

        holdCounter += 1.0f;
        if (holdCounter >= decimation)
        {
            holdCounter -= decimation;
            holdL = inL;
            holdR = inR;
        }
        float wetL = holdL;
        float wetR = holdR;

        // Bit depth reduction
        // Map bitDepth 0-1: at 0 = 16 bits (clean), at 1 = 1 bit (extreme)
        float bits = 16.0f - bitDepth * 15.0f;
        float levels = std::pow(2.0f, bits);
        float invLevels = 1.0f / levels;
        wetL = std::floor(wetL * levels + 0.5f) * invLevels;
        wetR = std::floor(wetR * levels + 0.5f) * invLevels;

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case RateReduction:
                rateReduction = std::clamp(value, 0.0f, 1.0f);
                break;
            case BitDepth:
                bitDepth = std::clamp(value, 0.0f, 1.0f);
                break;
            case PreFilter:
                preFilter = std::clamp(value, 0.0f, 1.0f);
                recalcFilterCoeff();
                break;
            case Mix:
                mix = std::clamp(value, 0.0f, 1.0f);
                break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case RateReduction: return rateReduction;
            case BitDepth:      return bitDepth;
            case PreFilter:     return preFilter;
            case Mix:           return mix;
            default:            return 0.0f;
        }
    }

private:
    float rateReduction = 0.3f;
    float bitDepth      = 0.0f;
    float preFilter     = 0.5f;

    float holdL = 0.0f;
    float holdR = 0.0f;
    float holdCounter = 0.0f;

    float preFilterL = 0.0f;
    float preFilterR = 0.0f;
    float cachedPreFilterCoeff = 0.5f;

    void recalcFilterCoeff()
    {
        if (sampleRate > 0.0)
        {
            // Filter cutoff tracks the decimation ratio
            // At full reduction, cut at ~Nyquist/64
            float cutoff = (float)sampleRate * 0.4f / (1.0f + rateReduction * 63.0f);
            cutoff = std::clamp(cutoff, 100.0f, (float)(sampleRate * 0.49));
            cachedPreFilterCoeff = 1.0f - std::exp(-dsp::TWO_PI_F * cutoff / (float)sampleRate);
        }
    }
};
