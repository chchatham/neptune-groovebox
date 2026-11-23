#pragma once

#include "ModDelay.h"

// Flanger: one voice of the modulated delay (ModDelay.h) with feedback, sweeping from next to
// nothing up to twice its centre delay.
//
// RATE, DPTH, FDBK (either polarity), MIX; on the second page SHPE (any LFO shape), SPRD (the right
// side's sweep up to half a cycle ahead), THRU (through zero: the dry sound is delayed by TIME, so
// the sweep passes through it and the two add or cancel completely at the crossing; the whole
// effect is then TIME late) and TIME (the centre delay, the "manual" of a flanger pedal).
class Flanger : public moddelay::Effect
{
public:
    enum Param { Rate = 0, Depth, Feedback, Mix };
    enum Extra { Shape = 0, Spread, Through, Time };

    Flanger() { seed(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "RATE", 0, 0.1f, 5.0f, 0.5f, K::Log, "Hz" },
            { "DPTH", 1, 0, 1, 0.5f },
            { "FDBK", 2, -0.95f, 0.95f, 0.3f },
            { "MIX", 3, 0, 1, 0.5f },
            fxm::Lfo::shapeSpec("SHPE", Shape),
            { "SPRD", Spread, 0, 1, 0.0f },
            thruSpec(Through),
            { "TIME", Time, 0.0005f, 0.01f, 0.003f, K::Log, "s" } };
        return c;
    }
    int mixCell() const override { return Mix; }

    void prepareCore() override
    {
        core.setVoices(1);
        core.setRange(0.0001f, 0.02f);
        core.prepare(sampleRate, 0.022, false);
    }

    void configure(const float* v) override
    {
        const float centre = v[4 + Time];
        core.setRate(v[Rate]);
        core.setDelay(0.0f, centre, v[Depth] * centre);
        core.setFeedback(v[Feedback]);
        core.setStereo(0.5f * v[4 + Spread]);
        setThrough(v[4 + Through]);
        core.setShape((int) v[4 + Shape]);
    }
};
