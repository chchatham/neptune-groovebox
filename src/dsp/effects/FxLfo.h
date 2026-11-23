#pragma once

// dsp/LFO.h as an effect reads it. What the effects with an LFO share (chorus, ensemble, flanger,
// vibrato, rotary speaker, tremolo, auto-pan, phaser, wah, ring modulator, and through them MODFX):
//
//   fxm::Lfo         every shape dsp/LFO.h has, stepped on the effects' clock (every
//                    CellEffect::CONTROL samples, counted from reset()) and joined by straight
//                    lines, with a second reader further on in the cycle (the other side of a
//                    stereo effect). It is set in Hz: an effect's SYNC turns its RATE cell into a
//                    division of the one table (dsp/TempoDivisions.h, FxSync in EffectBase.h), the
//                    same 22 a voice LFO offers, so this class keeps no list of its own;
//   fxm::Ramp        a value that reaches its goal in a straight line over one control step;
//   fxm::Levels      a level a side from one LFO: tremolo and auto-pan;
//   fxm::Onset       when a sound begins (dyn::Onset, Detector.h): what triggers an effect's LFO
//                    (dsp/LFO.h's trigger modes, its delay and its fade, for every effect LFO);
//   fxm::KeyedCells  an effect of cells that listens to a key a block at a time: the sound itself,
//                    or the track's input (KEY: SELF / IN).

#include "CellEffect.h"
#include "Detector.h"
#include "FxBlocks.h"
#include "../LFO.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace fxm
{

// ---- a straight line over one control step ----------------------------------------------------------
struct Ramp
{
    float now = 0.0f, inc = 0.0f, goal = 0.0f;

    void snap(float g) { now = goal = g; inc = 0.0f; }
    void aim(float g, float step) { now = goal; goal = g; inc = (g - now) * step; }
    float next() { return now += inc; }
};

// ---- the LFO ----------------------------------------------------------------------------------------
// One dsp/LFO.h, stepped every CellEffect::CONTROL samples with straight lines between: an
// effect's LFO runs at 20 Hz at most, and a shape that jumps (SQR, RND, USR) arrives over one
// step instead of in one sample. next() is the value for this sample; next(a, b) also gives the
// shape `offset` of a cycle further on.
class Lfo
{
public:
    static constexpr int INTERVAL = CellEffect::CONTROL;
    static constexpr int NUM_SHAPES = (int) LFO::Shape::COUNT;

    static LFO::Shape shapeOf(int index) { return (LFO::Shape) std::clamp(index, 0, NUM_SHAPES - 1); }

    // The SHAPE cell every one of these effects has.
    static ParamSpec shapeSpec(const char* label, int id, LFO::Shape initial = LFO::Shape::Sine)
    {
        return { label, id, 0, (float) (NUM_SHAPES - 1), (float) (int) initial, ParamKind::Choice, "", LFO::shapeLabels(), NUM_SHAPES };
    }

    void prepare(double sr)
    {
        lfo.setSampleRate(CellEffect::goodRate(sr) / (double) INTERVAL);
        tickSeconds = (double) INTERVAL / CellEffect::goodRate(sr);
        reset();
    }

    void reset()
    {
        lfo.reset();
        lfo.drawRandom();       // (RND and SRND move from the first cycle on)
        left = 0;
        primed = false;
        fadingSecond = holdingSecond = false;
        sinceTrigger = 0.0;
    }

    void setRate(float hz) { if (hz != lfo.getRate() && std::isfinite(hz)) lfo.setRate(std::max(0.0f, hz)); }
    void setShape(int index) { lfo.setShape(shapeOf(index)); }
    void setUserShape(const float* levels) { lfo.setUserShape(levels); }
    void setPhase(float cycles) { if (std::isfinite(cycles)) lfo.setStartPhase(cycles); }
    void setOffset(float cycles) { if (std::isfinite(cycles)) offset = cycles; }
    void setSeed(uint32_t seed) { lfo.setSeed(seed); }
    bool isUnipolar() const { return LFO::isUnipolar(lfo.getShape()); }

    // ---- triggered (dsp/LFO.h: FREE, KEY, ONE, HALF, HOLD; delay and fade from the trigger) --------
    static constexpr int NUM_TRIGGERS = (int) LFO::TriggerMode::COUNT;
    static ParamSpec triggerSpec(const char* label, int id)
    {
        return { label, id, 0, (float) (NUM_TRIGGERS - 1), 0, ParamKind::Choice, "", LFO::triggerLabels(), NUM_TRIGGERS };
    }
    void setTrigger(int mode, float delaySeconds, float fadeSeconds)
    {
        const auto m = (LFO::TriggerMode) std::clamp(mode, 0, NUM_TRIGGERS - 1);
        if (m != lfo.getTriggerMode()) { lfo.setTriggerMode(m); holdingSecond = false; }
        delay = std::isfinite(delaySeconds) ? std::max(0.0f, delaySeconds) : 0.0f;
        fade = std::isfinite(fadeSeconds) ? std::max(0.0f, fadeSeconds) : 0.0f;
        lfo.setDelayTime(delay);
        lfo.setFadeTime(fade);
        if (! listens()) fadingSecond = false;
    }
    // Whether a trigger would do anything (if not, nobody need listen for one).
    bool listens() const { return lfo.getTriggerMode() != LFO::TriggerMode::Free || delay > 0.0f || fade > 0.0f; }
    // A sound has begun. It is taken up at the LFO's next step (within a millisecond).
    void trigger()
    {
        lfo.trigger();
        sinceTrigger = 0.0;
        fadingSecond = delay > 0.0f || fade > 0.0f;
        if (lfo.getTriggerMode() == LFO::TriggerMode::Hold)
        {
            heldSecond = lfo.shapeAt(offset);
            holdingSecond = true;
        }
    }

    float next()
    {
        if (left <= 0) tick();
        --left;
        return a.next();
    }

    void next(float& first, float& second)
    {
        if (left <= 0) tick();
        --left;
        first = a.next();
        second = b.next();
    }

    // The value for this sample, and `n` samples taken at one go (for who reads it every n-th
    // sample: n divides INTERVAL, or a step of the LFO is late by what is left over).
    void take(int n, float& first, float& second)
    {
        if (left <= 0) tick();
        first = a.now + a.inc;
        second = b.now + b.inc;
        advance(n);
    }

    // For a loop that takes a whole run of samples at once: begin() begins a control step when the
    // last one is used up (true if it did); the lines then reach end() and endSecond() after
    // remaining() samples, and advance(n) says that n of them were taken.
    bool begin()
    {
        if (left > 0) return false;
        tick();
        return true;
    }
    int remaining() const { return left; }
    float end() const { return a.goal; }
    float endSecond() const { return b.goal; }
    void advance(int n)
    {
        left -= n;
        a.now += a.inc * (float) n;
        b.now += b.inc * (float) n;
    }

private:
    void tick()
    {
        const float va = lfo.process();
        const float vb = offset != 0.0f ? second() : va;
        if (primed) { a.aim(va, step); b.aim(vb, step); }
        else        { a.snap(va); b.snap(vb); primed = true; }
        left = INTERVAL;
    }

    // The second reader under the same trigger: held where the first is held, faded as it is
    // (LFO::process's own delay and fade, counted here once more for the place further on).
    float second()
    {
        float gain = 1.0f;
        if (fadingSecond)
        {
            const double t = sinceTrigger;
            sinceTrigger += tickSeconds;
            if (t < (double) delay)               gain = 0.0f;
            else if (t < (double) (delay + fade)) gain = (float) ((t - (double) delay) / (double) fade);
            else                                  fadingSecond = false;
        }
        return (holdingSecond ? heldSecond : lfo.shapeAt(offset)) * gain;
    }

    LFO lfo;
    Ramp a, b;
    static constexpr float step = 1.0f / (float) INTERVAL;
    float offset = 0.0f;
    int left = 0;
    bool primed = false;
    float delay = 0.0f, fade = 0.0f, heldSecond = 0.0f;
    double sinceTrigger = 0.0, tickSeconds = 32.0 / 44100.0;
    bool fadingSecond = false, holdingSecond = false;
};

// ---- when a sound begins ------------------------------------------------------------------------------
// What triggers an effect's LFO: the one onset detector, dyn::Onset (Detector.h), and dyn::Trigger,
// the onsets of a key over a run of samples. The modulation router listens to the track input with
// the same two (mod::Src ONST, and an LFO's TRIG at XKEY .. XHLD).
using Onset = dyn::Onset;
using Trigger = dyn::Trigger;

// The cells an effect's triggered LFO has beside its KEY: the delay and the fade from a trigger.
inline ParamSpec delaySpec(int id) { return { "DLAY", id, 0.0f, 2.0f, 0.0f, ParamKind::Linear, "s" }; }
inline ParamSpec fadeSpec(int id) { return { "FADE", id, 0.0f, 2.0f, 0.0f, ParamKind::Linear, "s" }; }

// ---- an effect of cells with a key --------------------------------------------------------------------
// The key is what an effect's detector listens to: the track's own sound (SELF), or the track's
// input (IN: the source chosen on the track's IN page, handed over by the AudioEngine before each
// block through EffectBase::setInput). dyn::Keyed (Detector.h) is the same promise for an effect
// that works a sample at a time; this one hands the key over with each run of samples, so a core
// can keep its loop to itself.
// With the key at IN and nothing handed over (no source chosen, a PERF pad, a send, the master)
// the key is the effect's own sound; keyConnected() says which. A key shorter than the block keys
// its own length, the rest keys itself. The pointers are used for one call and dropped.
class KeyedCells : public CellEffect
{
public:
    int keySource() const override
    {
        const int c = keyCell();
        return c >= 0 ? dyn::keySourceOf(cell(c)) : KEY_SELF;
    }
    void setInput(const float* left, const float* right, int frames) override
    {
        if (keySource() != KEY_SELF) pending.set(left, right, frames); else pending.take();
    }
    bool keyConnected() const { return connected; }

    void process(float& left, float& right) override
    {
        if (pending.frames != 0 || connected) dropKey();
        CellEffect::process(left, right);
    }

    void processBlock(float* left, float* right, int numSamples) override
    {
        key = pending.take();
        connected = key.left != nullptr && keySource() != KEY_SELF;
        if (! connected) key = {};
        CellEffect::processBlock(left, right, numSamples);
        key = {};
    }

protected:
    // The cell that holds the key choice (dyn::keySpec: SELF, a track by number, IN); -1 = none.
    virtual int keyCell() const = 0;
    // `n` samples in place, as CellEffect::runCore; keyLeft / keyRight are what is listened to for
    // them (the sound itself where there is no key from outside: a sample's key is read before the
    // sample is written).
    virtual void runKeyed(float* left, float* right, int n, const float* keyLeft, const float* keyRight, float mixFrom, float mixTo) = 0;

    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) final
    {
        const int keyed = std::min(n, key.frames);
        if (keyed <= 0)
        {
            runKeyed(left, right, n, left, right, mixFrom, mixTo);
            return;
        }
        const float mixMid = keyed == n ? mixTo : mixFrom + (mixTo - mixFrom) * (float) keyed / (float) n;
        runKeyed(left, right, keyed, key.left, key.right, mixFrom, mixMid);
        key.left += keyed;
        key.right += keyed;
        key.frames -= keyed;
        if (keyed < n)
            runKeyed(left + keyed, right + keyed, n - keyed, left + keyed, right + keyed, mixMid, mixTo);
    }

    void dropKey() { pending.take(); key = {}; connected = false; }

private:
    dyn::KeyInput pending, key;
    bool connected = false;
};

// ---- two levels from one LFO -------------------------------------------------------------------------
// What tremolo and auto-pan are: a level a side that goes where the LFO's two readers put it
// (`want`), rounded over a millisecond (the steps of SQR, RND and USR, a turned DPTH), under the
// mix it is handed (a line over the samples of the call). Levels are stepped with the LFO and
// joined by straight lines, so the loop over the samples is two multiplications a sample.
class Levels
{
public:
    Lfo lfo;
    Trigger trigger;        // the key's onsets, for the LFO's trigger modes

    void prepare(double sr)
    {
        lfo.prepare(sr);
        trigger.prepare(CellEffect::goodRate(sr));
        const double ticks = CellEffect::goodRate(sr) / (double) Lfo::INTERVAL;
        roundL.setTime(ticks, 1.0f);
        roundR.setTime(ticks, 1.0f);
        reset();
    }

    void reset()
    {
        lfo.reset();
        trigger.reset();
        fresh = true;
    }

    // The three cells of a triggered LFO (TRIG, DLAY, FADE), as their values.
    void setTrigger(float mode, float delaySeconds, float fadeSeconds) { lfo.setTrigger((int) mode, delaySeconds, fadeSeconds); }

    // want(a, b, levelL, levelR): the levels for the LFO at a (left) and b (right).
    // keyLeft / keyRight: what the LFO's trigger listens to (it may be the sound itself).
    template <typename Want>
    void process(float* left, float* right, int numSamples, const float* keyLeft, const float* keyRight, float mixFrom, float mixTo, Want&& want)
    {
        const bool listening = lfo.listens();
        const float mi = (mixTo - mixFrom) / (float) std::max(1, numSamples);
        for (int done = 0; done < numSamples;)
        {
            if (lfo.begin())
            {
                float wantL = 1.0f, wantR = 1.0f;
                want(lfo.end(), lfo.endSecond(), wantL, wantR);
                if (fresh)
                {
                    gainL.snap(roundL.z = wantL);
                    gainR.snap(roundR.z = wantR);
                    fresh = false;
                }
                else
                {
                    gainL.aim(roundL.next(wantL), step);
                    gainR.aim(roundR.next(wantR), step);
                }
            }
            const int n = std::min(lfo.remaining(), numSamples - done);
            float* l = left + done;
            float* r = right + done;
            const bool begun = listening && trigger.heard(keyLeft + done, keyRight + done, n);
            // (Each sample from the start of the run, not from the one before: several at once.)
            const float l0 = gainL.now - 1.0f, li = gainL.inc, r0 = gainR.now - 1.0f, ri = gainR.inc;
            const float m0 = mixFrom + mi * (float) done;
            for (int i = 0; i < n; ++i)
            {
                const float k = (float) (i + 1), m = m0 + mi * k;
                l[i] *= 1.0f + m * (l0 + li * k);
                r[i] *= 1.0f + m * (r0 + ri * k);
            }
            gainL.now += li * (float) n;
            gainR.now += ri * (float) n;
            lfo.advance(n);
            if (begun) lfo.trigger();
            done += n;
        }
    }

private:
    fxb::Smoother roundL, roundR;
    Ramp gainL, gainR;
    static constexpr float step = 1.0f / (float) Lfo::INTERVAL;
    bool fresh = true;
};

} // namespace fxm
