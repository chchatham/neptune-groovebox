#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>

// Allpass cascade phaser.
// 6-stage allpass filter chain with LFO-swept notch frequencies.
// Feedback path for resonant phasing. All state pre-allocated.
class Phaser : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "RATE", 0, 0.1f, 5.0f, 0.5f, K::Log, "Hz" };
            case 1: return { "DPTH", 1, 0, 1, 0.5f };
            case 2: return { "FDBK", 2, -0.9f, 0.9f, 0.3f };
            case 3: return { "STGS", 3, 2, 6, 4, K::Int };
            default: return {};
        }
    }

    enum Param
    {
        Rate = 0,       // 0.1 - 5 Hz
        Depth,          // 0 - 1
        Feedback,       // -0.9 to 0.9
        Stages,         // 2, 4, or 6
        Mix             // 0 - 1
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        reset();
    }

    void reset() override
    {
        for (auto& s : apStateL) s = 0.0f;
        for (auto& s : apStateR) s = 0.0f;
        feedbackL = 0.0f;
        feedbackR = 0.0f;
        lfoPhase = 0.0;
    }

    void process(float& left, float& right) override
    {
        if (!enabled)
            return;

        float dryL = left;
        float dryR = right;

        // Advance LFO
        lfoPhase += rate / sampleRate;
        while (lfoPhase >= 1.0)
            lfoPhase -= 1.0;

        float lfoVal = std::sin((float)lfoPhase * dsp::TWO_PI_F);

        // Map LFO to allpass coefficient range
        // Sweep between ~200 Hz and ~4000 Hz in terms of allpass break frequency
        float minFreq = 200.0f;
        float maxFreq = 4000.0f;
        float sweepFreq = minFreq + (maxFreq - minFreq) * (lfoVal * depth * 0.5f + 0.5f);
        float coeff = computeAllpassCoeff(sweepFreq);

        int numStages = std::clamp((int)stages, 1, MAX_STAGES);
        // Snap to even values: 2, 4, or 6
        if (numStages <= 2) numStages = 2;
        else if (numStages <= 4) numStages = 4;
        else numStages = 6;

        // Process left channel through allpass cascade
        float wetL = left + feedbackL * feedback;
        for (int s = 0; s < numStages; ++s)
            wetL = processAllpass(wetL, apStateL[s], coeff);

        // Process right channel through allpass cascade
        float wetR = right + feedbackR * feedback;
        for (int s = 0; s < numStages; ++s)
            wetR = processAllpass(wetR, apStateR[s], coeff);

        feedbackL = std::clamp(wetL, -4.0f, 4.0f);
        feedbackR = std::clamp(wetR, -4.0f, 4.0f);

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Rate:     rate     = std::clamp(value, 0.1f, 5.0f);    break;
            case Depth:    depth    = std::clamp(value, 0.0f, 1.0f);    break;
            case Feedback: feedback = std::clamp(value, -0.9f, 0.9f);   break;
            case Stages:   stages   = std::clamp(value, 2.0f, 6.0f);    break;
            case Mix:      mix      = std::clamp(value, 0.0f, 1.0f);    break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Rate:     return rate;
            case Depth:    return depth;
            case Feedback: return feedback;
            case Stages:   return stages;
            case Mix:      return mix;
            default:       return 0.0f;
        }
    }

private:
    static constexpr int MAX_STAGES = 6;

    // First-order allpass filter: y[n] = coeff * (x[n] - y[n-1]) + x[n-1]
    // state holds x[n-1] mixed with y[n-1]
    static float processAllpass(float input, float& state, float coeff)
    {
        float output = coeff * input + state;
        state = input - coeff * output;
        return output;
    }

    float computeAllpassCoeff(float freq) const
    {
        float w = dsp::PI_F * freq / (float)sampleRate;
        float t = dsp::fastTan(w);
        return (t - 1.0f) / (t + 1.0f);
    }

    std::array<float, MAX_STAGES> apStateL {};
    std::array<float, MAX_STAGES> apStateR {};
    float feedbackL = 0.0f;
    float feedbackR = 0.0f;

    double lfoPhase = 0.0;
    float rate     = 0.5f;
    float depth    = 0.5f;
    float feedback = 0.3f;
    float stages   = 6.0f;
};
