#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>
#include <cmath>
#include <algorithm>

// Plate reverb based on the Dattorro topology (1997).
// Four allpass diffusers → two cross-coupled tank delay lines with
// embedded allpass stages and damping filters. All buffers pre-allocated.
class Reverb : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "DCAY", 0, 0, 0.99f, 0.6f };
            case 1: return { "DAMP", 1, 0, 1, 0.4f };
            case 2: return { "SIZE", 2, 0.25f, 2.0f, 1.0f, K::Linear, "x" };
            case 3: return { "MIX", 3, 0, 1, 0.5f };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case 0: setDecay(value); break;
            case 1: setDamping(value); break;
            case 2: setSize(value); break;
            case 3: setMix(value); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case 0: return decay;
            case 1: return damping;
            case 2: return size;
            case 3: return mix;
            default: return 0.0f;
        }
    }

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
        for (auto& ap : tankAP) ap.clear();
        for (auto& dl : tankDelay) dl.clear();
        dampL = 0.0f;
        dampR = 0.0f;
        preLpZ = 0.0f;
    }

    void setDecay(float d) { decay = std::clamp(d, 0.0f, 0.99f); }
    void setDamping(float d) { damping = std::clamp(d, 0.0f, 1.0f); }
    void setPreDelay(float ms) { preDelayMs = std::clamp(ms, 0.0f, 100.0f); }
    void setSize(float s) { size = std::clamp(s, 0.25f, 2.0f); }
    void setHighCut(float hz) { preLpFreq = std::clamp(hz, 1000.0f, 20000.0f); recalcLpCoeff(); }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        float mono = (left + right) * 0.5f;

        preLpZ += cachedLpCoeff * (mono - preLpZ);
        mono = preLpZ;

        int pdSamples = (int)(preDelayMs * 0.001f * (float)sampleRate);
        pdSamples = std::clamp(pdSamples, 0, (int)preDelayBuf.size() - 1);
        preDelayBuf[(size_t)pdWritePos] = mono;
        int pdReadPos = pdWritePos - pdSamples;
        if (pdReadPos < 0) pdReadPos += (int)preDelayBuf.size();
        mono = preDelayBuf[(size_t)pdReadPos];
        pdWritePos = (pdWritePos + 1) % (int)preDelayBuf.size();

        mono = inputAP[0].process(mono, scaled(142), 0.75f);
        mono = inputAP[1].process(mono, scaled(107), 0.75f);
        mono = inputAP[2].process(mono, scaled(379), 0.625f);
        mono = inputAP[3].process(mono, scaled(277), 0.625f);

        float tankL = tankDelay[1].read(scaled(3720));
        float tankR = tankDelay[0].read(scaled(3163));

        float inL = mono + tankL * decay;
        float inR = mono + tankR * decay;

        inL = tankAP[0].process(inL, scaled(672), -decay);
        inL = dampFilter(inL, dampL);
        tankDelay[0].write(inL);

        inR = tankAP[1].process(inR, scaled(908), -decay);
        inR = dampFilter(inR, dampR);
        tankDelay[1].write(inR);

        float outL = tankDelay[0].read(scaled(266))
                   + tankDelay[0].read(scaled(2974))
                   - tankAP[1].readAt(scaled(1913))
                   + tankDelay[1].read(scaled(1996))
                   - tankDelay[0].read(scaled(1913))
                   - tankAP[0].readAt(scaled(1066))
                   - tankDelay[1].read(scaled(121));

        float outR = tankDelay[1].read(scaled(353))
                   + tankDelay[1].read(scaled(3627))
                   - tankAP[0].readAt(scaled(1228))
                   + tankDelay[0].read(scaled(2111))
                   - tankDelay[1].read(scaled(335))
                   - tankAP[1].readAt(scaled(187))
                   - tankDelay[0].read(scaled(1913));

        outL *= 0.6f;
        outR *= 0.6f;

        left  = dryL * (1.0f - mix) + outL * mix;
        right = dryR * (1.0f - mix) + outR * mix;
    }

private:
    float decay = 0.7f;
    float damping = 0.5f;
    float preDelayMs = 10.0f;
    float size = 1.0f;
    float preLpFreq = 10000.0f;
    double scaleFactor = 1.0;
    float cachedLpCoeff = 0.5f;
    float preLpZ = 0.0f;
    float dampL = 0.0f;
    float dampR = 0.0f;

    int scaled(int baseSamples) const
    {
        return std::max(1, (int)(baseSamples * scaleFactor * size));
    }

    void recalcLpCoeff()
    {
        if (sampleRate > 0.0)
            cachedLpCoeff = 1.0f - std::exp(-dsp::TWO_PI_F * preLpFreq / (float)sampleRate);
    }

    float dampFilter(float in, float& state) const
    {
        state = in * (1.0f - damping) + state * damping;
        return state;
    }

    static constexpr int MAX_AP_SIZE = 16384;
    static constexpr int MAX_DELAY_SIZE = 16384;
    static constexpr int MAX_PREDELAY = 8192;

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
            float out = -in * coeff + delayed;
            buf[(size_t)writeIdx] = in + delayed * coeff;
            writeIdx = (writeIdx + 1) % MAX_AP_SIZE;
            return out;
        }

        float readAt(int delaySamples) const
        {
            delaySamples = std::clamp(delaySamples, 1, MAX_AP_SIZE - 1);
            int readIdx = writeIdx - delaySamples;
            if (readIdx < 0) readIdx += MAX_AP_SIZE;
            return buf[(size_t)readIdx];
        }
    };

    struct DelayLine
    {
        std::array<float, MAX_DELAY_SIZE> buf {};
        int writeIdx = 0;

        void clear()
        {
            buf.fill(0.0f);
            writeIdx = 0;
        }

        void write(float sample)
        {
            buf[(size_t)writeIdx] = sample;
            writeIdx = (writeIdx + 1) % MAX_DELAY_SIZE;
        }

        float read(int delaySamples) const
        {
            delaySamples = std::clamp(delaySamples, 1, MAX_DELAY_SIZE - 1);
            int readIdx = writeIdx - delaySamples;
            if (readIdx < 0) readIdx += MAX_DELAY_SIZE;
            return buf[(size_t)readIdx];
        }
    };

    std::array<AllpassStage, 4> inputAP;
    std::array<AllpassStage, 2> tankAP;
    std::array<DelayLine, 2> tankDelay;
    std::array<float, MAX_PREDELAY> preDelayBuf {};
    int pdWritePos = 0;
};
