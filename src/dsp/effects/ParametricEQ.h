#pragma once

#include "CellEffect.h"
#include "EqBand.h"
#include <cmath>
#include <array>

// Parametric EQ, EQ (and the master section's): low shelf, three peaks, high shelf, each a
// fxb::EqBand (EqBand.h: RBJ cookbook biquads that glide to their settings and are not run at
// 0 dB). The cells reach the two shelves and the middle peak; the two outer peaks (400 Hz and
// 4 kHz, flat) are there for setParametric.
//
//   FX    LOW   low shelf gain          MID   middle peak gain
//         MFRQ  middle peak frequency   HIGH  high shelf gain
//   FX2   LFRQ  where the low shelf turns      MIDQ  width of the middle peak (0.1 wide .. 20 narrow)
//         HFRQ  where the high shelf turns
//
// The cells are CellEffect's: every one moves with modulateCell, and glides (a swept MFRQ is a filter sweep, not a staircase).
// Flat, the sound passes untouched, sample for sample: the master EQ of a project that has not
// set it costs nothing and is part of the "P01 Keyboard" pin.
// The slot's WET is not used.
class ParametricEQ : public CellEffect
{
public:
    enum Cell { LowCell = 0, MidCell, MidFreqCell, HighCell, LowFreqCell, MidQCell, HighFreqCell };

    // Parameter indices matching the spec
    enum Param
    {
        LowFreq = 0,
        LowGain,
        Mid1Freq,
        Mid1Gain,
        Mid1Q,
        Mid2Freq,
        Mid2Gain,
        Mid2Q,
        Mid3Freq,
        Mid3Gain,
        Mid3Q,
        HighFreq,
        HighGain,
        NumParams
    };

    ParametricEQ()
    {
        bands[0].setShape(fxb::EqBand::LowShelf);
        bands[4].setShape(fxb::EqBand::HighShelf);
        seed();
    }

    // ---- by name (the master section, tests) -------------------------------------------------
    // Low shelf: frequency (Hz), gain (dB)
    void setLowShelf(float freq, float gainDb)
    {
        if (! std::isfinite(freq) || ! std::isfinite(gainDb)) return;
        setCell(LowFreqCell, freq);
        setCell(LowCell, gainDb);
    }

    // High shelf: frequency (Hz), gain (dB)
    void setHighShelf(float freq, float gainDb)
    {
        if (! std::isfinite(freq) || ! std::isfinite(gainDb)) return;
        setCell(HighFreqCell, freq);
        setCell(HighCell, gainDb);
    }

    // Parametric band (index 0-2): frequency, gain (dB), Q. The middle one is the cells' (held to
    // their ranges); the outer two have no cell and are set here alone.
    void setParametric(int idx, float freq, float gainDb, float q)
    {
        if (idx < 0 || idx > 2) return;
        if (! std::isfinite(freq) || ! std::isfinite(gainDb) || ! std::isfinite(q)) return;
        if (idx == 1)
        {
            setCell(MidFreqCell, freq);
            setCell(MidCell, gainDb);
            setCell(MidQCell, q);
            return;
        }
        outer[(size_t) (idx / 2)] = { std::clamp(freq, 20.0f, 20000.0f), std::clamp(gainDb, -18.0f, 18.0f), std::clamp(q, 0.1f, 20.0f) };
        touch();
    }

    float getParam(int p) const
    {
        switch (p)
        {
            case LowFreq:  return cell(LowFreqCell);
            case LowGain:  return cell(LowCell);
            case Mid2Freq: return cell(MidFreqCell);
            case Mid2Gain: return cell(MidCell);
            case Mid2Q:    return cell(MidQCell);
            case HighFreq: return cell(HighFreqCell);
            case HighGain: return cell(HighCell);
            case Mid1Freq: case Mid1Gain: case Mid1Q: return outer[0][(size_t) (p - Mid1Freq)];
            case Mid3Freq: case Mid3Gain: case Mid3Q: return outer[1][(size_t) (p - Mid3Freq)];
            default:       return 0.0f;
        }
    }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "LOW", 0, -18, 18, 0, K::Linear, "dB" },
            { "MID", 1, -18, 18, 0, K::Linear, "dB" },
            { "MFRQ", 2, 100, 8000, 1000, K::Log, "Hz" },
            { "HIGH", 3, -18, 18, 0, K::Linear, "dB" },
            { "LFRQ", 0, 20, 2000, 80, K::Log, "Hz" },
            { "MIDQ", 1, 0.1f, 20, 1, K::Log, "" },
            { "HFRQ", 2, 2000, 20000, 8000, K::Log, "Hz" },
            {} };
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

    // The bands' targets from the cells (modulation included) and the two outer peaks.
    void configure(const float* v) override
    {
        bands[0].set(v[LowFreqCell], SHELF_Q, v[LowCell]);
        bands[1].set(outer[0][0], outer[0][2], outer[0][1]);
        bands[2].set(v[MidFreqCell], v[MidQCell], v[MidCell]);
        bands[3].set(outer[1][0], outer[1][2], outer[1][1]);
        bands[4].set(v[HighFreqCell], SHELF_Q, v[HighCell]);
    }

    bool coasting() const override { return true; }     // (nothing here waits for controlCore(): the bands glide by themselves)
    void runCore(float* left, float* right, int n, float, float) override
    {
        fxb::processBands(bands, left, right, n);
    }

private:
    static constexpr int NUM_BANDS = 5;
    static constexpr float SHELF_Q = 0.707f;

    std::array<fxb::EqBand, NUM_BANDS> bands;
    std::array<std::array<float, 3>, 2> outer {{ { 400.0f, 0.0f, 1.0f }, { 4000.0f, 0.0f, 1.0f } }};    // frequency, gain, Q
};
