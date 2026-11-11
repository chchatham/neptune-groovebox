#pragma once

#include "EffectBase.h"
#include "FxBlocks.h"

// Left / centre / right delay: one mono delay line (the two inputs summed) read at three taps.
// The left tap is heard on the left only, the right tap on the right only and the centre tap on
// both (at -3 dB). Feedback is taken from the centre tap through a one-pole low-pass (the classic
// L/C/R delay topology found in rack multi-effects; delay line and filter from FxBlocks.h).
//
// TIME  centre tap time
// FDBK  feedback from the centre tap
// SPRD  how far the side taps sit from the centre: left = TIME x (1 - SPRD/2),
//       right = TIME x (1 + SPRD/2). 0 = all three together, 1 = half / whole / one and a half
//       (eighth, quarter, dotted quarter), 0.67 = triplet spacing
// MIX   dry / wet
class LCRDelay : public EffectBase
{
public:
    enum Param { Time = 0, Feedback, Spread, Mix };

    static constexpr float MIN_TIME = 0.02f;
    static constexpr float MAX_TIME = 1.0f;
    static constexpr float CENTRE_GAIN = 0.70710678f;

    LCRDelay()
    {
        mix = 0.4f;
        prepare(44100.0, 0);
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "TIME", 0, MIN_TIME, MAX_TIME, 0.36f, K::Log, "s" };
            case 1: return { "FDBK", 1, 0, 0.95f, 0.35f };
            case 2: return { "SPRD", 2, 0, 1, 1.0f };
            case 3: return { "MIX", 3, 0, 1, 0.4f };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Time:     timeSec = std::clamp(value, MIN_TIME, MAX_TIME); break;
            case Feedback: feedback = std::clamp(value, 0.0f, 0.95f); break;
            case Spread:   spread = std::clamp(value, 0.0f, 1.0f); break;
            case Mix:      setMix(value); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Time:     return timeSec;
            case Feedback: return feedback;
            case Spread:   return spread;
            case Mix:      return mix;
            default:       return 0.0f;
        }
    }

    // Tap times in seconds for the current settings (left, centre, right).
    float getLeftTime() const { return timeSec * (1.0f - 0.5f * spread); }
    float getCentreTime() const { return timeSec; }
    float getRightTime() const { return timeSec * (1.0f + 0.5f * spread); }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        line.prepare((int) ((double) MAX_TIME * 1.5 * sr) + 16);
        timeSmooth.setTime(sr, 60.0f);
        spreadSmooth.setTime(sr, 60.0f);
        damping.setCutoff(sr, 5000.0f);
        reset();
    }

    void reset() override
    {
        line.clear();
        damping.reset();
        timeSmooth.snap(timeSec);
        spreadSmooth.snap(spread);
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float t = timeSmooth.next(timeSec) * (float) sampleRate;
        float s = 0.5f * spreadSmooth.next(spread);

        float tapL = line.readHermite(t * (1.0f - s));
        float tapC = line.readHermite(t);
        float tapR = line.readHermite(t * (1.0f + s));

        float in = 0.5f * (left + right);
        line.write(fxb::flushDenormal(in + feedback * damping.lowPass(tapC)));

        left = lerp(left, tapL + CENTRE_GAIN * tapC, mix);
        right = lerp(right, tapR + CENTRE_GAIN * tapC, mix);
    }

private:
    float timeSec = 0.36f;
    float feedback = 0.35f;
    float spread = 1.0f;

    fxb::DelayLine line;
    fxb::OnePole damping;
    fxb::Smoother timeSmooth, spreadSmooth;
};
