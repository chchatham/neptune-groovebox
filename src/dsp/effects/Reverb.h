#pragma once

#include "ReverbParts.h"

// The send reverb (RVB): a plate after Dattorro (1997) - four allpass diffusers, then two loops that
// feed each other, each with an allpass and a damping filter, heard at seven places a side. It is
// rvb::setting::plate() of the one network (ReverbParts.h), but it still plays its own pinned loop,
// rvb::Tank, which with SHIM at 0 and HOLD off does, operation for operation, what the send reverb
// always did (the suites "P01 Keyboard" and "Pinned On Cores" hold that in place), until the owner
// has compared the two by ear (rvb::network::forPinned puts it on the network). A turned SIZE is
// glided to (the pitch of what is in the tank bends) where it used to jump.
//
// DCAY  the loops' gain: how long the tail is
// DAMP  the top of the spectrum dies sooner
// SIZE  the plate's size: every length scaled
// MIX   dry / wet
// FX2:
// PDLY  pre-delay: the plate answers this much later
// SHIM  how much of what comes round is read at another pitch (the shimmer reverb's heads, in the plate)
// MODE  that pitch: an octave up, a fifth up, an octave and a fifth up, an octave down, backwards,
//       two octaves down; HOLD = as the HOLD cell
// HOLD  ON = the way in shuts and the tank loses nothing: what is in it goes round until HOLD is OFF
class Reverb : public CellEffect
{
public:
    enum Param { Decay = 0, Damping, Size, Mix };
    enum Extra { PreDelay = 0, Shimmer, ShiftMode, Hold };

    Reverb()
    {
        seed();
        setWetOnly(false);     // pinned (P01): it mixes by itself in a chain too, sample for sample as it always did
        // (The object as it always started, which is not what its cells call their defaults.)
        setCell(Decay, 0.7f);
        setCell(Damping, 0.5f);
        mix = 1.0f;
    }

    void setDecay(float d) { setCell(Decay, d); }
    void setDamping(float d) { setCell(Damping, d); }
    void setSize(float s) { setCell(Size, s); }
    void setPreDelay(float ms) { setCell(4 + PreDelay, ms); }
    void setHighCut(float hz) { tank.setHighCut(hz); net.setHighCut(hz); }      // the low-pass on the way in (not a cell: 10 kHz unless set)

    static constexpr float MAX_PRE_MS = 100.0f;      // (the PDLY cell's range)

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "DCAY", 0, 0, 0.99f, 0.6f },
            { "DAMP", 1, 0, 1, 0.4f },
            { "SIZE", 2, rvb::Front::MIN_SIZE, rvb::Front::MAX_SIZE, 1.0f, K::Linear, "x" },
            { "MIX", 3, 0, 1, 0.5f },
            { "PDLY", 0, 0, MAX_PRE_MS, 10, K::Linear, "ms" },
            { "SHIM", 1, 0, 1, 0 },
            { "MODE", 2, 0, (float) (rvb::shift::Count - 1), 0, K::Choice, "", rvb::shift::names(), rvb::shift::Count },
            { "HOLD", 3, 0, 1, 0, K::Choice, "", ParamChoices::offOn, 2 } };
        return c;
    }
    int mixCell() const override { return Mix; }

    void configure(const float* v) override
    {
        const float speed = rvb::shift::speedOf((int) std::lround(v[4 + ShiftMode]));
        if (onNet)
        {
            // DCAY is the loops' gain: here the time it takes them 60 dB down, and (as the plate
            // always had it) the loop allpasses' coefficient.
            net.setSize(v[Size]);
            net.setDecay(v[Decay] > 0.0f ? 6.9078f * net.passSeconds() / -std::log(v[Decay]) : 0.01f);
            net.setDiffusion(-v[Decay]);
            net.setDamping(v[Damping]);
            net.setPreDelay(v[4 + PreDelay]);
            net.setShimmer(v[4 + Shimmer], speed);
            net.setHold(v[4 + Hold] > 0.5f);
            return;
        }
        tank.setDecay(v[Decay]);
        tank.setDamping(v[Damping]);
        tank.setSize(v[Size]);
        tank.setPreDelay(v[4 + PreDelay]);
        tank.setShimmer(v[4 + Shimmer], speed);
        tank.setHold(v[4 + Hold] > 0.5f);
    }

    void prepareCore() override
    {
        if (onNet != rvb::network::forPinned) { onNet = rvb::network::forPinned; touch(); }
        if (onNet) net.prepare(sampleRate, rvb::setting::plate());
        else       tank.prepare(sampleRate);
    }
    void resetCore() override { if (onNet) net.reset(); else tank.reset(); }
    void controlCore() override { if (onNet) net.control(); else tank.control(); }
    bool coasting() const override { return onNet ? net.resting() : tank.resting(); }
    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override
    {
        if (onNet) net.run(left, right, n, mixFrom, mixTo);
        else       tank.run(left, right, n, mixFrom, mixTo);
    }

private:
    // PINNED: the plate's own loop, until the owner has heard it against the network's plate.
    rvb::Tank tank;
    rvb::Net net;
    bool onNet = false;
};
