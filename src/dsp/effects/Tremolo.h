#pragma once

#include "FxLfo.h"

// Tremolo: the level follows an LFO (dsp/LFO.h, any of its shapes).
//
// RATE, DPTH (at 100 the level goes down to nothing at the bottom of the shape), SHPE, STER (the
// right side's LFO this share of a cycle ahead: 50 = opposite).
// The shapes that only go one way are rhythm, not wobble: EXPD starts every cycle at full level and
// falls away (a pluck), EXPU starts it down and swells (with SYNC on, the pump of a side-chained
// compressor). SQR, RND and USR are a gate, a random level and a level sequence; their steps are
// rounded over a millisecond.
// On the second page the LFO's trigger (dsp/LFO.h), heard from the key:
// TRIG  FREE: the LFO runs by itself. KEY: every beginning sound starts the cycle again. ONE: it
//       plays one cycle and stops at its end (EXPD: a pluck that stays shut, EXPU: a dip that
//       comes back and stays: with KEY at IN the pump of the other track's own hits). HALF: half a
//       cycle. HOLD: the level it has at the onset is kept until the next
// DLAY  after an onset the level stays untouched this long
// FADE  and the tremolo then grows over this time
// KEY   whose onsets: SELF, or IN = the track's input
class Tremolo : public fxm::KeyedCells
{
public:
    enum Param
    {
        Rate = 0,       // 0.1 - 20 Hz
        Depth,          // 0 - 1
        Shape,          // an LFO shape (dsp/LFO.h)
        StereoOffset    // 0 - 1 (phase offset between L/R, 0.5 = 180 degrees)
    };

    enum Extra { Trig = 0, Delay, Fade, Key };

    Tremolo() { seed(); }

    void setUserShape(const float* levels_) override { levels.lfo.setUserShape(levels_); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "RATE", 0, 0.1f, 20.0f, 4.0f, K::Log, "Hz" },
            { "DPTH", 1, 0, 1, 0.5f },
            fxm::Lfo::shapeSpec("SHPE", 2),
            { "STER", 3, 0, 1, 0 },
            fxm::Lfo::triggerSpec("TRIG", Trig), fxm::delaySpec(Delay), fxm::fadeSpec(Fade), dyn::keySpec(Key) };
        return c;
    }

    int keyCell() const override { return 4 + Key; }
    void prepareCore() override { levels.prepare(sampleRate); }
    void resetCore() override { levels.reset(); dropKey(); }

    void configure(const float* v) override
    {
        levels.lfo.setRate(v[Rate]);
        levels.lfo.setShape((int) v[Shape]);
        levels.lfo.setOffset(v[StereoOffset]);
        levels.setTrigger(v[4 + Trig], v[4 + Delay], v[4 + Fade]);
        depth = v[Depth];
        unipolar = levels.lfo.isUnipolar();
    }

    // (Nothing here waits for controlCore(): the LFO counts its own steps. While it listens for
    // onsets the runs stay short, so a trigger is taken within a millisecond.)
    bool coasting() const override { return ! levels.lfo.listens(); }
    void runKeyed(float* left, float* right, int n, const float* keyLeft, const float* keyRight, float mixFrom, float mixTo) override
    {
        const float d = depth;
        const bool oneWay = unipolar;
        // The shape as a share of the way up, 0..1: the level is 1 at the top, 1 - depth at the bottom.
        levels.process(left, right, n, keyLeft, keyRight, mixFrom, mixTo, [d, oneWay](float a, float b, float& wantL, float& wantR)
        {
            wantL = 1.0f - d * (1.0f - (oneWay ? a : 0.5f * (a + 1.0f)));
            wantR = 1.0f - d * (1.0f - (oneWay ? b : 0.5f * (b + 1.0f)));
        });
    }

private:
    fxm::Levels levels;
    float depth = 0.5f;
    bool unipolar = false;
};
