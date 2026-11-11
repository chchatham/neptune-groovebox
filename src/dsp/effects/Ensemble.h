#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>
#include <cmath>
#include <algorithm>

// BBD-style ensemble chorus with 3 voices.
// Models the bucket-brigade delay behavior of classic string ensemble circuits.
// Each voice uses a short modulated delay with slightly different LFO rates
// and a subtle feedback path. The delay times and modulation depths are
// carefully chosen for the warm, subtle thickening characteristic of
// classic ensembles (Roland Dimension D, Solina). No heap on audio thread.
class Ensemble : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "RATE", 0, 0.1f, 3.0f, 0.6f, K::Log, "Hz" };
            case 1: return { "DPTH", 1, 0, 1, 0.5f };
            case 2: return { "WDTH", 2, 0, 1, 0.8f };
            case 3: return { "MIX", 3, 0, 1, 0.5f };
            default: return {};
        }
    }

    enum Param
    {
        Rate = 0,       // 0.1 - 3 Hz (LFO rate)
        Depth,          // 0 - 1 (modulation depth)
        Width,          // 0 - 1 (stereo spread)
        Mix             // 0 - 1
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        maxDelaySamples = (int)(0.015 * sr) + 1; // 15ms max delay
        if (maxDelaySamples > MAX_DELAY_SAMPLES)
            maxDelaySamples = MAX_DELAY_SAMPLES;
        reset();
    }

    void reset() override
    {
        for (auto& line : delayLine)
            line.fill(0.0f);
        for (auto& wp : writePos)
            wp = 0;
        for (auto& p : lfoPhase)
            p = 0.0;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        // BBD-style: input goes to mono first
        float monoIn = (left + right) * 0.5f;

        float voiceOutL[NUM_VOICES];
        float voiceOutR[NUM_VOICES];

        for (int v = 0; v < NUM_VOICES; ++v)
        {
            // Each voice has a unique LFO rate multiplier for organic detuning
            float voiceRate = rate * voiceRateMultipliers[v];

            lfoPhase[v] += (double)voiceRate / sampleRate;
            if (lfoPhase[v] >= 1.0) lfoPhase[v] -= 1.0;

            // Triangle LFO for smoother BBD modulation (less zipper noise than sine)
            float phase = (float)lfoPhase[v];
            float lfoVal = phase < 0.5f ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);

            // BBD delay center: ~5ms, modulation range controlled by depth
            float centerDelay = 0.005f;
            float modRange = depth * 0.003f; // up to +/- 3ms
            float delaySec = centerDelay + lfoVal * modRange;
            delaySec = std::clamp(delaySec, 0.001f, 0.012f);
            float delaySamples = (float)(delaySec * sampleRate);

            // Write mono input to this voice's delay line
            delayLine[v][(size_t)writePos[v]] = monoIn;

            // Read with linear interpolation
            float readPosF = (float)writePos[v] - delaySamples;
            if (readPosF < 0.0f) readPosF += (float)maxDelaySamples;
            int idx0 = (int)readPosF;
            int idx1 = idx0 + 1;
            float frac = readPosF - (float)idx0;
            if (idx0 >= maxDelaySamples) idx0 -= maxDelaySamples;
            if (idx1 >= maxDelaySamples) idx1 -= maxDelaySamples;
            if (idx0 < 0) idx0 += maxDelaySamples;
            if (idx1 < 0) idx1 += maxDelaySamples;

            float delayed = lerp(delayLine[v][(size_t)idx0], delayLine[v][(size_t)idx1], frac);

            // Stereo placement based on width
            // Voice 0: slightly left, Voice 1: center, Voice 2: slightly right
            float panAngle = voicePanPositions[v] * width;
            voiceOutL[v] = delayed * (0.5f - panAngle * 0.5f);
            voiceOutR[v] = delayed * (0.5f + panAngle * 0.5f);

            writePos[v] = (writePos[v] + 1) % maxDelaySamples;
        }

        // Sum all voices
        float wetL = 0.0f;
        float wetR = 0.0f;
        for (int v = 0; v < NUM_VOICES; ++v)
        {
            wetL += voiceOutL[v];
            wetR += voiceOutR[v];
        }

        // Normalize
        float norm = 2.0f / (float)NUM_VOICES;
        wetL *= norm;
        wetR *= norm;

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Rate:  rate  = std::clamp(value, 0.1f, 3.0f);  break;
            case Depth: depth = std::clamp(value, 0.0f, 1.0f);  break;
            case Width: width = std::clamp(value, 0.0f, 1.0f);  break;
            case Mix:   mix   = std::clamp(value, 0.0f, 1.0f);  break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Rate:  return rate;
            case Depth: return depth;
            case Width: return width;
            case Mix:   return mix;
            default:    return 0.0f;
        }
    }

private:
    static constexpr int NUM_VOICES = 3;
    static constexpr int MAX_DELAY_SAMPLES = 3072; // ~15ms at 192kHz

    // Voice LFO rate multipliers (slightly detuned for organic sound)
    static constexpr float voiceRateMultipliers[NUM_VOICES] = { 1.0f, 1.13f, 0.87f };
    // Voice pan positions: -1 = left, 0 = center, 1 = right
    static constexpr float voicePanPositions[NUM_VOICES] = { -0.7f, 0.0f, 0.7f };

    std::array<std::array<float, MAX_DELAY_SAMPLES>, NUM_VOICES> delayLine {};
    std::array<int, NUM_VOICES> writePos {};
    std::array<double, NUM_VOICES> lfoPhase {};

    int maxDelaySamples = MAX_DELAY_SAMPLES;
    float rate  = 0.8f;
    float depth = 0.5f;
    float width = 0.7f;
};
