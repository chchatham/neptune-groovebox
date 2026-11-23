#pragma once

// DIRT as an insert effect (CellEffect): drive / distortion type / bit crusher / sample-rate
// reducer and a filtered, tuned feedback path, all in one. Formerly the DirtBlock of TrackFx.h;
// now an ordinary insert type.
//
// FX page:  DRIV (drive), TYPE (saturation curve), BITS (bit crusher), RATE (sample-rate reducer)
// FX2 page: FDBK (feedback amount), TUNE (feedback pitch), DAMP (feedback damping), MIX
//
// Three stages in a row, and a stage that is set to nothing is not run (as the block had it):
// the shaper at DRIV 0, the reducer at BITS 0 and RATE 0, the combs at FDBK 0. The shaper at
// DRIV 0 is not a wire (its curve, its tone filter and its DC filter still colour the sound), so
// "bits only" is the reducer alone; and a stage that is skipped costs nothing.

#include "CellEffect.h"
#include "Waveshaper.h"
#include "RateReducer.h"
#include "../filters/CombFilter.h"
#include "../Saturation.h"
#include <array>
#include <cmath>

class DirtInsert : public CellEffect
{
public:
    enum Cell { Drive = 0, Type, Bits, Rate, Feedback, Tune, Damp, Mix, NumCells };

    DirtInsert() { seed(); }

    const ParamSpec* cells() const override { return dirtTable(); }
    int mixCell() const override { return Mix; }

protected:
    void configure(const float* v) override
    {
        shaper.setParameter(Waveshaper::Drive, v[Drive]);
        shaper.setParameter(Waveshaper::Curve, v[Type]);
        shaper.setParameter(Waveshaper::Tone, 0.8f);
        shaper.setParameter(Waveshaper::Asymmetry, 0.5f);
        shaper.setParameter(Waveshaper::Mix, 1.0f);

        reducer.setParameter(RateReducer::RateReduction, v[Rate]);
        reducer.setParameter(RateReducer::BitDepth, v[Bits]);
        reducer.setParameter(RateReducer::PreFilter, 0.0f);
        reducer.setParameter(RateReducer::Mix, 1.0f);

        for (auto* c : { &combL, &combR })
        {
            c->setCutoff(v[Tune]);
            c->setResonance(v[Feedback]);
            c->setDamping(v[Damp]);
        }

        shaping = v[Drive] > 0.0f;
        reducing = v[Bits] > 0.0f || v[Rate] > 0.0f;
        ringing = v[Feedback] > 0.0f;
        active = shaping || reducing || ringing;
    }

    void prepareCore() override
    {
        shaper.prepare(sampleRate, 0);
        reducer.prepare(sampleRate, 0);
        combL.setSampleRate(sampleRate);
        combR.setSampleRate(sampleRate);
    }

    void resetCore() override
    {
        shaper.reset();
        reducer.reset();
        combL.reset();
        combR.reset();
    }

    bool coasting() const override { return !active; }

    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override
    {
        if (!active)
        {
            // Nothing to do when inactive: just apply mix (which is a no-op when mix = 1 and there
            // was no processing, but we still need to honor the mix parameter for dry/wet blend).
            // CellEffect runs us at mix 1 in a chain (wet only), so this is a pass-through.
            return;
        }

        // Keep a copy of dry sound for mixing.
        for (int i = 0; i < n; ++i)
        {
            dryBuf[0][(size_t) i] = left[i];
            dryBuf[1][(size_t) i] = right[i];
        }

        // Waveshaper (drive + curve)
        if (shaping)
            shaper.processBlock(left, right, n);

        // Rate reducer (bits + rate)
        if (reducing)
            reducer.processBlock(left, right, n);

        // Comb feedback
        if (ringing)
            for (int i = 0; i < n; ++i)
            {
                left[i] = combL.process(left[i]);
                right[i] = combR.process(right[i]);
            }

        // Dry/wet mix
        if (mixFrom < 0.999f || mixTo < 0.999f)
        {
            const float step = n > 1 ? (mixTo - mixFrom) / (float) (n - 1) : 0.0f;
            float m = mixFrom;
            for (int i = 0; i < n; ++i)
            {
                left[i] = dryBuf[0][(size_t) i] + m * (left[i] - dryBuf[0][(size_t) i]);
                right[i] = dryBuf[1][(size_t) i] + m * (right[i] - dryBuf[1][(size_t) i]);
                m += step;
            }
        }
    }

private:
    static const ParamSpec* dirtTable()
    {
        static const ParamSpec t[8] = {
            { "DRIV", 0, 0, 1, 0 },
            { "TYPE", 1, 0, (float) (sat::Count - 1), 0, ParamKind::Choice, "", sat::names(), sat::Count },
            { "BITS", 2, 0, 1, 0 },
            { "RATE", 3, 0, 1, 0 },
            { "FDBK", 0, 0, 1, 0 },
            { "TUNE", 1, 30, 4000, 220, ParamKind::Log, "Hz" },
            { "DAMP", 2, 0, 1, 0.3f },
            { "MIX",  3, 0, 1, 1 }
        };
        return t;
    }

    Waveshaper shaper;
    RateReducer reducer;
    CombFilter combL, combR;
    bool active = false;
    bool shaping = false, reducing = false, ringing = false;    // the stages that are set to something
    std::array<std::array<float, CONTROL>, 2> dryBuf {};  // runCore is called with at most CONTROL samples
};
