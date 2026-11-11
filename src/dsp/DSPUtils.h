#pragma once

#include <cmath>

namespace dsp {

constexpr float PI_F = 3.14159265f;
constexpr float TWO_PI_F = 6.283185307f;
constexpr double TWO_PI_D = 6.283185307;

inline float fastTanh(float x)
{
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float linearToDb(float linear)
{
    if (linear <= 0.000001f) return -120.0f;
    return 20.0f * std::log10(linear);
}

inline float dbToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline double wrapPhase(double p)
{
    return p - std::floor(p);
}

inline double midiNoteToHz(int note)
{
    return 440.0 * std::pow(2.0, (note - 69) / 12.0);
}

inline float polyBLEP(double t, double dt)
{
    if (t < dt)
    {
        double n = t / dt;
        return (float)(n + n - n * n - 1.0);
    }
    else if (t > 1.0 - dt)
    {
        double n = (t - 1.0) / dt;
        return (float)(n * n + n + n + 1.0);
    }
    return 0.0f;
}

inline float fastTan(float x)
{
    float x2 = x * x;
    return x * (1.0f + x2 * (1.0f / 3.0f + x2 * (2.0f / 15.0f)));
}

inline int encoderDelta(int value)
{
    return value > 64 ? value - 128 : value;
}

inline uint32_t xorshift32(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

inline float xorshiftNoise(uint32_t& state)
{
    return (float)(xorshift32(state)) / (float)UINT32_MAX * 2.0f - 1.0f;
}

inline float xorshiftFloat(uint32_t& state)
{
    return static_cast<float>(xorshift32(state) & 0x7FFFFFFF) / 2147483648.0f;
}

// A voice that is struck again while it still sounds (the same key held, a stolen voice) restarts
// its oscillators, so its output would jump. Armed at that note-on, this carries the jump as an
// offset that glides to zero over about 2 ms: the output stays continuous and the new note's own
// attack is not touched. A voice starting from silence does not need it.
struct Declick
{
    void setSampleRate(double sr) { length = sr > 0.0 ? (int) (sr * 0.002) : 96; if (length < 1) length = 1; }
    void arm() { pending = true; }
    void reset() { last = 0.0f; before = 0.0f; offset = 0.0f; remaining = 0; pending = false; }

    float process(float x)
    {
        if (pending)
        {
            pending = false;
            // Where the sound was heading: its last value, carried on along its slope where it was
            // moving smoothly (so that arming on a sound that does not jump changes nothing).
            const float slope = last - before;
            const bool smooth = std::abs(slope - lastSlope) <= 0.5f * std::abs(slope);
            offset = last + (smooth ? slope : 0.0f) - x;
            remaining = length;
        }
        if (remaining > 0)
        {
            const float t = (float) remaining / (float) length;
            x += offset * t * t * (3.0f - 2.0f * t);
            --remaining;
        }
        lastSlope = last - before;
        before = last;
        last = x;
        return x;
    }

    float last = 0.0f, before = 0.0f, lastSlope = 0.0f, offset = 0.0f;
    int length = 96, remaining = 0;
    bool pending = false;
};

} // namespace dsp
