#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <cmath>
#include <array>

// 5-band parametric EQ: low shelf, 3 parametric mids, high shelf.
// Biquad filters using Robert Bristow-Johnson Audio EQ Cookbook formulas.
// No heap allocation. All buffers pre-allocated as member arrays.
class ParametricEQ : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "LOW", 0, -18, 18, 0, K::Linear, "dB" };
            case 1: return { "MID", 1, -18, 18, 0, K::Linear, "dB" };
            case 2: return { "MFRQ", 2, 100, 8000, 1000, K::Log, "Hz" };
            case 3: return { "HIGH", 3, -18, 18, 0, K::Linear, "dB" };
            default: return {};
        }
    }

    // FX-page view of the EQ: low shelf gain, middle band gain and frequency, high shelf gain.
    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case 0: setLowShelf(params[LowFreq], value); break;
            case 1: setParametric(1, params[Mid2Freq], value, params[Mid2Q]); break;
            case 2: setParametric(1, value, params[Mid2Gain], params[Mid2Q]); break;
            case 3: setHighShelf(params[HighFreq], value); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case 0: return params[LowGain];
            case 1: return params[Mid2Gain];
            case 2: return params[Mid2Freq];
            case 3: return params[HighGain];
            default: return 0.0f;
        }
    }

    // Parameter indices matching the spec
    enum Param
    {
        LowFreq = 0,
        LowGain,
        Mid1Freq,
        Mid1Gain,
        Mid1Q,
        Mid2Freq,
        Mid2Gain,
        Mid2Q,
        Mid3Freq,
        Mid3Gain,
        Mid3Q,
        HighFreq,
        HighGain,
        NumParams
    };

    ParametricEQ()
    {
        // Default band settings
        setLowShelf(80.0f, 0.0f);
        setParametric(0, 400.0f, 0.0f, 1.0f);
        setParametric(1, 1000.0f, 0.0f, 1.0f);
        setParametric(2, 4000.0f, 0.0f, 1.0f);
        setHighShelf(8000.0f, 0.0f);
    }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        recalcAll();
    }

    void reset() override
    {
        for (auto& band : bands)
            band.reset();
    }

    // Low shelf: frequency (Hz), gain (dB)
    void setLowShelf(float freq, float gainDb)
    {
        params[LowFreq] = std::clamp(freq, 20.0f, 2000.0f);
        params[LowGain] = std::clamp(gainDb, -18.0f, 18.0f);
        calcLowShelf();
    }

    // High shelf: frequency (Hz), gain (dB)
    void setHighShelf(float freq, float gainDb)
    {
        params[HighFreq] = std::clamp(freq, 2000.0f, 20000.0f);
        params[HighGain] = std::clamp(gainDb, -18.0f, 18.0f);
        calcHighShelf();
    }

    // Parametric band (index 0-2): frequency, gain (dB), Q
    void setParametric(int idx, float freq, float gainDb, float q)
    {
        if (idx < 0 || idx > 2) return;
        int base = Mid1Freq + idx * 3;
        params[base] = std::clamp(freq, 20.0f, 20000.0f);
        params[base + 1] = std::clamp(gainDb, -18.0f, 18.0f);
        params[base + 2] = std::clamp(q, 0.1f, 20.0f);
        calcParametric(idx);
    }

    float getParam(int p) const
    {
        if (p >= 0 && p < NumParams) return params[p];
        return 0.0f;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;
        for (auto& band : bands)
        {
            left = band.processL(left);
            right = band.processR(right);
        }
    }

private:
    static constexpr int NUM_BANDS = 5;
    static constexpr float PI = dsp::PI_F;

    // Biquad filter state for one channel
    struct BiquadState
    {
        float x1 = 0.0f, x2 = 0.0f;
        float y1 = 0.0f, y2 = 0.0f;
    };

    // Stereo biquad band
    struct Band
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        BiquadState stateL, stateR;

        void reset()
        {
            stateL = {};
            stateR = {};
        }

        float processL(float in) { return processSample(in, stateL); }
        float processR(float in) { return processSample(in, stateR); }

        float processSample(float in, BiquadState& s) const
        {
            float out = b0 * in + b1 * s.x1 + b2 * s.x2
                                - a1 * s.y1 - a2 * s.y2;
            s.x2 = s.x1;
            s.x1 = in;
            s.y2 = s.y1;
            s.y1 = out;
            return out;
        }
    };

    std::array<Band, NUM_BANDS> bands;
    std::array<float, NumParams> params {};

    void recalcAll()
    {
        calcLowShelf();
        calcParametric(0);
        calcParametric(1);
        calcParametric(2);
        calcHighShelf();
    }

    // RBJ low-shelf biquad
    void calcLowShelf()
    {
        float freq = params[LowFreq];
        float gainDb = params[LowGain];
        auto& b = bands[0];

        if (std::abs(gainDb) < 0.01f)
        {
            b.b0 = 1.0f; b.b1 = 0.0f; b.b2 = 0.0f;
            b.a1 = 0.0f; b.a2 = 0.0f;
            return;
        }

        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * PI * freq / (float)sampleRate;
        float cosw0 = std::cos(w0);
        float sinw0 = std::sin(w0);
        float alpha = sinw0 / (2.0f * 0.707f);
        float sqrtA = std::sqrt(A);

        float a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
        b.b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha) / a0;
        b.b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0) / a0;
        b.b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) / a0;
        b.a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0) / a0;
        b.a2 = ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) / a0;
    }

    // RBJ high-shelf biquad
    void calcHighShelf()
    {
        float freq = params[HighFreq];
        float gainDb = params[HighGain];
        auto& b = bands[4];

        if (std::abs(gainDb) < 0.01f)
        {
            b.b0 = 1.0f; b.b1 = 0.0f; b.b2 = 0.0f;
            b.a1 = 0.0f; b.a2 = 0.0f;
            return;
        }

        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * PI * freq / (float)sampleRate;
        float cosw0 = std::cos(w0);
        float sinw0 = std::sin(w0);
        float alpha = sinw0 / (2.0f * 0.707f);
        float sqrtA = std::sqrt(A);

        float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
        b.b0 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha) / a0;
        b.b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0) / a0;
        b.b2 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) / a0;
        b.a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0) / a0;
        b.a2 = ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) / a0;
    }

    // RBJ peakingEQ biquad
    void calcParametric(int idx)
    {
        int base = Mid1Freq + idx * 3;
        float freq = params[base];
        float gainDb = params[base + 1];
        float q = params[base + 2];
        auto& b = bands[1 + idx];

        if (std::abs(gainDb) < 0.01f)
        {
            b.b0 = 1.0f; b.b1 = 0.0f; b.b2 = 0.0f;
            b.a1 = 0.0f; b.a2 = 0.0f;
            return;
        }

        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * PI * freq / (float)sampleRate;
        float cosw0 = std::cos(w0);
        float sinw0 = std::sin(w0);
        float alpha = sinw0 / (2.0f * q);

        float a0 = 1.0f + alpha / A;
        b.b0 = (1.0f + alpha * A) / a0;
        b.b1 = (-2.0f * cosw0) / a0;
        b.b2 = (1.0f - alpha * A) / a0;
        b.a1 = (-2.0f * cosw0) / a0;
        b.a2 = (1.0f - alpha / A) / a0;
    }
};
