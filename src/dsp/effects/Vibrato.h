#pragma once

#include "EffectBase.h"
#include "FxBlocks.h"

// Vibrato: pitch modulation from a sine-modulated delay line, fully wet (no dry signal, so there is
// no chorus-style comb). Textbook structure (Zoelzer, "DAFX", delay-based effects: vibrato; Dattorro,
// "Effect Design Part 2"). The delay is read with Hermite interpolation (FxBlocks.h).
//
// DPTH is the peak pitch deviation in cents: the delay swing is derived from it and the rate
// (swing = deviation / (2 pi rate)), so the pitch depth stays the same when the rate changes. The
// swing is limited to 20 ms, which only matters for deep vibrato below about 0.5 Hz.
//
// FADE is a delayed-vibrato time: when the input comes up out of silence the depth starts at
// zero and grows to full over that time (an insert effect sees no note gates, so the onset is taken
// from an envelope follower; notes played legato do not restart it).
//
// With DPTH 0 the effect is a clean delay of BASE_DELAY samples.
class Vibrato : public EffectBase
{
public:
    enum Param { Rate = 0, Depth, Spread, Fade };

    static constexpr int BASE_DELAY = 2;   // samples

    Vibrato() { prepare(44100.0, 0); }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "RATE", 0, 0.1f, 12.0f, 5.0f, K::Log, "Hz" };
            case 1: return { "DPTH", 1, 0.0f, 100.0f, 25.0f, K::Linear, "ct" };
            case 2: return { "SPRD", 2, 0, 1, 0.0f };
            case 3: return { "FADE", 3, 0.0f, 2.0f, 0.0f, K::Linear, "s" };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Rate:   rate = std::clamp(value, 0.1f, 12.0f); break;
            case Depth:  depthCents = std::clamp(value, 0.0f, 100.0f); break;
            case Spread: spread = std::clamp(value, 0.0f, 1.0f); break;
            case Fade:   fadeSec = std::clamp(value, 0.0f, 2.0f); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Rate:   return rate;
            case Depth:  return depthCents;
            case Spread: return spread;
            case Fade:   return fadeSec;
            default:     return 0.0f;
        }
    }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        int maxSamples = (int) (2.0 * MAX_SWING_SEC * sr) + BASE_DELAY + 8;
        lineL.prepare(maxSamples);
        lineR.prepare(maxSamples);
        swingSmooth.setTime(sr, 50.0f);
        spreadSmooth.setTime(sr, 30.0f);
        fadeSmooth.setTime(sr, 5.0f);
        follower.setTimes(sr, 1.0f, 100.0f);
        reset();
    }

    void reset() override
    {
        lineL.clear();
        lineR.clear();
        phase = 0.0;
        swingSmooth.snap(targetSwing());
        spreadSmooth.snap(spread);
        follower.reset();
        gateOpen = false;
        fadeRamp = (fadeSec > 0.0f) ? 0.0f : 1.0f;
        fadeSmooth.snap(fadeRamp);
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        // Delayed vibrato: while the input is silent the depth is taken down to zero (unheard), and
        // it grows back once the input has come up again.
        float env = follower.process(0.5f * (left + right));
        if (!gateOpen && env > GATE_ON)
            gateOpen = true;
        else if (gateOpen && env < GATE_OFF)
            gateOpen = false;

        if (fadeSec <= 0.0f)
            fadeRamp = 1.0f;
        else if (!gateOpen)
            fadeRamp = 0.0f;
        else
            fadeRamp = std::min(1.0f, fadeRamp + 1.0f / (fadeSec * (float) sampleRate));

        float swing = swingSmooth.next(targetSwing()) * fadeSmooth.next(fadeRamp) * (float) sampleRate;
        float offset = spreadSmooth.next(spread) * 0.5f;   // right channel up to 180 degrees ahead

        float dL = (float) BASE_DELAY, dR = (float) BASE_DELAY;
        if (swing > 0.0f)
        {
            dL += swing * (1.0f + (float) std::sin(phase * dsp::TWO_PI_D));
            dR += swing * (1.0f + (float) std::sin((phase + (double) offset) * dsp::TWO_PI_D));
        }

        phase += (double) rate / sampleRate;
        if (phase >= 1.0) phase -= 1.0;

        float outL = lineL.readHermite(dL);
        float outR = lineR.readHermite(dR);
        lineL.write(left);
        lineR.write(right);
        left = outL;
        right = outR;
    }

private:
    static constexpr double MAX_SWING_SEC = 0.02;
    static constexpr float GATE_ON = 0.01f;     // -40 dB
    static constexpr float GATE_OFF = 0.003f;   // -50 dB

    // Peak delay swing in seconds for the wanted pitch deviation: a delay d(t) = A (1 + sin wt)
    // changes the pitch by the factor 1 - d'(t), so the peak deviation is A w.
    float targetSwing() const
    {
        float ratio = std::exp2(depthCents / 1200.0f) - 1.0f;
        return std::min((float) MAX_SWING_SEC, ratio / (dsp::TWO_PI_F * rate));
    }

    float rate = 5.0f;
    float depthCents = 25.0f;
    float spread = 0.0f;
    float fadeSec = 0.0f;

    fxb::DelayLine lineL, lineR;
    fxb::Smoother swingSmooth, spreadSmooth, fadeSmooth;
    fxb::EnvelopeFollower follower;
    double phase = 0.0;
    bool gateOpen = false;
    float fadeRamp = 1.0f;
};
