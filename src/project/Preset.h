#pragma once

#include "../engine/EngineBase.h"
#include "../dsp/ModMatrix.h"
#include <juce_core/juce_core.h>
#include <vector>

struct Preset
{
    juce::String name;
    juce::String category;
    EngineType engineType = EngineType::UnifiedVoice;
    ParamMap soundParams;
    std::vector<ModSlot> modSlots;
    bool isFactory = false;
    // Sampler / wavetable: the name of the sample the sound uses. soundParams holds its place in the
    // SampleBank as well, but places move when sample files are added: the name decides
    // (PresetManager::soundOf). Empty = the place alone (presets saved before this existed).
    juce::String sampleName;

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
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
            paramsArray.add(juce::var(p));
        }
        obj->setProperty("params", paramsArray);

        juce::Array<juce::var> modsArray;
        for (auto& slot : modSlots)
        {
            auto* m = new juce::DynamicObject();
            m->setProperty("source", static_cast<int>(slot.source));
            m->setProperty("dest", slot.destination);
            m->setProperty("depth", (double) slot.depth);
            m->setProperty("bipolar", slot.bipolar);
            modsArray.add(juce::var(m));
        }
        obj->setProperty("mods", modsArray);

        return juce::var(obj);
    }

    static Preset fromVar(const juce::var& v)
    {
        Preset p;
        if (auto* obj = v.getDynamicObject())
        {
            p.name = obj->getProperty("name").toString();
            p.category = obj->getProperty("category").toString();
            p.engineType = static_cast<EngineType>((int) obj->getProperty("engineType"));
            p.isFactory = (bool) obj->getProperty("isFactory");
            p.sampleName = obj->getProperty("sample").toString();

            if (auto* params = obj->getProperty("params").getArray())
            {
                for (auto& pv : *params)
                {
                    if (auto* po = pv.getDynamicObject())
                    {
                        int id = (int) po->getProperty("id");
                        float val = (float)(double) po->getProperty("value");
                        p.soundParams[id] = val;
                    }
                }
            }

            if (auto* mods = obj->getProperty("mods").getArray())
            {
                for (auto& mv : *mods)
                {
                    if (auto* mo = mv.getDynamicObject())
                    {
                        ModSlot slot;
                        slot.source = static_cast<ModSource>((int) mo->getProperty("source"));
                        slot.destination = (int) mo->getProperty("dest");
                        slot.depth = (float)(double) mo->getProperty("depth");
                        slot.bipolar = (bool) mo->getProperty("bipolar");
                        p.modSlots.push_back(slot);
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
