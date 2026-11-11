#pragma once

// Insert-effect filter: the whole filter set (state-variable, SEM-style, ladder, diode, comb) on
// any track, whatever its engine. Its four parameters are frequency, resonance, type and mode, so
// each can be locked per step from the FX page (filter sweeps and switches in a pattern).

#include "EffectBase.h"
#include "../filters/FilterSlot.h"

class FilterEffect : public EffectBase
{
public:
    enum Param { Frequency = 0, Resonance, Type, ModeParam };

    FilterEffect()
    {
        apply();
    }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        left.setSampleRate(sr);
        right.setSampleRate(sr);
        apply();
    }

    void reset() override
    {
        left.reset();
        right.reset();
    }

    void process(float& l, float& r) override
    {
        if (!enabled) return;
        float wetL = left.process(l);
        float wetR = right.process(r);
        l = l + mix * (wetL - l);
        r = r + mix * (wetR - r);
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        const auto model = FilterSlot::modelFromIndex(typeIndex);
        switch (index)
        {
            case 0: return { "CUT", 0, 20, 20000, 2000, K::Log, "Hz" };
            case 1: return { "RESO", 1, 0, 1, 0.2f };
            case 2: return { "TYPE", 2, 0, FilterSlot::NUM_MODELS - 2, 0, K::Choice, "", FilterSlot::modelNames(),
                             FilterSlot::NUM_MODELS - 1 };      // every type except OFF (use the ON cell)
            case 3: return { "MODE", 3, 0, (float) (FilterSlot::modeCount(model) - 1), 0, K::Choice, "",
                             FilterSlot::modeNames(model), FilterSlot::modeCount(model) };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Frequency: frequency = std::clamp(value, 20.0f, 20000.0f); break;
            case Resonance: resonance = std::clamp(value, 0.0f, 1.0f); break;
            case Type:
                typeIndex = std::clamp((int) std::lround(value), 0, FilterSlot::NUM_MODELS - 2);
                modeIndex = std::min(modeIndex, FilterSlot::modeCount(FilterSlot::modelFromIndex(typeIndex)) - 1);
                break;
            case ModeParam:
                modeIndex = std::clamp((int) std::lround(value), 0,
                                       FilterSlot::modeCount(FilterSlot::modelFromIndex(typeIndex)) - 1);
                break;
            default: return;
        }
        apply();
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Frequency: return frequency;
            case Resonance: return resonance;
            case Type:      return (float) typeIndex;
            case ModeParam: return (float) modeIndex;
            default:        return 0.0f;
        }
    }

private:
    void apply()
    {
        for (auto* f : { &left, &right })
        {
            f->setModel(0, FilterSlot::modelFromIndex(typeIndex));
            f->setMode(0, modeIndex);
            f->setCutoff(frequency);
            f->setResonance(resonance);
        }
    }

    FilterSlot left, right;
    float frequency = 2000.0f;
    float resonance = 0.2f;
    int typeIndex = 0;
    int modeIndex = 0;
};
