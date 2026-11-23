#pragma once

// fxm::Onset as it was before it became dyn::Onset (Detector.h), kept as the reference for
// tests/router_sources_tests.h: the two must hear the same onsets on the same samples.

#include "../../src/dsp/effects/Detector.h"

struct OldOnset
{
    static constexpr float ON = 0.01f, OFF = 0.003f;
    static constexpr float RISE = 1.6f, REARM = 1.2f;

    void prepare(double sr, bool jumps = true, float releaseMs = 30.0f)
    {
        rises = jumps;
        hold.length = jumps ? std::max(1, (int) (0.025 * sr)) : 1;
        fast.setTimes(sr, 1.0f, releaseMs);
        floor.setTimes(sr, 150.0f, 30.0f);
        reset();
    }

    void reset()
    {
        hold.reset();
        fast.reset(0.0f);
        floor.reset(0.0f);
        armed = true;
    }

    // One sample of the key's level (0 or more); true on the sample an onset is heard.
    bool hears(float level)
    {
        if (! rises)
        {
            const float f = fast.up(level);
            if (armed) { if (f > ON) { armed = false; return true; } }
            else if (f < OFF) armed = true;
            return false;
        }
        const float f = fast.up(hold.push(level)), under = floor.up(f);
        if (armed)
        {
            if (f > ON && f > RISE * under)
            {
                armed = false;
                return true;
            }
        }
        else if (f < OFF || f < REARM * under)
            armed = true;
        return false;
    }

    dyn::PeakHold hold;
    dyn::Follower fast, floor;
    bool armed = true;          // nothing is sounding (or what sounds has settled): the next rise is an onset
    bool rises = true;
};

