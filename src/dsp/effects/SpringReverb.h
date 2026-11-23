#pragma once

#include "ReverbParts.h"

// Spring reverb: rvb::setting::spring() of the one network (ReverbParts.h). Two springs of different lengths, one fed
// by each side; each is a loop of a delay (the travel time along the spring), the dispersion that
// turns a click into a chirp a little more on every round trip (the "boing"), and a low-pass.
//
// DCAY  time to die away
// DISP  how strongly the spring disperses: 0 = plain echoes, up = every round trip chirps more
// TENS  the spring's tension: slack = long, slow round trips; tight = short and fast. Turned under
//       sound the pitch of what is in the springs bends (a shaken spring)
// DAMP  the top of the spectrum dies sooner
// FX2:
// PDLY  pre-delay: the springs answer this much later
// SHIM  how much of what comes round is read at another pitch (the shimmer reverb's heads, in a spring)
// MODE  that pitch: an octave up, a fifth up, an octave and a fifth up, an octave down
// MOD   the effect LFO moves the springs' lengths: the tail wavers
class SpringReverb : public rvb::Family
{
public:
    enum Param { Decay = 0, Dispersion, Tension, Damping };
    enum Extra { PreDelay = 0, Shimmer, ShiftMode, Modulation };

    static constexpr float MAX_PRE_DELAY = 0.001f * rvb::Front::MAX_PRE_MS;     // seconds

    SpringReverb() { seed(); }

    // The springs' length for a tension: x 1.6 slack, x 0.4 tight.
    static float sizeFor(float tension) { return 1.6f * std::pow(0.25f, std::clamp(tension, 0.0f, 1.0f)); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "DCAY", 0, 0.1f, 5, 1.5f, K::Log, "s" },
            { "DISP", 1, 0, 1, 0.6f },
            { "TENS", 2, 0, 1, 0.5f },
            { "DAMP", 3, 0, 1, 0.4f },
            ParamSpec { "PDLY", 0, 0, MAX_PRE_DELAY, 0, K::Linear, "s" }.timed(),      // (E6: it can follow the tempo)
            { "SHIM", 1, 0, 1, 0 },
            { "MODE", 2, 0, (float) (rvb::shift::Count - 1), 0, K::Choice, "", rvb::shift::names(), rvb::shift::Count },
            { "MOD", 3, 0, 1, 0 } };
        return c;
    }

    rvb::Net::Setup setup() const override { return rvb::setting::spring(); }

    void configure(const float* v) override
    {
        core.setDecay(v[Decay]);
        core.setDispersion(v[Dispersion]);
        core.setSize(sizeFor(v[Tension]));
        core.setDamping(v[Damping]);
        core.setPreDelay(1000.0f * v[4 + PreDelay]);
        core.setShimmer(v[4 + Shimmer], rvb::shift::speedOf((int) std::lround(v[4 + ShiftMode])));
        core.setModulation(v[4 + Modulation]);
    }
};
