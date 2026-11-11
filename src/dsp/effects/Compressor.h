#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <cmath>
#include <algorithm>

// Dynamics compressor with RMS/peak detection, soft knee, and makeup gain.
// No heap allocation. All state is pre-allocated member variables.
// Gain computation follows standard feed-forward topology.
class Compressor : public EffectBase
{
public:
    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "THR", 0, -60, 0, -12, K::Linear, "dB" };
            case 1: return { "RATI", 1, 1, 20, 4, K::Log, "x" };
            case 2: return { "ATK", 2, 0.1f, 100, 10, K::Log, "ms" };
            case 3: return { "MAKE", 3, 0, 30, 0, K::Linear, "dB" };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case 0: setThreshold(value); break;
            case 1: setRatio(value); break;
            case 2: setAttack(value); break;
            case 3: setMakeupGain(value); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case 0: return thresholdDb;
            case 1: return ratio;
            case 2: return attackMs;
            case 3: return makeupGainDb;
            default: return 0.0f;
        }
    }

    enum class DetectionMode { Peak, RMS };

    Compressor() = default;

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        recalcCoeffs();
    }

    void reset() override
    {
        envelope = 0.0f;
        gainSmooth = 1.0f;
        rmsSum = 0.0f;
        rmsSampleCount = 0;
    }

    // Threshold in dB (-60 to 0)
    void setThreshold(float db)
    {
        thresholdDb = std::clamp(db, -60.0f, 0.0f);
    }

    // Ratio (1.0 = no compression, 20.0 = brickwall)
    void setRatio(float r)
    {
        ratio = std::clamp(r, 1.0f, 20.0f);
    }

    // Attack in milliseconds (0.1 to 100)
    void setAttack(float ms)
    {
        attackMs = std::clamp(ms, 0.1f, 100.0f);
        recalcCoeffs();
    }

    // Release in milliseconds (10 to 1000)
    void setRelease(float ms)
    {
        releaseMs = std::clamp(ms, 10.0f, 1000.0f);
        recalcCoeffs();
    }

    // Makeup gain in dB (0 to 30)
    void setMakeupGain(float db)
    {
        makeupGainDb = std::clamp(db, 0.0f, 30.0f);
        makeupGainLinear = dsp::dbToLinear(makeupGainDb);
    }

    // Soft knee width in dB (0 = hard knee, up to 12)
    void setKneeWidth(float db)
    {
        kneeWidthDb = std::clamp(db, 0.0f, 12.0f);
    }

    void setDetectionMode(DetectionMode mode)
    {
        detectionMode = mode;
    }

    // Current gain reduction in dB (for metering)
    float getGainReductionDb() const
    {
        return currentGainReductionDb;
    }

    float getThreshold() const { return thresholdDb; }
    float getRatio() const { return ratio; }
    float getAttack() const { return attackMs; }
    float getRelease() const { return releaseMs; }
    float getMakeupGain() const { return makeupGainDb; }
    float getKneeWidth() const { return kneeWidthDb; }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        // Detect level
        float inputLevel = detectLevel(left, right);

        // Convert to dB
        float inputDb = linearToDb(inputLevel);

        // Compute gain reduction
        float gainReductionDb = computeGainReduction(inputDb);

        // Smooth the gain
        float targetGain = dbToLinear(gainReductionDb);
        smoothGain(targetGain);

        // Apply gain and makeup
        float totalGain = gainSmooth * makeupGainLinear;
        left *= totalGain;
        right *= totalGain;

        currentGainReductionDb = linearToDb(gainSmooth);
    }

private:
    // Parameters
    float thresholdDb = -20.0f;
    float ratio = 4.0f;
    float attackMs = 10.0f;
    float releaseMs = 100.0f;
    float makeupGainDb = 0.0f;
    float kneeWidthDb = 3.0f;
    DetectionMode detectionMode = DetectionMode::Peak;

    // Derived
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;
    float makeupGainLinear = 1.0f;

    // State
    float envelope = 0.0f;
    float gainSmooth = 1.0f;
    float currentGainReductionDb = 0.0f;

    // RMS state (fixed window of ~3ms worth of samples, approximated with running average)
    float rmsSum = 0.0f;
    int rmsSampleCount = 0;
    static constexpr int RMS_WINDOW = 128;

    void recalcCoeffs()
    {
        if (sampleRate <= 0.0) return;
        // Time constants: coeff = exp(-1 / (time_in_seconds * sampleRate))
        attackCoeff = std::exp(-1.0f / (float)(attackMs * 0.001 * sampleRate));
        releaseCoeff = std::exp(-1.0f / (float)(releaseMs * 0.001 * sampleRate));
    }

    float detectLevel(float left, float right) const
    {
        if (detectionMode == DetectionMode::Peak)
        {
            return std::max(std::abs(left), std::abs(right));
        }
        else
        {
            // Approximate RMS from stereo pair
            return std::sqrt(0.5f * (left * left + right * right));
        }
    }

    float computeGainReduction(float inputDb) const
    {
        float halfKnee = kneeWidthDb * 0.5f;

        if (kneeWidthDb > 0.01f && inputDb > (thresholdDb - halfKnee)
            && inputDb < (thresholdDb + halfKnee))
        {
            // Soft knee: quadratic interpolation
            float x = inputDb - thresholdDb + halfKnee;
            float compressionDb = (1.0f / ratio - 1.0f) * x * x / (2.0f * kneeWidthDb);
            return compressionDb;
        }
        else if (inputDb >= thresholdDb + halfKnee)
        {
            // Above knee: standard compression
            float overDb = inputDb - thresholdDb;
            return overDb * (1.0f / ratio - 1.0f);
        }

        return 0.0f; // Below threshold
    }

    void smoothGain(float targetGain)
    {
        if (targetGain < gainSmooth)
            gainSmooth = attackCoeff * gainSmooth + (1.0f - attackCoeff) * targetGain;
        else
            gainSmooth = releaseCoeff * gainSmooth + (1.0f - releaseCoeff) * targetGain;
    }

    static float linearToDb(float linear) { return dsp::linearToDb(linear); }
    static float dbToLinear(float db) { return dsp::dbToLinear(db); }
};
