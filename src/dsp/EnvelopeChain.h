#pragma once

#include "Envelope.h"
#include <array>
#include <algorithm>

// Envelope chaining: series, parallel, and mixed mode routing
// for multi-stage envelope architectures.
//
// - Series: Env1 output feeds Env2 input (multiplicative gating).
//   When Env1 finishes release, Env2 begins.
// - Parallel: all envelopes trigger simultaneously on noteOn,
//   outputs combined (sum or max).
// - Mixed: groups of parallel envelopes chained in series.
//
// Voice integration: the chain can modulate amplitude, filter cutoff,
// or any other target via the outputLevel(). Wire it like:
//   float mod = chain.process();
//   cutoff *= mod;  // or amp *= mod;
//
// Audio-thread safe: fixed-size array, no heap allocation in process().

class EnvelopeChain
{
public:
    static constexpr int MaxEnvelopes = 8;

    enum class ChainMode { Series, Parallel, Mixed };

    // Parallel combine operation
    enum class CombineMode { Sum, Max, Multiply };

    // Which voice parameter this chain modulates
    enum class Target { Amplitude, FilterCutoff, Pitch, Custom };

    EnvelopeChain() = default;

    // Add an envelope stage. Returns the index, or -1 if full.
    int addEnvelope(const Envelope::Params& params)
    {
        if (envelopeCount >= MaxEnvelopes)
            return -1;

        int idx = envelopeCount++;
        envelopes[idx].setSampleRate(sampleRate);
        envelopes[idx].setParams(params);
        envelopes[idx].reset();
        // By default each envelope is in its own group (for mixed mode)
        groups[idx] = idx;
        return idx;
    }

    // Remove the last envelope. Returns the new count.
    int removeLastEnvelope()
    {
        if (envelopeCount > 0)
        {
            --envelopeCount;
            envelopes[envelopeCount].reset();
        }
        return envelopeCount;
    }

    // Set the chain mode
    void setMode(ChainMode m) { mode = m; }
    ChainMode getMode() const { return mode; }

    // Set the combine mode for parallel envelopes (Sum, Max, or Multiply)
    void setCombineMode(CombineMode m) { combineMode = m; }
    CombineMode getCombineMode() const { return combineMode; }

    // Set modulation target
    void setTarget(Target t) { target = t; }
    Target getTarget() const { return target; }

    // Set modulation depth (0.0 to 1.0)
    void setDepth(float d) { depth = std::clamp(d, 0.0f, 1.0f); }
    float getDepth() const { return depth; }

    // Assign an envelope to a group index (for Mixed mode).
    // Envelopes in the same group run in parallel; groups run in series
    // by ascending group number.
    void setGroup(int envelopeIndex, int groupId)
    {
        if (envelopeIndex >= 0 && envelopeIndex < envelopeCount)
            groups[envelopeIndex] = groupId;
    }

    // Mutable access to individual envelopes (for setting params from voice)
    Envelope& getEnvelopeMut(int index)
    {
        return envelopes[std::clamp(index, 0, MaxEnvelopes - 1)];
    }

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        for (int i = 0; i < MaxEnvelopes; ++i)
            envelopes[i].setSampleRate(sr);
    }

    void noteOn()
    {
        switch (mode)
        {
            case ChainMode::Series:
                // Reset all, trigger the first envelope
                for (int i = 0; i < envelopeCount; ++i)
                    envelopes[i].reset();
                if (envelopeCount > 0)
                {
                    activeSeriesIndex = 0;
                    envelopes[0].noteOn();
                }
                released = false;
                break;

            case ChainMode::Parallel:
                // Trigger all envelopes simultaneously
                for (int i = 0; i < envelopeCount; ++i)
                {
                    envelopes[i].reset();
                    envelopes[i].noteOn();
                }
                released = false;
                break;

            case ChainMode::Mixed:
            {
                // Reset all, then trigger only the first (lowest) group
                for (int i = 0; i < envelopeCount; ++i)
                    envelopes[i].reset();
                activeGroupId = findFirstGroup();
                for (int i = 0; i < envelopeCount; ++i)
                {
                    if (groups[i] == activeGroupId)
                        envelopes[i].noteOn();
                }
                released = false;
                break;
            }
        }
    }

    void noteOff()
    {
        released = true;
        // Release all currently active envelopes
        for (int i = 0; i < envelopeCount; ++i)
        {
            if (envelopes[i].isActive()
                && envelopes[i].getStage() != Envelope::Stage::Release)
            {
                envelopes[i].noteOff();
            }
        }
    }

    void reset()
    {
        for (int i = 0; i < envelopeCount; ++i)
            envelopes[i].reset();
        activeSeriesIndex = 0;
        activeGroupId = 0;
        released = false;
        lastOutput = 0.0f;
    }

    // Process and return the raw chain output (0.0 to 1.0 for typical use)
    float process()
    {
        if (envelopeCount == 0)
            return 0.0f;

        float raw = 0.0f;
        switch (mode)
        {
            case ChainMode::Series:
                raw = processSeries();
                break;
            case ChainMode::Parallel:
                raw = processParallel();
                break;
            case ChainMode::Mixed:
                raw = processMixed();
                break;
        }

        lastOutput = raw;
        return raw;
    }

    // Return the modulated value: applies depth-scaled chain output to a base value.
    // For amplitude: base * (1.0 - depth + depth * output)
    // For filter/pitch: base + depth * output * range
    float getModulatedValue(float baseValue, float modulationRange = 1.0f) const
    {
        if (envelopeCount == 0 || depth <= 0.0f)
            return baseValue;

        switch (target)
        {
            case Target::Amplitude:
                // Multiplicative: scales base by envelope output
                return baseValue * (1.0f - depth + depth * lastOutput);

            case Target::FilterCutoff:
            case Target::Pitch:
            case Target::Custom:
                // Additive: offset base by scaled envelope output
                return baseValue + depth * lastOutput * modulationRange;
        }
        return baseValue;
    }

    // Get the last computed output (for display, mod matrix readback)
    float getLastOutput() const { return lastOutput; }

    bool isActive() const
    {
        for (int i = 0; i < envelopeCount; ++i)
        {
            if (envelopes[i].isActive())
                return true;
        }
        return false;
    }

    int getStageCount() const { return envelopeCount; }

    // Returns the index of the currently active envelope (Series),
    // or the first active one (Parallel/Mixed), or -1 if idle.
    int getCurrentStage() const
    {
        if (mode == ChainMode::Series)
        {
            if (activeSeriesIndex < envelopeCount
                && envelopes[activeSeriesIndex].isActive())
                return activeSeriesIndex;
            return -1;
        }
        for (int i = 0; i < envelopeCount; ++i)
        {
            if (envelopes[i].isActive())
                return i;
        }
        return -1;
    }

    // Access individual envelopes (read-only, for display etc.)
    const Envelope& getEnvelope(int index) const
    {
        return envelopes[std::clamp(index, 0, MaxEnvelopes - 1)];
    }

private:
    float processSeries()
    {
        // In series mode, Env1 output gates Env2 input (multiplicative chaining).
        // Each envelope runs, and the outputs are multiplied together for
        // a multi-stage amplitude contour.
        if (activeSeriesIndex >= envelopeCount)
            return 0.0f;

        float level = envelopes[activeSeriesIndex].process();

        // In series mode, when an envelope reaches sustain, auto-trigger
        // its release so it progresses to idle and the next can start.
        if (!released
            && envelopes[activeSeriesIndex].getStage() == Envelope::Stage::Sustain)
        {
            envelopes[activeSeriesIndex].noteOff();
        }

        // If the current envelope has gone idle and we haven't been released,
        // advance to the next envelope in the series
        if (!envelopes[activeSeriesIndex].isActive() && !released)
        {
            activeSeriesIndex++;
            if (activeSeriesIndex < envelopeCount)
            {
                envelopes[activeSeriesIndex].noteOn();
                level = envelopes[activeSeriesIndex].process();
            }
        }

        return level;
    }

    float processParallel()
    {
        float result = 0.0f;
        bool first = true;

        for (int i = 0; i < envelopeCount; ++i)
        {
            float level = envelopes[i].process();
            switch (combineMode)
            {
                case CombineMode::Sum:
                    result += level;
                    break;
                case CombineMode::Max:
                    result = std::max(result, level);
                    break;
                case CombineMode::Multiply:
                    if (first)
                    {
                        result = level;
                        first = false;
                    }
                    else
                    {
                        result *= level;
                    }
                    break;
            }
        }

        return result;
    }

    float processMixed()
    {
        // Process all envelopes in the active group
        float result = 0.0f;
        bool activeGroupDone = true;
        bool first = true;

        for (int i = 0; i < envelopeCount; ++i)
        {
            if (groups[i] == activeGroupId)
            {
                float level = envelopes[i].process();
                switch (combineMode)
                {
                    case CombineMode::Sum:
                        result += level;
                        break;
                    case CombineMode::Max:
                        result = std::max(result, level);
                        break;
                    case CombineMode::Multiply:
                        if (first)
                        {
                            result = level;
                            first = false;
                        }
                        else
                        {
                            result *= level;
                        }
                        break;
                }

                // In mixed/series transition, auto-release when sustain is reached
                if (!released
                    && envelopes[i].getStage() == Envelope::Stage::Sustain)
                {
                    envelopes[i].noteOff();
                }

                if (envelopes[i].isActive())
                    activeGroupDone = false;
            }
        }

        // If the active group is done and we haven't been released,
        // advance to the next group
        if (activeGroupDone && !released)
        {
            int nextGroup = findNextGroup(activeGroupId);
            if (nextGroup >= 0)
            {
                activeGroupId = nextGroup;
                for (int i = 0; i < envelopeCount; ++i)
                {
                    if (groups[i] == activeGroupId)
                        envelopes[i].noteOn();
                }
                // Process one sample from the new group immediately
                result = 0.0f;
                first = true;
                for (int i = 0; i < envelopeCount; ++i)
                {
                    if (groups[i] == activeGroupId)
                    {
                        float level = envelopes[i].process();
                        switch (combineMode)
                        {
                            case CombineMode::Sum:
                                result += level;
                                break;
                            case CombineMode::Max:
                                result = std::max(result, level);
                                break;
                            case CombineMode::Multiply:
                                if (first)
                                {
                                    result = level;
                                    first = false;
                                }
                                else
                                {
                                    result *= level;
                                }
                                break;
                        }
                    }
                }
            }
        }

        return result;
    }

    // Find the lowest group id among all envelopes
    int findFirstGroup() const
    {
        int minGroup = groups[0];
        for (int i = 1; i < envelopeCount; ++i)
            minGroup = std::min(minGroup, groups[i]);
        return minGroup;
    }

    // Find the next group after the given one, or -1 if none
    int findNextGroup(int currentGroup) const
    {
        int nextGroup = -1;
        for (int i = 0; i < envelopeCount; ++i)
        {
            if (groups[i] > currentGroup)
            {
                if (nextGroup < 0 || groups[i] < nextGroup)
                    nextGroup = groups[i];
            }
        }
        return nextGroup;
    }

    std::array<Envelope, MaxEnvelopes> envelopes;
    std::array<int, MaxEnvelopes> groups{};
    int envelopeCount = 0;
    int activeSeriesIndex = 0;
    int activeGroupId = 0;
    double sampleRate = 44100.0;
    ChainMode mode = ChainMode::Series;
    CombineMode combineMode = CombineMode::Sum;
    Target target = Target::Amplitude;
    float depth = 1.0f;
    float lastOutput = 0.0f;
    bool released = false;
};
