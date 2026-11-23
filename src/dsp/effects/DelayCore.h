#pragma once

#include "CellEffect.h"
#include "FxBlocks.h"
#include "../Saturation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

// One delay line for every echo: the tape delay, the L/C/R delay, the multi-tap, the reverse delay
// and PERF's dub echo are settings of dly::Core, and so each has what the others have.
//
//   in -+-> [ + ] -> tape stage (a curve of Saturation.h, drive, bias, emphasis) -> loop filter -> line
//       |     ^                                                                                  |
//       |     +---- x FDBK <---- sum of the taps' feedback shares <-----------------------------+
//       |                                                                                        |
//       +--------------------------- dry / wet <---- sum of the taps (level, side) <-------------+
//
// - The line (dly::Line) is read between samples, linearly or with a Hermite curve.
// - Up to eight taps, each with its own time, level on either side and share of the feedback.
// - Every time is glided to, never jumped to: a turned TIME bends the pitch of what is in the line,
//   as a tape changing speed does. The transport adds wow and flutter; at depth 0 it costs nothing.
// - The read heads can move (dly::Heads: two heads that take turns, crossfaded): played backwards,
//   that is the reverse delay; played faster or slower, every repeat comes back higher or lower.
// - The loop passes a saturation curve when it is driven, and is held for any curve and any
//   feedback: above a loop gain of 1 it howls at a level, it does not run away.
//
// Nothing here allocates after prepare(). All of it is the textbook structure (Zoelzer, "DAFX",
// delays chapter; the two-head pitch shifter is the delay-line shifter of the same chapter).
#if defined(__GNUC__) || defined(__clang__)
 #define DLY_INLINE inline __attribute__((always_inline))
 #define DLY_INLINE_LAMBDA __attribute__((always_inline))
#else
 #define DLY_INLINE inline
 #define DLY_INLINE_LAMBDA
#endif

namespace dly
{
static constexpr int CONTROL = CellEffect::CONTROL;      // samples between two looks at the cells

// Linear below full scale, bent above it, never beyond +-2: a loop that passes through it loses
// nothing while it is in range and cannot run away.
inline float ceiling(float x)
{
    const float a = std::abs(x);
    if (a <= 1.0f) return x;
    return std::copysign(1.0f + fxb::softSat(a - 1.0f), x);
}

// ==== E7 >>> ====
// The loops that are held to the bit (the send delay here, the send reverb and the hall in
// ReverbParts.h) take NOTHING away from a state that has died down, not even what is below the
// smallest number of full precision: where the old classes left such a residue, it decides which
// way the next exact half is rounded when sound comes back (x + d g with g = 0.75 is an exact
// half one time in four), so a residue put to zero is a different sample a moment later.
// Measured: with fxb::flushDenormal, or a flush of the subnormal numbers alone, in these loops
// the send reverb differed from the old one at -140 dB after every silence. They keep the
// residue the old classes kept.
// ==== <<< E7 ====

// A slow sine (and its cosine) in the coupled ("magic circle") form: two multiplications a sample,
// and its size does not drift. The transport's wow and flutter, and the reverb loops' sway.
struct SlowSine
{
    void prepare(double hz, double sampleRate) { step = (float) (2.0 * std::sin(0.5 * dsp::TWO_PI_D * hz / sampleRate)); }
    void reset() { sin = 0.0f; cos = 1.0f; }
    DLY_INLINE void next()
    {
        sin += step * cos;
        cos -= step * sin;
    }
    float step = 0.0f, sin = 0.0f, cos = 1.0f;
};

// The share of the way a glide of `ms` goes in CONTROL samples (see Glide).
inline float glideShare(float ms, double sampleRate)
{
    return 1.0f - std::exp(-(float) CONTROL / std::max(1.0f, ms * 0.001f * (float) sampleRate));
}

// A tape transport's unsteadiness: slow wow and faster flutter, as a share of the delay time (the
// rates and depths the tape delay always had): two SlowSines, four multiplications a sample.
struct Transport
{
    static constexpr double WOW_HZ = 0.6, FLUTTER_HZ = 7.0;
    static constexpr float WOW_DEPTH = 0.003f, FLUTTER_DEPTH = 0.0006f;

    void prepare(double sampleRate) { wow.prepare(WOW_HZ, sampleRate); flutter.prepare(FLUTTER_HZ, sampleRate); }
    void reset() { wow.reset(); flutter.reset(); }

    float next()
    {
        const float mod = WOW_DEPTH * wow.sin + FLUTTER_DEPTH * flutter.sin;
        wow.next();
        flutter.next();
        return mod;
    }

    SlowSine wow, flutter;
};

// The line itself: a circular buffer of a power of two, allocated in prepare() only. It is read
// and written through a Cursor, a copy of its pointer and position that a loop keeps to itself
// (so the compiler can hold them in registers: a write to the buffer is not a write to them).
// The reads are those of fxb::DelayLine: call read...() before write() for the current sample,
// a delay of d then returns the sample written d calls ago. Linear read needs d >= 1, Hermite read
// d >= 2 (4-point, 3rd-order Hermite: Niemitalo, "Polynomial Interpolators for High-Quality
// Resampling of Oversampled Audio"); any d is held to the line.
class Line
{
public:
    struct Cursor
    {
        float* buf = nullptr;
        int mask = 0, w = 0;
        float maxDelay = 1.0f;

        DLY_INLINE void write(float x)
        {
            buf[w] = x;
            w = (w + 1) & mask;
        }

        DLY_INLINE float readLinear(float d) const
        {
            d = d >= 1.0f ? std::min(d, maxDelay) : 1.0f;      // (what is not a number is 1)
            const int di = (int) d;
            const float fr = d - (float) di;
            const float x0 = readAt(di), x1 = readAt(di + 1);
            return x0 + fr * (x1 - x0);
        }

        DLY_INLINE float readHermite(float d) const
        {
            d = d >= 2.0f ? std::min(d, maxDelay) : 2.0f;
            const int di = (int) d;
            const float fr = d - (float) di;
            const float xm1 = readAt(di - 1), x0 = readAt(di), x1 = readAt(di + 1), x2 = readAt(di + 2);
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * fr + c2) * fr + c1) * fr + x0;
        }

        // ==== E7 >>> the reads of the send delay, the send reverb and the hall ====
        // A whole number of samples back (before write(): the sample written `delay` calls ago;
        // after it, 1 is the one just written). Any number is held to the line by its mask.
        DLY_INLINE float readAt(int delay) const { return buf[(w - delay) & mask]; }

        // Between two samples in the weights form, older (1 - f) + newer f, the place worked out
        // from a write point as a float: the arithmetic the send delay and the hall reverb always
        // had (it is exact at a whole and at a half sample; in a ring of 32,768 the place is a
        // 512th of a sample coarse at its far end). `pos` counts the samples written, round a
        // ring of `ring`: the line's own (the hall), or the one the place was always worked out
        // in (the send delay's 192,000, whatever the line's length: which float a place between
        // two samples is rounded to depends on it). Needs d >= 1; any d is held to line and ring.
        DLY_INLINE float readBlend(float d, int pos, int ring) const
        {
            d = d >= 1.0f ? std::min(d, std::min(maxDelay, (float) (ring - 1))) : 1.0f;     // (what is not a number is 1)
            float place = (float) pos - d;
            if (place < 0.0f) place += (float) ring;
            int whole = (int) place;
            const float f = place - (float) whole;
            if (whole >= ring) whole -= ring;
            int back = pos - whole;                     // how many samples ago `whole` was written
            if (back <= 0) back += ring;
            return buf[(w - back) & mask] * (1.0f - f) + buf[(w - back + 1) & mask] * f;
        }
        // ==== <<< E7 ====
    };

    void prepare(int maxDelaySamples)
    {
        int n = 16;
        while (n < maxDelaySamples + 8)
            n <<= 1;
        buf.assign((size_t) n, 0.0f);
        mask = n - 1;
        maxDelay = (float) (n - 6);
        w = 0;
        empty = true;
    }

    // (A line nothing was written to since it was last emptied is left alone: the longest are
    // megabytes, and an effect is reset more than once before its first sound.)
    void clear()
    {
        if (! empty)
            std::fill(buf.begin(), buf.end(), 0.0f);
        w = 0;
        empty = true;
    }

    float getMaxDelay() const { return maxDelay; }
    Cursor open() { empty = false; return { buf.data(), mask, w, maxDelay }; }
    void close(const Cursor& c) { w = c.w; }

private:
    std::vector<float> buf;
    int mask = 0, w = 0;
    float maxDelay = 1.0f;
    bool empty = true;      // nothing written since prepare() / clear() (open() is the only way to write)
};

// A value on its way to where it is set: every CONTROL samples it is aimed (a share of what is
// left), and between two aims it moves in a straight line, one addition a sample. A turned knob,
// a pressed pad or a modulation that arrives once a block is reached without a step.
struct Glide
{
    float now = 0.0f, target = 0.0f, step = 0.0f;

    void set(float v) { target = v; }
    void snap() { now = target; step = 0.0f; }
    void snap(float v) { now = target = v; step = 0.0f; }
    bool moving() const { return step != 0.0f; }

    // It arrives: close to the end, a share of what is left is less than a float of that size can be
    // moved by (a time of 16,000 samples moves in steps of 0.002), and a glide that stalls there is
    // paid for ever after. So no step is smaller than the float's own (`least`), and when what is
    // left is one control period of such steps it is there.
    void aim(float share)
    {
        const float d = target - now, far = std::abs(d);
        const float least = 1.25e-7f * (1.0f + std::max(std::abs(target), std::abs(now)));
        if (far <= least * (float) CONTROL) { now = target; step = 0.0f; }
        else step = std::copysign(std::max(far * share * (1.0f / (float) CONTROL), least), d);
    }
};

// Two read heads that move along a line and take turns. What they read comes out at `speed` times
// its pitch: 2 = an octave up, 0.5 = an octave down, -1 = backwards. A head starts every `unit`
// samples and lives for 1 + crossfade units; while one fades out the next fades in, and the two
// weights always add up to 1. With a crossfade of 1 the heads overlap all the time (the delay-line
// pitch shifter); with a short one each plays its stretch alone (a reversed chunk).
// offset[h] is how far beyond the base delay head h reads; it runs between 0 and span.
struct Heads
{
    void set(float playSpeed, float crossfade)
    {
        speed = playSpeed;
        travel = std::abs(1.0f - playSpeed);
        xfade = std::clamp(crossfade, 0.02f, 1.0f);
        life = 1.0f + xfade;
    }

    void reset() { q = 0.0f; next(1000.0f); q = 0.0f; }
    bool moving() const { return speed != 1.0f; }

    DLY_INLINE void next(float unit)
    {
        if (unit != seenUnit)
        {
            seenUnit = unit;
            held = std::max(8.0f, unit);
            rate = 1.0f / held;
        }
        q += rate;
        if (q >= 2.0f) q -= 2.0f;
        const float ageA = q, ageB = q >= 1.0f ? q - 1.0f : q + 1.0f;
        weight[0] = window(ageA);
        weight[1] = 1.0f - weight[0];
        const float reach = travel * held;
        span = reach * life;
        // Slower than the line (or backwards): the head falls behind, its delay grows. Faster: it
        // starts far back and catches up.
        offset[0] = speed < 1.0f ? reach * ageA : reach * (life - ageA);
        offset[1] = speed < 1.0f ? reach * ageB : reach * (life - ageB);
    }

    template <typename Line>
    DLY_INLINE float read(const Line& line, float base) const
    {
        float y = 0.0f;
        if (weight[0] > 0.0f) y += weight[0] * line.readLinear(base + offset[0]);
        if (weight[1] > 0.0f) y += weight[1] * line.readLinear(base + offset[1]);
        return y;
    }

    float speed = 1.0f, travel = 0.0f, xfade = 1.0f, life = 2.0f;
    float q = 0.0f, span = 0.0f, seenUnit = -1.0f, held = 1000.0f, rate = 0.001f;
    float weight[2] = { 1.0f, 0.0f }, offset[2] = { 0.0f, 0.0f };

private:
    DLY_INLINE float window(float age) const
    {
        if (age < xfade) return smooth(age / xfade);
        if (age < 1.0f)  return 1.0f;
        if (age < life)  return 1.0f - smooth((age - 1.0f) / xfade);
        return 0.0f;
    }
    static float smooth(float t) { return t * t * (3.0f - 2.0f * t); }
};

// The record stage of a tape, one per channel (it was the dub echo's): the treble above
// EMPHASIS_HZ is recorded (1 + EMPHASIS) times hotter and played back as much quieter, so a hot
// repeat comes back duller; the curve is worked off-centre (sat::Biased), which adds the even
// harmonics. Silence in gives silence out exactly; the offset the lopsided curve leaves on a loud
// signal is taken out by the loop's high-pass.
struct TapeStage
{
    static constexpr float EMPHASIS_HZ = 3000.0f, EMPHASIS = 2.0f, BIAS = 0.25f;

    void prepare(double sr)
    {
        pre.setCutoff(sr, EMPHASIS_HZ);
        de.setCutoff(sr, EMPHASIS_HZ / (1.0f + EMPHASIS));      // the corner that undoes the pre-emphasis
    }

    void reset() { pre.reset(); de.reset(); }

    // Unity gain for a quiet sound whatever the curve (`back` is 1 / (drive x the curve's slope
    // where it is worked)), held near +-1 / drive: that is what holds a loop gain above 1.
    // `plain` = the curve is FAST worked at its centre (the tape delay's own): no list to look through.
    static DLY_INLINE float shaped(float x, float drive, float back, const sat::Biased& curve, bool plain)
    {
        return (plain ? fxb::softSat(x * drive) : curve(x * drive)) * back;
    }

    DLY_INLINE float record(float x, float drive, float back, const sat::Biased& curve, bool plain)
    {
        const float hot = x + EMPHASIS * pre.highPass(x);
        const float taped = shaped(hot, drive, back, curve, plain);
        return taped - (EMPHASIS / (1.0f + EMPHASIS)) * de.highPass(taped);
    }

    fxb::OnePole pre, de;
};

class Core
{
public:
    static constexpr int MAX_TAPS = 8;

    struct Setup
    {
        bool stereo = true;         // a line for each side; false = the two sides summed onto one line
        bool hermite = true;        // the read between samples: Hermite curve, or a straight line (cheaper, duller at the very top)
        bool filterInput = true;    // the loop filter stands before the line (the first echo passes it too), or in the feedback alone
        bool emphasis = false;      // the tape stage's pre- and de-emphasis
        bool makeup = false;        // what is heard is brought back up by sqrt(drive)
        float glideMs = 80.0f;      // how a changed time is reached
        float fbMs = 10.0f;         // how a changed feedback, drive or depth is reached
        // ==== E7 >>> ====
        // The send delay's loop: one tap on each side, read in the weights form (Cursor::readBlend);
        // the loop filter stands at the heads, so what is heard has passed it as often as what goes
        // back; and each side can be fed by the other (setCross: ping-pong). The tap's levels and
        // feedback share are not used. With the stage, the transport and the heads at rest it is
        // the arithmetic of the send delay as it always was, operation for operation.
        bool send = false;
        // The ring the send loop works its read place out in (Cursor::readBlend); 0 = the line's own.
        int ring = 0;
        // ==== <<< E7 ====
    };

    void prepare(double sr, float maxSeconds, const Setup& s)
    {
        sampleRate = sr;
        setup = s;
        const int maxSamples = (int) std::min(4.0e7, std::max(64.0, (double) maxSeconds * sr)) + 16;
        lineL.prepare(maxSamples);
        lineR.prepare(setup.stereo ? maxSamples : 16);
        maxDelay = lineL.getMaxDelay();
        timeShare = shareFor(setup.glideMs);
        gainShare = shareFor(15.0f);
        loopShare = shareFor(setup.fbMs);
        filterShare = shareFor(20.0f);
        gateShare = shareFor(1.5f);
        duckStep = 1.0f / std::max(8.0f, 0.004f * (float) sr);
        transport.prepare(sr);
        stageL.prepare(sr);
        stageR.prepare(sr);
        reset();
    }

    // Everything where it is set, the line empty.
    void reset()
    {
        lineL.clear();
        lineR.clear();
        for (auto* f : { &lpL, &lpR, &hpL, &hpR }) f->reset();
        stageL.reset();
        stageR.reset();
        transport.reset();
        for (auto& t : taps)
            for (auto* g : { &t.time, &t.gainL, &t.gainR, &t.feedback }) g->snap();
        live = count;
        gliding = turning = false;
        tapsSet = loopSet = true;
        unit.set(unitWanted);
        takeCurve();
        aimLoop();
        float first = taps[0].time.target;
        for (int k = 1; k < count; ++k) first = std::min(first, taps[(size_t) k].time.target);
        lead.set(first);
        for (auto* g : { &wide, &lead, &fb, &amount, &drive, &back, &makeup, &wow, &unit }) g->snap();
        cross.snap();       // (E7)
        ringPos = 0;        // (E7)
        speed = speedWanted;
        heads.set(speed, xfadeWanted);
        heads.reset();
        duck = duckTarget = 1.0f;
        feed.snap();                                                    // E6
        stopped = stopWanted;
        tapeNow = tapeTarget = stopped ? 0.0f : 1.0f;
        lag = 0.0;
        recording = true;
        lpNow = lpTarget; hpNow = hpTarget;
        applyFilters();
        satOn = amount.target > 0.0f;
        wowOn = wow.target > 0.0f;
    }

    // ---- what the effect asks for (taken up by the audio thread, glided to) --------------------
    void setTapCount(int n) { count = std::clamp(n, 1, MAX_TAPS); live = std::max(live, count); tapsSet = true; }

    // A tap: its time in seconds, its level on each side, its share of the feedback.
    void setTap(int k, float seconds, float left, float right, float feedbackShare)
    {
        if (k < 0 || k >= MAX_TAPS) return;
        Tap& t = taps[(size_t) k];
        t.time.set(std::clamp(seconds * (float) sampleRate, 2.0f, maxDelay));
        t.gainL.set(left); t.gainR.set(right); t.feedback.set(feedbackShare);
        // A tap that is not sounding has nowhere to glide from.
        if (t.gainL.now == 0.0f && t.gainR.now == 0.0f && t.feedback.now == 0.0f)
            t.time.snap();
        tapsSet = true;
    }

    // Taps from `from` on are silent (the effect has fewer than before).
    void silenceTapsFrom(int from)
    {
        for (int k = std::max(0, from); k < MAX_TAPS; ++k)
            for (auto* g : { &taps[(size_t) k].gainL, &taps[(size_t) k].gainR, &taps[(size_t) k].feedback }) g->set(0.0f);
        tapsSet = true;
    }

    void setFeedback(float loopGain) { fb.set(loopGain); loopSet = true; }
    // ==== E7 >>> ====
    // How much of what goes back goes to the other side (Setup::send): 0 = each side to itself,
    // 1 = each to the other, an echo that goes from side to side (ping-pong).
    void setCross(float share) { cross.set(std::clamp(share, 0.0f, 1.0f)); loopSet = true; }
    // ==== <<< E7 ====
    void setWide(float seconds) { wide.set(std::max(0.0f, seconds) * (float) sampleRate); tapsSet = true; }     // the right side reads this much later
    void setTransport(float depth) { wow.set(std::max(0.0f, depth)); loopSet = true; }
    // The loop filter's two corners; a high-pass of 0 is none.
    void setFilter(float lowPassHz, float highPassHz) { lpTarget = lowPassHz; hpTarget = highPassHz; }
    // The tape stage: how much of it (0 = the loop is clean, and it costs nothing), how hard it is hit.
    void setDrive(float share, float hit) { amount.set(std::clamp(share, 0.0f, 1.0f)); drive.set(std::max(1.0f, hit)); loopSet = true; }
    // The curve the tape stage works, and where on it. Under sound it is exchanged behind a gate:
    // what comes out of the stage is taken down to nothing over a few milliseconds (the gain that
    // undoes the drive goes to 0), the curve is changed, and it comes back.
    void setCurve(int curveIndex, float bias)
    {
        curveWanted = sat::valid(curveIndex);
        biasWanted = bias;
    }
    // The heads: 1 = they stand still (plain echo); otherwise dly::Heads. `unitSeconds` is the time
    // from one head's start to the next (a reversed chunk; half a pitch shifter's grain).
    void setRead(float playSpeed, float unitSeconds, float crossfade)
    {
        speedWanted = playSpeed;
        xfadeWanted = crossfade;
        unitWanted = unitSeconds * (float) sampleRate;
        loopSet = true;
    }

    // ---- E6: the tape's speed, and a line that is shut or stands (PERF's tape stop and beat repeat; READ: HOLD, STOP) ----
    // How much of the input reaches the line: 1 = all of it; 0 = the loop is shut and repeats what
    // it holds (how long is FDBK's to say).
    void setFeed(float gain) { feed.set(std::clamp(gain, 0.0f, 1.0f)); loopSet = true; }
    // The tape's speed: 1 = as it was recorded. Slower, the heads fall behind what is recorded
    // (every tap's time grows by 1 - speed a sample) and what they read comes out lower and
    // slower; at 0 they stand, and nothing is heard. Faster, they catch up, as far as the newest
    // sample. Gone to in a straight line over `seconds` (0 = at once).
    void setTape(float tapeSpeed, float seconds = 0.0f)
    {
        tapeTarget = std::clamp(tapeSpeed, 0.0f, 16.0f);
        if (seconds > 0.0f) tapeStep = std::abs(tapeTarget - tapeNow) / (seconds * (float) sampleRate);
        else                tapeNow = tapeTarget;
    }
    float getTape() const { return tapeNow; }
    // A slowing tape fades away as it comes to a stand (below a twelfth of its speed), so that what
    // stands is silence and not one held sample: for a tape that is being stopped. A head that is
    // only played slowly (a repeat far down) keeps its level.
    void setTapeFades(bool on) { tapeFades = on; }
    // How far the heads are behind where their times put them, in samples. Set, it is jumped to
    // (something for the dark: a gate that is shut, a ducked sound).
    float getLag() const { return (float) lag; }
    void setLag(float samples) { lag = (double) std::clamp(samples, 0.0f, maxDelay); }
    // The line records, or stands still with what it holds (nothing is written; a head at speed 1
    // then moves along it).
    void setRecording(bool on) { recording = on; }
    // The tape stops (it slows to a standstill over STOP_SECONDS) or runs again (the heads are put
    // right in the dark, as for another way of reading).
    static constexpr float STOP_SECONDS = 0.6f;
    void setStopped(bool stop) { stopWanted = stop; }
    // ---- E6 end ----

    // Every CONTROL samples: every glide is aimed, and what need not be done for every sample is done.
    void control()
    {
        // (Only what was set since the last time, or is still on its way.)
        if (tapsSet || gliding)
        {
            tapsSet = false;
            bool moving = false;
            for (int k = 0; k < live; ++k)
            {
                Tap& t = taps[(size_t) k];
                t.time.aim(timeShare);
                t.gainL.aim(gainShare); t.gainR.aim(gainShare); t.feedback.aim(gainShare);
                moving = moving || t.time.moving() || t.gainL.moving() || t.gainR.moving() || t.feedback.moving();
            }
            wide.aim(timeShare);
            // The earliest of the taps that are meant to sound (backwards, the others are counted from it).
            float first = taps[0].time.target;
            for (int k = 1; k < count; ++k) first = std::min(first, taps[(size_t) k].time.target);
            lead.set(first);
            lead.aim(timeShare);
            gliding = moving || wide.moving() || lead.moving();
            if (! moving) live = count;
        }
        if (curveWanted != curve.curve || biasWanted != curve.bias)
        {
            if (! satOn || back.now <= GATE_SHUT * backOpen) takeCurve();
            else exchanging = true;
            loopSet = true;
        }
        else if (exchanging)        // (turned back to the curve it has before the stage was shut)
        {
            exchanging = false;
            loopSet = true;
        }
        if (loopSet || turning)
        {
            loopSet = false;
            aimLoop();
            fb.aim(loopShare); amount.aim(loopShare); drive.aim(loopShare); makeup.aim(loopShare);
            back.aim(exchanging ? gateShare : loopShare);
            wow.aim(loopShare);
            // (The heads keep their stride until they are exchanged: see below.)
            if (speedWanted == speed && xfadeWanted == heads.xfade) unit.set(unitWanted);
            unit.aim(timeShare);
            turning = fb.moving() || amount.moving() || drive.moving() || back.moving() || makeup.moving() || wow.moving() || unit.moving();
            cross.aim(loopShare);                       // (E7)
            turning = turning || cross.moving();        // (E7)
            satOn = amount.target > 0.0f || amount.now > 0.0f;
            wowOn = wow.target > 0.0f || wow.now > 0.0f;
        }

        if (lpNow != lpTarget || hpNow != hpTarget)
        {
            lpNow = std::abs(lpTarget - lpNow) < 0.5f ? lpTarget : lpNow + filterShare * (lpTarget - lpNow);
            hpNow = std::abs(hpTarget - hpNow) < 0.05f ? hpTarget : hpNow + filterShare * (hpTarget - hpNow);
            applyFilters();
        }

        // Another way of reading: the wet sound is ducked for a few milliseconds, the heads are
        // exchanged in the dark.
        if (speedWanted != speed || xfadeWanted != heads.xfade)
        {
            duckTarget = 0.0f;
            if (duck <= 0.0f)
            {
                speed = speedWanted;
                heads.set(speed, xfadeWanted);
                heads.reset();
                unit.snap(unitWanted);
                duckTarget = 1.0f;
            }
        }

        // E6: the feed; the tape stopping, or running again from where its times put the heads.
        feed.aim(loopShare);
        if (stopWanted != stopped)
        {
            if (stopWanted)
            {
                stopped = true;
                setTape(0.0f, STOP_SECONDS);
            }
            else
            {
                duckTarget = 0.0f;
                if (duck <= 0.0f)
                {
                    stopped = false;
                    lag = 0.0;
                    tapeNow = tapeTarget = 1.0f;
                    duckTarget = 1.0f;
                }
            }
        }
    }

    // Nothing is on its way and nothing waits to be taken up: control() would do nothing, and
    // run() may take any number of samples.
    bool resting() const
    {
        return ! (tapsSet || gliding || loopSet || turning || exchanging || feed.moving()) && stopWanted == stopped
            && curveWanted == curve.curve && biasWanted == curve.bias
            && lpNow == lpTarget && hpNow == hpTarget
            && speedWanted == speed && xfadeWanted == heads.xfade
            && duck == 1.0f && duckTarget == 1.0f;
    }

    // `n` samples in place (no more than are left until the next control(), unless resting()): what is
    // heard is in (1 - mix) + wet mix, the mix going from mixFrom to mixTo over them (equal: exactly that mix).
    void run(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        // One loop for each way the core can be set up (2^6), so that none pays for the others.
        // (E6, the sixth: the tape's speed, a lag, a line that stands or is not fed all of the input.)
        static const auto runners = allRunners(std::make_index_sequence<64>());
        const bool taped = lag != 0.0 || tapeNow != 1.0f || tapeTarget != 1.0f || ! recording || feed.now != 1.0f || feed.moving();
        const int way = (setup.hermite ? 1 : 0) | (setup.stereo ? 2 : 0) | (heads.moving() ? 4 : 0)
                      | (setup.filterInput ? 8 : 0) | (satOn ? 16 : 0) | (taped ? 32 : 0);
        (this->*runners[(size_t) way])(left, right, n, mixFrom, mixTo);
    }

private:
    using Runner = void (Core::*)(float*, float*, int, float, float);
    template <size_t... I>
    static constexpr std::array<Runner, sizeof...(I)> allRunners(std::index_sequence<I...>) { return { { &Core::runAs<(int) I>... } }; }

    template <int Way>
    void runAs(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        constexpr bool hermite = (Way & 1) != 0, stereo = (Way & 2) != 0, headsOn = (Way & 4) != 0, before = (Way & 8) != 0, driven = (Way & 16) != 0;
        constexpr bool taped = (Way & 32) != 0;
        const bool reverse = heads.speed < 0.0f, emphasis = setup.emphasis, highPass = hpOn, unsteady = wowOn;
        const bool plain = curve.curve == sat::Fast && curve.bias == 0.0f;
        const bool ducking = duck != 1.0f || duckTarget != 1.0f;
        const sat::Biased shape = curve;
        // (Local copies: a write to the line may not be taken for a write to these.)
        fxb::OnePole lowL = lpL, lowR = lpR, highL = hpL, highR = hpR;
        TapeStage tapeL = stageL, tapeR = stageR;
        Glide g = fb, share = amount, hit = drive, undo = back, up = makeup, depth = wow;
        Line::Cursor curL = lineL.open(), curR = lineR.open();
        const float mixStep = (mixTo - mixFrom) / (float) std::max(1, n);
        float mix = mixFrom;
        // E6: the feed and the tape (see setFeed, setTape).
        Glide fd = feed;
        double lg = lag;        // (in double: it is counted up over seconds, a fraction of a sample at a time)
        float tp = tapeNow;
        const float tpTarget = tapeTarget, tpStep = tapeStep;
        const double lagMost = (double) maxDelay;
        const bool rec = ! taped || recording, tapeOn = taped && (lg != 0.0 || tp != 1.0f || tpTarget != 1.0f || ! rec);
        const bool fades = tapeOn && (tapeFades || stopped);

        auto toLine = [&](float in, float fed, TapeStage& tape, fxb::OnePole& low, fxb::OnePole& high) DLY_INLINE_LAMBDA
        {
            if (! before)
            {
                fed = low.lowPass(fed);
                if (highPass) fed = high.highPass(fed);
            }
            float x = in + fed;
            if (driven)
            {
                const float taped = emphasis ? tape.record(x, hit.now, undo.now, shape, plain)
                                             : TapeStage::shaped(x, hit.now, undo.now, shape, plain);
                x += share.now * (taped - x);
            }
            if (before)
            {
                x = low.lowPass(x);
                if (highPass) x = high.highPass(x);
            }
            // (Held without a rail: the input is within it, the clean loop's gain is below 1 and
            // the driven loop passes the curve.)
            return fxb::flushDenormal(x);
        };
        const bool turned = g.moving() || share.moving() || hit.moving() || undo.moving() || up.moving() || fd.moving();

        // ==== E7 >>> the send loop (Setup::send) ====
        const bool sendLoop = setup.send && stereo;
        Glide over = cross;
        const int ring = setup.ring > 0 ? setup.ring : curL.mask + 1;
        int pos = ringPos;
        // What goes onto a side's line: the input and what comes back in ONE expression (as the
        // send delay always wrote it), then the tape stage if it is driven.
        auto sendToLine = [&](float in, float fed, TapeStage& tape) DLY_INLINE_LAMBDA
        {
            float x = in + fed * g.now;
            if (driven)
            {
                const float taped = emphasis ? tape.record(x, hit.now, undo.now, shape, plain)
                                             : TapeStage::shaped(x, hit.now, undo.now, shape, plain);
                x += share.now * (taped - x);
            }
            return x;
        };
        // The loop filter at a head (the one-poles' own arithmetic, with nothing taken away: see the top).
        auto voiced = [&](float x, fxb::OnePole& low, fxb::OnePole& high) DLY_INLINE_LAMBDA
        {
            low.z += low.a * (x - low.z);
            x = low.z;
            if (highPass)
            {
                high.z += high.a * (x - high.z);
                x = x - high.z;
            }
            return x;
        };
        // ==== <<< E7 ====

        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];
            if (gliding) glide();
            // (A tape that stands gives nothing: the last sample is not held as an offset.)
            const float hush = taped && fades ? std::min(1.0f, tp * 12.0f) : 1.0f, lagNow = taped ? (float) lg : 0.0f;
            if (taped && tapeOn)
            {
                // (For the next sample: the heads fall behind by what the line moves on and they do not.)
                lg = std::clamp(lg + (rec ? 1.0 : 0.0) - (double) tp, 0.0, lagMost);
                tp += std::clamp(tpTarget - tp, -tpStep, tpStep);
            }
            float scale = 1.0f;
            if (unsteady)
            {
                scale += depth.now * transport.next();
                depth.now += depth.step;
            }

            // ==== E7 >>> the send loop: read, filter at the heads, cross, write, mix ====
            if (sendLoop)
            {
                const float d = unsteady ? taps[0].time.now * scale : taps[0].time.now;
                float a, b;
                if (! headsOn)
                {
                    a = curL.readBlend(d, pos, ring);
                    b = curR.readBlend(d, pos, ring);
                }
                else
                {
                    heads.next(unit.now);
                    unit.now += unit.step;
                    const float from = std::max(d - (reverse ? lead.now : 0.5f * heads.span), 0.0f) + 2.0f;
                    a = heads.read(curL, from);
                    b = heads.read(curR, from);
                }
                if (ducking)
                {
                    duck += std::clamp(duckTarget - duck, -duckStep, duckStep);
                    a *= duck; b *= duck;
                }
                a = voiced(a, lowL, highL);
                b = voiced(b, lowR, highR);

                float toL = a, toR = b;
                if (over.now != 0.0f)
                {
                    if (over.now == 1.0f) { toL = b; toR = a; }
                    else { toL = a + over.now * (b - a); toR = b + over.now * (a - b); }
                }
                over.now += over.step;
                curL.write(sendToLine(inL, toL, tapeL));
                curR.write(sendToLine(inR, toR, tapeR));
                if (++pos >= ring) pos = 0;

                const float wet = up.now * mix;
                left[i] = inL * (1.0f - mix) + a * wet;
                right[i] = inR * (1.0f - mix) + b * wet;
            }
            else
            {
            // ==== <<< E7 (the loop of every other delay follows, as it was) ====
            float outL = 0.0f, outR = 0.0f, backL = 0.0f, backR = 0.0f;
            if (! headsOn)
            {
                for (int k = 0; k < live; ++k)
                {
                    const Tap& t = taps[(size_t) k];
                    const float d = taped ? (unsteady ? t.time.now * scale : t.time.now) + lagNow : (unsteady ? t.time.now * scale : t.time.now);
                    const float a = hermite ? curL.readHermite(d) : curL.readLinear(d);
                    outL += t.gainL.now * a;
                    backL += t.feedback.now * a;
                    if (stereo)
                    {
                        const float b = hermite ? curR.readHermite(d + wide.now) : curR.readLinear(d + wide.now);
                        outR += t.gainR.now * b;
                        backR += t.feedback.now * b;
                    }
                    else
                        outR += t.gainR.now * a;
                }
            }
            else
            {
                heads.next(unit.now);
                unit.now += unit.step;
                // Backwards, the first tap starts at the write point (a chunk is heard as soon as it
                // is complete) and the others follow by their distance from it; shifted, the heads
                // sweep round the tap's own time.
                const float ahead = reverse ? lead.now : 0.5f * heads.span;
                for (int k = 0; k < live; ++k)
                {
                    const Tap& t = taps[(size_t) k];
                    const float d = taped ? std::max(t.time.now * scale - ahead, 0.0f) + 2.0f + lagNow : std::max(t.time.now * scale - ahead, 0.0f) + 2.0f;
                    const float a = heads.read(curL, d);
                    outL += t.gainL.now * a;
                    backL += t.feedback.now * a;
                    if (stereo)
                    {
                        const float b = heads.read(curR, d + wide.now);
                        outR += t.gainR.now * b;
                        backR += t.feedback.now * b;
                    }
                    else
                        outR += t.gainR.now * a;
                }
            }

            if (ducking)
            {
                duck += std::clamp(duckTarget - duck, -duckStep, duckStep);
                outL *= duck; outR *= duck; backL *= duck; backR *= duck;
            }
            if (taped && hush < 1.0f)
            {
                outL *= hush; outR *= hush; backL *= hush; backR *= hush;
            }

            // What goes back onto the line (E6: as much of the input as is fed; nothing while it stands).
            const float fedIn = taped ? fd.now : 1.0f;
            if (taped && ! rec)
                ;
            else if (stereo)
            {
                curL.write(toLine(taped ? inL * fedIn : inL, backL * g.now, tapeL, lowL, highL));
                curR.write(toLine(taped ? inR * fedIn : inR, backR * g.now, tapeR, lowR, highR));
            }
            else
                curL.write(toLine(taped ? 0.5f * (inL + inR) * fedIn : 0.5f * (inL + inR), backL * g.now, tapeL, lowL, highL));

            const float wet = up.now * mix;
            left[i] = inL * (1.0f - mix) + outL * wet;
            right[i] = inR * (1.0f - mix) + outR * wet;
            }       // (E7: the end of "else" above)

            mix += mixStep;
            if (turned)
            {
                g.now += g.step; share.now += share.step; hit.now += hit.step; undo.now += undo.step; up.now += up.step;
                if (taped) fd.now += fd.step;
            }
        }
        feed.now = fd.now; lag = lg; tapeNow = tp;      // E6

        lineL.close(curL);
        lineR.close(curR);
        lpL = lowL; lpR = lowR; hpL = highL; hpR = highR;
        stageL = tapeL; stageR = tapeR;
        fb.now = g.now; amount.now = share.now; drive.now = hit.now; back.now = undo.now; makeup.now = up.now; wow.now = depth.now;
        cross.now = over.now; ringPos = pos;        // (E7)
    }

    struct Tap
    {
        Glide time { 4800.0f, 4800.0f, 0.0f };      // samples
        Glide gainL, gainR, feedback;
    };

    float shareFor(float ms) const { return glideShare(ms, sampleRate); }

    void glide()
    {
        for (int k = 0; k < live; ++k)
        {
            Tap& t = taps[(size_t) k];
            t.time.now += t.time.step;
            t.gainL.now += t.gainL.step;
            t.gainR.now += t.gainR.step;
            t.feedback.now += t.feedback.step;
        }
        wide.now += wide.step;
        lead.now += lead.step;
    }

    // What follows from the drive and the curve: the gain that undoes them for a quiet sound, and
    // what brings the held level back up.
    void aimLoop()
    {
        backOpen = 1.0f / (drive.target * slope);
        back.set(exchanging ? 0.0f : backOpen);
        makeup.set(setup.makeup ? 1.0f + amount.target * (std::sqrt(drive.target) - 1.0f) : 1.0f);
    }

    // The curve that was asked for, and its gain for a quiet sound where it is worked. A curve that
    // is flat or turns there (RECT and XOVR at the centre: sat::Biased gives its floor) has none to
    // undo: it is taken by its gain for a sound of half scale instead, or what it lets through
    // would come back five times louder.
    void takeCurve()
    {
        static constexpr float FLAT = 0.2f, HALF = 0.5f;
        curve.set(curveWanted, biasWanted);
        exchanging = false;
        slope = curve.slope;
        if (slope <= FLAT)
            slope = std::max(slope, std::max(std::abs(curve(HALF)), std::abs(curve(-HALF))) / HALF);
    }

    void applyFilters()
    {
        lpL.setCutoff(sampleRate, std::max(20.0f, lpNow));
        lpR.setCutoff(sampleRate, std::max(20.0f, lpNow));
        hpOn = hpNow > 0.0f;
        hpL.setCutoff(sampleRate, std::max(1.0f, hpNow));
        hpR.setCutoff(sampleRate, std::max(1.0f, hpNow));
    }

    double sampleRate = 44100.0;
    Setup setup;
    Line lineL, lineR;
    float maxDelay = 16.0f;

    std::array<Tap, MAX_TAPS> taps;
    int count = 1, live = 1;
    bool gliding = false, turning = false, tapsSet = true, loopSet = true;
    float timeShare = 1.0f, gainShare = 1.0f, loopShare = 1.0f, filterShare = 1.0f;

    Glide wide, lead, fb, amount, wow;
    Glide cross;        // (E7) Setup::send: the share of the feedback that changes sides
    int ringPos = 0;    // (E7) Setup::send: the samples written since reset(), round Setup::ring
    Glide drive { 1.0f, 1.0f, 0.0f }, back { 1.0f, 1.0f, 0.0f }, makeup { 1.0f, 1.0f, 0.0f }, unit { 4800.0f, 4800.0f, 0.0f };
    bool satOn = false, wowOn = false;

    fxb::OnePole lpL, lpR, hpL, hpR;
    float lpTarget = 8000.0f, hpTarget = 0.0f, lpNow = 8000.0f, hpNow = 0.0f;
    bool hpOn = false;

    TapeStage stageL, stageR;
    sat::Biased curve;
    int curveWanted = sat::Fast;
    float biasWanted = 0.0f, slope = 1.0f;
    static constexpr float GATE_SHUT = 1.0e-3f;     // the stage's share of its gain at which its curve is exchanged (60 dB down)
    float backOpen = 1.0f, gateShare = 1.0f;
    bool exchanging = false;                        // the stage is being shut, or is shut, for another curve
    Transport transport;
    Heads heads;
    float speed = 1.0f, speedWanted = 1.0f, xfadeWanted = 1.0f, unitWanted = 4800.0f;
    float duck = 1.0f, duckTarget = 1.0f, duckStep = 0.01f;

    // E6: the feed, the tape
    Glide feed { 1.0f, 1.0f, 0.0f };
    double lag = 0.0;
    float tapeNow = 1.0f, tapeTarget = 1.0f, tapeStep = 32.0f;
    bool recording = true, stopWanted = false, stopped = false, tapeFades = false;
};

// ---- E6: ONE list of the ways a line is played ------------------------------------------------------
// The intervals a moving head can play (dly::Heads: its speed), and after them the two ways of
// playing that are not an interval. The delays' READ cell is this list in this order (dly::read);
// the reverbs' MODE cell is a view of it (rvb::shift, ReverbParts.h). Stored by index: it only
// grows at the end.
namespace interval
{
enum Id { Forward = 0, Reverse, OctaveUp, FifthUp, OctaveDown, OctaveFifth, Sub, Hold, Stop, Count };
struct Entry { const char* name; float speed; };
inline const Entry& entry(int id)
{
    static const Entry list[Count] = {
        { "FWD", 1.0f },            // as it was recorded
        { "REV", -1.0f },           // backwards, a stretch at a time
        { "OCT+", 2.0f },           // an octave up every time round
        { "5TH+", 1.4983071f },     // a fifth up
        { "OCT-", 0.5f },           // an octave down
        { "O+5", 2.9966142f },      // an octave and a fifth up
        { "SUB", 0.25f },           // two octaves down
        { "HOLD", 1.0f },           // the loop is shut: it repeats what it holds
        { "STOP", 1.0f } };         // the tape slows to a standstill
    return list[std::clamp(id, 0, (int) Count - 1)];
}
}

// How the loop is read: the choices of the READ cell (the list above, in its order).
namespace read
{
enum Mode { Forward = interval::Forward, Reverse = interval::Reverse, OctaveUp = interval::OctaveUp, FifthUp = interval::FifthUp,
            OctaveDown = interval::OctaveDown, OctaveFifth = interval::OctaveFifth, Sub = interval::Sub, Hold = interval::Hold,
            Stop = interval::Stop, Count = interval::Count };
inline const char* const* names()
{
    static const char* n[Count] = {};
    static const bool made = [] { for (int i = 0; i < Count; ++i) n[i] = interval::entry(i).name; return true; }();
    (void) made;
    return n;
}
inline float speedOf(int mode) { return interval::entry(mode).speed; }
static constexpr float GRAIN_UNIT = 0.015f;     // half a pitch shifter's grain, seconds
static constexpr float REVERSE_FADE = 0.1f;     // a reversed chunk's crossfade into the next, as a share of it
// Room on the line for a reversed chunk of `seconds` (its head runs back twice as far as it plays).
inline float reverseRoom(float seconds) { return 2.0f * (1.0f + REVERSE_FADE) * seconds; }
}

static constexpr float MAX_WIDE = 0.02f;        // seconds the right side may read later (WIDE)

// The line an effect needs, in seconds: its longest tap with wow at its deepest, and behind that a
// reversed chunk of its longest TIME.
inline float roomFor(float longestTap, float longestTime)
{
    return longestTap * 1.02f + MAX_WIDE + read::reverseRoom(longestTime) + 0.05f;
}

// A tape machine's playback heads: `count` of them evenly spaced up to `time`. The last one is the
// echo proper and feeds back; the earlier ones are a little quieter.
inline void heads(Core& core, float time, int count, int most)
{
    static constexpr float EARLY = 0.7f;
    count = std::clamp(count, 1, std::min(most, Core::MAX_TAPS));
    core.setTapCount(count);
    for (int i = 0; i < count; ++i)
    {
        const bool last = i == count - 1;
        core.setTap(i, time * (float) (i + 1) / (float) count, last ? 1.0f : EARLY, last ? 1.0f : EARLY, last ? 1.0f : 0.0f);
    }
    core.silenceTapsFrom(count);
}

// The DRIV and SAT cells of a delay that is clean at rest: the tape stage comes in over the first
// eighth of DRIV (nothing jumps when it leaves 0), then is hit up to eight times harder.
inline void driven(Core& core, float driv, float curve)
{
    core.setDrive(std::min(1.0f, driv * 8.0f), 1.0f + 7.0f * driv);
    core.setCurve((int) std::lround(curve), TapeStage::BIAS);
}

// -------------------------------------------------------------------------------------------
// An effect that is a setting of the core: its cells come from one table (CellEffect.h), and all
// it writes itself is configure(), which turns them into taps, a loop and a way of reading.
// -------------------------------------------------------------------------------------------
class Family : public CellEffect
{
protected:
    virtual Core::Setup setup() const = 0;
    virtual float lineSeconds() const = 0;

    void prepareCore() override { core.prepare(sampleRate, lineSeconds(), setup()); }
    void resetCore() override { core.reset(); }
    void controlCore() override { core.control(); }
    bool coasting() const override { return core.resting(); }
    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override { core.run(left, right, n, mixFrom, mixTo); }

    // The READ cell's choice into the core; `time` is the echo's own (a reversed chunk).
    void setRead(float choice, float time)
    {
        const int mode = std::clamp((int) std::lround(choice), 0, (int) read::Count - 1);
        core.setFeed(mode == read::Hold ? 0.0f : 1.0f);         // E6: HOLD shuts the loop, STOP stops the tape
        core.setStopped(mode == read::Stop);
        if (mode == read::Reverse)
            core.setRead(-1.0f, time, read::REVERSE_FADE);
        else
            core.setRead(read::speedOf(mode), read::GRAIN_UNIT, 1.0f);
    }

    Core core;
};

} // namespace dly
