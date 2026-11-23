#pragma once

#include <algorithm>
#include <cmath>

// The level-following crusher (LOFI of the cymbal machine and of the oscillator bank, the
// resonator drum's lofi / driveLofi): the sound is read out at a low rate (held between readings)
// by a converter with few steps. The steps follow the level of the sound, as the level control
// after an early sampler's converter did, down to a floor: the tail turns to grit and then stops.
//
// One body for the three that had it word for word (DrumVoice, OscBank, CymbalMachine): the
// arithmetic is theirs to the operation, so their renders are the samples they were. The two
// powers of two are worked out when the amount changes, not per sample.
// It is NOT RateReducer (fixed steps) nor dyn::Follower (one-pole attack and release).
namespace dsp
{
struct LevelCrusher
{
    // sr: the rate as the host holds it (the follower lets go over 30 ms).
    void prepare(double sr)
    {
        sampleRate = sr;
        followerDecay = std::exp(-1.0f / (0.03f * (float) sr));
    }

    void reset()
    {
        held = 0.0f;
        follower = 0.0f;
        peak = 0.0f;
        holdPhase = 1.0;
    }

    // amount 0..1: from 32 kHz and 256 steps down to about 5.3 kHz and 16.
    float process(float in, float amount)
    {
        if (amount != amountOf)
        {
            amountOf = amount;
            rate = 32000.0f * std::exp2(-2.6f * amount);
            levels = std::exp2(8.0f - 4.0f * amount);
        }
        const float mag = std::abs(in);
        follower = mag > follower ? mag : follower * followerDecay;
        peak = std::max(peak, mag);
        holdPhase += (double) rate / sampleRate;
        if (holdPhase >= 1.0)
        {
            holdPhase -= std::floor(holdPhase);
            const float stepSize = std::max(std::max(follower, 0.02f * peak), 1.0e-6f) * 2.0f / levels;
            held = std::round(in / stepSize) * stepSize;
        }
        return held;
    }

private:
    double sampleRate = 44100.0;
    float followerDecay = 0.0f;
    float held = 0.0f, follower = 0.0f, peak = 0.0f;
    double holdPhase = 1.0;
    float amountOf = -1.0f, rate = 32000.0f, levels = 256.0f;     // the amount the two powers are of
};
} // namespace dsp
