#pragma once

#include "ReverbParts.h"

// The hall reverb (HALL): four allpass diffusers, then four long lines mixed by a Householder
// matrix, each read by a head that its own slow sine moves, damped on the way back. It is
// rvb::setting::hall() of the one network (ReverbParts.h), but it still plays its own pinned loop,
// rvb::Network, which with MOD at 0.3, SHIM at 0 and HOLD off does, operation for operation, what
// the hall always did (the suites "P01 Keyboard" and "Pinned On Cores" hold that in place), until
// the owner has compared the two by ear (rvb::network::forPinned puts it on the network). A turned
// SIZE or PDLY is glided to where it used to jump.
//
// DCAY  time to die away
// SIZE  the hall's size: every length scaled
// DAMP  the top of the spectrum dies sooner
// PDLY  pre-delay: the hall answers this much later
// FX2:
// MOD   how far the slow sines move the heads: 0 = a still hall (it rings more), up = the tail is chorused
// SHIM  how much of what comes round is read at another pitch (the shimmer reverb's heads, in the hall)
// MODE  that pitch: an octave up, a fifth up, an octave and a fifth up, an octave down, backwards,
//       two octaves down; HOLD = as the HOLD cell
// HOLD  ON = the way in shuts and the lines lose nothing: what is in them goes round until HOLD is OFF
class HallReverb : public CellEffect
{
public:
    // (The numbers setParameter / getParameter always took: the four cells, then the head
    // modulation - now also FX2's MOD - and the dry / wet mix, which is the slot's WET.)
    enum Param { DecayTime = 0, Size, Damping, PreDelay, Modulation, Mix };
    enum Extra { Mod = 0, Shimmer, ShiftMode, Hold };
    static constexpr float MAX_PRE_MS = 100.0f;      // (the PDLY cell's range)
    static constexpr float HALL_TO_RT60 = 2.65f;

    HallReverb()
    {
        seed();
        setWetOnly(false);     // pinned (P01): it mixes by itself in a chain too, sample for sample as it always did
        // (The object as it always started, which is not what its cells call their defaults.)
        setCell(DecayTime, 3.0f);
        setCell(Damping, 0.5f);
        setCell(PreDelay, 20.0f);
    }

    void setParameter(int id, float value) override
    {
        if (id == Modulation)  setExtra(Mod, value);
        else if (id == Mix)    setMix(value);
        else                   CellEffect::setParameter(id, value);
    }

    float getParameter(int id) const override
    {
        if (id == Modulation) return getExtra(Mod);
        if (id == Mix)        return getMix();
        return CellEffect::getParameter(id);
    }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "DCAY", 0, 0.5f, 10, 2.5f, K::Log, "s" },
            { "SIZE", 1, rvb::Front::MIN_SIZE, rvb::Front::MAX_SIZE, 1, K::Linear, "x" },
            { "DAMP", 2, 0, 1, 0.4f },
            { "PDLY", 3, 0, MAX_PRE_MS, 10, K::Linear, "ms" },
            { "MOD", 0, 0, 1, 0.3f },
            { "SHIM", 1, 0, 1, 0 },
            { "MODE", 2, 0, (float) (rvb::shift::Count - 1), 0, K::Choice, "", rvb::shift::names(), rvb::shift::Count },
            { "HOLD", 3, 0, 1, 0, K::Choice, "", ParamChoices::offOn, 2 } };
        return c;
    }

    void configure(const float* v) override
    {
        const float speed = rvb::shift::speedOf((int) std::lround(v[4 + ShiftMode]));
        if (onNet)
        {
            // (The hall's DCAY always was the time to fall by 26 dB, e^-3, and its gain came from the
            // shortest line, so the longer ones rang on: measured, its tail fell by 60 dB in 2.65 x
            // DCAY. The network's DCAY is the time to fall by 60 dB on every line.)
            net.setDecay(v[DecayTime] * HALL_TO_RT60);
            net.setSize(v[Size]);
            net.setDamping(v[Damping]);
            net.setPreDelay(v[PreDelay]);
            net.setModulation(v[4 + Mod]);
            net.setShimmer(v[4 + Shimmer], speed);
            net.setHold(v[4 + Hold] > 0.5f);
            return;
        }
        hall.setDecay(v[DecayTime]);
        hall.setSize(v[Size]);
        hall.setDamping(v[Damping]);
        hall.setPreDelay(v[PreDelay]);
        hall.setModulation(v[4 + Mod]);
        hall.setShimmer(v[4 + Shimmer], speed);
        hall.setHold(v[4 + Hold] > 0.5f);
    }

    void prepareCore() override
    {
        if (onNet != rvb::network::forPinned) { onNet = rvb::network::forPinned; touch(); }
        if (onNet) net.prepare(sampleRate, rvb::setting::hall());
        else       hall.prepare(sampleRate);
    }
    void resetCore() override { if (onNet) net.reset(); else hall.reset(); }
    void controlCore() override { if (onNet) net.control(); else hall.control(); }
    bool coasting() const override { return onNet ? net.resting() : hall.resting(); }
    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override
    {
        if (onNet) net.run(left, right, n, mixFrom, mixTo);
        else       hall.run(left, right, n, mixFrom, mixTo);
    }

private:
    // PINNED: the hall's own loop, until the owner has heard it against the network's hall.
    rvb::Network hall;
    rvb::Net net;
    bool onNet = false;
};
