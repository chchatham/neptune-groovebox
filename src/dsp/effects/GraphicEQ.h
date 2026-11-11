#pragma once

#include "EffectBase.h"
#include "FxBlocks.h"
#include <array>

// Four-band fixed-frequency equaliser, +/- 12 dB per band:
//   LOW   low shelf at 120 Hz
//   LMID  peak at 500 Hz
//   HMID  peak at 2 kHz
//   HIGH  high shelf at 6 kHz
// Each band is an RBJ cookbook biquad (FxBlocks.h). Gains are smoothed and the coefficients follow
// them at control rate, so turning a band does not click. All bands at 0 dB is unity gain.
class GraphicEQ : public EffectBase
{
public:
    enum Param { Low = 0, LowMid, HighMid, High };

    static constexpr int NUM_BANDS = 4;
    static constexpr float MAX_DB = 12.0f;

    static float bandFrequency(int band)
    {
        static constexpr float hz[NUM_BANDS] = { 120.0f, 500.0f, 2000.0f, 6000.0f };
        return hz[std::clamp(band, 0, NUM_BANDS - 1)];
    }

    GraphicEQ() { prepare(44100.0, 0); }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "LOW", 0, -MAX_DB, MAX_DB, 0.0f, K::Linear, "dB" };
            case 1: return { "LMID", 1, -MAX_DB, MAX_DB, 0.0f, K::Linear, "dB" };
            case 2: return { "HMID", 2, -MAX_DB, MAX_DB, 0.0f, K::Linear, "dB" };
            case 3: return { "HIGH", 3, -MAX_DB, MAX_DB, 0.0f, K::Linear, "dB" };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        if (id >= 0 && id < NUM_BANDS)
            gainDb[(size_t) id] = std::clamp(value, -MAX_DB, MAX_DB);
    }

    float getParameter(int id) const override
    {
        return (id >= 0 && id < NUM_BANDS) ? gainDb[(size_t) id] : 0.0f;
    }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        for (auto& s : smooth)
            s.setTime(sr, 20.0f);
        reset();
    }

    void reset() override
    {
        for (int b = 0; b < NUM_BANDS; ++b)
        {
            smooth[(size_t) b].snap(gainDb[(size_t) b]);
            updateBand(b, gainDb[(size_t) b]);
            filters[0][(size_t) b].reset();
            filters[1][(size_t) b].reset();
        }
        controlCounter = 0;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        bool update = (--controlCounter <= 0);
        if (update)
            controlCounter = CONTROL_INTERVAL;

        for (int b = 0; b < NUM_BANDS; ++b)
        {
            float g = smooth[(size_t) b].next(gainDb[(size_t) b]);
            if (update && std::abs(g - applied[(size_t) b]) > 1.0e-4f)
                updateBand(b, g);

            left = filters[0][(size_t) b].process(left);
            right = filters[1][(size_t) b].process(right);
        }
    }

private:
    static constexpr int CONTROL_INTERVAL = 16;

    void updateBand(int b, float db)
    {
        applied[(size_t) b] = db;
        for (auto& chan : filters)
        {
            auto& f = chan[(size_t) b];
            switch (b)
            {
                case 0:  f.setLowShelf(sampleRate, bandFrequency(0), 0.707, db); break;
                case 3:  f.setHighShelf(sampleRate, bandFrequency(3), 0.707, db); break;
                default: f.setPeaking(sampleRate, bandFrequency(b), 0.9, db); break;
            }
        }
    }

    std::array<float, NUM_BANDS> gainDb {};
    std::array<float, NUM_BANDS> applied {};
    std::array<fxb::Smoother, NUM_BANDS> smooth;
    std::array<std::array<fxb::Biquad, NUM_BANDS>, 2> filters;
    int controlCounter = 0;
};
