#pragma once

#include "DelayCore.h"
#include "FxLfo.h"

// The reverbs: ONE network of lines (rvb::Net) of which the plate, the hall, the spring, the
// shimmer tank and PERF's frozen field are settings, so each has what the others have.
//
//   in -> the way in (rvb::Front: low-pass, pre-delay, input allpasses; or two sides kept apart)
//      -> x feed -> [ + ] -> dispersion -> loop allpass (swayed) -> damping -> line k -+-> taps -> out
//                    ^                                                                |
//                    +-- x gain k <-- the mixing matrix <-- shifted heads <-- line ends +
//
// - N lines (1..8), each with its length; the mixing matrix one of: each line feeds itself (two
//   springs), the lines of a pair feed each other (the plate's figure of eight), Householder (every
//   line feeds the others and takes itself away: the hall), Hadamard (the dense field). Every one
//   of them loses nothing, so the gain alone says how long the tail is: each line's gain takes a
//   round trip down by its share of 60 dB in DCAY (Jot).
// - The allpass inside each loop (Schroeder: v = x + g d, y = d - g v) smears what goes round; its
//   coefficient is the diffusion.
// - Sway: the shared effect LFO (fxm::Lfo, FxLfo.h) moves each line's read, or its allpass, by a
//   fraction of a millisecond: the tail is chorused instead of ringing.
// - Dispersion: a cascade of stretched first-order allpasses, in which the low frequencies of a
//   spring's band travel faster than the high ones: every round trip a click becomes more of a chirp
//   (the structure of Parker, "Efficient dispersion generation structures for spring reverb
//   emulation", EURASIP 2011, and Valimaki, Parker & Abel, "Parametric spring reverberation effect",
//   JAES 2010: the allpass (a + z^-k) / (1 + a z^-k), k = the stretch that puts the chirp below ~4.3 kHz).
// - Damping, one law for every reverb: x + depth (low-pass(x) - x), a one-pole low-pass whose corner
//   DAMP moves between the setting's bright and dark corners, by ratio; depth 1 is a low-pass, less
//   is a shelf (the hall's), 0 none (the field's). HOLD takes the depth to 0.
// - Shifted heads (dly::Heads): what comes round is read by two moving heads at another pitch, the
//   shimmer; in any loop, at any interval of the one list (shift::), through a low-pass if asked.
// - Taps: what is heard is read from a table of places (a line's end, or a place inside a line or
//   an allpass), each with its gain and side.
// - HOLD (rvb::Front): the way in shuts, the gains go to 1 and the damping out of the way: what is
//   in the reverb stays until it is let go.
// The lines are dly::Line, the glides dly::Glide; every length is a whole number of samples at
// rest and is glided to (read between samples on the way) when SIZE or the pre-delay is turned.
//
// The send reverb (RVB) and the hall (HALL) are settings of the network too (setting::plate, ::hall),
// but their sound is held to the bit by the suite "P01 Keyboard", and the network does not give
// their samples (one damping law, the shared LFO, a gain per line). So until the owner has heard
// them side by side, the two effects still play their old loops (rvb::Tank, rvb::Network, in the
// marked PINNED block at the end of this file), which share the way in (rvb::Front) with the
// network; `network::forPinned` puts them on the network instead (the recordings for the owner).
namespace rvb
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

// Lengths that follow one control together: each has a whole number of samples to be at; on the
// way there all of them move as one (pos 0 -> 1) and are read between samples.
template <int N>
struct Lengths
{
    std::array<int, N> whole {};
    std::array<float, N> from {}, to {};
    dly::Glide pos { 1.0f, 1.0f, 0.0f };

    bool resting() const { return pos.now == 1.0f; }
    DLY_INLINE float at(int k, float p) const { return from[(size_t) k] + p * (to[(size_t) k] - from[(size_t) k]); }

    void set(const std::array<int, N>& w)
    {
        if (w == whole) return;
        for (int k = 0; k < N; ++k)
        {
            from[(size_t) k] = at(k, pos.now);
            to[(size_t) k] = (float) w[(size_t) k];
        }
        whole = w;
        pos.now = 0.0f; pos.target = 1.0f; pos.step = 0.0f;
    }

    void snap()
    {
        for (int k = 0; k < N; ++k) from[(size_t) k] = to[(size_t) k] = (float) whole[(size_t) k];
        pos.snap(1.0f);
    }

    void aim(float share)
    {
        pos.aim(share);
        if (pos.now == 1.0f) from = to;
    }
};

// ---- the way in, one for every reverb ------------------------------------------------------------
// The low-pass and the pre-delay, the input allpasses, the shimmer heads and their interval, HOLD,
// the size, the table of lengths and what is glided. The network and the two pinned loops are
// made on it; wayIn() is the one way in, operation for operation what the send reverb and the hall
// always did (so the pinned loops keep their samples), and wayInPair() keeps two sides apart (the
// springs: each side feeds its own).
class Front
{
public:
    static constexpr double REFERENCE_RATE = 29761.0;       // the rate the pinned lengths are written for
    static constexpr float MIN_SIZE = 0.25f, MAX_SIZE = 2.0f, MAX_PRE_MS = 250.0f;
    static constexpr float HEADROOM = 8.0f;
    static constexpr int MAX_DIFFUSERS = 4, MAX_LENGTHS = 40;
    using Table = Lengths<MAX_LENGTHS>;

    void setSize(float s) { size = std::clamp(s, MIN_SIZE, maxSize); changed = true; }
    void setPreDelay(float ms) { preMs = std::clamp(ms, 0.0f, MAX_PRE_MS); changed = true; }
    void setHighCut(float hz) { highCut = std::clamp(hz, 1000.0f, 20000.0f); highCutSet = true; changed = true; }
    // What comes round is read at `speed` times its pitch, this much of it (0 = none, and it costs
    // nothing). A speed of 0 (the list's HOLD) is no interval: the reverb is held.
    void setShimmer(float amount, float speed)
    {
        heldByMode = speed == 0.0f;
        shimmerWanted = heldByMode ? 0.0f : std::clamp(amount, 0.0f, 1.0f);
        if (! heldByMode) speedWanted = speed;
        hold.set(isHeld() ? 1.0f : 0.0f);
        changed = true;
    }
    // The way in shuts and the loop loses nothing: what is in it goes round until it is let go.
    void setHold(bool on) { heldByCell = on; hold.set(isHeld() ? 1.0f : 0.0f); changed = true; }
    bool isHeld() const { return heldByCell || heldByMode; }

    // Nothing is on its way: control() would do nothing, and run() may take any number of samples.
    bool resting() const { return ! (changed || moving); }

protected:
    // Straight up to HEADROOM, never beyond twice that (and exactly x below it).
    static DLY_INLINE float held(float x) { return HEADROOM * dly::ceiling(x * (1.0f / HEADROOM)); }
    static float xfadeFor(float speed) { return speed < 0.0f ? 0.25f : 1.0f; }

    // `count` input allpasses with their coefficients, each as long as `room` samples at most;
    // `stereo`: the two sides are delayed apart; `lowPassed`: the low-pass on the way in is there.
    void prepareFront(double sr, int count, const float* coeffs, const int* room, bool stereo, bool lowPassed)
    {
        sampleRate = sr;
        scale = sr / REFERENCE_RATE;
        const int preRoom = (int) (MAX_PRE_MS * 0.001 * sr) + 16;
        pre[0].prepare(preRoom);
        pre[1].prepare(stereo ? preRoom : 16);
        diffusers = std::clamp(count, 0, MAX_DIFFUSERS);
        for (int k = 0; k < MAX_DIFFUSERS; ++k)
        {
            diffuser[k].prepare(k < diffusers ? std::max(4, room[k]) + 16 : 16);
            coeff[k] = k < diffusers ? coeffs[k] : 0.0f;
        }
        passLow = lowPassed;
        lengthShare = dly::glideShare(80.0f, sr);
        loopShare = dly::glideShare(15.0f, sr);
    }

    void resetFront()
    {
        for (auto& l : pre) l.clear();
        for (auto& l : diffuser) l.clear();
        preLp.reset();
        fresh = changed = true;
        moving = false;
    }

    int scaled(int base, int most) const { return std::clamp((int) (base * scale * size), 1, most); }

    // Every look while something is on its way; `shortest` = the shortest loop, for the heads' grain.
    void aimFront(float shortest)
    {
        preLp.a = passLow ? 1.0f - std::exp(-dsp::TWO_PI_F * highCut / (float) sampleRate) : 1.0f;
        preWhole = std::clamp((int) (preMs * 0.001f * (float) sampleRate), 0, (int) pre[0].getMaxDelay() - 2);
        preDelay.set((float) preWhole);
        // (Backwards, a stretch is as long as the shorter loop lets it be: the heads' whole reach,
        // 2 x 1.25 stretches, stays inside it.)
        const float travel = std::max(0.25f, std::abs(1.0f - speedWanted));
        unit.set(speedWanted < 0.0f ? 0.36f * shortest : std::min(dly::read::GRAIN_UNIT * (float) sampleRate, 0.3f * shortest / travel));
        if (fresh)
        {
            // (Nothing has sounded since reset(): everything is where it is set.)
            heads.set(speedWanted, xfadeFor(speedWanted));
            heads.reset();
            shimmer.set(shimmerWanted);
            for (auto* g : { &preDelay, &unit, &shimmer, &hold }) g->snap();
            return;
        }
        // Another interval: the shimmer is taken out, the heads exchanged, and it comes back.
        if (speedWanted != heads.speed)
        {
            shimmer.set(0.0f);
            if (shimmer.now == 0.0f) { heads.set(speedWanted, xfadeFor(speedWanted)); heads.reset(); }
        }
        else
            shimmer.set(shimmerWanted);
        preDelay.aim(lengthShare);
        unit.aim(lengthShare);
        shimmer.aim(loopShare);
        hold.aim(loopShare);
    }

    bool frontMoving() const
    {
        return preDelay.moving() || unit.moving() || shimmer.moving() || hold.moving() || speedWanted != heads.speed;
    }

    void flags()
    {
        shimmering = shimmer.target > 0.0f || shimmer.now > 0.0f;
        holding = hold.target > 0.0f || hold.now > 0.0f;
    }

    // The way in for one run of samples (cursors and states a loop keeps to itself).
    struct Way
    {
        dly::Line::Cursor p, q, d[MAX_DIFFUSERS];
        fxb::OnePole low;
        int preAt = 1;
        float coeff[MAX_DIFFUSERS] = {};     // (kept with the run: a write to a line is not a write to them)
    };

    Way openWay()
    {
        Way w;
        w.p = pre[0].open();
        w.q = pre[1].open();
        for (int k = 0; k < MAX_DIFFUSERS; ++k) w.d[k] = diffuser[k].open();
        w.low = preLp;
        w.preAt = preWhole + 1;
        for (int k = 0; k < MAX_DIFFUSERS; ++k) w.coeff[k] = coeff[k];
        return w;
    }

    void closeWay(const Way& w)
    {
        pre[0].close(w.p);
        pre[1].close(w.q);
        for (int k = 0; k < MAX_DIFFUSERS; ++k) diffuser[k].close(w.d[k]);
        preLp = w.low;
    }

    // Both sides as one: the low-pass, the pre-delay (`pre` samples while it glides), then the
    // input allpasses (D of them, or as many as prepared for D < 0), whose lengths are the table's
    // first entries. Nothing is flushed here (see the note at the top of DelayCore.h).
    template <bool Whole, int D>
    DLY_INLINE float wayIn(Way& w, float inL, float inR, float pos, float pre) const
    {
        float mono = (inL + inR) * 0.5f;
        w.low.z += w.low.a * (mono - w.low.z);
        mono = w.low.z;
        w.p.write(mono);
        mono = Whole ? w.p.readAt(w.preAt) : w.p.readLinear(pre + 1.0f);
        const int count = D >= 0 ? D : diffusers;
        for (int k = 0; k < count; ++k)
        {
            dly::Line::Cursor& c = w.d[k];
            const float d = Whole ? c.readAt(lengths.whole[(size_t) k]) : c.readLinear(lengths.at(k, pos));
            const float v = mono + d * w.coeff[k];
            const float y = d - v * w.coeff[k];
            c.write(v);
            mono = y;
        }
        return mono;
    }

    // The two sides apart (no low-pass, no allpasses): the pre-delay alone.
    template <bool Whole>
    DLY_INLINE void wayInPair(Way& w, float& left, float& right, float pre) const
    {
        w.p.write(left);
        w.q.write(right);
        left = Whole ? w.p.readAt(w.preAt) : w.p.readLinear(pre + 1.0f);
        right = Whole ? w.q.readAt(w.preAt) : w.q.readLinear(pre + 1.0f);
    }

    double sampleRate = 44100.0, scale = 1.0;
    float size = 1.0f, maxSize = MAX_SIZE, preMs = 10.0f, highCut = 10000.0f, shimmerWanted = 0.0f, speedWanted = 2.0f;
    dly::Line pre[2], diffuser[MAX_DIFFUSERS];
    float coeff[MAX_DIFFUSERS] = {};
    int diffusers = 0;
    fxb::OnePole preLp;
    int preWhole = 0;
    Table lengths;
    dly::Glide preDelay, shimmer, hold, unit { 720.0f, 720.0f, 0.0f };
    dly::Heads heads;
    float lengthShare = 1.0f, loopShare = 1.0f;
    bool fresh = true, changed = true, moving = false, shimmering = false, holding = false;
    bool heldByCell = false, heldByMode = false, passLow = true, highCutSet = false;
};

// ---- the network ------------------------------------------------------------------------------------
class Net : public Front
{
public:
    static constexpr int MAX_LINES = 8, MAX_TAPS = 8;
    static constexpr int ALLPASS = 8;       // a tap's `from`: line k, or ALLPASS + k (inside line k's allpass)
    static constexpr float MAX_DECAY = 1.0e6f;      // seconds; beyond it (or infinite) a round trip loses nothing

    enum class Matrix { Self, Cross, Householder, Hadamard };
    enum class Sway { None, Lines, Allpasses };

    // A place the reverb is heard from: `ms` back in a line or an allpass (scaled with the size),
    // or 0 = the line's end as it comes round. `gain` with its sign.
    struct Tap { int from = 0; float ms = 0.0f; float gain = 0.0f; };

    struct Setup
    {
        int lines = 2;
        float lineMs[MAX_LINES] = { 41.0f, 53.0f };         // at size 1
        Matrix matrix = Matrix::Self;
        float allpassMs[MAX_LINES] = {};                    // the allpass inside each loop; 0 = none
        float diffusion = 0.5f;                             // its coefficient (setDiffusion moves it)
        Sway sway = Sway::None;                             // what MOD moves
        float swayMs = 0.5f;                                // how far, at MOD 1 (half the swing)
        bool swayBothWays = false;                          // round the place it is set, or only further back from it
        float swayHz[MAX_LINES] = { 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f };
        float swayPhase[MAX_LINES] = {};                    // cycles
        bool dispersionAlways = false;                      // the dispersion is part of the sound, or comes in when asked for
        int diffusers = 0;                                  // input allpasses (with any, the two sides are one)
        float diffuserMs[MAX_DIFFUSERS] = {};
        float diffuserG[MAX_DIFFUSERS] = {};
        bool stereoIn = false;                              // no allpasses: the left side feeds the even lines, the right the odd
        float inGain = 1.0f;
        float highCutHz = 0.0f;                             // the low-pass on the way in; 0 = none
        float lowCutHz = 0.0f;                              // a high-pass on the way in; 0 = none
        float brightHz = 16000.0f, darkHz = 1200.0f;        // the damping's corner at DAMP 0 and at 1
        float dampDepth = 1.0f;                             // 1 = a low-pass, less = a shelf, 0 = none
        float toneHz = 0.0f;                                // the low-pass the shifted heads pass; 0 = none
        int tapsLeft = 2, tapsRight = 2;
        Tap left[MAX_TAPS] = { { 0, 0.0f, 0.8f }, { 1, 0.0f, 0.2f } };
        Tap right[MAX_TAPS] = { { 1, 0.0f, 0.8f }, { 0, 0.0f, 0.2f } };
        float outGain = 1.0f;
        float maxSize = 2.0f;                               // the largest size it is asked for
        bool sizeMovesAll = true;                           // SIZE moves the allpasses (in and in the loops) too, or the lines alone
        bool filtersOnRead = false;                         // the loop's filters on what comes round (it is heard through them) or on what goes in
        float headroom = Front::HEADROOM;                   // a line is straight up to this, bent above it, never beyond twice it
    };

    void prepare(double sr, const Setup& s)
    {
        sr = CellEffect::goodRate(sr);
        setup = s;
        int& n = setup.lines;
        n = std::clamp(n, 1, MAX_LINES);
        if (setup.matrix == Matrix::Hadamard && (n & (n - 1)) != 0) setup.matrix = Matrix::Householder;
        if (setup.matrix == Matrix::Cross && (n & 1) != 0)          setup.matrix = Matrix::Householder;
        setup.stereoIn = setup.stereoIn && setup.diffusers == 0;
        setup.tapsLeft = std::clamp(setup.tapsLeft, 0, MAX_TAPS);
        setup.tapsRight = std::clamp(setup.tapsRight, 0, MAX_TAPS);
        setup.maxSize = std::clamp(setup.maxSize, MIN_SIZE, MAX_SIZE);
        setup.headroom = std::clamp(setup.headroom, 0.25f, HEADROOM);
        for (Tap* t : { setup.left, setup.right })
            for (int i = 0; i < MAX_TAPS; ++i)
            {
                const int k = t[i].from >= ALLPASS ? t[i].from - ALLPASS : t[i].from;
                if (t[i].from < 0 || k >= n || ! std::isfinite(t[i].ms) || ! std::isfinite(t[i].gain))
                    t[i] = Tap {};      // (a place in no line is not heard)
            }
        maxSize = setup.maxSize;
        size = std::clamp(size, MIN_SIZE, maxSize);

        const double ms = 0.001 * sr * (double) setup.maxSize;
        int room[MAX_DIFFUSERS];
        for (int k = 0; k < MAX_DIFFUSERS; ++k) room[k] = (int) (setup.diffuserMs[k] * ms) + 4;
        prepareFront(sr, setup.diffusers, setup.diffuserG, room, setup.stereoIn, setup.highCutHz > 0.0f);
        if (setup.highCutHz > 0.0f && ! highCutSet) highCut = std::clamp(setup.highCutHz, 1000.0f, 20000.0f);
        if (! diffusionSet) diffusion.set(setup.diffusion);

        swayRoom = (int) std::ceil(2.0f * std::max(0.0f, setup.swayMs) * 0.001 * sr) + 2;
        allpassed = false;
        for (int k = 0; k < n; ++k)
        {
            float lineReach = setup.lineMs[k], apReach = setup.allpassMs[k];
            for (const Tap* t : taps())
            {
                if (t->from == k)           lineReach = std::max(lineReach, t->ms);
                if (t->from == ALLPASS + k) apReach = std::max(apReach, t->ms);
            }
            line[k].prepare((int) (lineReach * ms) + swayRoom + 16);
            allpass[k].prepare(apReach > 0.0f ? (int) (apReach * ms) + swayRoom + 16 : 16);
            allpassed = allpassed || setup.allpassMs[k] > 0.0f;
            lfo[k].prepare(sr);
            lfo[k].setShape((int) LFO::Shape::Sine);
            lfo[k].setRate(setup.swayHz[k]);
            lfo[k].setPhase(setup.swayPhase[k]);
        }
        for (auto& d : dispersion) d.prepare(sr);
        for (auto& h : lowCut) h.setCutoff(sr, std::max(1.0f, setup.lowCutHz));
        damped = setup.dampDepth > 0.0f;
        setDamping(dampAmount);
        reset();
    }

    // Everything where it is set, the lines empty.
    void reset()
    {
        for (int k = 0; k < setup.lines; ++k)
        {
            line[k].clear();
            allpass[k].clear();
            damp[k].reset();
            tone[k].reset();
            lfo[k].reset();
        }
        for (auto& d : dispersion) d.clear();
        for (auto& h : lowCut) h.reset();
        resetFront();
        dampNow = dampTarget;
        toneNow = toneWanted;
        applyDamping();
        applyTone();
        control();
    }

    // ---- what the effect asks for (glided to) --------------------------------------------------
    // The time a round trip takes to fall by 60 dB; infinite (or above MAX_DECAY) = nothing is lost.
    void setDecay(float seconds) { decay = std::isnan(seconds) ? 2.0f : std::max(0.01f, seconds); changed = true; }
    void setDamping(float amount)
    {
        dampAmount = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
        dampTarget = setup.brightHz * std::pow(setup.darkHz / setup.brightHz, dampAmount);
    }
    void setDiffusion(float g) { diffusion.set(std::isfinite(g) ? std::clamp(g, -0.99f, 0.99f) : 0.0f); diffusionSet = true; changed = true; }
    void setDispersion(float amount) { dispersionGain.set(0.8f * std::clamp(amount, 0.0f, 1.0f)); changed = true; }
    void setModulation(float depth) { swayAmount = std::isfinite(depth) ? std::clamp(depth, 0.0f, 1.0f) : 0.0f; changed = true; }
    // How much comes in (1 = all; HOLD shuts it as well).
    void setFeed(float open) { feed.set(std::clamp(open, 0.0f, 1.0f)); changed = true; }
    // The low-pass the shifted heads pass (with a setup whose toneHz is not 0).
    void setTone(float hz) { toneWanted = std::clamp(hz, 200.0f, 20000.0f); }

    // A round trip at the present size, on the mean of the lines, in seconds (what the decay law counts).
    float passSeconds() const
    {
        float sum = 0.0f;
        for (int k = 0; k < setup.lines; ++k) sum += setup.lineMs[k];
        return 0.001f * size * sum / (float) setup.lines;
    }

    bool resting() const { return Front::resting() && dampNow == dampTarget && toneNow == toneWanted; }

    void control()
    {
        if (dampNow != dampTarget)
        {
            dampNow = std::abs(dampTarget - dampNow) < 0.5f ? dampTarget : dampNow + loopShare * (dampTarget - dampNow);
            applyDamping();
        }
        if (toneNow != toneWanted)
        {
            toneNow = std::abs(toneWanted - toneNow) < 0.5f ? toneWanted : toneNow + loopShare * (toneWanted - toneNow);
            applyTone();
        }
        if (! (changed || moving)) return;
        changed = false;

        const int n = setup.lines;
        const float perMs = size * 0.001f * (float) sampleRate;
        const float fixedMs = setup.sizeMovesAll ? perMs : 0.001f * (float) sampleRate;
        auto samples = [perMs](float ms) { return (int) std::lround((double) ms * (double) perMs); };
        auto fixed = [fixedMs](float ms) { return (int) std::lround((double) ms * (double) fixedMs); };
        std::array<int, MAX_LENGTHS> w {};
        for (int k = 0; k < diffusers; ++k)
            w[(size_t) k] = std::clamp(fixed(setup.diffuserMs[k]), 2, (int) diffuser[k].getMaxDelay() - 2);
        float shortest = 1.0e9f;
        for (int k = 0; k < n; ++k)
        {
            const int most = (int) line[k].getMaxDelay() - swayRoom - 2;
            w[(size_t) (LINE0 + k)] = std::clamp(samples(setup.lineMs[k]), 8, most);
            shortest = std::min(shortest, (float) w[(size_t) (LINE0 + k)]);
            if (setup.allpassMs[k] > 0.0f)
                w[(size_t) (AP0 + k)] = std::clamp(fixed(setup.allpassMs[k]), 4, (int) allpass[k].getMaxDelay() - swayRoom - 2);
        }
        for (int side = 0; side < 2; ++side)
            for (int t = 0; t < (side == 0 ? setup.tapsLeft : setup.tapsRight); ++t)
            {
                const Tap& tap = (side == 0 ? setup.left : setup.right)[t];
                if (tap.ms <= 0.0f) continue;
                const dly::Line& l = tap.from >= ALLPASS ? allpass[tap.from - ALLPASS] : line[tap.from];
                w[(size_t) ((side == 0 ? TAPL : TAPR) + t)] = std::clamp(samples(tap.ms), 1, (int) l.getMaxDelay() - 2);
            }
        lengths.set(w);
        aimFront(shortest);

        // Each line's gain takes its round trip down by its share of 60 dB in DCAY. (The loop
        // allpass is not counted: its delay is not one length but differs with the frequency.)
        for (int k = 0; k < n; ++k)
        {
            const float pass = (float) w[(size_t) (LINE0 + k)];
            gain[k].set(! (decay < MAX_DECAY) ? 1.0f : std::min(0.9995f, std::exp(-6.9078f * pass / ((float) sampleRate * decay))));
        }
        swayDepth.set(swayAmount * std::max(0.0f, setup.swayMs) * 0.001f * (float) sampleRate);
        dispersionMix.set(dispersionOn() ? 1.0f : 0.0f);
        if (dispersionMix.target > 0.0f && dispersionMix.now == 0.0f && ! dispersing)
            for (auto& d : dispersion) d.clear();       // (coming in: nothing stale in it)

        dly::Glide* loopGlides[] = { &feed, &dispersionGain, &dispersionMix, &swayDepth, &diffusion };
        if (fresh)
        {
            lengths.snap();
            for (int k = 0; k < n; ++k) gain[k].snap();
            for (auto* g : loopGlides) g->snap();
        }
        else
        {
            lengths.aim(lengthShare);
            for (int k = 0; k < n; ++k) gain[k].aim(loopShare);
            for (auto* g : loopGlides) g->aim(loopShare);
        }
        moving = frontMoving() || ! lengths.resting();
        for (int k = 0; k < n; ++k) moving = moving || gain[k].moving();
        for (auto* g : loopGlides) moving = moving || g->moving();
        flags();
        dispersing = dispersionMix.target > 0.0f || dispersionMix.now > 0.0f;
        swaying = setup.sway != Sway::None && (swayDepth.target > 0.0f || swayDepth.now > 0.0f);
    }

    // `n` samples in place (no more than are left until the next control(), unless resting()): in (1 - mix) + wet mix.
    void run(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        fresh = false;
        const bool whole = lengths.resting() && preDelay.now == preDelay.target;
        // (The settings the instrument has are compiled for their line count and matrix; any other runs the general loop.)
        const int lines = setup.lines;
        const Matrix m = setup.matrix;
        constexpr int SELF = (int) Matrix::Self, CROSS = (int) Matrix::Cross, HOUSE = (int) Matrix::Householder, HADA = (int) Matrix::Hadamard;
        if (lines == 2 && m == Matrix::Self)             whole ? runAs<2, SELF, true>(left, right, n, mixFrom, mixTo) : runAs<2, SELF, false>(left, right, n, mixFrom, mixTo);
        else if (lines == 2 && m == Matrix::Cross)       whole ? runAs<2, CROSS, true>(left, right, n, mixFrom, mixTo) : runAs<2, CROSS, false>(left, right, n, mixFrom, mixTo);
        else if (lines == 4 && m == Matrix::Householder) whole ? runAs<4, HOUSE, true>(left, right, n, mixFrom, mixTo) : runAs<4, HOUSE, false>(left, right, n, mixFrom, mixTo);
        else if (lines == 8 && m == Matrix::Hadamard)    whole ? runAs<8, HADA, true>(left, right, n, mixFrom, mixTo) : runAs<8, HADA, false>(left, right, n, mixFrom, mixTo);
        else                                             whole ? runAs<0, -1, true>(left, right, n, mixFrom, mixTo) : runAs<0, -1, false>(left, right, n, mixFrom, mixTo);
    }

private:
    // The table of lengths: the input allpasses, the lines, their allpasses, the taps of each side.
    enum { LINE0 = MAX_DIFFUSERS, AP0 = LINE0 + MAX_LINES, TAPL = AP0 + MAX_LINES, TAPR = TAPL + MAX_TAPS };
    static_assert(TAPR + MAX_TAPS <= MAX_LENGTHS, "the table holds every length");

    std::array<const Tap*, 2 * MAX_TAPS> tapList(int& count) const
    {
        std::array<const Tap*, 2 * MAX_TAPS> all {};
        count = 0;
        for (int t = 0; t < setup.tapsLeft; ++t) all[(size_t) count++] = &setup.left[t];
        for (int t = 0; t < setup.tapsRight; ++t) all[(size_t) count++] = &setup.right[t];
        return all;
    }
    std::vector<const Tap*> taps() const        // (prepare only)
    {
        int count = 0;
        const auto all = tapList(count);
        return { all.begin(), all.begin() + count };
    }

    bool dispersionOn() const { return setup.dispersionAlways || dispersionGain.target > 0.0f; }
    void applyDamping() { for (auto& d : damp) d.setCutoff(sampleRate, dampNow); }
    void applyTone() { for (auto& t : tone) t.setCutoff(sampleRate, toneNow); }

    template <int Lines, int Mx, bool Whole>
    void runAs(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        using Cursor = dly::Line::Cursor;
        constexpr int L = Lines > 0 ? Lines : MAX_LINES;
        const int lines = Lines > 0 ? Lines : setup.lines;
        const Setup& s = setup;
        Way way = openWay();
        Cursor c[L], a[L];
        dly::Glide g[L];
        for (int k = 0; k < lines; ++k) { c[k] = line[k].open(); a[k] = allpass[k].open(); g[k] = gain[k]; }
        // (The filters' states with the run, in locals: a write to a line is not a write to them.)
        fxb::OnePole dmp[L], tn[L], cutL = lowCut[0], cutR = lowCut[1];
        for (int k = 0; k < lines; ++k) { dmp[k] = damp[k]; tn[k] = tone[k]; }
        dly::Glide fd = feed, da = dispersionGain, dm = dispersionMix, sw = swayDepth, df = diffusion;
        dly::Glide hd = hold, sh = shimmer, un = unit, ps = lengths.pos, pd = preDelay;
        const Table& len = lengths;
        const bool shim = shimmering, kept = holding, glide = moving, disperse = dispersing, swayed = swaying;
        const bool swayLines = swayed && s.sway == Sway::Lines, swayAps = swayed && s.sway == Sway::Allpasses;
        const bool aps = allpassed, damps = damped, toned = s.toneHz > 0.0f, stereo = s.stereoIn, cut = s.lowCutHz > 0.0f, onRead = s.filtersOnRead;
        const float inGain = s.inGain, outGain = s.outGain, depthAll = s.dampDepth, swayBias = s.swayBothWays ? 0.0f : 1.0f;
        const float room = s.headroom, perRoom = 1.0f / s.headroom;
        const Matrix matrix = Mx >= 0 ? (Matrix) Mx : s.matrix;
        // The places heard, resolved once: a line's end (`end`), or a cursor and an entry of the table.
        struct Heard { int end; const Cursor* cur; int k; float gain; int side; };
        Heard heard[2 * MAX_TAPS];
        int heardCount = 0;
        for (int t = 0; t < s.tapsLeft + s.tapsRight; ++t)
        {
            const bool isLeft = t < s.tapsLeft;
            const Tap& tap = isLeft ? s.left[t] : s.right[t - s.tapsLeft];
            if (tap.gain == 0.0f || (tap.ms <= 0.0f && tap.from >= ALLPASS)) continue;
            Heard& h = heard[heardCount++];
            h.end = tap.ms <= 0.0f ? tap.from : -1;
            h.cur = tap.from >= ALLPASS ? &a[tap.from - ALLPASS] : &c[tap.from];
            h.k = isLeft ? TAPL + t : TAPR + t - s.tapsLeft;
            h.gain = tap.gain;
            h.side = isLeft ? 0 : 1;
        }
        const float mixStep = (mixTo - mixFrom) / (float) std::max(1, n);
        float mix = mixFrom;

        auto place = [&](int k) DLY_INLINE_LAMBDA { return Whole ? (float) len.whole[(size_t) k] : len.at(k, ps.now); };
        auto readFixed = [&](const Cursor& cur, int k) DLY_INLINE_LAMBDA
        {
            return Whole ? cur.readAt(len.whole[(size_t) k]) : cur.readLinear(len.at(k, ps.now));
        };

        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];

            // The way in.
            float xa = inL, xb = inR;
            if (stereo) wayInPair<Whole>(way, xa, xb, pd.now);
            else        xa = xb = wayIn<Whole, -1>(way, inL, inR, ps.now, pd.now);
            if (cut)
            {
                xa -= cutL.lowPass(xa);
                xb = stereo ? xb - cutR.lowPass(xb) : xa;
            }
            const float open = fd.now * inGain * (kept ? 1.0f - hd.now : 1.0f);
            xa *= open;
            xb *= open;

            // The slow sway: each line by its own LFO, either way round where it is set, or from
            // there up to twice the depth further back.
            float off[L];
            if (swayed)
                for (int k = 0; k < lines; ++k) off[k] = (swayBias + lfo[k].next()) * sw.now;

            // The loop's filters: dispersion, the loop allpass, damping.
            auto filter = [&](float* v) DLY_INLINE_LAMBDA
            {
                if (disperse)
                    for (int k = 0; k + 1 < lines; k += 2)
                    {
                        float d0 = v[k], d1 = v[k + 1];
                        dispersion[k >> 1].process(d0, d1, da.now);
                        v[k] += dm.now * (d0 - v[k]);
                        v[k + 1] += dm.now * (d1 - v[k + 1]);
                    }
                if (aps)
                    for (int k = 0; k < lines; ++k)
                    {
                        if (len.whole[(size_t) (AP0 + k)] == 0) continue;
                        const float d = swayAps ? a[k].readLinear(place(AP0 + k) + off[k]) : readFixed(a[k], AP0 + k);
                        const float u = v[k] + df.now * d;
                        a[k].write(fxb::flushDenormal(u));
                        v[k] = d - df.now * u;
                    }
                if (damps)
                {
                    const float depth = kept ? depthAll * (1.0f - hd.now) : depthAll;
                    for (int k = 0; k < lines; ++k) v[k] += depth * (dmp[k].lowPass(v[k]) - v[k]);
                }
            };

            // What comes round.
            float y[L];
            for (int k = 0; k < lines; ++k)
                y[k] = swayLines ? c[k].readLinear(place(LINE0 + k) + off[k]) : readFixed(c[k], LINE0 + k);
            if (shim)
            {
                heads.next(un.now);
                for (int k = 0; k < lines; ++k)
                {
                    float up = heads.read(c[k], std::max(2.0f, place(LINE0 + k) - heads.span));
                    if (toned) up = tn[k].lowPass(up);
                    y[k] += sh.now * (up - y[k]);
                }
            }
            if (onRead) filter(y);

            // The mixing matrix.
            float back[L];
            switch (matrix)
            {
                case Matrix::Self:
                    for (int k = 0; k < lines; ++k) back[k] = y[k];
                    break;
                case Matrix::Cross:
                    for (int k = 0; k < lines; ++k) back[k] = y[k ^ 1];
                    break;
                case Matrix::Householder:
                {
                    float sum = 0.0f;
                    for (int k = 0; k < lines; ++k) sum += y[k];
                    const float share = sum * (2.0f / (float) lines);
                    for (int k = 0; k < lines; ++k) back[k] = share - y[k];
                    break;
                }
                case Matrix::Hadamard:
                {
                    for (int k = 0; k < lines; ++k) back[k] = y[k];
                    for (int span = 1; span < lines; span <<= 1)
                        for (int k = 0; k < lines; k += span << 1)
                            for (int j = k; j < k + span; ++j)
                            {
                                const float p = back[j], q = back[j + span];
                                back[j] = p + q;
                                back[j + span] = p - q;
                            }
                    const float norm = 1.0f / std::sqrt((float) lines);
                    for (int k = 0; k < lines; ++k) back[k] *= norm;
                    break;
                }
            }

            // Back into the lines: what comes in and what came round (through the filters here,
            // if not on the way out).
            float x[L];
            for (int k = 0; k < lines; ++k)
            {
                const float loop = kept ? g[k].now + hd.now * (1.0f - g[k].now) : g[k].now;
                x[k] = ((k & 1) ? xb : xa) + back[k] * loop;
            }
            if (! onRead) filter(x);
            for (int k = 0; k < lines; ++k) c[k].write(fxb::flushDenormal(room * dly::ceiling(x[k] * perRoom)));

            // What is heard.
            float out[2] = { 0.0f, 0.0f };
            for (int t = 0; t < heardCount; ++t)
            {
                const Heard& h = heard[t];
                out[h.side] += h.gain * (h.end >= 0 ? y[h.end] : readFixed(*h.cur, h.k));
            }

            const float wet = outGain * mix;
            left[i] = inL * (1.0f - mix) + out[0] * wet;
            right[i] = inR * (1.0f - mix) + out[1] * wet;
            mix += mixStep;

            if (glide)
            {
                for (int k = 0; k < lines; ++k) g[k].now += g[k].step;
                fd.now += fd.step; da.now += da.step; dm.now += dm.step; sw.now += sw.step; df.now += df.step;
                hd.now += hd.step; sh.now += sh.step; un.now += un.step; ps.now += ps.step; pd.now += pd.step;
            }
        }

        closeWay(way);
        for (int k = 0; k < lines; ++k) { damp[k] = dmp[k]; tone[k] = tn[k]; }
        lowCut[0] = cutL; lowCut[1] = cutR;
        for (int k = 0; k < lines; ++k) { line[k].close(c[k]); allpass[k].close(a[k]); gain[k].now = g[k].now; }
        feed.now = fd.now; dispersionGain.now = da.now; dispersionMix.now = dm.now; swayDepth.now = sw.now; diffusion.now = df.now;
        hold.now = hd.now; shimmer.now = sh.now; unit.now = un.now; lengths.pos.now = ps.now; preDelay.now = pd.now;
    }

    Setup setup;
    dly::Line line[MAX_LINES], allpass[MAX_LINES];
    fxb::OnePole damp[MAX_LINES], tone[MAX_LINES], lowCut[2];
    fxm::Lfo lfo[MAX_LINES];
    Dispersion dispersion[MAX_LINES / 2];
    dly::Glide gain[MAX_LINES];
    dly::Glide feed { 1.0f, 1.0f, 0.0f }, dispersionGain, dispersionMix, swayDepth, diffusion { 0.5f, 0.5f, 0.0f };
    float decay = 2.0f, swayAmount = 0.0f, dampAmount = 0.4f, dampTarget = 8000.0f, dampNow = 8000.0f, toneWanted = 5000.0f, toneNow = 5000.0f;
    int swayRoom = 2;
    bool allpassed = false, damped = true, dispersing = false, swaying = false, diffusionSet = false;
};

// ---- the settings ------------------------------------------------------------------------------------
// Every reverb the instrument has is one of these, with its cells on top (the effect headers).
namespace setting
{
// (The plate's and the hall's lengths were written in samples at 29,761 Hz: ms = samples / 29.761.)
inline float atReference(int samples) { return (float) samples * 1000.0f / (float) Front::REFERENCE_RATE; }

// The send reverb's plate (Dattorro, "Effect Design Part 1", JAES 1997): four allpasses on the way
// in, two loops that feed each other (the figure of eight), each through an allpass and a damping
// filter, heard at seven places a side.
inline Net::Setup plate()
{
    Net::Setup s;
    s.lines = 2;
    s.lineMs[0] = atReference(3163); s.lineMs[1] = atReference(3720);
    s.matrix = Net::Matrix::Cross;
    s.allpassMs[0] = atReference(672); s.allpassMs[1] = atReference(908);
    s.diffusion = -0.7f;
    s.diffusers = 4;
    const int d[4] = { 142, 107, 379, 277 };
    const float g[4] = { 0.75f, 0.75f, 0.625f, 0.625f };
    for (int k = 0; k < 4; ++k) { s.diffuserMs[k] = atReference(d[k]); s.diffuserG[k] = g[k]; }
    s.highCutHz = 10000.0f;
    s.brightHz = 28600.0f; s.darkHz = 837.0f;          // (DAMP 0.4 at 7 kHz and 0.8 at 1.7 kHz, as the plate had at 48 kHz)
    const int A = Net::ALLPASS;
    const Net::Tap l[7] = { { 0, atReference(266), 1 }, { 0, atReference(2974), 1 }, { A + 1, atReference(1913), -1 }, { 1, atReference(1996), 1 },
                            { 0, atReference(1913), -1 }, { A + 0, atReference(1066), -1 }, { 1, atReference(121), -1 } };
    const Net::Tap r[7] = { { 1, atReference(353), 1 }, { 1, atReference(3627), 1 }, { A + 0, atReference(1228), -1 }, { 0, atReference(2111), 1 },
                            { 1, atReference(335), -1 }, { A + 1, atReference(187), -1 }, { 0, atReference(1913), -1 } };
    s.tapsLeft = s.tapsRight = 7;
    for (int t = 0; t < 7; ++t) { s.left[t] = l[t]; s.right[t] = r[t]; }
    s.outGain = 0.6f;
    s.maxSize = Front::MAX_SIZE;
    return s;
}

// The hall: four allpasses on the way in, four long lines (no common divisor) mixed by a
// Householder matrix, each read by a head that the LFO moves, damped by a shelf on the way back.
inline Net::Setup hall()
{
    Net::Setup s;
    s.lines = 4;
    const int lines[4] = { 4799, 5399, 6199, 7001 };
    for (int k = 0; k < 4; ++k) s.lineMs[k] = atReference(lines[k]);
    s.matrix = Net::Matrix::Householder;
    s.sway = Net::Sway::Lines;
    s.swayMs = 0.25f;
    s.swayBothWays = true;
    const float hz[4] = { 0.31f, 0.37f, 0.43f, 0.29f };
    for (int k = 0; k < 4; ++k) s.swayHz[k] = hz[k];
    s.diffusers = 4;
    const int d[4] = { 547, 953, 1361, 1777 };
    const float g[4] = { 0.7f, 0.7f, 0.6f, 0.6f };
    for (int k = 0; k < 4; ++k) { s.diffuserMs[k] = atReference(d[k]); s.diffuserG[k] = g[k]; }
    s.inGain = 4.9f;
    s.highCutHz = 10000.0f;
    s.brightHz = 16000.0f; s.darkHz = 1750.0f; s.dampDepth = 0.5f;     // (DAMP 0.5 the hall's own shelf at 48 kHz)
    s.tapsLeft = s.tapsRight = 3;
    s.left[0] = { 0, 0, 1 }; s.left[1] = { 2, 0, 0.7f }; s.left[2] = { 3, 0, -0.3f };
    s.right[0] = { 1, 0, 1 }; s.right[1] = { 3, 0, 0.7f }; s.right[2] = { 2, 0, -0.3f };
    s.outGain = 0.4f;
    s.maxSize = Front::MAX_SIZE;
    return s;
}

// Two springs of different lengths, one fed by each side, each its own loop with the dispersion.
inline Net::Setup spring()
{
    Net::Setup s;
    s.lines = 2;
    s.lineMs[0] = 41.0f; s.lineMs[1] = 53.0f;
    s.matrix = Net::Matrix::Self;
    s.sway = Net::Sway::Lines;
    s.swayPhase[1] = 0.25f;
    s.dispersionAlways = true;
    s.stereoIn = true;
    s.brightHz = 7000.0f; s.darkHz = 900.0f;        // (a spring carries little above a few kHz)
    s.left[0] = { 0, 0, 0.8f }; s.left[1] = { 1, 0, 0.2f };
    s.right[0] = { 1, 0, 0.8f }; s.right[1] = { 0, 0, 0.2f };
    s.outGain = 1.4f;
    s.maxSize = 1.6f;
    s.filtersOnRead = true;
    return s;
}

// The shimmer tank: three allpasses on the way in, two long loops that feed each other, each
// through a swayed allpass, heard at their ends and inside each line.
inline Net::Setup shimmer()
{
    Net::Setup s;
    s.lines = 2;
    s.lineMs[0] = 161.3f; s.lineMs[1] = 142.6f;
    s.matrix = Net::Matrix::Cross;
    s.allpassMs[0] = 22.6f; s.allpassMs[1] = 30.5f;
    s.diffusion = 0.5f;
    s.sway = Net::Sway::Allpasses;
    s.swayPhase[1] = 0.25f;
    s.diffusers = 3;
    const float d[3] = { 14.1f, 26.0f, 37.5f };
    for (int k = 0; k < 3; ++k) { s.diffuserMs[k] = d[k]; s.diffuserG[k] = 0.65f; }
    s.tapsLeft = s.tapsRight = 3;
    s.left[0] = { 0, 0, 0.5f }; s.left[1] = { 0, 0.317f * 161.3f, 0.5f }; s.left[2] = { 1, 0.646f * 142.6f, 0.3f };
    s.right[0] = { 1, 0, 0.5f }; s.right[1] = { 1, 0.435f * 142.6f, 0.5f }; s.right[2] = { 0, 0.650f * 161.3f, 0.3f };
    s.outGain = 2.5f;
    s.maxSize = 2.0f;
    s.sizeMovesAll = false;         // (SIZE moves the two loops; the allpasses stay)
    s.filtersOnRead = true;
    return s;
}

// PERF's frozen field: eight lines (no two share a factor) mixed through a Hadamard matrix, fed
// through three allpasses and a high-pass, not damped; what the shifted heads read passes a low-pass.
inline Net::Setup field()
{
    Net::Setup s;
    s.lines = 8;
    const float lines[8] = { 53.3f, 61.7f, 68.9f, 77.3f, 85.1f, 93.7f, 103.1f, 112.9f };
    for (int k = 0; k < 8; ++k) s.lineMs[k] = lines[k];
    s.matrix = Net::Matrix::Hadamard;
    s.diffusers = 3;
    const float d[3] = { 4.7f, 8.3f, 12.1f };
    for (int k = 0; k < 3; ++k) { s.diffuserMs[k] = d[k]; s.diffuserG[k] = 0.6f; }
    s.lowCutHz = 25.0f;
    s.highCutHz = 6000.0f;          // (what the field always let in: its input allpasses were read between samples)
    s.inGain = 0.35355339f;         // 1 / sqrt(8): into every line
    s.dampDepth = 0.0f;
    s.toneHz = 5000.0f;
    s.headroom = 1.0f;              // (a held sound that climbs is bent above full scale: it brightens as it goes)
    s.tapsLeft = s.tapsRight = 4;
    for (int t = 0; t < 4; ++t)
    {
        s.left[t] = { 2 * t, 0, (t & 1) ? -0.5f : 0.5f };
        s.right[t] = { 2 * t + 1, 0, (t & 1) ? -0.5f : 0.5f };
    }
    s.maxSize = 1.0f;
    return s;
}
} // namespace setting

// An effect that is a setting of the network (cells from one table: CellEffect.h).
class Family : public CellEffect
{
protected:
    virtual Net::Setup setup() const = 0;

    void prepareCore() override { core.prepare(sampleRate, setup()); }
    void resetCore() override { core.reset(); }
    void controlCore() override { core.control(); }
    bool coasting() const override { return core.resting(); }
    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override { core.run(left, right, n, mixFrom, mixTo); }

    Net core;
};

// ==== PINNED >>> the send reverb's and the hall's own loops ========================================
//
// RVB and HALL as the project P01 uses them are held to the bit ("P01 Keyboard"), and the network
// cannot give their samples: its damping is one law (theirs are two others), its sway is the
// shared LFO (the hall's was std::sin per sample), its gain is one per line (theirs one for all),
// and its taps are read in another order. So these two loops stay, for these two effects only,
// until the owner has compared them with setting::plate() and setting::hall() by ear (the
// recordings: tests/one_network_tests.h). Everything round them is shared: the way in (Front:
// low-pass, pre-delay, input allpasses, as wayIn() does it), the shimmer heads and their
// interval, HOLD, the table of lengths and its glides, dly::Line, dly::Glide.
//
//   Tank     in -> low-pass -> pre-delay -> four allpasses -+-> [ + ] -> allpass (-decay) -> damping -> line A -+
//            (the plate of Dattorro, "Effect Design       |     ^                                             |
//            Part 1", JAES 1997: two loops that feed      |     +-- x decay <------- line B <-----------------|--+
//            each other, heard at seven places each side)  +-> [ + ] -> allpass (-decay) -> damping -> line B --+  |
//                                                                ^                                                |
//                                                                +-- x decay <------- line A <--------------------+
//   Network  in -> low-pass -> pre-delay -> four allpasses -> x 4.9 -> four lines, each read by a head that a
//            slow sine moves, mixed by a Householder matrix (every line feeds the three others and takes
//            itself away), damped, and written back
//
// At rest every length is a whole number of samples and every operation is the one the two
// reverbs always did, in the same order. Nothing is taken away from a state that has died down
// (see the note at the top of DelayCore.h).
// When the owner accepts the network's plate and hall: RVB and HALL play setting::plate() /
// setting::hall() (network::forPinned = true, then the switch goes), this block goes, and the pin
// is made again once for the two sends' sound.
namespace network
{
// false: RVB and HALL play their pinned loops (the default); true: the network's plate and hall.
// Read when an effect is prepared (the message thread); the recordings and their tests set it.
inline bool forPinned = false;
}

// The send reverb's loop.
class Tank : public Front
{
public:
    void prepare(double sr)
    {
        const double most = (sr / REFERENCE_RATE) * (double) MAX_SIZE;
        static constexpr float G[4] = { 0.75f, 0.75f, 0.625f, 0.625f };
        int room[4];
        for (int k = 0; k < 4; ++k) room[k] = (int) (BASE[D0 + k] * most);
        prepareFront(sr, 4, G, room, false, true);
        loopAp[0].prepare((int) (1228 * most) + 16);        // (heard as far back as its two taps)
        loopAp[1].prepare((int) (1913 * most) + 16);
        line[0].prepare((int) (3163 * most) + 16);
        line[1].prepare((int) (3720 * most) + 16);
        reset();
    }

    // Everything where it is set, the lines empty.
    void reset()
    {
        for (auto& l : loopAp) l.clear();
        for (auto& l : line) l.clear();
        dampL = dampR = 0.0f;
        resetFront();
        control();
    }

    void setDecay(float d) { decay.set(std::clamp(d, 0.0f, 0.99f)); changed = true; }       // the loop's gain
    void setDamping(float d) { damping.set(std::clamp(d, 0.0f, 1.0f)); changed = true; }

    void control()
    {
        if (! (changed || moving)) return;
        changed = false;
        std::array<int, MAX_LENGTHS> w {};
        for (int k = 0; k < COUNT; ++k) w[(size_t) k] = scaled(BASE[k], 1 << 28);
        lengths.set(w);
        aimFront((float) w[R1]);
        if (fresh)
        {
            lengths.snap();
            decay.snap();
            damping.snap();
        }
        else
        {
            lengths.aim(lengthShare);
            decay.aim(loopShare);
            damping.aim(loopShare);
        }
        moving = frontMoving() || ! lengths.resting() || decay.moving() || damping.moving();
        flags();
    }

    // `n` samples in place (no more than are left until the next control(), unless resting()).
    void run(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        fresh = false;
        if (lengths.resting() && preDelay.now == preDelay.target) runAs<true>(left, right, n, mixFrom, mixTo);
        else                                                      runAs<false>(left, right, n, mixFrom, mixTo);
    }

private:
    // The lengths, in samples at the reference rate: the four allpasses on the way in, the allpass
    // in each loop, where each loop reads the other, and the seven places each side is heard at.
    enum { D0 = 0, A0 = 4, A1, R0, R1, TL = 8, TR = 15, COUNT = 22 };
    static constexpr int BASE[COUNT] = { 142, 107, 379, 277, 672, 908, 3720, 3163,
                                         266, 2974, 1913, 1996, 1913, 1066, 121,
                                         353, 3627, 1228, 2111, 335, 187, 1913 };

    template <bool Whole>
    void runAs(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        using Cursor = dly::Line::Cursor;
        Way way = openWay();
        Cursor a0 = loopAp[0].open(), a1 = loopAp[1].open(), t0 = line[0].open(), t1 = line[1].open();
        dly::Glide dec = decay, dmp = damping, hd = hold, sh = shimmer, un = unit, ps = lengths.pos, pd = preDelay;
        const Table& len = lengths;
        const bool shim = shimmering, frozen = holding, guard = shim || frozen, glide = moving;
        float dL = dampL, dR = dampR;
        const float mixStep = (mixTo - mixFrom) / (float) std::max(1, n);
        float mix = mixFrom;

        // A place on a line: a whole number of samples at rest, between two on the way.
        auto place = [&](int k) DLY_INLINE_LAMBDA { return Whole ? (float) len.whole[(size_t) k] : len.at(k, ps.now); };
        auto read = [&](const Cursor& c, int k) DLY_INLINE_LAMBDA
        {
            return Whole ? c.readAt(len.whole[(size_t) k]) : c.readLinear(len.at(k, ps.now));
        };
        // Schroeder allpass: v = x + g d, y = d - g v, v stored.
        auto allpass = [&](Cursor& c, float x, int k, float g) DLY_INLINE_LAMBDA
        {
            const float d = read(c, k);
            const float v = x + d * g;
            const float y = d - v * g;
            c.write(v);
            return y;
        };

        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];

            // The way in.
            float mono = wayIn<Whole, 4>(way, inL, inR, ps.now, pd.now);

            // What comes round: each loop reads the other.
            float tankL = read(t1, R0), tankR = read(t0, R1);
            if (shim)
            {
                heads.next(un.now);
                const float upL = heads.read(t1, std::max(2.0f, place(R0) - heads.span));
                const float upR = heads.read(t0, std::max(2.0f, place(R1) - heads.span));
                tankL += sh.now * (upL - tankL);
                tankR += sh.now * (upR - tankR);
            }

            float loop = dec.now, damp = dmp.now;
            if (frozen)
            {
                const float keep = 1.0f - hd.now;
                mono *= keep;
                loop += hd.now * (1.0f - loop);
                damp *= keep;
            }

            float xl = mono + tankL * loop;
            xl = allpass(a0, xl, A0, -dec.now);
            dL = xl * (1.0f - damp) + dL * damp;
            t0.write(guard ? held(dL) : dL);

            float xr = mono + tankR * loop;
            xr = allpass(a1, xr, A1, -dec.now);
            dR = xr * (1.0f - damp) + dR * damp;
            t1.write(guard ? held(dR) : dR);

            // What is heard.
            float outL = read(t0, TL) + read(t0, TL + 1) - read(a1, TL + 2) + read(t1, TL + 3)
                       - read(t0, TL + 4) - read(a0, TL + 5) - read(t1, TL + 6);
            float outR = read(t1, TR) + read(t1, TR + 1) - read(a0, TR + 2) + read(t0, TR + 3)
                       - read(t1, TR + 4) - read(a1, TR + 5) - read(t0, TR + 6);
            outL *= 0.6f;
            outR *= 0.6f;

            left[i] = inL * (1.0f - mix) + outL * mix;
            right[i] = inR * (1.0f - mix) + outR * mix;
            mix += mixStep;

            if (glide)
            {
                dec.now += dec.step; dmp.now += dmp.step; hd.now += hd.step; sh.now += sh.step; un.now += un.step;
                ps.now += ps.step; pd.now += pd.step;
            }
        }

        closeWay(way);
        loopAp[0].close(a0); loopAp[1].close(a1); line[0].close(t0); line[1].close(t1);
        dampL = dL; dampR = dR;
        decay.now = dec.now; damping.now = dmp.now; hold.now = hd.now; shimmer.now = sh.now; unit.now = un.now;
        lengths.pos.now = ps.now; preDelay.now = pd.now;
    }

    dly::Line loopAp[2], line[2];
    dly::Glide decay { 0.7f, 0.7f, 0.0f }, damping { 0.5f, 0.5f, 0.0f };
    float dampL = 0.0f, dampR = 0.0f;
};

// The hall's loop.
class Network : public Front
{
public:
    static constexpr int LINES = 4;
    static constexpr float MOD_SAMPLES = 12.0f;         // how far a head is moved at full depth

    void prepare(double sr)
    {
        const double most = (sr / REFERENCE_RATE) * (double) MAX_SIZE;
        static constexpr float G[4] = { 0.7f, 0.7f, 0.6f, 0.6f };
        int room[4];
        for (int k = 0; k < 4; ++k) room[k] = (int) (BASE[k] * most);
        prepareFront(sr, 4, G, room, false, true);
        // (A ring of 32,768 wherever the lines fit one at every SIZE - up to 69 kHz - as it always
        // was. Above that, as long as the lines need: see `ring` in control().)
        const int lineRoom = std::max(32760, (int) (BASE[L0 + LINES - 1] * most) + (int) MOD_SAMPLES + 24);
        for (auto& l : line) l.prepare(lineRoom);
        for (int k = 0; k < LINES; ++k) modStep[k] = MOD_HZ[k] / sampleRate;
        reset();
    }

    void reset()
    {
        for (auto& l : line) l.clear();
        for (auto& d : damp) d = 0.0f;
        for (auto& ph : modPhase) ph = 0.0;
        resetFront();
        control();
    }

    void setDecay(float seconds) { decayTime = std::clamp(seconds, 0.5f, 10.0f); changed = true; }
    void setDamping(float d) { damping.set(std::clamp(d, 0.0f, 1.0f)); changed = true; }
    void setModulation(float depth) { modDepth.set(std::clamp(depth, 0.0f, 1.0f) * MOD_SAMPLES); changed = true; }

    void control()
    {
        if (! (changed || moving)) return;
        changed = false;
        const int most = (int) line[0].getMaxDelay() - (int) MOD_SAMPLES - 4;
        std::array<int, MAX_LENGTHS> w {};
        for (int k = 0; k < COUNT; ++k) w[(size_t) k] = scaled(BASE[k], most);
        lengths.set(w);
        // The heads' places between two samples were always worked out in a ring of 32,768 (which
        // float a place is rounded to depends on it): they still are while the longest line fits one.
        ring = w[L0 + LINES - 1] + (int) MOD_SAMPLES + 2 <= OLD_RING - 2 ? OLD_RING : (int) line[0].getMaxDelay() + 6;
        // The loop's gain for the decay time, from the first line's length.
        gain.set(std::clamp(std::exp(-3.0f / (decayTime * (float) sampleRate) * (float) w[L0]), 0.0f, 0.999f));
        aimFront((float) w[L0]);
        if (fresh)
        {
            lengths.snap();
            for (auto* g : { &gain, &damping, &modDepth }) g->snap();
        }
        else
        {
            lengths.aim(lengthShare);
            for (auto* g : { &gain, &damping, &modDepth }) g->aim(loopShare);
        }
        moving = frontMoving() || ! lengths.resting() || gain.moving() || damping.moving() || modDepth.moving();
        swaying = modDepth.target > 0.0f || modDepth.now > 0.0f;
        flags();
    }

    void run(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        fresh = false;
        if (lengths.resting() && preDelay.now == preDelay.target) runAs<true>(left, right, n, mixFrom, mixTo);
        else                                                      runAs<false>(left, right, n, mixFrom, mixTo);
    }

private:
    // In samples at the reference rate: the four allpasses on the way in, then the four lines
    // (no common divisor: their echoes do not pile up).
    enum { L0 = 4, COUNT = 8 };
    static constexpr int BASE[COUNT] = { 547, 953, 1361, 1777, 4799, 5399, 6199, 7001 };
    static constexpr float MOD_HZ[LINES] = { 0.31f, 0.37f, 0.43f, 0.29f };     // each head's own slow sine
    static constexpr int OLD_RING = 32768;

    template <bool Whole>
    void runAs(float* left, float* right, int n, float mixFrom, float mixTo)
    {
        using Cursor = dly::Line::Cursor;
        Way way = openWay();
        Cursor c[LINES] = { line[0].open(), line[1].open(), line[2].open(), line[3].open() };
        dly::Glide g = gain, dmp = damping, md = modDepth, hd = hold, sh = shimmer, un = unit, ps = lengths.pos, pd = preDelay;
        const Table& len = lengths;
        const bool shim = shimmering, frozen = holding, guard = shim || frozen, glide = moving, swayed = swaying;
        const int inRing = ring - 1;        // (a power of two that divides the line's own: a mask)
        float ds[LINES] = { damp[0], damp[1], damp[2], damp[3] };
        double phase[LINES] = { modPhase[0], modPhase[1], modPhase[2], modPhase[3] };
        const float mixStep = (mixTo - mixFrom) / (float) std::max(1, n);
        float mix = mixFrom;

        auto place = [&](int k) DLY_INLINE_LAMBDA { return Whole ? (float) len.whole[(size_t) k] : len.at(k, ps.now); };

        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];

            // The way in.
            float mono = wayIn<Whole, 4>(way, inL, inR, ps.now, pd.now);
            mono *= 4.9f;

            // What comes round: each line read by its head, which its own slow sine moves.
            float out[LINES];
            for (int k = 0; k < LINES; ++k)
            {
                phase[k] += modStep[k];
                if (phase[k] >= 1.0) phase[k] -= 1.0;
                float at = place(L0 + k);
                if (swayed)
                {
                    const float offset = std::sin((float) phase[k] * dsp::TWO_PI_F) * md.now;
                    at = at + offset;
                }
                out[k] = c[k].readBlend(at, c[k].w & inRing, inRing + 1);
            }
            if (shim)
            {
                heads.next(un.now);
                for (int k = 0; k < LINES; ++k)
                {
                    const float up = heads.read(c[k], std::max(2.0f, place(L0 + k) - heads.span));
                    out[k] += sh.now * (up - out[k]);
                }
            }

            float loop = g.now, damping01 = dmp.now;
            if (frozen)
            {
                const float keep = 1.0f - hd.now;
                mono *= keep;
                loop += hd.now * (1.0f - loop);
                damping01 *= keep;
            }

            // Householder: every line is fed by the sum of all four less twice itself, halved.
            const float sum = out[0] + out[1] + out[2] + out[3];
            for (int k = 0; k < LINES; ++k)
            {
                const float back = (sum - 2.0f * out[k]) * 0.5f;
                float sig = mono + back * loop;
                ds[k] += damping01 * (sig - ds[k]);
                sig = ds[k] * damping01 + sig * (1.0f - damping01);
                c[k].write(guard ? held(sig) : sig);
            }

            float outL = out[0] + out[2] * 0.7f - out[3] * 0.3f;
            float outR = out[1] + out[3] * 0.7f - out[2] * 0.3f;
            outL *= 0.4f;
            outR *= 0.4f;

            left[i] = inL * (1.0f - mix) + outL * mix;
            right[i] = inR * (1.0f - mix) + outR * mix;
            mix += mixStep;

            if (glide)
            {
                g.now += g.step; dmp.now += dmp.step; md.now += md.step; hd.now += hd.step; sh.now += sh.step;
                un.now += un.step; ps.now += ps.step; pd.now += pd.step;
            }
        }

        closeWay(way);
        for (int k = 0; k < LINES; ++k)
        {
            line[k].close(c[k]);
            damp[k] = ds[k];
            modPhase[k] = phase[k];
        }
        gain.now = g.now; damping.now = dmp.now; modDepth.now = md.now; hold.now = hd.now; shimmer.now = sh.now;
        unit.now = un.now; lengths.pos.now = ps.now; preDelay.now = pd.now;
    }

    dly::Line line[LINES];
    float decayTime = 3.0f;
    dly::Glide gain { 0.9f, 0.9f, 0.0f }, damping { 0.5f, 0.5f, 0.0f }, modDepth { 3.6f, 3.6f, 0.0f };
    float damp[LINES] = { 0.0f, 0.0f, 0.0f, 0.0f };
    double modPhase[LINES] = { 0.0, 0.0, 0.0, 0.0 }, modStep[LINES] = { 0.0, 0.0, 0.0, 0.0 };
    bool swaying = true;
    int ring = OLD_RING;
};
// ==== <<< PINNED ==================================================================================

} // namespace rvb
