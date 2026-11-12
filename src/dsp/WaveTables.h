#pragma once

#include "WaveMorph.h"
#include <cstdint>
#include <vector>

// =============================================================================
// More built-in wavetables, every one computed here from a formula: no table or
// wave data from any instrument is used, and the names say what the table is.
//
// Each table is 64 frames. A frame is made either from a list of harmonics, or
// from a function of the phase that is evaluated at eight times the frame's
// resolution and then reduced to the 127 harmonics a frame can hold, so no
// frame carries aliasing of its own. Every frame is free of DC and brought to
// the same loudness (FRAME_RMS), or to a peak of 1 if that comes first.
// =============================================================================
namespace wavetables
{
    using wavemorph::Complex;
    using wavemorph::Frame;
    using wavemorph::H;
    using wavemorph::N;
    using wavemorph::PI;
    using wavemorph::Spectrum;
    using wavemorph::TWO_PI;

    constexpr int FRAMES = 64;
    constexpr float FRAME_RMS = 0.35f;

    // Same loudness for every frame; a frame too peaky for that is brought to a peak of 1.
    inline void level(Frame& f)
    {
        const float rms = wavemorph::rmsOf(f);
        if (rms <= 1.0e-9f)
            return;
        wavemorph::scale(f, FRAME_RMS / rms);
        const float peak = wavemorph::peakOf(f);
        if (peak > 1.0f)
            wavemorph::scale(f, 1.0f / peak);
    }

    // A set of harmonics: level and phase (0 = starts like a sine) of harmonic h in amp[h], phase[h].
    struct Harmonics
    {
        std::array<double, H + 1> amp {};
        std::array<double, H + 1> phase {};

        Frame frame() const
        {
            Spectrum s;
            for (int h = 1; h <= H; ++h)
                s.c[(size_t) h] = std::polar(amp[(size_t) h], phase[(size_t) h] - 0.5 * PI);
            Frame f = wavemorph::synthesise(s);
            level(f);
            return f;
        }
    };

    // A fixed phase for each harmonic that is the same in every frame of a table (so neighbouring
    // frames never cancel) and spreads the harmonics over the cycle (so many harmonics of similar
    // level do not pile up into one spike, which would cost the frame its loudness).
    inline double scatteredPhase(int h)
    {
        const double x = 0.5 * 0.6180339887498949 * (double) h * (double) h;
        return TWO_PI * (x - std::floor(x));
    }

    // One frame from a function of the phase 0 .. 1.
    template <typename Fn>
    Frame fromFunction(Fn&& fn)
    {
        constexpr int M = 8 * N;
        std::vector<float> x((size_t) M);
        for (int i = 0; i < M; ++i)
            x[(size_t) i] = (float) fn(((double) i + 0.5) / (double) M);
        Frame f = wavemorph::synthesise(wavemorph::analyse(x.data(), M));
        level(f);
        return f;
    }

    template <typename Fn>
    std::vector<Frame> table(Fn&& frameAt)        // frameAt(t), t = 0 .. 1 along the table
    {
        std::vector<Frame> frames((size_t) FRAMES);
        for (int f = 0; f < FRAMES; ++f)
            frames[(size_t) f] = frameAt((double) f / (double) (FRAMES - 1));
        return frames;
    }

    // A two-pole resonance at harmonic number `centre` (level and phase of harmonic h).
    inline Complex resonance(double h, double centre, double q)
    {
        const double x = h / centre;
        return 1.0 / Complex(1.0 - x * x, x / q);
    }

    // ---- VOWL: the vowels A E I O U, one after the other ----------------------------------------
    // A source falling 6 dB per octave through three formant resonances in series (a cascade
    // formant synthesiser: Klatt, "Software for a cascade/parallel formant synthesizer", JASA 67,
    // 1980). Formant frequencies: the adult male averages of Peterson & Barney, "Control Methods
    // Used in a Study of the Vowels", JASA 24, 1952, taken as harmonics of a 110 Hz voice.
    inline std::vector<Frame> vowels()
    {
        static const double formants[5][3] = {
            { 730, 1090, 2440 }, { 530, 1840, 2480 }, { 270, 2290, 3010 }, { 570, 840, 2410 }, { 300, 870, 2240 } };
        static const double bandwidth[3] = { 80, 100, 140 };
        return table([](double t)
        {
            const double v = t * 4.0;
            const int a = std::min(3, (int) v);
            const double mix = v - (double) a;
            Harmonics hm;
            for (int h = 1; h <= H; ++h)
            {
                Complex c(1.0 / (double) h, 0.0);
                for (int k = 0; k < 3; ++k)
                {
                    const double hz = formants[a][k] * (1.0 - mix) + formants[a + 1][k] * mix;
                    c *= resonance((double) h, hz / 110.0, hz / bandwidth[k]);
                }
                hm.amp[(size_t) h] = std::abs(c);
                hm.phase[(size_t) h] = std::arg(c);
            }
            return hm.frame();
        });
    }

    // ---- SYNC: oscillator sync. A sine restarted at every cycle of the key's pitch, running 1 to
    // 8 times as fast and fading towards the restart (a sawtooth restarted that way has edges that
    // jump from frame to frame; this one scans smoothly and keeps the sweeping sync formant).
    inline std::vector<Frame> hardSync()
    {
        return table([](double t)
        {
            const double ratio = 1.0 + 7.0 * t;
            return fromFunction([ratio](double p) { return std::sin(TWO_PI * p * ratio) * (1.0 - p); });
        });
    }

    // ---- FOLD: a sine driven harder and harder into a folder that turns it back at the rails ----
    inline std::vector<Frame> wavefold()
    {
        return table([](double t)
        {
            const double gain = 1.0 + 8.0 * t, offset = 0.6 * t;
            return fromFunction([gain, offset](double p) { return std::sin(0.5 * PI * gain * std::sin(TWO_PI * p) + offset); });
        });
    }

    // ---- FM1, FM3: a sine whose phase is moved by another sine (Chowning, "The Synthesis of
    // Complex Audio Spectra by Means of Frequency Modulation", JAES 21, 1973), the index rising
    // along the table. Modulator at the pitch of the carrier (FM1) or three times it (FM3).
    inline std::vector<Frame> fm(int ratio, double maxIndex)
    {
        return table([ratio, maxIndex](double t)
        {
            const double index = maxIndex * t;
            return fromFunction([ratio, index](double p) { return std::sin(TWO_PI * p + index * std::sin(TWO_PI * (double) ratio * p)); });
        });
    }

    // ---- BELL: partials spaced like those of a struck bar ((2k + 1)^2 / 9: 1, 2.8, 5.4, 9 ...),
    // each on the nearest harmonic. From a soft strike (hardly more than the fundamental) to a
    // hard one (every partial).
    inline std::vector<Frame> bell()
    {
        return table([](double t)
        {
            Harmonics hm;
            const double decay = 1.6 * std::pow(0.04, t);       // per partial: 1.6 -> 0.064
            for (int k = 1; k <= 16; ++k)
            {
                const int h = (int) std::lround((2.0 * k + 1.0) * (2.0 * k + 1.0) / 9.0);
                if (h > H)
                    break;
                hm.amp[(size_t) h] = std::exp(-decay * (double) (k - 1));
                hm.phase[(size_t) h] = scatteredPhase(h);
            }
            return hm.frame();
        });
    }

    // ---- ORGN: a drawbar organ pulled out stop by stop. The key plays the lowest rank; the nine
    // drawbars are harmonics 1 2 3 4 6 8 10 12 16 of it. A drawbar step is 3 dB.
    inline std::vector<Frame> organ()
    {
        static const int harmonic[9] = { 1, 2, 3, 4, 6, 8, 10, 12, 16 };
        static const double registration[7][9] = {
            { 8, 0, 0, 0, 0, 0, 0, 0, 0 }, { 8, 8, 0, 0, 0, 0, 0, 0, 0 }, { 8, 8, 8, 0, 0, 0, 0, 0, 0 },
            { 8, 8, 8, 8, 0, 0, 0, 0, 0 }, { 8, 6, 8, 6, 5, 4, 3, 2, 2 }, { 8, 8, 8, 8, 8, 8, 8, 8, 8 },
            { 5, 0, 0, 4, 0, 8, 0, 8, 8 } };
        return table([](double t)
        {
            const double v = t * 6.0;
            const int a = std::min(5, (int) v);
            const double mix = v - (double) a;
            Harmonics hm;
            for (int d = 0; d < 9; ++d)
            {
                const double bar = registration[a][d] * (1.0 - mix) + registration[a + 1][d] * mix;
                if (bar <= 0.0)
                    continue;
                hm.amp[(size_t) harmonic[d]] = std::min(1.0, bar) * std::pow(10.0, -3.0 * (8.0 - bar) / 20.0);
                hm.phase[(size_t) harmonic[d]] = scatteredPhase(harmonic[d]);
            }
            return hm.frame();
        });
    }

    // ---- NOIS: from noise to a tone. Every harmonic at a random level and phase (one cycle of
    // noise), the upper ones falling away along the table until the fundamental is left.
    inline std::vector<Frame> noiseToTone()
    {
        std::array<double, H + 1> level {}, phase {};
        uint32_t lcg = 0x2F6E2B1u;
        auto next = [&lcg] { lcg = lcg * 1664525u + 1013904223u; return (double) (lcg >> 8) / 16777216.0; };
        for (int h = 1; h <= H; ++h)
        {
            level[(size_t) h] = 0.3 + 0.7 * next();
            phase[(size_t) h] = TWO_PI * next();
        }
        return table([&](double t)
        {
            Harmonics hm;
            const double slope = 0.3 + 4.7 * t;
            for (int h = 1; h <= H; ++h)
            {
                hm.amp[(size_t) h] = std::pow(level[(size_t) h], 1.0 - t) * std::pow((double) h, -slope);
                hm.phase[(size_t) h] = phase[(size_t) h];
            }
            return hm.frame();
        });
    }

    // ---- RESO: a sawtooth through a resonant low-pass whose cutoff opens along the table --------
    inline std::vector<Frame> resonantSweep()
    {
        return table([](double t)
        {
            Harmonics hm;
            const double cutoff = 1.5 * std::exp2(5.5 * t);
            for (int h = 1; h <= H; ++h)
            {
                const Complex c = resonance((double) h, cutoff, 3.5) / (double) h;
                hm.amp[(size_t) h] = std::abs(c);
                hm.phase[(size_t) h] = std::arg(c);
            }
            return hm.frame();
        });
    }

    // ---- WLSH: Walsh functions, the square-cornered counterpart of harmonics (Walsh, "A Closed
    // Set of Normal Orthogonal Functions", Amer. J. Math. 45, 1923), in order of how often they
    // change sign; more and more of them are added along the table (with signs that keep them from
    // piling up at the start of the cycle).
    inline double walsh(int k, double p)
    {
        const int gray = k ^ (k >> 1);
        double v = 1.0;
        for (int bit = 0; bit < 8; ++bit)
            if ((gray >> bit) & 1)
                if (((int) std::floor(p * (double) (2 << bit))) & 1)
                    v = -v;
        return v;
    }
    inline std::vector<Frame> walshSet()
    {
        return table([](double t)
        {
            const double count = 1.0 + 31.0 * t;
            return fromFunction([count](double p)
            {
                double sum = 0.0;
                for (int k = 1; k <= 32; ++k)
                {
                    const double w = std::max(0.0, std::min(1.0, count - (double) (k - 1)));
                    if (w <= 0.0)
                        break;
                    sum += w * walsh(k, p) * (((0xB5297A4Du >> (k - 1)) & 1u) ? 1.0 : -1.0) / std::sqrt((double) k);
                }
                return sum;
            });
        });
    }

    // ---- PRIM: the fundamental and the prime-numbered harmonics, more of them along the table ---
    inline std::vector<Frame> primes()
    {
        std::vector<int> list { 1 };
        for (int n = 2; n <= H; ++n)
        {
            bool prime = true;
            for (int d = 2; d * d <= n; ++d)
                if (n % d == 0) { prime = false; break; }
            if (prime)
                list.push_back(n);
        }
        return table([list](double t)
        {
            Harmonics hm;
            const double count = 1.0 + ((double) list.size() - 1.0) * t;
            for (size_t k = 0; k < list.size(); ++k)
            {
                const double w = std::max(0.0, std::min(1.0, count - (double) k));
                hm.amp[(size_t) list[k]] = w * std::pow((double) list[k], -0.7);
                hm.phase[(size_t) list[k]] = scatteredPhase(list[k]);
            }
            return hm.frame();
        });
    }

    // ---- OCTV: only octaves (harmonics 1 2 4 8 16 32); the weight climbs from the bottom octave
    // to the top ones along the table, the fundamental never quite leaving.
    inline std::vector<Frame> octaves()
    {
        return table([](double t)
        {
            Harmonics hm;
            const double centre = 4.2 * t;
            for (int j = 0; j <= 5; ++j)
            {
                const double d = ((double) j - centre) / 1.1;
                hm.amp[(size_t) (1 << j)] = std::max(j == 0 ? 0.12 : 0.0, std::exp(-0.5 * d * d));
                hm.phase[(size_t) (1 << j)] = scatteredPhase(1 << j);
            }
            return hm.frame();
        });
    }

    // ---- PDST: phase distortion. A cosine read at an uneven speed, faster and faster through its
    // first half: from a sine to a sawtooth-like wave.
    inline std::vector<Frame> phaseDistortion()
    {
        return table([](double t)
        {
            const double knee = 0.5 * std::pow(0.04, t);        // 0.5 -> 0.02
            return fromFunction([knee](double p)
            {
                const double w = p < knee ? 0.5 * p / knee : 0.5 + 0.5 * (p - knee) / (1.0 - knee);
                return -std::cos(TWO_PI * w);
            });
        });
    }

    // ---- COMB: a sawtooth and a copy of it that slides away by up to half a cycle. The notches of
    // the comb this makes move down through the harmonics.
    inline std::vector<Frame> combSaw()
    {
        return table([](double t)
        {
            Harmonics hm;
            const double delay = 0.02 + 0.46 * t;
            for (int h = 1; h <= H; ++h)
            {
                const Complex c = (1.0 + std::polar(1.0, -TWO_PI * (double) h * delay)) * (0.5 / (double) h);
                hm.amp[(size_t) h] = std::abs(c);
                hm.phase[(size_t) h] = std::arg(c);
            }
            return hm.frame();
        });
    }

    struct Definition
    {
        const char* label;                      // what the TABL cell shows
        std::vector<Frame> (*make)();
    };

    // The order is stored in projects (TABL is an index): add new tables at the end only.
    inline const std::vector<Definition>& definitions()
    {
        static const std::vector<Definition> list = {
            { "VOWL", vowels },
            { "SYNC", hardSync },
            { "FOLD", wavefold },
            { "FM1",  [] { return fm(1, 8.0); } },
            { "FM3",  [] { return fm(3, 5.0); } },
            { "BELL", bell },
            { "ORGN", organ },
            { "NOIS", noiseToTone },
            { "RESO", resonantSweep },
            { "WLSH", walshSet },
            { "PRIM", primes },
            { "OCTV", octaves },
            { "PDST", phaseDistortion },
            { "COMB", combSaw },
        };
        return list;
    }
}
