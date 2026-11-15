// Compiled with -O2 in every build type (CMakeLists.txt): see fast_measure.h.
#include "fast_measure.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>

namespace fastmeasure
{
    static const float twoPi = 6.283185307179586476925286766559f;
    static const float pi = 3.1415926535897932384626433832795f;

    static void fft(std::vector<std::complex<float>>& a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const float ang = -2.0f * pi / (float) len;
            const std::complex<float> wl(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<float> w(1.0f, 0.0f);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    std::vector<std::vector<float>> frames(const std::vector<float>& x, int fftSize)
    {
        std::vector<std::vector<float>> out;
        std::vector<std::complex<float>> buf((size_t) fftSize);
        for (size_t start = 0; start + (size_t) fftSize <= x.size(); start += (size_t) fftSize / 2)
        {
            for (int i = 0; i < fftSize; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos(twoPi * (float) i / (float) fftSize);
                buf[(size_t) i] = { x[start + (size_t) i] * w, 0.0f };
            }
            fft(buf);
            std::vector<float> mag((size_t) fftSize / 2);
            for (int i = 0; i < fftSize / 2; ++i)
                mag[(size_t) i] = std::sqrt(std::abs(buf[(size_t) i]));
            out.push_back(std::move(mag));
        }
        return out;
    }

    float difference(const std::vector<float>& a, const std::vector<float>& b, int fftSize)
    {
        const auto fa = frames(a, fftSize), fb = frames(b, fftSize);
        double diff = 0.0, sum = 0.0;
        for (size_t f = 0; f < std::min(fa.size(), fb.size()); ++f)
            for (size_t i = 0; i < fa[f].size(); ++i)
            {
                diff += std::abs(fa[f][i] - fb[f][i]);
                sum += fa[f][i] + fb[f][i];
            }
        return sum > 0.0 ? (float) (diff / sum) : 0.0f;
    }

    void rmsAndCentroid(const std::vector<float>& x, int fftSize, double sampleRate, float& rms, float& centroid)
    {
        double sq = 0.0;
        for (float s : x) sq += (double) s * s;
        rms = x.empty() ? 0.0f : (float) std::sqrt(sq / (double) x.size());
        double num = 0.0, den = 0.0;
        for (auto& frame : frames(x, fftSize))
            for (size_t i = 1; i < frame.size(); ++i)
            {
                const double p = std::pow((double) frame[i], 4.0);     // back to power
                num += p * (double) i * sampleRate / fftSize;
                den += p;
            }
        centroid = den > 0.0 ? (float) (num / den) : 0.0f;
    }

    void fft(std::vector<std::complex<double>>& a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double angle = -2.0 * 3.14159265358979323846 / (double) len;
            const std::complex<double> wl(std::cos(angle), std::sin(angle));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<double> w(1.0);
                for (size_t j = 0; j < len / 2; ++j)
                {
                    const auto u = a[i + j], v = a[i + j + len / 2] * w;
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    double goertzel(const std::vector<float>& x, size_t from, size_t n, double hz, double sampleRate)
    {
        const double PI = 3.141592653589793;
        const double w = 2.0 * PI * hz / sampleRate, c = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = 0; i < n && from + i < x.size(); ++i)
        {
            const double win = 0.5 - 0.5 * std::cos(2.0 * PI * (double) i / (double) n);
            const double s0 = win * x[from + i] + c * s1 - s2;
            s2 = s1; s1 = s0;
        }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    }

    double harmonic(const std::vector<float>& x, double hz, int h, double sampleRate)
    {
        double c = 0.0, s = 0.0;
        for (size_t n = 0; n < x.size(); ++n)
        {
            const double a = 2.0 * 3.14159265358979323846 * hz * (double) h * (double) n / sampleRate;
            c += x[n] * std::cos(a);
            s += x[n] * std::sin(a);
        }
        return 2.0 * std::sqrt(c * c + s * s) / (double) x.size();
    }

    int bestLag(const std::vector<float>& x, int start, double expected, double& correlation)
    {
        const int lo = (int) (expected * 0.6), hi = (int) (expected * 1.4), points = 1500;
        const int span = (int) x.size() - start - hi - 1;
        int best = 0;
        correlation = -1.0;
        if (span < points)
            return 0;
        const int stride = span / points;
        double mean = 0.0;                      // the bowed string sits off centre
        for (size_t i = (size_t) start; i < x.size(); ++i)
            mean += x[i];
        mean /= (double) (x.size() - (size_t) start);
        for (int lag = lo; lag <= hi; ++lag)
        {
            double ab = 0.0, aa = 0.0, bb = 0.0;
            for (int k = 0; k < points; ++k)
            {
                const double a = x[(size_t) (start + k * stride)] - mean, b = x[(size_t) (start + k * stride + lag)] - mean;
                ab += a * b; aa += a * a; bb += b * b;
            }
            const double c = ab / std::sqrt(std::max(aa * bb, 1.0e-30));
            if (c > correlation) { correlation = c; best = lag; }
        }
        return best;
    }
}
