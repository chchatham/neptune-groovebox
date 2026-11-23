#pragma once

// Shared building blocks for the effects added in iteration 51 (ring modulator, vibrato, wah, amp,
// graphic EQ, tape delay, L/C/R delay). Nothing here is new DSP: each block is a standard
// structure, named with its reference.

#include "../DSPUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fxb
{

// Values this small are treated as zero so recursive states never sit in the denormal range.
inline float flushDenormal(float x) { return std::abs(x) < 1.0e-15f ? 0.0f : x; }

// Bounded, monotonic saturator: the project's tanh approximation (dsp::fastTanh) is only tanh-like
// for |x| <= 3 (beyond that it grows as x / 9), so the input is clamped there. At 3 it reaches
// exactly 1 with zero slope, which makes the clamp seamless. Output is always within -1..1.
constexpr float softSat(float x) { return dsp::fastTanh(std::clamp(x, -3.0f, 3.0f)); }

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
// (Its last user is the track-input follower in AudioEngine.h; every effect's follower is
// dyn::Follower, Detector.h, which is this recursion with a hold. When that one has moved, this goes.)
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

// (The circular line with a fractional read is dly::Line, DelayCore.h; a tape's wow and flutter
// dly::Transport there.)

// Fixed-size circular line read at a whole number of samples (the reverbs' tank and FDN-style lines).
template <int N>
struct FixedDelay
{
    std::array<float, N> buf {};
    int writeIdx = 0;

    void clear()
    {
        buf.fill(0.0f);
        writeIdx = 0;
    }

    void write(float sample)
    {
        buf[(size_t)writeIdx] = sample;
        writeIdx = (writeIdx + 1) % N;
    }

    float read(int delaySamples) const
    {
        delaySamples = std::clamp(delaySamples, 1, N - 1);
        int readIdx = writeIdx - delaySamples;
        if (readIdx < 0) readIdx += N;
        return buf[(size_t)readIdx];
    }
};

// Schroeder allpass on a fixed-size line: v = x + g v[n-D], y = v[n-D] - g v. (The output once used
// x in place of v, which is a comb with gain above 1: the plate's tank fed on itself and never died.)
template <int N>
struct FixedAllpass
{
    std::array<float, N> buf {};
    int writeIdx = 0;

    void clear()
    {
        buf.fill(0.0f);
        writeIdx = 0;
    }

    float process(float in, int delaySamples, float coeff)
    {
        delaySamples = std::clamp(delaySamples, 1, N - 1);
        int readIdx = writeIdx - delaySamples;
        if (readIdx < 0) readIdx += N;
        float delayed = buf[(size_t)readIdx];
        const float v = in + delayed * coeff;
        float out = delayed - v * coeff;
        buf[(size_t)writeIdx] = v;
        writeIdx = (writeIdx + 1) % N;
        return out;
    }

    float readAt(int delaySamples) const
    {
        delaySamples = std::clamp(delaySamples, 1, N - 1);
        int readIdx = writeIdx - delaySamples;
        if (readIdx < 0) readIdx += N;
        return buf[(size_t)readIdx];
    }
};

} // namespace fxb
