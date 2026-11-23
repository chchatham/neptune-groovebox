#pragma once

// THE OLD CODE, FOR THE TESTS ONLY. The reverb loops as they were at 3f77ffb, before the one
// reverb network (rvb::Net, ReverbParts.h) took their place: rvb::Core (of which the spring and the
// shimmer reverb were settings) and rvb::Field (PERF's freeze), with the three effects made of
// them. Only the namespace (oldrvb), the class names (Old...) and the includes were changed. The
// suite "One Network" plays them beside the new settings and renders the pairs for the owner
// (.ralph/atlas/one-network/). It is not part of the product. Do not edit it: it is the reference.

#include "../../src/dsp/effects/DelayCore.h"
#include "../../src/dsp/effects/PerfEffects.h"      // (PerfGesture)

// The parts a reverb is made of, and one loop that puts them together: the spring reverb and the
// shimmer reverb are settings of rvb::Core, and so each has what the other has.
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
// - Shifted heads (dly::Heads): what comes round is read by two moving heads at another pitch, the
//   shimmer; in any loop, at any interval.
// The loop lines are dly::Line, the glides dly::Glide: a length that follows a control is glided to.
namespace oldrvb
{
// Schroeder allpass on a line read between samples.
struct Allpass
{
    void prepare(int maxSamples) { line.prepare(maxSamples); }
    void clear() { line.clear(); }

    DLY_INLINE float process(float x, float delay, float g)
    {
        dly::Line::Cursor c = line.open();
        const float d = c.readLinear(delay);
        const float v = x + g * d;
        c.write(fxb::flushDenormal(v));
        line.close(c);
        return d - g * v;
    }

    dly::Line line;
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

// ---- E6: the intervals the shifted heads can play: a view of the one list (dly::interval) ----------
// The first four are what a reverb's MODE always offered, in its order (stored: the list only
// grows at the end; `Sub` was always an octave down, the one list's OCT-). After them: REV (what
// comes round is read backwards, a stretch at a time), the one list's SUB (two octaves down) and
// HOLD (the loop keeps what it has: nothing comes in, nothing is lost).
namespace shift
{
enum Mode { Octave = 0, Fifth, OctaveFifth, Sub, Reverse, TwoDown, Hold, Count };
inline int idOf(int mode)
{
    static const int ids[Count] = { dly::interval::OctaveUp, dly::interval::FifthUp, dly::interval::OctaveFifth, dly::interval::OctaveDown,
                                    dly::interval::Reverse, dly::interval::Sub, dly::interval::Hold };
    return ids[std::clamp(mode, 0, (int) Count - 1)];
}
inline const char* const* names()
{
    static const char* n[Count] = {};
    static const bool made = [] { for (int i = 0; i < Count; ++i) n[i] = dly::interval::entry(idOf(i)).name; return true; }();
    (void) made;
    return n;
}
// The heads' speed; 0 = they stand: the loop is held (Core::setShimmer).
inline float speedOf(int mode) { return idOf(mode) == dly::interval::Hold ? 0.0f : dly::interval::entry(idOf(mode)).speed; }
}
// ---- E6 end ----

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
        hold.snap(holdWanted ? 1.0f : 0.0f);        // E6
        if (holdWanted) gain.snap(1.0f);
        holding = holdWanted;
        heads.set(speedWanted, xfadeFor(speedWanted));
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
    // (E6) A speed of 0 is no interval: the heads stand, and the loop is held (setHold).
    void setShimmer(float amount, float speed)
    {
        setHold(speed == 0.0f);
        if (speed == 0.0f) { shimmerWanted = 0.0f; changed = true; return; }
        shimmerWanted = std::clamp(amount, 0.0f, 1.0f); speedWanted = speed; changed = true;
    }
    // ---- E6: HOLD. The loops keep what they have: nothing comes in, a round trip loses nothing
    // (the gain is 1 and the damping is out of the way). Let go, the reverb is as it is set again.
    void setHold(bool on) { if (on != holdWanted) { holdWanted = on; changed = true; } }
    bool isHeld() const { return holdWanted; }
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
                if (shimmer.now == 0.0f) { heads.set(speedWanted, xfadeFor(speedWanted)); heads.reset(); }
            }
            else
                shimmer.set(shimmerWanted);
            dispersionMix.set(dispersionOn() ? 1.0f : 0.0f);
            if (dispersionMix.target > 0.0f && dispersionMix.now == 0.0f && ! dispersing)
                dispersion.clear();     // (coming in: nothing stale in it)
            for (auto* g : { &length[0], &length[1], &preDelay, &unit }) g->aim(lengthShare);
            for (auto* g : { &gain, &shimmer, &dispersionGain, &dispersionMix, &modDepth }) g->aim(loopShare);
            hold.set(holdWanted ? 1.0f : 0.0f);         // E6
            if (holdWanted) gain.set(1.0f);
            gain.aim(loopShare);
            hold.aim(loopShare);
            holding = hold.target > 0.0f || hold.now > 0.0f;
            moving = false;
            moving = moving || hold.moving();
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
    // (E6: held or on the way there, another loop; the loop that is not held is as it was.)
    void run(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        if (holding) runAs<true>(left, right, n, mixFrom, mixTo);
        else         runAs<false>(left, right, n, mixFrom, mixTo);
    }

private:
    template <bool Kept>
    void runAs(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        const bool cross = setup.cross, taps = setup.taps, diffuse = setup.diffusers > 0, loopAp = hasLoopAllpass;
        const bool shim = shimmering, disperse = dispersing, swayed = swaying, late = delayed, glide = moving;
        const float diffusion = setup.diffusion, loopDiffusion = setup.loopDiffusion, inGain = setup.inGain, outGain = setup.outGain;
        const float crossOut = setup.crossOut;
        dly::Line::Cursor a = tank[0].open(), b = tank[1].open(), pa = pre[0].open(), pb = pre[1].open();
        fxb::OnePole dampA = damp[0], dampB = damp[1];
        dly::Glide lenA = length[0], lenB = length[1], g = gain, sh = shimmer, da = dispersionGain, dm = dispersionMix,
                   md = modDepth, pd = preDelay, un = unit, hd = hold;
        constexpr bool kept = Kept;     // E6
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
            if (kept)
            {
                // (E6: held, the damping is taken out of the way by degrees.)
                ya += (1.0f - hd.now) * (dampA.lowPass(ya) - ya);
                yb += (1.0f - hd.now) * (dampB.lowPass(yb) - yb);
            }
            else
            {
                ya = dampA.lowPass(ya);
                yb = dampB.lowPass(yb);
            }
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
            const float fed = kept ? inGain * (1.0f - hd.now) : inGain;         // (E6: held, nothing comes in)
            a.write(fxb::flushDenormal(held(xa * fed + g.now * (cross ? yb : ya))));
            b.write(fxb::flushDenormal(held(xb * fed + g.now * (cross ? ya : yb))));

            const float wet = outGain * mix;
            left[i] = inL * (1.0f - mix) + outL * wet;
            right[i] = inR * (1.0f - mix) + outR * wet;
            mix += mixStep;

            if (glide)
            {
                lenA.now += lenA.step; lenB.now += lenB.step; g.now += g.step; sh.now += sh.step; da.now += da.step;
                dm.now += dm.step; md.now += md.step; pd.now += pd.step; un.now += un.step;
                if (kept) hd.now += hd.step;
            }
        }

        tank[0].close(a); tank[1].close(b); pre[0].close(pa); pre[1].close(pb);
        damp[0] = dampA; damp[1] = dampB;
        length[0].now = lenA.now; length[1].now = lenB.now; gain.now = g.now; shimmer.now = sh.now; dispersionGain.now = da.now;
        dispersionMix.now = dm.now; modDepth.now = md.now; preDelay.now = pd.now; unit.now = un.now; hold.now = hd.now;
    }

    static constexpr float HEADROOM = 8.0f;     // a long tail under a loud sound adds up to several times full scale
    static DLY_INLINE float held(float x) { return HEADROOM * dly::ceiling(x * (1.0f / HEADROOM)); }

    float shareFor(float ms) const { return dly::glideShare(ms, sampleRate); }
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
        // (E6: backwards, a stretch is as long as the shorter loop lets it be: the heads' whole
        // reach, 2 x 1.25 stretches, stays inside it.)
        if (speedWanted < 0.0f) unit.set(0.36f * shortest);
        else                    unit.set(std::min(dly::read::GRAIN_UNIT * (float) sampleRate, 0.3f * shortest / travel));
    }

    void applyDamping()
    {
        damp[0].setCutoff(sampleRate, dampNow);
        damp[1].setCutoff(sampleRate, dampNow);
    }

    double sampleRate = 44100.0;
    Setup setup;
    dly::Line tank[2], pre[2];
    Allpass diffuser[MAX_DIFFUSERS], loopAllpass[2];
    float diffuserLen[MAX_DIFFUSERS] = { 2.0f, 2.0f, 2.0f }, loopAllpassLen[2] = { 4.0f, 4.0f };
    bool hasLoopAllpass = false;
    fxb::OnePole damp[2];
    Dispersion dispersion;
    dly::Heads heads;

    float decay = 2.0f, size = 1.0f, shimmerWanted = 0.0f, speedWanted = 2.0f;
    float dampTarget = 8000.0f, dampNow = 8000.0f;
    dly::Glide length[2], preDelay, gain, shimmer, dispersionGain, dispersionMix, modDepth, unit;
    float lengthShare = 1.0f, loopShare = 1.0f;
    dly::SlowSine sway;
    bool changed = true, moving = false, shimmering = false, dispersing = false, swaying = false, delayed = false;

    // E6: HOLD, and the heads' crossfade (shifted: they overlap all the time; backwards: each plays its stretch)
    static float xfadeFor(float speed) { return speed < 0.0f ? 0.25f : 1.0f; }
    dly::Glide hold;
    bool holdWanted = false, holding = false;
};

// ---- E6: a field of lines ------------------------------------------------------------------------------
// The other loop a reverb can be: eight lines mixed through a Hadamard matrix, which loses nothing
// (a feedback delay network: Stautner & Puckette / Jot), fed through three allpass diffusers. It
// was PERF's freeze, on lines, heads and glides of its own; here it is made of the parts above, so
// what comes round can be read at any interval of the one list (dly::interval), backwards too.
//
//   in -> high-pass -> x feed -> diffusers -> [ + ] -> eight lines -> out (four lines a side)
//                                               ^            |
//                                               +- x gain <- Hadamard <- (1 - share) straight + share x low-pass( heads at an interval )
//
// With the feed at 0 and the gain at 1 it holds what it has for as long as it is left to.
class Field
{
public:
    static constexpr int LINES = 8, DIFFUSERS = 3;

    struct Setup
    {
        // Line lengths in milliseconds: no two share a factor once in samples (near enough).
        float lineMs[LINES] = { 53.3f, 61.7f, 68.9f, 77.3f, 85.1f, 93.7f, 103.1f, 112.9f };
        float diffuserMs[DIFFUSERS] = { 4.7f, 8.3f, 12.1f };
        float diffusion = 0.6f;
        float inHpHz = 25.0f;
    };

    void prepare(double sr, const Setup& s)
    {
        sampleRate = sr;
        setup = s;
        shortest = 1.0e9f;
        mean = 0.0f;
        for (int i = 0; i < LINES; ++i)
        {
            // (Whole samples: a read between two samples would dull a held sound a little every time round.)
            length[i] = std::max(16.0f, std::floor((float) ((double) setup.lineMs[i] * 0.001 * sr)));
            line[i].prepare((int) length[i] + 8);
            shortest = std::min(shortest, length[i]);
            mean += length[i] / (float) LINES;
        }
        for (int i = 0; i < DIFFUSERS; ++i)
        {
            diffuserLen[i] = std::max(2.0f, (float) ((double) setup.diffuserMs[i] * 0.001 * sr));
            diffuser[i].prepare((int) diffuserLen[i] + 8);
        }
        inHp.setCutoff(sr, setup.inHpHz);
        feedShare = dly::glideShare(15.0f, sr);
        loopShare = dly::glideShare(30.0f, sr);
        reset();
    }

    // Everything where it is set, the lines empty.
    void reset()
    {
        for (auto& l : line) l.clear();
        for (auto& d : diffuser) d.clear();
        for (auto& d : damp) d.reset();
        inHp.reset();
        aim();
        share.set(shareWanted);
        for (auto* g : { &feed, &gain, &share, &unit }) g->snap();
        heads.set(speedWanted, xfadeFor(speedWanted));
        heads.reset();
        toneNow = toneWanted;
        applyTone();
    }

    // ---- what the effect asks for (glided to) --------------------------------------------------
    void setFeed(float open) { feed.set(std::clamp(open, 0.0f, 1.0f)); }            // how much comes in
    void setGain(float loopGain) { gain.set(std::clamp(loopGain, 0.0f, 1.0f)); }    // 1 = nothing is lost
    // What comes round is read at `speed` times its pitch (a head's speed: dly::interval), this much of it.
    void setShift(float amount, float speed) { shareWanted = std::clamp(amount, 0.0f, 1.0f); if (speed != 0.0f) speedWanted = speed; }
    // The low-pass the shifted share passes: what climbs is taken out at the top.
    void setTone(float hz) { toneWanted = std::clamp(hz, 200.0f, 20000.0f); }
    // The gain that takes a round trip down by its share of 60 dB in `seconds`.
    float gainFor(float seconds) const { return std::exp(-6.908f * (mean / (float) sampleRate) / std::max(0.01f, seconds)); }

    void control()
    {
        aim();
        // Another interval: the shifted share is taken out, the heads exchanged, and it comes back.
        if (speedWanted != heads.speed)
        {
            share.set(0.0f);
            if (share.now == 0.0f) { heads.set(speedWanted, xfadeFor(speedWanted)); heads.reset(); unit.snap(); }
        }
        else
            share.set(shareWanted);
        feed.aim(feedShare);
        gain.aim(loopShare);
        share.aim(loopShare);
        unit.aim(loopShare);
        if (toneNow != toneWanted)
        {
            toneNow = std::abs(toneWanted - toneNow) < 0.5f ? toneWanted : toneNow + loopShare * (toneWanted - toneNow);
            applyTone();
        }
    }

    // `n` samples in place, no more than are left until the next control(): in (1 - mix) + wet mix.
    void run(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        dly::Line::Cursor c[LINES];
        for (int i = 0; i < LINES; ++i) c[i] = line[i].open();
        dly::Glide fd = feed, g = gain, sh = share, un = unit;
        const bool climbing = sh.now > 1.0e-4f || sh.target > 1.0e-4f;
        const float diffusion = setup.diffusion;
        const float mixStep = (mixTo - mixFrom) / (float) std::max(1, n);
        float mix = mixFrom;
        static constexpr float NORM = 0.35355339f;      // 1 / sqrt(8)

        for (int s = 0; s < n; ++s)
        {
            const float inL = left[s], inR = right[s];
            float in = inHp.highPass((inL + inR) * 0.5f) * fd.now;
            for (int i = 0; i < DIFFUSERS; ++i)
                in = diffuser[i].process(in, diffuserLen[i], diffusion);

            heads.next(un.now);         // (always: where the heads stand does not depend on when they were last used)
            float out[LINES];
            for (int i = 0; i < LINES; ++i)
            {
                float v = c[i].readLinear(length[i]);
                if (climbing)
                {
                    const float up = heads.read(c[i], std::max(2.0f, length[i] - heads.span));
                    v += sh.now * (damp[i].lowPass(up) - v);
                }
                out[i] = v;
            }

            // Hadamard, 8 x 8, scaled to keep the energy (three butterfly passes).
            float m[LINES];
            for (int i = 0; i < LINES; ++i) m[i] = out[i];
            for (int span = 1; span < LINES; span <<= 1)
                for (int i = 0; i < LINES; i += span << 1)
                    for (int j = i; j < i + span; ++j)
                    {
                        const float a = m[j], b = m[j + span];
                        m[j] = a + b;
                        m[j + span] = a - b;
                    }
            for (int i = 0; i < LINES; ++i)
                c[i].write(fxb::flushDenormal(dly::ceiling(in * NORM + m[i] * NORM * g.now)));

            const float wetL = (out[0] - out[2] + out[4] - out[6]) * 0.5f;
            const float wetR = (out[1] - out[3] + out[5] - out[7]) * 0.5f;
            left[s] = inL * (1.0f - mix) + wetL * mix;
            right[s] = inR * (1.0f - mix) + wetR * mix;
            mix += mixStep;
            fd.now += fd.step; g.now += g.step; sh.now += sh.step; un.now += un.step;
        }

        for (int i = 0; i < LINES; ++i) line[i].close(c[i]);
        feed.now = fd.now; gain.now = g.now; share.now = sh.now; unit.now = un.now;
    }

private:
    static float xfadeFor(float speed) { return speed < 0.0f ? 0.25f : 1.0f; }

    // A grain for the heads that keeps their whole reach inside the shortest line.
    void aim()
    {
        const float travel = std::max(0.25f, std::abs(1.0f - speedWanted));
        if (speedWanted < 0.0f) unit.set(0.36f * shortest);
        else                    unit.set(std::min(dly::read::GRAIN_UNIT * (float) sampleRate, 0.45f * shortest / travel));
    }

    void applyTone() { for (auto& d : damp) d.setCutoff(sampleRate, toneNow); }

    double sampleRate = 44100.0;
    Setup setup;
    dly::Line line[LINES];
    float length[LINES] = {}, shortest = 16.0f, mean = 3600.0f;
    Allpass diffuser[DIFFUSERS];
    float diffuserLen[DIFFUSERS] = { 2.0f, 2.0f, 2.0f };
    fxb::OnePole damp[LINES], inHp;
    dly::Heads heads;
    dly::Glide feed, share, unit;
    dly::Glide gain { 1.0f, 1.0f, 0.0f };
    float shareWanted = 0.0f, speedWanted = 2.0f, toneWanted = 5000.0f, toneNow = 5000.0f;
    float feedShare = 1.0f, loopShare = 1.0f;
};
// ---- E6 end ----

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
} // namespace oldrvb


// Spring reverb: a setting of oldrvb::Core (ReverbParts.h). Two springs of different lengths, one fed
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
// MOD   a slow sine moves the springs' lengths: the tail wavers
class OldSpringReverb : public oldrvb::Family
{
public:
    enum Param { Decay = 0, Dispersion, Tension, Damping };
    enum Extra { PreDelay = 0, Shimmer, ShiftMode, Modulation };

    OldSpringReverb() { seed(); }

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
            ParamSpec { "PDLY", 0, 0, oldrvb::Core::MAX_PRE_DELAY, 0, K::Linear, "s" }.timed(),      // (E6: it can follow the tempo)
            { "SHIM", 1, 0, 1, 0 },
            { "MODE", 2, 0, (float) (oldrvb::shift::Count - 1), 0, K::Choice, "", oldrvb::shift::names(), oldrvb::shift::Count },
            { "MOD", 3, 0, 1, 0 } };
        return c;
    }

    oldrvb::Core::Setup setup() const override
    {
        oldrvb::Core::Setup s;
        s.loopMs[0] = 41.0f; s.loopMs[1] = 53.0f;
        s.maxSize = 1.6f;
        s.dispersionAlways = true;
        s.crossOut = 0.2f;
        s.brightHz = 7000.0f; s.darkHz = 900.0f;        // (a spring carries little above a few kHz)
        s.outGain = 1.4f;
        return s;
    }

    void configure(const float* v) override
    {
        core.setDecay(v[Decay]);
        core.setDispersion(v[Dispersion]);
        core.setSize(sizeFor(v[Tension]));
        core.setDamping(v[Damping]);
        core.setPreDelay(v[4 + PreDelay]);
        core.setShimmer(v[4 + Shimmer], oldrvb::shift::speedOf((int) std::lround(v[4 + ShiftMode])));
        core.setModulation(v[4 + Modulation]);
    }
};


// Shimmer reverb: a setting of oldrvb::Core (ReverbParts.h). Three input diffusers, then a tank of two
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
// MOD   a slow sine moves the allpass inside each loop: the tail is chorused instead of ringing
class OldShimmerReverb : public oldrvb::Family
{
public:
    enum Param { Decay = 0, Shimmer, ShiftMode, Damping };
    enum Extra { Size = 0, PreDelay, Dispersion, Modulation };

    OldShimmerReverb() { seed(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "DCAY", 0, 0.3f, 10, 3, K::Log, "s" },
            { "SHIM", 1, 0, 1, 0.4f },
            { "MODE", 2, 0, (float) (oldrvb::shift::Count - 1), 0, K::Choice, "", oldrvb::shift::names(), oldrvb::shift::Count },
            { "DAMP", 3, 0, 1, 0.4f },
            { "SIZE", 0, 0.25f, 2, 1, K::Log, "x" },
            ParamSpec { "PDLY", 1, 0, oldrvb::Core::MAX_PRE_DELAY, 0, K::Linear, "s" }.timed(),      // (E6: it can follow the tempo)
            { "DISP", 2, 0, 1, 0 },
            { "MOD", 3, 0, 1, 0.25f } };
        return c;
    }

    oldrvb::Core::Setup setup() const override
    {
        oldrvb::Core::Setup s;
        s.loopMs[0] = 161.3f; s.loopMs[1] = 142.6f;
        s.maxSize = 2.0f;
        s.cross = true;
        s.diffusers = 3;
        s.loopAllpassMs[0] = 22.6f; s.loopAllpassMs[1] = 30.5f;
        s.taps = true;
        s.outGain = 2.5f;
        return s;
    }

    void configure(const float* v) override
    {
        core.setDecay(v[Decay]);
        core.setShimmer(v[Shimmer], oldrvb::shift::speedOf((int) std::lround(v[ShiftMode])));
        core.setDamping(v[Damping]);
        core.setSize(v[4 + Size]);
        core.setPreDelay(v[4 + PreDelay]);
        core.setDispersion(v[4 + Dispersion]);
        core.setModulation(v[4 + Modulation]);
    }
};

// FRZ as it was (PerfEffects.h)
class OldFreezeReverb : public CellEffect, public PerfGesture
{
public:
    enum Param { Grab = 0, Shimmer, Spiral, Decay };
    enum Extra { Mode = 0, Damp };
    static constexpr int LINES = oldrvb::Field::LINES;

    OldFreezeReverb() { mix = 1.0f; seed(); }

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
            { "MODE", Mode, 0, (float) (oldrvb::shift::Hold - 1), 0, K::Choice, "", oldrvb::shift::names(), oldrvb::shift::Hold },
            { "DAMP", Damp, 0, 1, 0 },
            {}, {} };
        return c;
    }

    void prepareCore() override { field.prepare(sampleRate, oldrvb::Field::Setup()); }
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
        releaseGain = field.gainFor(v[Decay]);
        speed = oldrvb::shift::speedOf((int) v[4 + Mode]);
        field.setTone(5000.0f * std::pow(0.1f, v[4 + Damp]));
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
        field.setGain(held ? 1.0f : releaseGain);
        field.setShift(held ? shimmer : spiral, speed);
    }

    oldrvb::Field field;
    float grabSec = 0.25f, shimmer = 0.0f, spiral = 0.5f, releaseGain = 0.8f, speed = 2.0f;
    bool held = false;
    int grabLeft = 0;
};

