#pragma once

// The wavetable classes: what a table is (Wavetable), the one set of built-in tables
// (WavetableFactory), a table made from key frames (WavetableKeys) and the one read-out
// (WavetableRead). Moved here, unchanged, from engine/WavetableEngine.h, so that the oscillator
// (dsp/PhaseOsc.h) and every engine can use them without including an engine.

#include "DSPUtils.h"
#include "WaveMorph.h"
#include "WaveTables.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// ============================================================================
// Wavetable — a collection of single-cycle waveforms (frames) that can be
// scanned with a position parameter.  Each frame is 256 samples.  Up to 256
// frames per table.  Mip-mapped at construction for bandlimited playback.
// ============================================================================

struct Wavetable
{
    static constexpr int kFrameSize = 256;
    static constexpr int kMaxFrames = 256;
    static constexpr int kMaxMipLevels = 10; // covers down to ~43 Hz at 44100

    juce::String name;

    // frames[mipLevel][frameIndex][sample]
    // mipLevel 0 = full bandwidth, each successive level halves the harmonics
    std::vector<std::vector<std::array<float, kFrameSize>>> frames;
    int numFrames = 0;

    // Build mip-map chain from the base frames (level 0).
    // Each successive level removes the top half of harmonics via simple
    // spectral truncation in the frequency domain (rectangular window).
    void buildMipMaps()
    {
        if (frames.empty() || frames[0].empty())
            return;

        numFrames = (int) frames[0].size();
        int levels = 1;
        int harmonics = kFrameSize / 2;
        while (harmonics > 1 && levels < kMaxMipLevels)
        {
            harmonics /= 2;
            ++levels;
        }

        frames.resize((size_t) levels);

        for (int level = 1; level < levels; ++level)
        {
            frames[(size_t) level].resize((size_t) numFrames);
            int maxHarmonic = (kFrameSize / 2) >> level;

            for (int f = 0; f < numFrames; ++f)
            {
                const auto& src = frames[0][(size_t) f];
                auto& dst = frames[(size_t) level][(size_t) f];

                // Additive resynthesis with truncated harmonics
                // Analyse base frame via DFT of each harmonic, then resynthesize
                // with only the allowed harmonics.  This is O(N*H) but done once.
                dst.fill(0.0f);

                for (int h = 1; h <= maxHarmonic; ++h)
                {
                    // DFT bin h
                    float cosSum = 0.0f, sinSum = 0.0f;
                    for (int n = 0; n < kFrameSize; ++n)
                    {
                        float angle = 2.0f * juce::MathConstants<float>::pi * h * n / kFrameSize;
                        cosSum += src[(size_t) n] * std::cos(angle);
                        sinSum += src[(size_t) n] * std::sin(angle);
                    }
                    cosSum *= 2.0f / kFrameSize;
                    sinSum *= 2.0f / kFrameSize;

                    // Resynthesize
                    for (int n = 0; n < kFrameSize; ++n)
                    {
                        float angle = 2.0f * juce::MathConstants<float>::pi * h * n / kFrameSize;
                        dst[(size_t) n] += cosSum * std::cos(angle) + sinSum * std::sin(angle);
                    }
                }
            }
        }
    }

    // The same mip chain (level L keeps harmonics 1 .. 128 >> L of the base frame), built from one
    // DFT per frame with a sine table instead of a DFT per level: about fifty times less work, which
    // matters for 128- and 256-frame tables made from a sample. The values differ from
    // buildMipMaps() only by rounding, so the factory tables and the classic 64-frame USER table
    // keep using that one and sound exactly as they always did.
    void buildMipMapsFast()
    {
        if (frames.empty() || frames[0].empty())
            return;

        numFrames = (int) frames[0].size();
        int levels = 1;
        int harmonics = kFrameSize / 2;
        while (harmonics > 1 && levels < kMaxMipLevels)
        {
            harmonics /= 2;
            ++levels;
        }
        frames.resize((size_t) levels);
        for (int level = 1; level < levels; ++level)
            frames[(size_t) level].resize((size_t) numFrames);

        std::array<float, kFrameSize> cosT {}, sinT {};
        for (int n = 0; n < kFrameSize; ++n)
        {
            const double a = 2.0 * juce::MathConstants<double>::pi * n / kFrameSize;
            cosT[(size_t) n] = (float) std::cos(a);
            sinT[(size_t) n] = (float) std::sin(a);
        }

        std::array<float, kFrameSize> acc {};
        for (int f = 0; f < numFrames; ++f)
        {
            const auto& src = frames[0][(size_t) f];
            acc.fill(0.0f);
            int h = 1;
            for (int level = levels - 1; level >= 1; --level)       // fewest harmonics first
            {
                const int maxHarmonic = (kFrameSize / 2) >> level;
                for (; h <= maxHarmonic; ++h)
                {
                    float c = 0.0f, sn = 0.0f;
                    for (int n = 0; n < kFrameSize; ++n)
                    {
                        const size_t k = (size_t) ((h * n) & (kFrameSize - 1));
                        c += src[(size_t) n] * cosT[k];
                        sn += src[(size_t) n] * sinT[k];
                    }
                    c *= 2.0f / kFrameSize;
                    sn *= 2.0f / kFrameSize;
                    for (int n = 0; n < kFrameSize; ++n)
                    {
                        const size_t k = (size_t) ((h * n) & (kFrameSize - 1));
                        acc[(size_t) n] += c * cosT[k] + sn * sinT[k];
                    }
                }
                frames[(size_t) level][(size_t) f] = acc;
            }
        }
    }

    // The same mip chain once more, by Fourier transform: one analysis per frame and one
    // synthesis per level. Level 0 is left exactly as it is. For the tables built while playing
    // (the key-frame table is rebuilt at every turn of one of its knobs) and the newer built-in ones.
    void buildMipMapsFFT()
    {
        if (frames.empty() || frames[0].empty())
            return;

        numFrames = (int) frames[0].size();
        int levels = 1;
        int harmonics = kFrameSize / 2;
        while (harmonics > 1 && levels < kMaxMipLevels)
        {
            harmonics /= 2;
            ++levels;
        }
        frames.resize((size_t) levels);
        for (int level = 1; level < levels; ++level)
            frames[(size_t) level].resize((size_t) numFrames);
        for (int f = 0; f < numFrames; ++f)
        {
            const wavemorph::Spectrum spectrum = wavemorph::analyse(frames[0][(size_t) f]);
            for (int level = 1; level < levels; ++level)
                frames[(size_t) level][(size_t) f] = wavemorph::synthesise(spectrum, (kFrameSize / 2) >> level);
        }
    }

    int getMipLevelCount() const { return (int) frames.size(); }
};

// ============================================================================
// Factory wavetable generation — built-in tables from mathematical functions.
// No external data files, no trademarked names.
// ============================================================================

struct WavetableFactory
{
    static constexpr int N = Wavetable::kFrameSize;
    static constexpr float twoPi = dsp::TWO_PI_F;

    // Normalize a single wavetable frame to peak amplitude 1.0
    static void normalizeFrame(std::array<float, Wavetable::kFrameSize>& frame)
    {
        float peak = 0.0f;
        for (int n = 0; n < N; ++n)
            peak = std::max(peak, std::abs(frame[(size_t) n]));
        if (peak > 0.0f)
            for (int n = 0; n < N; ++n)
                frame[(size_t) n] /= peak;
    }

    // Saw sweep: increasing harmonic content across frames
    static Wavetable createSaw(int numFrames = 64)
    {
        Wavetable wt;
        wt.name = "Saw";
        wt.frames.resize(1);
        wt.frames[0].resize((size_t) numFrames);

        for (int f = 0; f < numFrames; ++f)
        {
            int maxHarmonic = 1 + (f * (N / 2 - 1)) / std::max(numFrames - 1, 1);
            auto& frame = wt.frames[0][(size_t) f];
            frame.fill(0.0f);

            for (int h = 1; h <= maxHarmonic; ++h)
            {
                float amp = 1.0f / (float) h;
                float sign = (h % 2 == 0) ? 1.0f : -1.0f;
                for (int n = 0; n < N; ++n)
                    frame[(size_t) n] += sign * amp * std::sin(twoPi * h * n / N);
            }

            normalizeFrame(frame);
        }
        wt.buildMipMaps();
        return wt;
    }

    // Square sweep: odd harmonics only, increasing content
    static Wavetable createSquare(int numFrames = 64)
    {
        Wavetable wt;
        wt.name = "Square";
        wt.frames.resize(1);
        wt.frames[0].resize((size_t) numFrames);

        for (int f = 0; f < numFrames; ++f)
        {
            int maxHarmonic = 1 + (f * (N / 2 - 1)) / std::max(numFrames - 1, 1);
            auto& frame = wt.frames[0][(size_t) f];
            frame.fill(0.0f);

            for (int h = 1; h <= maxHarmonic; h += 2)
            {
                float amp = 1.0f / (float) h;
                for (int n = 0; n < N; ++n)
                    frame[(size_t) n] += amp * std::sin(twoPi * h * n / N);
            }

            normalizeFrame(frame);
        }
        wt.buildMipMaps();
        return wt;
    }

    // Triangle: odd harmonics, alternating sign, 1/h^2 amplitude
    static Wavetable createTriangle(int numFrames = 32)
    {
        Wavetable wt;
        wt.name = "Triangle";
        wt.frames.resize(1);
        wt.frames[0].resize((size_t) numFrames);

        for (int f = 0; f < numFrames; ++f)
        {
            int maxHarmonic = 1 + (f * (N / 2 - 1)) / std::max(numFrames - 1, 1);
            auto& frame = wt.frames[0][(size_t) f];
            frame.fill(0.0f);

            for (int h = 1; h <= maxHarmonic; h += 2)
            {
                float amp = 1.0f / ((float) h * (float) h);
                float sign = (((h - 1) / 2) % 2 == 0) ? 1.0f : -1.0f;
                for (int n = 0; n < N; ++n)
                    frame[(size_t) n] += sign * amp * std::sin(twoPi * h * n / N);
            }

            normalizeFrame(frame);
        }
        wt.buildMipMaps();
        return wt;
    }

    // Sine: single frame, pure sine
    static Wavetable createSine()
    {
        Wavetable wt;
        wt.name = "Sine";
        wt.frames.resize(1);
        wt.frames[0].resize(1);
        auto& frame = wt.frames[0][0];
        for (int n = 0; n < N; ++n)
            frame[(size_t) n] = std::sin(twoPi * n / N);
        wt.buildMipMaps();
        return wt;
    }

    // PWM sweep: pulse width from ~5% to ~95%
    static Wavetable createPWM(int numFrames = 64)
    {
        Wavetable wt;
        wt.name = "PWM";
        wt.frames.resize(1);
        wt.frames[0].resize((size_t) numFrames);

        for (int f = 0; f < numFrames; ++f)
        {
            float pw = 0.05f + 0.9f * ((float) f / std::max(numFrames - 1, 1));
            auto& frame = wt.frames[0][(size_t) f];
            for (int n = 0; n < N; ++n)
            {
                float phase = (float) n / N;
                frame[(size_t) n] = phase < pw ? 1.0f : -1.0f;
            }
        }
        wt.buildMipMaps();
        return wt;
    }

    // Formant sweep: formant peak frequency moves across frames
    static Wavetable createFormant(int numFrames = 64)
    {
        Wavetable wt;
        wt.name = "Formant";
        wt.frames.resize(1);
        wt.frames[0].resize((size_t) numFrames);

        for (int f = 0; f < numFrames; ++f)
        {
            float formantPos = (float) f / std::max(numFrames - 1, 1);
            float formantFreq = 2.0f + formantPos * 30.0f; // harmonics 2-32
            auto& frame = wt.frames[0][(size_t) f];
            frame.fill(0.0f);

            for (int h = 1; h <= N / 2; ++h)
            {
                float dist = std::abs((float) h - formantFreq);
                float amp = std::exp(-dist * dist * 0.05f);
                for (int n = 0; n < N; ++n)
                    frame[(size_t) n] += amp * std::sin(twoPi * h * n / N);
            }

            normalizeFrame(frame);
        }
        wt.buildMipMaps();
        return wt;
    }

    // Harmonic series: frames progressively add harmonics (1, 1+2, 1+2+3, ...)
    static Wavetable createHarmonicSeries(int numFrames = 64)
    {
        Wavetable wt;
        wt.name = "Harmonics";
        wt.frames.resize(1);
        wt.frames[0].resize((size_t) numFrames);

        for (int f = 0; f < numFrames; ++f)
        {
            int maxHarmonic = 1 + (f * 63) / std::max(numFrames - 1, 1);
            auto& frame = wt.frames[0][(size_t) f];
            frame.fill(0.0f);

            for (int h = 1; h <= maxHarmonic; ++h)
            {
                // Equal amplitude harmonics (organ-like)
                float amp = 1.0f / (float) maxHarmonic;
                for (int n = 0; n < N; ++n)
                    frame[(size_t) n] += amp * std::sin(twoPi * h * n / N);
            }

            normalizeFrame(frame);
        }
        wt.buildMipMaps();
        return wt;
    }

    // Spectral sweep: a resonant peak sweeps from low to high harmonics
    static Wavetable createSpectralSweep(int numFrames = 64)
    {
        Wavetable wt;
        wt.name = "Spectral";
        wt.frames.resize(1);
        wt.frames[0].resize((size_t) numFrames);

        for (int f = 0; f < numFrames; ++f)
        {
            float centerHarmonic = 1.0f + ((float) f / std::max(numFrames - 1, 1)) * 60.0f;
            float bandwidth = 3.0f + centerHarmonic * 0.2f;
            auto& frame = wt.frames[0][(size_t) f];
            frame.fill(0.0f);

            for (int h = 1; h <= N / 2; ++h)
            {
                float dist = std::abs((float) h - centerHarmonic) / bandwidth;
                float amp = std::exp(-dist * dist);
                for (int n = 0; n < N; ++n)
                    frame[(size_t) n] += amp * std::sin(twoPi * h * n / N);
            }

            normalizeFrame(frame);
        }
        wt.buildMipMaps();
        return wt;
    }

    // Digital/bitcrush: progressively reduced bit depth
    static Wavetable createDigital(int numFrames = 32)
    {
        Wavetable wt;
        wt.name = "Digital";
        wt.frames.resize(1);
        wt.frames[0].resize((size_t) numFrames);

        for (int f = 0; f < numFrames; ++f)
        {
            float crushFactor = 1.0f + ((float) f / std::max(numFrames - 1, 1)) * 31.0f;
            auto& frame = wt.frames[0][(size_t) f];

            for (int n = 0; n < N; ++n)
            {
                float sine = std::sin(twoPi * n / N);
                // Quantize to crushFactor levels
                float levels = std::max(2.0f, 256.0f / crushFactor);
                frame[(size_t) n] = std::round(sine * levels) / levels;
            }
        }
        wt.buildMipMaps();
        return wt;
    }

    // ========================================================================
    // Sample-to-wavetable conversion
    //
    // Chops an audio buffer into numFrames equal-length chunks, resamples each
    // chunk to kFrameSize samples via linear interpolation, normalizes, and
    // builds mip-maps.  Called on the main thread, not the audio thread.
    // ========================================================================
    static Wavetable createFromSamples(const float* samples, int numSamples, int numFrames = 64)
    {
        Wavetable wt;
        wt.name = "Sample";
        numFrames = juce::jlimit(1, Wavetable::kMaxFrames, numFrames);
        wt.frames.resize(1);
        wt.frames[0].resize((size_t) numFrames);

        // Each frame covers (numSamples / numFrames) source samples
        float samplesPerFrame = (float) numSamples / (float) numFrames;

        for (int f = 0; f < numFrames; ++f)
        {
            auto& frame = wt.frames[0][(size_t) f];
            float frameStart = (float) f * samplesPerFrame;
            float chunkLen = samplesPerFrame;

            for (int n = 0; n < Wavetable::kFrameSize; ++n)
            {
                // Map frame sample n to source position
                float srcPos = frameStart + ((float) n / (float) Wavetable::kFrameSize) * chunkLen;

                // Linear interpolation from source
                int srcIdx0 = (int) srcPos;
                int srcIdx1 = srcIdx0 + 1;
                float frac = srcPos - (float) srcIdx0;

                // Clamp to valid range
                srcIdx0 = juce::jlimit(0, numSamples - 1, srcIdx0);
                srcIdx1 = juce::jlimit(0, numSamples - 1, srcIdx1);

                frame[(size_t) n] = samples[srcIdx0] * (1.0f - frac) + samples[srcIdx1] * frac;
            }

            normalizeFrame(frame);
        }

        wt.buildMipMaps();
        return wt;
    }

    // ========================================================================
    // Period-aligned sample-to-wavetable conversion
    //
    // createFromSamples() cuts pieces of arbitrary length, so a frame holds some odd number of
    // cycles starting at an arbitrary phase: the pitch of a frame is not the pitch of the note, and
    // neighbouring frames cancel while the scan crossfades between them. Here each piece is
    // reduced to ONE period of the source at that point in time, and each such cycle is rotated to
    // the phase of the one before it, so a scan glides through the recording's timbre at the pitch
    // of the key (pitch-synchronous analysis, as in PSOLA: Moulines & Charpentier, "Pitch-
    // synchronous waveform processing techniques for text-to-speech synthesis using diphones",
    // Speech Communication 9, 1990).
    //
    // The period comes from the normalised square difference function, the autocorrelation-based
    // time-domain pitch estimate of McLeod & Wyvill, "A Smarter Way to Find Pitch", Proc. ICMC
    // 2005 (autocorrelation pitch detection itself: Rabiner, "On the Use of Autocorrelation
    // Analysis for Pitch Detection", IEEE Trans. ASSP 25(1), 1977):
    //     n(tau) = 2 * sum x[j] x[j + tau] / sum (x[j]^2 + x[j + tau]^2),   -1 .. 1
    // with their peak picking (the first peak that reaches 90 % of the highest one, which avoids
    // octave errors) and parabolic interpolation of the peak. The search runs on a copy of the
    // sample decimated to about 8 kHz and is then refined at the full rate around the lag found.
    // A piece whose best peak is below kPitchClarity (noise, silence, chords, drums) is not
    // pitched: it is cut exactly as createFromSamples() cuts it.
    //
    // Message thread only (it allocates).
    // ========================================================================
    static constexpr float kPitchClarity = 0.85f;
    static constexpr double kPitchMinHz = 50.0;
    static constexpr double kPitchMaxHz = 1500.0;

    class PeriodFinder
    {
    public:
        PeriodFinder(const float* samples, int numSamples, double sampleRate)
            : x(samples), n(numSamples)
        {
            if (sampleRate <= 0.0)
                sampleRate = 44100.0;
            decim = juce::jmax(1, (int) (sampleRate / 8000.0));
            dec.resize((size_t) (n / decim));
            for (size_t i = 0; i < dec.size(); ++i)
            {
                float sum = 0.0f;
                for (int k = 0; k < decim; ++k)
                    sum += x[i * (size_t) decim + (size_t) k];
                dec[i] = sum / (float) decim;
            }
            minLag = juce::jmax(2, (int) (sampleRate / kPitchMaxHz));
            maxLag = juce::jmin((int) (sampleRate / kPitchMinHz), n / 3);
            nsdf.resize((size_t) (maxLag / decim + 2));
        }

        // The period in samples of the sound around `centre`, or 0 when it has no clear pitch there.
        float periodAt(double centre, float* clarityOut = nullptr)
        {
            if (clarityOut != nullptr)
                *clarityOut = 0.0f;

            // Coarse search on the decimated copy.
            const int maxLd = maxLag / decim;
            const int minLd = juce::jmax(2, minLag / decim);
            const int window = 2 * maxLd;
            if (maxLd <= minLd + 1 || (int) dec.size() < window)
                return 0.0f;
            const int start = juce::jlimit(0, (int) dec.size() - window, (int) (centre / decim) - window / 2);
            for (int lag = 0; lag <= maxLd; ++lag)
                nsdf[(size_t) lag] = nsdfAt(dec.data() + start, window, lag);

            int from = 1;                                   // past the peak at lag 0
            while (from < maxLd && nsdf[(size_t) from] <= nsdf[(size_t) from - 1])
                ++from;
            from = juce::jmax(from, minLd);
            float best = -1.0f;
            for (int lag = from; lag < maxLd; ++lag)
                best = juce::jmax(best, nsdf[(size_t) lag]);
            if (best < 0.5f)
                return 0.0f;
            int coarse = -1;
            for (int lag = from; lag < maxLd; ++lag)
            {
                const float v = nsdf[(size_t) lag];
                if (v >= 0.9f * best && v >= nsdf[(size_t) lag - 1] && v >= nsdf[(size_t) lag + 1])
                {
                    coarse = lag;
                    break;
                }
            }
            if (coarse < 0)
                return 0.0f;

            // Refinement at the full rate.
            const int fullWindow = juce::jmin(2 * maxLag, n);
            const int fullStart = juce::jlimit(0, n - fullWindow, (int) centre - fullWindow / 2);
            const int lo = juce::jmax(2, coarse * decim - decim - 1);
            const int hi = juce::jmin(fullWindow - 2, coarse * decim + decim + 1);
            if (hi <= lo)
                return 0.0f;
            int bestLag = lo;
            float bestValue = -2.0f;
            for (int lag = lo; lag <= hi; ++lag)
            {
                const float v = nsdfAt(x + fullStart, fullWindow, lag);
                if (v > bestValue)
                {
                    bestValue = v;
                    bestLag = lag;
                }
            }
            if (clarityOut != nullptr)
                *clarityOut = bestValue;
            if (bestValue < kPitchClarity)
                return 0.0f;

            const float a = nsdfAt(x + fullStart, fullWindow, bestLag - 1);
            const float c = nsdfAt(x + fullStart, fullWindow, bestLag + 1);
            const float denom = a - 2.0f * bestValue + c;
            float shift = denom < -1.0e-9f ? 0.5f * (a - c) / denom : 0.0f;
            shift = juce::jlimit(-0.5f, 0.5f, shift);
            return (float) bestLag + shift;
        }

    private:
        static float nsdfAt(const float* s, int window, int lag)
        {
            double r = 0.0, m = 0.0;
            for (int j = 0; j + lag < window; ++j)
            {
                const double a = s[j], b = s[j + lag];
                r += a * b;
                m += a * a + b * b;
            }
            return m > 1.0e-12 ? (float) (2.0 * r / m) : 0.0f;
        }

        const float* x;
        int n;
        int decim = 1, minLag = 2, maxLag = 2;
        std::vector<float> dec, nsdf;
    };

    // numFrames frames from a sample. aligned = false: equal pieces, as createFromSamples(), for
    // any frame count. aligned = true: one phase-aligned period per frame where the sample is
    // pitched (see above), the equal piece where it is not. pitchedFrames, if given, receives how
    // many frames were taken as single periods.
    static Wavetable createFromSamplesEx(const float* samples, int numSamples, double sampleRate,
                                         int numFrames, bool aligned, int* pitchedFrames = nullptr)
    {
        Wavetable wt;
        wt.name = "Sample";
        numFrames = juce::jlimit(1, Wavetable::kMaxFrames, numFrames);
        wt.frames.resize(1);
        wt.frames[0].resize((size_t) numFrames);
        if (pitchedFrames != nullptr)
            *pitchedFrames = 0;

        auto at = [samples, numSamples](float srcPos)
        {
            int i0 = (int) srcPos;
            int i1 = i0 + 1;
            const float frac = srcPos - (float) i0;
            i0 = juce::jlimit(0, numSamples - 1, i0);
            i1 = juce::jlimit(0, numSamples - 1, i1);
            return samples[i0] * (1.0f - frac) + samples[i1] * frac;
        };

        PeriodFinder finder(samples, numSamples, sampleRate);
        const float samplesPerFrame = (float) numSamples / (float) numFrames;
        const std::array<float, N>* previous = nullptr;      // the last single-period frame
        std::array<float, N> cycle {};

        for (int f = 0; f < numFrames; ++f)
        {
            auto& frame = wt.frames[0][(size_t) f];
            const float frameStart = (float) f * samplesPerFrame;
            const float period = aligned ? finder.periodAt((double) frameStart + 0.5 * samplesPerFrame) : 0.0f;

            if (period < 2.0f || period + 2.0f > (float) numSamples)
            {
                for (int n = 0; n < N; ++n)
                    frame[(size_t) n] = at(frameStart + ((float) n / (float) N) * samplesPerFrame);
                normalizeFrame(frame);
                previous = nullptr;
                continue;
            }

            // One period around the middle of the piece.
            const float start = juce::jlimit(0.0f, (float) numSamples - 1.0f - period,
                                             frameStart + 0.5f * samplesPerFrame - 0.5f * period);
            for (int n = 0; n < N; ++n)
                cycle[(size_t) n] = at(start + ((float) n / (float) N) * period);
            // A real period does not end exactly where it began (the level and the pitch drift):
            // spread that step over the cycle so the frame loops without a click.
            const float seam = at(start + period) - cycle[0];
            for (int n = 0; n < N; ++n)
                cycle[(size_t) n] -= seam * ((float) n / (float) N - 0.5f);

            // Rotate it: to the phase of the frame before, or, for the first of a run, so that its
            // fundamental starts like a sine.
            int rotate = 0;
            if (previous != nullptr)
            {
                float bestCorr = -1.0e30f;
                for (int r = 0; r < N; ++r)
                {
                    float corr = 0.0f;
                    for (int n = 0; n < N; ++n)
                        corr += cycle[(size_t) ((n + r) & (N - 1))] * (*previous)[(size_t) n];
                    if (corr > bestCorr)
                    {
                        bestCorr = corr;
                        rotate = r;
                    }
                }
            }
            else
            {
                float c = 0.0f, s = 0.0f;
                for (int n = 0; n < N; ++n)
                {
                    c += cycle[(size_t) n] * std::cos(twoPi * (float) n / (float) N);
                    s += cycle[(size_t) n] * std::sin(twoPi * (float) n / (float) N);
                }
                const float phi = std::atan2(c, s);        // cycle ~ sin(theta + phi)
                rotate = (int) std::lround(-phi / twoPi * (float) N);
                rotate = ((rotate % N) + N) % N;
            }
            for (int n = 0; n < N; ++n)
                frame[(size_t) n] = cycle[(size_t) ((n + rotate) & (N - 1))];
            normalizeFrame(frame);
            previous = &frame;
            if (pitchedFrames != nullptr)
                ++*pitchedFrames;
        }

        wt.buildMipMapsFast();
        return wt;
    }

    // The built-in tables, built once and shared (read only): a wavetable engine starts from a
    // copy of them (building them takes far longer than copying them), the unified voice's
    // oscillator slots read them where they are.
    static const std::vector<Wavetable>& sharedFactoryTables()
    {
        static const std::vector<Wavetable> tables = createAllFactoryTables();
        return tables;
    }

    // The newer built-in tables (src/dsp/WaveTables.h), built once and shared like the first nine.
    // They are a list of their own, so the first nine keep their numbers; builtInTables() is the
    // two lists one after the other.
    static const std::vector<Wavetable>& sharedMoreTables()
    {
        static const std::vector<Wavetable> tables = []
        {
            std::vector<Wavetable> list;
            for (auto& def : wavetables::definitions())
            {
                Wavetable wt;
                wt.name = def.label;
                wt.frames.resize(1);
                wt.frames[0] = def.make();
                wt.buildMipMapsFFT();
                list.push_back(std::move(wt));
            }
            return list;
        }();
        return tables;
    }

    // ---- The built-in set: the one list of tables every oscillator in the program reads ----------
    // The first nine, then the newer ones in the order of wavetables::definitions(); read only,
    // built on first use (call it from the message thread before audio runs: both engines'
    // constructors do). The wavetable engine's TABL puts its own USER and KEYS tables after the
    // first nine; the unified voice's oscillator slots offer exactly this list (nTBL is an index
    // into it). Both orders are stored in projects: new tables go at the end.
    static constexpr int NUM_FIRST_TABLES = 9;
    static const std::vector<const Wavetable*>& builtInTables()
    {
        static const std::vector<const Wavetable*> list = []
        {
            std::vector<const Wavetable*> l;
            for (auto& t : sharedFactoryTables())
                l.push_back(&t);
            for (auto& t : sharedMoreTables())
                l.push_back(&t);
            return l;
        }();
        return list;
    }
    static int builtInCount() { return NUM_FIRST_TABLES + (int) wavetables::definitions().size(); }
    // What a table cell shows for each of them, in the same order.
    static const char* const* builtInLabels()
    {
        static const std::vector<const char*> labels = []
        {
            std::vector<const char*> l { "SAW", "SQR", "TRI", "SIN", "PWM", "FORM", "HARM", "SPEC", "DIGI" };
            for (auto& def : wavetables::definitions())
                l.push_back(def.label);
            return l;
        }();
        return labels.data();
    }

    // Returns the first nine built-in tables
    static std::vector<Wavetable> createAllFactoryTables()
    {
        std::vector<Wavetable> tables;
        tables.push_back(createSaw());
        tables.push_back(createSquare());
        tables.push_back(createTriangle());
        tables.push_back(createSine());
        tables.push_back(createPWM());
        tables.push_back(createFormant());
        tables.push_back(createHarmonicSeries());
        tables.push_back(createSpectralSweep());
        tables.push_back(createDigital());
        return tables;
    }
};

// ============================================================================
// Key frames: a table made from a few single cycles taken out of samples.
//
// The player picks up to eight waves (KEY 1 .. 8: a sample and a place in it), puts them along
// the table, and the frames between two neighbouring keys are computed by one of the algorithms
// of wavemorph::morph(). The frames AT the keys are the key waves themselves.
//
// extract()  one cycle out of a sample, as its harmonics. Where the sample has a clear pitch at
//            that place (WavetableFactory::PeriodFinder, as for the aligned USER table) the cycle
//            is exactly one period, so the wave plays at the pitch of the key whatever the pitch
//            of the recording; where it has none (noise, a drum, a chord) it is a piece of about
//            12 ms. The period is read with cubic interpolation at 256 points or more (the next
//            power of two above its length), the step where its end meets its start is spread over
//            the cycle, and only harmonics 1 .. 127 are kept: a long period is band-limited on its
//            way into a frame instead of aliasing.
// build()    the table. Each key is limited to HARM harmonics, tilted by TILT, turned to a common
//            phase (ALGN) and brought to a peak of 1; the keys are put in the order of their
//            places, the first at the first frame and the last at the last (WRAP adds the first
//            key once more at the end), each on the frame nearest to its place.
//
// Message thread only (both allocate).
// ============================================================================

struct WavetableKeys
{
    static constexpr int MAX_KEYS = 8;
    static constexpr int NUM_FRAME_OPTIONS = 6;
    static int frameCount(int option)
    {
        static const int counts[NUM_FRAME_OPTIONS] = { 8, 16, 32, 64, 128, 256 };
        return counts[(size_t) juce::jlimit(0, NUM_FRAME_OPTIONS - 1, option)];
    }

    enum Norm { NormOff, NormPeak, NormRms, NumNorms };

    struct Settings
    {
        wavemorph::Algo algo = wavemorph::Algo::Linear;
        int frames = 64;
        float bias = 0.0f;          // -1 .. 1: the in-between frames lean towards the earlier / the later key
        bool wrap = false;          // after the last key the table goes back to the first
        bool align = true;          // keys turned to a common phase
        int norm = NormRms;         // level of the frames: as computed, each to a peak of 1, all equally loud
        float tilt = 0.0f;          // dB per octave on every key
        int harmonics = wavemorph::H;   // highest harmonic of a key
    };

    struct Key
    {
        wavemorph::Spectrum spectrum;
        float place = 0.0f;         // 0 .. 1 along the table (only the order and the spacing count)
    };

    // One cycle of the sample around `position` (0 .. 1 of its length). right may be null (mono).
    // False when there is nothing to take.
    static bool extract(const float* left, const float* right, int numSamples, double sampleRate, float position,
                        wavemorph::Spectrum& out, bool* pitched = nullptr)
    {
        if (pitched != nullptr)
            *pitched = false;
        if (left == nullptr || numSamples < 64)
            return false;
        if (sampleRate <= 0.0)
            sampleRate = 44100.0;

        // A window of about 0.19 s around the place is all the pitch search needs.
        const int window = juce::jmin(numSamples, juce::jmax(256, (int) (sampleRate * 0.1858)));
        const double centre = (double) juce::jlimit(0.0f, 1.0f, position) * (double) (numSamples - 1);
        const int windowStart = juce::jlimit(0, numSamples - window, (int) centre - window / 2);
        std::vector<float> x((size_t) window);
        for (int i = 0; i < window; ++i)
            x[(size_t) i] = right != nullptr ? 0.5f * (left[windowStart + i] + right[windowStart + i]) : left[windowStart + i];
        const double local = centre - (double) windowStart;

        WavetableFactory::PeriodFinder finder(x.data(), window, sampleRate);
        float period = finder.periodAt(local);
        const bool isPitched = period >= 2.0f && period + 4.0f < (float) window;
        if (! isPitched)
            period = juce::jmin((float) window - 4.0f, (float) (sampleRate * 512.0 / 44100.0));
        if (pitched != nullptr)
            *pitched = isPitched;

        auto at = [&x, window](double pos)         // cubic (Catmull-Rom) interpolation
        {
            const int i = (int) std::floor(pos);
            const float f = (float) (pos - (double) i);
            auto s = [&x, window](int k) { return x[(size_t) juce::jlimit(0, window - 1, k)]; };
            const float a = s(i - 1), b = s(i), c = s(i + 1), d = s(i + 2);
            return b + 0.5f * f * (c - a + f * (2.0f * a - 5.0f * b + 4.0f * c - d + f * (3.0f * (b - c) + d - a)));
        };

        int points = wavemorph::N;
        while ((float) points < period && points < 4096)
            points *= 2;
        const double start = juce::jlimit(0.0, (double) window - 1.0 - (double) period, local - 0.5 * (double) period);
        std::vector<float> cycle((size_t) points);
        for (int n = 0; n < points; ++n)
            cycle[(size_t) n] = at(start + (double) n / (double) points * (double) period);
        // A real period does not end exactly where it began: spread that step over the cycle.
        const float seam = at(start + (double) period) - cycle[0];
        for (int n = 0; n < points; ++n)
            cycle[(size_t) n] -= seam * ((float) n / (float) points - 0.5f);

        out = wavemorph::analyse(cycle.data(), points);
        return out.energy() > 1.0e-12;
    }

    // The frame each key sits on, for keys at `place` (ascending) in a table of `frames` frames:
    // the nearest frame, and never two keys on one.
    static std::vector<int> keyFrames(const std::vector<float>& place, int frames)
    {
        const int count = (int) place.size();
        std::vector<int> index((size_t) count, 0);
        if (count < 2)
            return index;
        const float span = place.back() - place.front();
        for (int k = 0; k < count; ++k)
        {
            const float u = span > 1.0e-6f ? (place[(size_t) k] - place.front()) / span : (float) k / (float) (count - 1);
            index[(size_t) k] = juce::roundToInt(u * (float) (frames - 1));
        }
        for (int k = 1; k < count; ++k)
            index[(size_t) k] = juce::jmax(index[(size_t) k], index[(size_t) k - 1] + 1);
        for (int k = count - 1; k >= 0; --k)
            index[(size_t) k] = juce::jmin(index[(size_t) k], frames - 1 - (count - 1 - k));
        return index;
    }

    // keyFrameIndices, if given, receives the frame of each key in the order of their places (with
    // WRAP one more: the first key again, on the last frame).
    static Wavetable build(std::vector<Key> keys, const Settings& s, std::vector<int>* keyFrameIndices = nullptr)
    {
        Wavetable wt;
        wt.name = "KEYS";
        wt.frames.resize(1);
        if (keyFrameIndices != nullptr)
            keyFrameIndices->clear();

        if (keys.empty())           // nothing chosen yet: a sine
        {
            wt.frames[0].resize(1);
            for (int n = 0; n < wavemorph::N; ++n)
                wt.frames[0][0][(size_t) n] = std::sin(dsp::TWO_PI_F * (float) n / (float) wavemorph::N);
            wt.buildMipMapsFFT();
            return wt;
        }

        const bool classic = s.algo == wavemorph::Algo::Classic;
        auto frameOf = [classic](const wavemorph::Spectrum& sp)
        {
            return classic ? wavemorph::classicFrame(sp) : wavemorph::synthesise(sp);
        };

        // The keys as they go into the table.
        std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.place < b.place; });
        for (auto& key : keys)
        {
            wavemorph::tiltAndLimit(key.spectrum, (double) s.tilt, juce::jlimit(1, wavemorph::H, s.harmonics));
            if (s.align)
                wavemorph::alignToFundamental(key.spectrum);
            const float peak = wavemorph::peakOf(wavemorph::synthesise(key.spectrum));
            if (peak > 1.0e-9f)
                wavemorph::scale(key.spectrum, 1.0 / (double) peak);
        }

        std::vector<float> place;
        for (auto& key : keys)
            place.push_back(key.place);
        if (s.wrap && keys.size() > 1)
        {
            // The way back to the first key is as long as the average step between two keys.
            keys.push_back(keys.front());
            const float span = place.back() - place.front();
            place.push_back(place.back() + (span > 1.0e-6f ? span / (float) (place.size() - 1) : 1.0f));
        }

        const int count = (int) keys.size();
        const int frames = count == 1 ? 1 : juce::jlimit(count, Wavetable::kMaxFrames, s.frames);
        const std::vector<int> index = keyFrames(place, frames);
        if (keyFrameIndices != nullptr)
            *keyFrameIndices = index;

        wt.frames[0].resize((size_t) frames);
        const double exponent = std::pow(4.0, -(double) juce::jlimit(-1.0f, 1.0f, s.bias));
        int segment = 0;
        for (int f = 0; f < frames; ++f)
        {
            while (segment < count - 2 && f >= index[(size_t) segment + 1])
                ++segment;
            const int a = index[(size_t) segment];
            const int b = count > 1 ? index[(size_t) segment + 1] : a;
            if (f <= a || count == 1)
                wt.frames[0][(size_t) f] = frameOf(keys[(size_t) segment].spectrum);
            else if (f >= b)
                wt.frames[0][(size_t) f] = frameOf(keys[(size_t) segment + 1].spectrum);
            else
            {
                const double t = std::pow((double) (f - a) / (double) (b - a), exponent);
                wt.frames[0][(size_t) f] = frameOf(wavemorph::morph(s.algo, keys[(size_t) segment].spectrum,
                                                                    keys[(size_t) segment + 1].spectrum, t));
            }
        }

        // The level of the frames.
        if (s.norm == NormPeak)
        {
            for (auto& frame : wt.frames[0])
                WavetableFactory::normalizeFrame(frame);
        }
        else if (s.norm == NormRms)
        {
            // Every frame as loud as the others: as loud as the peakiest of them can be with its
            // peak at 1 (but not below 0.25: a frame peakier than that is held at a peak of 1).
            float target = 1.0f;
            for (auto& frame : wt.frames[0])
            {
                const float peak = wavemorph::peakOf(frame);
                if (peak > 1.0e-9f)
                    target = juce::jmin(target, wavemorph::rmsOf(frame) / peak);
            }
            target = juce::jmax(0.25f, target);
            for (auto& frame : wt.frames[0])
            {
                const float rms = wavemorph::rmsOf(frame);
                if (rms <= 1.0e-9f)
                    continue;
                wavemorph::scale(frame, target / rms);
                const float peak = wavemorph::peakOf(frame);
                if (peak > 1.0f)
                    wavemorph::scale(frame, 1.0f / peak);
            }
        }

        wt.buildMipMapsFFT();
        return wt;
    }
};

// ============================================================================
// Interpolation modes for scanning between wavetable frames.
// ============================================================================

enum class WavetableInterpMode
{
    Linear,     // Crossfade between adjacent frames (default)
    Spectral,   // FFT-based interpolation (expensive, highest quality)
    Stepped,    // No interpolation — snaps to nearest frame (lo-fi)
    Vintage,    // Stepped with short crossfade (8-sample xfade, retro character)
    // The whole read-out of an early digital wavetable instrument, from what is publicly known of
    // how those worked: a table was 64 waves of 128 samples of 8 bits; the wave was read by a
    // phase counter with no interpolation between samples, the scan went from one wave to the
    // next in whole steps, and nothing was band-limited to the pitch being played. So here: the
    // position is taken to the nearest of 64 places, the frame is read at 128 samples per cycle
    // with each held, rounded to 8 bits, and ALWAYS from the full-bandwidth copy (never a mip
    // level). On high notes the harmonics above half the sample rate fold back as tones that are
    // not harmonics of the note: the glassy top such instruments are known for.
    // Next to the two older lo-fi modes: STEP also holds samples and frames but reads the band-
    // limited copy (grainy, no aliasing); VINT interpolates samples and softens each frame step
    // (smooth, no aliasing); this one leaves everything raw.
    Classic,
    Count
};

// ============================================================================
// The timed scan ("stretch"): each voice runs the scan position through the
// table in a set time from its own note-on, whatever the pitch of the note.
// See the SCAN page of WavetableEngine for the controls and the rules.
// ============================================================================

enum class WavetableScanMode
{
    Off,    // POS stays where the knob is
    Run,    // once through, then hold the last frame
    Loop,   // wrap around
    Ping,   // back and forth
    Step,   // once through in STEP plateaus, then hold
    Rand,   // a random frame, a new one STEP times per pass
    Count
};

// ============================================================================
// WavetableRead — how a table is read: the one read-out in the program.
//
// One sample of a table is a function of the table, the position along it, the
// phase within the cycle, the pitch (as phase advance per sample, which picks the
// band-limited copy) and the INTP mode, and of nothing else: there is no memory
// between samples. The wavetable engine's voices call it with their own phase,
// and so do the unified voice's oscillator slots set to WT (UnifiedVoice::
// generateOsc, where every oscillator kind has to be a function of the slot's
// phase). What needs memory - the timed scan, glide, the position motion - is in
// WavetableVoice and decides only WHICH position and phase are asked for.
// ============================================================================

struct WavetableRead
{
    // The copy whose harmonics all stay under half the sample rate when the phase advances by
    // phaseInc (cycles per sample): each level halves the harmonics.
    static int mipLevel(const Wavetable& table, double phaseInc)
    {
        const int last = table.getMipLevelCount() - 1;
        int level = 0;
        for (double ratio = phaseInc * 2.0 * Wavetable::kFrameSize; ratio > 1.0 && level < last; ratio *= 0.5)
            ++level;
        return level;
    }

    // position 0 .. 1 along the table, phase 0 .. 1 within the cycle.
    static float sample(const Wavetable& table, int mipLevel, float position, double phase,
                        WavetableInterpMode interpMode = WavetableInterpMode::Linear)
    {
        if (mipLevel < 0 || mipLevel >= table.getMipLevelCount())
            return 0.0f;

        const auto& levelFrames = table.frames[(size_t) mipLevel];
        int nFrames = (int) levelFrames.size();
        if (nFrames == 0) return 0.0f;

        // Frame position from the position parameter
        float framePos = position * (float) (nFrames - 1);
        int frameA = juce::jlimit(0, nFrames - 1, (int) framePos);
        int frameB = juce::jlimit(0, nFrames - 1, frameA + 1);
        float frameFrac = framePos - (float) frameA;

        // Sample position within frame
        float samplePos = (float) phase * Wavetable::kFrameSize;
        int sampleA = ((int) samplePos) % Wavetable::kFrameSize;
        int sampleB = (sampleA + 1) % Wavetable::kFrameSize;
        float sampleFrac = samplePos - std::floor(samplePos);

        switch (interpMode)
        {
            case WavetableInterpMode::Count:
            case WavetableInterpMode::Spectral:
                // Falls through to Linear (true FFT interpolation not yet implemented;
                // mip-mapped tables already provide spectral quality).
                [[fallthrough]];
            case WavetableInterpMode::Linear:
            {
                float valA = dsp::lerp(levelFrames[(size_t) frameA][(size_t) sampleA],
                                       levelFrames[(size_t) frameA][(size_t) sampleB],
                                       sampleFrac);
                float valB = dsp::lerp(levelFrames[(size_t) frameB][(size_t) sampleA],
                                       levelFrames[(size_t) frameB][(size_t) sampleB],
                                       sampleFrac);
                return dsp::lerp(valA, valB, frameFrac);
            }

            case WavetableInterpMode::Stepped:
            {
                // Nearest frame, nearest sample — lo-fi digital character
                int nearestFrame = juce::jlimit(0, nFrames - 1, (int) std::round(framePos));
                int nearestSample = ((int) std::round(samplePos)) % Wavetable::kFrameSize;
                return levelFrames[(size_t) nearestFrame][(size_t) nearestSample];
            }

            case WavetableInterpMode::Classic:
            {
                const auto& raw = table.frames[0];
                const int place = (int) std::lround(position * 63.0f);
                const int frame = juce::jlimit(0, nFrames - 1, (int) std::lround((float) place * (float) (nFrames - 1) / 63.0f));
                const float v = raw[(size_t) frame][(size_t) (sampleA & ~1)];
                return std::round(v * 127.0f) * (1.0f / 127.0f);
            }

            case WavetableInterpMode::Vintage:
            {
                // Stepped frames with short crossfade (retro digital character)
                int nearestFrame = juce::jlimit(0, nFrames - 1, (int) std::round(framePos));

                // Within-frame: linear interpolation on samples
                float val = dsp::lerp(levelFrames[(size_t) nearestFrame][(size_t) sampleA],
                                      levelFrames[(size_t) nearestFrame][(size_t) sampleB],
                                      sampleFrac);

                // Short crossfade near frame boundaries (8 samples worth)
                float crossfadeZone = 8.0f / Wavetable::kFrameSize;
                float distToEdge = frameFrac < 0.5f ? frameFrac : (1.0f - frameFrac);
                if (distToEdge < crossfadeZone && frameA != frameB)
                {
                    int otherFrame = frameFrac < 0.5f
                        ? juce::jlimit(0, nFrames - 1, nearestFrame - 1)
                        : juce::jlimit(0, nFrames - 1, nearestFrame + 1);
                    float otherVal = dsp::lerp(levelFrames[(size_t) otherFrame][(size_t) sampleA],
                                               levelFrames[(size_t) otherFrame][(size_t) sampleB],
                                               sampleFrac);
                    float xfade = distToEdge / crossfadeZone;
                    val = dsp::lerp(otherVal, val, xfade);
                }

                return val;
            }
        }

        return 0.0f;
    }
};
