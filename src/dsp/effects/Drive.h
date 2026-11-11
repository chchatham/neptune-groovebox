#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <cmath>

class Drive : public EffectBase
{

public:
    static const char* const* algoNames()
    {
        static const char* const names[] = { "SOFT", "HARD", "FOLD", "CRSH" };
        return names;
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "DRIV", 0, 0, 1, 0.3f };
            case 1: return { "TONE", 1, 0, 1, 0.5f };
            case 2: return { "MIX", 2, 0, 1, 1.0f };
            case 3: return { "ALGO", 3, 0, 3, 0, K::Choice, "", algoNames(), 4 };
            default: return {};
        }
    }

    enum Param
    {
        DriveAmount = 0,    // 0 - 1 (maps to gain multiplier)
        Tone,               // 0 - 1 (post-filter cutoff: dark to bright)
        Mix,                // 0 - 1
        Algorithm           // 0=soft, 1=hard, 2=wavefold, 3=bitcrush
    };

    enum AlgoType
    {
        SoftClip = 0,
        HardClip = 1,
        Wavefolder = 2,
        Bitcrush = 3
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        updateCachedCoeffs();
        reset();
    }

    void reset() override
    {
        toneFilterL = 0.0f;
        toneFilterR = 0.0f;
        sampleHoldL = 0.0f;
        sampleHoldR = 0.0f;
        sampleHoldCounter = 0.0f;
    }

    void process(float& left, float& right) override
    {
        if (!enabled)
            return;

        float dryL = left;
        float dryR = right;

        float wetL = left * cachedGain;
        float wetR = right * cachedGain;

        switch (algo)
        {
            case SoftClip:
                wetL = dsp::fastTanh(wetL);
                wetR = dsp::fastTanh(wetR);
                break;

            case HardClip:
                wetL = std::clamp(wetL, -1.0f, 1.0f);
                wetR = std::clamp(wetR, -1.0f, 1.0f);
                break;

            case Wavefolder:
                wetL = wavefold(wetL);
                wetR = wavefold(wetR);
                break;

            case Bitcrush:
                processBitcrush(wetL, wetR);
                break;
        }

        toneFilterL += cachedToneCoeff * (wetL - toneFilterL);
        toneFilterR += cachedToneCoeff * (wetR - toneFilterR);
        wetL = toneFilterL;
        wetR = toneFilterR;

        wetL *= cachedCompensation;
        wetR *= cachedCompensation;

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case DriveAmount:
                drive = std::clamp(value, 0.0f, 1.0f);
                updateCachedCoeffs();
                break;
            case Tone:
                tone = std::clamp(value, 0.0f, 1.0f);
                updateCachedCoeffs();
                break;
            case Mix:
                mix = std::clamp(value, 0.0f, 1.0f);
                break;
            case Algorithm:
                algo = (AlgoType)std::clamp((int)value, 0, 3);
                break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case DriveAmount: return drive;
            case Tone:        return tone;
            case Mix:         return mix;
            case Algorithm:   return (float)algo;
            default:          return 0.0f;
        }
    }

private:
    // Wavefolder: folds signal back when it exceeds [-1, 1]
    static float wavefold(float x)
    {
        // Sine-based wavefolder for smooth folds
        // Clamp to prevent extreme values
        x = std::clamp(x, -5.0f, 5.0f);
        return std::sin(x * 1.5707963f); // sin(x * pi/2)
    }

    // Bitcrush: reduce sample rate and bit depth
    void processBitcrush(float& l, float& r)
    {
        // Sample rate reduction: drive controls reduction ratio
        // At drive=0: no reduction. At drive=1: ~16x reduction
        float rateReduction = 1.0f + drive * 15.0f;

        sampleHoldCounter += 1.0f;
        if (sampleHoldCounter >= rateReduction)
        {
            sampleHoldCounter -= rateReduction;
            sampleHoldL = l;
            sampleHoldR = r;
        }
        l = sampleHoldL;
        r = sampleHoldR;

        // Bit depth reduction: drive also controls this
        // At drive=0: 16-bit equivalent, at drive=1: ~3-bit
        float bits = 16.0f - drive * 13.0f;
        float levels = std::pow(2.0f, bits);
        l = std::round(l * levels) / levels;
        r = std::round(r * levels) / levels;
    }

    float drive = 0.3f;
    float tone  = 0.7f;
    AlgoType algo = SoftClip;

    float cachedGain = 1.0f + 0.3f * 39.0f;
    float cachedCompensation = 1.0f / std::sqrt(1.0f + 0.3f * 39.0f);
    float cachedToneCoeff = 0.5f;

    void updateCachedCoeffs()
    {
        cachedGain = 1.0f + drive * 39.0f;
        cachedCompensation = 1.0f / std::sqrt(cachedGain);
        float cutoffHz = 500.0f * std::pow(36.0f, tone);
        float w = dsp::TWO_PI_F * cutoffHz / (float)sampleRate;
        cachedToneCoeff = w / (1.0f + w);
    }

    float toneFilterL = 0.0f;
    float toneFilterR = 0.0f;

    // Bitcrush state
    float sampleHoldL = 0.0f;
    float sampleHoldR = 0.0f;
    float sampleHoldCounter = 0.0f;
};
