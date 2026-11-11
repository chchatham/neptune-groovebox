#pragma once

// Four-pole transistor-ladder low-pass with the ladder's own non-linearity in every stage.
//
// Algorithm: Antti Huovilainen, "Non-linear digital implementation of the Moog ladder filter"
// (DAFx-04): four one-pole stages each with a tanh, run at twice the sample rate, a half-sample
// averaged feedback path, and the paper's polynomial corrections for tuning and resonance.
//
// (Until iteration 51 the stage coefficient here lacked the 1 - exp(-2 pi f) mapping, which put the
// real cutoff about six times below the requested one and kept the filter from self-oscillating.)

#include "../DSPUtils.h"
#include <cmath>
#include <algorithm>

class LadderFilter
{
public:
    // Outputs taken by mixing the ladder's stages (the classic pole-mixing technique): the full
    // 24 dB low-pass, the gentler 12 dB tap, and band-pass / high-pass responses that keep the
    // ladder's resonance character.
    enum class Mode { LowPass24, LowPass18, LowPass12, LowPass6, BandPass, HighPass };

    void setMode(Mode m) { mode = m; }
    Mode getMode() const { return mode; }

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

    // 0..1; self-oscillation starts just below 1.
    void setResonance(float r)
    {
        resonance = std::clamp(r, 0.0f, 1.0f);
        calcCoefficients();
    }

    // Extra input drive into the ladder (0..1).
    void setSaturation(float s) { saturation = s; }

    float process(float input)
    {
        const float in = input * (1.0f + saturation * 3.0f);
        float out = 0.0f;

        for (int os = 0; os < 2; ++os)
        {
            // Feedback is the average of the last two outputs (half a sample of delay).
            y0 = dsp::fastTanh(in - resonK * feedback);

            stage[0] += tune * (y0 - stageTanh[0]);
            stageTanh[0] = dsp::fastTanh(stage[0]);

            stage[1] += tune * (stageTanh[0] - stageTanh[1]);
            stageTanh[1] = dsp::fastTanh(stage[1]);

            stage[2] += tune * (stageTanh[1] - stageTanh[2]);
            stageTanh[2] = dsp::fastTanh(stage[2]);

            stage[3] += tune * (stageTanh[2] - dsp::fastTanh(stage[3]));

            feedback = 0.5f * (stage[3] + lastStage3);
            lastStage3 = stage[3];

            switch (mode)
            {
                case Mode::LowPass24: out = feedback; break;
                case Mode::LowPass18: out = stage[2]; break;
                case Mode::LowPass12: out = stage[1]; break;
                case Mode::LowPass6:  out = stage[0]; break;
                case Mode::BandPass:  out = 4.0f * (stage[1] - 2.0f * stage[2] + stage[3]); break;
                case Mode::HighPass:  out = y0 - 4.0f * stage[0] + 6.0f * stage[1] - 4.0f * stage[2] + stage[3]; break;
            }
        }

        // A ladder's pass-band drops as resonance rises; half of that loss is made up here so the
        // low-pass outputs do not thin out as much when resonance is turned up.
        if (mode != Mode::BandPass && mode != Mode::HighPass)
            out *= 1.0f + 0.5f * resonK;

        return out;
    }

    void reset()
    {
        for (auto& s : stage)
            s = 0.0f;
        for (auto& t : stageTanh)
            t = 0.0f;
        feedback = 0.0f;
        lastStage3 = 0.0f;
        y0 = 0.0f;
    }

private:
    void calcCoefficients()
    {
        // Normalised to the sample rate; the loop runs twice per sample, hence f = fc / 2.
        float fc = std::clamp(cutoff, 20.0f, (float) (sampleRate * 0.45)) / (float) sampleRate;
        float f = fc * 0.5f;
        float fc2 = fc * fc;
        float fc3 = fc2 * fc;

        // Huovilainen's corrections: fcr keeps the tuning, acr the resonance, right across the range.
        float fcr = 1.8730f * fc3 + 0.4955f * fc2 - 0.6490f * fc + 0.9988f;
        float acr = -3.9364f * fc2 + 1.8409f * fc + 0.9968f;

        tune = 1.0f - std::exp(-dsp::TWO_PI_F * f * fcr);
        resonK = 4.0f * resonance * acr;
    }

    double sampleRate = 44100.0;
    float cutoff = 1000.0f;
    float resonance = 0.0f;
    float saturation = 0.0f;

    float tune = 0.0f;
    float resonK = 0.0f;
    float stage[4] = {};
    float stageTanh[3] = {};
    float feedback = 0.0f;
    float lastStage3 = 0.0f;
    float y0 = 0.0f;
    Mode mode = Mode::LowPass24;
};
