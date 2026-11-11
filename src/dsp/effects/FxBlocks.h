#pragma once

// Shared building blocks for the effects added in iteration 51 (ring modulator, vibrato, wah, amp,
// graphic EQ, tape delay, L/C/R delay). Nothing here is new DSP: each block is a standard
// structure, named with its reference.

#include "../DSPUtils.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace fxb
{

// Values this small are treated as zero so recursive states never sit in the denormal range.
inline float flushDenormal(float x) { return std::abs(x) < 1.0e-15f ? 0.0f : x; }

// Bounded, monotonic saturator: the project's tanh approximation (dsp::fastTanh) is only tanh-like
// for |x| <= 3 (beyond that it grows as x / 9), so the input is clamped there. At 3 it reaches
// exactly 1 with zero slope, which makes the clamp seamless. Output is always within -1..1.
inline float softSat(float x) { return dsp::fastTanh(std::clamp(x, -3.0f, 3.0f)); }

// One-pole parameter smoother (exponential approach, y += a (target - y)).
struct Smoother
{
    void setTime(double sampleRate, float milliseconds)
    {
        a = 1.0f - std::exp(-1.0f / std::max(1.0f, milliseconds * 0.001f * (float) sampleRate));
    }

    void snap(float v) { z = v; }
    float value() const { return z; }

    float next(float target)
    {
        float d = target - z;
        if (std::abs(d) < 1.0e-9f)
            z = target;
        else
            z += a * d;
        return z;
    }

    float z = 0.0f;
    float a = 1.0f;
};

// Second-order section with the coefficient formulas from Robert Bristow-Johnson's "Cookbook
// formulae for audio EQ biquad filter coefficients", run in transposed direct form II.
struct Biquad
{
    void reset() { s1 = s2 = 0.0f; }

    float process(float x)
    {
        float y = b0 * x + s1;
        s1 = flushDenormal(b1 * x - a1 * y + s2);
        s2 = flushDenormal(b2 * x - a2 * y);
        return y;
    }

    void setLowPass(double sr, double hz, double q)
    {
        double cs, alpha;
        prewarp(sr, hz, q, cs, alpha);
        set((1.0 - cs) * 0.5, 1.0 - cs, (1.0 - cs) * 0.5, 1.0 + alpha, -2.0 * cs, 1.0 - alpha);
    }

    void setHighPass(double sr, double hz, double q)
    {
        double cs, alpha;
        prewarp(sr, hz, q, cs, alpha);
        set((1.0 + cs) * 0.5, -(1.0 + cs), (1.0 + cs) * 0.5, 1.0 + alpha, -2.0 * cs, 1.0 - alpha);
    }

    void setPeaking(double sr, double hz, double q, double gainDb)
    {
        double cs, alpha;
        prewarp(sr, hz, q, cs, alpha);
        double A = std::pow(10.0, gainDb / 40.0);
        set(1.0 + alpha * A, -2.0 * cs, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * cs, 1.0 - alpha / A);
    }

    void setLowShelf(double sr, double hz, double q, double gainDb)
    {
        double cs, alpha;
        prewarp(sr, hz, q, cs, alpha);
        double A = std::pow(10.0, gainDb / 40.0);
        double beta = 2.0 * std::sqrt(A) * alpha;
        set(A * ((A + 1.0) - (A - 1.0) * cs + beta),
            2.0 * A * ((A - 1.0) - (A + 1.0) * cs),
            A * ((A + 1.0) - (A - 1.0) * cs - beta),
            (A + 1.0) + (A - 1.0) * cs + beta,
            -2.0 * ((A - 1.0) + (A + 1.0) * cs),
            (A + 1.0) + (A - 1.0) * cs - beta);
    }

    void setHighShelf(double sr, double hz, double q, double gainDb)
    {
        double cs, alpha;
        prewarp(sr, hz, q, cs, alpha);
        double A = std::pow(10.0, gainDb / 40.0);
        double beta = 2.0 * std::sqrt(A) * alpha;
        set(A * ((A + 1.0) + (A - 1.0) * cs + beta),
            -2.0 * A * ((A - 1.0) + (A + 1.0) * cs),
            A * ((A + 1.0) + (A - 1.0) * cs - beta),
            (A + 1.0) - (A - 1.0) * cs + beta,
            2.0 * ((A - 1.0) - (A + 1.0) * cs),
            (A + 1.0) - (A - 1.0) * cs - beta);
    }

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float s1 = 0.0f, s2 = 0.0f;

private:
    static void prewarp(double sr, double hz, double q, double& cs, double& alpha)
    {
        hz = std::clamp(hz, 5.0, sr * 0.45);
        double w0 = 6.283185307179586 * hz / sr;
        cs = std::cos(w0);
        alpha = std::sin(w0) / (2.0 * std::max(q, 0.05));
    }

    void set(double B0, double B1, double B2, double A0, double A1, double A2)
    {
        double inv = 1.0 / A0;
        b0 = (float) (B0 * inv);
        b1 = (float) (B1 * inv);
        b2 = (float) (B2 * inv);
        a1 = (float) (A1 * inv);
        a2 = (float) (A2 * inv);
    }
};

// One-pole RC low-pass / high-pass pair (the same structure Delay.h uses in its feedback path).
struct OnePole
{
    void setCutoff(double sr, float hz)
    {
        a = 1.0f - std::exp(-dsp::TWO_PI_F * std::min(hz, (float) sr * 0.45f) / (float) sr);
    }

    void reset() { z = 0.0f; }

    float lowPass(float x)
    {
        z = flushDenormal(z + a * (x - z));
        return z;
    }

    float highPass(float x) { return x - lowPass(x); }

    float z = 0.0f;
    float a = 1.0f;
};

// Peak envelope follower with separate attack and release times (Zoelzer, "DAFX: Digital Audio
// Effects", dynamics chapter: peak level detector).
struct EnvelopeFollower
{
    void setTimes(double sr, float attackMs, float releaseMs)
    {
        att = 1.0f - std::exp(-1.0f / std::max(1.0f, attackMs * 0.001f * (float) sr));
        rel = 1.0f - std::exp(-1.0f / std::max(1.0f, releaseMs * 0.001f * (float) sr));
    }

    void reset() { env = 0.0f; }

    float process(float x)
    {
        float a = std::abs(x);
        env = flushDenormal(env + (a > env ? att : rel) * (a - env));
        return env;
    }

    float env = 0.0f;
    float att = 1.0f, rel = 1.0f;
};

// Circular delay line with fractional read. Memory is allocated in prepare() only.
// Call read...() before write() for the current sample: a delay of d then returns the sample
// written d calls ago. Linear read needs d >= 1, Hermite read needs d >= 2.
// Interpolation: 4-point, 3rd-order Hermite (Catmull-Rom) as in Olli Niemitalo, "Polynomial
// Interpolators for High-Quality Resampling of Oversampled Audio".
class DelayLine
{
public:
    void prepare(int maxDelaySamples)
    {
        int n = 16;
        while (n < maxDelaySamples + 8)
            n <<= 1;
        buf.assign((size_t) n, 0.0f);
        mask = n - 1;
        maxDelay = (float) (n - 6);
        w = 0;
    }

    void clear()
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        w = 0;
    }

    bool isPrepared() const { return !buf.empty(); }
    float getMaxDelay() const { return maxDelay; }

    void write(float x)
    {
        buf[(size_t) w] = x;
        w = (w + 1) & mask;
    }

    float readLinear(float d) const
    {
        d = std::clamp(d, 1.0f, maxDelay);
        int di = (int) d;
        float fr = d - (float) di;
        float x0 = at(di), x1 = at(di + 1);
        return x0 + fr * (x1 - x0);
    }

    float readHermite(float d) const
    {
        d = std::clamp(d, 2.0f, maxDelay);
        int di = (int) d;
        float fr = d - (float) di;
        float xm1 = at(di - 1), x0 = at(di), x1 = at(di + 1), x2 = at(di + 2);
        float c1 = 0.5f * (x1 - xm1);
        float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * fr + c2) * fr + c1) * fr + x0;
    }

private:
    float at(int delay) const { return buf[(size_t) ((w - delay) & mask)]; }

    std::vector<float> buf;
    int mask = 0;
    int w = 0;
    float maxDelay = 1.0f;
};

} // namespace fxb
