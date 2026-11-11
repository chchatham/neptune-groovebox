#pragma once

// Two-pole multimode filter (low-pass / high-pass / band-pass) with diode-style limiting in its
// resonance loop: the aggressive one. Driven hard it clips asymmetrically and screams at high
// resonance, in the manner of the diode-feedback two-pole designs of the 1970s.
//
// Core: the trapezoidal state-variable structure (Simper 2013; Zavalishin, "The Art of VA Filter
// Design", TPT and non-linear chapters). The diode behaviour is an asymmetric saturation of the
// integrator states, which bounds the loop: the filter can self-oscillate but cannot run away.
//
// (The previous version fed back a one-sample-delayed, non-monotonic clipper around two cascaded
// one-poles: it went unstable at high resonance with drive, and its "high-pass" output was not one.)

#include "../DSPUtils.h"
#include <cmath>
#include <algorithm>

class DiodeFilter
{
public:
    enum class Mode { LowPass, HighPass, BandPass };

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        calcCoefficients();
    }

    void setCutoff(float hz)
    {
        cutoff = hz;
        calcCoefficients();
    }

    // 0..1; self-oscillates at the top.
    void setResonance(float r)
    {
        resonance = std::clamp(r, 0.0f, 1.0f);
        calcCoefficients();
    }

    void setDrive(float d) { drive = std::clamp(d, 0.0f, 1.0f); }
    void setMode(Mode m) { mode = m; }

    float process(float input)
    {
        const float driveGain = 1.0f + drive * 8.0f;
        float v0 = input * driveGain;

        float v3 = v0 - ic2eq;
        float v1 = a1 * ic1eq + a2 * v3;
        float v2 = ic2eq + a2 * ic1eq + a3 * v3;

        ic1eq = diodeLimit(2.0f * v1 - ic1eq);
        ic2eq = diodeLimit(2.0f * v2 - ic2eq);

        float out;
        switch (mode)
        {
            case Mode::HighPass: out = v0 - k * v1 - v2; break;
            case Mode::BandPass: out = v1; break;
            case Mode::LowPass:
            default:             out = v2; break;
        }

        // Drive raises the level by about its square root: more bite and more loudness, not 9x.
        return out / std::sqrt(driveGain);
    }

    void reset()
    {
        ic1eq = 0.0f;
        ic2eq = 0.0f;
    }

private:
    void calcCoefficients()
    {
        float fc = std::clamp(cutoff, 20.0f, (float) (sampleRate * 0.49));
        g = std::tan(dsp::PI_F * fc / (float) sampleRate);
        k = 2.0f - 2.0f * resonance;          // 2 = no resonance, 0 = self-oscillation
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    // Asymmetric, bounded, monotonic: the positive half saturates softly, the negative half
    // earlier and harder, like a diode pair with unequal thresholds.
    static float diodeLimit(float x)
    {
        return x >= 0.0f ? 2.0f * dsp::fastTanh(x * 0.5f)
                         : 1.4f * dsp::fastTanh(x * (1.0f / 1.4f));
    }

    double sampleRate = 44100.0;
    float cutoff = 1000.0f;
    float resonance = 0.0f;
    float drive = 0.0f;
    Mode mode = Mode::LowPass;

    float g = 0.0f, k = 2.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    float ic1eq = 0.0f, ic2eq = 0.0f;
};
