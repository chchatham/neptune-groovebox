#pragma once

#include "EffectBase.h"
#include "FxBlocks.h"

// Ring modulator: the input is multiplied by a sine carrier, which replaces every input partial f
// by the pair (f - carrier) and (f + carrier). Textbook structure (Zoelzer, "DAFX", modulators
// chapter). A sine LFO can sweep the carrier frequency by up to +/- one octave.
//
// Parameters: FREQ carrier frequency, RATE LFO rate, DPTH LFO depth (1 = +/- one octave), MIX.
class RingMod : public EffectBase
{
public:
    enum Param { Freq = 0, Rate, Depth, Mix };

    RingMod() { mix = 1.0f; }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "FREQ", 0, 20.0f, 5000.0f, 220.0f, K::Log, "Hz" };
            case 1: return { "RATE", 1, 0.05f, 20.0f, 1.0f, K::Log, "Hz" };
            case 2: return { "DPTH", 2, 0, 1, 0.0f };
            case 3: return { "MIX", 3, 0, 1, 1.0f };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Freq:  freq = std::clamp(value, 20.0f, 5000.0f); break;
            case Rate:  rate = std::clamp(value, 0.05f, 20.0f); break;
            case Depth: depth = std::clamp(value, 0.0f, 1.0f); break;
            case Mix:   setMix(value); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Freq:  return freq;
            case Rate:  return rate;
            case Depth: return depth;
            case Mix:   return mix;
            default:    return 0.0f;
        }
    }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        reset();
    }

    void reset() override
    {
        carrierPhase = 0.0;
        lfoPhase = 0.0;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float f = freq;
        if (depth > 0.0f)
        {
            float lfo = (float) std::sin(lfoPhase * dsp::TWO_PI_D);
            f *= std::exp2(depth * lfo);
            lfoPhase += (double) rate / sampleRate;
            if (lfoPhase >= 1.0) lfoPhase -= 1.0;
        }
        f = std::min(f, (float) sampleRate * 0.45f);

        float carrier = (float) std::sin(carrierPhase * dsp::TWO_PI_D);
        carrierPhase += (double) f / sampleRate;
        if (carrierPhase >= 1.0) carrierPhase -= 1.0;

        left = lerp(left, left * carrier, mix);
        right = lerp(right, right * carrier, mix);
    }

private:
    float freq = 220.0f;
    float rate = 1.0f;
    float depth = 0.0f;
    double carrierPhase = 0.0;
    double lfoPhase = 0.0;
};
