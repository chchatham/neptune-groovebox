#pragma once

#include "EngineBase.h"
#include "VoiceAllocator.h"
#include "VoiceFilterSection.h"
#include "EngineLfoSection.h"
#include "SampleBank.h"
#include "../dsp/Envelope.h"
#include "../dsp/EnvelopeChain.h"
#include "../dsp/DSPUtils.h"
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

    // Returns all built-in factory tables
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
// Interpolation modes for scanning between wavetable frames.
// ============================================================================

enum class WavetableInterpMode
{
    Linear,     // Crossfade between adjacent frames (default)
    Spectral,   // FFT-based interpolation (expensive, highest quality)
    Stepped,    // No interpolation — snaps to nearest frame (lo-fi)
    Vintage     // Stepped with short crossfade (8-sample xfade, retro character)
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
// WavetableVoice — a single voice that reads from a wavetable.
//
// Supports optional EnvelopeChain for multi-stage envelope architectures.
// When envChainEnabled is true, the chain output replaces the simple ADSR.
// ============================================================================

class WavetableVoice
{
public:
    // LFO destinations (EngineLfoSection), in the order of the DEST cell.
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // +-12 semitones, depth squared
        LfoAmp,         // gain 0..2
        LfoPan,         // +-1
        LfoCutoff,      // +-4 octaves
        LfoResonance,   // +-0.5
        LfoPosition,    // scan position, +-1 (the whole table) around POS
        LfoSpread,      // +-1 around SPRD (0..1)
        LfoSub,         // +-1 around SUB (0..1)
        NumLfoDests
    };

    static const char* const* lfoDestLabels()
    {
        static const char* const names[NumLfoDests] = {
            "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "POS", "SPRD", "SUB" };
        return names;
    }

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        ampEnv.setSampleRate(sr);
        envChain.setSampleRate(sr);
        filter.setSampleRate(sr);
        lfos.setSampleRate(sr);
        declick.setSampleRate(sr);
        gainDeclick.setSampleRate(sr);
    }

    void setWavetable(const Wavetable* wt) { wavetable = wt; }

    void setNote(int note, float velocity)
    {
        currentNote = note;
        currentVelocity = velocity;
        targetFrequency = dsp::midiNoteToHz(note);
        // Glide from the previous pitch only if this voice is still sounding.
        if (glideCoeff >= 1.0f || !active)
            baseFrequency = targetFrequency;
    }

    void noteOn()
    {
        if (active)
        {
            declick.arm();      // struck again while sounding: the phase restart must not jump,
            gainDeclick.arm();  // nor the step to the new velocity
        }
        // An LFO on a page cell is read before the first sample (the engine sets the cell then).
        if (lfos.keyed()) lfos.noteOnAndSample(); else lfos.noteOn();
        filter.noteOn(currentNote, currentVelocity, active);
        if (envChainEnabled && envChain.getStageCount() > 0)
            envChain.noteOn();
        else
            ampEnv.noteOn();
        active = true;
        phase = 0.0;
        phase2 = 0.25;
        subPhase = 0.0;
        // Every note-on starts this voice's own scan run again (like its envelope and its phase).
        scanPhase = 0.0;
        scanHeldStep = -1;
    }

    void noteOff()
    {
        if (envChainEnabled && envChain.getStageCount() > 0)
            envChain.noteOff();
        else
            ampEnv.noteOff();
        filter.noteOff();
    }

    bool isActive() const { return active; }

    void setPosition(float pos) { position = juce::jlimit(0.0f, 1.0f, pos); }
    void setInterpMode(WavetableInterpMode m) { interpMode = m; }
    void setDetune(float cents) { detuneCents = cents; }
    void setSubLevel(float lvl) { subLevel = juce::jlimit(0.0f, 1.0f, lvl); }
    void setMorphSpeed(float speed) { morphSpeed = speed; }
    void setOctave(int oct) { octaveShift = juce::jlimit(-4, 4, oct); }
    void setSpread(float s) { spread = juce::jlimit(0.0f, 1.0f, s); }          // second, detuned copy
    void setPitchOffset(float semitones) { pitchOffset = semitones; }           // tune, bend, vibrato
    void setPositionMod(float m) { positionMod = m; }                           // pressure
    void setGainMod(float g) { gainMod = g; }
    void setMorphDepth(float d) { morphDepth = juce::jlimit(0.0f, 1.0f, d); }
    void setEnvToPosition(float a) { envToPos = juce::jlimit(-1.0f, 1.0f, a); }
    void setVelToPosition(float a) { velToPos = juce::jlimit(0.0f, 1.0f, a); }
    void setVelocitySensitivity(float s) { velSens = juce::jlimit(0.0f, 1.0f, s); }
    void setGlideTime(float seconds)
    {
        glideCoeff = seconds > 0.0005f ? 1.0f - std::exp(-3.0f / (seconds * (float) sampleRate)) : 1.0f;
    }

    // --- Timed scan ---
    void setScanMode(WavetableScanMode m) { scanMode = m; }
    void setScanPassSeconds(double seconds) { scanInc = 1.0 / (juce::jmax(0.001, seconds) * sampleRate); }
    void setScanSteps(int n) { scanSteps = juce::jlimit(2, 256, n); }
    void setScanReverse(bool r) { scanReverse = r; }
    void setScanSeed(uint32_t seed) { scanRng = seed != 0 ? seed : 0x9E3779B9u; }
    double getScanPhase() const { return scanPhase; }                   // passes since the note-on
    float getEffectivePosition() const { return effectivePosition; }    // as of the last sample

    // --- EnvelopeChain support ---
    void setEnvelopeChainEnabled(bool enabled) { envChainEnabled = enabled; }
    bool isEnvelopeChainEnabled() const { return envChainEnabled; }
    EnvelopeChain& getEnvelopeChain() { return envChain; }

    float process()
    {
        if (!active || wavetable == nullptr || wavetable->numFrames == 0)
            return 0.0f;

        // LFOs: spread and sub level are moved for this sample only (the set values stay).
        const bool lfoOn = lfos.anyOn();
        const float setSpreadValue = spread, setSubValue = subLevel;
        if (lfoOn)
        {
            lfos.tick();
            lfoApplied = true;
            lfoPosition = lfos.amount(LfoPosition);
            lfoSemis = lfos.pitchSemitones(LfoPitch);
            spread = juce::jlimit(0.0f, 1.0f, spread + lfos.amount(LfoSpread));
            subLevel = juce::jlimit(0.0f, 1.0f, subLevel + lfos.amount(LfoSub));
            filter.setLfoOctaves(lfos.amount(LfoCutoff) * VoiceLfos::CUTOFF_OCTAVES);
            filter.setResonanceMod(lfos.amount(LfoResonance) * VoiceLfos::RESONANCE_RANGE);
            lfoGain = juce::jlimit(0.0f, 2.0f, 1.0f + lfos.amount(LfoAmp));
            lfoPan = juce::jlimit(-1.0f, 1.0f, lfos.amount(LfoPan));
        }
        else if (lfoApplied)
        {
            lfoPosition = 0.0f;
            lfoSemis = 0.0f;
            filter.setLfoOctaves(0.0f);
            filter.setResonanceMod(0.0f);
            lfoGain = 1.0f;
            lfoPan = 0.0f;
            lfoApplied = false;
        }

        // The timed scan sets the base position (the knob's value when it is off).
        const bool scanning = scanMode != WavetableScanMode::Off;
        const float basePosition = scanning ? advanceScan() : position;

        // Auto-morph position
        if (morphSpeed > 0.0f)
        {
            morphPhase += morphSpeed / (float) sampleRate;
            while (morphPhase >= 1.0f)
                morphPhase -= 1.0f;
            // Sinusoidal LFO modulation of position
            effectivePosition = basePosition + 0.5f * morphDepth * std::sin(morphPhase * 2.0f * juce::MathConstants<float>::pi);
            if (! scanning)
                effectivePosition = effectivePosition - std::floor(effectivePosition); // wrap 0-1
        }
        else
        {
            effectivePosition = basePosition;
        }
        // Envelope, velocity and pressure move the scan position.
        float posOffset = positionMod + envToPos * lastEnv + velToPos * currentVelocity + lfoPosition;
        if (posOffset != 0.0f || scanning)
            effectivePosition = juce::jlimit(0.0f, 1.0f, effectivePosition + posOffset);

        if (baseFrequency != targetFrequency)
        {
            baseFrequency += (targetFrequency - baseFrequency) * (double) glideCoeff;
            if (std::abs(targetFrequency - baseFrequency) < 0.01)
                baseFrequency = targetFrequency;
        }

        // Compute frequency with detune, octave and pitch offset
        double freq = baseFrequency * std::exp2((double) octaveShift + detuneCents / 1200.0 + (pitchOffset + lfoSemis) / 12.0);
        double phaseInc = freq / sampleRate;

        // Select mip level based on frequency to avoid aliasing
        int mipLevel = 0;
        if (wavetable->getMipLevelCount() > 1)
        {
            // Each mip level halves the maximum safe playback frequency
            double maxFreqForLevel0 = sampleRate / (2.0 * Wavetable::kFrameSize);
            double ratio = freq / maxFreqForLevel0;
            while (ratio > 1.0 && mipLevel < wavetable->getMipLevelCount() - 1)
            {
                ratio *= 0.5;
                ++mipLevel;
            }
        }

        // Read from wavetable with position-based frame interpolation
        float sample = readWavetable(mipLevel, phase);

        // Spread: a second copy up to 30 cents away, mixed equally.
        if (spread > 0.0f)
        {
            sample = (sample + readWavetable(mipLevel, phase2)) * 0.7071f;
            phase2 += phaseInc * std::exp2((double) spread * 30.0 / 1200.0);
            while (phase2 >= 1.0)
                phase2 -= 1.0;
        }

        // Sub oscillator: a sine one octave below
        float sub = 0.0f;
        if (subLevel > 0.0f)
            sub = subLevel * std::sin((float) subPhase * juce::MathConstants<float>::twoPi);
        subPhase += phaseInc * 0.5;
        while (subPhase >= 1.0)
            subPhase -= 1.0;

        // Advance phase
        phase += phaseInc;
        while (phase >= 1.0)
            phase -= 1.0;

        // Envelope: use chain if enabled, otherwise simple ADSR
        float envVal;
        if (envChainEnabled && envChain.getStageCount() > 0)
        {
            envVal = envChain.process();
            if (!envChain.isActive())
                active = false;
        }
        else
        {
            envVal = ampEnv.process();
            if (!ampEnv.isActive())
                active = false;
        }

        lastEnv = envVal;
        if (lfoOn)
        {
            spread = setSpreadValue;
            subLevel = setSubValue;
        }
        return gainDeclick.process(filter.process(declick.process(sample + sub)) * envVal * (1.0f - velSens + velSens * currentVelocity) * gainMod * lfoGain);
    }

    void reset()
    {
        declick.reset();
        gainDeclick.reset();
        lastEnv = 0.0f;         // ENVP must not start a note from the note before's envelope
        ampEnv.reset();
        envChain.reset();
        filter.reset();
        active = false;
        phase = 0.0;
        morphPhase = 0.0f;
        scanPhase = 0.0;
        scanHeldStep = -1;
    }

    Envelope& getAmpEnvelope() { return ampEnv; }
    VoiceFilter& getFilter() { return filter; }
    VoiceLfos& getLfos() { return lfos; }
    // -1..1 from LFOs routed to the pan (as of the last processed sample); the engine places the voice.
    float getLfoPan() const { return lfoPan; }

private:
    // One sample of the scan run: where the run is between the POS knob (0) and the end of the
    // table (1), then the base position. The first sample of a note is exactly at the start.
    float advanceScan()
    {
        const double u = scanPhase;
        scanPhase += scanInc;
        float t = 0.0f;
        switch (scanMode)
        {
            case WavetableScanMode::Run:
                t = u >= 1.0 ? 1.0f : (float) u;
                break;
            case WavetableScanMode::Loop:
                t = (float) (u - std::floor(u));
                break;
            case WavetableScanMode::Ping:
            {
                const double v = u - 2.0 * std::floor(u * 0.5);
                t = (float) (v <= 1.0 ? v : 2.0 - v);
                break;
            }
            case WavetableScanMode::Step:
            {
                const int k = u >= 1.0 ? scanSteps - 1 : juce::jmin(scanSteps - 1, (int) (u * scanSteps));
                t = (float) k / (float) (scanSteps - 1);
                break;
            }
            case WavetableScanMode::Rand:
            {
                const juce::int64 k = (juce::int64) (u * scanSteps);
                if (k != scanHeldStep)
                {
                    scanHeldStep = k;
                    // xorshift32 (Marsaglia, "Xorshift RNGs", J. Statistical Software 8(14), 2003)
                    scanRng ^= scanRng << 13;
                    scanRng ^= scanRng >> 17;
                    scanRng ^= scanRng << 5;
                    scanHeld = (float) (scanRng >> 8) * (1.0f / 16777215.0f);
                }
                t = scanHeld;
                break;
            }
            case WavetableScanMode::Off:
            case WavetableScanMode::Count:
                break;
        }
        if (scanReverse)
            t = 1.0f - t;
        float pos = position + t * (1.0f - position);
        if (scanMode == WavetableScanMode::Rand && wavetable->numFrames > 1)
        {
            const float last = (float) (wavetable->numFrames - 1);      // whole frames only
            pos = std::round(pos * last) / last;
        }
        return pos;
    }

    float readWavetable(int mipLevel, double phase) const
    {
        if (wavetable == nullptr || mipLevel >= wavetable->getMipLevelCount())
            return 0.0f;

        const auto& levelFrames = wavetable->frames[(size_t) mipLevel];
        int nFrames = (int) levelFrames.size();
        if (nFrames == 0) return 0.0f;

        // Frame position from the position parameter
        float framePos = effectivePosition * (float) (nFrames - 1);
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

    double sampleRate = 44100.0;
    double baseFrequency = 440.0;
    double phase = 0.0;
    int currentNote = 60;
    float currentVelocity = 1.0f;
    bool active = false;

    const Wavetable* wavetable = nullptr;
    float position = 0.0f;          // 0.0 - 1.0 scan through frames
    float effectivePosition = 0.0f; // after morph modulation
    WavetableInterpMode interpMode = WavetableInterpMode::Linear;
    float detuneCents = 0.0f;
    float subLevel = 0.0f;
    float morphSpeed = 0.0f;        // Hz, 0 = manual position only
    float morphPhase = 0.0f;
    int octaveShift = 0;
    float spread = 0.0f;
    float pitchOffset = 0.0f;
    float positionMod = 0.0f;
    float gainMod = 1.0f;
    float morphDepth = 1.0f;
    float envToPos = 0.0f;
    float velToPos = 0.0f;
    float velSens = 1.0f;
    float lastEnv = 0.0f;
    float glideCoeff = 1.0f;
    double targetFrequency = 440.0;
    double phase2 = 0.25;
    double subPhase = 0.0;

    // Timed scan
    WavetableScanMode scanMode = WavetableScanMode::Off;
    double scanPhase = 0.0;         // passes since the note-on
    double scanInc = 0.0;           // passes per sample
    int scanSteps = 16;
    bool scanReverse = false;
    juce::int64 scanHeldStep = -1;
    float scanHeld = 0.0f;
    uint32_t scanRng = 0x9E3779B9u;

    Envelope ampEnv;
    dsp::Declick declick;       // on the oscillators, before the filter (whose state smears a jump)
    dsp::Declick gainDeclick;   // on the output

    // Filter (after the oscillators and sub, before the amp envelope)
    VoiceFilter filter;

    // LFOs
    VoiceLfos lfos;
    bool lfoApplied = false;
    float lfoPosition = 0.0f;
    float lfoSemis = 0.0f;
    float lfoGain = 1.0f;
    float lfoPan = 0.0f;

    // EnvelopeChain support — replaces ampEnv when enabled
    EnvelopeChain envChain;
    bool envChainEnabled = false;
};

// ============================================================================
// WavetableEngine — EngineBase implementation wrapping VoiceAllocator and
// WavetableVoice, with built-in factory tables and parameter interface.
// ============================================================================

class WavetableEngine : public EngineBase
{
public:
    enum Param
    {
        Position       = 0,
        InterpMode     = 1,
        Detune         = 2,
        UnisonSpread   = 3,
        SubLevel       = 4,
        MorphSpeed     = 5,
        Level          = 6,
        Octave         = 7,
        TableSelect    = 8,
        AmpAttack      = 9,
        AmpDecay       = 10,
        AmpSustain     = 11,
        AmpRelease     = 12,
        EnvChainMode   = 13,   // 0=off, 1=series, 2=parallel
        EnvChainStages = 14,   // number of envelope stages in the chain
        EnvChainCombine = 15,  // 0=sum, 1=max (for parallel mode)
        TuneParam = 16,        // semitones
        BendRangeParam = 17,   // semitones
        MorphDepth = 18,       // depth of the position motion
        EnvToPosition = 19,    // amp envelope -> scan position, -1..1
        VelToPosition = 20,    // velocity -> scan position
        VelSens = 21,          // velocity -> level
        Glide = 22,            // seconds
        Vibrato = 23,          // constant vibrato depth (the mod strip adds to it)
        UserSample = 24,       // which SampleBank entry the USER table is built from
        // The SCAN page: a timed run of the position through the table (see pages()).
        ScanMode = 25,         // WavetableScanMode index
        ScanTime = 26,         // seconds for one pass (SYNC = OFF)
        ScanSync = 27,         // 0 = TIME, 1 = LEN at the tempo, 2 = NAT (the USER sample's own length)
        ScanLength = 28,       // index into scanLengthBeats (SYNC = ON)
        ScanSteps = 29,        // index into scanStepCounts (STEP and RAND)
        ScanDirection = 30,    // 0 = forward, 1 = reverse
        ScanSpeed = 31,        // 0.25 .. 4: the pass is this many times faster (1 = 100 %)
        UserFrames = 32,       // how the USER table is cut: index into userFrameCounts, +3 = period-aligned
        FilterBase = 100       // 100-115: the per-voice filter (VoiceFilterSection offsets)
    };

    explicit WavetableEngine(int maxVoices = 8)
        : allocator(maxVoices)
    {
        voices.resize((size_t) maxVoices);

        // Build factory tables (done once at construction, not on audio thread)
        tables = WavetableFactory::sharedFactoryTables();

        // One more: USER, built from a sample (a recording made in the app, or a file). Until one
        // is chosen it is a copy of the sine table.
        tables.reserve(tables.size() + 1);
        tables.push_back(tables[3]);
        tables.back().name = "USER";

        // Point all voices at the first table
        for (auto& v : voices)
            v.setWavetable(tables.empty() ? nullptr : &tables[0]);
        setScanSeed(0x4E455054u);

        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            voices[(size_t) voiceIdx].setNote(v.note, v.velocity);
            voices[(size_t) voiceIdx].noteOn();
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            voices[(size_t) voiceIdx].noteOff();
        });

        // A new note takes a silent voice while there is one: release tails ring out. Not with
        // GLID on: a glide starts from the pitch of a voice that still sounds, so it takes that one.
        allocator.setVoiceSoundingQuery([this](int voiceIdx)
        {
            return voices[(size_t) voiceIdx].isActive() && getParameter(Glide) <= 0.0f;
        });

        // Seed every parameter so getParameter is always the true value.
        for (auto& page : pages())
            for (auto& p : page.params)
                if (p.isValid())
                    setParameter(p.id, p.defaultVal);
        filterSection.applyAll(VoiceFilterSection::each(voices));
        lfoSection.applyAll(EngineLfoSection::each(voices));
    }

    // ---- The SCAN page ---------------------------------------------------------------------
    // MODE  OFF / RUN / LOOP / PING / STEP / RAND. Each voice starts its own run at its note-on
    //       (every note-on restarts it, exactly as it restarts the voice's envelope) and travels
    //       from the POS knob's value to the end of the table: POS is the start offset.
    //         RUN   once through, then holds the last frame
    //         LOOP  jumps back to POS and goes again
    //         PING  to the end and back
    //         STEP  once through in STEP equal plateaus (first = POS, last = the end), then holds
    //         RAND  a random whole frame between POS and the end, a new one STEP times per pass
    // TIME  seconds for one pass (SYNC = OFF).
    // SYNC  OFF: TIME.  ON: LEN at the tempo, followed live.  NAT: the natural length — one pass
    //       over the whole USER table lasts exactly as long as the sample it was built from, so RUN
    //       replays the recording at its original speed at any pitch. With POS above 0 the pass is
    //       shorter in proportion (the rest of the recording, still at its own speed). On any
    //       other table NAT has no source and uses TIME.
    // LEN   1/16 note ... 16 bars of 4 beats.
    // STEP  4 / 8 / 16 / 32 / 64 steps per pass (STEP and RAND).
    // DIR   FWD: POS -> end.  REV: end -> POS.
    // SPD   x0.25 ... x4 (25 % - 400 %): the pass is that much faster, in every SYNC setting
    //       (NAT x 0.5 = half speed at the original pitch of the key).
    // FRMS  how the USER table is cut from its sample: 64 / 128 / 256 equal pieces, or A64 / A128 /
    //       A256 = one phase-aligned period per frame (WavetableFactory::createFromSamplesEx).
    //       The table is rebuilt when this changes. 64 is the default: it is what every USER table
    //       made so far uses, so saved sounds do not change.
    //
    // How the run adds up with everything else that moves the position, per sample:
    //     base     = POS knob                       (MODE = OFF)
    //              = POS + run * (1 - POS)          (run = 0..1 from the mode and DIR)
    //     position = clamp(base + MSPD/MDPT motion + pressure + ENVP + VELP + LFOs to POS, 0, 1)
    // The run sets the base and the others add to it; the sum stops at the two ends of the table.
    // (With MODE = OFF the MSPD motion wraps around the table as it always has; under a run it is
    // clamped like the rest, so a held last frame does not jump to the first.)
    static constexpr int NUM_SCAN_LENGTHS = 9;
    static constexpr int NUM_SCAN_STEPS = 5;
    static constexpr int NUM_USER_FRAME_OPTIONS = 6;
    static float scanLengthBeats(int index)
    {
        static const float beats[NUM_SCAN_LENGTHS] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f };
        return beats[(size_t) juce::jlimit(0, NUM_SCAN_LENGTHS - 1, index)];
    }
    static int scanStepCount(int index)
    {
        static const int counts[NUM_SCAN_STEPS] = { 4, 8, 16, 32, 64 };
        return counts[(size_t) juce::jlimit(0, NUM_SCAN_STEPS - 1, index)];
    }
    static int userFrameCount(int option)
    {
        static const int counts[3] = { 64, 128, 256 };
        return counts[(size_t) (juce::jlimit(0, NUM_USER_FRAME_OPTIONS - 1, option) % 3)];
    }
    static bool userFramesAligned(int option) { return option >= 3; }

    // WAVE (table, scan position, motion), SCAN (the timed run), OSC (pitch, thickness) and AMP
    // (envelope, chain, level).
    static const ParamPages& pages()
    {
        static const char* const scanModes[] = { "OFF", "RUN", "LOOP", "PING", "STEP", "RAND" };
        static const char* const scanSync[] = { "OFF", "ON", "NAT" };
        static const char* const scanLengths[] = { "1/16", "1/8", "1/4", "1/2", "1BAR", "2BAR", "4BAR", "8BAR", "16BR" };
        static const char* const scanSteps[] = { "4", "8", "16", "32", "64" };
        static const char* const scanDirs[] = { "FWD", "REV" };
        static const char* const userFrames[] = { "64", "128", "256", "A64", "A128", "A256" };
        static const char* const tableNames[] = { "SAW", "SQR", "TRI", "SIN", "PWM", "FORM", "HARM", "SPEC", "DIGI", "USER" };
        static const char* const interp[] = { "LIN", "SPEC", "STEP", "VINT" };
        static const char* const chain[] = { "OFF", "SER", "PAR" };
        static const char* const combine[] = { "SUM", "MAX" };
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;
        static const ParamPages list = [&]
        {
          ParamPages l = {
            { "WAVE", "SRC", {{
                { "TABL", TableSelect, 0, USER_TABLE, 0, C, "", tableNames, USER_TABLE + 1 },
                { "POS",  Position, 0, 1, 0, L },
                { "MSPD", MorphSpeed, 0, 10, 0, L, "Hz" },
                { "MDPT", MorphDepth, 0, 1, 1, L },
                { "ENVP", EnvToPosition, -1, 1, 0, L },
                { "VELP", VelToPosition, 0, 1, 0, L },
                { "INTP", InterpMode, 0, 3, 0, C, "", interp, 4 },
                { "LEVL", Level, 0, 1, 0.8f, L } }} },
            { "SCAN", "SRC", {{
                { "MODE", ScanMode, 0, (float) ((int) WavetableScanMode::Count - 1), 0, C, "", scanModes, (int) WavetableScanMode::Count },
                { "TIME", ScanTime, 0.05f, 60, 2, G, "s" },
                { "SYNC", ScanSync, 0, 2, 0, C, "", scanSync, 3 },
                { "LEN",  ScanLength, 0, NUM_SCAN_LENGTHS - 1, 4, C, "", scanLengths, NUM_SCAN_LENGTHS },
                { "STEP", ScanSteps, 0, NUM_SCAN_STEPS - 1, 2, C, "", scanSteps, NUM_SCAN_STEPS },
                { "DIR",  ScanDirection, 0, 1, 0, C, "", scanDirs, 2 },
                { "SPD",  ScanSpeed, 0.25f, 4, 1, G, "x" },
                { "FRMS", UserFrames, 0, NUM_USER_FRAME_OPTIONS - 1, 0, C, "", userFrames, NUM_USER_FRAME_OPTIONS } }} },
            { "OSC", "SRC", {{
                { "OCT",  Octave, -4, 4, 0, I },
                { "TUNE", TuneParam, -12, 12, 0, I, "st" },
                { "FINE", Detune, -100, 100, 0, L, "ct" },
                { "BEND", BendRangeParam, 0, 24, 2, I, "st" },
                { "GLID", Glide, 0, 2, 0, L, "s" },
                { "VIBR", Vibrato, 0, 1, 0, L },
                { "SPRD", UnisonSpread, 0, 1, 0, L },
                { "SUB",  SubLevel, 0, 1, 0, L } }} },
            { "AMP", "AMP", {{
                { "ATK",  AmpAttack, 0.001f, 10, 0.01f, G, "s" },
                { "DEC",  AmpDecay, 0.001f, 10, 0.3f, G, "s" },
                { "SUS",  AmpSustain, 0, 1, 0.7f, L },
                { "REL",  AmpRelease, 0.001f, 10, 0.3f, G, "s" },
                { "VEL",  VelSens, 0, 1, 0.7f, L },
                { "CHAN", EnvChainMode, 0, 2, 0, C, "", chain, 3 },
                { "STGS", EnvChainStages, 1, 8, 2, I },
                { "COMB", EnvChainCombine, 0, 1, 0, C, "", combine, 2 } }} },
          };
          VoiceFilterSection::appendPages(l, FilterBase);
          EngineLfoSection(lfoDests(&l)).appendPages(l);
          return l;
        }();
        return list;
    }

    // The LFO destinations: the original eight, then the other continuous cells of the pages and
    // the other LFO. TIME, SPD (the scan) and VIBR are read once per block, the rest per sample.
    // Not offered: TABL, INTP, the scan's MODE / SYNC / LEN / STEP / DIR / FRMS, CHAN, STGS, COMB
    // (choices and counts), OCT, TUNE, FINE and BEND (PTCH is the pitch), LEVL (the level of the
    // mixed voices; AMP is each voice's), and what the filter leaves out.
    static const LfoDestList& lfoDests(const ParamPages* enginePages = nullptr)
    {
        if (enginePages == nullptr)
            pages();                    // the list is made on the way
        static const LfoDestList list = makeLfoDests(*enginePages);
        static const bool registered = (list.registerGroups(), true);
        juce::ignoreUnused(registered);
        return list;
    }

    static LfoDestList makeLfoDests(const ParamPages& p)
    {
        LfoDestList l(WavetableVoice::lfoDestLabels(), WavetableVoice::NumLfoDests);
        l.group();
        for (int id : { (int) MorphSpeed, (int) MorphDepth, (int) EnvToPosition, (int) VelToPosition })
            l.add(p, id);
        l.group();
        l.add(p, ScanTime);
        l.add(p, ScanSpeed);
        l.group();
        l.add(p, Glide);
        l.add(p, Vibrato);
        l.group();
        for (int id : { (int) AmpAttack, (int) AmpDecay, (int) AmpSustain, (int) AmpRelease, (int) VelSens })
            l.add(p, id);
        VoiceFilterSection::addLfoDests(l, p, FilterBase);
        l.addCross();
        // The mod strip until the STRIP page says otherwise: 60 % of the table further on, and a
        // quarter-tone of vibrato.
        l.stripRoute(0, WavetableVoice::LfoPosition, 0.6f);
        l.stripVibrato(0.25f);
        return l;
    }

    bool isRebuildParam(ParamId id) const override { return id == UserSample || id == UserFrames; }

    ParamPages getParamPages() const override
    {
        ParamPages out = pages();
        filterSection.updateModeCell(out);
        return out;
    }

    void pitchBend(float amount) override { bend = juce::jlimit(-1.0f, 1.0f, amount); }
    void modWheel(float value) override { wheel = juce::jlimit(0.0f, 1.0f, value); }
    void setTempo(float bpm) override
    {
        if (bpm > 0.0f)
            tempo = bpm;        // a synced scan follows it from the next block on
        lfoSection.setTempo(bpm, EngineLfoSection::each(voices));
    }

    EngineType getType() const override { return EngineType::Wavetable; }
    juce::String getName() const override { return "Wavetable"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr;
        for (auto& v : voices)
            v.setSampleRate(sr);
        applyParameter(Glide, getParameter(Glide));
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        int numSamples = buffer.getNumSamples();

        if (userReady.load(std::memory_order_relaxed))
        {
            const juce::SpinLock::ScopedTryLockType lock(userLock);
            if (lock.isLocked())
            {
                std::swap(tables[(size_t) USER_TABLE], userPending);     // the voices' pointers stay valid
                userSeconds = userPendingSeconds;
                userReady.store(false, std::memory_order_relaxed);
            }
        }

        // The timed scan: one pass length for the block (so SYNC follows the tempo live).
        if (scanMode != WavetableScanMode::Off)
        {
            const double seconds = getScanPassSeconds();
            for (auto& voice : voices)
                voice.setScanPassSeconds(voice.getLfos().keyed() ? getScanPassSeconds(&voice.getLfos()) : seconds);
        }

        // Once per block: tuning, bend, vibrato and each voice's pressure (scan position, volume).
        // The mod strip: the routes of the STRIP page (by default the table position and
        // vibrato), and the cutoff by the filter's WHL amount.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, sampleRate);
        lfoSection.stripBlock(wheelNow, numSamples, sampleRate, EngineLfoSection::each(voices),
                              [this](int key) { setParameter(key, getParameter(key)); });
        vibratoPhase += 5.5 * numSamples / sampleRate;
        vibratoPhase -= std::floor(vibratoPhase);
        const float vib = std::sin((float) vibratoPhase * juce::MathConstants<float>::twoPi);
        for (int v = 0; v < (int) voices.size(); ++v)
        {
            float pressure = v < allocator.getVoiceCount() ? allocator.getAftertouch(v) : 0.0f;
            AftertouchRouter::ModValues m;
            if (pressure > 0.0f)
                m = (aftertouchRouter ? *aftertouchRouter : allocator.getAftertouchRouter()).computeModulation(pressure);
            auto& voice = voices[(size_t) v];
            voice.setPositionMod(m.wavePosMod + m.timbreMod);
            voice.setGainMod(juce::jlimit(0.0f, 2.0f, 1.0f + m.volumeMod));
            voice.setPitchOffset(tune + bend * bendRange
                                 + vib * (m.vibratoMod + voice.getLfos().value(Vibrato, vibrato) * 0.5f));
            voice.getFilter().setExternalOctaves(filterSection.externalOctaves(wheelNow, m.cutoffMod));
        }

        for (int i = 0; i < numSamples; ++i)
        {
            // An LFO routed to PAN moves its own voice between the channels (balance law: the
            // centre is unchanged, so a sound without that routing is exactly as before).
            float sampleL = 0.0f, sampleR = 0.0f;
            for (auto& voice : voices)
            {
                if (! voice.isActive())
                    continue;
                // LFOs on page cells: this voice's own value of each (as of its last sample).
                if (voice.getLfos().keyed())
                    voice.getLfos().forEachKeyed([&voice](int key, float value) { applyVoiceParam(voice, key, value); });
                const float s = voice.process();
                const float pan = voice.getLfoPan();
                sampleL += pan > 0.0f ? s * (1.0f - pan) : s;
                sampleR += pan < 0.0f ? s * (1.0f + pan) : s;
            }

            sampleL *= masterLevel * 0.7f;
            sampleR *= masterLevel * 0.7f;

            if (right)
            {
                left[i] += sampleL;
                right[i] += sampleR;
            }
            else
            {
                left[i] += 0.5f * (sampleL + sampleR);
            }
        }

        lfoSection.endBlock(numSamples, EngineLfoSection::each(voices));
    }

    void releaseResources() override
    {
        for (auto& v : voices)
            v.reset();
    }

    void noteOn(int note, float velocity, int /*voiceIndex*/) override
    {
        allocator.noteOn(note, velocity);
    }

    void noteOff(int note, int /*voiceIndex*/) override
    {
        allocator.noteOff(note);
    }

    void allNotesOff() override
    {
        allocator.allNotesOff();
        for (auto& v : voices)
            v.reset();
    }

    void aftertouch(int note, float pressure) override
    {
        allocator.setAftertouch(note, pressure);
    }

    // ---- Per-stage envelope curves (StageCurveParam): the amp envelope is envelope slot 0 -------
    int getStageCurveParam(ParamId cellId) const override
    {
        if (cellId == AmpAttack)  return StageCurveParam::id(0, StageCurveParam::Attack);
        if (cellId == AmpDecay)   return StageCurveParam::id(0, StageCurveParam::Decay);
        if (cellId == AmpRelease) return StageCurveParam::id(0, StageCurveParam::Release);
        return filterSection.stageCurveParamFor(cellId);
    }

private:
    StageCurves stageCurves;

    void setStageCurve(ParamId id, float value)
    {
        stageCurves.set(id, value);
        if (StageCurveParam::envelopeOf(id) != 0)
            return;
        for (auto& v : voices)
        {
            auto p = v.getAmpEnvelope().getParams();
            stageCurves.applyTo(p, 0);
            v.getAmpEnvelope().setParams(p);
            // Chain stages are copies of this envelope.
            auto& chain = v.getEnvelopeChain();
            for (int s = 0; s < chain.getStageCount(); ++s)
                chain.getEnvelopeMut(s).setParams(p);
        }
    }

public:
    void setParameter(ParamId id, float value) override
    {
        if (filterSection.owns(id))
        {
            filterSection.set(id, value, VoiceFilterSection::each(voices));
            lfoSection.baseChanged(id, filterSection.get(id), EngineLfoSection::each(voices));
            return;
        }
        if (lfoSection.owns(id))
        {
            // A cell an LFO has let go of goes back to its set value on every voice.
            const int released = lfoSection.set(id, value, EngineLfoSection::each(voices),
                                                [this](int key) { return getParameter(key); });
            if (released >= 0)
                setParameter(released, getParameter(released));
            return;
        }
        if (StageCurveParam::owns(id))
        {
            setStageCurve(id, value);
            return;
        }
        params[id] = value;
        applyParameter(id, value);
        lfoSection.baseChanged(id, value, EngineLfoSection::each(voices));
    }

    float getParameter(ParamId id) const override
    {
        if (filterSection.owns(id))
            return filterSection.get(id);
        if (lfoSection.owns(id))
            return lfoSection.get(id);
        if (StageCurveParam::owns(id))
            return stageCurves.get(id);
        auto it = params.find(id);
        return it != params.end() ? it->second : 0.0f;
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (filterSection.owns(id))
            return VoiceFilterSection::name(id - FilterBase);
        if (EngineLfoSection::owns(id))
            return EngineLfoSection::name(id);
        switch (id)
        {
            case Position:       return "Position";
            case InterpMode:     return "Interp";
            case Detune:         return "Detune";
            case UnisonSpread:   return "Spread";
            case SubLevel:       return "Sub";
            case MorphSpeed:     return "Morph";
            case Level:          return "Level";
            case Octave:         return "Octave";
            case TableSelect:    return "Table";
            case AmpAttack:      return "Attack";
            case AmpDecay:       return "Decay";
            case AmpSustain:     return "Sustain";
            case AmpRelease:     return "Release";
            case EnvChainMode:   return "Env Chain";
            case EnvChainStages: return "Chain Stages";
            case EnvChainCombine: return "Chain Combine";
            case ScanMode:       return "Scan Mode";
            case ScanTime:       return "Scan Time";
            case ScanSync:       return "Scan Sync";
            case ScanLength:     return "Scan Length";
            case ScanSteps:      return "Scan Steps";
            case ScanDirection:  return "Scan Direction";
            case ScanSpeed:      return "Scan Speed";
            case UserFrames:     return "User Frames";
            default:             return "Param " + juce::String(id);
        }
    }

    int getParameterCount() const override { return 24; }

    // Access for tests and advanced use
    VoiceAllocator& getAllocator() { return allocator; }
    WavetableVoice& getVoice(int index) { return voices[(size_t) index]; }
    int getTableCount() const { return (int) tables.size(); }
    const Wavetable& getTable(int index) const { return tables[(size_t) index]; }

    // The USER table: the sample is cut into frames (FRMS: 64 equal pieces unless set otherwise),
    // so POS scans through the recording. Built on the calling (message) thread; the audio thread
    // swaps it in, with the sample's duration (the natural scan length), at the start of its next
    // block and does nothing else: no allocation, and only a try-lock.
    static constexpr int USER_TABLE = 9;
    void buildUserTable(int bankIndex)
    {
        const auto* e = SampleBank::get().entry(bankIndex);
        if (e == nullptr || e->left.size() < 64)
            return;
        std::vector<float> mono(e->left);
        if (e->right.size() == mono.size())
            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] = 0.5f * (mono[i] + e->right[i]);
        buildUserTableFrom(mono.data(), (int) mono.size(), e->sampleRate);
    }

    // The same from any mono buffer (the bank is not involved).
    void buildUserTableFrom(const float* mono, int numSamples, double sourceRate)
    {
        if (mono == nullptr || numSamples < 64)
            return;
        if (sourceRate <= 0.0)
            sourceRate = 44100.0;
        Wavetable wt = userFrameOption == 0
            ? WavetableFactory::createFromSamples(mono, numSamples, 64)
            : WavetableFactory::createFromSamplesEx(mono, numSamples, sourceRate, userFrameCount(userFrameOption),
                                                    userFramesAligned(userFrameOption));
        wt.name = "USER";
        const juce::SpinLock::ScopedLockType lock(userLock);
        userPending = std::move(wt);
        userPendingSeconds = (double) numSamples / sourceRate;
        userReady.store(true);
    }
    bool userTablePending() const { return userReady.load(); }

    // Duration of the sample behind the USER table that is playing (0 until one has been built).
    double getUserSampleSeconds() const { return userSeconds; }

    // Seconds one scan pass takes right now (TIME, LEN at the tempo or the natural length, over SPD).
    // With a voice's LFOs: for that voice, whose LFOs may be moving TIME or SPD.
    double getScanPassSeconds(const VoiceLfos* lfos = nullptr) const
    {
        double seconds = lfos != nullptr ? lfos->value(ScanTime, scanTime) : scanTime;
        if (scanSync == 2 && currentTableIndex == USER_TABLE && userSeconds > 0.0)
            seconds = userSeconds * (double) juce::jmax(0.0f, 1.0f - scanStart);
        else if (scanSync == 1)
            seconds = (double) scanLengthBeats(scanLength) * 60.0 / (double) tempo;
        return seconds / (double) (lfos != nullptr ? lfos->value(ScanSpeed, scanSpeed) : scanSpeed);
    }

    // RAND draws from one generator per voice; the same seed gives the same jumps.
    void setScanSeed(uint32_t seed)
    {
        for (size_t v = 0; v < voices.size(); ++v)
            voices[v].setScanSeed(seed + (uint32_t) v * 0x9E3779B9u);
    }

    void selectTable(int index)
    {
        if (index >= 0 && index < (int) tables.size())
        {
            currentTableIndex = index;
            for (auto& v : voices)
                v.setWavetable(&tables[(size_t) index]);
        }
    }

private:
    // One voice's own value of a page cell: what an LFO routed to that cell sets, sample by
    // sample (TIME, SPD and VIBR are read per block instead, where the engine works them out).
    // Envelope times act on the stage that is running (the chain's stages are copies).
    static void applyVoiceParam(WavetableVoice& voice, ParamId id, float value)
    {
        if (id >= FilterBase)
        {
            VoiceFilterSection::applyTo(voice.getFilter(), id - FilterBase, value);
            return;
        }
        switch (id)
        {
            case MorphSpeed:    voice.setMorphSpeed(value); return;
            case MorphDepth:    voice.setMorphDepth(value); return;
            case EnvToPosition: voice.setEnvToPosition(value); return;
            case VelToPosition: voice.setVelToPosition(value); return;
            case Glide:         voice.setGlideTime(value); return;
            case VelSens:       voice.setVelocitySensitivity(value); return;
            case AmpAttack: case AmpDecay: case AmpSustain: case AmpRelease: break;
            default: return;
        }
        auto setEnvelope = [id, value](Envelope& e)
        {
            if (id == AmpAttack)       e.setAttack(value);
            else if (id == AmpDecay)   e.setDecay(value);
            else if (id == AmpSustain) e.setSustain(value);
            else                       e.setRelease(value);
        };
        setEnvelope(voice.getAmpEnvelope());
        auto& chain = voice.getEnvelopeChain();
        for (int s = 0; s < chain.getStageCount(); ++s)
            setEnvelope(chain.getEnvelopeMut(s));
    }

    void applyParameter(ParamId id, float value)
    {
        switch (id)
        {
            case Position:
                scanStart = juce::jlimit(0.0f, 1.0f, value);
                for (auto& v : voices)
                    v.setPosition(value);
                break;

            case ScanMode:
                scanMode = static_cast<WavetableScanMode>(juce::jlimit(0, (int) WavetableScanMode::Count - 1, (int) std::lround(value)));
                for (auto& v : voices)
                    v.setScanMode(scanMode);
                break;
            case ScanTime:       scanTime = juce::jlimit(0.05f, 60.0f, value); break;
            case ScanSync:       scanSync = juce::jlimit(0, 2, (int) std::lround(value)); break;
            case ScanLength:     scanLength = juce::jlimit(0, NUM_SCAN_LENGTHS - 1, (int) std::lround(value)); break;
            case ScanSpeed:      scanSpeed = juce::jlimit(0.25f, 4.0f, value); break;
            case ScanSteps:
                for (auto& v : voices)
                    v.setScanSteps(scanStepCount((int) std::lround(value)));
                break;
            case ScanDirection:
                for (auto& v : voices)
                    v.setScanReverse(value > 0.5f);
                break;
            case UserFrames:
            {
                const int option = juce::jlimit(0, NUM_USER_FRAME_OPTIONS - 1, (int) std::lround(value));
                if (option != userFrameOption)
                {
                    userFrameOption = option;
                    auto source = params.find(UserSample);       // rebuild the table that is in use
                    if (source != params.end())
                        buildUserTable((int) std::lround(source->second));
                }
                break;
            }

            case InterpMode:
            {
                int mode = juce::jlimit(0, 3, (int) value);
                for (auto& v : voices)
                    v.setInterpMode(static_cast<WavetableInterpMode>(mode));
                break;
            }

            case Detune:
                for (auto& v : voices)
                    v.setDetune(value);
                break;

            case UnisonSpread:
                unisonSpread = value;
                for (auto& v : voices)
                    v.setSpread(value);
                break;

            case TuneParam:      tune = value; break;
            case BendRangeParam: bendRange = value; break;
            case Vibrato:        vibrato = value; break;
            case MorphDepth:     for (auto& v : voices) v.setMorphDepth(value); break;
            case EnvToPosition:  for (auto& v : voices) v.setEnvToPosition(value); break;
            case VelToPosition:  for (auto& v : voices) v.setVelToPosition(value); break;
            case VelSens:        for (auto& v : voices) v.setVelocitySensitivity(value); break;
            case Glide:          for (auto& v : voices) v.setGlideTime(value); break;

            case SubLevel:
                for (auto& v : voices)
                    v.setSubLevel(value);
                break;

            case MorphSpeed:
                for (auto& v : voices)
                    v.setMorphSpeed(value);
                break;

            case Level:
                masterLevel = juce::jlimit(0.0f, 1.0f, value);
                break;

            case Octave:
                for (auto& v : voices)
                    v.setOctave((int) value);
                break;

            case TableSelect:
                selectTable((int) value);
                break;
            case UserSample:
                buildUserTable((int) std::lround(value));
                break;

            case AmpAttack:
            case AmpDecay:
            case AmpSustain:
            case AmpRelease:
            {
                for (auto& v : voices)
                {
                    auto p = v.getAmpEnvelope().getParams();
                    if (id == AmpAttack) p.attack = value;
                    else if (id == AmpDecay) p.decay = value;
                    else if (id == AmpSustain) p.sustain = value;
                    else if (id == AmpRelease) p.release = value;
                    v.getAmpEnvelope().setParams(p);
                    // Chain stages are copies of this envelope.
                    auto& chain = v.getEnvelopeChain();
                    for (int s = 0; s < chain.getStageCount(); ++s)
                        chain.getEnvelopeMut(s).setParams(p);
                }
                break;
            }

            case EnvChainMode:
            {
                // 0 = off (use simple ADSR), 1 = series, 2 = parallel
                int modeVal = juce::jlimit(0, 2, (int) value);
                for (auto& v : voices)
                {
                    if (modeVal == 0)
                    {
                        v.setEnvelopeChainEnabled(false);
                    }
                    else
                    {
                        v.setEnvelopeChainEnabled(true);
                        auto chainMode = (modeVal == 1)
                            ? EnvelopeChain::ChainMode::Series
                            : EnvelopeChain::ChainMode::Parallel;
                        v.getEnvelopeChain().setMode(chainMode);
                    }
                }
                break;
            }

            case EnvChainStages:
            {
                // Rebuild the chain with N identical envelope stages
                // using the current ADSR parameters
                int stages = juce::jlimit(1, EnvelopeChain::MaxEnvelopes, (int) value);
                for (auto& v : voices)
                {
                    auto& chain = v.getEnvelopeChain();
                    chain.reset();
                    // Reconstruct the chain (reset clears active state, not stages;
                    // we rebuild by creating a new chain)
                    EnvelopeChain newChain;
                    newChain.setSampleRate(sampleRate);
                    newChain.setMode(chain.getMode());
                    newChain.setCombineMode(chain.getCombineMode());
                    auto p = v.getAmpEnvelope().getParams();
                    for (int s = 0; s < stages; ++s)
                        newChain.addEnvelope(p);
                    v.getEnvelopeChain() = newChain;
                }
                break;
            }

            case EnvChainCombine:
            {
                // 0 = sum, 1 = max
                auto combineMode = ((int) value == 0)
                    ? EnvelopeChain::CombineMode::Sum
                    : EnvelopeChain::CombineMode::Max;
                for (auto& v : voices)
                    v.getEnvelopeChain().setCombineMode(combineMode);
                break;
            }

            default:
                break;
        }
    }

    double sampleRate = 44100.0;
    float masterLevel = 0.8f;
    float unisonSpread = 0.0f;
    float tune = 0.0f, bend = 0.0f, bendRange = 2.0f, wheel = 0.0f, vibrato = 0.0f;
    float wheelNow = 0.0f;                              // the strip, smoothed per block
    double vibratoPhase = 0.0;
    int currentTableIndex = 0;

    // Timed scan (SCAN page)
    WavetableScanMode scanMode = WavetableScanMode::Off;
    float scanTime = 2.0f, scanSpeed = 1.0f, scanStart = 0.0f, tempo = 120.0f;
    int scanSync = 0, scanLength = 4, userFrameOption = 0;
    double userSeconds = 0.0, userPendingSeconds = 0.0;

    VoiceFilterSection filterSection { FilterBase };
    EngineLfoSection lfoSection { lfoDests() };

    VoiceAllocator allocator;
    std::vector<WavetableVoice> voices;
    std::vector<Wavetable> tables;
    ParamMap params;
    Wavetable userPending;
    juce::SpinLock userLock;
    std::atomic<bool> userReady { false };
};
