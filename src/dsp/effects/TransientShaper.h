#pragma once

#include "Detector.h"

// Transient shaper, TRNS: makes the attacks and the tails of a sound louder or softer without a
// threshold, so it works the same on a quiet sound as on a loud one (the differential-envelope
// idea of the hardware transient designers; structure as in Zoelzer, "DAFX", dynamics chapter).
//
// The key's level (dyn::Detector, peak) is held over its own ripple: the largest level of the last
// 10 to 20 ms (dyn::PeakHold), let go with a short release. That envelope, p, is followed three
// ways (dyn::Follower each):
//   fast   rises to p in a millisecond or so, falls with it
//   slow   rises to p in tens of milliseconds, falls with it
//   tail   rises with p, falls several times slower
// While the level rises, fast runs ahead of slow: (fast - slow) / fast is the attack, 0..1. While it
// falls, p drops under tail: (tail - p) / tail is the tail, 0..1. On a steady sound both are
// exactly 0. Each has its own gain and the two are multiplied:
//   1 + 3 ATK attack   (ATK below 0: 1 / (1 + 3 |ATK| attack), the same in dB the other way)
//   1 + 3 SUS tail     (likewise)
// so each knob reaches about +/- 12 dB. With both at 0 and OUT at 0 dB the sound passes untouched,
// sample for sample (the old shaper bent everything through a tanh).
//
// The gain is never switched from one to the other (it was the attack's OR the tail's until the
// review of phase E3, and stepped by up to 12 dB in one sample at the hand-over). What is left
// is where a hit begins: a ratio of two levels goes from 0 to 1 in a sample when the level comes
// out of nothing, and the tail of the hit before is let go at once. Keyed by itself, that step of
// the gain sits on the step of the sound and belongs to it (an attack is not smoothed). Keyed from
// the track's input it would be a click in a sound that has no step of its own there, so the gain
// then goes through a 1 ms smoother: a steady sound keyed by a drum gets a swell.
//
// ATK  attacks softer (-1) .. sharper (+1)        SUS  tails shorter (-1) .. longer (+1)
// SPD  how fast the followers are                 OUT  output level
// The cells are CellEffect's (every continuous one moves with modulateCell); the slot's WET leaves
// the dry sound beside the shaped one.
// KEY  (FX2) what the detector listens to: SELF, or IN = the track's input: this track gets the
//      attacks and tails of another one (a pad that swells on every drum hit, or drops under it)
class TransientShaper : public dyn::Keyed<TransientShaper>
{
public:
    enum Param
    {
        Attack = 0,     // -1 to 1 (negative = soften, positive = sharpen attacks)
        Sustain,        // -1 to 1 (negative = reduce sustain, positive = boost)
        Speed,          // 0 - 1 (envelope follower speed / sensitivity)
        Output          // -12 to 12 dB (output gain compensation)
    };
    static constexpr int KEY_EXTRA = 0;

    static constexpr float DEPTH = 3.0f;        // a knob at its end: x4 (12 dB) at d = 1
    static constexpr float HOLD_MS = 10.0f;     // bridges the half-waves of anything from 50 Hz up
    static constexpr float TAIL_FALL = 4.0f;    // the tail follower's fall against the slow one's rise

    TransientShaper() { seed(); }


    void tick(float& left, float& right, float keyLeft, float keyRight)
    {
        const float p = peak.up(held.push(detector.level(keyLeft, keyRight)));
        const float f = fast.up(p);
        const float s = slow.up(p);
        const float t = tail.up(p);
        const float gain = shaped(attackSmooth.next(attackNow), std::max(0.0f, f - s) / (f + FLOOR))
                         * shaped(sustainSmooth.next(sustainNow), (t - p) / (t + FLOOR));
        if (keyConnected()) gainSmooth.next(gain); else gainSmooth.snap(gain);
        applyGain(left, right, gainSmooth.value() * outputSmooth.next(outputNow));
    }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "ATK", 0, -1, 1, 0 },
            { "SUS", 1, -1, 1, 0 },
            { "SPD", 2, 0, 1, 0.5f },
            { "OUT", 3, -12, 12, 0, K::Linear, "dB" },
            dyn::keySpec(KEY_EXTRA),
            {}, {}, {} };
        return c;
    }

    void prepareCore() override
    {
        detector.prepare(sampleRate);
        for (auto* s : { &attackSmooth, &sustainSmooth, &outputSmooth })
            s->setTime(sampleRate, 10.0f);
        gainSmooth.setTime(sampleRate, GAIN_MS);
    }

    void resetCore() override
    {
        detector.reset();
        held.reset();
        peak.reset(0.0f);
        fast.reset(0.0f);
        slow.reset(0.0f);
        tail.reset(0.0f);
        attackSmooth.snap(attackNow);
        sustainSmooth.snap(sustainNow);
        outputSmooth.snap(outputNow);
        gainSmooth.snap(1.0f);
        dropKey();
    }

    void configure(const float* v) override
    {
        attackNow = v[Attack];
        sustainNow = v[Sustain];
        outputNow = dsp::dbToLinear(v[Output]);
        const float spd = v[Speed];
        if (spd == speedSet && sampleRate == rateSet) return;
        speedSet = spd;
        rateSet = sampleRate;
        // Slow .. fast: the envelope lets go in 40 .. 10 ms, the fast follower rises in 3 .. 0.3 ms,
        // the slow one in 60 .. 10 ms (about how long an attack lasts, as the shaper hears it), and
        // the tail falls in 240 .. 40 ms.
        peak.setTimes(sampleRate, 0.0f, lerp(40.0f, 10.0f, spd));
        held.length = std::max(1, (int) (HOLD_MS * 0.001 * sampleRate));
        const float fastMs = lerp(3.0f, 0.3f, spd), slowMs = lerp(60.0f, 10.0f, spd);
        fast.setTimes(sampleRate, fastMs, 0.0f);
        slow.setTimes(sampleRate, slowMs, 0.0f);
        tail.setTimes(sampleRate, 0.0f, TAIL_FALL * slowMs);
    }

private:
    static constexpr float FLOOR = 1.0e-6f;     // -120 dB: below it there is no attack and no tail
    static constexpr float GAIN_MS = 1.0f;      // the gain's own rise and fall under a key from outside

    // What a knob at `amount` makes of a share of 0..1: up to x4, or down to a quarter.
    static float shaped(float amount, float share)
    {
        const float x = DEPTH * std::abs(amount) * share;
        return amount >= 0.0f ? 1.0f + x : 1.0f / (1.0f + x);
    }

    float attackNow = 0.0f, sustainNow = 0.0f, outputNow = 1.0f;
    float speedSet = -1.0f;
    double rateSet = 0.0;

    dyn::Detector detector;
    dyn::PeakHold held;
    dyn::Follower peak, fast, slow, tail;
    fxb::Smoother attackSmooth, sustainSmooth, outputSmooth, gainSmooth;
};
