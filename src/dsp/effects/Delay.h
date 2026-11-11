#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>
#include <cmath>
#include <algorithm>

class Delay : public EffectBase
{
public:
    static const char* const* modeNames()
    {
        static const char* const names[] = { "STER", "PING", "TAPE" };
        return names;
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "TIME", 0, 0.01f, 2.0f, 0.375f, K::Log, "s" };
            case 1: return { "FDBK", 1, 0, 0.95f, 0.35f };
            case 2: return { "MIX", 2, 0, 1, 0.5f };
            case 3: return { "MODE", 3, 0, 2, 0, K::Choice, "", modeNames(), 3 };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case 0: setTime(value); break;
            case 1: setFeedback(value); break;
            case 2: setMix(value); break;
            case 3: setMode(static_cast<Mode>(std::clamp((int) std::lround(value), 0, 2))); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case 0: return delayTime;
            case 1: return feedback;
            case 2: return mix;
            case 3: return (float) (int) mode;
            default: return 0.0f;
        }
    }

    enum class Mode { Stereo, PingPong, Tape };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        recalcFilterCoeffs();
        reset();
    }

    void reset() override
    {
        bufferL.fill(0.0f);
        bufferR.fill(0.0f);
        writePos = 0;
        tapePhase = 0.0;
    }

    void setTime(float seconds) { delayTime = std::clamp(seconds, 0.01f, 2.0f); }
    void setFeedback(float fb) { feedback = std::clamp(fb, 0.0f, 0.95f); }
    void setMode(Mode m) { mode = m; }
    void setHighCut(float hz) { highCut = std::clamp(hz, 500.0f, 20000.0f); recalcFilterCoeffs(); }
    void setLowCut(float hz) { lowCut = std::clamp(hz, 20.0f, 2000.0f); recalcFilterCoeffs(); }
    void setTapeWow(float w) { tapeWow = std::clamp(w, 0.0f, 1.0f); }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        float delaySamples = (float)(delayTime * sampleRate);
        delaySamples = std::clamp(delaySamples, 1.0f, (float)(MAX_SAMPLES - 1));

        if (mode == Mode::Tape)
        {
            tapePhase += 0.3 / sampleRate;
            if (tapePhase >= 1.0) tapePhase -= 1.0;
            float wobble = (float)std::sin(tapePhase * dsp::TWO_PI_D) * tapeWow * 0.002f;
            delaySamples *= (1.0f + wobble);
            delaySamples = std::clamp(delaySamples, 1.0f, (float)(MAX_SAMPLES - 1));
        }

        float readL = readInterp(bufferL, delaySamples);
        float readR = readInterp(bufferR, delaySamples);

        readL = applyFilters(readL, filterStateL);
        readR = applyFilters(readR, filterStateR);

        float writeL, writeR;
        switch (mode)
        {
            case Mode::PingPong:
                writeL = left + readR * feedback;
                writeR = right + readL * feedback;
                break;
            case Mode::Stereo:
            case Mode::Tape:
            default:
                writeL = left + readL * feedback;
                writeR = right + readR * feedback;
                break;
        }

        bufferL[(size_t)writePos] = writeL;
        bufferR[(size_t)writePos] = writeR;
        writePos = (writePos + 1) % MAX_SAMPLES;

        left  = dryL * (1.0f - mix) + readL * mix;
        right = dryR * (1.0f - mix) + readR * mix;
    }

private:
    static constexpr int MAX_SAMPLES = 96000 * 2;

    std::array<float, MAX_SAMPLES> bufferL {};
    std::array<float, MAX_SAMPLES> bufferR {};
    int writePos = 0;

    float delayTime = 0.375f;
    float feedback = 0.4f;
    float highCut = 8000.0f;
    float lowCut = 80.0f;
    float tapeWow = 0.0f;
    Mode mode = Mode::Stereo;
    double tapePhase = 0.0;

    struct FilterState { float lpZ = 0.0f; float hpZ = 0.0f; };
    FilterState filterStateL, filterStateR;

    float readInterp(const std::array<float, MAX_SAMPLES>& buf, float delaySamples) const
    {
        float readPosF = (float)writePos - delaySamples;
        if (readPosF < 0.0f) readPosF += (float)MAX_SAMPLES;

        int idx0 = (int)readPosF;
        int idx1 = idx0 + 1;
        float frac = readPosF - (float)idx0;

        if (idx0 >= MAX_SAMPLES) idx0 -= MAX_SAMPLES;
        if (idx1 >= MAX_SAMPLES) idx1 -= MAX_SAMPLES;
        if (idx0 < 0) idx0 += MAX_SAMPLES;
        if (idx1 < 0) idx1 += MAX_SAMPLES;

        return buf[(size_t)idx0] * (1.0f - frac) + buf[(size_t)idx1] * frac;
    }

    float cachedLpCoeff = 0.5f;
    float cachedHpCoeff = 0.01f;

    void recalcFilterCoeffs()
    {
        if (sampleRate > 0.0)
        {
            cachedLpCoeff = 1.0f - std::exp(-dsp::TWO_PI_F * highCut / (float)sampleRate);
            cachedHpCoeff = 1.0f - std::exp(-dsp::TWO_PI_F * lowCut / (float)sampleRate);
        }
    }

    float applyFilters(float sample, FilterState& state) const
    {
        state.lpZ += cachedLpCoeff * (sample - state.lpZ);
        float filtered = state.lpZ;

        state.hpZ += cachedHpCoeff * (filtered - state.hpZ);
        filtered = filtered - state.hpZ;

        return filtered;
    }
};
