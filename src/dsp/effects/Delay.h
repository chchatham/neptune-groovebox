#pragma once

#include "DelayCore.h"

// The send delay (DLY): a setting of dly::Core (DelayCore.h) with Setup::send - one tap on each
// side, the loop filter (low-pass 8 kHz, high-pass 80 Hz) at the heads, each side fed by itself or
// by the other. With READ at FWD, DRIV and AGE at 0 and MODE at STER or PING it does, operation
// for operation, what the send delay did before it was moved onto the core (the suites
// "P01 Keyboard" and "Pinned On Cores" hold that in place); a turned TIME is now glided to (the
// pitch of what is in the line bends) where it used to jump.
//
// TIME  echo time (can follow the tempo)
// FDBK  how much comes back
// MIX   dry / wet
// MODE  STER = each side repeats on its own side; PING = each repeat changes sides;
//       TAPE = STER on an unsteady, lightly driven tape (AGE counts from TAPE_AGE, DRIV from TAPE_DRIVE)
// FX2:
// READ  FWD, REV (each stretch of TIME comes back backwards), OCT+ / 5TH+ / OCT- (every repeat higher or lower)
// DRIV  how hard the loop hits the tape stage: 0 = clean
// SAT   the stage's curve: any of src/dsp/Saturation.h
// AGE   wow and flutter of the transport: 0 = steady
class Delay : public dly::Family
{
public:
    enum Param { Time = 0, Feedback, Mix, ModeCell };
    enum Extra { Read = 0, Drive, Sat, Age };
    enum class Mode { Stereo, PingPong, Tape };

    static constexpr float MIN_TIME = 0.01f, MAX_TIME = 2.0f;
    static constexpr float TAPE_AGE = 0.35f, TAPE_DRIVE = 0.125f;
    // The send delay's line was 192,000 samples round, and a place between two samples was worked
    // out in it: it still is wherever 2 s fit that (up to 96 kHz), so a time that is not a whole
    // number of samples is read at the places it always was.
    static constexpr int RING = 192000;

    static const char* const* modeNames()
    {
        static const char* const names[] = { "STER", "PING", "TAPE" };
        return names;
    }

    Delay()
    {
        seed();
        setWetOnly(false);     // pinned (P01): it mixes by itself in a chain too, sample for sample as it always did
        // (The object as it always started, which is not what its cells call their defaults.)
        setCell(Feedback, 0.4f);
        mix = 1.0f;
    }

    void setTime(float seconds) { setCell(Time, seconds); }
    void setFeedback(float fb) { setCell(Feedback, fb); }
    void setMode(Mode m) { setCell(ModeCell, (float) (int) m); }
    // The loop filter's corners (not cells: the send delay's own 8 kHz and 80 Hz unless set).
    void setHighCut(float hz) { highCut = std::clamp(hz, 500.0f, 20000.0f); touch(); }
    void setLowCut(float hz) { lowCut = std::clamp(hz, 20.0f, 2000.0f); touch(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "TIME", 0, MIN_TIME, MAX_TIME, 0.375f, K::Log, "s" },
            { "FDBK", 1, 0, 0.95f, 0.35f },
            { "MIX", 2, 0, 1, 0.5f },
            { "MODE", 3, 0, 2, 0, K::Choice, "", modeNames(), 3 },
            { "READ", 0, 0, (float) (dly::read::Count - 1), 0, K::Choice, "", dly::read::names(), dly::read::Count },
            { "DRIV", 1, 0, 1, 0 },
            { "SAT", 2, 0, (float) (sat::Count - 1), (float) sat::Fast, K::Choice, "", sat::names(), sat::Count },
            { "AGE", 3, 0, 1, 0 } };
        return c;
    }
    int mixCell() const override { return Mix; }

    dly::Core::Setup setup() const override
    {
        dly::Core::Setup s;
        s.send = true;
        s.hermite = false;
        s.filterInput = false;
        s.ring = (double) MAX_TIME * sampleRate <= (double) RING ? RING : 0;
        return s;
    }
    float lineSeconds() const override { return dly::roomFor(MAX_TIME, MAX_TIME); }

    void configure(const float* v) override
    {
        const int mode = (int) std::lround(v[ModeCell]);
        const bool tape = mode == (int) Mode::Tape;
        const float time = v[Time];
        core.setTapCount(1);
        core.setTap(0, time, 1.0f, 1.0f, 1.0f);
        core.setFeedback(v[Feedback]);
        core.setCross(mode == (int) Mode::PingPong ? 1.0f : 0.0f);
        core.setFilter(highCut, lowCut);
        core.setTransport(tape ? TAPE_AGE + (1.0f - TAPE_AGE) * v[4 + Age] : v[4 + Age]);
        dly::driven(core, tape ? TAPE_DRIVE + (1.0f - TAPE_DRIVE) * v[4 + Drive] : v[4 + Drive], v[4 + Sat]);
        setRead(v[4 + Read], time);
        set = true;
    }

    // A cell set before anything has sounded is where it is set (there is nothing in the line to
    // bend): the core is put there, not glided there.
    void resetCore() override { core.reset(); fresh = true; set = false; }
    void controlCore() override
    {
        if (fresh && set) core.reset();
        set = false;
        core.control();
    }
    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override
    {
        fresh = false;
        core.run(left, right, n, mixFrom, mixTo);
    }

private:
    float highCut = 8000.0f, lowCut = 80.0f;
    bool fresh = true, set = false;
};
