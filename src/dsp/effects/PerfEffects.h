#pragma once

#include "EffectBase.h"
#include "FxBlocks.h"
#include "DelayCore.h"
#include "ReverbParts.h"
#include "../Saturation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

// The effects that exist only on the PERF screen's pads (src/engine/PerfFx.h): they are played,
// not left switched on. Each gives the WET signal alone (its mix is 1); the pad adds it to, or
// puts it in place of, what it is fed.
//
// A pad's effect may want to know what the finger does: PerfGesture. (The tempo is EffectBase's.)
struct PerfGesture
{
    virtual ~PerfGesture() = default;
    virtual void padGate(bool down) = 0;
};

namespace perffx
{
// Linear below full scale, bent above it, never beyond +-2 (dly::ceiling, DelayCore.h).
inline float ceiling(float x) { return dly::ceiling(x); }
}

// -------------------------------------------------------------------------------------------
// DUB: the echo of a dub mixing desk: a tape echo with its feedback ridden by hand. A setting of
// dly::Core (DelayCore.h): a tape-style line (Hermite read at a gliding position, so a moved TIME
// bends the pitch of what is in the loop, with the transport's wow and flutter on top) whose
// repeats pass a band filter and the tape each time round.
//
// The tape (dly::TapeStage) is the usual model of a recording stage, three things a plain
// saturator does not do:
//  - record pre-emphasis, saturation, playback de-emphasis: flat while the level is low, but the
//    top of the spectrum reaches the saturation first, so a hot repeat comes back duller and
//    squashed and the next one more so (tape's level-dependent treble loss);
//  - a bias offset inside the saturator: the two halves of the wave bend differently, which adds
//    the even harmonics (the octave) that make a driven repeat thicken instead of only fuzz;
//  - the saturation bounds the loop, so FDBK at the top howls at a held level and never runs away.
//
// TIME  echo time
// FDBK  0..1 -> loop gain 0..1.15: the top of the range feeds on itself, held by the tape
// TONE  where the loop's band sits, 150 Hz .. 5 kHz: every repeat is narrowed towards it
// DRIV  how hard the tape is hit: more squash, more harmonics, more treble lost per repeat, and
//       a less steady transport
// PERF's MORE page:
// SAT   the tape's saturation curve: any of src/dsp/Saturation.h, FAST to start with
// HEAD  1..4 playback heads, evenly spaced up to TIME (the last one feeds back)
// WIDE  the right side's heads sit up to 20 ms later than the left's
// READ  FWD, REV (each stretch of TIME comes back backwards), OCT+ / 5TH+ / OCT- (every repeat higher or lower)
// -------------------------------------------------------------------------------------------
class DubDelay : public dly::Family
{
public:
    enum Param { Time = 0, Feedback, Tone, DriveAmt };
    enum Extra { Sat = 0, Head, Wide, Read };
    static constexpr float MIN_TIME = 0.02f, MAX_TIME = 1.5f;
    static constexpr int MAX_HEADS = 4;

    DubDelay() { seed(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "TIME", 0, MIN_TIME, MAX_TIME, 0.375f, K::Log, "s" },
            { "FDBK", 1, 0, 1, 0.55f },
            { "TONE", 2, 0, 1, 0.5f },
            { "DRIV", 3, 0, 1, 0.3f },
            { "SAT", 0, 0, (float) (sat::Count - 1), (float) sat::Fast, K::Choice, "", sat::names(), sat::Count },
            { "HEAD", 1, 1, (float) MAX_HEADS, 1, K::Int },
            { "WIDE", 2, 0, 1, 0 },
            { "READ", 3, 0, (float) (dly::read::Count - 1), 0, K::Choice, "", dly::read::names(), dly::read::Count } };
        return c;
    }
    dly::Core::Setup setup() const override
    {
        dly::Core::Setup s;
        s.emphasis = s.makeup = true;       // what is heard is brought back up by sqrt(drive): the tape holds near 1 / drive
        return s;
    }
    float lineSeconds() const override { return dly::roomFor(MAX_TIME, MAX_TIME); }

    void configure(const float* v) override
    {
        const float time = v[Time], driven = v[DriveAmt];
        dly::heads(core, time, (int) std::lround(v[4 + Head]), MAX_HEADS);
        core.setFeedback(v[Feedback] * MAX_LOOP_GAIN * BAND_MAKEUP);
        core.setWide(v[4 + Wide] * dly::MAX_WIDE);
        // The transport: slow wow and faster flutter move the head a little, more when driven.
        core.setTransport(0.35f + 0.65f * driven);
        const float centre = 150.0f * std::pow(5000.0f / 150.0f, v[Tone]);
        core.setFilter(centre * 2.2f, centre / 2.2f);
        core.setDrive(1.0f, 1.0f + 7.0f * driven);
        core.setCurve((int) std::lround(v[4 + Sat]), dly::TapeStage::BIAS);
        setRead(v[4 + Read], time);
    }

private:
    static constexpr float MAX_LOOP_GAIN = 1.15f;
    // The band's two corners sit a factor 2.2 either side of its centre, where they pass 0.829 together:
    // made up for, so that FDBK means the same wherever TONE is.
    static constexpr float BAND_MAKEUP = 1.0f / 0.829f;
};

// -------------------------------------------------------------------------------------------
// FRZ: catch a moment and hold it. rvb::setting::field() of the one reverb network (ReverbParts.h):
// eight delay lines mixed through a Hadamard matrix, which loses nothing, fed through three allpass
// diffusers.
// While the pad is down the field takes what it is given for GRAB, then shuts its input and
// holds what it has (loop gain 1). Let go, it dies away in DEC.
// In the loop each line can be read by a pair of moving heads (dly::Heads, the delay-line pitch
// shifter) at an interval: SHIM is how much of that is fed back while the pad is down, SPRL how
// much once it is let go - the held sound climbs away, octave on octave, into a low-pass that
// takes it out at the top.
//
// GRAB  how long the input stays open after the press (it can follow the tempo: SYNC)
// SHIM  shifted feedback while held
// SPRL  shifted feedback after the release: the spiral away
// DEC   time to die away after the release
// PERF's MORE page:
// MODE  the interval of SHIM and SPRL (the reverbs' list): OCT+ as it always was, 5TH+, O+5, OCT-
//       (it sinks away), REV (what is held is turned round, a stretch at a time), SUB
// DAMP  the low-pass what is shifted passes each time round: 5 kHz as it always was, down to 500 Hz
// -------------------------------------------------------------------------------------------
class FreezeReverb : public CellEffect, public PerfGesture
{
public:
    enum Param { Grab = 0, Shimmer, Spiral, Decay };
    enum Extra { Mode = 0, Damp };
    static constexpr int LINES = 8;

    FreezeReverb() { mix = 1.0f; seed(); }

    void padGate(bool down) override
    {
        held = down;
        if (down) grabLeft = (int) (grabSec * (float) sampleRate);
    }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            ParamSpec { "GRAB", 0, 0.02f, 2.0f, 0.25f, K::Log, "s" }.timed(),
            { "SHIM", 1, 0, 1, 0.0f },
            { "SPRL", 2, 0, 1, 0.5f },
            { "DEC", 3, 0.2f, 20.0f, 3.0f, K::Log, "s" },
            { "MODE", Mode, 0, (float) (rvb::shift::Hold - 1), 0, K::Choice, "", rvb::shift::names(), rvb::shift::Hold },
            { "DAMP", Damp, 0, 1, 0 },
            {}, {} };
        return c;
    }

    void prepareCore() override { field.prepare(sampleRate, rvb::setting::field()); }
    void resetCore() override
    {
        aimField();
        field.reset();
        grabLeft = 0;
    }

    void configure(const float* v) override
    {
        grabSec = v[Grab];
        shimmer = v[Shimmer];
        spiral = v[Spiral];
        releaseDecay = v[Decay];
        speed = rvb::shift::speedOf((int) v[4 + Mode]);
        field.setTone(5000.0f * std::pow(0.1f, v[4 + Damp]));
        aimField();
    }

    void controlCore() override
    {
        aimField();
        field.control();
    }

    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override
    {
        if (grabLeft > 0) grabLeft -= n;
        field.run(left, right, n, mixFrom, mixTo);
    }

private:
    // The input: open for GRAB after the press, then shut. Held: nothing is lost. Let go: it dies away in DEC.
    void aimField()
    {
        field.setFeed(held && grabLeft > 0 ? 1.0f : 0.0f);
        field.setDecay(held ? std::numeric_limits<float>::infinity() : releaseDecay);
        field.setShimmer(held ? shimmer : spiral, speed);
    }

    rvb::Net field;
    float grabSec = 0.25f, shimmer = 0.0f, spiral = 0.5f, releaseDecay = 3.0f, speed = 2.0f;
    bool held = false;
    int grabLeft = 0;
};

// -------------------------------------------------------------------------------------------
// RPT: beat repeat. A setting of dly::Core: a line that records from the press on, and one head
// on it that the effect puts back to the start of the recording at every slice and runs at a
// speed (the core's tape speed). The first slice is heard as it is played, and from then on that
// slice comes round again and again. The slice's length is a division of the tempo, read at the
// start of every repeat, so a division changed under the finger (pressure) takes over on the
// next one. (Any delay repeats what it holds with READ at HOLD: that one is the loop shut, this
// one the recording played again.)
//
// DIV   the slice: 1/4 .. 1/64
// DCAY  every repeat quieter than the last
// PTCH  every repeat this many semitones away from the last (played faster or slower)
// GATE  how much of each slice sounds
// -------------------------------------------------------------------------------------------
class BeatRepeat : public dly::Family, public PerfGesture
{
public:
    enum Param { Division = 0, DecayAmt, Pitch, Gate };
    static constexpr int NUM_DIVS = 7;
    static constexpr float RECORD_SECONDS = 3.05f;      // a quarter note at 20 BPM is three seconds
    static const char* const* divNames()
    {
        static const char* const n[NUM_DIVS] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/64" };
        return n;
    }
    static double divBeats(int index)
    {
        static const double b[NUM_DIVS] = { 1.0, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125, 0.0625 };
        return b[std::clamp(index, 0, NUM_DIVS - 1)];
    }

    BeatRepeat() { mix = 1.0f; seed(); }

    void padGate(bool down) override { if (down) restart(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "DIV", 0, 0, NUM_DIVS - 1, 3, K::Choice, "", divNames(), NUM_DIVS },
            { "DCAY", 1, 0, 1, 0.0f },
            { "PTCH", 2, -12, 12, 0, K::Int, "st" },
            { "GATE", 3, 0.1f, 1, 1.0f },
            {}, {}, {}, {} };
        return c;
    }
    dly::Core::Setup setup() const override
    {
        dly::Core::Setup s;
        s.hermite = false;          // (a straight line between two samples, as the repeat always read)
        s.filterInput = false;      // (nothing stands before the line: what is recorded is the sound)
        return s;
    }
    float lineSeconds() const override { return RECORD_SECONDS + 0.05f; }

    void prepareCore() override
    {
        dly::Family::prepareCore();
        capacity = (int) (RECORD_SECONDS * sampleRate);
    }
    void resetCore() override
    {
        dly::Family::resetCore();
        restart();
    }

    void configure(const float* v) override
    {
        division = (int) v[Division];
        decayAmt = v[DecayAmt];
        pitch = v[Pitch];
        gate = v[Gate];
        core.setTapCount(1);
        core.setTap(0, 0.0f, 1.0f, 1.0f, 0.0f);     // (the head itself: as near the newest sample as a head can be)
        core.silenceTapsFrom(1);
        core.setFeedback(0.0f);
        core.setTransport(0.0f);
        core.setDrive(0.0f, 1.0f);
    }

    bool coasting() const override { return false; }    // (the slices are counted in runCore)

    // What is heard is the head alone (the pad mixes it: PerfFx.h).
    void runCore(float* left, float* right, int n, float, float) override
    {
        while (n > 0)
        {
            if (posInSlice >= sliceLen)
                nextSlice();
            int m = std::min(n, sliceLen - posInSlice);
            if (recorded < capacity) m = std::min(m, capacity - recorded);      // (the line stands from its last sample on, exactly)

            // The first time through is the sound itself, to the sample (the line takes it down as it
            // passes; its start is not faded: it is already playing). After that, the head.
            const float wet = repeat == 0 ? 0.0f : 1.0f;
            core.run(left, right, m, wet, wet);

            const int fade = std::max(1, std::min((int) (0.002 * sampleRate), sliceLen / 4));
            const int sounding = std::max(2 * fade, (int) ((float) sliceLen * gate));
            for (int i = 0; i < m; ++i)
            {
                const int pos = posInSlice + i;
                float g = level;
                if (pos < fade)                  g = repeat == 0 ? level : g * (float) pos / (float) fade;
                else if (pos >= sounding)        g = 0.0f;
                else if (pos >= sounding - fade) g *= (float) (sounding - pos) / (float) fade;
                left[i] *= g;
                right[i] *= g;
            }

            // Recording goes on until the line is full (it holds the longest slice at any tempo); then it stands.
            if (recorded < capacity)
            {
                recorded = std::min(capacity, recorded + m);
                if (recorded >= capacity) core.setRecording(false);
            }
            posInSlice += m;
            left += m; right += m;
            n -= m;
        }
    }

private:
    void restart()
    {
        recorded = 0;
        repeat = -1;
        posInSlice = 0;
        sliceLen = 0;
        level = 1.0f;
        rate = 1.0;
        core.setRecording(true);
        core.setLag(0.0f);
        core.setTape(1.0f);
    }

    void nextSlice()
    {
        ++repeat;
        posInSlice = 0;
        const float bpm = getTempo();
        const double samples = divBeats(division) * 60.0 / (double) (bpm > 1.0f && std::isfinite(bpm) ? bpm : 120.0f) * sampleRate;
        sliceLen = std::clamp((int) std::lround(samples), 16, capacity - 2);
        // A repeat can only play what has been recorded by now (this sample with the rest).
        const int there = std::min(capacity, recorded + 1);
        if (repeat > 0)
            sliceLen = std::min(sliceLen, std::max(16, there - 1));
        level = repeat <= 0 ? 1.0f : std::pow(1.0f - 0.5f * decayAmt, (float) repeat);
        // Played faster, a slice would run past what was recorded: held to four octaves either way.
        rate = repeat <= 0 ? 1.0 : std::pow(2.0, std::clamp((double) pitch * (double) repeat, -48.0, 48.0) / 12.0);
        if (rate > 1.0)
            rate = std::min(rate, (double) std::max(16, there - 2) / (double) sliceLen);
        // The head: back at the start of the recording (in the dark: the slice begins at nothing),
        // at the repeat's speed. (Its tap is HEAD samples behind the newest already.)
        if (repeat > 0)
        {
            core.setLag((float) recorded - HEAD);
            core.setTape((float) rate);
        }
    }

    static constexpr float HEAD = 2.0f;     // the core's shortest tap, in samples

    int division = 3;
    float decayAmt = 0.0f, pitch = 0.0f, gate = 1.0f;
    int capacity = 0, recorded = 0, repeat = -1, posInSlice = 0, sliceLen = 0;
    float level = 1.0f;
    double rate = 1.0;
};

// -------------------------------------------------------------------------------------------
// STOP: the tape stops. A setting of dly::Core: one head as near the newest sample as a head can
// be, and the core's tape speed taken down to a standstill over TIME: the head falls behind,
// the pitch falls away and the sound with it. Letting go brings the live sound back (the pad's
// own fade). (Any delay's echoes stop this way with READ at STOP.)
//
// TIME  how long the tape takes to stop
// CURV  the way it slows: 0 evenly, 1 quickly at first with a long last turn
// -------------------------------------------------------------------------------------------
class TapeStop : public dly::Family, public PerfGesture
{
public:
    enum Param { Time = 0, Curve };
    static constexpr float MIN_TIME = 0.05f, MAX_TIME = 4.0f;

    TapeStop() { mix = 1.0f; seed(); }

    void padGate(bool down) override { if (down) restart(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "TIME", 0, MIN_TIME, MAX_TIME, 0.5f, K::Log, "s" },
            { "CURV", 1, 0, 1, 0.3f },
            {}, {}, {}, {}, {}, {} };
        return c;
    }
    dly::Core::Setup setup() const override
    {
        dly::Core::Setup s;
        s.hermite = false;
        s.filterInput = false;
        return s;
    }
    // (The head falls behind by at most the whole of TIME, and stands from there.)
    float lineSeconds() const override { return MAX_TIME * 1.02f; }

    void resetCore() override
    {
        dly::Family::resetCore();
        restart();
    }

    void configure(const float* v) override
    {
        timeSec = v[Time];
        curve = v[Curve];
        core.setTapCount(1);
        core.setTap(0, 0.0f, 1.0f, 1.0f, 0.0f);
        core.silenceTapsFrom(1);
        core.setFeedback(0.0f);
        core.setTransport(0.0f);
        core.setDrive(0.0f, 1.0f);
        core.setTapeFades(true);
    }

    bool coasting() const override { return false; }    // (the speed is set at every look)

    // Every look: the speed the tape is to have a look further on (straight lines between).
    void controlCore() override
    {
        core.setTape(speedAt(elapsed + CONTROL), (float) CONTROL / (float) sampleRate);
        core.control();
    }

    void runCore(float* left, float* right, int n, float, float) override
    {
        core.run(left, right, n, 1.0f, 1.0f);
        elapsed += n;
    }

private:
    // How far the tape has slowed: 1 at the press, 0 once TIME has passed.
    float speedAt(int samples) const
    {
        const float t = std::min(1.0f, (float) samples / std::max(1.0f, timeSec * (float) sampleRate));
        const float straight = 1.0f - t;
        return straight * (1.0f - curve) + straight * straight * straight * curve;
    }

    void restart()
    {
        elapsed = 0;
        core.setLag(0.0f);
        core.setTape(1.0f);
    }

    float timeSec = 0.5f, curve = 0.3f;
    int elapsed = 0;
};
