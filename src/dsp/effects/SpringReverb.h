#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>
#include <cmath>
#include <algorithm>

// Spring reverb simulation using a dispersive allpass chain.
// Real springs exhibit frequency-dependent delay (dispersion): higher
// frequencies travel faster. Modeled with a cascade of allpass filters
// whose coefficients sweep with frequency, plus a delay line for the
// overall travel time and feedback for the characteristic "boing."
// All buffers pre-allocated. No heap on audio thread.
class SpringReverb : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "DCAY", 0, 0.1f, 5, 1.5f, K::Log, "s" };
            case 1: return { "DISP", 1, 0, 1, 0.6f };
            case 2: return { "TENS", 2, 0, 1, 0.5f };
            case 3: return { "DAMP", 3, 0, 1, 0.4f };
            default: return {};
        }
    }

    enum Param
    {
        Decay = 0,      // 0.1 - 5 seconds
        Dispersion,     // 0 - 1 (allpass chirp intensity)
        Tension,        // 0 - 1 (affects allpass coefficient range)
        Damping,        // 0 - 1 (high-frequency damping)
        Mix             // 0 - 1
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        recalcCoeffs();
        reset();
    }

    void reset() override
    {
        for (auto& s : apStateL) s = 0.0f;
        for (auto& s : apStateR) s = 0.0f;
        delayLineL.fill(0.0f);
        delayLineR.fill(0.0f);
        writePos = 0;
        dampL = 0.0f;
        dampR = 0.0f;
        feedbackL = 0.0f;
        feedbackR = 0.0f;
        chirpPhase = 0.0;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        // Inject input with feedback
        float inL = left + feedbackL * feedbackGain;
        float inR = right + feedbackR * feedbackGain;

        // Dispersive allpass chain: sweep coefficient creates chirp effect
        // Advance chirp modulation slowly for the "drip" character
        chirpPhase += 0.7 / sampleRate;
        if (chirpPhase >= 1.0) chirpPhase -= 1.0;

        float baseCoeff = 0.3f + tension * 0.5f;
        float dispMod = dispersion * 0.4f;

        for (int i = 0; i < NUM_AP_STAGES; ++i)
        {
            // Each stage gets a slightly different coefficient to create dispersion
            float stageOffset = (float)i / (float)NUM_AP_STAGES;
            float coeff = baseCoeff + dispMod * std::sin((stageOffset + (float)chirpPhase) * dsp::TWO_PI_F);
            coeff = std::clamp(coeff, -0.9f, 0.9f);

            // First-order allpass: y[n] = coeff * (x[n] - y[n-1]) + x[n-1]
            float outL = coeff * (inL - apStateL[i]) + apStateL[i];
            apStateL[i] = inL;
            inL = outL;

            float outR = coeff * (inR - apStateR[i]) + apStateR[i];
            apStateR[i] = inR;
            inR = outR;
        }

        // Travel-time delay line (simulates spring length)
        int delaySamples = std::clamp((int)(0.030 * sampleRate), 1, MAX_DELAY - 1);
        delayLineL[(size_t)writePos] = inL;
        delayLineR[(size_t)writePos] = inR;
        int readPos = writePos - delaySamples;
        if (readPos < 0) readPos += MAX_DELAY;
        float wetL = delayLineL[(size_t)readPos];
        float wetR = delayLineR[(size_t)readPos];
        writePos = (writePos + 1) % MAX_DELAY;

        // Damping filter (one-pole lowpass in feedback path)
        dampL += damping * (wetL - dampL);
        dampR += damping * (wetR - dampR);
        wetL = dampL * damping + wetL * (1.0f - damping);
        wetR = dampR * damping + wetR * (1.0f - damping);

        // Store feedback
        feedbackL = std::clamp(wetL, -4.0f, 4.0f);
        feedbackR = std::clamp(wetR, -4.0f, 4.0f);

        // Slight stereo spread: add some inverted signal to opposite channel
        float outL = wetL * 0.8f + wetR * 0.2f;
        float outR = wetR * 0.8f + wetL * 0.2f;

        left  = dryL * (1.0f - mix) + outL * mix;
        right = dryR * (1.0f - mix) + outR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Decay:
                decay = std::clamp(value, 0.1f, 5.0f);
                recalcCoeffs();
                break;
            case Dispersion: dispersion = std::clamp(value, 0.0f, 1.0f); break;
            case Tension:    tension    = std::clamp(value, 0.0f, 1.0f); break;
            case Damping:    damping    = std::clamp(value, 0.0f, 1.0f); break;
            case Mix:        mix        = std::clamp(value, 0.0f, 1.0f); break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Decay:      return decay;
            case Dispersion: return dispersion;
            case Tension:    return tension;
            case Damping:    return damping;
            case Mix:        return mix;
            default:         return 0.0f;
        }
    }

private:
    static constexpr int NUM_AP_STAGES = 12;
    static constexpr int MAX_DELAY = 8192;

    float decay      = 2.0f;
    float dispersion = 0.6f;
    float tension    = 0.5f;
    float damping    = 0.5f;
    float feedbackGain = 0.0f;

    std::array<float, NUM_AP_STAGES> apStateL {};
    std::array<float, NUM_AP_STAGES> apStateR {};
    std::array<float, MAX_DELAY> delayLineL {};
    std::array<float, MAX_DELAY> delayLineR {};
    int writePos = 0;
    float dampL = 0.0f;
    float dampR = 0.0f;
    float feedbackL = 0.0f;
    float feedbackR = 0.0f;
    double chirpPhase = 0.0;

    void recalcCoeffs()
    {
        if (sampleRate > 0.0)
        {
            // Feedback gain based on decay time and delay length (~30ms loop)
            float loopTimeSec = 0.030f;
            feedbackGain = std::exp(-3.0f * loopTimeSec / decay);
            feedbackGain = std::clamp(feedbackGain, 0.0f, 0.95f);
        }
    }
};
