#pragma once

#include "../engine/EngineBase.h"
#include "../engine/ModRouter.h"
#include "../controller/KeyboardSplit.h"
#include <juce_core/juce_core.h>
#include <cstring>
#include <vector>

// The cells of a sound whose value is a position in a list (a DEST, a SRC, a shape, a TYPE ...)
// are stored by the entry's name: StoredLists.h, which needs every engine and is therefore not
// included here (the drum engine's own headers include this file). Whoever uses a preset's or a
// project's JSON includes Project.h or FactoryPresets.h, and they bring it.
namespace stored
{
    inline const char* nameOf(int engineType, int id, float value);
    inline float loaded(int engineType, int id, float number, const juce::var& name, juce::StringArray& lost);
    inline void report(const juce::String& file, const juce::StringArray& lost);
    // A sound of before format 3 made today's (old::sound), and afterwards the locks of its steps
    // (old::locks): `layout` is what the sound was read by.
    inline int soundOfBefore(int engineType, ParamMap& sound, juce::StringArray& lost);
    inline void locksOfBefore(int engineType, int layout, ParamMap& locks, juce::StringArray& lost);

    // A preset written in code names what a route aims at and reads (PresetRoute, presets/
    // PresetRoutes.h; the resonator presets and kits, which are below that header, placeOf here).
    // A name that does not exist - a mistyped label - is put in THIS ONE LIST by whoever looked it
    // up, and every preset suite fails while it is not empty ("Format V3", "Fusion Presets",
    // "Factory Multis"): such a route must never load as OFF and say nothing.
    inline juce::StringArray& unknownNames()
    {
        static juce::StringArray names;
        return names;
    }
    // A name that was not found: remembered (each once), and 0.
    inline float missedName(const juce::String& inList, const char* name)
    {
        unknownNames().addIfNotAlreadyThere(inList + " \"" + juce::String(name != nullptr ? name : "(null)") + "\"");
        jassertfalse;       // a preset written in code names an entry that is not in the list
        return 0.0f;
    }
    // The place of `name` in a list of `count` labels; an unknown one is remembered and is 0.
    // first: the first place that counts. 0 for a list whose first entry is an entry like the
    // others (a shape, a trigger, a division, a wave); 1 for a list that begins with OFF (every
    // DEST, SRC and VIA): a route that is to be off is not written at all, so "OFF" by name is a
    // name that was not found.
    inline float placeOf(const char* const* labels, int count, const char* name, const juce::String& inList, int first = 0)
    {
        if (labels != nullptr && name != nullptr)
            for (int i = first; i < count; ++i)
                if (labels[i] != nullptr && std::strcmp(labels[i], name) == 0)
                    return (float) i;
        return missedName(inList, name);
    }
}

struct Preset
{
    juce::String name;
    juce::String category;
    EngineType engineType = EngineType::UnifiedVoice;
    ParamMap soundParams;
    // The "mods" entries of preset files written before the mod slots existed (for a matrix that
    // nothing ever played): read and written back as they are, used by nothing. A sound's mod
    // slots are parameters (ModSlotParam, ids 700-772) in soundParams.
    struct LegacyMod
    {
        int source = 0;
        int destination = 0;
        float depth = 0.0f;
        bool bipolar = true;
    };
    std::vector<LegacyMod> legacyMods;
    bool isFactory = false;
    // Sampler / wavetable: the name of the sample the sound uses. soundParams holds its place in the
    // SampleBank as well, but places move when sample files are added: the name decides
    // (PresetManager::soundOf). Empty = the place alone (presets saved before this existed).
    juce::String sampleName;
    // The track's arpeggiator as it was when the preset was saved (ProjectBridge::captureArp: its
    // settings, on / off, the tapped rhythm). Empty = the preset has none and leaves the track's.
    std::vector<float> arp;
    // The keyboard when the preset was saved: its split and its octave. A part that is not stored
    // (a preset from before, a factory preset) leaves the keyboard as it is.
    KeyboardSetup keyboard;

    // Not stored - what reading the file found:
    // names it holds that their lists no longer have (each said once; already in the app's log), and
    // for a file of before format 3 the layout it was read by (MultiPreset: its steps' locks).
    juce::StringArray lost;
    int fileVersion = FORMAT_VERSION;
    int oldLayout = 0;

    // 3: a cell that is a position in a list carries the entry's name ("n") beside its number,
    // and the name decides on load (StoredLists.h). A file without a version is of before: its
    // positions are translated from the layout its ids tell.
    static constexpr int FORMAT_VERSION = 3;

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("version", FORMAT_VERSION);
        obj->setProperty("name", name);
        obj->setProperty("category", category);
        obj->setProperty("engineType", static_cast<int>(engineType));
        obj->setProperty("isFactory", isFactory);
        if (sampleName.isNotEmpty())
            obj->setProperty("sample", sampleName);

        juce::Array<juce::var> paramsArray;
        for (auto& [id, value] : soundParams)
        {
            auto* p = new juce::DynamicObject();
            p->setProperty("id", id);
            p->setProperty("value", (double) value);
            if (const char* entry = stored::nameOf((int) engineType, id, value))
                p->setProperty("n", juce::String(entry));
            paramsArray.add(juce::var(p));
        }
        obj->setProperty("params", paramsArray);

        juce::Array<juce::var> modsArray;
        for (auto& slot : legacyMods)
        {
            auto* m = new juce::DynamicObject();
            m->setProperty("source", slot.source);
            m->setProperty("dest", slot.destination);
            m->setProperty("depth", (double) slot.depth);
            m->setProperty("bipolar", slot.bipolar);
            modsArray.add(juce::var(m));
        }
        obj->setProperty("mods", modsArray);

        if (!arp.empty())
        {
            juce::Array<juce::var> arpArray;
            for (float v : arp)
                arpArray.add((double) v);
            obj->setProperty("arp", arpArray);
        }

        if (keyboard.split >= 0)
            obj->setProperty("split", keyboard.split);
        if (keyboard.octave != KeyboardSetup::UNKNOWN)
            obj->setProperty("octave", keyboard.octave);

        return juce::var(obj);
    }

    // quiet: the caller reports what was lost itself (a multi preset: once for all its members).
    static Preset fromVar(const juce::var& v, bool quiet = false)
    {
        Preset p;
        if (auto* obj = v.getDynamicObject())
        {
            p.fileVersion = juce::jmax(1, (int) obj->getProperty("version"));
            p.name = obj->getProperty("name").toString();
            p.category = obj->getProperty("category").toString();
            p.engineType = static_cast<EngineType>((int) obj->getProperty("engineType"));
            p.isFactory = (bool) obj->getProperty("isFactory");
            p.sampleName = obj->getProperty("sample").toString();

            const int engineTypeInt = (int) p.engineType;
            if (auto* params = obj->getProperty("params").getArray())
            {
                for (auto& pv : *params)
                {
                    if (auto* po = pv.getDynamicObject())
                    {
                        int id = (int) po->getProperty("id");
                        float val = (float)(double) po->getProperty("value");
                        if (p.fileVersion >= FORMAT_VERSION)
                            val = stored::loaded(engineTypeInt, id, val, po->getProperty("n"), p.lost);
                        p.soundParams[id] = val;
                    }
                }
            }
            if (p.fileVersion < FORMAT_VERSION)
                p.oldLayout = stored::soundOfBefore(engineTypeInt, p.soundParams, p.lost);
            if (! quiet)
                stored::report("preset " + p.name, p.lost);

            if (auto* arpArray = obj->getProperty("arp").getArray())
                for (auto& av : *arpArray)
                    p.arp.push_back((float) (double) av);

            if (auto splitVar = obj->getProperty("split"); !splitVar.isVoid())
                p.keyboard.split = juce::jlimit(0, 3, (int) splitVar);
            if (auto octaveVar = obj->getProperty("octave"); !octaveVar.isVoid())
                p.keyboard.octave = juce::jlimit(-8, 8, (int) octaveVar);

            if (auto* mods = obj->getProperty("mods").getArray())
            {
                for (auto& mv : *mods)
                {
                    if (auto* mo = mv.getDynamicObject())
                    {
                        LegacyMod slot;
                        slot.source = (int) mo->getProperty("source");
                        slot.destination = (int) mo->getProperty("dest");
                        slot.depth = (float)(double) mo->getProperty("depth");
                        slot.bipolar = (bool) mo->getProperty("bipolar");
                        p.legacyMods.push_back(slot);
                    }
                }
            }
        }
        return p;
    }

    juce::String toJSON() const
    {
        return juce::JSON::toString(toVar());
    }

    static Preset fromJSON(const juce::String& json)
    {
        auto parsed = juce::JSON::parse(json);
        return fromVar(parsed);
    }
};
