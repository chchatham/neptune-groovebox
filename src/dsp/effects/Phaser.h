#pragma once

#include "SweptFilter.h"

// Allpass cascade phaser: a setting of the swept filter (SweptFilter.h). Two to six first-order
// allpass stages a side, their break frequency swept between 200 Hz and 4 kHz by an LFO (dsp/LFO.h,
// any of its shapes), with a feedback path. The notches come from the stages' output meeting the
// dry sound: the slot's WET starts at half.
//
// RATE, DPTH, FDBK, STGS; on the second page SHPE, SPRD (the right side's sweep up to half a
// cycle ahead of the left), and what the wah always had:
// ENV   the level of the key moves the sweep: up (to the right of the middle) or down, by the
//       whole of the LFO's range at either end. With DPTH at 0 the phaser is played by the level alone
// KEY   whose level that is: SELF, or IN = the track's input (another track's level sweeps the phaser)
class Phaser : public swp::Effect
{
public:
    enum Param
    {
        Rate = 0,       // 0.1 - 5 Hz
        Depth,          // 0 - 1
        Feedback,       // -0.9 to 0.9
        Stages          // 2 to 6
    };
    enum Extra { Shape = 0, Spread, Env, Key };

    Phaser()
    {
        mix = 0.5f;
        seed();
    }

    int keyCell() const override { return 4 + Key; }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "RATE", 0, 0.1f, 5.0f, 0.5f, K::Log, "Hz" },
            { "DPTH", 1, 0, 1, 0.5f },
            { "FDBK", 2, -0.9f, 0.9f, 0.3f },
            { "STGS", 3, 2, 6, 4, K::Int },
            fxm::Lfo::shapeSpec("SHPE", Shape),
            { "SPRD", Spread, 0, 1, 0.0f },
            { "ENV", Env, -1, 1, 0.0f },
            dyn::keySpec(Key) };
        return c;
    }

    swp::Core::Voicing voicing() const override
    {
        swp::Core::Voicing v;
        v.law = swp::Core::Hertz;
        v.resoMs = v.depthMs = 5.0f;
        v.lowHz = 50.0f;
        return v;
    }

    void configure(const float* v) override
    {
        core.setFilter(swp::filt::Phaser, (int) v[Stages]);
        core.setBase(0.5f * (MIN_HZ + MAX_HZ));
        core.setResonance(v[Feedback]);
        core.setLfo(v[Rate], (int) v[4 + Shape], 0.5f * (MAX_HZ - MIN_HZ) * v[Depth], 0.5f * v[4 + Spread]);
        core.setEnvelope(v[4 + Env] * (MAX_HZ - MIN_HZ));
    }

private:
    static constexpr float MIN_HZ = 200.0f, MAX_HZ = 4000.0f;
};
