#pragma once

#include "Detector.h"
#include <cmath>
#include <algorithm>
#include <array>

// Brickwall lookahead limiter, LMT (and the master section's).
// Uses a pre-allocated circular delay buffer for lookahead (1-5ms).
// Output is guaranteed to never exceed the ceiling.
// No heap allocation in process. All buffers are fixed-size member arrays.
//
// It is the shared dynamics core (Detector.h) at its hardest setting: the gain each sample asks for
// (ceiling / peak) goes through dyn::SlidingMin over the lookahead (the smallest gain of the
// window, at a constant cost: it was a scan of the whole window for every sample) and then through
// dyn::Follower::down with no attack time. The arithmetic is what it was, bit for bit: the master
// limiter is part of the "P01 Keyboard" pin.
//
// Its cells are CellEffect's: CEIL and REL move with modulateCell. LOOK does not (it is the length
// of a delay). It has no KEY cell: its promise is about its own signal; a limiter keyed from
// another track is DUCK at 20:1. The slot's WET is the chain's (EffectChain.h): below the top the
// dry sound, delayed by LOOK so that the two are in step, stands beside the limited one (parallel
// limiting: the peaks are no longer held to the ceiling).
class Limiter : public CellEffect
{
public:
    enum Param { Ceiling = 0, Release, Lookahead };

    Limiter()
    {
        delayBufferL.fill(0.0f);
        delayBufferR.fill(0.0f);
        window.reset(1.0f);
        gain.reset(1.0f);
        seed();
    }

    // By name (the master section, tests): ceiling in dB (-3 to 0), lookahead in ms (1-5),
    // release in ms (50-500).
    void setCeiling(float db)   { setCell(Ceiling, db); }
    void setLookahead(float ms) { setCell(Lookahead, ms); }
    void setRelease(float ms)   { setCell(Release, ms); }
    float getCeiling() const    { return cell(Ceiling); }
    float getLookahead() const  { return cell(Lookahead); }
    float getRelease() const    { return cell(Release); }

    // The limited sound is LOOK behind the sound that came in (EffectChain mixes the dry sound as late).
    int latencySamples() const override { return lookaheadSamples; }

    // Current gain reduction in dB (for metering)
    float getGainReductionDb() const
    {
        if (gain.y <= 0.000001f) return -120.0f;
        return 20.0f * std::log10(gain.y);
    }

    // One sample (the master section runs this way).
    void process(float& left, float& right) override
    {
        if (! enabled) return;
        clock(left, right);
        sample(left, right);
    }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "CEIL", 0, -3, 0, -0.3f, K::Linear, "dB" },
            { "REL", 1, 50, 500, 200, K::Log, "ms" },
            { "LOOK", 2, 1, 5, 3, K::Linear, "ms" },
            {}, {}, {}, {}, {} };
        return c;
    }

    void prepareCore() override {}

    void resetCore() override
    {
        delayBufferL.fill(0.0f);
        delayBufferR.fill(0.0f);
        window.reset(1.0f);
        writePos = 0;
        gain.reset(1.0f);
    }

    void configure(const float* v) override
    {
        ceilingLinear = std::pow(10.0f, v[Ceiling] / 20.0f);
        // (LOOK as it is set: modulation does not move the length of the delay.)
        lookaheadSamples = std::clamp((int) (cell(Lookahead) * 0.001f * (float) sampleRate), 1, MAX_DELAY - 1);
        gain.attack = 0.0f;
        gain.release = dyn::timeCoeff(v[Release], sampleRate);
    }

    bool coasting() const override { return true; }     // (nothing here waits for controlCore(): one sample at a time)
    void runCore(float* left, float* right, int n, float, float) override
    {
        for (int i = 0; i < n; ++i)
            sample(left[i], right[i]);
    }

private:
    void sample(float& left, float& right)
    {
        // Write input to delay buffer
        delayBufferL[writePos] = left;
        delayBufferR[writePos] = right;

        // Detect peak of current input
        float peak = std::max(std::abs(left), std::abs(right));

        // Compute needed gain for this sample
        float neededGain = 1.0f;
        if (peak > ceilingLinear)
            neededGain = ceilingLinear / peak;

        // The smallest gain asked for within the lookahead window (this sample included), so the
        // gain is already down when the peak leaves the delay.
        const float minGain = window.push(neededGain, lookaheadSamples);

        // Down at once (the lookahead is the attack), back up slowly; never above 1.
        const float currentGain = gain.y = std::min(gain.down(minGain), 1.0f);

        // Read from delayed position
        int readPos = (writePos - lookaheadSamples + MAX_DELAY) % MAX_DELAY;
        left = delayBufferL[readPos] * currentGain;
        right = delayBufferR[readPos] * currentGain;

        // Final hard clip safety net (should never activate if logic is correct)
        left = std::clamp(left, -ceilingLinear, ceilingLinear);
        right = std::clamp(right, -ceilingLinear, ceilingLinear);

        writePos = (writePos + 1) % MAX_DELAY;
    }

    // Max 5ms at 192kHz = 960 samples. Round up to power of 2 for safety.
    static constexpr int MAX_DELAY = 1024;

    // Parameters
    float ceilingLinear = 0.966f;  // ~-0.3 dB

    // Derived
    int lookaheadSamples = 132;    // 3ms at 44100

    // Pre-allocated buffers (no heap in process)
    std::array<float, MAX_DELAY> delayBufferL {};
    std::array<float, MAX_DELAY> delayBufferR {};
    dyn::SlidingMin<MAX_DELAY> window;      // the gains asked for; the smallest of the last lookaheadSamples

    // State
    int writePos = 0;
    dyn::Follower gain;                     // the gain applied: no attack time, release = REL
};
