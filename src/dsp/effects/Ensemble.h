#pragma once

#include "ModDelay.h"

// Ensemble: the bucket-brigade string-ensemble chorus as a setting of the modulated delay
// (ModDelay.h). The middle of the input goes into one line; one to six voices read it, each with
// its own LFO (13 % apart in rate, an even share of the cycle apart in phase) and its own place
// between left and right.
//
// RATE, DPTH, WDTH (how far apart the voices sit), MIX; on the second page SHPE (any LFO shape:
// TRI is the classic), THRU (through zero: the dry sound is delayed by TIME so the sweep passes
// through it; the chain handles the delay), VOIC and TIME (the centre delay; the swing is
// DPTH x 3 ms, or 0.6 of TIME where that is shorter).
// Through-zero ensemble: THRU delays the dry by TIME, so the multi-voice bucket-brigade
// sweep passes through it and the output cancels at the crossing.
class Ensemble : public moddelay::Effect
{
public:
    enum Param { Rate = 0, Depth, Width, Mix };
    enum Extra { Shape = 0, Through, Voices, Time };

    Ensemble() { seed(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "RATE", 0, 0.1f, 3.0f, 0.6f, K::Log, "Hz" },
            { "DPTH", 1, 0, 1, 0.5f },
            { "WDTH", 2, 0, 1, 0.8f },
            { "MIX", 3, 0, 1, 0.5f },
            fxm::Lfo::shapeSpec("SHPE", Shape, LFO::Shape::Triangle),
            thruSpec(Through),
            { "VOIC", Voices, 1, (float) moddelay::MAX_VOICES, 3, K::Int },
            { "TIME", Time, 0.001f, 0.015f, 0.005f, K::Log, "s" } };
        return c;
    }
    int mixCell() const override { return Mix; }

    void prepareCore() override
    {
        core.setSpread(0.13f, 1.0f, 0.7f);
        core.setRange(0.0005f, 0.019f);
        core.prepare(sampleRate, 0.022, true);
    }

    void configure(const float* v) override
    {
        const float centre = v[4 + Time];
        core.setVoices((int) v[4 + Voices]);
        core.setRate(v[Rate]);
        core.setDelay(0.0f, centre, v[Depth] * std::min(0.003f, centre * 0.6f));
        core.setWidth(v[Width]);
        setThrough(v[4 + Through]);
        core.setShape((int) v[4 + Shape]);
    }
};
