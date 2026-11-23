#pragma once

#include "../ProjectBridge.h"
#include "../StoredLists.h"
#include "../../engine/ModRouter.h"
#include "../../engine/SampleBank.h"
#include "../../dsp/LFO.h"
#include <juce_core/juce_core.h>

// -----------------------------------------------------------------------------
// How a factory preset names what its router does.
//
// A DEST, SRC or VIA cell stores a PLACE in a list of the engine's (ModRouter.h:
// mod::DestTable, mod::SourceTable), and so does an LFO's shape, trigger and
// division. A preset written with a number breaks silently the day a list grows;
// one written with a name follows the list. Everything here takes the name the
// display shows ("CUT", "BMRP", "1:C1", "OP3", "XONE", "1/8") and gives the place
// it has today, from the same tables the file format resolves names against
// (stored::tablesOf, StoredLists.h: each engine's own tables, built at first use).
//
// A NAME THAT DOES NOT RESOLVE IS NEVER LOADED AS "OFF" UNSEEN: it is written
// down in unresolved() - the ONE list of such names, stored::unknownNames() of
// Preset.h, which the resonator presets and kits write to as well - and asserts
// in a debug build. Every preset suite fails on a list that is not empty
// ("Format V3", "Fusion Presets", "Factory Multis"), and on every routing cell a
// factory preset sets to 0.
// A DEST, a SRC and a VIA begin with OFF, and a route that is to be off is not
// written: "OFF" by name is a name that did not resolve. A shape, a trigger, a
// division and a wave have an entry at 0 like any other.
//
// Three lists of destinations, three functions:
//   cellDest      what the DEST of an LFO, of the STRIP page and of the XIN page offers: the
//                 engine's visible entries, then the cells of the effects on the track
//                 ("1:C1" .. "8:C8", "1:WT" .. : mod::TrackDest)
//   slotDest      what a free slot's DEST offers: the visible entries, L1RT .., AMT1 .., then
//                 the track's effect cells
//   pressureDest  a pressure slot's DEST (PressureSlotParam): an index of the table itself,
//                 its hidden entries included (RSN's EXC, the macro's BOW)
//
// Ids: every engine but the unified voice has LFO1 / LFO2 at 158-173, STRIP at 174-181 and XIN
// at 182-189; the unified voice shows STRIP at 158-165 and XIN at 166-173 (its LFOs are the
// classic ones at 40-89). strip() and xin() know that; lfo() is for the other engines only.
// -----------------------------------------------------------------------------
namespace PresetRoute
{
    inline juce::StringArray& unresolved() { return stored::unknownNames(); }

    inline juce::String listOn(EngineType type, const char* list)
    {
        return juce::String(stored::engineLabel((int) type)) + " " + list;
    }
    inline float missed(EngineType type, const char* list, const char* label) { return stored::missedName(listOn(type, list), label); }

    // (The lists are stored::tablesOf's - StoredLists.h, where format 3 keeps every list a stored
    // position belongs to - and each is searched as the cell that holds the place reads it.)
    inline stored::Tables tables(EngineType type) { return stored::tablesOf((int) type); }

    inline float cellDest(EngineType type, const char* label)
    {
        const auto* t = tables(type).dests;
        return t != nullptr ? stored::placeOf(t->cellLabels(), t->cellChoices(), label, listOn(type, "DEST"), 1) : missed(type, "DEST", label);
    }

    inline float slotDest(EngineType type, const char* label)
    {
        const auto* t = tables(type).dests;
        return t != nullptr ? stored::placeOf(t->slotLabels(), t->slotChoices(), label, listOn(type, "slot DEST"), 1) : missed(type, "slot DEST", label);
    }

    // A pressure slot's DEST is a place in the pressure slots' own list (ModRouter.h, THE PRESSURE
    // SLOTS: the table, hidden entries included, then VIB, then the track's effect cells).
    inline float pressureDest(EngineType type, const char* label)
    {
        const auto* t = tables(type).dests;
        return t != nullptr ? stored::placeOf(t->pressureLabels(), t->pressureChoices(), label, listOn(type, "pressure DEST"), 1)
                            : missed(type, "pressure DEST", label);
    }

    inline float source(EngineType type, const char* label)
    {
        const auto* t = tables(type).sources;
        return t != nullptr ? stored::placeOf(t->labels(), t->size(), label, listOn(type, "SRC"), 1) : missed(type, "SRC", label);
    }

    // A list that is the same on every engine, its first entry an entry.
    inline float inList(const char* const* names, int count, const char* list, const char* label)
    {
        return stored::placeOf(names, count, label, list);
    }
    inline float shape(const char* label)    { return inList(LFO::shapeLabels(), (int) LFO::Shape::COUNT, "SHPE", label); }
    inline float trigger(const char* label)  { return inList(mod::Router::triggerLabels(), mod::Router::NUM_TRIGGERS, "TRIG", label); }
    inline float division(const char* label) { return inList(LFO::divisionLabels(), LFO::NUM_DIVISIONS, "DIV", label); }

    // ---- The cells, written into a preset's parameters --------------------------------------------

    // A free slot (SLT pages): SRC > DEST by AMT, through VIA when one is named.
    inline void slot(ParamMap& p, EngineType type, int n, const char* src, const char* dest, float amount, const char* via = nullptr)
    {
        p[ModSlotParam::id(n, ModSlotParam::SRC)] = source(type, src);
        p[ModSlotParam::id(n, ModSlotParam::DEST)] = slotDest(type, dest);
        p[ModSlotParam::id(n, ModSlotParam::AMT)] = amount;
        if (via != nullptr)
            p[ModSlotParam::id(n, ModSlotParam::VIA)] = source(type, via);
    }

    // LFO n (0 or 1) of an engine whose LFO pages are the router's (every engine but the unified voice).
    inline void lfo(ParamMap& p, EngineType type, int n, const char* dest, float depth, float rateHz,
                    const char* shapeName = "SIN", const char* triggerName = "FREE")
    {
        jassert(type != EngineType::UnifiedVoice);
        using R = mod::Router;
        p[R::lfoId(n, R::Rate)] = rateHz;
        p[R::lfoId(n, R::Depth)] = depth;
        p[R::lfoId(n, R::Shape)] = shape(shapeName);
        p[R::lfoId(n, R::Trigger)] = trigger(triggerName);
        p[R::lfoId(n, R::DestCell)] = cellDest(type, dest);
    }
    // ... its cycle a division of the tempo instead of RATE.
    inline void lfoSync(ParamMap& p, int n, const char* divisionName)
    {
        using R = mod::Router;
        p[R::lfoId(n, R::Sync)] = 1.0f;
        p[R::lfoId(n, R::Division)] = division(divisionName);
    }

    // A route of the STRIP page.
    inline void strip(ParamMap& p, EngineType type, int route, const char* dest, float amount)
    {
        const int base = type == EngineType::UnifiedVoice ? UnifiedVoiceEngine::STRIP_IDS : mod::Router::STRIP_BASE;
        p[base + route * 2] = cellDest(type, dest);
        p[base + route * 2 + 1] = amount;
    }

    // A route of the XIN page (the track input at audio rate): filter 0 RAW, 1 LP, 2 HP, 3 BP.
    inline void xin(ParamMap& p, EngineType type, int route, const char* dest, float amount, int filter = 0)
    {
        const int base = (type == EngineType::UnifiedVoice ? UnifiedVoiceEngine::XIN_IDS : mod::Router::XIN_BASE) + route * 4;
        p[base + mod::Router::XinDest1] = cellDest(type, dest);
        p[base + mod::Router::XinAmount1] = amount;
        p[base + mod::Router::XinFilter1] = (float) filter;
    }

    // A pressure slot (AT ROUTE): the pad's pressure, this voice's own, on DEST.
    inline void pressure(ParamMap& p, EngineType type, int n, const char* dest, float amount, bool bipolar = false)
    {
        p[PressureSlotParam::id(n, PressureSlotParam::DEST_OFFSET)] = pressureDest(type, dest);
        p[PressureSlotParam::id(n, PressureSlotParam::AMT_OFFSET)] = amount;
        p[PressureSlotParam::id(n, PressureSlotParam::BIPOLAR_OFFSET)] = bipolar ? 1.0f : 0.0f;
    }

    // ---- Insert effects stored with a sound (a preset carries its track's effects) -----------------
    // One insert slot: the effect type and its first four cells, switched on, all of it (WET 1).
    inline void insert(ParamMap& sound, int slotIndex, EffectType type, std::initializer_list<float> cells)
    {
        sound[ProjectBridge::INSERT_TYPE_BASE + slotIndex] = (float) ((int) type + 1);
        sound[AudioEngine::INSERT_ON_BASE + slotIndex] = 1.0f;
        sound[AudioEngine::INSERT_WET_BASE + slotIndex] = 1.0f;
        int n = 0;
        for (float value : cells)
            sound[AudioEngine::EFFECT_PARAM_BASE + slotIndex * 4 + n++] = value;
    }
    // ... its cells beyond four (TRK FX page FX2), from the first.
    inline void insertExtra(ParamMap& sound, int slotIndex, std::initializer_list<float> extras)
    {
        int n = 0;
        for (float value : extras)
            sound[AudioEngine::INSERT_EXTRA_BASE + slotIndex * EffectBase::MAX_EXTRAS + n++] = value;
    }
    // ... and how much of the slot's effect is heard.
    inline void insertWet(ParamMap& sound, int slotIndex, float wet) { sound[AudioEngine::INSERT_WET_BASE + slotIndex] = wet; }

    // ---- What a preset's routing cells say, for the suites ----------------------------------------
    // Every cell of `sound` that holds a place in a named list (mod::NamedList::classify) or a
    // pressure slot's DEST, with the name it resolves to today ("" = OFF or beyond the list).
    struct NamedCell { int id; int value; juce::String name; };
    inline std::vector<NamedCell> namedCells(EngineType type, const ParamMap& sound)
    {
        std::vector<NamedCell> out;
        for (auto& [id, value] : sound)
        {
            const int v = (int) std::lround(value);
            const auto list = stored::listOf((int) type, id);
            if (! list)
                continue;
            const auto kind = mod::NamedList::classify(id);
            const bool place = kind == mod::NamedList::Kind::CellDest || kind == mod::NamedList::Kind::SlotDest
                               || kind == mod::NamedList::Kind::PressureDest || kind == mod::NamedList::Kind::Source;
            const char* name = stored::nameOf((int) type, id, value);
            out.push_back({ id, v, name != nullptr && ! (place && v == 0) ? juce::String(name) : juce::String() });
        }
        return out;
    }

    // ---- A drum break by name (SampleBank: B100 and B170 are rendered from the drum voices at
    // start-up and are always there) -----------------------------------------------------------
    inline float breakIndex(const char* name)
    {
        const int i = SampleBank::get().indexOfBreak(name);
        if (i < 0)
            return missed(EngineType::Sampler, "break", name);
        return (float) i;
    }
}
