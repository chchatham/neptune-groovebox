#pragma once

#include "ReverbParts.h"

// Shimmer reverb: rvb::setting::shimmer() of the one network (ReverbParts.h). Three input diffusers, then a tank of two
// long loops that feed each other; what comes round is read by two moving heads at another pitch
// (the delay-line pitch shifter), so the tail climbs away interval on interval.
//
// DCAY  time to die away
// SHIM  how much of what comes round is read at the other pitch
// MODE  that pitch: an octave up, a fifth up, an octave and a fifth up, an octave down
// DAMP  the top of the spectrum dies sooner
// FX2:
// SIZE  the tank's size (its loops' length): turned under sound, the pitch of what is in it bends
// PDLY  pre-delay: the tank answers this much later
// DISP  a spring's dispersion inside the tank: every round trip chirps more (0 = none, and it costs nothing)
// MOD   the effect LFO moves the allpass inside each loop: the tail is chorused instead of ringing
class ShimmerReverb : public rvb::Family
{
public:
    enum Param { Decay = 0, Shimmer, ShiftMode, Damping };
    enum Extra { Size = 0, PreDelay, Dispersion, Modulation };

    static constexpr float MAX_PRE_DELAY = 0.001f * rvb::Front::MAX_PRE_MS;     // seconds

    ShimmerReverb() { seed(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "DCAY", 0, 0.3f, 10, 3, K::Log, "s" },
            { "SHIM", 1, 0, 1, 0.4f },
            { "MODE", 2, 0, (float) (rvb::shift::Count - 1), 0, K::Choice, "", rvb::shift::names(), rvb::shift::Count },
            { "DAMP", 3, 0, 1, 0.4f },
            { "SIZE", 0, 0.25f, 2, 1, K::Log, "x" },
            ParamSpec { "PDLY", 1, 0, MAX_PRE_DELAY, 0, K::Linear, "s" }.timed(),      // (E6: it can follow the tempo)
            { "DISP", 2, 0, 1, 0 },
            { "MOD", 3, 0, 1, 0.25f } };
        return c;
    }

    rvb::Net::Setup setup() const override { return rvb::setting::shimmer(); }

    void configure(const float* v) override
    {
        core.setDecay(v[Decay]);
        core.setShimmer(v[Shimmer], rvb::shift::speedOf((int) std::lround(v[ShiftMode])));
        core.setDamping(v[Damping]);
        core.setSize(v[4 + Size]);
        core.setPreDelay(1000.0f * v[4 + PreDelay]);
        core.setDispersion(v[4 + Dispersion]);
        core.setModulation(v[4 + Modulation]);
    }
};
