#pragma once

#include <cmath>
#include <algorithm>
#include <string>
#include "../../engine/ParamSpec.h"

// Tempo divisions an effect's time or rate can be locked to, shortest first.
namespace FxSync
{
    static constexpr int NUM_DIVISIONS = 22;
    static constexpr int DEFAULT_DIVISION = 12;     // 1/4
    inline const char* const* divisionLabels()
    {
        static const char* const names[NUM_DIVISIONS] = {
            "1/128", "1/64", "1/32T", "1/32", "1/16T", "1/32D", "1/16", "1/8T", "1/16D", "1/8", "1/4T",
            "1/8D", "1/4", "1/2T", "1/4D", "1/2", "1/2D", "1BAR", "2BAR", "4BAR", "8BAR", "16BAR" };
        return names;
    }
    // Length of one division in beats (quarter notes); a bar is four beats.
    inline double divisionBeats(int index)
    {
        static const double beats[NUM_DIVISIONS] = {
            1.0 / 32, 1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 6, 3.0 / 16, 1.0 / 4, 1.0 / 3, 3.0 / 8, 1.0 / 2, 2.0 / 3,
            3.0 / 4, 1.0, 4.0 / 3, 1.5, 2.0, 3.0, 4.0, 8.0, 16.0, 32.0, 64.0 };
        return beats[std::clamp(index, 0, NUM_DIVISIONS - 1)];
    }
}

class EffectBase
{
public:
    virtual ~EffectBase() = default;

    void setSampleRate(double sr) { prepare(sr, 0); }

    virtual void prepare(double sr, int /*maxBlockSize*/)
    {
        sampleRate = sr;
    }

    virtual void reset() = 0;

    virtual void process(float& left, float& right) = 0;

    virtual void processBlock(float* left, float* right, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
            process(left[i], right[i]);
    }

    void processStereo(float* left, float* right, int numSamples)
    {
        processBlock(left, right, numSamples);
    }

    void setEnabled(bool e) { enabled = e; }
    bool isEnabled() const { return enabled; }

    void setMix(float m) { mix = std::clamp(m, 0.0f, 1.0f); }
    float getMix() const { return mix; }

    bool isBypassed() const { return !enabled; }

    virtual void setParameter(int /*id*/, float /*value*/) {}
    virtual float getParameter(int /*id*/) const { return 0.0f; }

    // Description of parameter 0..3 as shown on the FX page (label, native range, kind). The spec's
    // id is the index itself; values passed to setParameter / returned by getParameter are in the
    // native range.
    virtual ParamSpec getParamSpec(int /*index*/) const { return {}; }

    // ---- Tempo sync ---------------------------------------------------------------------------
    // An effect whose four parameters include a delay TIME (seconds) or an LFO RATE (Hz) can lock
    // it to a division of the tempo. While synced the division sets that parameter and the knob's
    // own value is kept for when sync is switched off. A division too long or too short for the
    // parameter's range is halved or doubled until it fits, so it stays in time.
    int getSyncParamIndex() const
    {
        for (int i = 0; i < 4; ++i)
        {
            const ParamSpec spec = getParamSpec(i);
            if (! spec.isValid() || spec.label == nullptr) continue;
            const std::string label(spec.label);
            if (label == "TIME" || label == "RATE") return i;
        }
        return -1;
    }
    bool canSync() const { return getSyncParamIndex() >= 0; }

    void setSync(bool on)
    {
        const int idx = getSyncParamIndex();
        if (idx < 0 || on == synced) return;
        if (on) freeValue = getParameter(idx);
        synced = on;
        if (on) applySync(); else setParameter(idx, freeValue);
    }
    bool isSynced() const { return synced; }
    void setSyncDivision(int division)
    {
        syncDivision = std::clamp(division, 0, FxSync::NUM_DIVISIONS - 1);
        applySync();
    }
    int getSyncDivision() const { return syncDivision; }
    void setTempo(float bpm)
    {
        if (bpm == tempoBpm || bpm <= 0.0f) return;
        tempoBpm = bpm;
        applySync();
    }

    // What the knob is set to: while synced, the stored free value rather than the division's.
    void setUserParameter(int index, float value)
    {
        if (synced && index == getSyncParamIndex()) { freeValue = value; return; }
        setParameter(index, value);
    }
    float getUserParameter(int index) const
    {
        return (synced && index == getSyncParamIndex()) ? freeValue : getParameter(index);
    }

protected:
    double sampleRate = 44100.0;
    bool enabled = true;
    float mix = 1.0f;

    static float lerp(float a, float b, float t) { return a + t * (b - a); }

private:
    void applySync()
    {
        const int idx = getSyncParamIndex();
        if (! synced || idx < 0) return;
        const ParamSpec spec = getParamSpec(idx);
        const double seconds = FxSync::divisionBeats(syncDivision) * 60.0 / (double) tempoBpm;
        const bool isTime = std::string(spec.label) == "TIME";
        double v = isTime ? seconds : 1.0 / seconds;
        const double lo = std::max(1.0e-4, (double) std::max(spec.minVal, isTime ? 0.001f : 0.01f)), hi = (double) spec.maxVal;
        while (v > hi && v * 0.5 >= lo) v *= 0.5;
        while (v < lo && v * 2.0 <= hi) v *= 2.0;
        setParameter(idx, (float) std::clamp(v, (double) spec.minVal, hi));
    }

    bool synced = false;
    int syncDivision = FxSync::DEFAULT_DIVISION;
    float tempoBpm = 120.0f;
    float freeValue = 0.0f;
};
