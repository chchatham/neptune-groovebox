#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <cmath>
#include <algorithm>

// LFO-driven stereo auto-panner.
// Modulates the left/right balance using a selectable LFO shape.
// Supports sine, triangle, and square shapes with adjustable depth,
// rate, and stereo phase offset. No buffers needed - pure computation.
// No heap on audio thread.
class AutoPan : public EffectBase
{
public:
    static const char* const* panShapes()
    {
        static const char* const n[] = { "SIN", "TRI", "SQR" };
        return n;
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "RATE", 0, 0.1f, 20.0f, 2.0f, K::Log, "Hz" };
            case 1: return { "DPTH", 1, 0, 1, 0.7f };
            case 2: return { "SHPE", 2, 0, 2, 0, K::Choice, "", panShapes(), 3 };
            case 3: return { "PHAS", 3, 0, 1, 0 };
            default: return {};
        }
    }

    enum Param
    {
        Rate = 0,       // 0.1 - 20 Hz
        Depth,          // 0 - 1
        Shape,          // 0 = sine, 1 = triangle, 2 = square
        Phase,          // 0 - 1 (LFO start phase offset)
        Mix             // 0 - 1
    };

    enum ShapeType
    {
        Sine = 0,
        Triangle = 1,
        Square = 2
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        reset();
    }

    void reset() override
    {
        lfoPhase = (double)phase;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        // Advance LFO
        lfoPhase += rate / sampleRate;
        while (lfoPhase >= 1.0) lfoPhase -= 1.0;

        float lfoVal = computeShape((float)lfoPhase);

        // Map LFO to pan position: lfoVal in [-1, 1]
        // Depth controls how far from center the pan moves
        // At depth=0: no panning. At depth=1: full L/R swing
        float panL = 1.0f - depth * 0.5f * (1.0f + lfoVal);  // Goes from 1 to (1-depth)
        float panR = 1.0f - depth * 0.5f * (1.0f - lfoVal);  // Opposite phase

        // Apply equal-power panning curves for smoother transitions
        // Approximate: sqrt(pan) for constant power (using linear as simpler alternative)
        float wetL = left * panL;
        float wetR = right * panR;

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Rate:  rate  = std::clamp(value, 0.1f, 20.0f);                  break;
            case Depth: depth = std::clamp(value, 0.0f, 1.0f);                   break;
            case Shape: shape = (ShapeType)std::clamp((int)value, 0, 2);          break;
            case Phase: phase = std::clamp(value, 0.0f, 1.0f);                   break;
            case Mix:   mix   = std::clamp(value, 0.0f, 1.0f);                   break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Rate:  return rate;
            case Depth: return depth;
            case Shape: return (float)shape;
            case Phase: return phase;
            case Mix:   return mix;
            default:    return 0.0f;
        }
    }

private:
    float computeShape(float p) const
    {
        switch (shape)
        {
            case Sine:
                return std::sin(p * dsp::TWO_PI_F);

            case Triangle:
                return p < 0.5f ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);

            case Square:
                return p < 0.5f ? 1.0f : -1.0f;

            default:
                return std::sin(p * dsp::TWO_PI_F);
        }
    }

    double lfoPhase = 0.0;
    float rate  = 2.0f;
    float depth = 0.5f;
    ShapeType shape = Sine;
    float phase = 0.0f;
};
