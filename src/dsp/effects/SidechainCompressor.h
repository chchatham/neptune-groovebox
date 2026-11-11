#pragma once

#include "EffectBase.h"
#include "../DSPUtils.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <array>

/**
 * SidechainCompressor — dynamics compressor with optional external sidechain input.
 *
 * Inherits EffectBase. When no sidechain buffer is provided, the compressor
 * uses its own input for detection (normal compression). When a sidechain
 * buffer is supplied, that signal drives the detector while compression is
 * applied to the main input (classic sidechain ducking).
 *
 * Features:
 *   - Sidechain source: any track buffer, send bus, or external
 *   - Sidechain high-pass filter (6 dB/oct) to remove low-end from detector
 *   - Key-listen mode: monitor the sidechain signal instead of the compressed output
 *   - Peak or RMS detection
 *   - Soft-knee with configurable width
 *   - Makeup gain
 *   - Metering: gain reduction readout
 *
 * Audio-thread safe: no heap allocation in process path. All state is pre-allocated.
 *
 * Parameter IDs for setParameter/getParameter:
 *   0 = Threshold (-60..0 dB)
 *   1 = Ratio (1..20)
 *   2 = Attack (0.1..100 ms)
 *   3 = Release (10..1000 ms)
 *   4 = Makeup gain (0..30 dB)
 *   5 = Knee width (0..12 dB)
 *   6 = Sidechain HPF frequency (20..2000 Hz, 0 = off)
 *   7 = Key-listen (0 = off, 1 = on)
 *   8 = Detection mode (0 = peak, 1 = RMS)
 *   9 = Mix (0..1, dry/wet)
 */
class SidechainCompressor : public EffectBase
{
public:
    enum class DetectionMode { Peak, RMS };

    /// Identifies where the sidechain signal comes from
    enum class SidechainSource
    {
        Self,       ///< Use own input (normal compression)
        Track,      ///< Sidechain from a track buffer
        SendBus,    ///< Sidechain from a send bus buffer
        External    ///< Sidechain from an external buffer pointer
    };

    // Parameter indices
    static constexpr int PARAM_THRESHOLD   = 0;
    static constexpr int PARAM_RATIO       = 1;
    static constexpr int PARAM_ATTACK      = 2;
    static constexpr int PARAM_RELEASE     = 3;
    static constexpr int PARAM_MAKEUP      = 4;
    static constexpr int PARAM_KNEE        = 5;
    static constexpr int PARAM_SC_HPF      = 6;
    static constexpr int PARAM_KEY_LISTEN  = 7;
    static constexpr int PARAM_DETECT_MODE = 8;
    static constexpr int PARAM_MIX         = 9;
    static constexpr int NUM_PARAMS        = 10;

    SidechainCompressor() = default;

    // ----------------------------------------------------------------
    // EffectBase interface
    // ----------------------------------------------------------------

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        recalcCoeffs();
        recalcHPFCoeff();
    }

    void reset() override
    {
        envelope = 0.0f;
        gainSmooth = 1.0f;
        hpfStateL = 0.0f;
        hpfStateR = 0.0f;
    }

    /// Per-sample process (no sidechain — uses own input for detection)
    void process(float& left, float& right) override
    {
        if (!enabled) return;
        processWithSidechain(left, right, left, right);
    }

    /// Block process with optional sidechain buffer
    void processBlock(float* left, float* right, int numSamples) override
    {
        if (!enabled) return;

        if (sidechainL != nullptr && sidechainR != nullptr)
        {
            // External sidechain provided
            for (int i = 0; i < numSamples; ++i)
                processWithSidechain(left[i], right[i], sidechainL[i], sidechainR[i]);
        }
        else
        {
            // Self-detection (normal compression)
            for (int i = 0; i < numSamples; ++i)
                processWithSidechain(left[i], right[i], left[i], right[i]);
        }
    }

    // ----------------------------------------------------------------
    // Sidechain configuration
    // ----------------------------------------------------------------

    /// Set external sidechain buffer pointers (call before processBlock each block).
    /// Pass nullptr to revert to self-detection.
    void setSidechainBuffers(const float* scLeft, const float* scRight)
    {
        sidechainL = scLeft;
        sidechainR = scRight;
    }

    void setSidechainSource(SidechainSource src) { scSource = src; }
    SidechainSource getSidechainSource() const { return scSource; }

    /// Track or bus index that the sidechain listens to (interpretation depends on source)
    void setSidechainIndex(int index) { scIndex = index; }
    int getSidechainIndex() const { return scIndex; }

    // ----------------------------------------------------------------
    // Parameters
    // ----------------------------------------------------------------

    void setThreshold(float db)
    {
        thresholdDb = std::clamp(db, -60.0f, 0.0f);
    }

    void setRatio(float r)
    {
        ratio = std::clamp(r, 1.0f, 20.0f);
    }

    void setAttack(float ms)
    {
        attackMs = std::clamp(ms, 0.1f, 100.0f);
        recalcCoeffs();
    }

    void setRelease(float ms)
    {
        releaseMs = std::clamp(ms, 10.0f, 1000.0f);
        recalcCoeffs();
    }

    void setMakeupGain(float db)
    {
        makeupGainDb = std::clamp(db, 0.0f, 30.0f);
        makeupGainLinear = dsp::dbToLinear(makeupGainDb);
    }

    void setKneeWidth(float db)
    {
        kneeWidthDb = std::clamp(db, 0.0f, 12.0f);
    }

    void setDetectionMode(DetectionMode mode)
    {
        detectionMode = mode;
    }

    /// Set the sidechain high-pass filter frequency in Hz (0 = off, 20-2000)
    void setSidechainHPF(float freqHz)
    {
        scHPFFreq = std::clamp(freqHz, 0.0f, 2000.0f);
        recalcHPFCoeff();
    }

    /// Enable key-listen mode: outputs the sidechain signal for monitoring
    void setKeyListen(bool on) { keyListen = on; }
    bool isKeyListening() const { return keyListen; }

    float getThreshold() const { return thresholdDb; }
    float getRatio() const { return ratio; }
    float getAttack() const { return attackMs; }
    float getRelease() const { return releaseMs; }
    float getMakeupGain() const { return makeupGainDb; }
    float getKneeWidth() const { return kneeWidthDb; }
    float getSidechainHPF() const { return scHPFFreq; }
    DetectionMode getDetectionMode() const { return detectionMode; }

    /// Current gain reduction in dB (for metering, always <= 0)
    float getGainReductionDb() const { return currentGainReductionDb; }

    // ----------------------------------------------------------------
    // Generic parameter interface
    // ----------------------------------------------------------------

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case PARAM_THRESHOLD:   setThreshold(value); break;
            case PARAM_RATIO:       setRatio(value); break;
            case PARAM_ATTACK:      setAttack(value); break;
            case PARAM_RELEASE:     setRelease(value); break;
            case PARAM_MAKEUP:      setMakeupGain(value); break;
            case PARAM_KNEE:        setKneeWidth(value); break;
            case PARAM_SC_HPF:      setSidechainHPF(value); break;
            case PARAM_KEY_LISTEN:  setKeyListen(value >= 0.5f); break;
            case PARAM_DETECT_MODE: setDetectionMode(value >= 0.5f ? DetectionMode::RMS
                                                                    : DetectionMode::Peak); break;
            case PARAM_MIX:         setMix(value); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case PARAM_THRESHOLD:   return thresholdDb;
            case PARAM_RATIO:       return ratio;
            case PARAM_ATTACK:      return attackMs;
            case PARAM_RELEASE:     return releaseMs;
            case PARAM_MAKEUP:      return makeupGainDb;
            case PARAM_KNEE:        return kneeWidthDb;
            case PARAM_SC_HPF:      return scHPFFreq;
            case PARAM_KEY_LISTEN:  return keyListen ? 1.0f : 0.0f;
            case PARAM_DETECT_MODE: return detectionMode == DetectionMode::RMS ? 1.0f : 0.0f;
            case PARAM_MIX:         return mix;
            default: return 0.0f;
        }
    }

private:
    // ----------------------------------------------------------------
    // Core processing
    // ----------------------------------------------------------------

    void processWithSidechain(float& left, float& right, float scL, float scR)
    {
        // Apply HPF to sidechain signal
        if (scHPFFreq > 0.0f)
        {
            float filteredL = scL - hpfStateL;
            hpfStateL += hpfCoeff * (scL - hpfStateL);
            scL = filteredL;

            float filteredR = scR - hpfStateR;
            hpfStateR += hpfCoeff * (scR - hpfStateR);
            scR = filteredR;
        }

        // Key-listen: output sidechain signal for monitoring
        if (keyListen)
        {
            left = scL;
            right = scR;
            return;
        }

        // Detect level from sidechain
        float inputLevel = detectLevel(scL, scR);

        // Convert to dB
        float inputDb = dsp::linearToDb(inputLevel);

        // Compute gain reduction with soft knee
        float gainReductionDb = computeGainReduction(inputDb);

        // Smooth the gain change
        float targetGain = dsp::dbToLinear(gainReductionDb);
        smoothGain(targetGain);

        // Apply gain with makeup
        float totalGain = gainSmooth * makeupGainLinear;

        // Wet/dry mix
        float dryL = left;
        float dryR = right;
        left *= totalGain;
        right *= totalGain;

        if (mix < 1.0f)
        {
            left = lerp(dryL, left, mix);
            right = lerp(dryR, right, mix);
        }

        currentGainReductionDb = dsp::linearToDb(gainSmooth);
    }

    float detectLevel(float left, float right) const
    {
        if (detectionMode == DetectionMode::Peak)
            return std::max(std::abs(left), std::abs(right));
        else
            return std::sqrt(0.5f * (left * left + right * right));
    }

    float computeGainReduction(float inputDb) const
    {
        float halfKnee = kneeWidthDb * 0.5f;

        if (kneeWidthDb > 0.01f
            && inputDb > (thresholdDb - halfKnee)
            && inputDb < (thresholdDb + halfKnee))
        {
            // Soft knee: quadratic interpolation
            float x = inputDb - thresholdDb + halfKnee;
            return (1.0f / ratio - 1.0f) * x * x / (2.0f * kneeWidthDb);
        }
        else if (inputDb >= thresholdDb + halfKnee)
        {
            // Above threshold+knee: standard ratio
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

    void recalcCoeffs()
    {
        if (sampleRate <= 0.0) return;
        attackCoeff = std::exp(-1.0f / (float)(attackMs * 0.001 * sampleRate));
        releaseCoeff = std::exp(-1.0f / (float)(releaseMs * 0.001 * sampleRate));
    }

    void recalcHPFCoeff()
    {
        if (sampleRate <= 0.0 || scHPFFreq <= 0.0f)
        {
            hpfCoeff = 0.0f;
            return;
        }
        // Simple one-pole HPF coefficient: alpha = 1 / (1 + 2*pi*fc/fs)
        // We use the complementary: state += alpha * (input - state); output = input - state
        float w = 2.0f * dsp::PI_F * scHPFFreq / (float) sampleRate;
        hpfCoeff = w / (1.0f + w);
    }

    // Parameters
    float thresholdDb = -20.0f;
    float ratio = 4.0f;
    float attackMs = 10.0f;
    float releaseMs = 100.0f;
    float makeupGainDb = 0.0f;
    float kneeWidthDb = 3.0f;
    float scHPFFreq = 0.0f;         ///< Sidechain HPF frequency (0 = off)
    bool keyListen = false;          ///< Monitor the sidechain signal
    DetectionMode detectionMode = DetectionMode::Peak;

    // Derived coefficients
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;
    float makeupGainLinear = 1.0f;
    float hpfCoeff = 0.0f;

    // State (no heap allocation)
    float envelope = 0.0f;
    float gainSmooth = 1.0f;
    float currentGainReductionDb = 0.0f;
    float hpfStateL = 0.0f;
    float hpfStateR = 0.0f;

    // Sidechain buffer pointers (set per block, not owned)
    const float* sidechainL = nullptr;
    const float* sidechainR = nullptr;
    SidechainSource scSource = SidechainSource::Self;
    int scIndex = -1;               ///< Track/bus index for sidechain source
};
