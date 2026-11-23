#pragma once

#include "DelayCore.h"

// Multi-tap delay: a setting of dly::Core (DelayCore.h). One line (the two inputs summed) read
// at up to eight evenly spaced taps.
//
// TIME  time of the last tap; the taps are spaced evenly up to it (tap i at TIME x i / TAPS)
// TAPS  number of taps, 1..8; they alternate left / right and get quieter towards the last
// FDBK  feedback from the last tap, through a low-pass: the whole pattern comes round again every TIME
// MIX   dry / wet
// FX2:
// WOW   the tape transport's wow and flutter on every tap (0 = a steady line)
// DRIV  the tape stage in the loop: 0 = clean; up, every round is squashed, thickened and duller
// SAT   the curve DRIV works: any of src/dsp/Saturation.h
// READ  FWD, REV, OCT+ / 5TH+ / OCT- (see DelayCore.h)
class MultiTapInsert : public dly::Family
{
public:
    enum Param { Time = 0, Taps, Feedback, Mix };
    enum Extra { Wow = 0, Drive, Sat, Read };

    static constexpr int MAX_TAPS = dly::Core::MAX_TAPS;
    static constexpr float MIN_TIME = 0.05f, MAX_TIME = 2.0f;

    MultiTapInsert() { seed(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "TIME", 0, MIN_TIME, MAX_TIME, 0.5f, K::Log, "s" },
            { "TAPS", 1, 1, (float) MAX_TAPS, 4, K::Int },
            { "FDBK", 2, 0, 0.9f, 0.3f },
            { "MIX", 3, 0, 1, 0.4f },
            { "WOW", 0, 0, 1, 0 },
            { "DRIV", 1, 0, 1, 0 },
            { "SAT", 2, 0, (float) (sat::Count - 1), (float) sat::Fast, K::Choice, "", sat::names(), sat::Count },
            { "READ", 3, 0, (float) (dly::read::Count - 1), 0, K::Choice, "", dly::read::names(), dly::read::Count } };
        return c;
    }
    int mixCell() const override { return Mix; }
    dly::Core::Setup setup() const override
    {
        dly::Core::Setup s;
        s.stereo = false;
        s.hermite = false;          // eight reads a sample: the straight line
        s.filterInput = false;
        s.emphasis = s.makeup = true;
        s.glideMs = 60.0f;
        return s;
    }
    float lineSeconds() const override { return dly::roomFor(MAX_TIME, MAX_TIME); }

    void configure(const float* v) override
    {
        const float time = v[Time];
        const int n = std::clamp((int) std::lround(v[Taps]), 1, MAX_TAPS);
        core.setTapCount(n);
        for (int i = 0; i < n; ++i)
        {
            const float level = n > 1 ? 1.0f - 0.6f * (float) i / (float) (n - 1) : 1.0f;
            const float pan = n == 1 ? 0.5f : (i % 2 == 0 ? 0.15f : 0.85f);
            core.setTap(i, time * (float) (i + 1) / (float) n, level * (1.0f - pan), level * pan, i == n - 1 ? 1.0f : 0.0f);
        }
        core.silenceTapsFrom(n);
        core.setFeedback(v[Feedback]);
        core.setFilter(8000.0f, 0.0f);
        core.setTransport(v[4 + Wow]);
        dly::driven(core, v[4 + Drive], v[4 + Sat]);
        setRead(v[4 + Read], time);
    }
};
