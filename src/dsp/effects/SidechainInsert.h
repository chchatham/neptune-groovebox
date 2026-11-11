#pragma once

#include "EffectBase.h"
#include "SidechainCompressor.h"

// Four-parameter front end for SidechainCompressor, so it can sit in an insert slot (ducking a
// track from another track, e.g. the bass from the kick). SidechainCompressor itself is unchanged;
// this class owns one and fixes the settings that do not fit on the page (attack 2 ms, 6 dB soft
// knee, peak detection, no make-up gain, detector high-pass off).
//
// SRC   what the detector listens to: SELF (ordinary compression) or track 1..16
// THRS  threshold
// RATI  ratio
// RELS  release time
//
// The effect cannot reach other tracks' audio by itself. Each block, before the insert chain of
// the track is processed, the audio engine must do, for every SidechainInsert in the chain:
//
//     int src = fx->getSidechainTrack();              // -1 = SELF, otherwise a 0-based track index
//     if (src >= 0 && src != thisTrack && <that track has audio for this block>)
//         fx->setSidechainBuffers(srcLeft, srcRight); // numSamples valid samples each
//
// The pointers are used for the next processBlock call only and then dropped, so a missing call
// simply falls back to SELF and a stale pointer is never read.
class SidechainInsert : public EffectBase
{
public:
    enum Param { Source = 0, Threshold, Ratio, Release };

    static constexpr int MAX_SOURCE_TRACKS = 16;

    static const char* const* sourceNames()
    {
        static const char* const names[] = { "SELF", "T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8",
                                             "T9", "T10", "T11", "T12", "T13", "T14", "T15", "T16" };
        return names;
    }

    SidechainInsert()
    {
        comp.setAttack(2.0f);
        comp.setKneeWidth(6.0f);
        comp.setMakeupGain(0.0f);
        comp.setSidechainHPF(0.0f);
        comp.setDetectionMode(SidechainCompressor::DetectionMode::Peak);
        comp.setThreshold(thresholdDb);
        comp.setRatio(ratio);
        comp.setRelease(releaseMs);
        prepare(44100.0, 0);
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "SRC", 0, 0, MAX_SOURCE_TRACKS, 0, K::Choice, "", sourceNames(), MAX_SOURCE_TRACKS + 1 };
            case 1: return { "THRS", 1, -60.0f, 0.0f, -24.0f, K::Linear, "dB" };
            case 2: return { "RATI", 2, 1.0f, 20.0f, 4.0f, K::Linear, "x" };
            case 3: return { "RELS", 3, 10.0f, 1000.0f, 150.0f, K::Log, "ms" };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Source:    source = std::clamp((int) std::lround(value), 0, MAX_SOURCE_TRACKS); break;
            case Threshold: thresholdDb = std::clamp(value, -60.0f, 0.0f); break;
            case Ratio:     ratio = std::clamp(value, 1.0f, 20.0f); break;
            case Release:   releaseMs = std::clamp(value, 10.0f, 1000.0f); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Source:    return (float) source;
            case Threshold: return thresholdDb;
            case Ratio:     return ratio;
            case Release:   return releaseMs;
            default:        return 0.0f;
        }
    }

    // -1 = the track's own signal, otherwise the 0-based index of the track to listen to.
    int getSidechainTrack() const { return source - 1; }

    // Detector input for the next processBlock call only (see the class comment).
    void setSidechainBuffers(const float* scLeft, const float* scRight)
    {
        pendingL = scLeft;
        pendingR = scRight;
    }

    // Gain reduction in dB (<= 0), for a meter.
    float getGainReductionDb() const { return comp.getGainReductionDb(); }

    void prepare(double sr, int maxBlockSize) override
    {
        sampleRate = sr;
        comp.prepare(sr, maxBlockSize);
        reset();
    }

    void reset() override
    {
        comp.reset();
        pendingL = pendingR = nullptr;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;
        applySettings();
        comp.setSidechainBuffers(nullptr, nullptr);
        comp.process(left, right);
    }

    void processBlock(float* left, float* right, int numSamples) override
    {
        if (!enabled)
        {
            pendingL = pendingR = nullptr;
            return;
        }

        applySettings();
        const bool external = (source > 0 && pendingL != nullptr && pendingR != nullptr);
        comp.setSidechainBuffers(external ? pendingL : nullptr, external ? pendingR : nullptr);
        comp.processBlock(left, right, numSamples);
        comp.setSidechainBuffers(nullptr, nullptr);
        pendingL = pendingR = nullptr;
    }

private:
    // Settings are handed to the compressor from the audio thread, so its coefficient updates
    // never race with processing.
    void applySettings()
    {
        if (thresholdDb != comp.getThreshold()) comp.setThreshold(thresholdDb);
        if (ratio != comp.getRatio()) comp.setRatio(ratio);
        if (releaseMs != comp.getRelease()) comp.setRelease(releaseMs);
    }

    int source = 0;
    float thresholdDb = -24.0f;
    float ratio = 4.0f;
    float releaseMs = 150.0f;

    SidechainCompressor comp;
    const float* pendingL = nullptr;
    const float* pendingR = nullptr;
};
