#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>

// Short delay flanger effect.
// Very short delay (0.1-10ms) modulated by LFO with feedback.
// All buffers pre-allocated. No heap on audio thread.
class Flanger : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "RATE", 0, 0.1f, 5.0f, 0.5f, K::Log, "Hz" };
            case 1: return { "DPTH", 1, 0, 1, 0.5f };
            case 2: return { "FDBK", 2, -0.95f, 0.95f, 0.3f };
            case 3: return { "MIX", 3, 0, 1, 0.5f };
            default: return {};
        }
    }

    enum Param
    {
        Rate = 0,       // 0.1 - 5 Hz
        Depth,          // 0 - 1
        Feedback,       // -0.95 to 0.95
        Mix             // 0 - 1
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        // Max delay = 10ms at max sample rate (192kHz) = 1920 samples
        maxDelaySamples = (int)(0.012 * sr) + 1;
        if (maxDelaySamples > MAX_DELAY_SAMPLES)
            maxDelaySamples = MAX_DELAY_SAMPLES;
        reset();
    }

    void reset() override
    {
        std::fill(delayLineL.begin(), delayLineL.end(), 0.0f);
        std::fill(delayLineR.begin(), delayLineR.end(), 0.0f);
        writePos = 0;
        lfoPhase = 0.0;
        feedbackSampleL = 0.0f;
        feedbackSampleR = 0.0f;
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

        // Delay time: 0.1ms to 10ms, modulated by LFO
        float centerDelay = 0.003f;  // 3ms center
        float modRange = depth * 0.003f; // up to +/- 3ms
        float delaySec = centerDelay + lfoVal * modRange;
        delaySec = std::clamp(delaySec, 0.0001f, 0.010f);
        float delaySamples = (float)(delaySec * sampleRate);

        // Write input + feedback
        delayLineL[writePos] = left + feedbackSampleL * feedback;
        delayLineR[writePos] = right + feedbackSampleR * feedback;

        // Read with linear interpolation
        float readPosF = (float)writePos - delaySamples;
        if (readPosF < 0.0f)
            readPosF += (float)maxDelaySamples;

        int idx0 = (int)readPosF;
        int idx1 = idx0 + 1;
        float frac = readPosF - (float)idx0;

        if (idx0 >= maxDelaySamples) idx0 -= maxDelaySamples;
        if (idx1 >= maxDelaySamples) idx1 -= maxDelaySamples;
        if (idx0 < 0) idx0 += maxDelaySamples;
        if (idx1 < 0) idx1 += maxDelaySamples;

        float wetL = lerp(delayLineL[idx0], delayLineL[idx1], frac);
        float wetR = lerp(delayLineR[idx0], delayLineR[idx1], frac);

        // Store feedback (clamped to prevent runaway)
        feedbackSampleL = std::clamp(wetL, -4.0f, 4.0f);
        feedbackSampleR = std::clamp(wetR, -4.0f, 4.0f);

        writePos = (writePos + 1) % maxDelaySamples;

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Rate:     rate     = std::clamp(value, 0.1f, 5.0f);    break;
            case Depth:    depth    = std::clamp(value, 0.0f, 1.0f);    break;
            case Feedback: feedback = std::clamp(value, -0.95f, 0.95f);  break;
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
            case Mix:      return mix;
            default:       return 0.0f;
        }
    }

private:
    static constexpr int MAX_DELAY_SAMPLES = 2560;

    std::array<float, MAX_DELAY_SAMPLES> delayLineL {};
    std::array<float, MAX_DELAY_SAMPLES> delayLineR {};
    int writePos = 0;
    int maxDelaySamples = MAX_DELAY_SAMPLES;

    double lfoPhase = 0.0;
    float feedbackSampleL = 0.0f;
    float feedbackSampleR = 0.0f;

    float rate     = 0.5f;
    float depth    = 0.5f;
    float feedback = 0.3f;
};
