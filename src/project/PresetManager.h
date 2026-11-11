#pragma once

#include "Preset.h"
#include "FactoryPresets.h"
#include "../sequencer/Track.h"
#include <juce_core/juce_core.h>
#include <vector>
#include <algorithm>

class PresetManager
{
public:
    PresetManager()
    {
        loadFactoryPresets();
        FactoryPresets::registerAll(*this);
    }

    int getPresetCount() const { return (int) presets.size(); }

    const Preset& getPreset(int index) const { return presets[(size_t) index]; }

    void addPreset(const Preset& p)
    {
        presets.push_back(p);
    }

    void removePreset(int index)
    {
        if (index >= 0 && index < (int) presets.size() && !presets[(size_t) index].isFactory)
            presets.erase(presets.begin() + index);
    }

    std::vector<int> getFilteredIndices(EngineType type) const
    {
        std::vector<int> result;
        for (int i = 0; i < (int) presets.size(); ++i)
            if (presets[(size_t) i].engineType == type)
                result.push_back(i);
        return result;
    }

    std::vector<int> getFilteredIndices(EngineType type, const juce::String& category) const
    {
        if (category.isEmpty() || category == "ALL")
            return getFilteredIndices(type);

        std::vector<int> result;
        for (int i = 0; i < (int) presets.size(); ++i)
            if (presets[(size_t) i].engineType == type && presets[(size_t) i].category == category)
                result.push_back(i);
        return result;
    }

    static Preset fromTrack(const Track& track, const juce::String& name,
                            const juce::String& category)
    {
        Preset p;
        p.name = name;
        p.category = category;
        p.engineType = track.engineType;
        p.soundParams = track.soundParams;
        p.isFactory = false;
        return p;
    }

    static void applyToTrack(const Preset& preset, Track& track)
    {
        track.engineType = preset.engineType;
        track.soundParams = preset.soundParams;
    }

    void applyToEngine(const Preset& preset, EngineBase* engine) const
    {
        if (!engine) return;
        for (auto& [id, value] : preset.soundParams)
            engine->setParameter(id, value);
    }

    bool saveToFile(const Preset& preset, const juce::File& file) const
    {
        return file.replaceWithText(preset.toJSON());
    }

    bool loadFromFile(const juce::File& file)
    {
        if (!file.existsAsFile()) return false;
        auto json = file.loadFileAsString();
        auto preset = Preset::fromJSON(json);
        if (preset.name.isEmpty()) return false;
        presets.push_back(preset);
        return true;
    }

    void loadFromDirectory(const juce::File& dir)
    {
        if (!dir.isDirectory()) return;
        for (auto& file : dir.findChildFiles(juce::File::findFiles, false, "*.json"))
            loadFromFile(file);
    }

    void saveUserPresetsToDirectory(const juce::File& dir) const
    {
        dir.createDirectory();
        for (auto& p : presets)
        {
            if (p.isFactory) continue;
            auto file = dir.getChildFile(sanitizeFilename(p.name) + ".json");
            file.replaceWithText(p.toJSON());
        }
    }

    static juce::StringArray getCategories()
    {
        return {"ALL", "INIT", "BASS", "PAD", "LEAD", "KEYS", "FX", "PERC", "KIT"};
    }

private:
    std::vector<Preset> presets;

    static juce::String sanitizeFilename(const juce::String& name)
    {
        return name.replaceCharacters(" /\\:*?\"<>|", "__________")
                   .substring(0, 64);
    }

    void loadFactoryPresets()
    {
        // UnifiedVoice presets
        addFactory("Init VA", "INIT", EngineType::UnifiedVoice, {
            {0, 0.5f}, {1, 0.5f}, {2, 0.5f}, {3, 0.0f},
            {10, 20000.0f}, {11, 0.0f}, {20, 0.01f}, {21, 0.1f},
            {22, 0.7f}, {23, 0.3f}, {24, 0.8f}
        });

        addFactory("Deep Bass", "BASS", EngineType::UnifiedVoice, {
            {0, 0.8f}, {1, 0.3f}, {2, 0.7f}, {3, 0.6f},
            {10, 800.0f}, {11, 0.4f}, {12, 1.0f}, {16, 0.3f},
            {20, 0.005f}, {21, 0.4f}, {22, 0.3f}, {23, 0.2f}, {24, 0.9f}
        });

        addFactory("Warm Pad", "PAD", EngineType::UnifiedVoice, {
            {0, 0.5f}, {1, 0.5f}, {2, 0.5f}, {5, 0.08f},
            {10, 3000.0f}, {11, 0.1f},
            {20, 1.5f}, {21, 2.0f}, {22, 0.6f}, {23, 3.0f}, {24, 0.6f}
        });

        addFactory("Bright Lead", "LEAD", EngineType::UnifiedVoice, {
            {0, 0.7f}, {1, 0.0f}, {2, 0.8f},
            {10, 8000.0f}, {11, 0.3f}, {14, 0.5f},
            {20, 0.01f}, {21, 0.2f}, {22, 0.8f}, {23, 0.4f}, {24, 0.8f},
            {7, 0.05f}
        });

        // FM presets
        addFactory("Init FM", "INIT", EngineType::FM, {
            {1, 1.0f}, {11, 0.0f},
            {60, 0.0f}, {61, 0.8f}
        });

        addFactory("E.Piano", "KEYS", EngineType::FM, {
            {0, 1.0f}, {1, 0.8f}, {4, 0.01f}, {5, 0.8f}, {6, 0.0f}, {7, 0.5f},
            {10, 14.0f}, {11, 0.3f}, {14, 0.005f}, {15, 0.3f}, {16, 0.0f}, {17, 0.2f},
            {60, 0.0f}, {61, 0.7f}
        });

        addFactory("FM Bell", "KEYS", EngineType::FM, {
            {0, 1.0f}, {1, 0.6f}, {4, 0.001f}, {5, 2.0f}, {6, 0.0f}, {7, 3.0f},
            {10, 3.5f}, {11, 0.5f}, {14, 0.001f}, {15, 1.5f}, {16, 0.0f}, {17, 2.0f},
            {60, 0.0f}, {61, 0.6f}
        });

        addFactory("FM Bass", "BASS", EngineType::FM, {
            {0, 1.0f}, {1, 1.0f}, {4, 0.005f}, {5, 0.3f}, {6, 0.4f}, {7, 0.2f},
            {10, 2.0f}, {11, 0.6f}, {14, 0.005f}, {15, 0.15f}, {16, 0.1f}, {17, 0.1f},
            {60, 0.0f}, {61, 0.9f}
        });

        // Drum presets
        addFactory("808 Kit", "KIT", EngineType::Drum, {});
        addFactory("Perc Kit", "KIT", EngineType::Drum, {});

        // MacroOsc presets
        addFactory("Init MO", "INIT", EngineType::MacroOsc, {});

        // Wavetable presets
        addFactory("Init WT", "INIT", EngineType::Wavetable, {});

        // WaveSeq presets
        addFactory("Init WS", "INIT", EngineType::WaveSeq, {});
    }

    void addFactory(const juce::String& name, const juce::String& category,
                    EngineType type, ParamMap params)
    {
        Preset p;
        p.name = name;
        p.category = category;
        p.engineType = type;
        p.soundParams = std::move(params);
        p.isFactory = true;
        presets.push_back(std::move(p));
    }
};
