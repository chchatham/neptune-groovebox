#pragma once

#include "ModDelay.h"

// Chorus: one to six voices of the modulated delay (ModDelay.h) on a stereo line, each with its
// own LFO a tenth apart in rate and an even share of the cycle apart in phase.
//
// RATE, DPTH, MIX, VOIC; on the second page SHPE (any LFO shape), THRU (through zero: the dry
// sound is delayed by TIME so the sweep passes through it; the chain handles the delay), SPRD
// (the right side's LFOs up to half a cycle ahead: 0 = both sides alike) and TIME (the centre
// delay; the swing is DPTH x 6 ms, or 6/7 of TIME where that is shorter).
// Through-zero chorus: THRU delays the dry by TIME and the multi-voice sweep passes through it,
// giving a deeper modulated comb filter that cancels to nothing at the crossing.
class Chorus : public moddelay::Effect
{
public:
    enum Param { Rate = 0, Depth, Mix, Voices };
    enum Extra { Shape = 0, Through, Spread, Time };

    Chorus() { seed(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "RATE", 0, 0.1f, 5.0f, 0.8f, K::Log, "Hz" },
            { "DPTH", 1, 0, 1, 0.5f },
            { "MIX", 2, 0, 1, 0.5f },
            { "VOIC", 3, 1, (float) moddelay::MAX_VOICES, 2, K::Int },
            fxm::Lfo::shapeSpec("SHPE", Shape),
            thruSpec(Through),
            { "SPRD", Spread, 0, 1, 0.0f },
            { "TIME", Time, 0.001f, 0.02f, 0.007f, K::Log, "s" } };
        return c;
    }
    int mixCell() const override { return Mix; }

    void prepareCore() override
    {
        core.setSpread(0.1f, 1.0f, 0.0f);
        core.setRange(0.0005f, 0.027f);
        core.prepare(sampleRate, 0.03, false);
    }

    void configure(const float* v) override
    {
        const float centre = v[4 + Time];
        core.setVoices((int) v[Voices]);
        core.setRate(v[Rate]);
        core.setDelay(0.0f, centre, v[Depth] * std::min(0.006f, centre * (6.0f / 7.0f)));
        core.setStereo(0.5f * v[4 + Spread]);
        setThrough(v[4 + Through]);
        core.setShape((int) v[4 + Shape]);
    }
};
