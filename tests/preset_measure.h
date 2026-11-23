#pragma once

#include "fast_measure.h"
#include <algorithm>
#include <cmath>
#include <vector>

// What the preset suites ("Factory Multis", "Fusion Presets") measure of a render: levels,
// brightness, the power at a pitch. The loops that cost are fast_measure.cpp's.
namespace presetmeasure
{
    inline float dB(double ratio) { return (float) (20.0 * std::log10(std::max(ratio, 1.0e-9))); }
    inline float powerDb(double ratio) { return (float) (10.0 * std::log10(std::max(ratio, 1.0e-18))); }

    inline float rms(const std::vector<float>& x, size_t from = 0, size_t count = (size_t) -1)
    {
        from = std::min(from, x.size());
        count = std::min(count, x.size() - from);
        if (count == 0)
            return 0.0f;
        double sum = 0.0;
        for (size_t i = from; i < from + count; ++i)
            sum += (double) x[i] * x[i];
        return (float) std::sqrt(sum / (double) count);
    }

    inline float peak(const std::vector<float>& x)
    {
        float p = 0.0f;
        for (float s : x)
            p = std::max(p, std::abs(s));
        return p;
    }

    inline bool finite(const std::vector<float>& x)
    {
        for (float s : x)
            if (! std::isfinite(s))
                return false;
        return true;
    }

    // The part of x from `from` to `to` seconds.
    inline std::vector<float> part(const std::vector<float>& x, double from, double to, double sampleRate)
    {
        const size_t a = std::min(x.size(), (size_t) std::max(0.0, from * sampleRate));
        const size_t b = std::min(x.size(), (size_t) std::max(0.0, to * sampleRate));
        return b > a ? std::vector<float>(x.begin() + (long) a, x.begin() + (long) b) : std::vector<float>();
    }

    // The energy-weighted mean frequency, Hz.
    inline float centroid(const std::vector<float>& x, double sampleRate)
    {
        float level = 0.0f, hz = 0.0f;
        if (x.size() >= 2048)
            fastmeasure::rmsAndCentroid(x, 2048, sampleRate, level, hz);
        return hz;
    }

    // The power of the whole of x at `hz` (Hann window).
    inline double tone(const std::vector<float>& x, double hz, double sampleRate)
    {
        return fastmeasure::goertzel(x, 0, x.size(), hz, sampleRate);
    }
    // ... summed over the first `count` harmonics of `hz`.
    inline double harmonics(const std::vector<float>& x, double hz, int count, double sampleRate)
    {
        double sum = 0.0;
        for (int h = 1; h <= count; ++h)
            sum += tone(x, hz * h, sampleRate);
        return sum;
    }

    inline double noteHz(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

    // The levels of x in windows of `seconds`, dB (a silent window: -180).
    inline std::vector<float> levels(const std::vector<float>& x, double seconds, double sampleRate)
    {
        std::vector<float> out;
        const size_t win = (size_t) std::max(1.0, seconds * sampleRate);
        for (size_t at = 0; at + win <= x.size(); at += win)
            out.push_back(dB(rms(x, at, win)));
        return out;
    }

    // The frequency of a tone near `hz` in x from `from` seconds on: how far its phase moves
    // between two windows of `window` seconds, one after the other.
    inline double frequencyNear(const std::vector<float>& x, double from, double window, double hz, double sampleRate)
    {
        const size_t n = (size_t) (window * sampleRate), a = (size_t) (from * sampleRate);
        if (a + 2 * n > x.size())
            return 0.0;
        const double p0 = fastmeasure::phase(x, a, n, hz, sampleRate), p1 = fastmeasure::phase(x, a + n, n, hz, sampleRate);
        // (both windows are projected on one tone that runs from the start of x: a tone exactly at
        // hz shows the same phase in both; one that is off by d Hz moves by d x window turns.
        // Unambiguous while it is off by less than half a turn a window.)
        double turn = (p1 - p0) / 6.283185307179586;
        turn -= std::round(turn);
        return hz + turn / window;
    }
}
