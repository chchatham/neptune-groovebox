#pragma once

#include "CellEffect.h"
#include "EqBand.h"
#include <array>

// Four-band equaliser, GEQ, +/- 12 dB per band, each band a fxb::EqBand (EqBand.h: an RBJ cookbook
// biquad pair that glides to its settings, so turning a band does not click, and is not run at 0 dB):
//   LOW   low shelf, 120 Hz to start with        LMID  peak, 500 Hz
//   HMID  peak, 2 kHz                            HIGH  high shelf, 6 kHz
// On FX2 each band's frequency can be moved (LFRQ, LMFQ, HMFQ, HFRQ): four gains over four
// frequencies. All bands at 0 dB is the sound untouched. The cells are
// CellEffect's: every one moves with modulateCell.
// The slot's WET is not used (an equaliser beside the dry sound is a weaker equaliser).
class GraphicEQ : public CellEffect
{
public:
    enum Param { Low = 0, LowMid, HighMid, High };

    static constexpr int NUM_BANDS = 4;
    static constexpr float MAX_DB = 12.0f;

    // Where band b sits unless its frequency cell says otherwise.
    static float bandFrequency(int band)
    {
        static constexpr float hz[NUM_BANDS] = { 120.0f, 500.0f, 2000.0f, 6000.0f };
        return hz[std::clamp(band, 0, NUM_BANDS - 1)];
    }

    GraphicEQ()
    {
        bands[0].setShape(fxb::EqBand::LowShelf);
        bands[3].setShape(fxb::EqBand::HighShelf);
        seed();
    }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "LOW", 0, -MAX_DB, MAX_DB, 0.0f, K::Linear, "dB" },
            { "LMID", 1, -MAX_DB, MAX_DB, 0.0f, K::Linear, "dB" },
            { "HMID", 2, -MAX_DB, MAX_DB, 0.0f, K::Linear, "dB" },
            { "HIGH", 3, -MAX_DB, MAX_DB, 0.0f, K::Linear, "dB" },
            { "LFRQ", 0, 40.0f, 400.0f, bandFrequency(0), K::Log, "Hz" },
            { "LMFQ", 1, 150.0f, 1500.0f, bandFrequency(1), K::Log, "Hz" },
            { "HMFQ", 2, 600.0f, 6000.0f, bandFrequency(2), K::Log, "Hz" },
            { "HFRQ", 3, 2000.0f, 16000.0f, bandFrequency(3), K::Log, "Hz" } };
        return c;
    }

    void prepareCore() override
    {
        for (auto& band : bands)
            band.prepare(sampleRate);
    }

    void resetCore() override
    {
        for (auto& band : bands)
            band.reset();
    }

    void configure(const float* v) override
    {
        for (int b = 0; b < NUM_BANDS; ++b)
            bands[(size_t) b].set(v[NUM_BANDS + b], (b == 0 || b == NUM_BANDS - 1) ? 0.707f : 0.9f, v[b]);
    }

    bool coasting() const override { return true; }     // (nothing here waits for controlCore(): the bands glide by themselves)
    void runCore(float* left, float* right, int n, float, float) override
    {
        fxb::processBands(bands, left, right, n);
    }

private:
    std::array<fxb::EqBand, NUM_BANDS> bands;
};
