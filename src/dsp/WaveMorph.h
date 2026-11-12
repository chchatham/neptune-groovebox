#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

// =============================================================================
// Single-cycle waves as spectra, and ways of getting from one wave to another.
//
// A wavetable frame is one cycle of 256 samples, so it is exactly a set of harmonics
// 1 .. 127 with a level and a phase each. Everything here works on that set
// (Spectrum): the key-frame tables of the wavetable engine are built by computing
// the frames between two key waves with one of the algorithms in morph().
//
// Nothing here runs on the audio thread (it allocates); tables are built on the
// thread that changes a parameter and handed over as the USER table is.
// =============================================================================
namespace wavemorph
{
    constexpr int N = 256;                  // samples in a frame
    constexpr int H = N / 2 - 1;            // highest harmonic of a frame (127)
    constexpr double PI = 3.14159265358979323846;
    constexpr double TWO_PI = 2.0 * PI;

    using Frame = std::array<float, N>;
    using Complex = std::complex<double>;

    // In-place radix-2 Fourier transform (Cooley & Tukey, "An Algorithm for the Machine
    // Calculation of Complex Fourier Series", Math. Comp. 19, 1965). n is a power of two;
    // neither direction is scaled.
    inline void fft(Complex* a, int n, bool inverse)
    {
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(a[i], a[j]);
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const double angle = TWO_PI / (double) len * (inverse ? 1.0 : -1.0);
            const double wr = std::cos(angle), wi = std::sin(angle);
            const int half = len / 2;
            for (int i = 0; i < n; i += len)
            {
                double cr = 1.0, ci = 0.0;
                for (int k = 0; k < half; ++k)
                {
                    const double ur = a[i + k].real(), ui = a[i + k].imag();
                    const double xr = a[i + k + half].real(), xi = a[i + k + half].imag();
                    const double vr = xr * cr - xi * ci, vi = xr * ci + xi * cr;
                    a[i + k] = { ur + vr, ui + vi };
                    a[i + k + half] = { ur - vr, ui - vi };
                    const double nr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;
                    cr = nr;
                }
            }
        }
    }

    // The harmonics of one cycle:  x[n] = sum over h = 1 .. H of Re(c[h] * exp(i 2 pi h n / N)).
    // c[0] stays zero: a wave has no DC.
    struct Spectrum
    {
        std::array<Complex, H + 1> c {};

        double magnitude(int h) const { return std::abs(c[(size_t) h]); }
        double energy() const
        {
            double e = 0.0;
            for (int h = 1; h <= H; ++h)
                e += std::norm(c[(size_t) h]);
            return e;
        }
    };

    // One cycle given as n samples (n a power of two, 256 or more): its harmonics 1 .. 127. Anything
    // above is dropped, which is the band-limiting when a long period is brought down to a frame.
    inline Spectrum analyse(const float* x, int n)
    {
        std::vector<Complex> buf((size_t) n);
        for (int i = 0; i < n; ++i)
            buf[(size_t) i] = { (double) x[i], 0.0 };
        fft(buf.data(), n, false);
        Spectrum s;
        for (int h = 1; h <= H && h < n / 2; ++h)
            s.c[(size_t) h] = buf[(size_t) h] * (2.0 / (double) n);
        return s;
    }
    inline Spectrum analyse(const Frame& f) { return analyse(f.data(), N); }

    // The frame of a spectrum, keeping harmonics 1 .. maxHarmonic.
    inline Frame synthesise(const Spectrum& s, int maxHarmonic = H)
    {
        std::array<Complex, N> buf {};
        for (int h = 1; h <= H && h <= maxHarmonic; ++h)
        {
            buf[(size_t) h] = s.c[(size_t) h] * 0.5;
            buf[(size_t) (N - h)] = std::conj(s.c[(size_t) h]) * 0.5;
        }
        fft(buf.data(), N, true);
        Frame f;
        for (int n = 0; n < N; ++n)
            f[(size_t) n] = (float) buf[(size_t) n].real();
        return f;
    }

    inline float peakOf(const Frame& f)
    {
        float p = 0.0f;
        for (float v : f)
            p = std::max(p, std::abs(v));
        return p;
    }
    inline float rmsOf(const Frame& f)
    {
        double sq = 0.0;
        for (float v : f)
            sq += (double) v * v;
        return (float) std::sqrt(sq / (double) N);
    }
    inline void scale(Frame& f, float g)
    {
        for (float& v : f)
            v *= g;
    }
    inline void scale(Spectrum& s, double g)
    {
        for (auto& c : s.c)
            c *= g;
    }

    inline double wrapPi(double a)
    {
        a = std::fmod(a + PI, TWO_PI);
        if (a < 0.0)
            a += TWO_PI;
        return a - PI;
    }

    // Turns the cycle in time (which changes no level) until its lowest strong harmonic starts like
    // a sine. Waves cut from different places and different samples then line up, so a crossfade
    // between two of them does not cancel their fundamentals.
    inline void alignToFundamental(Spectrum& s)
    {
        double top = 0.0;
        for (int h = 1; h <= H; ++h)
            top = std::max(top, s.magnitude(h));
        if (top <= 0.0)
            return;
        int ref = 1;
        while (ref < H && s.magnitude(ref) < 0.05 * top)
            ++ref;
        const double shift = (-0.5 * PI - std::arg(s.c[(size_t) ref])) / (double) ref;
        for (int h = 1; h <= H; ++h)
            s.c[(size_t) h] *= std::polar(1.0, shift * (double) h);
    }

    // dB per octave on every harmonic (negative = darker); harmonics above `highest` are removed.
    inline void tiltAndLimit(Spectrum& s, double dbPerOctave, int highest)
    {
        for (int h = 1; h <= H; ++h)
        {
            if (h > highest)
                s.c[(size_t) h] = 0.0;
            else if (dbPerOctave != 0.0)
                s.c[(size_t) h] *= std::pow((double) h, dbPerOctave / 6.0206);
        }
    }

    // What an early digital wavetable instrument kept of a wave: 128 samples of 8 bits. The frame
    // is first limited to the 63 harmonics that fit 128 samples, then every second sample is held
    // and rounded to 1/127. The steps this leaves in the frame are part of the sound: read without
    // band-limiting (the wavetable engine's INTP = CLSC) they are what aliases.
    inline Frame classicFrame(const Spectrum& s)
    {
        Frame f = synthesise(s, 63);
        for (int n = 0; n < N; n += 2)
        {
            const float v = std::round(std::max(-1.0f, std::min(1.0f, f[(size_t) n])) * 127.0f) / 127.0f;
            f[(size_t) n] = v;
            f[(size_t) n + 1] = v;
        }
        return f;
    }

    // ---- From one wave to the next ------------------------------------------------------------
    //
    // Linear     the two waves are crossfaded sample by sample (the same as crossfading every
    //            harmonic as a complex number): a harmonic that the two waves hold in opposite
    //            phase passes through silence, and a peak that sits at a different harmonic in
    //            each fades out in one place while it fades in at the other.
    // Step       no in-between: the nearer wave.
    // Classic    Linear; the builder then stores each frame as classicFrame().
    // Phase      level and phase of every harmonic separately: the level goes straight from one
    //            to the other and the phase turns the short way round, so nothing cancels.
    // Transport  see transport().
    // Vocoder    see vocoder().
    enum class Algo { Linear, Step, Classic, Phase, Transport, Vocoder, Count };

    inline Spectrum crossfade(const Spectrum& a, const Spectrum& b, double t)
    {
        Spectrum out;
        for (int h = 1; h <= H; ++h)
            out.c[(size_t) h] = a.c[(size_t) h] * (1.0 - t) + b.c[(size_t) h] * t;
        return out;
    }

    // A phase between two phases, the short way round. A harmonic that one wave does not have
    // takes the phase of the wave that has it.
    inline double phaseBetween(const Complex& a, const Complex& b, double t)
    {
        const double ma = std::abs(a), mb = std::abs(b);
        if (ma <= 1.0e-12 * (mb + 1.0e-300)) return std::arg(b);
        if (mb <= 1.0e-12 * (ma + 1.0e-300)) return std::arg(a);
        const double pa = std::arg(a);
        return pa + t * wrapPi(std::arg(b) - pa);
    }

    inline Spectrum phaseAligned(const Spectrum& a, const Spectrum& b, double t)
    {
        Spectrum out;
        for (int h = 1; h <= H; ++h)
        {
            const Complex& ca = a.c[(size_t) h];
            const Complex& cb = b.c[(size_t) h];
            out.c[(size_t) h] = std::polar((1.0 - t) * std::abs(ca) + t * std::abs(cb), phaseBetween(ca, cb, t));
        }
        return out;
    }

    // Optimal transport (displacement interpolation: McCann, "A Convexity Principle for Interacting
    // Gases", Adv. Math. 128, 1997; applied to audio spectra by Henderson & Solomon, "Audio
    // Transport: A Generalized Portamento via Optimal Transport", Proc. DAFx 2019).
    //
    // What is moved: the ENERGY of the wave, as a distribution over its harmonics (each wave's
    // energy scaled to 1; the total goes straight from one wave's to the other's). Where a
    // harmonic sits is measured in octaves (log2 of its number), so the cost of moving energy is
    // the musical interval it travels. On a line the cheapest plan is the one that never lets two
    // parcels cross (the monotone plan, read off the two cumulative distributions), so it is found
    // in one pass with no optimiser. At time t each parcel is (1 - t) of the way from its harmonic
    // in the first wave to its harmonic in the second: a formant or a single partial GLIDES across
    // the spectrum instead of fading out in one place and in at another.
    //
    // A frame can only hold whole harmonics, so a parcel between two of them is shared between the
    // two by distance (in energy, i.e. an equal-power pan). Phase: each parcel turns the short way
    // from the phase of the harmonic it left to the phase of the one it arrives at; a harmonic's
    // phase is that of the energy-weighted sum of the parcels on it, its level the root of their
    // energy. At t = 0 and t = 1 this gives the two waves exactly.
    inline Spectrum transport(const Spectrum& a, const Spectrum& b, double t)
    {
        const double ea = a.energy(), eb = b.energy();
        if (ea <= 1.0e-18 || eb <= 1.0e-18)
            return crossfade(a, b, t);

        std::array<double, H + 2> power {};
        std::array<Complex, H + 2> phasor {};
        int i = 1, j = 1;
        double ra = std::norm(a.c[1]) / ea, rb = std::norm(b.c[1]) / eb;
        while (i <= H && j <= H)
        {
            const double m = std::min(ra, rb);
            if (m > 0.0)
            {
                const double at = std::exp2((1.0 - t) * std::log2((double) i) + t * std::log2((double) j));
                int lo = (int) std::floor(at + 1.0e-9);
                lo = std::max(1, std::min(H, lo));
                const double frac = lo < H ? std::max(0.0, std::min(1.0, at - (double) lo)) : 0.0;
                const Complex turn = std::polar(1.0, phaseBetween(a.c[(size_t) i], b.c[(size_t) j], t));
                power[(size_t) lo] += m * (1.0 - frac);
                phasor[(size_t) lo] += turn * (m * (1.0 - frac));
                power[(size_t) lo + 1] += m * frac;
                phasor[(size_t) lo + 1] += turn * (m * frac);
            }
            ra -= m;
            rb -= m;
            if (ra <= 1.0e-15) { ++i; ra = i <= H ? std::norm(a.c[(size_t) i]) / ea : 0.0; }
            if (rb <= 1.0e-15) { ++j; rb = j <= H ? std::norm(b.c[(size_t) j]) / eb : 0.0; }
        }

        const double total = (1.0 - t) * ea + t * eb;
        Spectrum out;
        for (int h = 1; h <= H; ++h)
        {
            const double mag = std::abs(phasor[(size_t) h]);
            if (power[(size_t) h] > 0.0)
                out.c[(size_t) h] = (mag > 1.0e-300 ? phasor[(size_t) h] / mag : Complex(1.0, 0.0))
                                    * std::sqrt(total * power[(size_t) h]);
        }
        return out;
    }

    // The smooth outline of a spectrum (its formants): the root-mean-square level over a
    // triangular window of three harmonics either side. Never zero.
    inline std::array<double, H + 1> envelopeOf(const Spectrum& s)
    {
        constexpr int R = 3;
        std::array<double, H + 1> env {};
        for (int h = 1; h <= H; ++h)
        {
            double sum = 0.0, weight = 0.0;
            for (int k = -R; k <= R; ++k)
            {
                const int g = h + k;
                if (g < 1 || g > H)
                    continue;
                const double w = (double) (R + 1 - std::abs(k));
                sum += w * std::norm(s.c[(size_t) g]);
                weight += w;
            }
            env[(size_t) h] = std::sqrt(sum / weight) + 1.0e-9;
        }
        return env;
    }

    // Vocoder-style blend (the channel vocoder's split of a sound into a slowly varying spectral
    // envelope and what excites it: Dudley, "The Vocoder", Bell Labs Record 18, 1939; cross-
    // synthesis by putting one sound's envelope on another's excitation).
    //
    // Each wave is split into its envelope (envelopeOf) and its fine structure: every harmonic
    // divided by the envelope there, which keeps which harmonics are present (odd only, every
    // third ...) and all the phases. The two parts travel one after the other:
    //     first half    the envelope goes straight from the first wave's to the second's, on the
    //                   FIRST wave's fine structure;
    //     half way      the second wave's envelope on the first wave's fine structure: the cross-
    //                   synthesis of the two;
    //     second half   the fine structure goes over to the second wave's (levels straight,
    //                   phases the short way round) under the second wave's envelope.
    // At t = 0 and t = 1 this gives the two waves exactly.
    inline Spectrum vocoder(const Spectrum& a, const Spectrum& b, double t)
    {
        const auto envA = envelopeOf(a), envB = envelopeOf(b);
        const double te = std::max(0.0, std::min(1.0, 2.0 * t));
        const double tx = std::max(0.0, std::min(1.0, 2.0 * t - 1.0));
        Spectrum out;
        for (int h = 1; h <= H; ++h)
        {
            const double env = (1.0 - te) * envA[(size_t) h] + te * envB[(size_t) h];
            const Complex fineA = a.c[(size_t) h] / envA[(size_t) h];
            const Complex fineB = b.c[(size_t) h] / envB[(size_t) h];
            const double fine = (1.0 - tx) * std::abs(fineA) + tx * std::abs(fineB);
            out.c[(size_t) h] = std::polar(env * fine, phaseBetween(fineA, fineB, tx));
        }
        return out;
    }

    inline Spectrum morph(Algo algo, const Spectrum& a, const Spectrum& b, double t)
    {
        if (t <= 0.0) return a;
        if (t >= 1.0) return b;
        switch (algo)
        {
            case Algo::Step:      return t < 0.5 ? a : b;
            case Algo::Phase:     return phaseAligned(a, b, t);
            case Algo::Transport: return transport(a, b, t);
            case Algo::Vocoder:   return vocoder(a, b, t);
            case Algo::Linear:
            case Algo::Classic:
            case Algo::Count:     break;
        }
        return crossfade(a, b, t);
    }
}
