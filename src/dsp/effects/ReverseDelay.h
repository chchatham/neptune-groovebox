#pragma once

#include "DelayCore.h"

// Reverse delay: a setting of dly::Core (DelayCore.h) with its heads running backwards. Each
// stretch of TIME is heard backwards as soon as it is complete; a head crossfades into the next
// over a tenth of the stretch (no gap between two stretches).
//
// TIME  the length of a stretch
// FDBK  feedback: what was heard backwards goes round again (and so comes back forwards)
// HCUT  low-pass on everything that comes back
// MIX   dry / wet
// FX2:
// TAPS  1..4 heads, TIME / TAPS apart: each plays its own stretches backwards, so everything is heard that
//       many times on a grid of TIME / TAPS, quieter and to alternate sides
// DRIV  the tape stage in the loop: 0 = clean; up, every round is squashed, thickened and duller
// SAT   the curve DRIV works: any of src/dsp/Saturation.h
// READ  REV to start with; FWD makes it a plain echo, OCT+ / 5TH+ / OCT- shift every repeat instead
class ReverseDelay : public dly::Family
{
public:
    enum Param { Time = 0, Feedback, HighCut, Mix };
    enum Extra { Taps = 0, Drive, Sat, Read };

    static constexpr float MIN_TIME = 0.1f, MAX_TIME = 2.0f;
    static constexpr int MAX_TAPS = 4;

    ReverseDelay() { seed(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "TIME", 0, MIN_TIME, MAX_TIME, 0.5f, K::Log, "s" },
            { "FDBK", 1, 0, 0.95f, 0.3f },
            { "HCUT", 2, 500, 20000, 8000, K::Log, "Hz" },
            { "MIX", 3, 0, 1, 0.5f },
            { "TAPS", 0, 1, (float) MAX_TAPS, 1, K::Int },
            { "DRIV", 1, 0, 1, 0 },
            { "SAT", 2, 0, (float) (sat::Count - 1), (float) sat::Fast, K::Choice, "", sat::names(), sat::Count },
            { "READ", 3, 0, (float) (dly::read::Count - 1), (float) dly::read::Reverse, K::Choice, "", dly::read::names(), dly::read::Count } };
        return c;
    }
    int mixCell() const override { return Mix; }
    dly::Core::Setup setup() const override
    {
        dly::Core::Setup s;
        s.hermite = false;
        s.emphasis = s.makeup = true;
        s.glideMs = 60.0f;
        return s;
    }
    float lineSeconds() const override { return dly::roomFor(MAX_TIME, MAX_TIME); }

    void configure(const float* v) override
    {
        const float time = v[Time];
        const int n = std::clamp((int) std::lround(v[4 + Taps]), 1, MAX_TAPS);
        core.setTapCount(n);
        for (int i = 0; i < n; ++i)
        {
            // The first in the middle and feeding back, the rest quieter and to alternate sides.
            const float level = n > 1 ? 1.0f - 0.5f * (float) i / (float) (n - 1) : 1.0f;
            const float far = i == 0 ? level : 0.4f * level;
            core.setTap(i, time * (float) (i + 1) / (float) n, i % 2 == 1 ? level : far, i % 2 == 1 ? far : level, i == 0 ? 1.0f : 0.0f);
        }
        core.silenceTapsFrom(n);
        core.setFeedback(v[Feedback]);
        core.setFilter(v[HighCut], 0.0f);
        core.setTransport(0.0f);
        dly::driven(core, v[4 + Drive], v[4 + Sat]);
        setRead(v[4 + Read], time);
    }
};
