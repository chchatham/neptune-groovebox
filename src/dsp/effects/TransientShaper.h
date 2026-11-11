#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <cmath>
#include <algorithm>

// Transient shaper using an envelope follower.
// Detects the signal envelope with fast attack and adjustable release,
// then uses the difference between fast and slow envelopes to identify
// transients (attack phase) vs. sustain. The attack and sustain gains
// are applied independently. No buffers needed - pure computation.
// No heap on audio thread.
class TransientShaper : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "ATK", 0, -1, 1, 0 };
            case 1: return { "SUS", 1, -1, 1, 0 };
            case 2: return { "SPD", 2, 0, 1, 0.5f };
            case 3: return { "OUT", 3, -12, 12, 0, K::Linear, "dB" };
            default: return {};
        }
    }

    enum Param
    {
        Attack = 0,     // -1 to 1 (negative = soften, positive = sharpen attacks)
        Sustain,        // -1 to 1 (negative = reduce sustain, positive = boost)
        Speed,          // 0 - 1 (envelope follower speed / sensitivity)
        Output,         // -12 to 12 dB (output gain compensation)
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
        envFast = 0.0f;
        envSlow = 0.0f;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        // Detect input level (peak of stereo pair)
        float inputLevel = std::max(std::abs(left), std::abs(right));

        // Fast envelope follower (tracks transients)
        if (inputLevel > envFast)
            envFast += fastAttackCoeff * (inputLevel - envFast);
        else
            envFast += fastReleaseCoeff * (inputLevel - envFast);

        // Slow envelope follower (tracks sustain/body)
        if (inputLevel > envSlow)
            envSlow += slowAttackCoeff * (inputLevel - envSlow);
        else
            envSlow += slowReleaseCoeff * (inputLevel - envSlow);

        // Transient signal: difference between fast and slow envelopes
        // Positive = attack phase, near zero = sustain phase
        float transientAmount = envFast - envSlow;
        transientAmount = std::max(0.0f, transientAmount);

        // Sustain signal: slow envelope level
        float sustainAmount = envSlow;

        // Compute gain modification
        float gain = 1.0f;

        // Attack shaping: boost or cut during transient phase
        // attack > 0: boost transients, attack < 0: soften transients
        if (transientAmount > 0.001f)
        {
            float attackBoost = 1.0f + attack * 3.0f * transientAmount;
            attackBoost = std::max(attackBoost, 0.01f);
            gain *= attackBoost;
        }

        // Sustain shaping: boost or cut during sustain phase
        // sustain > 0: boost sustain, sustain < 0: reduce sustain
        if (sustainAmount > 0.001f && transientAmount < sustainAmount * 0.5f)
        {
            float sustainBoost = 1.0f + sustain * 2.0f;
            sustainBoost = std::max(sustainBoost, 0.01f);
            gain *= sustainBoost;
        }

        // Output gain compensation
        gain *= outputGainLinear;

        // Apply gain
        float wetL = left * gain;
        float wetR = right * gain;

        // Soft clip to prevent harsh overloads
        wetL = dsp::fastTanh(wetL);
        wetR = dsp::fastTanh(wetR);

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Attack:
                attack = std::clamp(value, -1.0f, 1.0f);
                break;
            case Sustain:
                sustain = std::clamp(value, -1.0f, 1.0f);
                break;
            case Speed:
                speed = std::clamp(value, 0.0f, 1.0f);
                recalcCoeffs();
                break;
            case Output:
                outputDb = std::clamp(value, -12.0f, 12.0f);
                outputGainLinear = dsp::dbToLinear(outputDb);
                break;
            case Mix:
                mix = std::clamp(value, 0.0f, 1.0f);
                break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Attack:  return attack;
            case Sustain: return sustain;
            case Speed:   return speed;
            case Output:  return outputDb;
            case Mix:     return mix;
            default:      return 0.0f;
        }
    }

private:
    float attack   = 0.0f;
    float sustain  = 0.0f;
    float speed    = 0.5f;
    float outputDb = 0.0f;
    float outputGainLinear = 1.0f;

    // Envelope follower state
    float envFast = 0.0f;
    float envSlow = 0.0f;

    // Envelope follower coefficients
    float fastAttackCoeff  = 0.0f;
    float fastReleaseCoeff = 0.0f;
    float slowAttackCoeff  = 0.0f;
    float slowReleaseCoeff = 0.0f;

    void recalcCoeffs()
    {
        if (sampleRate <= 0.0) return;

        // Fast envelope: 0.1-2ms attack, 5-50ms release (speed adjusts)
        float fastAttackMs  = lerp(2.0f, 0.1f, speed);
        float fastReleaseMs = lerp(50.0f, 5.0f, speed);

        // Slow envelope: 10-50ms attack, 100-500ms release
        float slowAttackMs  = lerp(50.0f, 10.0f, speed);
        float slowReleaseMs = lerp(500.0f, 100.0f, speed);

        fastAttackCoeff  = 1.0f - std::exp(-1.0f / (float)(fastAttackMs * 0.001 * sampleRate));
        fastReleaseCoeff = 1.0f - std::exp(-1.0f / (float)(fastReleaseMs * 0.001 * sampleRate));
        slowAttackCoeff  = 1.0f - std::exp(-1.0f / (float)(slowAttackMs * 0.001 * sampleRate));
        slowReleaseCoeff = 1.0f - std::exp(-1.0f / (float)(slowReleaseMs * 0.001 * sampleRate));
    }
};
