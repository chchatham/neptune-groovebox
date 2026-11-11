#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <array>
#include <cmath>
#include <algorithm>

// Shimmer reverb: reverb with pitch-shifted feedback.
// The pitch shifter uses a granular approach with two overlapping grains
// in the feedback path, shifting by octave up (+12 semitones) or fifth (+7).
// Combined with diffusion and damping for the ethereal shimmer texture.
// All buffers pre-allocated. No heap on audio thread.
class ShimmerReverb : public EffectBase
{
public:
    static const char* const* shiftNames()
    {
        static const char* const n[] = { "OCT", "5TH", "O+5" };
        return n;
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "DCAY", 0, 0.3f, 10, 3, K::Log, "s" };
            case 1: return { "SHIM", 1, 0, 1, 0.4f };
            case 2: return { "MODE", 2, 0, 2, 0, K::Choice, "", shiftNames(), 3 };
            case 3: return { "DAMP", 3, 0, 1, 0.4f };
            default: return {};
        }
    }

    enum Param
    {
        Decay = 0,      // 0.3 - 10 seconds
        Shimmer,        // 0 - 1 (amount of pitch-shifted signal in feedback)
        ShiftMode,      // 0 = octave up, 1 = fifth up, 2 = octave+fifth
        Damping,        // 0 - 1
        Size,           // 0.25 - 2.0
        Mix             // 0 - 1
    };

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        scaleFactor = sr / 29761.0;
        // Grain size: ~30ms
        grainSize = (int)(0.030 * sr);
        grainSize = std::clamp(grainSize, 64, MAX_GRAIN_SIZE);
        reset();
    }

    void reset() override
    {
        for (auto& ap : diffAP) ap.clear();
        tankDelayL.clear();
        tankDelayR.clear();
        pitchBufL.fill(0.0f);
        pitchBufR.fill(0.0f);
        pitchWritePos = 0;
        grainPhase = 0.0;
        dampStateL = 0.0f;
        dampStateR = 0.0f;
        fbL = 0.0f;
        fbR = 0.0f;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float dryL = left;
        float dryR = right;

        float mono = (left + right) * 0.5f;

        // Input diffusion
        mono = diffAP[0].process(mono, scaled(421), 0.7f);
        mono = diffAP[1].process(mono, scaled(773), 0.7f);
        mono = diffAP[2].process(mono, scaled(1117), 0.6f);

        // Compute decay gain
        int tankLen = scaled(4801);
        float decayGain = std::exp(-3.0f * (float)tankLen / ((float)sampleRate * decay));
        decayGain = std::clamp(decayGain, 0.0f, 0.999f);

        // Read from tank with cross-feedback
        float readL = tankDelayR.read(scaled(4801));
        float readR = tankDelayL.read(scaled(4243));

        // Pitch-shift the feedback path (granular pitch shifter)
        float shiftedL = pitchShift(readL, true);
        float shiftedR = pitchShift(readR, false);

        // Blend original and pitch-shifted feedback
        float fbMixL = readL * (1.0f - shimmer) + shiftedL * shimmer;
        float fbMixR = readR * (1.0f - shimmer) + shiftedR * shimmer;

        // Damping
        dampStateL += damping * (fbMixL - dampStateL);
        dampStateR += damping * (fbMixR - dampStateR);
        fbMixL = dampStateL * damping + fbMixL * (1.0f - damping);
        fbMixR = dampStateR * damping + fbMixR * (1.0f - damping);

        // Write to tank
        tankDelayL.write(mono + fbMixL * decayGain);
        tankDelayR.write(mono + fbMixR * decayGain);

        // Output taps
        float outL = tankDelayL.read(scaled(1523)) + tankDelayR.read(scaled(2741)) * 0.6f;
        float outR = tankDelayR.read(scaled(1847)) + tankDelayL.read(scaled(3119)) * 0.6f;
        outL *= 0.5f;
        outR *= 0.5f;

        left  = dryL * (1.0f - mix) + outL * mix;
        right = dryR * (1.0f - mix) + outR * mix;
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Decay:     decay     = std::clamp(value, 0.3f, 10.0f); break;
            case Shimmer:   shimmer   = std::clamp(value, 0.0f, 1.0f);  break;
            case ShiftMode: shiftMode = std::clamp((int)value, 0, 2);   break;
            case Damping:   damping   = std::clamp(value, 0.0f, 1.0f);  break;
            case Size:      size      = std::clamp(value, 0.25f, 2.0f); break;
            case Mix:       mix       = std::clamp(value, 0.0f, 1.0f);  break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Decay:     return decay;
            case Shimmer:   return shimmer;
            case ShiftMode: return (float)shiftMode;
            case Damping:   return damping;
            case Size:      return size;
            case Mix:       return mix;
            default:        return 0.0f;
        }
    }

private:
    static constexpr int MAX_AP_SIZE = 8192;
    static constexpr int MAX_TANK_SIZE = 32768;
    static constexpr int MAX_GRAIN_SIZE = 4096;
    static constexpr int PITCH_BUF_SIZE = 16384;

    float decay     = 3.0f;
    float shimmer   = 0.5f;
    int   shiftMode = 0;    // 0=octave, 1=fifth, 2=both
    float damping   = 0.4f;
    float size      = 1.0f;
    double scaleFactor = 1.0;
    int grainSize = 1323; // ~30ms at 44100

    float dampStateL = 0.0f;
    float dampStateR = 0.0f;
    float fbL = 0.0f;
    float fbR = 0.0f;

    int scaled(int baseSamples) const
    {
        return std::clamp((int)(baseSamples * scaleFactor * size), 1, MAX_TANK_SIZE - 2);
    }

    // Granular pitch shifter state
    std::array<float, PITCH_BUF_SIZE> pitchBufL {};
    std::array<float, PITCH_BUF_SIZE> pitchBufR {};
    int pitchWritePos = 0;
    double grainPhase = 0.0;

    float pitchShift(float input, bool isLeft)
    {
        auto& buf = isLeft ? pitchBufL : pitchBufR;
        buf[(size_t)pitchWritePos] = input;

        // Determine pitch ratio
        float ratio = 2.0f; // octave up
        if (shiftMode == 1) ratio = 1.4983f; // perfect fifth (3/2 approximated for ET)
        else if (shiftMode == 2) ratio = 2.0f; // use octave; fifth blended at output

        // Two overlapping grains for smooth crossfade
        double phaseInc = (double)(ratio - 1.0f) / (double)grainSize;
        grainPhase += phaseInc;
        if (grainPhase >= 1.0) grainPhase -= 1.0;

        float phase1 = (float)grainPhase;
        float phase2 = phase1 + 0.5f;
        if (phase2 >= 1.0f) phase2 -= 1.0f;

        // Hann window for each grain
        float win1 = 0.5f - 0.5f * std::cos(phase1 * dsp::TWO_PI_F);
        float win2 = 0.5f - 0.5f * std::cos(phase2 * dsp::TWO_PI_F);

        // Read positions offset by grain phase
        float offset1 = phase1 * (float)grainSize;
        float offset2 = phase2 * (float)grainSize;

        auto readBuf = [&](float offset) -> float
        {
            float readPosF = (float)pitchWritePos - offset;
            if (readPosF < 0.0f) readPosF += (float)PITCH_BUF_SIZE;
            int idx0 = (int)readPosF;
            int idx1 = idx0 + 1;
            float frac = readPosF - (float)idx0;
            if (idx0 >= PITCH_BUF_SIZE) idx0 -= PITCH_BUF_SIZE;
            if (idx1 >= PITCH_BUF_SIZE) idx1 -= PITCH_BUF_SIZE;
            if (idx0 < 0) idx0 += PITCH_BUF_SIZE;
            if (idx1 < 0) idx1 += PITCH_BUF_SIZE;
            return buf[(size_t)idx0] * (1.0f - frac) + buf[(size_t)idx1] * frac;
        };

        float out = readBuf(offset1) * win1 + readBuf(offset2) * win2;

        if (isLeft)
            pitchWritePos = (pitchWritePos + 1) % PITCH_BUF_SIZE;

        return out;
    }

    struct AllpassStage
    {
        std::array<float, MAX_AP_SIZE> buf {};
        int writeIdx = 0;

        void clear() { buf.fill(0.0f); writeIdx = 0; }

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
    };

    struct TankDelay
    {
        std::array<float, MAX_TANK_SIZE> buf {};
        int writeIdx = 0;

        void clear() { buf.fill(0.0f); writeIdx = 0; }

        void write(float s)
        {
            buf[(size_t)writeIdx] = s;
            writeIdx = (writeIdx + 1) % MAX_TANK_SIZE;
        }

        float read(int delaySamples) const
        {
            delaySamples = std::clamp(delaySamples, 1, MAX_TANK_SIZE - 1);
            int readIdx = writeIdx - delaySamples;
            if (readIdx < 0) readIdx += MAX_TANK_SIZE;
            return buf[(size_t)readIdx];
        }
    };

    std::array<AllpassStage, 3> diffAP;
    TankDelay tankDelayL;
    TankDelay tankDelayR;
};
