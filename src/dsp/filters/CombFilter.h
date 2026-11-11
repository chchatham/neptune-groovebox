#pragma once

// Feedback comb filter: a short delay tuned to the cutoff frequency with feedback, giving a
// series of harmonic peaks (positive feedback) or odd-harmonic peaks an octave lower (negative).
// Standard structure (see J. O. Smith, "Physical Audio Signal Processing", feedback comb filters)
// with a one-pole low-pass in the loop for damping.

#include "../DSPUtils.h"
#include <vector>
#include <cmath>
#include <algorithm>

class CombFilter
{
public:
    void setSampleRate(double sr) { sampleRate = sr; calc(); }
    void setCutoff(float hz) { cutoff = hz; calc(); }
    void setResonance(float r) { feedback = std::clamp(r, 0.0f, 1.0f) * 0.97f; }
    void setNegative(bool n) { negative = n; }
    // 0 = bright, 1 = heavily damped feedback.
    void setDamping(float d) { damping = std::clamp(d, 0.0f, 1.0f) * 0.9f; }

    float process(float input)
    {
        // Read the delayed sample with linear interpolation.
        float readPos = (float) writePos - delaySamples;
        if (readPos < 0.0f) readPos += (float) SIZE;
        int i0 = (int) readPos;
        float frac = readPos - (float) i0;
        float delayed = buffer[(size_t) (i0 & MASK)] * (1.0f - frac) + buffer[(size_t) ((i0 + 1) & MASK)] * frac;

        // Damping in the loop.
        loopState += (1.0f - damping) * (delayed - loopState);
        float fb = loopState * (negative ? -feedback : feedback);

        float out = input + fb;
        buffer[(size_t) writePos] = dsp::fastTanh(out);     // the loop can never run away
        writePos = (writePos + 1) & MASK;

        // Keep the peaks from being much louder than the input.
        return out * (1.0f - 0.5f * feedback);
    }

    void reset()
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        loopState = 0.0f;
        writePos = 0;
    }

private:
    static constexpr int SIZE = 8192;       // 23 Hz at 192 kHz
    static constexpr int MASK = SIZE - 1;

    void calc()
    {
        float fc = std::clamp(cutoff, 20.0f, (float) (sampleRate * 0.45));
        delaySamples = std::clamp((float) (sampleRate / fc), 2.0f, (float) (SIZE - 2));
    }

    double sampleRate = 44100.0;
    float cutoff = 1000.0f;
    float feedback = 0.0f;
    float damping = 0.0f;
    bool negative = false;
    float delaySamples = 44.1f;
    float loopState = 0.0f;
    int writePos = 0;
    // On the heap: a voice holds several filters and should stay small enough to live anywhere.
    std::vector<float> buffer = std::vector<float>((size_t) SIZE, 0.0f);
};
