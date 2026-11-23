#pragma once

// The effects phase E6 moved onto shared cores, as they were at b4ee701, compiled beside the new ones
// so that old and new can be rendered with the same gesture and measured against each other (suite
// "Effect Cores Sweep"; spec-unify.md: "what must not be lost"). Nothing in the product includes
// this file. The text of each class is the old file's, unchanged, inside namespace e6old.

#include "../src/dsp/effects/Detector.h"
#include "../src/dsp/effects/FxLfo.h"
#include "../src/dsp/effects/ModDelay.h"
#include "../src/dsp/effects/DelayCore.h"
#include "../src/dsp/effects/PerfEffects.h"
#include "../src/dsp/effects/TrackFx.h"
#include "../src/dsp/filters/FilterSlot.h"
#include "../src/dsp/filters/SVFilter.h"
#include <array>
#include <vector>

namespace e6old
{
namespace fxb
{
using namespace ::fxb;

// Peak envelope follower with separate attack and release times (Zoelzer, "DAFX: Digital Audio
// Effects", dynamics chapter: peak level detector).
struct EnvelopeFollower
{
    void setTimes(double sr, float attackMs, float releaseMs)
    {
        att = 1.0f - std::exp(-1.0f / std::max(1.0f, attackMs * 0.001f * (float) sr));
        rel = 1.0f - std::exp(-1.0f / std::max(1.0f, releaseMs * 0.001f * (float) sr));
    }

    void reset() { env = 0.0f; }

    float process(float x)
    {
        float a = std::abs(x);
        env = flushDenormal(env + (a > env ? att : rel) * (a - env));
        return env;
    }

    float env = 0.0f;
    float att = 1.0f, rel = 1.0f;
};

// Circular delay line with fractional read. Memory is allocated in prepare() only.
// Call read...() before write() for the current sample: a delay of d then returns the sample
// written d calls ago. Linear read needs d >= 1, Hermite read needs d >= 2.
// Interpolation: 4-point, 3rd-order Hermite (Catmull-Rom) as in Olli Niemitalo, "Polynomial
// Interpolators for High-Quality Resampling of Oversampled Audio".
class DelayLine
{
public:
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

    bool isPrepared() const { return !buf.empty(); }
    float getMaxDelay() const { return maxDelay; }

    void write(float x)
    {
        buf[(size_t) w] = x;
        w = (w + 1) & mask;
    }

    float readLinear(float d) const
    {
        d = std::clamp(d, 1.0f, maxDelay);
        int di = (int) d;
        float fr = d - (float) di;
        float x0 = at(di), x1 = at(di + 1);
        return x0 + fr * (x1 - x0);
    }

    float readHermite(float d) const
    {
        d = std::clamp(d, 2.0f, maxDelay);
        int di = (int) d;
        float fr = d - (float) di;
        float xm1 = at(di - 1), x0 = at(di), x1 = at(di + 1), x2 = at(di + 2);
        float c1 = 0.5f * (x1 - xm1);
        float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * fr + c2) * fr + c1) * fr + x0;
    }

private:
    float at(int delay) const { return buf[(size_t) ((w - delay) & mask)]; }

    std::vector<float> buf;
    int mask = 0;
    int w = 0;
    float maxDelay = 1.0f;
};
} // namespace fxb

namespace perffx { inline float ceiling(float x) { return dly::ceiling(x); } }

// Wah: a resonant band-pass swept by a level's envelope (auto-wah) and / or an LFO (dsp/LFO.h,
// any of its shapes). Structure: Zoelzer, "DAFX", filters chapter (wah-wah = swept band-pass) with a
// peak envelope follower; the filter is the project's trapezoidal state-variable filter (SVFilter.h,
// Simper / Zavalishin), which stays tuned and stable while its cutoff is modulated every sample.
//
// FREQ  resting centre frequency
// RESO  0..1 -> Q 1..12 (output scaled so the peak gain is sqrt(Q): about 0 to +11 dB)
// SENS  envelope amount: a full-level input raises the centre by up to 4 octaves
// RATE  LFO rate; 0 = LFO off, otherwise the centre swings +/- one octave
// FX2:
// SHPE  the LFO's shape: SQR is two vowels in turn, RND a new one every cycle, USR the track's
//       eight-step shape as a filter sequence, EXPD a "wow" on every cycle
// KEY   (last cell) whose level SENS follows: SELF, or IN = the track's input (the source of the
//       track's IN page): another track's envelope opens the filter. With no input: as SELF.
//       The envelope is the shared detector's (Detector.h: dyn::Detector + dyn::Follower).
// The slot's WET leaves the dry sound beside the filtered one (1 = the filter alone).
// The cells are CellEffect's (every continuous one moves with modulateCell); FREQ, RESO and SENS
// glide to where they are put, so one thrown under sound is a sweep and not a click.
class Wah : public dyn::Keyed<Wah>
{
public:
    enum Param { Freq = 0, Reso, Sens, Rate };
    enum Extra { Shape = 0 };
    static constexpr int KEY_EXTRA = MAX_EXTRAS - 1;

    Wah() { seed(); }

    void setUserShape(const float* levels) override { lfo.setUserShape(levels); }

    bool keyFromInput() const { return cell(4 + KEY_EXTRA) >= 0.5f; }
    bool keyOutside() const { return keyFromInput(); }

    // Centre frequency the filter is at right now (after envelope and LFO), for meters and tests.
    float getCurrentFrequency() const { return currentHz; }

    // One sample (dyn::Keyed calls it): the key is the track's input with KEY at IN, else the sound itself.
    void tick(float& left, float& right, float keyLeft, float keyRight)
    {
        // The level SENS follows.
        const float env = std::min(1.0f, 2.0f * follower.up(detector.level(keyLeft, keyRight)));

        float octaves = sensSmooth.next(sens) * ENV_OCTAVES * env;
        const float lfoDepth = lfoDepthSmooth.next(rate > LFO_OFF ? 1.0f : 0.0f);
        if (lfoDepth > 0.0f)
            octaves += lfoDepth * lfo.next();

        float hz = freqSmooth.next(freq) * std::exp2(octaves);
        hz = std::clamp(hz, 60.0f, std::min(12000.0f, (float) sampleRate * 0.45f));
        currentHz = hz;

        const float q = qSmooth.next(q0);
        const float gain = 1.0f / std::sqrt(q);     // band output peaks at Q; leave sqrt(Q) of it

        filterL.setResonance(q);
        filterR.setResonance(q);
        filterL.setCutoff(hz);
        filterR.setCutoff(hz);

        const float inL = left, inR = right;
        left = inL + wet * (filterL.process(inL) * gain - inL);
        right = inR + wet * (filterR.process(inR) * gain - inR);

        // The filter's states are private: once input and output have both been at nothing for a
        // while, clear them so a ringing tail cannot linger in the denormal range.
        if (std::abs(inL) + std::abs(inR) + std::abs(left) + std::abs(right) < 1.0e-12f)
        {
            if (++quietCount >= QUIET_SAMPLES)
            {
                filterL.reset();
                filterR.reset();
                left = right = 0.0f;
                quietCount = QUIET_SAMPLES;
            }
        }
        else
        {
            quietCount = 0;
        }
    }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "FREQ", 0, 150.0f, 2500.0f, 400.0f, K::Log, "Hz" },
            { "RESO", 1, 0, 1, 0.5f },
            { "SENS", 2, 0, 1, 0.5f },
            { "RATE", 3, 0.0f, 10.0f, 0.0f, K::Linear, "Hz" },
            fxm::Lfo::shapeSpec("SHPE", Shape),
            {}, {},
            dyn::keySpec(KEY_EXTRA) };
        return c;
    }

    void prepareCore() override
    {
        for (auto* f : { &filterL, &filterR })
        {
            f->setSampleRate(sampleRate);
            f->setMode(SVFilter::Mode::BandPass);
        }
        detector.prepare(sampleRate);
        follower.setTimes(sampleRate, 3.0f, 150.0f);
        freqSmooth.setTime(sampleRate, 20.0f);
        lfoDepthSmooth.setTime(sampleRate, 30.0f);
        qSmooth.setTime(sampleRate, GLIDE_MS);
        sensSmooth.setTime(sampleRate, GLIDE_MS);
        lfo.prepare(sampleRate);
    }

    void resetCore() override
    {
        filterL.reset();
        filterR.reset();
        detector.reset();
        follower.reset(0.0f);
        dropKey();
        freqSmooth.snap(freq);
        qSmooth.snap(q0);
        sensSmooth.snap(sens);
        lfoDepthSmooth.snap(rate > LFO_OFF ? 1.0f : 0.0f);
        lfo.reset();
        quietCount = 0;
    }

    void configure(const float* v) override
    {
        freq = v[Freq];
        q0 = std::pow(12.0f, v[Reso]);
        sens = v[Sens];
        rate = v[Rate];
        lfo.setRate(rate);
        lfo.setShape((int) v[4 + Shape]);
    }

private:
    static constexpr float ENV_OCTAVES = 4.0f;
    static constexpr float LFO_OFF = 0.005f;
    static constexpr float GLIDE_MS = 10.0f;        // RESO and SENS
    static constexpr int QUIET_SAMPLES = 64;

    float freq = 400.0f;
    float q0 = 3.4641f;     // 12 ^ RESO
    float sens = 0.5f;
    float rate = 0.0f;

    SVFilter filterL, filterR;
    dyn::Detector detector;             // the key (its own signal or the track's input) as a level
    dyn::Follower follower;             // attack 3 ms, release 150 ms
    fxb::Smoother freqSmooth, lfoDepthSmooth, qSmooth, sensSmooth;
    fxm::Lfo lfo;
    float currentHz = 400.0f;
    int quietCount = 0;
};

// Allpass cascade phaser: two to six first-order allpass stages a side, their break frequency
// swept between 200 Hz and 4 kHz by an LFO (dsp/LFO.h, any of its shapes), with a feedback path.
// The notches come from the stages' output meeting the dry sound: the slot's WET starts at half.
//
// RATE, DPTH, FDBK, STGS; on the second page SHPE and SPRD (the right side's sweep up to half a
// cycle ahead of the left).
class Phaser : public CellEffect
{
public:
    enum Param
    {
        Rate = 0,       // 0.1 - 5 Hz
        Depth,          // 0 - 1
        Feedback,       // -0.9 to 0.9
        Stages          // 2 to 6
    };
    enum Extra { Shape = 0, Spread };

    Phaser()
    {
        mix = 0.5f;
        seed();
    }

    void setUserShape(const float* levels) override { lfo.setUserShape(levels); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "RATE", 0, 0.1f, 5.0f, 0.5f, K::Log, "Hz" },
            { "DPTH", 1, 0, 1, 0.5f },
            { "FDBK", 2, -0.9f, 0.9f, 0.3f },
            { "STGS", 3, 2, 6, 4, K::Int },
            fxm::Lfo::shapeSpec("SHPE", Shape),
            { "SPRD", Spread, 0, 1, 0.0f },
            {}, {} };
        return c;
    }

    void prepareCore() override
    {
        lfo.prepare(sampleRate);
        depthSmooth.setTime(sampleRate, 5.0f);
        fbSmooth.setTime(sampleRate, 5.0f);
        stageStep = 1.0f / (0.004f * (float) sampleRate);
    }

    void resetCore() override
    {
        for (auto& s : apStateL) s = 0.0f;
        for (auto& s : apStateR) s = 0.0f;
        feedbackL = 0.0f;
        feedbackR = 0.0f;
        lfo.reset();
        depthSmooth.snap(depth);
        fbSmooth.snap(feedback);
        stagesNow = (float) stages;
    }

    void configure(const float* v) override
    {
        lfo.setRate(v[Rate]);
        lfo.setShape((int) v[4 + Shape]);
        lfo.setOffset(0.5f * v[4 + Spread]);
        depth = v[Depth];
        feedback = v[Feedback];
        stages = std::clamp((int) v[Stages], 2, MAX_STAGES);
    }

    bool coasting() const override { return true; }     // (nothing here waits for controlCore(): the LFO counts its own steps)
    void runCore(float* left, float* right, int numSamples, float mixFrom, float mixTo) override
    {
        const float stagesGoal = (float) stages;
        const float mixSlope = (mixTo - mixFrom) / (float) numSamples;
        float m = mixFrom;
        // (Local copies for the loop: a write to the output could otherwise be a write to any of them.)
        std::array<float, MAX_STAGES> stateL = apStateL, stateR = apStateR;
        float fedL = feedbackL, fedR = feedbackR, count = stagesNow;
        for (int i = 0; i < numSamples; ++i)
        {
            const float dryL = left[i], dryR = right[i];
            float a, b;
            lfo.next(a, b);

            // The break frequency, between 200 Hz and 4 kHz.
            const float d = 0.5f * depthSmooth.next(depth), fb = fbSmooth.next(feedback);
            const float coeffL = computeAllpassCoeff(MIN_HZ + (MAX_HZ - MIN_HZ) * (a * d + 0.5f));
            const float coeffR = b != a ? computeAllpassCoeff(MIN_HZ + (MAX_HZ - MIN_HZ) * (b * d + 0.5f)) : coeffL;

            // The stages in use; a new count is crossed to over a few milliseconds, the last stage
            // coming in or going out by degrees.
            if (count != stagesGoal)
                count = dsp::towards(count, stagesGoal, stageStep);
            const int whole = (int) count;
            const float part = count - (float) whole;

            float wetL = dryL + fedL * fb, wetR = dryR + fedR * fb;
            for (int s = 0; s < whole; ++s)
            {
                wetL = processAllpass(wetL, stateL[(size_t) s], coeffL);
                wetR = processAllpass(wetR, stateR[(size_t) s], coeffR);
            }
            if (part > 0.0f)
            {
                wetL += part * (processAllpass(wetL, stateL[(size_t) whole], coeffL) - wetL);
                wetR += part * (processAllpass(wetR, stateR[(size_t) whole], coeffR) - wetR);
            }
            fedL = fxb::flushDenormal(std::clamp(wetL, -4.0f, 4.0f));
            fedR = fxb::flushDenormal(std::clamp(wetR, -4.0f, 4.0f));

            m += mixSlope;
            left[i]  = dryL * (1.0f - m) + wetL * m;
            right[i] = dryR * (1.0f - m) + wetR * m;
        }
        // (A stage left to die away ends at the smallest number there is and stays there for good.)
        for (int s = 0; s < MAX_STAGES; ++s)
        {
            apStateL[(size_t) s] = fxb::flushDenormal(stateL[(size_t) s]);
            apStateR[(size_t) s] = fxb::flushDenormal(stateR[(size_t) s]);
        }
        feedbackL = fedL;
        feedbackR = fedR;
        stagesNow = count;
    }

private:
    static constexpr int MAX_STAGES = 6;
    static constexpr float MIN_HZ = 200.0f, MAX_HZ = 4000.0f;

    // First-order allpass filter: y[n] = coeff * (x[n] - y[n-1]) + x[n-1]
    // state holds x[n-1] mixed with y[n-1]
    static float processAllpass(float input, float& state, float coeff)
    {
        float output = coeff * input + state;
        state = input - coeff * output;
        return output;
    }

    float computeAllpassCoeff(float freq) const
    {
        float w = dsp::PI_F * std::min(freq / (float)sampleRate, 0.45f);
        float t = dsp::fastTan(w);
        return (t - 1.0f) / (t + 1.0f);
    }

    std::array<float, MAX_STAGES> apStateL {};
    std::array<float, MAX_STAGES> apStateR {};
    float feedbackL = 0.0f;
    float feedbackR = 0.0f;

    fxm::Lfo lfo;
    fxb::Smoother depthSmooth, fbSmooth;
    float depth = 0.5f, feedback = 0.3f;
    int stages = 4;
    float stagesNow = 4.0f, stageStep = 0.005f;
};

// Insert-effect filter: the whole filter set (state-variable, SEM-style, ladder, diode, comb) on
// any track, whatever its engine. Its four parameters are frequency, resonance, type and mode, so
// each can be locked per step from the FX page (filter sweeps and switches in a pattern).


class FilterEffect : public EffectBase
{
public:
    enum Param { Frequency = 0, Resonance, Type, ModeParam };

    FilterEffect()
    {
        apply();
    }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        left.setSampleRate(sr);
        right.setSampleRate(sr);
        apply();
    }

    void reset() override
    {
        left.reset();
        right.reset();
    }

    void process(float& l, float& r) override
    {
        if (!enabled) return;
        float wetL = left.process(l);
        float wetR = right.process(r);
        l = l + mix * (wetL - l);
        r = r + mix * (wetR - r);
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        const auto model = FilterSlot::modelFromIndex(typeIndex);
        switch (index)
        {
            case 0: return { "CUT", 0, 20, 20000, 2000, K::Log, "Hz" };
            case 1: return { "RESO", 1, 0, 1, 0.2f };
            case 2: return { "TYPE", 2, 0, FilterSlot::NUM_MODELS - 2, 0, K::Choice, "", FilterSlot::modelNames(),
                             FilterSlot::NUM_MODELS - 1 };      // every type except OFF (use the ON cell)
            case 3: return { "MODE", 3, 0, (float) (FilterSlot::modeCount(model) - 1), 0, K::Choice, "",
                             FilterSlot::modeNames(model), FilterSlot::modeCount(model) };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Frequency: frequency = std::clamp(value, 20.0f, 20000.0f); break;
            case Resonance: resonance = std::clamp(value, 0.0f, 1.0f); break;
            case Type:
                typeIndex = std::clamp((int) std::lround(value), 0, FilterSlot::NUM_MODELS - 2);
                modeIndex = std::min(modeIndex, FilterSlot::modeCount(FilterSlot::modelFromIndex(typeIndex)) - 1);
                break;
            case ModeParam:
                modeIndex = std::clamp((int) std::lround(value), 0,
                                       FilterSlot::modeCount(FilterSlot::modelFromIndex(typeIndex)) - 1);
                break;
            default: return;
        }
        apply();
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Frequency: return frequency;
            case Resonance: return resonance;
            case Type:      return (float) typeIndex;
            case ModeParam: return (float) modeIndex;
            default:        return 0.0f;
        }
    }

private:
    void apply()
    {
        for (auto* f : { &left, &right })
        {
            f->setModel(0, FilterSlot::modelFromIndex(typeIndex));
            f->setMode(0, modeIndex);
            f->setCutoff(frequency);
            f->setResonance(resonance);
        }
    }

    FilterSlot left, right;
    float frequency = 2000.0f;
    float resonance = 0.2f;
    int typeIndex = 0;
    int modeIndex = 0;
};


// (moddelay::Effect as it was: the vibrato overrode its runCore.)
class OldModEffect : public CellEffect
{
public:
    void setUserShape(const float* levels) override { core.setUserShape(levels); }

protected:
    void resetCore() override { core.reset(); }
    bool coasting() const override { return true; }
    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override { core.process(left, right, n, mixFrom, mixTo); }

    moddelay::Core core;
};

// Vibrato: pitch modulation from one voice of the modulated delay (ModDelay.h), fully wet unless
// the slot's WET is turned down (the dry sound next to it makes a chorus). Textbook structure
// (Zoelzer, "DAFX", delay-based effects: vibrato; Dattorro, "Effect Design Part 2").
//
// DPTH is the peak pitch deviation in cents for the sine: the delay swing is derived from it and
// the rate (swing = deviation / (2 pi rate)), so the pitch depth stays the same when the rate
// changes. The swing is limited to 20 ms, which only matters for deep vibrato below about 0.5 Hz.
// SHPE is the shape of the DELAY, so the pitch is its slope: TRI is a trill between two pitches,
// SAW and RAMP a steady detune with a bend back once a cycle, SQR and RND bends from place to place.
//
// SPRD puts the right side's modulation up to half a cycle ahead of the left.
//
// FADE is a delayed-vibrato time: when the input comes up out of silence the depth starts at
// zero and grows to full over that time (an insert effect sees no note gates, so the onset is taken
// from an envelope follower; notes played legato do not restart it).
//
// With DPTH 0 the effect is a clean delay of BASE_DELAY samples.
class Vibrato : public OldModEffect
{
public:
    enum Param { Rate = 0, Depth, Spread, Fade };
    enum Extra { Shape = 0 };

    static constexpr int BASE_DELAY = 2;   // samples

    Vibrato() { seed(); }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "RATE", 0, 0.1f, 12.0f, 5.0f, K::Log, "Hz" },
            { "DPTH", 1, 0.0f, 100.0f, 25.0f, K::Linear, "ct" },
            { "SPRD", 2, 0, 1, 0.0f },
            { "FADE", 3, 0.0f, 2.0f, 0.0f, K::Linear, "s" },
            fxm::Lfo::shapeSpec("SHPE", Shape),
            {}, {}, {} };
        return c;
    }

    void prepareCore() override
    {
        core.setVoices(1);
        core.setRange(0.0f, 2.0f * MAX_SWING_SEC);
        core.prepare(sampleRate, 2.0 * MAX_SWING_SEC + 0.001, false);
        swingSmooth.setTime(sampleRate / CONTROL, 50.0f);
        fadeSmooth.setTime(sampleRate / CONTROL, 5.0f);
        follower.setTimes(sampleRate, 1.0f, 100.0f);
    }

    void resetCore() override
    {
        core.reset();
        follower.reset();
        gateOpen = false;
        fadeRamp = (fadeSec > 0.0f) ? 0.0f : 1.0f;
        fadeSmooth.snap(fadeRamp);
        swingSmooth.snap(targetSwing());
    }

    void configure(const float* v) override
    {
        rate = v[Rate];
        depthCents = v[Depth];
        fadeSec = v[Fade];
        core.setRate(rate);
        core.setStereo(0.5f * v[Spread]);    // right channel up to 180 degrees ahead
        core.setShape((int) v[4 + Shape]);
    }

    bool coasting() const override { return false; }    // (the swing is set at every look)

    // Every look: the swing for the pitch depth asked for, under the fade.
    void controlCore() override
    {
        const float swing = swingSmooth.next(targetSwing()) * fadeSmooth.next(fadeRamp);
        core.setDelay((float) BASE_DELAY, swing, swing);
    }

    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override
    {
        // Delayed vibrato: while the input is silent the depth is taken down to zero (unheard),
        // and it grows back once the input has come up again.
        for (int i = 0; i < n; ++i)
        {
            const float env = follower.process(0.5f * (left[i] + right[i]));
            if (! gateOpen && env > GATE_ON)
                gateOpen = true;
            else if (gateOpen && env < GATE_OFF)
                gateOpen = false;

            if (fadeSec <= 0.0f)
                fadeRamp = 1.0f;
            else if (! gateOpen)
                fadeRamp = 0.0f;
            else
                fadeRamp = std::min(1.0f, fadeRamp + 1.0f / (fadeSec * (float) sampleRate));
        }
        core.process(left, right, n, mixFrom, mixTo);
    }

private:
    static constexpr float MAX_SWING_SEC = 0.02f;
    static constexpr float GATE_ON = 0.01f;     // -40 dB
    static constexpr float GATE_OFF = 0.003f;   // -50 dB

    // Peak delay swing in seconds for the wanted pitch deviation: a delay d(t) = A (1 + sin wt)
    // changes the pitch by the factor 1 - d'(t), so the peak deviation is A w.
    float targetSwing() const
    {
        float ratio = std::exp2(depthCents / 1200.0f) - 1.0f;
        return std::min(MAX_SWING_SEC, ratio / (dsp::TWO_PI_F * rate));
    }

    float rate = 5.0f;
    float depthCents = 25.0f;
    float fadeSec = 0.0f;

    fxb::Smoother swingSmooth, fadeSmooth;
    fxb::EnvelopeFollower follower;
    bool gateOpen = false;
    float fadeRamp = 1.0f;
};

// ---- TALK ---------------------------------------------------------------------
// Three parallel band-pass filters placed on the first three formants of a vowel. The vowel
// position moves continuously through A - E - I - O - U; it can be set by hand, swept by an LFO
// and pushed by the level of the playing, so the sound "talks". Formant frequencies and relative
// levels are standard phonetics values for a male voice (as tabulated e.g. in the Csound manual).
class TalkBlock
{
public:
    enum Cell { Vowel = 0, Shift, Reso, Mix, LfoRate, LfoDepth, Env, Bite, NumCells };

    static ParamPage page()
    {
        ParamPage p;
        p.name = "TALK";
        p.section = "FX";
        const int b = TrackFxParam::TALK_BASE;
        p.params = {{
            { "VOWL", b + Vowel, 0, 1, 0 },                              // A .. E .. I .. O .. U
            { "SHFT", b + Shift, -1, 1, 0 },                             // formants down / up an octave
            { "RESO", b + Reso, 0, 1, 0.5f },                            // how sharp the formants are
            { "MIX",  b + Mix, 0, 1, 0 },                                // 0 = off
            { "RATE", b + LfoRate, 0.05f, 12, 1, ParamKind::Log, "Hz" }, // LFO on the vowel
            { "DPTH", b + LfoDepth, 0, 1, 0 },
            { "ENV",  b + Env, -1, 1, 0 },                               // playing level -> vowel
            { "BITE", b + Bite, 0, 1, 0.3f } }};                         // harmonics fed into the formants
        return p;
    }

    static const char* vowelName(float pos)
    {
        static const char* const names[] = { "A", "E", "I", "O", "U" };
        return names[juce::jlimit(0, 4, (int) std::lround(pos * 4.0f))];
    }

    TalkBlock()
    {
        for (auto& spec : page().params)
            values[(size_t) (spec.id - TrackFxParam::TALK_BASE)] = spec.defaultVal;
    }

    void prepare(double sr, int /*blockSize*/)
    {
        sampleRate = sr;
        for (auto& ch : filters)
            for (auto& f : ch)
            {
                f.setSampleRate(sr);
                f.setMode(SVFilter::Mode::BandPass);
                f.reset();
            }
        updateCountdown = 0;
    }

    void set(int cell, float v)
    {
        if (cell >= 0 && cell < NumCells)
            values[(size_t) cell] = page().params[(size_t) cell].clamp(v);
    }
    float get(int cell) const { return (cell >= 0 && cell < NumCells) ? values[(size_t) cell] : 0.0f; }

    bool isActive() const { return values[Mix] > 0.0f; }

    // Where the vowel is right now (0..1), including LFO and envelope, for display.
    float currentPosition() const { return position; }

    void process(float* left, float* right, int numSamples)
    {
        if (!isActive())
            return;

        const float mix = values[Mix];
        const float biteGain = 1.0f + values[Bite] * 6.0f;
        const float attack = 1.0f - std::exp(-1.0f / (0.005f * (float) sampleRate));
        const float release = 1.0f - std::exp(-1.0f / (0.150f * (float) sampleRate));

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = right[i];

            // Envelope follower on the input level.
            float level = 0.5f * (std::abs(inL) + std::abs(inR));
            envelope += (level > envelope ? attack : release) * (level - envelope);

            if (--updateCountdown <= 0)
            {
                updateCountdown = UPDATE_INTERVAL;
                lfoPhase += (double) values[LfoRate] * UPDATE_INTERVAL / sampleRate;
                lfoPhase -= std::floor(lfoPhase);
                float lfo = std::sin((float) lfoPhase * 6.2831853f);
                position = juce::jlimit(0.0f, 1.0f, values[Vowel]
                                                    + 0.5f * values[LfoDepth] * lfo
                                                    + values[Env] * juce::jmin(1.0f, envelope * 3.0f));
                updateFormants();
            }

            // A little saturation first gives the formants harmonics to pick out.
            float xL = dsp::fastTanh(inL * biteGain);
            float xR = dsp::fastTanh(inR * biteGain);

            float outL = 0.0f, outR = 0.0f;
            for (int f = 0; f < 3; ++f)
            {
                outL += filters[0][(size_t) f].process(xL) * gains[(size_t) f];
                outR += filters[1][(size_t) f].process(xR) * gains[(size_t) f];
            }

            left[i] = inL + mix * (outL * MAKEUP - inL);
            right[i] = inR + mix * (outR * MAKEUP - inR);
        }
    }

private:
    static constexpr int UPDATE_INTERVAL = 16;
    static constexpr float MAKEUP = 2.0f;

    void updateFormants()
    {
        //                               F1      F2      F3       levels (F2, F3 lifted for clarity)
        static const float table[5][6] = { { 650.0f, 1080.0f, 2650.0f, 1.0f, 0.70f, 0.50f },    // A
                                           { 400.0f, 1700.0f, 2600.0f, 1.0f, 0.45f, 0.40f },    // E
                                           { 290.0f, 1870.0f, 2800.0f, 1.0f, 0.40f, 0.30f },    // I
                                           { 400.0f,  800.0f, 2600.0f, 1.0f, 0.55f, 0.35f },    // O
                                           { 350.0f,  600.0f, 2700.0f, 1.0f, 0.35f, 0.30f } };  // U
        float p = position * 4.0f;
        int a = juce::jlimit(0, 3, (int) p);
        float t = p - (float) a;
        const float shift = std::exp2(values[Shift]);
        const float q = 3.0f + values[Reso] * values[Reso] * 27.0f;

        for (int f = 0; f < 3; ++f)
        {
            // Frequencies move on a log scale between neighbouring vowels.
            float hz = table[a][f] * std::pow(table[a + 1][f] / table[a][f], t) * shift;
            float amp = table[a][f + 3] + t * (table[a + 1][f + 3] - table[a][f + 3]);
            for (auto& ch : filters)
            {
                ch[(size_t) f].setResonance(q);
                ch[(size_t) f].setCutoff(juce::jlimit(60.0f, (float) (sampleRate * 0.45), hz));
            }
            // The band output peaks at Q: bring it back to the formant's level, with a little extra
            // loudness as the bands narrow.
            gains[(size_t) f] = amp / q * (1.0f + values[Reso]);
        }
    }

    std::array<float, NumCells> values {};
    std::array<std::array<SVFilter, 3>, 2> filters;
    std::array<float, 3> gains { 1.0f, 0.5f, 0.3f };
    double sampleRate = 44100.0;
    double lfoPhase = 0.0;
    float envelope = 0.0f;
    float position = 0.0f;
    int updateCountdown = 0;
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

} // namespace e6old
