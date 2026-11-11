#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>
#include <cmath>
#include <algorithm>

// Multi-tap delay with up to 8 taps, each with individual time, level,
// pan, and feedback. A single shared delay buffer is read at 8 different
// positions. All buffers pre-allocated. No heap on audio thread.
class MultiTapDelay : public EffectBase
{
public:
    // Parameter layout: per-tap params grouped, then globals
    // Tap 0: Time(0), Level(1), Pan(2), Feedback(3)
    // Tap 1: Time(4), Level(5), Pan(6), Feedback(7)
    // ...
    // Tap 7: Time(28), Level(29), Pan(30), Feedback(31)
    // Global: NumTaps(32), HighCut(33), Mix(34)
    enum GlobalParam
    {
        NumTaps = 32,   // 1 - 8
        HighCut = 33,   // 500 - 20000 Hz
        MixParam = 34   // 0 - 1
    };

    static constexpr int TAP_PARAM_COUNT = 4;
    static constexpr int tapTime(int tap)     { return tap * TAP_PARAM_COUNT + 0; }
    static constexpr int tapLevel(int tap)    { return tap * TAP_PARAM_COUNT + 1; }
    static constexpr int tapPan(int tap)      { return tap * TAP_PARAM_COUNT + 2; }
    static constexpr int tapFeedback(int tap) { return tap * TAP_PARAM_COUNT + 3; }

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
        lpStateL = 0.0f;
        lpStateR = 0.0f;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        int activeTaps = std::clamp(numTaps, 1, MAX_TAPS);

        // Compute feedback sum from all active taps
        float fbSumL = 0.0f;
        float fbSumR = 0.0f;

        float wetL = 0.0f;
        float wetR = 0.0f;

        for (int t = 0; t < activeTaps; ++t)
        {
            float delaySamples = taps[t].timeSec * (float)sampleRate;
            delaySamples = std::clamp(delaySamples, 1.0f, (float)(MAX_SAMPLES - 1));

            // Read with linear interpolation
            float readL = readInterp(bufferL, delaySamples);
            float readR = readInterp(bufferR, delaySamples);

            float mono = (readL + readR) * 0.5f;

            // Pan: 0 = full left, 0.5 = center, 1 = full right
            float panR = taps[t].pan;
            float panL = 1.0f - panR;

            wetL += mono * taps[t].level * panL;
            wetR += mono * taps[t].level * panR;

            // Each tap contributes its own feedback
            fbSumL += readL * taps[t].feedback;
            fbSumR += readR * taps[t].feedback;
        }

        // Normalize feedback to prevent runaway
        float fbScale = 1.0f / std::max(1.0f, (float)activeTaps * 0.5f);
        fbSumL *= fbScale;
        fbSumR *= fbScale;

        // Apply high-cut filter to feedback
        lpStateL += cachedLpCoeff * (fbSumL - lpStateL);
        lpStateR += cachedLpCoeff * (fbSumR - lpStateR);

        // Write input + filtered feedback to delay buffer
        bufferL[(size_t)writePos] = left + lpStateL;
        bufferR[(size_t)writePos] = right + lpStateR;
        writePos = (writePos + 1) % MAX_SAMPLES;

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        if (id >= 0 && id < MAX_TAPS * TAP_PARAM_COUNT)
        {
            int tap = id / TAP_PARAM_COUNT;
            int param = id % TAP_PARAM_COUNT;
            if (tap < MAX_TAPS)
            {
                switch (param)
                {
                    case 0: taps[tap].timeSec  = std::clamp(value, 0.01f, 2.0f);  break;
                    case 1: taps[tap].level    = std::clamp(value, 0.0f, 1.0f);   break;
                    case 2: taps[tap].pan      = std::clamp(value, 0.0f, 1.0f);   break;
                    case 3: taps[tap].feedback = std::clamp(value, 0.0f, 0.95f);  break;
                }
            }
            return;
        }

        switch (id)
        {
            case NumTaps:  numTaps = std::clamp((int)value, 1, MAX_TAPS);    break;
            case HighCut:
                highCut = std::clamp(value, 500.0f, 20000.0f);
                recalcFilterCoeff();
                break;
            case MixParam: mix = std::clamp(value, 0.0f, 1.0f);             break;
        }
    }

    float getParameter(int id) const override
    {
        if (id >= 0 && id < MAX_TAPS * TAP_PARAM_COUNT)
        {
            int tap = id / TAP_PARAM_COUNT;
            int param = id % TAP_PARAM_COUNT;
            if (tap < MAX_TAPS)
            {
                switch (param)
                {
                    case 0: return taps[tap].timeSec;
                    case 1: return taps[tap].level;
                    case 2: return taps[tap].pan;
                    case 3: return taps[tap].feedback;
                }
            }
        }

        switch (id)
        {
            case NumTaps:  return (float)numTaps;
            case HighCut:  return highCut;
            case MixParam: return mix;
            default:       return 0.0f;
        }
    }

private:
    static constexpr int MAX_TAPS = 8;
    static constexpr int MAX_SAMPLES = 96000 * 2; // 2 seconds at 96kHz

    struct Tap
    {
        float timeSec  = 0.25f;
        float level    = 0.7f;
        float pan      = 0.5f;  // 0=left, 0.5=center, 1=right
        float feedback = 0.3f;
    };

    std::array<Tap, MAX_TAPS> taps = []()
    {
        std::array<Tap, MAX_TAPS> t {};
        // Default: evenly spaced taps with decreasing level
        for (int i = 0; i < MAX_TAPS; ++i)
        {
            t[i].timeSec = 0.125f * (float)(i + 1);
            t[i].level = 0.8f - 0.08f * (float)i;
            t[i].pan = (i % 2 == 0) ? 0.3f : 0.7f; // alternating L/R
            t[i].feedback = 0.2f;
        }
        return t;
    }();

    int numTaps = 4;
    float highCut = 8000.0f;
    float cachedLpCoeff = 0.5f;

    std::array<float, MAX_SAMPLES> bufferL {};
    std::array<float, MAX_SAMPLES> bufferR {};
    int writePos = 0;
    float lpStateL = 0.0f;
    float lpStateR = 0.0f;

    float readInterp(const std::array<float, MAX_SAMPLES>& buf, float delaySamples) const
    {
        float readPosF = (float)writePos - delaySamples;
        if (readPosF < 0.0f) readPosF += (float)MAX_SAMPLES;
        int idx0 = (int)readPosF;
        int idx1 = idx0 + 1;
        float frac = readPosF - (float)idx0;
        if (idx0 >= MAX_SAMPLES) idx0 -= MAX_SAMPLES;
        if (idx1 >= MAX_SAMPLES) idx1 -= MAX_SAMPLES;
        if (idx0 < 0) idx0 += MAX_SAMPLES;
        if (idx1 < 0) idx1 += MAX_SAMPLES;
        return buf[(size_t)idx0] * (1.0f - frac) + buf[(size_t)idx1] * frac;
    }

    void recalcFilterCoeff()
    {
        if (sampleRate > 0.0)
            cachedLpCoeff = 1.0f - std::exp(-dsp::TWO_PI_F * highCut / (float)sampleRate);
    }
};
