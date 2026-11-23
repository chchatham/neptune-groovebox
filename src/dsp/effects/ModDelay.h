#pragma once

// One modulated delay. The chorus, the ensemble, the flanger, the vibrato and the rotary speaker's
// two rotors are settings of it (Chorus.h, Ensemble.h, Flanger.h, Vibrato.h, RotarySpeaker.h):
//
//   voices      1..MAX_VOICES taps on one stereo line (or on one line fed with the middle of the
//               input, as a bucket-brigade ensemble has it), each with its own dsp/LFO.h: the same
//               shape, set apart in phase by an even share of the cycle, in rate by a spread, and
//               across the stereo field by a pan spread;
//   delay       base (samples) + centre + swing x LFO (seconds), held to a range; every tap follows
//               its place through a 1.5 ms lag and never faster than MAX_SLEW samples a sample, so
//               a shape that jumps, a knob or an offset from outside is a bend of the pitch (a
//               quarter at most) and never a click;
//   stereo      the right side reads the LFO a part of a cycle further on;
//   feedback    the wet sum back into the line, either polarity, held to +-FB_LIMIT;
//   through     the dry sound delayed by the centre, so that the tap passes through it (a flanger
//   zero        that cancels to nothing at the crossing);
//   level       a tap's level can follow its LFO a quarter turn on (a rotor: loudest as it faces
//               you, which is a quarter turn after it came fastest towards you), opposite on the
//               two sides;
//   mix         dry (1 - mix) + wet mix, along the line it is handed (CellEffect's).
//
// Taps are read with 4-point, 3rd-order Hermite interpolation (Catmull-Rom; Niemitalo, "Polynomial
// Interpolators for High-Quality Resampling of Oversampled Audio"): good for slow deep sweeps and
// for vibrato, and exact at a whole number of samples. The LFOs are stepped on the effects' clock
// (every CellEffect::CONTROL samples) and joined by straight lines.
//
// Memory is taken in prepare() only. The input is CellEffect's: numbers within its limit; any such
// input gives a finite, bounded output.

#include "FxLfo.h"
#include <vector>


namespace moddelay
{

static constexpr int MAX_VOICES = 6;

// A stereo delay line (frames side by side) read at a fractional delay; one set of interpolation
// weights serves both sides when they read the same place.
class Line
{
public:
    struct Tap
    {
        int at[4];
        float w[4];
    };

    void prepare(int maxDelaySamples)
    {
        int n = 16;
        while (n < maxDelaySamples + 8)
            n <<= 1;
        buf.assign((size_t) n * 2, 0.0f);
        mask = n - 1;
        longest = (float) (n - 6);
        pos = 0;
    }

    void clear()
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        pos = 0;
    }

    float limit() const { return longest; }

    // Read before writing: a delay of d then gives what was written d calls ago (d from 2 up).
    Tap tap(float d) const
    {
        d = std::min(std::max(2.0f, d), longest);       // (what is not a number is 2)
        const int whole = (int) d;
        const float f = d - (float) whole, f2 = f * f, f3 = f2 * f;
        Tap t;
        t.w[0] = -0.5f * f + f2 - 0.5f * f3;            // whole - 1
        t.w[1] = 1.0f - 2.5f * f2 + 1.5f * f3;          // whole
        t.w[2] = 0.5f * f + 2.0f * f2 - 1.5f * f3;      // whole + 1
        t.w[3] = 0.5f * (f3 - f2);                      // whole + 2
        const int newest = pos - whole + 1;
        for (int i = 0; i < 4; ++i)
            t.at[i] = ((newest - i) & mask) << 1;
        return t;
    }

    float left(const Tap& t) const  { return read(t, 0); }
    float right(const Tap& t) const { return read(t, 1); }

    void write(float l, float r)
    {
        const size_t i = (size_t) pos << 1;
        buf[i] = l;
        buf[i + 1] = r;
        pos = (pos + 1) & mask;
    }

private:
    float read(const Tap& t, int side) const
    {
        const float* b = buf.data() + side;
        return b[t.at[0]] * t.w[0] + b[t.at[1]] * t.w[1] + b[t.at[2]] * t.w[2] + b[t.at[3]] * t.w[3];
    }

    std::vector<float> buf;
    int mask = 0, pos = 0;
    float longest = 2.0f;
};

class Core
{
public:
    static constexpr float MAX_SLEW = 0.25f;            // samples a sample: the pitch bends by a quarter at most
    static constexpr float FOLLOW_SECONDS = 0.0015f;
    static constexpr float GLIDE_MS = 5.0f;             // levels, feedback
    static constexpr int INTERVAL = CellEffect::CONTROL;
    static constexpr float FB_LIMIT = 4.0f;

    Core()
    {
        for (int v = 0; v < MAX_VOICES; ++v)
            voices[(size_t) v].lfo.setSeed(12345u + 7919u * (uint32_t) v);
    }

    // monoInput: one line fed with the middle of the input instead of one a side.
    void prepare(double sr, double maxDelaySeconds, bool monoInput)
    {
        sampleRate = CellEffect::goodRate(sr);
        monoIn = monoInput;
        line.prepare((int) std::ceil(std::clamp(maxDelaySeconds, 0.001, 2.0) * sampleRate) + 8);
        const double ticks = sampleRate / (double) INTERVAL;
        for (auto& q : voices)
        {
            q.lfo.setSampleRate(ticks);
            q.baseL.setTime(ticks, GLIDE_MS);
            q.baseR.setTime(ticks, GLIDE_MS);
        }
        fbGlide.setTime(ticks, GLIDE_MS);
        zeroGlide.setTime(ticks, 2.0f * GLIDE_MS);
        follow = 1.0f - std::exp(-1.0f / (FOLLOW_SECONDS * (float) sampleRate));
        layout();
        reset();
    }

    // The line empty, the LFOs at their start, every glide at its goal.
    void reset()
    {
        line.clear();
        for (auto& q : voices)
        {
            q.lfo.reset();
            q.lfo.drawRandom();
            q.awake = false;
            q.holding = false;
        }
        left = 0;
        active = 0;
        fresh = true;
        split = false;
    }

    // ---- the setting ------------------------------------------------------------------------------
    void setVoices(int n)
    {
        n = std::clamp(n, 1, MAX_VOICES);
        if (n == wanted) return;
        wanted = n;
        layout();
    }
    void setShape(int index) { for (auto& q : voices) q.lfo.setShape(fxm::Lfo::shapeOf(index)); }
    // What a trigger does to the voices' LFOs (dsp/LFO.h's trigger modes), and the trigger itself:
    // a sound has begun (fxm::Onset). A held LFO is held on both sides.
    void setTrigger(int mode)
    {
        const auto m = (LFO::TriggerMode) std::clamp(mode, 0, (int) LFO::TriggerMode::COUNT - 1);
        for (auto& q : voices)
            if (m != q.lfo.getTriggerMode()) { q.lfo.setTriggerMode(m); q.holding = false; }
    }
    bool listens() const { return voices[0].lfo.getTriggerMode() != LFO::TriggerMode::Free; }
    void trigger()
    {
        for (auto& q : voices)
        {
            q.lfo.trigger();
            q.holding = q.lfo.getTriggerMode() == LFO::TriggerMode::Hold;
            if (q.holding) q.heldAhead = q.lfo.shapeAt(stereo);
        }
    }
    void setUserShape(const float* levels) { for (auto& q : voices) q.lfo.setUserShape(levels); }
    void setRate(float hz)
    {
        if (hz == rate || ! std::isfinite(hz)) return;
        rate = std::max(0.0f, hz);
        layout();
    }
    // Voice v runs at rate x (1 + rateSpread x skew[v]) and starts phaseSpread x v / voices of a
    // cycle on; the outermost voices sit at -+ pan (0..1) of the stereo field.
    void setSpread(float rates, float phases, float pan)
    {
        rateSpread = rates;
        phaseSpread = phases;
        panSpread = pan;
        layout();
    }
    // delay = baseSamples + (centre + swing x LFO) seconds, held to [minSeconds, base + maxSeconds].
    void setDelay(float base, float centreSeconds, float swingSeconds)
    {
        if (! (std::isfinite(base) && std::isfinite(centreSeconds) && std::isfinite(swingSeconds))) return;
        baseSamples = std::max(0.0f, base);
        centre = centreSeconds;
        swing = swingSeconds;
    }
    void setRange(float minSeconds, float maxSeconds) { shortest = minSeconds; longest = maxSeconds; }
    void setFeedback(float f) { if (std::isfinite(f)) feedback = std::clamp(f, -0.98f, 0.98f); }
    void setStereo(float cycles) { if (std::isfinite(cycles)) stereo = cycles; }
    void setWidth(float w) { if (std::isfinite(w)) width = std::clamp(w, 0.0f, 1.0f); }
    void setThroughZero(bool on) { throughZero = on; }
    void setLevelFollow(float amount) { if (std::isfinite(amount)) am = std::clamp(amount, 0.0f, 1.0f); }

    // The centre delay (the through-zero crossing point) in samples: base + centre * sampleRate.
    float centreDelaySamples() const
    {
        return baseSamples + centre * (float) sampleRate;
    }

    // Where a tap is now, in samples (side 0 left, 1 right): for meters and tests.
    float tapDelay(int voice, int side) const
    {
        const Voice& q = voices[(size_t) std::clamp(voice, 0, MAX_VOICES - 1)];
        return (side == 0 || ! split) ? q.dL : q.dR;
    }

    // In place: dry (1 - mix) + wet mix, the mix going from mixFrom to mixTo over the samples.
    // (The steps are counted here, from reset(): an effect's stretches fall on them.)
    void process(float* l, float* r, int numSamples, float mixFrom = 1.0f, float mixTo = 1.0f)
    {
        const float slope = (mixTo - mixFrom) / (float) std::max(1, numSamples);
        int done = 0;
        while (done < numSamples)
        {
            if (left <= 0)
                tick();
            const int n = std::min(left, numSamples - done);
            run(l + done, r + done, n, mixFrom + slope * (float) done, slope);
            left -= n;
            done += n;
        }
    }

private:
    // What a voice moves every sample (copied to the stack for the loop: a write to the output
    // could otherwise be a write to any of it, as far as the compiler knows).
    struct Tapping
    {
        fxm::Ramp tL, tR, gL, gR;           // where its tap should be (samples), its level
        float dL = 2.0f, dR = 2.0f;         // where its tap is
    };

    struct Voice : Tapping
    {
        LFO lfo;
        fxb::Smoother baseL, baseR;         // its share of each side, glided
        bool awake = false;
        bool holding = false;               // the LFO is held (trigger mode HOLD): so is its second reader
        float heldAhead = 0.0f;
    };

    static float skew(int v)
    {
        static constexpr float s[MAX_VOICES] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.75f };
        return s[v];
    }

    void layout()
    {
        for (int v = 0; v < MAX_VOICES; ++v)
        {
            auto& lfo = voices[(size_t) v].lfo;
            lfo.setRate(rate * (1.0f + rateSpread * skew(v)));
            lfo.setStartPhase(phaseSpread * (float) v / (float) wanted);
        }
    }

    // Once every INTERVAL samples: the LFOs, and where everything is to be one step on.
    void tick()
    {
        const int n = wanted;
        const float sr = (float) sampleRate;
        const float lo = std::max(2.0f, shortest * sr);
        const float hi = std::max(lo, std::min(line.limit(), baseSamples + longest * sr));
        const float share = 1.0f / (float) n, phase = stereo, level = am;
        // The sides read their own places while the right is set ahead, and afterwards until it
        // has come back to where the left is.
        const bool wasSplit = split, ahead = phase != 0.0f;
        bool together = true;
        int last = 0;
        for (int v = 0; v < MAX_VOICES; ++v)
        {
            Voice& q = voices[(size_t) v];
            const bool on = v < n;
            if (! on && ! q.awake)
                continue;
            const float pan = (on && n > 1) ? (-1.0f + 2.0f * (float) v / (float) (n - 1)) * panSpread * width : 0.0f;
            const float wantL = on ? share * (1.0f - pan) : 0.0f, wantR = on ? share * (1.0f + pan) : 0.0f;
            const bool start = fresh || ! q.awake;
            if (fresh)      { q.baseL.snap(wantL); q.baseR.snap(wantR); }
            else if (start) { q.baseL.snap(0.0f);  q.baseR.snap(0.0f); }

            const float a = q.lfo.process();
            const float b = ahead ? (q.holding ? q.heldAhead : q.lfo.shapeAt(phase)) : a;
            const float tl = std::clamp(baseSamples + (centre + swing * a) * sr, lo, hi);
            const float tr = std::clamp(baseSamples + (centre + swing * b) * sr, lo, hi);
            float gl = q.baseL.next(wantL), gr = q.baseR.next(wantR);
            if (level > 0.0f)
            {
                const float turn = q.lfo.shapeAt(0.25f);
                gl *= 1.0f - level + level * turn;
                gr *= 1.0f - level - level * turn;
            }
            if (start)
            {
                // (A voice that joins the others comes up from nothing: its level is not jumped to.)
                q.tL.snap(tl); q.tR.snap(tr);
                q.gL.snap(fresh ? gl : 0.0f); q.gR.snap(fresh ? gr : 0.0f);
                q.dL = tl; q.dR = tr;
            }
            if (! fresh)
                { q.gL.aim(gl, step); q.gR.aim(gr, step); }
            if (! start)
            {
                q.tL.aim(tl, step); q.tR.aim(tr, step);
                if (! wasSplit)
                    q.dR = q.dL;
                together = together && std::abs(q.dR - q.dL) < 0.001f;
            }
            q.awake = on || q.baseL.value() != 0.0f || q.baseR.value() != 0.0f;
            if (q.awake)
                last = v + 1;
        }
        active = last;
        split = ahead || (wasSplit && ! together);

        const float dry = std::clamp(baseSamples + centre * sr, lo, hi);
        const float zeroWanted = (throughZero && ! monoIn) ? 1.0f : 0.0f;
        if (fresh)
        {
            fbRamp.snap(fbGlide.z = feedback);
            zeroRamp.snap(zeroGlide.z = zeroWanted);
            dryTap = dry;
        }
        else
        {
            fbRamp.aim(fbGlide.next(feedback), step);
            zeroRamp.aim(zeroGlide.next(zeroWanted), step);
        }
        dryGoal = dry;
        zero = zeroRamp.now != 0.0f || zeroRamp.goal != 0.0f;
        fresh = false;
        left = INTERVAL;
    }

    // (The two things fixed for a block of the loop are compiled in: one line or two, and whether
    // the sides read different places.)
    void run(float* l, float* r, int n, float mix, float slope)
    {
        if (monoIn) { if (split) runAs<true, true>(l, r, n, mix, slope);  else runAs<true, false>(l, r, n, mix, slope); }
        else        { if (split) runAs<false, true>(l, r, n, mix, slope); else runAs<false, false>(l, r, n, mix, slope); }
    }

    template <bool Mono, bool Split>
    void runAs(float* l, float* r, int n, float mix, float slope)
    {
        const bool thru = zero, loop = fbRamp.now != 0.0f || fbRamp.goal != 0.0f;
        const float k = follow;
        const int count = active;
        Tapping taps[MAX_VOICES];
        for (int v = 0; v < count; ++v)
            taps[v] = voices[(size_t) v];
        fxm::Ramp feeding = fbRamp, crossing = zeroRamp;
        float dryAt = dryTap;
        const float dryTo = dryGoal;

        for (int s = 0; s < n; ++s)
        {
            const float inL = l[s], inR = r[s];
            float wetL = 0.0f, wetR = 0.0f;
            for (int v = 0; v < count; ++v)
            {
                Tapping& q = taps[v];
                q.dL += std::clamp((q.tL.next() - q.dL) * k, -MAX_SLEW, MAX_SLEW);
                const Line::Tap t = line.tap(q.dL);
                const float a = line.left(t);
                float b;
                if (Split)
                {
                    q.dR += std::clamp((q.tR.next() - q.dR) * k, -MAX_SLEW, MAX_SLEW);
                    const Line::Tap u = line.tap(q.dR);
                    b = Mono ? line.left(u) : line.right(u);
                }
                else
                    b = Mono ? a : line.right(t);
                wetL += a * q.gL.next();
                wetR += b * q.gR.next();
            }

            float dryL = inL, dryR = inR;
            if (thru)
            {
                dryAt += std::clamp((dryTo - dryAt) * k, -MAX_SLEW, MAX_SLEW);
                const Line::Tap t = line.tap(dryAt);
                const float z = crossing.next();
                dryL += z * (line.left(t) - inL);
                dryR += z * (line.right(t) - inR);
            }

            if (loop)
            {
                const float f = feeding.next();
                if (Mono)
                    line.write(fxb::flushDenormal(0.5f * (inL + inR) + f * std::clamp(0.5f * (wetL + wetR), -FB_LIMIT, FB_LIMIT)), 0.0f);
                else
                    line.write(fxb::flushDenormal(inL + f * std::clamp(wetL, -FB_LIMIT, FB_LIMIT)),
                               fxb::flushDenormal(inR + f * std::clamp(wetR, -FB_LIMIT, FB_LIMIT)));
            }
            else if (Mono)
                line.write(0.5f * (inL + inR), 0.0f);
            else
                line.write(inL, inR);

            const float m = (mix += slope);
            l[s] = dryL * (1.0f - m) + wetL * m;
            r[s] = dryR * (1.0f - m) + wetR * m;
        }

        for (int v = 0; v < count; ++v)
            static_cast<Tapping&> (voices[(size_t) v]) = taps[v];
        fbRamp = feeding;
        zeroRamp = crossing;
        dryTap = dryAt;
    }

    Line line;
    std::array<Voice, MAX_VOICES> voices;
    double sampleRate = 44100.0;
    bool monoIn = false;
    static constexpr float step = 1.0f / (float) INTERVAL;
    int left = 0, active = 0;
    float follow = 1.0f;
    bool fresh = true, split = false, zero = false;

    // the setting
    int wanted = 1;
    float rate = 1.0f, rateSpread = 0.0f, phaseSpread = 1.0f, panSpread = 0.0f;
    float baseSamples = 0.0f, centre = 0.005f, swing = 0.0f, shortest = 0.0f, longest = 0.02f;
    float feedback = 0.0f, stereo = 0.0f, width = 1.0f, am = 0.0f;
    bool throughZero = false;

    fxb::Smoother fbGlide, zeroGlide;
    fxm::Ramp fbRamp, zeroRamp;
    float dryTap = 2.0f, dryGoal = 2.0f;
};

// An effect that is the core and its cells (CellEffect.h): it names them, prepares the core and
// says in configure() what each cell sets.
// (Through fxm::KeyedCells: one of them that has a KEY cell names it in keyCell() and listens
// in runKeyed(); the others never ask for a key.)
//
// THRU, through zero, is here once for all of them (FLG, CHR, ENS, VIB): the cell (thruSpec),
// what it sets (setThrough, from configure()) and what the chain is told. With THRU on the dry
// sound is to be delayed by the centre delay, so that the sweep passes through it. In an
// EffectChain the chain does that (dryDelaySamples: its DryLine); the core's own dry tap serves
// an effect that is run outside a chain.
class Effect : public fxm::KeyedCells
{
public:
    void setUserShape(const float* levels) override { core.setUserShape(levels); }

    bool needsDryLine() const override { return true; }
    float dryDelaySamples() const override { return thruOn ? core.centreDelaySamples() : 0.0f; }

protected:
    static ParamSpec thruSpec(int id) { return { "THRU", id, 0, 1, 0, ParamKind::Choice, "", ParamChoices::offOn, 2 }; }
    void setThrough(float cellValue)
    {
        thruOn = cellValue >= 0.5f;
        core.setThroughZero(thruOn && ! chained);
    }

    int keyCell() const override { return -1; }
    void resetCore() override { core.reset(); dropKey(); }
    bool coasting() const override { return true; }     // (nothing here waits for controlCore(): the core counts its own steps)
    void runKeyed(float* left, float* right, int n, const float*, const float*, float mixFrom, float mixTo) override
    {
        core.process(left, right, n, mixFrom, mixTo);
    }

    Core core;

private:
    bool thruOn = false;
};

} // namespace moddelay
