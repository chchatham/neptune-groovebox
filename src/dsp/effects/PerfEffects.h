#pragma once

#include "EffectBase.h"
#include "FxBlocks.h"
#include "../Saturation.h"
#include <algorithm>
#include <array>
#include <cmath>
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
// Linear below full scale, bent above it, never beyond +-2: a loop that passes through it loses
// nothing while it is in range (a held freeze stays) and cannot run away.
inline float ceiling(float x)
{
    const float a = std::abs(x);
    if (a <= 1.0f) return x;
    return std::copysign(1.0f + fxb::softSat(a - 1.0f), x);
}
}

// -------------------------------------------------------------------------------------------
// DUB: the echo of a dub mixing desk: a tape echo with its feedback ridden by hand. A tape-style
// line (the structure of TapeDelay.h: Hermite read at a gliding position, so a moved TIME bends
// the pitch of what is in the loop, with the transport's wow and flutter on top) whose repeats
// pass a band filter and the tape each time round.
//
// The tape (TapeStage) is the usual model of a recording stage, three things a plain saturator
// does not do:
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
// SAT   (PERF's MORE page) the tape's saturation curve: any of src/dsp/Saturation.h, FAST to start with
// -------------------------------------------------------------------------------------------
class DubDelay : public EffectBase
{
public:
    enum Param { Time = 0, Feedback, Tone, DriveAmt };
    static constexpr float MIN_TIME = 0.02f, MAX_TIME = 1.5f;

    DubDelay() { mix = 1.0f; curve.set(sat::Fast, TapeStage::BIAS); prepare(44100.0, 0); }

    ParamSpec getParamSpec(int index) const override
    {
        switch (index)
        {
            case 0: return { "TIME", 0, MIN_TIME, MAX_TIME, 0.375f, ParamKind::Log, "s" };
            case 1: return { "FDBK", 1, 0, 1, 0.55f };
            case 2: return { "TONE", 2, 0, 1, 0.5f };
            case 3: return { "DRIV", 3, 0, 1, 0.3f };
            default: return {};
        }
    }

    ParamSpec getExtraSpec(int index) const override
    {
        if (index != 0) return {};
        return { "SAT", 0, 0, (float) (sat::Count - 1), (float) sat::Fast, ParamKind::Choice, "", sat::names(), sat::Count };
    }
    void setExtra(int index, float value) override
    {
        if (index == 0 && std::isfinite(value)) curve.set((int) std::lround(value), TapeStage::BIAS);
    }
    float getExtra(int index) const override { return index == 0 ? (float) curve.curve : 0.0f; }

    void setParameter(int id, float value) override
    {
        if (! std::isfinite(value)) return;
        switch (id)
        {
            case Time:     timeSec = std::clamp(value, MIN_TIME, MAX_TIME); break;
            case Feedback: feedback = std::clamp(value, 0.0f, 1.0f); break;
            case Tone:     tone = std::clamp(value, 0.0f, 1.0f); break;
            case DriveAmt: driveAmt = std::clamp(value, 0.0f, 1.0f); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Time:     return timeSec;
            case Feedback: return feedback;
            case Tone:     return tone;
            case DriveAmt: return driveAmt;
            default:       return 0.0f;
        }
    }

    void prepare(double sr, int) override
    {
        sampleRate = sr;
        const int maxSamples = (int) ((double) MAX_TIME * 1.02 * sr) + 16;
        lineL.prepare(maxSamples);
        lineR.prepare(maxSamples);
        timeSmooth.setTime(sr, 80.0f);
        toneSmooth.setTime(sr, 20.0f);
        fbSmooth.setTime(sr, 10.0f);
        driveSmooth.setTime(sr, 10.0f);
        tapeL.prepare(sr);
        tapeR.prepare(sr);
        appliedTone = -1.0f;
        reset();
    }

    void reset() override
    {
        lineL.clear();
        lineR.clear();
        for (auto* f : { &lpL, &lpR, &hpL, &hpR })
            f->reset();
        tapeL.reset();
        tapeR.reset();
        transport.reset();
        timeSmooth.snap(timeSec);
        toneSmooth.snap(tone);
        fbSmooth.snap(feedback);
        driveSmooth.snap(driveAmt);
        toneCount = 0;
    }

    void process(float& left, float& right) override
    {
        // The band follows TONE a little at a time (its two corners are set every 16 samples).
        const float t = toneSmooth.next(tone);
        if (--toneCount <= 0)
        {
            toneCount = 16;
            if (t != appliedTone)
            {
                appliedTone = t;
                const float centre = 150.0f * std::pow(5000.0f / 150.0f, t);
                for (auto* f : { &lpL, &lpR }) f->setCutoff(sampleRate, centre * 2.2f);
                for (auto* f : { &hpL, &hpR }) f->setCutoff(sampleRate, centre / 2.2f);
            }
        }

        const float driven = driveSmooth.next(driveAmt);
        const float drive = 1.0f + 7.0f * driven;

        // The transport: slow wow and faster flutter move the head a little, more when driven.
        const float unsteady = (0.35f + 0.65f * driven) * transport.next(sampleRate);

        const float d = timeSmooth.next(timeSec) * (float) sampleRate * (1.0f + unsteady);
        const float wetL = hpL.highPass(lpL.lowPass(lineL.readHermite(d)));
        const float wetR = hpR.highPass(lpR.lowPass(lineR.readHermite(d)));

        const float loopGain = fbSmooth.next(feedback) * MAX_LOOP_GAIN * BAND_MAKEUP;
        // What goes onto the tape: unity gain for small signals, held near +-1 / drive (which is
        // what holds a loop gain above 1). What is heard is brought back up by sqrt(drive).
        const sat::Biased tape = curve;
        lineL.write(fxb::flushDenormal(tapeL.record(left + wetL * loopGain, drive, tape)));
        lineR.write(fxb::flushDenormal(tapeR.record(right + wetR * loopGain, drive, tape)));

        const float makeup = std::sqrt(drive);
        left = wetL * makeup;
        right = wetR * makeup;
    }

private:
    static constexpr float MAX_LOOP_GAIN = 1.15f;
    // The band's two corners sit a factor 2.2 either side of its centre, where they pass 0.829 together:
    // made up for, so that FDBK means the same wherever TONE is.
    static constexpr float BAND_MAKEUP = 1.0f / 0.829f;
    // The tape's record stage, one per channel: the treble above EMPHASIS_HZ is recorded
    // (1 + EMPHASIS) times hotter and played back as much quieter; BIAS is the offset that makes the
    // saturation lopsided. Silence in gives silence out exactly; the offset the lopsided curve leaves
    // on a loud signal is taken out by the loop's high-pass before it is heard or fed back.
    struct TapeStage
    {
        static constexpr float EMPHASIS_HZ = 3000.0f, EMPHASIS = 2.0f, BIAS = 0.25f;

        void prepare(double sr)
        {
            pre.setCutoff(sr, EMPHASIS_HZ);
            de.setCutoff(sr, EMPHASIS_HZ / (1.0f + EMPHASIS));      // the corner that undoes the pre-emphasis
        }

        void reset() { pre.reset(); de.reset(); }

        // `curve` is the chosen saturation worked at BIAS; dividing by its slope there keeps a quiet
        // sound at unity whatever the curve.
        float record(float x, float drive, const sat::Biased& curve)
        {
            const float hot = x + EMPHASIS * pre.highPass(x);
            const float taped = curve(hot * drive) / (drive * curve.slope);
            return taped - (EMPHASIS / (1.0f + EMPHASIS)) * de.highPass(taped);
        }

        fxb::OnePole pre, de;
    };

    float timeSec = 0.375f, feedback = 0.55f, tone = 0.5f, driveAmt = 0.3f;
    float appliedTone = -1.0f;
    int toneCount = 0;
    fxb::DelayLine lineL, lineR;
    fxb::OnePole lpL, lpR, hpL, hpR;
    TapeStage tapeL, tapeR;
    sat::Biased curve;             // SAT, worked at the tape's BIAS
    fxb::TapeTransport transport;
    fxb::Smoother timeSmooth, toneSmooth, fbSmooth, driveSmooth;
};

// -------------------------------------------------------------------------------------------
// FRZ: catch a moment and hold it. A feedback delay network (Stautner & Puckette / Jot: eight
// delay lines mixed through a Hadamard matrix, which loses nothing) fed through three allpass
// diffusers. While the pad is down the network takes what it is given for GRAB, then shuts its
// input and holds what it has (loop gain 1). Let go, it dies away in DEC.
// In the loop each line can be read by a pair of moving taps (the delay-line pitch shifter: two
// Hann-windowed heads half a grain apart) an octave up: SHIM is how much of that is fed back
// while the pad is down, SPRL how much once it is let go - the held sound climbs away, octave
// on octave, into a low-pass that takes it out at the top.
//
// GRAB  how long the input stays open after the press
// SHIM  octave-up feedback while held
// SPRL  octave-up feedback after the release: the spiral away
// DEC   time to die away after the release
// -------------------------------------------------------------------------------------------
class FreezeReverb : public EffectBase, public PerfGesture
{
public:
    enum Param { Grab = 0, Shimmer, Spiral, Decay };
    static constexpr int LINES = 8;

    FreezeReverb() { mix = 1.0f; prepare(44100.0, 0); }

    ParamSpec getParamSpec(int index) const override
    {
        switch (index)
        {
            case 0: return { "GRAB", 0, 0.02f, 2.0f, 0.25f, ParamKind::Log, "s" };
            case 1: return { "SHIM", 1, 0, 1, 0.0f };
            case 2: return { "SPRL", 2, 0, 1, 0.5f };
            case 3: return { "DEC", 3, 0.2f, 20.0f, 3.0f, ParamKind::Log, "s" };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        if (! std::isfinite(value)) return;
        switch (id)
        {
            case Grab:    grabSec = std::clamp(value, 0.02f, 2.0f); break;
            case Shimmer: shimmer = std::clamp(value, 0.0f, 1.0f); break;
            case Spiral:  spiral = std::clamp(value, 0.0f, 1.0f); break;
            case Decay:   decaySec = std::clamp(value, 0.2f, 20.0f); releaseGain = releaseGainFor(decaySec); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Grab:    return grabSec;
            case Shimmer: return shimmer;
            case Spiral:  return spiral;
            case Decay:   return decaySec;
            default:      return 0.0f;
        }
    }

    void padGate(bool down) override
    {
        held = down;
        if (down) grabLeft = (int) (grabSec * (float) sampleRate);
    }

    void prepare(double sr, int) override
    {
        sampleRate = sr;
        // Line lengths in milliseconds: no two share a factor once in samples (near enough), 53 - 113 ms.
        static const float ms[LINES] = { 53.3f, 61.7f, 68.9f, 77.3f, 85.1f, 93.7f, 103.1f, 112.9f };
        for (int i = 0; i < LINES; ++i)
        {
            // (Whole samples: a read between two samples would dull a held sound a little every time round.)
            length[(size_t) i] = std::floor((float) ((double) ms[i] * 0.001 * sr));
            line[(size_t) i].prepare((int) length[(size_t) i] + 8);
            damp[(size_t) i].setCutoff(sr, 5000.0f);
        }
        static const float apMs[3] = { 4.7f, 8.3f, 12.1f };
        for (int i = 0; i < 3; ++i)
        {
            apLen[(size_t) i] = (float) ((double) apMs[i] * 0.001 * sr);
            ap[(size_t) i].prepare((int) apLen[(size_t) i] + 8);
        }
        grain = (float) (0.030 * sr);
        inHp.setCutoff(sr, 25.0f);
        gateSmooth.setTime(sr, 15.0f);
        shareSmooth.setTime(sr, 30.0f);
        gainSmooth.setTime(sr, 30.0f);
        reset();
    }

    void reset() override
    {
        for (auto& l : line) l.clear();
        for (auto& a : ap) a.clear();
        for (auto& d : damp) d.reset();
        inHp.reset();
        gateSmooth.snap(0.0f);
        shareSmooth.snap(0.0f);
        gainSmooth.snap(1.0f);
        grainPhase = 0.0f;
        grabLeft = 0;
    }

    void process(float& left, float& right) override
    {
        // The input: open for GRAB after the press, then shut.
        const bool open = held && grabLeft > 0;
        if (grabLeft > 0) --grabLeft;
        float in = inHp.highPass((left + right) * 0.5f) * gateSmooth.next(open ? 1.0f : 0.0f);
        for (int i = 0; i < 3; ++i)
        {
            // Schroeder allpass: v = x + g d, y = d - g v.
            const float d = ap[(size_t) i].readLinear(apLen[(size_t) i]);
            const float v = in + 0.6f * d;
            ap[(size_t) i].write(fxb::flushDenormal(v));
            in = d - 0.6f * v;
        }

        // Held: nothing is lost. Let go: it dies away in DEC.
        const float loopGain = gainSmooth.next(held ? 1.0f : releaseGain);
        const float share = shareSmooth.next(held ? shimmer : spiral);

        // The two heads of the octave-up read: they move towards the write point at one sample a
        // sample (ratio 2), each faded by a Hann window, half a grain apart.
        grainPhase += 1.0f / grain;
        if (grainPhase >= 1.0f) grainPhase -= 1.0f;
        float phase2 = grainPhase + 0.5f;
        if (phase2 >= 1.0f) phase2 -= 1.0f;
        const bool climbing = share > 1.0e-4f;
        const float win1 = climbing ? 0.5f - 0.5f * std::cos(grainPhase * dsp::TWO_PI_F) : 0.0f;
        const float win2 = 1.0f - win1;

        std::array<float, LINES> out;
        for (int i = 0; i < LINES; ++i)
        {
            const float len = length[(size_t) i];
            float v = line[(size_t) i].readLinear(len);
            if (climbing)
            {
                const float up = line[(size_t) i].readLinear(len - grainPhase * grain) * win1
                               + line[(size_t) i].readLinear(len - phase2 * grain) * win2;
                v += share * (damp[(size_t) i].lowPass(up) - v);
            }
            out[(size_t) i] = v;
        }

        // Hadamard, 8 x 8, scaled to keep the energy (three butterfly passes).
        std::array<float, LINES> m = out;
        for (int span = 1; span < LINES; span <<= 1)
            for (int i = 0; i < LINES; i += span << 1)
                for (int j = i; j < i + span; ++j)
                {
                    const float a = m[(size_t) j], b = m[(size_t) (j + span)];
                    m[(size_t) j] = a + b;
                    m[(size_t) (j + span)] = a - b;
                }
        static constexpr float NORM = 0.35355339f;      // 1 / sqrt(8)
        for (int i = 0; i < LINES; ++i)
            line[(size_t) i].write(fxb::flushDenormal(perffx::ceiling(in * NORM + m[(size_t) i] * NORM * loopGain)));

        left  = (out[0] - out[2] + out[4] - out[6]) * 0.5f;
        right = (out[1] - out[3] + out[5] - out[7]) * 0.5f;
    }

private:
    // Let go: -60 dB in DEC (the mean line is 82 ms).
    static float releaseGainFor(float decay) { return std::exp(-6.908f * 0.082f / decay); }

    float grabSec = 0.25f, shimmer = 0.0f, spiral = 0.5f, decaySec = 3.0f;
    float releaseGain = releaseGainFor(3.0f);
    bool held = false;
    int grabLeft = 0;
    float grain = 1323.0f, grainPhase = 0.0f;
    std::array<fxb::DelayLine, LINES> line;
    std::array<float, LINES> length {};
    std::array<fxb::OnePole, LINES> damp;
    std::array<fxb::DelayLine, 3> ap;
    std::array<float, 3> apLen {};
    fxb::OnePole inHp;
    fxb::Smoother gateSmooth, shareSmooth, gainSmooth;
};

// -------------------------------------------------------------------------------------------
// RPT: beat repeat. From the press on, the sound is recorded; the first slice is heard as it
// is played, and from then on that slice comes round again and again. The slice's length is a
// division of the tempo, read at the start of every repeat, so a division changed under the
// finger (pressure) takes over on the next one.
//
// DIV   the slice: 1/4 .. 1/64
// DCAY  every repeat quieter than the last
// PTCH  every repeat this many semitones away from the last (played faster or slower)
// GATE  how much of each slice sounds
// -------------------------------------------------------------------------------------------
class BeatRepeat : public EffectBase, public PerfGesture
{
public:
    enum Param { Division = 0, DecayAmt, Pitch, Gate };
    static constexpr int NUM_DIVS = 7;
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

    BeatRepeat() { mix = 1.0f; prepare(44100.0, 0); }

    ParamSpec getParamSpec(int index) const override
    {
        switch (index)
        {
            case 0: return { "DIV", 0, 0, NUM_DIVS - 1, 3, ParamKind::Choice, "", divNames(), NUM_DIVS };
            case 1: return { "DCAY", 1, 0, 1, 0.0f };
            case 2: return { "PTCH", 2, -12, 12, 0, ParamKind::Int, "st" };
            case 3: return { "GATE", 3, 0.1f, 1, 1.0f };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        if (! std::isfinite(value)) return;
        switch (id)
        {
            case Division: division = std::clamp((int) std::lround(value), 0, NUM_DIVS - 1); break;
            case DecayAmt: decayAmt = std::clamp(value, 0.0f, 1.0f); break;
            case Pitch:    pitch = std::clamp(std::round(value), -12.0f, 12.0f); break;
            case Gate:     gate = std::clamp(value, 0.1f, 1.0f); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Division: return (float) division;
            case DecayAmt: return decayAmt;
            case Pitch:    return pitch;
            case Gate:     return gate;
            default:       return 0.0f;
        }
    }

    void padGate(bool down) override { if (down) restart(); }

    void prepare(double sr, int) override
    {
        sampleRate = sr;
        // A quarter note at 20 BPM is three seconds.
        capacity = (int) (3.05 * sr);
        bufL.assign((size_t) capacity, 0.0f);
        bufR.assign((size_t) capacity, 0.0f);
        reset();
    }

    void reset() override { restart(); }

    void process(float& left, float& right) override
    {
        // Recording goes on until the buffer is full (it holds the longest slice at any tempo).
        if (recorded < capacity)
        {
            bufL[(size_t) recorded] = left;
            bufR[(size_t) recorded] = right;
            ++recorded;
        }
        if (posInSlice >= sliceLen)
            nextSlice();

        const int fade = std::max(1, std::min((int) (0.002 * sampleRate), sliceLen / 4));
        const int sounding = std::max(2 * fade, (int) ((float) sliceLen * gate));
        float g = level;
        if (posInSlice < fade)                  g *= (float) posInSlice / (float) fade;
        else if (posInSlice >= sounding)        g = 0.0f;
        else if (posInSlice >= sounding - fade) g *= (float) (sounding - posInSlice) / (float) fade;

        if (repeat == 0)
        {
            // The first time through is the sound itself (its start is not faded: it is already playing).
            if (posInSlice < fade) g = level;
            left *= g;
            right *= g;
        }
        else
        {
            const double at = std::min((double) posInSlice * rate, (double) (recorded - 2));
            const int i = std::max(0, (int) at);
            const float f = (float) (at - (double) i);
            const int j = std::min(i + 1, capacity - 1);
            left  = (bufL[(size_t) i] + f * (bufL[(size_t) j] - bufL[(size_t) i])) * g;
            right = (bufR[(size_t) i] + f * (bufR[(size_t) j] - bufR[(size_t) i])) * g;
        }
        ++posInSlice;
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
    }

    void nextSlice()
    {
        ++repeat;
        posInSlice = 0;
        const float bpm = getTempo();
        const double samples = divBeats(division) * 60.0 / (double) (bpm > 1.0f && std::isfinite(bpm) ? bpm : 120.0f) * sampleRate;
        sliceLen = std::clamp((int) std::lround(samples), 16, capacity - 2);
        // A repeat can only play what has been recorded by now.
        if (repeat > 0)
            sliceLen = std::min(sliceLen, std::max(16, recorded - 1));
        level = repeat <= 0 ? 1.0f : std::pow(1.0f - 0.5f * decayAmt, (float) repeat);
        // Played faster, a slice would run past what was recorded: held to four octaves either way.
        rate = repeat <= 0 ? 1.0 : std::pow(2.0, std::clamp((double) pitch * (double) repeat, -48.0, 48.0) / 12.0);
        if (rate > 1.0)
            rate = std::min(rate, (double) std::max(16, recorded - 2) / (double) sliceLen);
    }

    int division = 3;
    float decayAmt = 0.0f, pitch = 0.0f, gate = 1.0f;
    std::vector<float> bufL, bufR;
    int capacity = 0, recorded = 0, repeat = -1, posInSlice = 0, sliceLen = 0;
    float level = 1.0f;
    double rate = 1.0;
};

// -------------------------------------------------------------------------------------------
// STOP: the tape stops. From the press the sound is recorded and played back from a head that
// slows to a standstill over TIME: the pitch falls away and the sound with it. Letting go
// brings the live sound back (the pad's own fade).
//
// TIME  how long the tape takes to stop
// CURV  the way it slows: 0 evenly, 1 quickly at first with a long last turn
// -------------------------------------------------------------------------------------------
class TapeStop : public EffectBase, public PerfGesture
{
public:
    enum Param { Time = 0, Curve };
    static constexpr float MIN_TIME = 0.05f, MAX_TIME = 4.0f;

    TapeStop() { mix = 1.0f; prepare(44100.0, 0); }

    ParamSpec getParamSpec(int index) const override
    {
        switch (index)
        {
            case 0: return { "TIME", 0, MIN_TIME, MAX_TIME, 0.5f, ParamKind::Log, "s" };
            case 1: return { "CURV", 1, 0, 1, 0.3f };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        if (! std::isfinite(value)) return;
        if (id == Time)  timeSec = std::clamp(value, MIN_TIME, MAX_TIME);
        if (id == Curve) curve = std::clamp(value, 0.0f, 1.0f);
    }

    float getParameter(int id) const override { return id == Time ? timeSec : (id == Curve ? curve : 0.0f); }

    void padGate(bool down) override { if (down) restart(); }

    void prepare(double sr, int) override
    {
        sampleRate = sr;
        // The tape slows at least as fast as a straight line over the longest TIME, so the head never
        // gets past half of that: what would be recorded beyond it is never played.
        capacity = (int) ((double) MAX_TIME * 0.5 * 1.05 * sr) + 16;
        bufL.assign((size_t) capacity, 0.0f);
        bufR.assign((size_t) capacity, 0.0f);
        reset();
    }

    void reset() override { restart(); }

    void process(float& left, float& right) override
    {
        if (written < capacity)
        {
            bufL[(size_t) written] = left;
            bufR[(size_t) written] = right;
            ++written;
        }
        // How far the tape has slowed: 1 at the press, 0 once TIME has passed.
        const float t = std::min(1.0f, (float) elapsed / std::max(1.0f, timeSec * (float) sampleRate));
        const float straight = 1.0f - t;
        const float speed = straight * (1.0f - curve) + straight * straight * straight * curve;
        ++elapsed;

        const double at = std::min(head, (double) (written - 2));
        const int i = std::max(0, (int) at);
        const float f = (float) (at - (double) i);
        const int j = std::min(i + 1, capacity - 1);
        // A stopped tape gives nothing (the last sample is not held as an offset).
        const float g = std::min(1.0f, speed * 12.0f);
        left  = (bufL[(size_t) i] + f * (bufL[(size_t) j] - bufL[(size_t) i])) * g;
        right = (bufR[(size_t) i] + f * (bufR[(size_t) j] - bufR[(size_t) i])) * g;
        head += (double) speed;
    }

private:
    void restart() { written = 0; elapsed = 0; head = 0.0; }

    float timeSec = 0.5f, curve = 0.3f;
    std::vector<float> bufL, bufR;
    int capacity = 0, written = 0, elapsed = 0;
    double head = 0.0;
};
