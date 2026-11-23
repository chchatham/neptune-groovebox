#pragma once

#include "DelayCore.h"

// Left / centre / right delay: a setting of dly::Core (DelayCore.h). One line (the two inputs
// summed) read at three taps. The left tap is heard on the left only, the right tap on the right
// only and the centre tap on both (at -3 dB). Feedback is taken from the centre tap through a
// low-pass (the classic L/C/R topology of rack multi-effects).
//
// TIME  centre tap time
// FDBK  feedback from the centre tap
// SPRD  how far the side taps sit from the centre: left = TIME x (1 - SPRD/2),
//       right = TIME x (1 + SPRD/2). 0 = all three together, 1 = half / whole / one and a half
//       (eighth, quarter, dotted quarter), 0.67 = triplet spacing
// MIX   dry / wet
// FX2:
// WOW   the tape transport's wow and flutter on all three taps (0 = a steady line)
// DRIV  the tape stage in the loop: 0 = clean; up, every repeat is squashed, thickened and duller
// SAT   the curve DRIV works: any of src/dsp/Saturation.h
// READ  FWD, REV, OCT+ / 5TH+ / OCT- (see DelayCore.h)
class LCRDelay : public dly::Family
{
public:
    enum Param { Time = 0, Feedback, Spread, Mix };
    enum Extra { Wow = 0, Drive, Sat, Read };

    static constexpr float MIN_TIME = 0.02f;
    static constexpr float MAX_TIME = 1.0f;
    static constexpr float CENTRE_GAIN = 0.70710678f;

    LCRDelay() { seed(); }

    // Tap times in seconds for the current settings (left, centre, right).
    float getLeftTime() const { return cell(Time) * (1.0f - 0.5f * cell(Spread)); }
    float getCentreTime() const { return cell(Time); }
    float getRightTime() const { return cell(Time) * (1.0f + 0.5f * cell(Spread)); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "TIME", 0, MIN_TIME, MAX_TIME, 0.36f, K::Log, "s" },
            { "FDBK", 1, 0, 0.95f, 0.35f },
            { "SPRD", 2, 0, 1, 1.0f },
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
        s.filterInput = false;      // the first echoes are as bright as the input
        s.emphasis = s.makeup = true;
        s.glideMs = 60.0f;
        return s;
    }
    float lineSeconds() const override { return dly::roomFor(MAX_TIME * 1.5f, MAX_TIME); }

    void configure(const float* v) override
    {
        const float time = v[Time], side = 0.5f * v[Spread];
        core.setTapCount(3);
        core.setTap(0, time * (1.0f - side), 1.0f, 0.0f, 0.0f);
        core.setTap(1, time, CENTRE_GAIN, CENTRE_GAIN, 1.0f);
        core.setTap(2, time * (1.0f + side), 0.0f, 1.0f, 0.0f);
        core.setFeedback(v[Feedback]);
        core.setFilter(5000.0f, 0.0f);
        core.setTransport(v[4 + Wow]);
        dly::driven(core, v[4 + Drive], v[4 + Sat]);
        setRead(v[4 + Read], time);
    }
};
