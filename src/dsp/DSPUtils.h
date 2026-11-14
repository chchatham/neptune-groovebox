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

// A phase kept in [0, 1) whatever it is given: an infinite or not-a-number phase (a pitch gone
// wrong) is 0, never an index outside a table or a loop that does not end.
// `now` moved towards `target` by at most `step`: what a value that is set under a sounding note
// is heard through, so that it is never jumped to (a jump is a click).
inline float towards(float now, float target, float step)
{
    const float d = target - now;
    return std::abs(d) <= step ? target : now + (d > 0.0f ? step : -step);
}

inline double wrapPhase(double p)
{
    p -= std::floor(p);
    return (p >= 0.0 && p < 1.0) ? p : 0.0;
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
    void arm() { pending = true; tail = false; }
    // For a sound whose new note may start on a value of its own (a drum's knock): the old sound
    // is carried down UNDER the new note, whose first sample is left exactly as it is.
    void armTail() { pending = true; tail = true; }
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
            offset = last + (smooth ? slope : 0.0f) - (tail ? 0.0f : x);
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
    bool pending = false, tail = false;
};

// The same for a signal that is handled a block at a time (a track, the master output), at no cost
// while nothing happens: it remembers how each block ended and, when armed, carries the jump
// between that and the next block's first sample off over about 2 ms. For what cuts or swaps a
// sound between two blocks: an engine replaced, an effect taken out, every note stopped at once.
struct BlockDeclick
{
    void setSampleRate(double sr) { length = sr > 0.0 ? (int) (sr * 0.002) : 96; if (length < 1) length = 1; }
    // arm: the sound goes on, changed - the next block is joined to this one.
    // armTail: the sound was stopped - it is carried down under whatever starts in the next block
    // (a new note's first sample is its own, and is not joined to anything).
    void arm() { pending = true; tail = false; }
    void armTail() { pending = true; tail = true; }
    void reset() { last = 0.0f; before = 0.0f; lastSlope = 0.0f; offset = 0.0f; remaining = 0; pending = false; }

    void process(float* x, int numSamples)
    {
        if (numSamples <= 0)
            return;
        if (pending)
        {
            pending = false;
            const float slope = last - before;
            const bool smooth = std::abs(slope - lastSlope) <= 0.5f * std::abs(slope);
            offset = held = last + (smooth ? slope : 0.0f);
            remaining = length;
        }
        for (int i = 0; i < numSamples && remaining > 0; ++i, --remaining)
        {
            // tail: the old sound's last value dies away under the new. Otherwise a crossfade from
            // it to the new sound (which may need a moment to settle: a filter that starts empty).
            const float t = (float) remaining / (float) length;
            const float w = t * t * (3.0f - 2.0f * t);
            x[i] = tail ? x[i] + offset * w : x[i] + (held - x[i]) * w;
        }
        for (int i = numSamples > 3 ? numSamples - 3 : 0; i < numSamples; ++i)
        {
            lastSlope = last - before;
            before = last;
            last = x[i];
        }
    }

    float last = 0.0f, before = 0.0f, lastSlope = 0.0f, offset = 0.0f, held = 0.0f;
    int length = 96, remaining = 0;
    bool pending = false, tail = false;
};

// A gain (a level, a pan) moved by an LFO may not jump: a ramp, square or random shape steps it,
// and a step in gain is a click on whatever is sounding. The value follows its target at no more
// than a range of 2 (a level's 0..2, a pan's -1..1) in 4 ms. No continuous shape at LFO rates
// moves that fast, so those pass through exactly; a jump becomes a ramp of at most 4 ms.
struct GainSlew
{
    void setSampleRate(double sr) { maxStep = sr > 0.0 ? (float) (2.0 / (0.004 * sr)) : 0.01f; }
    void snap() { fresh = true; }       // the next value is taken as it is (a voice starting from silence)

    float process(float target)
    {
        const float d = target - value;
        if (fresh || std::abs(d) <= maxStep)
            value = target;
        else
            value += d > 0.0f ? maxStep : -maxStep;
        fresh = false;
        return value;
    }

    float value = 0.0f, maxStep = 0.01f;
    bool fresh = true;
};

} // namespace dsp
