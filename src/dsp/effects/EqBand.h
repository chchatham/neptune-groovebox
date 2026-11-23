#pragma once

// One band of an equaliser (spec-unify.md, phase E3): the parametric EQ, the graphic EQ and the
// amp's tone stack are rows of these. A band is a stereo pair of fxb::Biquad (RBJ cookbook) whose
// gain, frequency and Q glide to what they are set to (so a turn, or a modulation, does not click:
// a step every 16 samples, the coefficients in a straight line between two steps),
// and which, while it rests at 0 dB, costs four comparisons a sample (process) or nothing (processBands).

#include "FxBlocks.h"
#include <cmath>
#include <algorithm>
#include <array>
#include <limits>

namespace fxb
{

struct EqBand
{
    enum Shape { LowShelf = 0, Peak, HighShelf };

    static constexpr float FLAT_DB = 0.01f;         // a band set within this of 0 dB is not run
    static constexpr int CONTROL_INTERVAL = 16;     // samples between two sets of coefficients in a glide
    static constexpr float GLIDE_MS = 20.0f;

    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        // One step of the glide is CONTROL_INTERVAL samples long.
        step = 1.0f - std::exp(-(float) CONTROL_INTERVAL / std::max(1.0f, GLIDE_MS * 0.001f * (float) sampleRate));
        reset();
    }

    // Empties the filters; the band is at its settings at once when it next runs.
    void reset()
    {
        filter[0].reset();
        filter[1].reset();
        db = FRESH;
    }

    void setShape(Shape s) { if (s != shape) { shape = s; db = FRESH; } }
    void set(float hz, float q, float db) { setFrequency(hz); setQ(q); setGain(db); }
    void setGain(float db)      { if (std::isfinite(db)) targetDb = std::clamp(db, -48.0f, 48.0f); }
    void setFrequency(float hz) { if (std::isfinite(hz)) targetHz = std::clamp(hz, 10.0f, 30000.0f); }
    void setQ(float q)          { if (std::isfinite(q))  targetQ = std::clamp(q, 0.05f, 40.0f); }

    float getGain() const { return targetDb; }
    float getFrequency() const { return targetHz; }
    float getQ() const { return targetQ; }
    // Where the glide is now.
    float currentGain() const { return db == db ? db : targetDb; }
    float currentFrequency() const { return hz; }
    bool isFlat() const { return flat; }

    void process(float& left, float& right)
    {
        if (moving())
            glide();
        if (flat) return;
        left = filter[0].process(left);
        right = filter[1].process(right);
    }

    // At rest at 0 dB: process() would do nothing.
    bool idle() const { return flat && ! moving(); }

private:
    bool moving() const { return ramp > 0 || targetDb != db || targetHz != hz || targetQ != q; }

    struct Coefficients { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };

    static float towards(float now, float target, float share, float close)
    {
        const float d = target - now;
        return std::abs(d) <= close ? target : now + share * d;
    }

    // The same by ratio, for a frequency or a Q (both above 0): a share of the octaves that are left.
    // (In a line, a Q thrown from 0.1 to 20 was four times itself after the first step.)
    static float towardsRatio(float now, float target, float share)
    {
        return std::abs(target - now) <= 0.0005f * target ? target : now * std::pow(target / now, share);
    }

    // One sample of the way to the settings. Every CONTROL_INTERVAL samples gain, frequency and Q
    // take a step and the coefficients of that step are worked out; in between, the coefficients
    // move to them in a straight line (between two stable filters every point is stable). A step
    // put on at once is a click of its own at low frequencies under a shelf, whose b0 is large: a
    // HIGH turned end to end under a bass note crackled.
    void glide()
    {
        if (db != db) { arrive(); return; }         // fresh: no glide from nowhere
        if (ramp == 0)
        {
            if (flat && std::abs(targetDb) < FLAT_DB) { arrive(); return; }     // nothing to hear: no glide
            db = towards(db, targetDb, step, 0.002f);
            hz = towardsRatio(hz, targetHz, step);
            q = towardsRatio(q, targetQ, step);
            if (flat)
            {
                flat = false;               // it starts from rest, as a wire
                filter[0].reset();
                filter[1].reset();
                put({});
            }
            aim = design();
            const auto& f = filter[0];
            const float share = 1.0f / (float) CONTROL_INTERVAL;
            inc = { (aim.b0 - f.b0) * share, (aim.b1 - f.b1) * share, (aim.b2 - f.b2) * share,
                    (aim.a1 - f.a1) * share, (aim.a2 - f.a2) * share };
            ramp = CONTROL_INTERVAL;
        }
        if (--ramp > 0)
        {
            const auto& f = filter[0];
            put({ f.b0 + inc.b0, f.b1 + inc.b1, f.b2 + inc.b2, f.a1 + inc.a1, f.a2 + inc.a2 });
            return;
        }
        put(aim);
        if (restsFlat())
        {
            flat = true;        // at rest at 0 dB the band is not there (a band of 0 dB is a wire)
            filter[0].reset();
            filter[1].reset();
        }
    }

    void arrive()
    {
        db = targetDb;
        hz = targetHz;
        q = targetQ;
        ramp = 0;
        flat = restsFlat();
        if (! flat) put(design());
    }

    bool restsFlat() const { return db == targetDb && std::abs(db) < FLAT_DB; }

    Coefficients design() const
    {
        Biquad f;
        switch (shape)
        {
            case LowShelf:  f.setLowShelf(sampleRate, hz, q, db); break;
            case HighShelf: f.setHighShelf(sampleRate, hz, q, db); break;
            default:        f.setPeaking(sampleRate, hz, q, db); break;
        }
        return { f.b0, f.b1, f.b2, f.a1, f.a2 };
    }

    void put(const Coefficients& c)
    {
        for (auto& f : filter)
        {
            f.b0 = c.b0; f.b1 = c.b1; f.b2 = c.b2;
            f.a1 = c.a1; f.a2 = c.a2;
        }
    }

    double sampleRate = 44100.0;
    Shape shape = Peak;
    float targetDb = 0.0f, targetHz = 1000.0f, targetQ = 0.707f;
    // (FRESH = not a number: "not run since reset()", which makes the first process() call arrive.)
    static constexpr float FRESH = std::numeric_limits<float>::quiet_NaN();
    float db = FRESH, hz = 1000.0f, q = 0.707f;
    float step = 1.0f;
    Coefficients aim, inc;      // where the coefficients are going, and how far each sample
    int ramp = 0;               // samples left of that
    bool flat = true;
    Biquad filter[2];
};

// A row of bands over a block: the samples process() gives one by one, band after band. The bands
// that rest at 0 dB when the block begins are left out of it (a flat equaliser costs nothing).
// Sample by sample, not band by band: each band is a recursion that waits for its own last sample,
// and several of them side by side are worked on at once (measured: three bands, 5.3 against 14 us).
template <size_t N>
inline void processBands(std::array<EqBand, N>& bands, float* left, float* right, int numSamples)
{
    std::array<EqBand*, N> busy;
    size_t count = 0;
    for (auto& band : bands)
        if (! band.idle())
            busy[count++] = &band;
    if (count == N)                         // (all of them: without the detour, a tenth faster)
        for (int i = 0; i < numSamples; ++i)
            for (auto& band : bands)
                band.process(left[i], right[i]);
    else if (count > 0)
        for (int i = 0; i < numSamples; ++i)
            for (size_t b = 0; b < count; ++b)
                busy[b]->process(left[i], right[i]);
}

} // namespace fxb
