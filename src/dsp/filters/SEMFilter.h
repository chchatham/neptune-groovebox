#pragma once

// Two-pole multimode filter in the style of the classic expander-module state-variable design:
// 12 dB/oct, a single control sweeping low-pass -> notch -> high-pass, a separate band-pass output,
// and a resonance that gets strong and vocal but never self-oscillates. Overloading it rounds the
// sound off instead of making it harsh.
//
// The core is the same trapezoidal state-variable structure as SVFilter (Simper 2013 / Zavalishin);
// what makes it this filter is the mode mixing, the limited resonance range and the soft limiting
// of the integrator states.

#include "../DSPUtils.h"
#include <cmath>
#include <algorithm>

class SEMFilter
{
public:
    void setSampleRate(double sr) { sampleRate = sr; calcCoefficients(); }
    void setCutoff(float hz) { cutoff = hz; calcCoefficients(); }

    // 0..1. Full resonance is a damping of about 0.14 (Q near 7): loud, not oscillating.
    void setResonance(float r)
    {
        resonance = std::clamp(r, 0.0f, 1.0f);
        calcCoefficients();
    }

    // 0 = low-pass, 0.5 = notch, 1 = high-pass.
    void setMorph(float m) { morph = std::clamp(m, 0.0f, 1.0f); }
    void setBandPass(bool bp) { bandPass = bp; }
    void setDrive(float d) { drive = std::clamp(d, 0.0f, 1.0f); }

    float process(float input)
    {
        const float gain = 1.0f + drive * 3.0f;
        float v0 = input * gain;

        float v3 = v0 - ic2eq;
        float v1 = a1 * ic1eq + a2 * v3;
        float v2 = ic2eq + a2 * ic1eq + a3 * v3;

        // Integrator states pass through a gentle limiter: transparent at normal levels,
        // rounding off when the filter is pushed or resonating hard.
        ic1eq = softLimit(2.0f * v1 - ic1eq);
        ic2eq = softLimit(2.0f * v2 - ic2eq);

        float low = v2;
        float band = v1;
        float high = v0 - k * v1 - v2;

        float out;
        if (bandPass)
        {
            out = band * k;                         // unity gain at the centre frequency
        }
        else
        {
            // Low and high are cross-faded so that the middle of the control is the full notch.
            float lowAmt = std::min(1.0f, 2.0f * (1.0f - morph));
            float highAmt = std::min(1.0f, 2.0f * morph);
            out = low * lowAmt + high * highAmt;
        }
        return out / gain;
    }

    void reset()
    {
        ic1eq = 0.0f;
        ic2eq = 0.0f;
    }

private:
    static float softLimit(float x)
    {
        // Linear up to about +/-1, saturating towards +/-4.
        return 4.0f * dsp::fastTanh(x * 0.25f);
    }

    void calcCoefficients()
    {
        float fc = std::clamp(cutoff, 20.0f, (float) (sampleRate * 0.49));
        g = std::tan(dsp::PI_F * fc / (float) sampleRate);
        k = 2.0f - 1.86f * resonance;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    double sampleRate = 44100.0;
    float cutoff = 1000.0f;
    float resonance = 0.0f;
    float morph = 0.0f;
    float drive = 0.0f;
    bool bandPass = false;

    float g = 0.0f, k = 2.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    float ic1eq = 0.0f, ic2eq = 0.0f;
};
