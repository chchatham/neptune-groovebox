#pragma once

#include "ModDelay.h"

// Vibrato: pitch modulation from one voice of the modulated delay (ModDelay.h), fully wet unless
// the slot's WET is turned down (the dry sound next to it makes a chorus). Textbook structure
// (Zoelzer, "DAFX", delay-based effects: vibrato; Dattorro, "Effect Design Part 2").
//
// DPTH is the peak pitch deviation in cents for the sine: the delay swing is derived from it and
// the rate (swing = deviation / (2 pi rate)), so the pitch depth stays the same when the rate
// changes. The swing is limited to 20 ms, which only matters for deep vibrato below about 0.5 Hz.
// SHPE is the shape of the DELAY, so the pitch is its slope: TRI is a trill between two pitches,
// SAW and RAMP a steady detune with a bend back once a cycle, SQR and RND bends from place to place.
//
// SPRD puts the right side's modulation up to half a cycle ahead of the left.
//
// An insert effect sees no note gates, so the beginning of a sound is heard:
// FADE  a delayed-vibrato time: from the beginning of a sound the depth starts at nothing and
//       grows to full over it
// DLAY  (second page) and waits this long before it starts to grow
// TRIG  (second page) what a beginning is, and what it does to the cycle. FREE: a sound that
//       comes up out of silence (over -40 dB after it fell under -50 dB, about half a second of
//       rest: the vibrato as it always was, one fade for a phrase), and the cycle runs on. Any
//       other: every onset the effects' detector hears (fxm::Onset, FxLfo.h: out of silence or a
//       jump of 4 dB, so one fade for every note), and KEY starts the cycle again, ONE one cycle
//       and stop (a single bend), HALF half of one, HOLD the delay it has is kept
// KEY   (second page) whose onsets: SELF, or IN = the track's input
//
// With DPTH 0 the effect is a clean delay of BASE_DELAY samples.
// Through-zero vibrato: THRU delays the dry by the vibrato's centre, so the pitch sweep
// passes through it. At MIX below 1 this creates a one-voice flanger with the vibrato's
// onset fade and trigger modes — a sound that could not be had from the flanger alone.
class Vibrato : public moddelay::Effect
{
public:
    enum Param { Rate = 0, Depth, Spread, Fade };
    enum Extra { Shape = 0, Trig, Delay, Through };

    static constexpr int BASE_DELAY = 2;   // samples

    Vibrato() { seed(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "RATE", 0, 0.1f, 12.0f, 5.0f, K::Log, "Hz" },
            { "DPTH", 1, 0.0f, 100.0f, 25.0f, K::Linear, "ct" },
            { "SPRD", 2, 0, 1, 0.0f },
            { "FADE", 3, 0.0f, 2.0f, 0.0f, K::Linear, "s" },
            fxm::Lfo::shapeSpec("SHPE", Shape),
            fxm::Lfo::triggerSpec("TRIG", Trig), fxm::delaySpec(Delay), thruSpec(Through) };
        return c;
    }

    void prepareCore() override
    {
        core.setVoices(1);
        core.setRange(0.0f, 2.0f * MAX_SWING_SEC);
        core.prepare(sampleRate, 2.0 * MAX_SWING_SEC + 0.001, false);
        swingSmooth.setTime(sampleRate / CONTROL, 50.0f);
        fadeSmooth.setTime(sampleRate / CONTROL, 5.0f);
        trigger.prepare(sampleRate);
        phrase.prepare(sampleRate, false, 100.0f);
    }

    void resetCore() override
    {
        core.reset();
        dropKey();
        trigger.reset();
        phrase.reset();
        sinceOnset = -1.0;      // (nothing has begun: a vibrato that fades in is at nothing)
        begun = false;
        fadeSmooth.snap(fadeNow());
        swingSmooth.snap(targetSwing());
    }

    void configure(const float* v) override
    {
        rate = v[Rate];
        depthCents = v[Depth];
        fadeSec = v[Fade];
        delaySec = v[4 + Delay];
        core.setRate(rate);
        core.setStereo(0.5f * v[Spread]);    // right channel up to 180 degrees ahead
        core.setShape((int) v[4 + Shape]);
        core.setTrigger((int) v[4 + Trig]);
        setThrough(v[4 + Through]);
    }

    bool coasting() const override { return false; }    // (the swing is set at every look)

    // Every look: an onset heard since the last one is taken up (here, so that it is the same
    // moment whatever the block size); the swing for the pitch depth asked for, under the fade.
    void controlCore() override
    {
        if (sinceOnset >= 0.0) sinceOnset = std::min(sinceOnset + (double) CONTROL / sampleRate, 1.0e6);
        if (begun)
        {
            begun = false;
            sinceOnset = 0.0;
            core.trigger();
        }
        // (TRIG at FREE: while the sound is away the depth is at nothing, unheard.)
        if (phrase.armed && ! core.listens()) sinceOnset = -1.0;
        const float swing = swingSmooth.next(targetSwing()) * fadeSmooth.next(fadeNow());
        core.setDelay((float) BASE_DELAY, swing, swing);
    }

    void runKeyed(float* left, float* right, int n, const float* keyLeft, const float* keyRight, float mixFrom, float mixTo) override
    {
        // Delayed vibrato: from the beginning of a sound the depth waits, then grows.
        if (core.listens())
            begun = trigger.heard(keyLeft, keyRight, n) || begun;
        else if (fadeSec > 0.0f || delaySec > 0.0f)
        {
            // TRIG at FREE: a sound that comes up out of silence, as the vibrato always heard it
            // (the two sides added; what is not a number is nothing).
            for (int i = 0; i < n; ++i)
            {
                const float both = std::abs(0.5f * (keyLeft[i] + keyRight[i]));
                begun = phrase.hears(both < 1.0f ? both : (both >= 1.0f ? 1.0f : 0.0f)) || begun;
            }
        }
        core.process(left, right, n, mixFrom, mixTo);
    }

private:
    static constexpr float MAX_SWING_SEC = 0.02f;

    // Peak delay swing in seconds for the wanted pitch deviation: a delay d(t) = A (1 + sin wt)
    // changes the pitch by the factor 1 - d'(t), so the peak deviation is A w.
    float targetSwing() const
    {
        float ratio = std::exp2(depthCents / 1200.0f) - 1.0f;
        return std::min(MAX_SWING_SEC, ratio / (dsp::TWO_PI_F * rate));
    }

    // How much of the depth there is now: none during DLAY, then a straight line over FADE.
    float fadeNow() const
    {
        if (fadeSec <= 0.0f && delaySec <= 0.0f) return 1.0f;
        if (sinceOnset < 0.0) return 0.0f;
        const float t = (float) sinceOnset - delaySec;
        return t <= 0.0f ? 0.0f : (fadeSec > 0.0f ? std::min(1.0f, t / fadeSec) : 1.0f);
    }

    float rate = 5.0f;
    float depthCents = 25.0f;
    float fadeSec = 0.0f, delaySec = 0.0f;

    fxb::Smoother swingSmooth, fadeSmooth;
    fxm::Trigger trigger;       // every onset of the key (TRIG away from FREE)
    fxm::Onset phrase;          // TRIG at FREE: only a sound out of silence
    double sinceOnset = -1.0;
    bool begun = false;         // an onset was heard since the last look
};
