#pragma once

#include "FxLfo.h"

// Auto-pan: an LFO (dsp/LFO.h, any of its shapes) turns the left side down as it rises and the
// right side down as it falls.
//
// RATE, DPTH (at 100 the sound goes all the way to either side), SHPE, PHAS (where in its cycle the
// LFO stands: with SYNC on, which side the beat falls on).
// The shapes that only go one way (EXPU, EXPD) lean to one side and come back; SQR hops between
// the sides, RND picks a place per cycle, USR plays the track's eight places. Steps are rounded
// over a millisecond.
// On the second page the LFO's trigger, as the tremolo has it: TRIG (FREE, KEY: every beginning
// sound starts the sweep again from PHAS, ONE / HALF: one sweep and stop, HOLD: the place it has
// is kept until the next onset: every note at another place), DLAY, FADE (a fade from the middle),
// KEY (whose onsets: SELF, or IN = the track's input).
class AutoPan : public fxm::KeyedCells
{
public:
    enum Param
    {
        Rate = 0,       // 0.1 - 20 Hz
        Depth,          // 0 - 1
        Shape,          // an LFO shape (dsp/LFO.h)
        Phase           // 0 - 1 (the LFO's place in its cycle)
    };

    enum Extra { Trig = 0, Delay, Fade, Key };

    AutoPan() { seed(); }

    void setUserShape(const float* levels_) override { levels.lfo.setUserShape(levels_); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "RATE", 0, 0.1f, 20.0f, 2.0f, K::Log, "Hz" },
            { "DPTH", 1, 0, 1, 0.7f },
            fxm::Lfo::shapeSpec("SHPE", 2),
            { "PHAS", 3, 0, 1, 0 },
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
        levels.lfo.setPhase(v[Phase]);
        levels.setTrigger(v[4 + Trig], v[4 + Delay], v[4 + Fade]);
        depth = v[Depth];
    }

    bool coasting() const override { return ! levels.lfo.listens(); }      // (as the tremolo)
    void runKeyed(float* left, float* right, int n, const float* keyLeft, const float* keyRight, float mixFrom, float mixTo) override
    {
        const float half = 0.5f * depth;
        levels.process(left, right, n, keyLeft, keyRight, mixFrom, mixTo, [half](float v, float, float& wantL, float& wantR)
        {
            wantL = 1.0f - half * (1.0f + v);
            wantR = 1.0f - half * (1.0f - v);
        });
    }

private:
    fxm::Levels levels;
    float depth = 0.7f;
};
