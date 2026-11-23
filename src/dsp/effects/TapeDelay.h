#pragma once

#include "DelayCore.h"

// Tape-style echo: a setting of dly::Core (DelayCore.h). Each side has its own line, read with a
// Hermite curve at a position that glides when TIME changes (pitch slides like a tape changing
// speed) and wobbles with wow (0.6 Hz) and flutter (7 Hz); the repeats pass a low-pass and a
// high-pass each time round; the signal going onto the tape is saturated, which also holds the loop.
//
// TIME  echo time
// FDBK  0..1 -> loop gain 0..1.1: the top of the range self-oscillates, held by the saturation
// AGE   0..1: more wow and flutter, harder saturation, duller and thinner repeats
// MIX   dry / wet
// FX2:
// HEAD  1..4 playback heads, evenly spaced up to TIME (the last one feeds back; the earlier ones are a little quieter)
// WIDE  the right side's heads sit up to 20 ms later than the left's
// SAT   the tape's saturation curve: any of src/dsp/Saturation.h (FAST = the tape as it always was)
// READ  FWD, REV (each stretch of TIME comes back backwards), OCT+ / 5TH+ / OCT- (every repeat higher or lower)
class TapeDelay : public dly::Family
{
public:
    enum Param { Time = 0, Feedback, Age, Mix };
    enum Extra { Head = 0, Wide, Sat, Read };

    static constexpr float MIN_TIME = 0.02f;
    static constexpr float MAX_TIME = 1.5f;
    static constexpr int MAX_HEADS = 4;
    static constexpr float MAX_LOOP_GAIN = 1.1f;

    TapeDelay() { seed(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "TIME", 0, MIN_TIME, MAX_TIME, 0.3f, K::Log, "s" },
            { "FDBK", 1, 0, 1, 0.45f },
            { "AGE", 2, 0, 1, 0.4f },
            { "MIX", 3, 0, 1, 0.35f },
            { "HEAD", 0, 1, (float) MAX_HEADS, 1, K::Int },
            { "WIDE", 1, 0, 1, 0 },
            { "SAT", 2, 0, (float) (sat::Count - 1), (float) sat::Fast, K::Choice, "", sat::names(), sat::Count },
            { "READ", 3, 0, (float) (dly::read::Count - 1), 0, K::Choice, "", dly::read::names(), dly::read::Count } };
        return c;
    }
    int mixCell() const override { return Mix; }
    dly::Core::Setup setup() const override { return {}; }      // (the core's own: two lines, Hermite, the filter before the line)
    float lineSeconds() const override { return dly::roomFor(MAX_TIME, MAX_TIME); }

    void configure(const float* v) override
    {
        const float time = v[Time], age = v[Age];
        dly::heads(core, time, (int) std::lround(v[4 + Head]), MAX_HEADS);
        core.setFeedback(v[Feedback] * MAX_LOOP_GAIN);
        core.setWide(v[4 + Wide] * dly::MAX_WIDE);
        core.setTransport(age);                                 // (full depth at AGE 1)
        core.setFilter(9000.0f * std::pow(2200.0f / 9000.0f, age), 40.0f * std::pow(180.0f / 40.0f, age));
        core.setDrive(1.0f, 1.0f + 2.0f * age);
        core.setCurve((int) std::lround(v[4 + Sat]), 0.0f);
        setRead(v[4 + Read], time);
    }
};
