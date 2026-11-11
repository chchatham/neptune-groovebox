#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>
#include <cmath>
#include <algorithm>

// Leslie rotary speaker simulation.
// Models the horn (high-frequency) and drum (low-frequency) rotors with
// a crossover filter separating the bands. Each rotor has amplitude modulation
// (doppler) and frequency modulation (pitch wobble) at slow/fast speeds.
// Includes spin-up/spin-down acceleration behavior.
// All buffers pre-allocated. No heap on audio thread.
class RotarySpeaker : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "SPD", 0, 0, 1, 0 };
            case 1: return { "HORN", 1, 5, 10, 6.7f, K::Linear, "Hz" };
            case 2: return { "DRUM", 2, 0.5f, 2, 1.0f, K::Linear, "Hz" };
            case 3: return { "ACCL", 3, 0, 1, 0.5f };
            default: return {};
        }
    }

    enum Param
    {
        Speed = 0,      // 0 = slow (chorale), 1 = fast (tremolo)
        HornRate,       // fast horn rate: 5 - 10 Hz
        DrumRate,       // fast drum rate: 0.5 - 2 Hz
        Acceleration,   // 0 - 1 (spin-up speed)
        CrossoverFreq,  // 500 - 2000 Hz
        Mix             // 0 - 1
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        maxDelaySamples = (int)(0.005 * sr) + 1; // 5ms max for doppler
        if (maxDelaySamples > MAX_DELAY_SAMPLES)
            maxDelaySamples = MAX_DELAY_SAMPLES;
        recalcCrossover();
        recalcAccelCoeff();
        reset();
    }

    void reset() override
    {
        hornDelayL.fill(0.0f);
        hornDelayR.fill(0.0f);
        drumDelayL.fill(0.0f);
        drumDelayR.fill(0.0f);
        hornWritePos = 0;
        drumWritePos = 0;
        hornPhase = 0.0;
        drumPhase = 0.0;
        currentHornRate = SLOW_HORN_RATE;
        currentDrumRate = SLOW_DRUM_RATE;
        crossLpL = 0.0f;
        crossLpR = 0.0f;
        crossHpL = 0.0f;
        crossHpR = 0.0f;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        // Smooth speed transitions (acceleration / deceleration)
        float targetHornRate = (speed > 0.5f) ? hornFastRate : SLOW_HORN_RATE;
        float targetDrumRate = (speed > 0.5f) ? drumFastRate : SLOW_DRUM_RATE;
        currentHornRate += cachedAccelCoeff * (targetHornRate - currentHornRate);
        currentDrumRate += cachedAccelCoeff * (targetDrumRate - currentDrumRate);

        // Crossover: one-pole LP/HP split
        crossLpL += cachedCrossCoeff * (left - crossLpL);
        crossLpR += cachedCrossCoeff * (right - crossLpR);
        float lowL = crossLpL;
        float lowR = crossLpR;
        float highL = left - lowL;
        float highR = right - lowR;

        // --- Horn rotor (high frequencies) ---
        hornPhase += currentHornRate / sampleRate;
        if (hornPhase >= 1.0) hornPhase -= 1.0;
        float hornAngle = (float)hornPhase * dsp::TWO_PI_F;
        float hornSin = std::sin(hornAngle);
        float hornCos = std::cos(hornAngle);

        // Doppler delay modulation for horn
        float hornDelayMs = 0.002f + hornSin * 0.001f; // 1-3ms
        float hornDelaySamples = (float)(hornDelayMs * sampleRate);
        hornDelaySamples = std::clamp(hornDelaySamples, 1.0f, (float)(maxDelaySamples - 1));

        hornDelayL[(size_t)hornWritePos] = highL;
        hornDelayR[(size_t)hornWritePos] = highR;

        float hornWetL = readInterp(hornDelayL, hornWritePos, hornDelaySamples);
        float hornWetR = readInterp(hornDelayR, hornWritePos, hornDelaySamples);
        hornWritePos = (hornWritePos + 1) % maxDelaySamples;

        // Amplitude modulation from horn rotation (stereo: L and R are opposite lobes)
        float hornAmpL = 0.7f + 0.3f * hornCos;
        float hornAmpR = 0.7f - 0.3f * hornCos;
        hornWetL *= hornAmpL;
        hornWetR *= hornAmpR;

        // --- Drum rotor (low frequencies) ---
        drumPhase += currentDrumRate / sampleRate;
        if (drumPhase >= 1.0) drumPhase -= 1.0;
        float drumAngle = (float)drumPhase * dsp::TWO_PI_F;
        float drumSin = std::sin(drumAngle);
        float drumCos = std::cos(drumAngle);

        // Doppler delay modulation for drum (slower, broader)
        float drumDelayMs = 0.003f + drumSin * 0.0015f;
        float drumDelaySamples = (float)(drumDelayMs * sampleRate);
        drumDelaySamples = std::clamp(drumDelaySamples, 1.0f, (float)(maxDelaySamples - 1));

        drumDelayL[(size_t)drumWritePos] = lowL;
        drumDelayR[(size_t)drumWritePos] = lowR;

        float drumWetL = readInterp(drumDelayL, drumWritePos, drumDelaySamples);
        float drumWetR = readInterp(drumDelayR, drumWritePos, drumDelaySamples);
        drumWritePos = (drumWritePos + 1) % maxDelaySamples;

        // Amplitude modulation from drum rotation
        float drumAmpL = 0.8f + 0.2f * drumCos;
        float drumAmpR = 0.8f - 0.2f * drumCos;
        drumWetL *= drumAmpL;
        drumWetR *= drumAmpR;

        // Recombine bands
        float wetL = hornWetL + drumWetL;
        float wetR = hornWetR + drumWetR;

        left  = dryL * (1.0f - mix) + wetL * mix;
        right = dryR * (1.0f - mix) + wetR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Speed:         speed         = std::clamp(value, 0.0f, 1.0f);     break;
            case HornRate:      hornFastRate  = std::clamp(value, 5.0f, 10.0f);    break;
            case DrumRate:      drumFastRate  = std::clamp(value, 0.5f, 2.0f);     break;
            case Acceleration:
                acceleration  = std::clamp(value, 0.0f, 1.0f);
                recalcAccelCoeff();
                break;
            case CrossoverFreq:
                crossoverFreq = std::clamp(value, 500.0f, 2000.0f);
                recalcCrossover();
                break;
            case Mix:           mix           = std::clamp(value, 0.0f, 1.0f);     break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Speed:         return speed;
            case HornRate:      return hornFastRate;
            case DrumRate:      return drumFastRate;
            case Acceleration:  return acceleration;
            case CrossoverFreq: return crossoverFreq;
            case Mix:           return mix;
            default:            return 0.0f;
        }
    }

private:
    static constexpr int MAX_DELAY_SAMPLES = 1024;
    static constexpr float SLOW_HORN_RATE = 0.8f;  // chorale horn speed
    static constexpr float SLOW_DRUM_RATE = 0.4f;  // chorale drum speed

    float speed         = 0.0f;     // 0 = slow, 1 = fast
    float hornFastRate  = 7.0f;     // Hz
    float drumFastRate  = 1.0f;     // Hz
    float acceleration  = 0.5f;
    float crossoverFreq = 800.0f;

    float currentHornRate = SLOW_HORN_RATE;
    float currentDrumRate = SLOW_DRUM_RATE;

    double hornPhase = 0.0;
    double drumPhase = 0.0;

    // Crossover filter state
    float crossLpL = 0.0f;
    float crossLpR = 0.0f;
    float crossHpL = 0.0f;
    float crossHpR = 0.0f;
    float cachedCrossCoeff = 0.5f;
    float cachedAccelCoeff = 0.1f;

    int maxDelaySamples = MAX_DELAY_SAMPLES;
    int hornWritePos = 0;
    int drumWritePos = 0;

    std::array<float, MAX_DELAY_SAMPLES> hornDelayL {};
    std::array<float, MAX_DELAY_SAMPLES> hornDelayR {};
    std::array<float, MAX_DELAY_SAMPLES> drumDelayL {};
    std::array<float, MAX_DELAY_SAMPLES> drumDelayR {};

    float readInterp(const std::array<float, MAX_DELAY_SAMPLES>& buf, int wp, float delaySamples) const
    {
        float readPosF = (float)wp - delaySamples;
        if (readPosF < 0.0f) readPosF += (float)maxDelaySamples;
        int idx0 = (int)readPosF;
        int idx1 = idx0 + 1;
        float frac = readPosF - (float)idx0;
        if (idx0 >= maxDelaySamples) idx0 -= maxDelaySamples;
        if (idx1 >= maxDelaySamples) idx1 -= maxDelaySamples;
        if (idx0 < 0) idx0 += maxDelaySamples;
        if (idx1 < 0) idx1 += maxDelaySamples;
        return buf[(size_t)idx0] * (1.0f - frac) + buf[(size_t)idx1] * frac;
    }

    void recalcCrossover()
    {
        if (sampleRate > 0.0)
            cachedCrossCoeff = 1.0f - std::exp(-dsp::TWO_PI_F * crossoverFreq / (float)sampleRate);
    }

    void recalcAccelCoeff()
    {
        if (sampleRate > 0.0)
            cachedAccelCoeff = 1.0f - std::exp(-acceleration * 10.0f / (float)sampleRate);
    }
};
