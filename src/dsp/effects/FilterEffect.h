#pragma once

// Insert-effect filter: a setting of the swept filter (SweptFilter.h). The whole filter set
// (state-variable, SEM-style, ladder, diode, comb) on any track, whatever its engine. Its four
// parameters are frequency, resonance, type and mode, so each can be locked per step from the FX
// page (filter sweeps and switches in a pattern). A moved CUT or RESO is glided to over a few
// milliseconds; another TYPE or MODE is exchanged in the dark (a dip of 3 ms, no click).
//
// On the second page, what the wah always had:
// RATE  an LFO on the cutoff (a sine)
// DPTH  how far it swings: up to three octaves either way
// ENV   the level of the key moves the cutoff: up to five octaves up (right of the middle) or down
// KEY   whose level that is: SELF, or IN = the track's input (another track opens or shuts the filter)

#include "SweptFilter.h"

class FilterEffect : public swp::Effect
{
public:
    enum Param { Frequency = 0, Resonance, Type, ModeParam };
    enum Extra { Rate = 0, Depth, Env, Key };
    static constexpr float LFO_OCTAVES = 3.0f, ENV_OCTAVES = 5.0f;

    FilterEffect()
    {
        using K = ParamKind;
        table[0] = { "CUT", 0, 20, 20000, 2000, K::Log, "Hz" };
        table[1] = { "RESO", 1, 0, 1, 0.2f };
        table[2] = { "TYPE", 2, 0, FilterSlot::NUM_MODELS - 2, 0, K::Choice, "", FilterSlot::modelNames(),
                     FilterSlot::NUM_MODELS - 1 };      // every type except OFF (use the ON cell)
        table[4 + Rate] = ParamSpec { "RATE", Rate, 0.05f, 20.0f, 1.0f, K::Log, "Hz" }.timed();
        table[4 + Depth] = { "DPTH", Depth, 0, 1, 0.0f };
        table[4 + Env] = { "ENV", Env, -1, 1, 0.0f };
        table[4 + Key] = dyn::keySpec(Key);
        modeSpecFor(0);
        seed();
    }

    int keyCell() const override { return 4 + Key; }

    // (MODE's list is the type's: with another type it is another list, and the mode is held to it.)
    void setParameter(int id, float value) override
    {
        swp::Effect::setParameter(id, value);
        if (id == Type)
        {
            modeSpecFor((int) cell(Type));
            setCell(ModeParam, cell(ModeParam));
        }
    }

protected:
    const ParamSpec* cells() const override { return table; }

    swp::Core::Voicing voicing() const override
    {
        swp::Core::Voicing v;
        v.baseMs = v.resoMs = 5.0f;
        v.lowHz = 20.0f;
        v.highHz = 20000.0f;
        return v;
    }

    void configure(const float* v) override
    {
        core.setFilter((int) v[Type], (int) v[ModeParam]);
        core.setBase(v[Frequency]);
        core.setResonance(v[Resonance]);
        core.setLfo(v[4 + Rate], 0, LFO_OCTAVES * v[4 + Depth], 0.0f);
        core.setEnvelope(ENV_OCTAVES * v[4 + Env]);
    }

private:
    void modeSpecFor(int typeIndex)
    {
        const auto model = FilterSlot::modelFromIndex(typeIndex);
        table[3] = { "MODE", 3, 0, (float) (FilterSlot::modeCount(model) - 1), 0, ParamKind::Choice, "",
                     FilterSlot::modeNames(model), FilterSlot::modeCount(model) };
    }

    ParamSpec table[CELLS];
};
