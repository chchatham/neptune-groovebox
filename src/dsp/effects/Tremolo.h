#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"

// Amplitude modulation (tremolo) effect.
// LFO modulates amplitude with selectable shape: sine, triangle, square.
// Stereo mode offsets L/R LFO phase for spatial width.
// No allocations - pure computation.
class Tremolo : public EffectBase
{
public:
    static const char* const* shapeNames()
    {
        static const char* const names[] = { "SIN", "TRI", "SQR" };
        return names;
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "RATE", 0, 0.1f, 20.0f, 4.0f, K::Log, "Hz" };
            case 1: return { "DPTH", 1, 0, 1, 0.5f };
            case 2: return { "SHPE", 2, 0, 2, 0, K::Choice, "", shapeNames(), 3 };
            case 3: return { "STER", 3, 0, 1, 0 };
            default: return {};
        }
    }

    enum Param
    {
        Rate = 0,       // 0.1 - 20 Hz
        Depth,          // 0 - 1
        Shape,          // 0=sine, 1=triangle, 2=square
        StereoOffset,   // 0 - 1 (phase offset between L/R, 0.5 = 180 degrees)
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
        lfoPhase = 0.0;
    }

    void process(float& left, float& right) override
    {
        if (!enabled)
            return;

        // Advance LFO
        lfoPhase += rate / sampleRate;
        while (lfoPhase >= 1.0)
            lfoPhase -= 1.0;

        float phaseL = (float)lfoPhase;
        float phaseR = phaseL + stereoOffset;
        if (phaseR >= 1.0f)
            phaseR -= 1.0f;

        float modL = computeShape(phaseL);
        float modR = computeShape(phaseR);

        // Map modulation: at depth=1, signal goes from 0 to 1. At depth=0, no effect.
        // mod is in [-1, 1], map to gain: gain = 1 - depth * (1 - (mod * 0.5 + 0.5))
        //                                      = 1 - depth * 0.5 * (1 - mod)
        float gainL = 1.0f - depth * 0.5f * (1.0f - modL);
        float gainR = 1.0f - depth * 0.5f * (1.0f - modR);

        float dryL = left;
        float dryR = right;

        float wetL = left * gainL;
        float wetR = right * gainR;

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Rate:         rate         = std::clamp(value, 0.1f, 20.0f);  break;
            case Depth:        depth        = std::clamp(value, 0.0f, 1.0f);   break;
            case Shape:        shape        = (ShapeType)std::clamp((int)value, 0, 2); break;
            case StereoOffset: stereoOffset = std::clamp(value, 0.0f, 1.0f);   break;
            case Mix:          mix          = std::clamp(value, 0.0f, 1.0f);   break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Rate:         return rate;
            case Depth:        return depth;
            case Shape:        return (float)shape;
            case StereoOffset: return stereoOffset;
            case Mix:          return mix;
            default:           return 0.0f;
        }
    }

private:
    float computeShape(float phase) const
    {
        switch (shape)
        {
            case Sine:
                return std::sin(phase * dsp::TWO_PI_F);

            case Triangle:
                return phase < 0.5f ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);

            case Square:
                return phase < 0.5f ? 1.0f : -1.0f;

            default:
                return std::sin(phase * dsp::TWO_PI_F);
        }
    }

    double lfoPhase = 0.0;
    float rate         = 4.0f;
    float depth        = 0.5f;
    ShapeType shape    = Sine;
    float stereoOffset = 0.0f;
};
