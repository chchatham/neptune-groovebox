#pragma once

// dly::Core and rvb::Core as they were at b4ee701, compiled beside the new ones under other names
// (e6dly, e6rvb), for one purpose: the cost of what phase E6 added to them, old against new in one
// binary (suite "Effect Cores Sweep", NEPTUNE_E6_TIME=1). Nothing in the product includes this file.

#include "../src/dsp/effects/DelayCore.h"

// One delay line for every echo: the tape delay, the L/C/R delay, the multi-tap, the reverse delay
// and PERF's dub echo are settings of e6dly::Core, and so each has what the others have.
//
//   in -+-> [ + ] -> tape stage (a curve of Saturation.h, drive, bias, emphasis) -> loop filter -> line
//       |     ^                                                                                  |
//       |     +---- x FDBK <---- sum of the taps' feedback shares <-----------------------------+
//       |                                                                                        |
//       +--------------------------- dry / wet <---- sum of the taps (level, side) <-------------+
//
// - The line (e6dly::Line) is read between samples, linearly or with a Hermite curve.
// - Up to eight taps, each with its own time, level on either side and share of the feedback.
// - Every time is glided to, never jumped to: a turned TIME bends the pitch of what is in the line,
//   as a tape changing speed does. The transport adds wow and flutter; at depth 0 it costs nothing.
// - The read heads can move (e6dly::Heads: two heads that take turns, crossfaded): played backwards,
//   that is the reverse delay; played faster or slower, every repeat comes back higher or lower.
// - The loop passes a saturation curve when it is driven, and is held for any curve and any
//   feedback: above a loop gain of 1 it howls at a level, it does not run away.
//
// Nothing here allocates after prepare(). All of it is the textbook structure (Zoelzer, "DAFX",
// delays chapter; the two-head pitch shifter is the delay-line shifter of the same chapter).

namespace e6dly
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
            const float x0 = at(di), x1 = at(di + 1);
            return x0 + fr * (x1 - x0);
        }

        DLY_INLINE float readHermite(float d) const
        {
            d = d >= 2.0f ? std::min(d, maxDelay) : 2.0f;
            const int di = (int) d;
            const float fr = d - (float) di;
            const float xm1 = at(di - 1), x0 = at(di), x1 = at(di + 1), x2 = at(di + 2);
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * fr + c2) * fr + c1) * fr + x0;
        }

    private:
        DLY_INLINE float at(int delay) const { return buf[(w - delay) & mask]; }
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
    }

    void clear()
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        w = 0;
    }

    float getMaxDelay() const { return maxDelay; }
    Cursor open() { return { buf.data(), mask, w, maxDelay }; }
    void close(const Cursor& c) { w = c.w; }

private:
    std::vector<float> buf;
    int mask = 0, w = 0;
    float maxDelay = 1.0f;
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
        speed = speedWanted;
        heads.set(speed, xfadeWanted);
        heads.reset();
        duck = duckTarget = 1.0f;
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
    // The heads: 1 = they stand still (plain echo); otherwise e6dly::Heads. `unitSeconds` is the time
    // from one head's start to the next (a reversed chunk; half a pitch shifter's grain).
    void setRead(float playSpeed, float unitSeconds, float crossfade)
    {
        speedWanted = playSpeed;
        xfadeWanted = crossfade;
        unitWanted = unitSeconds * (float) sampleRate;
        loopSet = true;
    }

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
    }

    // Nothing is on its way and nothing waits to be taken up: control() would do nothing, and
    // run() may take any number of samples.
    bool resting() const
    {
        return ! (tapsSet || gliding || loopSet || turning || exchanging)
            && curveWanted == curve.curve && biasWanted == curve.bias
            && lpNow == lpTarget && hpNow == hpTarget
            && speedWanted == speed && xfadeWanted == heads.xfade
            && duck == 1.0f && duckTarget == 1.0f;
    }

    // `n` samples in place (no more than are left until the next control(), unless resting()): what is
    // heard is in (1 - mix) + wet mix, the mix going from mixFrom to mixTo over them (equal: exactly that mix).
    void run(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        // One loop for each way the core can be set up (2^5), so that none pays for the others.
        static const auto runners = allRunners(std::make_index_sequence<32>());
        const int way = (setup.hermite ? 1 : 0) | (setup.stereo ? 2 : 0) | (heads.moving() ? 4 : 0)
                      | (setup.filterInput ? 8 : 0) | (satOn ? 16 : 0);
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
        const bool turned = g.moving() || share.moving() || hit.moving() || undo.moving() || up.moving();

        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];
            if (gliding) glide();
            float scale = 1.0f;
            if (unsteady)
            {
                scale += depth.now * transport.next();
                depth.now += depth.step;
            }

            float outL = 0.0f, outR = 0.0f, backL = 0.0f, backR = 0.0f;
            if (! headsOn)
            {
                for (int k = 0; k < live; ++k)
                {
                    const Tap& t = taps[(size_t) k];
                    const float d = unsteady ? t.time.now * scale : t.time.now;
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
                    const float d = std::max(t.time.now * scale - ahead, 0.0f) + 2.0f;
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

            // What goes back onto the line.
            if (stereo)
            {
                curL.write(toLine(inL, backL * g.now, tapeL, lowL, highL));
                curR.write(toLine(inR, backR * g.now, tapeR, lowR, highR));
            }
            else
                curL.write(toLine(0.5f * (inL + inR), backL * g.now, tapeL, lowL, highL));

            const float wet = up.now * mix;
            left[i] = inL * (1.0f - mix) + outL * wet;
            right[i] = inR * (1.0f - mix) + outR * wet;

            mix += mixStep;
            if (turned)
            {
                g.now += g.step; share.now += share.step; hit.now += hit.step; undo.now += undo.step; up.now += up.step;
            }
        }

        lineL.close(curL);
        lineR.close(curR);
        lpL = lowL; lpR = lowR; hpL = highL; hpR = highR;
        stageL = tapeL; stageR = tapeR;
        fb.now = g.now; amount.now = share.now; drive.now = hit.now; back.now = undo.now; makeup.now = up.now; wow.now = depth.now;
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
};

// How the loop is read: the choices of the READ cell (stored: the list only grows at the end).
namespace read
{
enum Mode { Forward = 0, Reverse, OctaveUp, FifthUp, OctaveDown, Count };
inline const char* const* names()
{
    static const char* const n[Count] = { "FWD", "REV", "OCT+", "5TH+", "OCT-" };
    return n;
}
inline float speedOf(int mode)
{
    static const float s[Count] = { 1.0f, -1.0f, 2.0f, 1.4983071f, 0.5f };
    return s[std::clamp(mode, 0, (int) Count - 1)];
}
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
        if (mode == read::Reverse)
            core.setRead(-1.0f, time, read::REVERSE_FADE);
        else
            core.setRead(read::speedOf(mode), read::GRAIN_UNIT, 1.0f);
    }

    Core core;
};

} // namespace e6dly

// The parts a reverb is made of, and one loop that puts them together: the spring reverb and the
// shimmer reverb are settings of e6rvb::Core, and so each has what the other has.
//
//   in -> pre-delay -> input diffusers (allpass chain) -+-> [ + ] -----------------------> loop line A -+-> out
//                                                       |     ^                                         |
//                                                       |     +- x decay <- loop allpass (modulated) <- damping <- dispersion <- shifted heads
//                                                       +-> the same for loop B; the two feed themselves (two springs) or each other (a tank)
//
// - PreDelay: the dry sound arrives first.
// - Allpass (Schroeder: v = x + g d, y = d - g v): smears an impulse without colouring it; three of
//   them before the loops, one inside each loop whose length can be moved by a slow sine (the
//   modulated allpass of a plate or hall: it breaks up the loop's ringing).
// - Damping: a low-pass in the loop, the top dies first.
// - Dispersion: a cascade of stretched first-order allpasses, in which the low frequencies of a
//   spring's band travel faster than the high ones: every round trip a click becomes more of a chirp
//   (the structure of Parker, "Efficient dispersion generation structures for spring reverb
//   emulation", EURASIP 2011, and Valimaki, Parker & Abel, "Parametric spring reverberation effect",
//   JAES 2010: the allpass (a + z^-k) / (1 + a z^-k), k = the stretch that puts the chirp below ~4.3 kHz).
// - Shifted heads (e6dly::Heads): what comes round is read by two moving heads at another pitch, the
//   shimmer; in any loop, at any interval.
// The loop lines are e6dly::Line, the glides e6dly::Glide: a length that follows a control is glided to.
namespace e6rvb
{
// Schroeder allpass on a line read between samples.
struct Allpass
{
    void prepare(int maxSamples) { line.prepare(maxSamples); }
    void clear() { line.clear(); }

    DLY_INLINE float process(float x, float delay, float g)
    {
        e6dly::Line::Cursor c = line.open();
        const float d = c.readLinear(delay);
        const float v = x + g * d;
        c.write(fxb::flushDenormal(v));
        line.close(c);
        return d - g * v;
    }

    e6dly::Line line;
};

// The spring's dispersion for two channels at once: STAGES allpasses (a + z^-k) / (1 + a z^-k) each.
// With a = 0 it is a plain delay of STAGES x k samples.
struct Dispersion
{
    static constexpr int STAGES = 12, MAX_STRETCH = 32;
    static constexpr double CORNER_HZ = 4300.0;

    void prepare(double sr)
    {
        stretch = std::clamp((int) std::lround(sr / (2.0 * CORNER_HZ)), 1, MAX_STRETCH);
        clear();
    }

    void clear()
    {
        buf.fill(0.0f);
        pos = 0;
    }

    DLY_INLINE void process(float& x0, float& x1, float a)
    {
        float* p = buf.data() + (size_t) pos * 2;
        float y0 = x0, y1 = x1;
        for (int s = 0; s < STAGES; ++s, p += MAX_STRETCH * 2)
        {
            const float d0 = p[0], d1 = p[1];
            const float v0 = y0 - a * d0, v1 = y1 - a * d1;
            p[0] = v0;
            p[1] = v1;
            y0 = d0 + a * v0;
            y1 = d1 + a * v1;
        }
        if (++pos >= stretch) pos = 0;
        x0 = fxb::flushDenormal(y0);
        x1 = fxb::flushDenormal(y1);
    }

    std::array<float, (size_t) STAGES * MAX_STRETCH * 2> buf {};
    int stretch = 5, pos = 0;
};

// The intervals the shifted heads can play (stored: the list only grows at the end).
namespace shift
{
enum Mode { Octave = 0, Fifth, OctaveFifth, Sub, Count };
inline const char* const* names()
{
    static const char* const n[Count] = { "OCT", "5TH", "O+5", "SUB" };
    return n;
}
inline float speedOf(int mode)
{
    static const float s[Count] = { 2.0f, 1.4983071f, 2.9966142f, 0.5f };
    return s[std::clamp(mode, 0, (int) Count - 1)];
}
}

class Core
{
public:
    static constexpr int MAX_DIFFUSERS = 3;
    static constexpr float MAX_PRE_DELAY = 0.25f;       // seconds
    static constexpr float MAX_MOD_MS = 1.0f;           // how far the sine moves a length at full depth
    static constexpr double MOD_HZ = 0.9;

    struct Setup
    {
        float loopMs[2] = { 41.0f, 53.0f };             // the two loops at size 1
        float maxSize = 2.0f;                           // the largest size they are asked for
        bool cross = false;                             // each loop feeds the other (a tank) or itself (two springs)
        int diffusers = 0;                              // input allpasses, 0..3; with any, the two inputs are summed
        float diffuserMs[MAX_DIFFUSERS] = { 14.1f, 26.0f, 37.5f };
        float diffusion = 0.65f;
        float loopAllpassMs[2] = { 0.0f, 0.0f };        // the allpass inside each loop; 0 = none (the sine then moves the loop itself)
        float loopDiffusion = 0.5f;
        bool dispersionAlways = false;                  // the dispersion is part of the sound (a spring), or comes in when asked for
        bool taps = false;                              // heard from inside the loop lines as well (a tank), or at their ends alone
        float crossOut = 0.0f;                          // how much of the other side each output hears
        float brightHz = 16000.0f, darkHz = 1200.0f;    // the damping's corner at 0 and at 1
        float inGain = 1.0f, outGain = 1.0f;
    };

    void prepare(double sr, const Setup& s)
    {
        sampleRate = sr;
        setup = s;
        setup.diffusers = std::clamp(setup.diffusers, 0, MAX_DIFFUSERS);
        const float ms = 0.001f * (float) sr;
        for (int i = 0; i < 2; ++i)
        {
            tank[i].prepare((int) (setup.loopMs[i] * setup.maxSize * ms + MAX_MOD_MS * ms) + 16);
            pre[i].prepare((int) (MAX_PRE_DELAY * (float) sr) + 16);
            loopAllpassLen[i] = std::max(4.0f, std::floor(setup.loopAllpassMs[i] * ms));
            loopAllpass[i].prepare((int) (loopAllpassLen[i] + MAX_MOD_MS * ms) + 16);
        }
        for (int i = 0; i < MAX_DIFFUSERS; ++i)
        {
            diffuserLen[i] = std::max(2.0f, std::floor(setup.diffuserMs[i] * ms));
            diffuser[i].prepare(i < setup.diffusers ? (int) diffuserLen[i] + 16 : 16);
        }
        hasLoopAllpass = setup.loopAllpassMs[0] > 0.0f && setup.loopAllpassMs[1] > 0.0f;
        dispersion.prepare(sr);
        lengthShare = shareFor(80.0f);
        loopShare = shareFor(15.0f);
        sway.prepare(MOD_HZ, sr);
        reset();
    }

    // Everything where it is set, the lines empty.
    void reset()
    {
        for (int i = 0; i < 2; ++i)
        {
            tank[i].clear();
            pre[i].clear();
            loopAllpass[i].clear();
            damp[i].reset();
        }
        for (auto& d : diffuser) d.clear();
        dispersion.clear();
        aim();
        shimmer.set(shimmerWanted);
        dispersionMix.set(dispersionOn() ? 1.0f : 0.0f);
        for (auto* g : { &length[0], &length[1], &preDelay, &gain, &shimmer, &dispersionGain, &dispersionMix, &modDepth, &unit })
            g->snap();
        heads.set(speedWanted, 1.0f);
        heads.reset();
        sway.reset();
        dampNow = dampTarget;
        applyDamping();
        moving = false;
        changed = true;
    }

    // ---- what the effect asks for (glided to) --------------------------------------------------
    void setDecay(float seconds) { decay = std::max(0.01f, seconds); changed = true; }                 // to -60 dB
    void setSize(float scale) { size = std::clamp(scale, 0.05f, setup.maxSize); changed = true; }       // the loops' length
    void setDamping(float amount) { dampTarget = setup.brightHz * std::pow(setup.darkHz / setup.brightHz, std::clamp(amount, 0.0f, 1.0f)); }
    void setPreDelay(float seconds) { preDelay.set(std::clamp(seconds, 0.0f, MAX_PRE_DELAY) * (float) sampleRate); changed = true; }
    // What comes round is read at `speed` times its pitch, this much of it (0 = none, and it costs nothing).
    void setShimmer(float amount, float speed) { shimmerWanted = std::clamp(amount, 0.0f, 1.0f); speedWanted = speed; changed = true; }
    void setDispersion(float amount) { dispersionGain.set(0.8f * std::clamp(amount, 0.0f, 1.0f)); changed = true; }
    void setModulation(float depth) { modDepth.set(std::clamp(depth, 0.0f, 1.0f) * MAX_MOD_MS * 0.001f * (float) sampleRate); changed = true; }

    void control()
    {
        if (changed || moving)
        {
            changed = false;
            aim();
            // Another interval: the shimmer is taken out, the heads exchanged, and it comes back.
            if (speedWanted != heads.speed)
            {
                shimmer.set(0.0f);
                if (shimmer.now == 0.0f) { heads.set(speedWanted, 1.0f); heads.reset(); }
            }
            else
                shimmer.set(shimmerWanted);
            dispersionMix.set(dispersionOn() ? 1.0f : 0.0f);
            if (dispersionMix.target > 0.0f && dispersionMix.now == 0.0f && ! dispersing)
                dispersion.clear();     // (coming in: nothing stale in it)
            for (auto* g : { &length[0], &length[1], &preDelay, &unit }) g->aim(lengthShare);
            for (auto* g : { &gain, &shimmer, &dispersionGain, &dispersionMix, &modDepth }) g->aim(loopShare);
            moving = false;
            for (auto* g : { &length[0], &length[1], &preDelay, &unit, &gain, &shimmer, &dispersionGain, &dispersionMix, &modDepth })
                moving = moving || g->moving();
            moving = moving || speedWanted != heads.speed;
            shimmering = shimmer.target > 0.0f || shimmer.now > 0.0f;
            dispersing = dispersionMix.target > 0.0f || dispersionMix.now > 0.0f;
            swaying = modDepth.target > 0.0f || modDepth.now > 0.0f;
            delayed = preDelay.target > 0.0f || preDelay.now > 0.0f;
        }
        if (dampNow != dampTarget)
        {
            dampNow = std::abs(dampTarget - dampNow) < 0.5f ? dampTarget : dampNow + loopShare * (dampTarget - dampNow);
            applyDamping();
        }
    }

    // Nothing is on its way: control() would do nothing, and run() may take any number of samples.
    bool resting() const { return ! (changed || moving) && dampNow == dampTarget; }

    // `n` samples in place (no more than are left until the next control(), unless resting()): in (1 - mix) + wet mix.
    void run(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        const bool cross = setup.cross, taps = setup.taps, diffuse = setup.diffusers > 0, loopAp = hasLoopAllpass;
        const bool shim = shimmering, disperse = dispersing, swayed = swaying, late = delayed, glide = moving;
        const float diffusion = setup.diffusion, loopDiffusion = setup.loopDiffusion, inGain = setup.inGain, outGain = setup.outGain;
        const float crossOut = setup.crossOut;
        e6dly::Line::Cursor a = tank[0].open(), b = tank[1].open(), pa = pre[0].open(), pb = pre[1].open();
        fxb::OnePole dampA = damp[0], dampB = damp[1];
        e6dly::Glide lenA = length[0], lenB = length[1], g = gain, sh = shimmer, da = dispersionGain, dm = dispersionMix,
                   md = modDepth, pd = preDelay, un = unit;
        const float mixStep = (mixTo - mixFrom) / (float) std::max(1, n);
        float mix = mixFrom;

        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];

            // The way in: pre-delay, then the diffusers.
            float xa = inL, xb = inR;
            pa.write(inL);
            pb.write(inR);
            if (late)
            {
                // (Read after the write: a delay of d is then d - 1 calls back, so 1 is "now".)
                xa = pa.readLinear(pd.now + 1.0f);
                xb = pb.readLinear(pd.now + 1.0f);
            }
            if (diffuse)
            {
                float m = 0.5f * (xa + xb);
                for (int k = 0; k < setup.diffusers; ++k)
                    m = diffuser[k].process(m, diffuserLen[k], diffusion);
                xa = xb = m;
            }

            // The slow sine: one side by its sine, the other by its cosine.
            float swayA = 0.0f, swayB = 0.0f;
            if (swayed)
            {
                swayA = md.now * 0.5f * (1.0f + sway.sin);
                swayB = md.now * 0.5f * (1.0f + sway.cos);
                sway.next();
            }

            // What comes round.
            const float atA = loopAp ? lenA.now : lenA.now + swayA, atB = loopAp ? lenB.now : lenB.now + swayB;
            float ya = a.readLinear(atA), yb = b.readLinear(atB);
            if (shim)
            {
                heads.next(un.now);
                const float upA = heads.read(a, std::max(2.0f, atA - heads.span));
                const float upB = heads.read(b, std::max(2.0f, atB - heads.span));
                ya += sh.now * (upA - ya);
                yb += sh.now * (upB - yb);
            }
            if (disperse)
            {
                float da0 = ya, da1 = yb;
                dispersion.process(da0, da1, da.now);
                ya += dm.now * (da0 - ya);
                yb += dm.now * (da1 - yb);
            }
            ya = dampA.lowPass(ya);
            yb = dampB.lowPass(yb);
            if (loopAp)
            {
                ya = loopAllpass[0].process(ya, loopAllpassLen[0] + swayA, loopDiffusion);
                yb = loopAllpass[1].process(yb, loopAllpassLen[1] + swayB, loopDiffusion);
            }

            // What is heard: the loops' ends, and (a tank) two places inside each line.
            float outL, outR;
            if (taps)
            {
                outL = 0.5f * (ya + a.readLinear(lenA.now * 0.317f) + 0.6f * b.readLinear(lenB.now * 0.646f));
                outR = 0.5f * (yb + b.readLinear(lenB.now * 0.435f) + 0.6f * a.readLinear(lenA.now * 0.650f));
            }
            else
            {
                outL = ya + crossOut * (yb - ya);
                outR = yb + crossOut * (ya - yb);
            }

            // Back into the loops: held whatever comes round (straight up to HEADROOM, never beyond twice that).
            a.write(fxb::flushDenormal(held(xa * inGain + g.now * (cross ? yb : ya))));
            b.write(fxb::flushDenormal(held(xb * inGain + g.now * (cross ? ya : yb))));

            const float wet = outGain * mix;
            left[i] = inL * (1.0f - mix) + outL * wet;
            right[i] = inR * (1.0f - mix) + outR * wet;
            mix += mixStep;

            if (glide)
            {
                lenA.now += lenA.step; lenB.now += lenB.step; g.now += g.step; sh.now += sh.step; da.now += da.step;
                dm.now += dm.step; md.now += md.step; pd.now += pd.step; un.now += un.step;
            }
        }

        tank[0].close(a); tank[1].close(b); pre[0].close(pa); pre[1].close(pb);
        damp[0] = dampA; damp[1] = dampB;
        length[0].now = lenA.now; length[1].now = lenB.now; gain.now = g.now; shimmer.now = sh.now; dispersionGain.now = da.now;
        dispersionMix.now = dm.now; modDepth.now = md.now; preDelay.now = pd.now; unit.now = un.now;
    }

private:
    static constexpr float HEADROOM = 8.0f;     // a long tail under a loud sound adds up to several times full scale
    static DLY_INLINE float held(float x) { return HEADROOM * e6dly::ceiling(x * (1.0f / HEADROOM)); }

    float shareFor(float ms) const { return e6dly::glideShare(ms, sampleRate); }
    bool dispersionOn() const { return setup.dispersionAlways || dispersionGain.target > 0.0f; }

    // What follows from the size and the decay: the loops' lengths (whole samples: at rest nothing
    // is read between two), the gain that takes a round trip down by its share of 60 dB, and a
    // grain for the heads that fits the shorter loop.
    void aim()
    {
        const float ms = 0.001f * (float) sampleRate;
        float mean = 0.0f, shortest = 1.0e9f;
        for (int i = 0; i < 2; ++i)
        {
            const float len = std::clamp(std::round(setup.loopMs[i] * size * ms), 8.0f, tank[i].getMaxDelay() - MAX_MOD_MS * ms - 2.0f);
            length[i].set(len);
            mean += 0.5f * len;
            shortest = std::min(shortest, len);
        }
        gain.set(std::min(0.9995f, std::exp(-6.9078f * mean / ((float) sampleRate * decay))));
        const float travel = std::max(0.25f, std::abs(1.0f - speedWanted));
        unit.set(std::min(e6dly::read::GRAIN_UNIT * (float) sampleRate, 0.3f * shortest / travel));
    }

    void applyDamping()
    {
        damp[0].setCutoff(sampleRate, dampNow);
        damp[1].setCutoff(sampleRate, dampNow);
    }

    double sampleRate = 44100.0;
    Setup setup;
    e6dly::Line tank[2], pre[2];
    Allpass diffuser[MAX_DIFFUSERS], loopAllpass[2];
    float diffuserLen[MAX_DIFFUSERS] = { 2.0f, 2.0f, 2.0f }, loopAllpassLen[2] = { 4.0f, 4.0f };
    bool hasLoopAllpass = false;
    fxb::OnePole damp[2];
    Dispersion dispersion;
    e6dly::Heads heads;

    float decay = 2.0f, size = 1.0f, shimmerWanted = 0.0f, speedWanted = 2.0f;
    float dampTarget = 8000.0f, dampNow = 8000.0f;
    e6dly::Glide length[2], preDelay, gain, shimmer, dispersionGain, dispersionMix, modDepth, unit;
    float lengthShare = 1.0f, loopShare = 1.0f;
    e6dly::SlowSine sway;
    bool changed = true, moving = false, shimmering = false, dispersing = false, swaying = false, delayed = false;
};

// An effect that is a setting of the reverb core (cells from one table: CellEffect.h).
class Family : public CellEffect
{
protected:
    virtual Core::Setup setup() const = 0;

    void prepareCore() override { core.prepare(sampleRate, setup()); }
    void resetCore() override { core.reset(); }
    void controlCore() override { core.control(); }
    bool coasting() const override { return core.resting(); }
    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override { core.run(left, right, n, mixFrom, mixTo); }

    Core core;
};

} // namespace e6rvb
