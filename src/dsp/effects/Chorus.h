#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>

// Multi-voice chorus effect.
// 3 voices with LFO-modulated delay lines at slightly different rates.
// All buffers pre-allocated in prepare(). No heap on audio thread.
class Chorus : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "RATE", 0, 0.1f, 5.0f, 0.8f, K::Log, "Hz" };
            case 1: return { "DPTH", 1, 0, 1, 0.5f };
            case 2: return { "MIX", 2, 0, 1, 0.5f };
            case 3: return { "VOIC", 3, 1, 3, 2, K::Int };
            default: return {};
        }
    }

    enum Param
    {
        Rate = 0,   // 0.1 - 5 Hz
        Depth,      // 0 - 1
        Mix,        // 0 - 1
        Voices      // 1 - 3
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        // Max delay = 20ms + some headroom for modulation
        maxDelaySamples = (int)(0.025 * sr) + 1;
        for (auto& line : delayLineL)
            std::fill(line.begin(), line.end(), 0.0f);
        for (auto& line : delayLineR)
            std::fill(line.begin(), line.end(), 0.0f);
        for (auto& wp : writePos)
            wp = 0;
        for (auto& p : lfoPhase)
            p = 0.0;
        reset();
    }

    void reset() override
    {
        for (auto& line : delayLineL)
            std::fill(line.begin(), line.end(), 0.0f);
        for (auto& line : delayLineR)
            std::fill(line.begin(), line.end(), 0.0f);
        for (auto& wp : writePos)
            wp = 0;
        for (auto& p : lfoPhase)
            p = 0.0;
    }

    void process(float& left, float& right) override
    {
        if (!enabled)
            return;

        float dryL = left;
        float dryR = right;
        float wetL = 0.0f;
        float wetR = 0.0f;

        int numVoices = std::clamp((int)voices, 1, MAX_VOICES);

        for (int v = 0; v < numVoices; ++v)
        {
            // Each voice has a slightly different LFO rate
            // Voice 0: base rate, Voice 1: 1.1x, Voice 2: 0.9x
            float voiceRate = rate;
            if (v == 1) voiceRate *= 1.1f;
            else if (v == 2) voiceRate *= 0.93f;

            lfoPhase[v] += voiceRate / sampleRate;
            while (lfoPhase[v] >= 1.0)
                lfoPhase[v] -= 1.0;

            // LFO output: sine
            float lfoVal = std::sin((float)lfoPhase[v] * dsp::TWO_PI_F);

            // Delay time: center at ~7ms, modulate by depth (1-20ms range)
            float centerDelay = 0.007f; // 7ms center
            float modRange = depth * 0.006f; // up to +/-6ms
            float delaySec = centerDelay + lfoVal * modRange;
            delaySec = std::clamp(delaySec, 0.001f, 0.020f);
            float delaySamples = (float)(delaySec * sampleRate);

            // Write to delay line
            delayLineL[v][writePos[v]] = left;
            delayLineR[v][writePos[v]] = right;

            // Read from delay line with linear interpolation
            float readPosF = (float)writePos[v] - delaySamples;
            if (readPosF < 0.0f)
                readPosF += (float)maxDelaySamples;

            int readIdx0 = (int)readPosF;
            int readIdx1 = readIdx0 + 1;
            float frac = readPosF - (float)readIdx0;

            if (readIdx0 >= maxDelaySamples) readIdx0 -= maxDelaySamples;
            if (readIdx1 >= maxDelaySamples) readIdx1 -= maxDelaySamples;
            if (readIdx0 < 0) readIdx0 += maxDelaySamples;
            if (readIdx1 < 0) readIdx1 += maxDelaySamples;

            wetL += lerp(delayLineL[v][readIdx0], delayLineL[v][readIdx1], frac);
            wetR += lerp(delayLineR[v][readIdx0], delayLineR[v][readIdx1], frac);

            writePos[v] = (writePos[v] + 1) % maxDelaySamples;
        }

        // Normalize wet signal by number of active voices
        float norm = 1.0f / (float)numVoices;
        wetL *= norm;
        wetR *= norm;

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Rate:   rate   = std::clamp(value, 0.1f, 5.0f); break;
            case Depth:  depth  = std::clamp(value, 0.0f, 1.0f); break;
            case Mix:    mix    = std::clamp(value, 0.0f, 1.0f); break;
            case Voices: voices = std::clamp(value, 1.0f, 3.0f); break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Rate:   return rate;
            case Depth:  return depth;
            case Mix:    return mix;
            case Voices: return voices;
            default:     return 0.0f;
        }
    }

private:
    static constexpr int MAX_VOICES = 3;
    // Pre-allocated delay lines: max 25ms at 192kHz = 4800 samples, round up
    static constexpr int MAX_DELAY_SAMPLES = 5120;

    std::array<std::array<float, MAX_DELAY_SAMPLES>, MAX_VOICES> delayLineL {};
    std::array<std::array<float, MAX_DELAY_SAMPLES>, MAX_VOICES> delayLineR {};
    std::array<int, MAX_VOICES> writePos {};
    std::array<double, MAX_VOICES> lfoPhase {};

    int maxDelaySamples = MAX_DELAY_SAMPLES;
    float rate  = 1.0f;
    float depth = 0.5f;
    float voices = 3.0f;
};
