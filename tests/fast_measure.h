#pragma once

#include <complex>
#include <cstddef>
#include <vector>

// -----------------------------------------------------------------------------
// The measuring arithmetic of the sound tests, compiled with optimisation.
//
// The test binary is a Debug build (it is the reference: guardrails, TESTING), and
// in a Debug build a hand-written FFT over std::complex is function calls all the
// way down: the "Mod Wheel", "Strip Routes", "LFO Destinations" and "Dead
// Controls" suites spent 60 to 90 % of their time measuring what they had
// rendered, not rendering it. The arithmetic lives in fast_measure.cpp, which
// CMake compiles with -O2 whatever the build type; the instrument itself is still
// compiled as the build type says. The code is the code the suites had, moved:
// same operations in the same order, so the numbers are the same numbers.
//
// A new measurement that loops over samples (an FFT, a Goertzel, a correlation)
// belongs here, not in a test header.
// -----------------------------------------------------------------------------
namespace fastmeasure
{
    // Magnitude frames of the "Mod Wheel" measurement: Hann window, hop of half a frame, the
    // square root of the magnitude of the lower half of the spectrum.
    std::vector<std::vector<float>> frames(const std::vector<float>& x, int fftSize);

    // 0 = the same sound, 1 = nothing in common (the frames of both, compared bin by bin).
    float difference(const std::vector<float>& a, const std::vector<float>& b, int fftSize);

    // RMS of the whole render and its energy-weighted mean frequency in Hz.
    void rmsAndCentroid(const std::vector<float>& x, int fftSize, double sampleRate, float& rms, float& centroid);

    // In-place radix-2 FFT in double precision (the "Cymbal Machine" suite's spectra).
    void fft(std::vector<std::complex<double>>& a);

    // Goertzel power of `hz` in x[from, from + n), Hann window; samples past the end are left out
    // (the sampler suites' pitch and spectrum measurements).
    double goertzel(const std::vector<float>& x, size_t from, size_t n, double hz, double sampleRate);

    // Level of harmonic h of `hz` in x, by projection on its sine and cosine ("Wavetable Embed").
    double harmonic(const std::vector<float>& x, double hz, int h, double sampleRate);

    // The lag at which x repeats best (normalised correlation), searched from 0.6 to 1.4 of the
    // expected period, and how well it repeats there (1 = exactly); 0 when x is too short.
    int bestLag(const std::vector<float>& x, int start, double expected, double& correlation);
}
