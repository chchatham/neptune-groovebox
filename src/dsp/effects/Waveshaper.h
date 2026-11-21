#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include "../Saturation.h"
#include <cmath>
#include <algorithm>

// Waveshaper: pre-gain into one of the saturation curves (src/dsp/Saturation.h), a DC blocker, a
// tone filter and output compensation. ASYM works the chosen curve off-centre, which adds even
// harmonics to any of them (on the ASYM curve, whose two halves differ already, it moves the knee).
// No buffers - pure computation. No heap on the audio thread.
class Waveshaper : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "DRIV", 0, 0, 1, 0.3f };
            case 1: return { "CURV", 1, 0, (float) (sat::Count - 1), 0, K::Choice, "", sat::names(), sat::Count };
            case 2: return { "TONE", 2, 0, 1, 0.7f };
            case 3: return { "ASYM", 3, 0, 1, 0.5f };
            default: return {};
        }
    }

    enum Param
    {
        Drive = 0,      // 0 - 1 (maps to 1x - 40x gain)
        Curve,          // sat::Curve
        Tone,           // 0 - 1 (post-filter: dark to bright)
        Asymmetry,      // 0 - 1 (how far off-centre the curve is worked: 0 - 0.5)
        Mix             // 0 - 1
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        dcL.setCutoff(sr, DC_HZ);
        dcR.setCutoff(sr, DC_HZ);
        updateCachedCoeffs();
        reset();
    }

    void reset() override
    {
        toneFilterL = 0.0f;
        toneFilterR = 0.0f;
        dcL.reset();
        dcR.reset();
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        // Pre-gain, the curve, and out with the steady offset an off-centre or one-sided curve leaves.
        float wetL = dcL.highPass(shaper(left * cachedGain));
        float wetR = dcR.highPass(shaper(right * cachedGain));

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
                curve = sat::valid((int) std::lround(value));
                shaper.set(curve, asymmetry * 0.5f);
                break;
            case Tone:
                tone = std::clamp(value, 0.0f, 1.0f);
                updateCachedCoeffs();
                break;
            case Asymmetry:
                asymmetry = std::clamp(value, 0.0f, 1.0f);
                shaper.set(curve, asymmetry * 0.5f);
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
    static constexpr float DC_HZ = 10.0f;

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
    int curve = sat::Soft;
    sat::Biased shaper { sat::Soft, 0.0f, 0.0f, 1.0f };
    fxb::OnePole dcL, dcR;

    float cachedGain = 1.0f;
    float cachedCompensation = 1.0f;
    float cachedToneCoeff = 0.5f;

    float toneFilterL = 0.0f;
    float toneFilterR = 0.0f;
};
