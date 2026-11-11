#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <cmath>
#include <algorithm>

// Configurable waveshaper with multiple shaping curves.
// Provides soft clip (tanh), hard clip, wavefolding, and asymmetric
// distortion algorithms. Includes pre-gain, tone filter, and
// output compensation. No buffers needed - pure computation.
// No heap on audio thread.
class Waveshaper : public EffectBase
{
public:
    static const char* const* curveNames()
    {
        static const char* const n[] = { "SOFT", "HARD", "FOLD", "ASYM" };
        return n;
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "DRIV", 0, 0, 1, 0.3f };
            case 1: return { "CURV", 1, 0, 3, 0, K::Choice, "", curveNames(), 4 };
            case 2: return { "TONE", 2, 0, 1, 0.7f };
            case 3: return { "ASYM", 3, 0, 1, 0.5f };
            default: return {};
        }
    }

    enum Param
    {
        Drive = 0,      // 0 - 1 (maps to 1x - 40x gain)
        Curve,          // 0 = soft clip, 1 = hard clip, 2 = fold, 3 = asymmetric
        Tone,           // 0 - 1 (post-filter: dark to bright)
        Asymmetry,      // 0 - 1 (positive bias for asymmetric mode)
        Mix             // 0 - 1
    };

    enum CurveType
    {
        SoftClip = 0,
        HardClip = 1,
        Fold = 2,
        Asymmetric = 3
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
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        // Pre-gain
        float wetL = left * cachedGain;
        float wetR = right * cachedGain;

        // Apply shaping curve
        switch (curve)
        {
            case SoftClip:
                wetL = dsp::fastTanh(wetL);
                wetR = dsp::fastTanh(wetR);
                break;

            case HardClip:
                wetL = std::clamp(wetL, -1.0f, 1.0f);
                wetR = std::clamp(wetR, -1.0f, 1.0f);
                break;

            case Fold:
                wetL = wavefold(wetL);
                wetR = wavefold(wetR);
                break;

            case Asymmetric:
                wetL = asymmetricShape(wetL);
                wetR = asymmetricShape(wetR);
                break;
        }

        // Tone filter (one-pole lowpass)
        toneFilterL += cachedToneCoeff * (wetL - toneFilterL);
        toneFilterR += cachedToneCoeff * (wetR - toneFilterR);
        wetL = toneFilterL;
        wetR = toneFilterR;

        // Output compensation
        wetL *= cachedCompensation;
        wetR *= cachedCompensation;

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Drive:
                drive = std::clamp(value, 0.0f, 1.0f);
                updateCachedCoeffs();
                break;
            case Curve:
                curve = (CurveType)std::clamp((int)value, 0, 3);
                break;
            case Tone:
                tone = std::clamp(value, 0.0f, 1.0f);
                updateCachedCoeffs();
                break;
            case Asymmetry:
                asymmetry = std::clamp(value, 0.0f, 1.0f);
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
            case Drive:     return drive;
            case Curve:     return (float)curve;
            case Tone:      return tone;
            case Asymmetry: return asymmetry;
            case Mix:       return mix;
            default:        return 0.0f;
        }
    }

private:
    // Sine-based wavefolder: folds back when signal exceeds [-1, 1]
    static float wavefold(float x)
    {
        x = std::clamp(x, -5.0f, 5.0f);
        // Multiple folds using sin(x * pi/2)
        return std::sin(x * dsp::PI_F * 0.5f);
    }

    // Asymmetric distortion: positive half clipped differently from negative
    float asymmetricShape(float x) const
    {
        float bias = asymmetry * 0.5f;
        x += bias;
        if (x > 0.0f)
            x = dsp::fastTanh(x * 1.5f);
        else
            x = dsp::fastTanh(x * 0.8f);
        x -= dsp::fastTanh(bias); // DC offset compensation
        return x;
    }

    void updateCachedCoeffs()
    {
        cachedGain = 1.0f + drive * 39.0f;
        cachedCompensation = 1.0f / std::sqrt(cachedGain);
        float cutoffHz = 500.0f * std::pow(36.0f, tone);
        if (sampleRate > 0.0)
        {
            float w = dsp::TWO_PI_F * cutoffHz / (float)sampleRate;
            cachedToneCoeff = w / (1.0f + w);
        }
    }

    float drive     = 0.3f;
    float tone      = 0.7f;
    float asymmetry = 0.0f;
    CurveType curve = SoftClip;

    float cachedGain = 1.0f;
    float cachedCompensation = 1.0f;
    float cachedToneCoeff = 0.5f;

    float toneFilterL = 0.0f;
    float toneFilterR = 0.0f;
};
