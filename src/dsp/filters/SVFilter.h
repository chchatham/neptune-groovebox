#pragma once

// Two-pole state-variable filter (12 dB/oct): low-pass, high-pass, band-pass, notch.
//
// Topology-preserving (trapezoidal) form: Andrew Simper, "Linear Trapezoidal Integrated SVF"
// (Cytomic technical paper, 2013), the same structure as the TPT SVF in Zavalishin, "The Art of VA
// Filter Design". Unlike the Chamberlin form it replaced, it is stable and correctly tuned up to
// Nyquist and at any resonance, so the cutoff is no longer limited to a quarter of the sample rate.

#include "../DSPUtils.h"
#include <cmath>
#include <algorithm>

class SVFilter
{
public:
    enum class Mode
    {
        LowPass,
        HighPass,
        BandPass,
        Notch
    };

    void setSampleRate(double sr) { sampleRate = sr; calcCoefficients(); }
    void setCutoff(float hz) { cutoff = hz; calcCoefficients(); }
    // Q: 0.5 = no resonance; the filter rings harder as it rises.
    void setResonance(float q) { resonance = q; calcCoefficients(); }
    void setMode(Mode m) { mode = m; }
    Mode getMode() const { return mode; }

    void setMorph(float m)
    {
        morph = std::clamp(m, 0.0f, 1.0f);
    }

    float process(float input)
    {
        tick(input);
        switch (mode)
        {
            case Mode::LowPass:    return low;
            case Mode::HighPass:   return high;
            case Mode::BandPass:   return band;
            case Mode::Notch:      return low + high;
        }
        return low;
    }

    // Continuous low-pass -> notch -> high-pass -> band-pass.
    float processMorphed(float input)
    {
        tick(input);
        float notch = low + high;

        if (morph <= 0.33f)
        {
            float t = morph / 0.33f;
            return low * (1.0f - t) + notch * t;
        }
        else if (morph <= 0.66f)
        {
            float t = (morph - 0.33f) / 0.33f;
            return notch * (1.0f - t) + high * t;
        }
        else
        {
            float t = (morph - 0.66f) / 0.34f;
            return high * (1.0f - t) + band * t;
        }
    }

    void reset()
    {
        ic1eq = 0.0f;
        ic2eq = 0.0f;
        low = band = high = 0.0f;
    }

private:
    void tick(float v0)
    {
        float v3 = v0 - ic2eq;
        float v1 = a1 * ic1eq + a2 * v3;
        float v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = 2.0f * v1 - ic1eq;
        ic2eq = 2.0f * v2 - ic2eq;

        band = v1;
        low = v2;
        high = v0 - k * v1 - v2;
    }

    void calcCoefficients()
    {
        float fc = std::clamp(cutoff, 20.0f, (float) (sampleRate * 0.49));
        g = std::tan(dsp::PI_F * fc / (float) sampleRate);
        k = 1.0f / std::max(resonance, 0.5f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    double sampleRate = 44100.0;
    float cutoff = 1000.0f;
    float resonance = 0.707f;
    Mode mode = Mode::LowPass;
    float morph = 0.0f;

    float g = 0.0f, k = 1.414f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    float ic1eq = 0.0f, ic2eq = 0.0f;
    float low = 0.0f, band = 0.0f, high = 0.0f;
};
