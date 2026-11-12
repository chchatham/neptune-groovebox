#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>
#include <cmath>
#include <algorithm>

// Algorithmic hall reverb with longer diffusion tails and modulated tank delays.
// Topology: 4 input allpass diffusers -> FDN (feedback delay network) with
// 4 delay lines, Householder mixing matrix, damping, and modulated read heads.
// All buffers pre-allocated. No heap on audio thread.
class HallReverb : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "DCAY", 0, 0.5f, 10, 2.5f, K::Log, "s" };
            case 1: return { "SIZE", 1, 0.25f, 2, 1, K::Linear, "x" };
            case 2: return { "DAMP", 2, 0, 1, 0.4f };
            case 3: return { "PDLY", 3, 0, 100, 10, K::Linear, "ms" };
            default: return {};
        }
    }

    enum Param
    {
        DecayTime = 0,  // 0.5 - 10 seconds
        Size,           // 0.25 - 2.0 (scales delay lengths)
        Damping,        // 0 - 1 (high-frequency damping)
        PreDelay,       // 0 - 100 ms
        Modulation,     // 0 - 1 (chorus-like delay modulation depth)
        Mix             // 0 - 1
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        scaleFactor = sr / 29761.0;
        recalcLpCoeff();
        reset();
    }

    void reset() override
    {
        for (auto& ap : inputAP) ap.clear();
        for (auto& dl : fdnDelay) dl.clear();
        for (auto& d : dampState) d = 0.0f;
        for (auto& p : modPhase) p = 0.0;
        preDelayBuf.fill(0.0f);
        pdWritePos = 0;
        preLpZ = 0.0f;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        float mono = (left + right) * 0.5f;

        // Pre-lowpass
        preLpZ += cachedLpCoeff * (mono - preLpZ);
        mono = preLpZ;

        // Pre-delay
        int pdSamples = (int)(preDelay * 0.001f * (float)sampleRate);
        pdSamples = std::clamp(pdSamples, 0, MAX_PREDELAY - 1);
        preDelayBuf[(size_t)pdWritePos] = mono;
        int pdReadPos = pdWritePos - pdSamples;
        if (pdReadPos < 0) pdReadPos += MAX_PREDELAY;
        mono = preDelayBuf[(size_t)pdReadPos];
        pdWritePos = (pdWritePos + 1) % MAX_PREDELAY;

        // Input diffusion through 4 allpass stages
        mono = inputAP[0].process(mono, scaled(547), 0.7f);
        mono = inputAP[1].process(mono, scaled(953), 0.7f);
        mono = inputAP[2].process(mono, scaled(1361), 0.6f);
        mono = inputAP[3].process(mono, scaled(1777), 0.6f);
        // The level the diffusers had before they were made true allpasses (2026-11-11), so that
        // sounds and projects made with this reverb keep their balance.
        mono *= 4.9f;

        // Read from FDN delay lines with modulated read positions
        float fdnOut[NUM_FDN_LINES];
        for (int i = 0; i < NUM_FDN_LINES; ++i)
        {
            int baseDel = scaled(fdnBaseLengths[i]);
            float modDepth = modulation * 12.0f; // up to 12 samples of modulation

            // Advance mod LFO (each line has slightly different rate)
            modPhase[i] += fdnModRates[i] / sampleRate;
            if (modPhase[i] >= 1.0) modPhase[i] -= 1.0;
            float modOffset = std::sin((float)modPhase[i] * dsp::TWO_PI_F) * modDepth;

            float readDelay = (float)baseDel + modOffset;
            readDelay = std::clamp(readDelay, 1.0f, (float)(MAX_FDN_SIZE - 2));
            fdnOut[i] = fdnDelay[i].readInterp(readDelay);
        }

        // Householder mixing matrix: H = I - 2/N * ones (N=4)
        // Simpler: each output = sum of all inputs - 2 * own input, scaled by 0.5
        float sum = fdnOut[0] + fdnOut[1] + fdnOut[2] + fdnOut[3];
        float fdnIn[NUM_FDN_LINES];
        for (int i = 0; i < NUM_FDN_LINES; ++i)
        {
            fdnIn[i] = (sum - 2.0f * fdnOut[i]) * 0.5f;
        }

        // Compute decay gain per sample from decay time
        // Each delay line contributes; feedback = exp(-3 * delayLength / (decayTime * sampleRate))
        float decayGain = std::exp(-3.0f / (decayTime * (float)sampleRate) * (float)scaled(fdnBaseLengths[0]));
        decayGain = std::clamp(decayGain, 0.0f, 0.999f);

        // Write back with input injection, damping, and decay
        for (int i = 0; i < NUM_FDN_LINES; ++i)
        {
            float sig = mono + fdnIn[i] * decayGain;
            // Damping: one-pole lowpass in feedback path
            dampState[i] += damping * (sig - dampState[i]);
            sig = dampState[i] * damping + sig * (1.0f - damping);
            fdnDelay[i].write(sig);
        }

        // Tap stereo output from different delay lines
        float outL = fdnOut[0] + fdnOut[2] * 0.7f - fdnOut[3] * 0.3f;
        float outR = fdnOut[1] + fdnOut[3] * 0.7f - fdnOut[2] * 0.3f;
        outL *= 0.4f;
        outR *= 0.4f;

        left  = dryL * (1.0f - mix) + outL * mix;
        right = dryR * (1.0f - mix) + outR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case DecayTime:  decayTime  = std::clamp(value, 0.5f, 10.0f);  break;
            case Size:       size       = std::clamp(value, 0.25f, 2.0f);   break;
            case Damping:    damping    = std::clamp(value, 0.0f, 1.0f);    break;
            case PreDelay:   preDelay   = std::clamp(value, 0.0f, 100.0f);  break;
            case Modulation: modulation = std::clamp(value, 0.0f, 1.0f);    break;
            case Mix:        mix        = std::clamp(value, 0.0f, 1.0f);    break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case DecayTime:  return decayTime;
            case Size:       return size;
            case Damping:    return damping;
            case PreDelay:   return preDelay;
            case Modulation: return modulation;
            case Mix:        return mix;
            default:         return 0.0f;
        }
    }

private:
    static constexpr int NUM_FDN_LINES = 4;
    static constexpr int MAX_FDN_SIZE = 32768;
    static constexpr int MAX_AP_SIZE = 16384;
    static constexpr int MAX_PREDELAY = 8192;

    // Base delay lengths in samples at 29761 Hz reference rate
    // Chosen as mutually prime for maximal density
    static constexpr int fdnBaseLengths[NUM_FDN_LINES] = { 4799, 5399, 6199, 7001 };
    // Modulation LFO rates in Hz (slow, slightly different per line)
    static constexpr float fdnModRates[NUM_FDN_LINES] = { 0.31f, 0.37f, 0.43f, 0.29f };

    float decayTime  = 3.0f;
    float size       = 1.0f;
    float damping    = 0.5f;
    float preDelay   = 20.0f;
    float modulation = 0.3f;
    double scaleFactor = 1.0;
    float cachedLpCoeff = 0.5f;
    float preLpZ = 0.0f;

    int scaled(int baseSamples) const
    {
        return std::clamp((int)(baseSamples * scaleFactor * size), 1, MAX_FDN_SIZE - 2);
    }

    void recalcLpCoeff()
    {
        if (sampleRate > 0.0)
            cachedLpCoeff = 1.0f - std::exp(-dsp::TWO_PI_F * 10000.0f / (float)sampleRate);
    }

    struct AllpassStage
    {
        std::array<float, MAX_AP_SIZE> buf {};
        int writeIdx = 0;

        void clear()
        {
            buf.fill(0.0f);
            writeIdx = 0;
        }

        float process(float in, int delaySamples, float coeff)
        {
            delaySamples = std::clamp(delaySamples, 1, MAX_AP_SIZE - 1);
            int readIdx = writeIdx - delaySamples;
            if (readIdx < 0) readIdx += MAX_AP_SIZE;
            float delayed = buf[(size_t)readIdx];
            // Schroeder allpass: v = x + g v[n-D], y = v[n-D] - g v. (The output once used x in place
            // of v, which is a comb with gain above 1: the plate's tank fed on itself and never died.)
            const float v = in + delayed * coeff;
            float out = delayed - v * coeff;
            buf[(size_t)writeIdx] = v;
            writeIdx = (writeIdx + 1) % MAX_AP_SIZE;
            return out;
        }
    };

    struct FDNDelayLine
    {
        std::array<float, MAX_FDN_SIZE> buf {};
        int writeIdx = 0;

        void clear()
        {
            buf.fill(0.0f);
            writeIdx = 0;
        }

        void write(float sample)
        {
            buf[(size_t)writeIdx] = sample;
            writeIdx = (writeIdx + 1) % MAX_FDN_SIZE;
        }

        float readInterp(float delaySamples) const
        {
            float readPosF = (float)writeIdx - delaySamples;
            if (readPosF < 0.0f) readPosF += (float)MAX_FDN_SIZE;
            int idx0 = (int)readPosF;
            int idx1 = idx0 + 1;
            float frac = readPosF - (float)idx0;
            if (idx0 >= MAX_FDN_SIZE) idx0 -= MAX_FDN_SIZE;
            if (idx1 >= MAX_FDN_SIZE) idx1 -= MAX_FDN_SIZE;
            if (idx0 < 0) idx0 += MAX_FDN_SIZE;
            if (idx1 < 0) idx1 += MAX_FDN_SIZE;
            return buf[(size_t)idx0] * (1.0f - frac) + buf[(size_t)idx1] * frac;
        }
    };

    std::array<AllpassStage, 4> inputAP;
    std::array<FDNDelayLine, NUM_FDN_LINES> fdnDelay;
    std::array<float, NUM_FDN_LINES> dampState {};
    std::array<double, NUM_FDN_LINES> modPhase {};
    std::array<float, MAX_PREDELAY> preDelayBuf {};
    int pdWritePos = 0;
};
