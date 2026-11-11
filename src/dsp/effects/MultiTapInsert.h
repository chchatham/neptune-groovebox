#pragma once

#include "EffectBase.h"
#include "FxBlocks.h"
#include "MultiTapDelay.h"

// Four-parameter front end for MultiTapDelay, so it can sit in an insert slot. MultiTapDelay
// itself is unchanged (it keeps its 35 per-tap parameters for anything that drives it directly);
// this class owns one and sets its taps from four macro controls:
//
// TIME  time of the last tap; the taps are spaced evenly up to it (tap i at TIME x i / TAPS)
// TAPS  number of taps, 1..8; they alternate left / right and get quieter towards the last
// FDBK  feedback (regeneration of the whole pattern)
// MIX   dry / wet
//
// MultiTapDelay sums the feedback of all active taps and divides by max(1, taps / 2), so with every
// tap feeding back fully the loop gain could reach 2. The adapter gives each tap FDBK / 2 (FDBK
// for a single tap), which keeps the worst-case loop gain at FDBK (at most 0.9).
// TIME is smoothed here, because MultiTapDelay jumps its read positions when a tap time is set.
class MultiTapInsert : public EffectBase
{
public:
    enum Param { Time = 0, Taps, Feedback, Mix };

    static constexpr int MAX_TAPS = 8;

    MultiTapInsert()
    {
        mix = 0.4f;
        prepare(44100.0, 0);
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "TIME", 0, 0.05f, 2.0f, 0.5f, K::Log, "s" };
            case 1: return { "TAPS", 1, 1, MAX_TAPS, 4, K::Int };
            case 2: return { "FDBK", 2, 0, 0.9f, 0.3f };
            case 3: return { "MIX", 3, 0, 1, 0.4f };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Time:     timeSec = std::clamp(value, 0.05f, 2.0f); break;
            case Taps:     numTaps = std::clamp((int) std::lround(value), 1, MAX_TAPS); break;
            case Feedback: feedback = std::clamp(value, 0.0f, 0.9f); break;
            case Mix:      setMix(value); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Time:     return timeSec;
            case Taps:     return (float) numTaps;
            case Feedback: return feedback;
            case Mix:      return mix;
            default:       return 0.0f;
        }
    }

    void prepare(double sr, int maxBlockSize) override
    {
        sampleRate = sr;
        delay.prepare(sr, maxBlockSize);
        timeSmooth.setTime(sr, 60.0f);
        reset();
    }

    void reset() override
    {
        delay.reset();
        timeSmooth.snap(timeSec);
        appliedTime = -1.0f;
        appliedTaps = -1;
        appliedFeedback = -1.0f;
        appliedMix = -1.0f;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float t = timeSmooth.next(timeSec);
        if (t != appliedTime || numTaps != appliedTaps || feedback != appliedFeedback)
            applyTaps(t);

        if (mix != appliedMix)
        {
            appliedMix = mix;
            delay.setParameter(MultiTapDelay::MixParam, mix);
        }

        delay.process(left, right);
    }

private:
    void applyTaps(float t)
    {
        const int n = numTaps;
        if (n != appliedTaps)
            delay.setParameter(MultiTapDelay::NumTaps, (float) n);

        const float perTapFeedback = (n >= 2) ? feedback * 0.5f : feedback;
        for (int i = 0; i < n; ++i)
        {
            delay.setParameter(MultiTapDelay::tapTime(i), t * (float) (i + 1) / (float) n);
            delay.setParameter(MultiTapDelay::tapFeedback(i), perTapFeedback);

            if (n != appliedTaps)
            {
                float level = (n > 1) ? 1.0f - 0.6f * (float) i / (float) (n - 1) : 1.0f;
                float pan = (n == 1) ? 0.5f : ((i % 2 == 0) ? 0.15f : 0.85f);
                delay.setParameter(MultiTapDelay::tapLevel(i), level);
                delay.setParameter(MultiTapDelay::tapPan(i), pan);
            }
        }

        appliedTime = t;
        appliedTaps = n;
        appliedFeedback = feedback;
    }

    float timeSec = 0.5f;
    int numTaps = 4;
    float feedback = 0.3f;

    MultiTapDelay delay;
    fxb::Smoother timeSmooth;
    float appliedTime = -1.0f;
    int appliedTaps = -1;
    float appliedFeedback = -1.0f;
    float appliedMix = -1.0f;
};
