#pragma once

#include "../src/project/FactoryPresets.h"
#include "../src/project/PresetManager.h"
#include "../src/engine/DrumEngine.h"
#include <juce_core/juce_core.h>
#include <set>

class FactoryPresetCoverageTests : public juce::UnitTest
{
public:
    FactoryPresetCoverageTests() : UnitTest("Factory Preset Coverage") {}

    void runTest() override
    {
        // ============================================================
        // 1. Every engine type has at least 3 factory presets
        // ============================================================

        beginTest("Every engine type has at least 3 factory presets");
        {
            PresetManager pm;
            int count = pm.getPresetCount();

            // Count presets per engine type
            std::map<EngineType, int> engineCounts;
            engineCounts[EngineType::UnifiedVoice] = 0;
            engineCounts[EngineType::FM] = 0;
            engineCounts[EngineType::MacroOsc] = 0;
            engineCounts[EngineType::Wavetable] = 0;
            engineCounts[EngineType::WaveSeq] = 0;
            engineCounts[EngineType::Sampler] = 0;
            engineCounts[EngineType::Drum] = 0;

            for (int i = 0; i < count; ++i)
            {
                const auto& preset = pm.getPreset(i);
                if (preset.isFactory)
                    engineCounts[preset.engineType]++;
            }

            expect(engineCounts[EngineType::UnifiedVoice] >= 3,
                   "UnifiedVoice should have >= 3 presets, got "
                   + juce::String(engineCounts[EngineType::UnifiedVoice]));
            expect(engineCounts[EngineType::FM] >= 3,
                   "FM should have >= 3 presets, got "
                   + juce::String(engineCounts[EngineType::FM]));
            expect(engineCounts[EngineType::MacroOsc] >= 3,
                   "MacroOsc should have >= 3 presets, got "
                   + juce::String(engineCounts[EngineType::MacroOsc]));
            expect(engineCounts[EngineType::Wavetable] >= 3,
                   "Wavetable should have >= 3 presets, got "
                   + juce::String(engineCounts[EngineType::Wavetable]));
            expect(engineCounts[EngineType::WaveSeq] >= 3,
                   "WaveSeq should have >= 3 presets, got "
                   + juce::String(engineCounts[EngineType::WaveSeq]));
            expect(engineCounts[EngineType::Sampler] >= 3,
                   "Sampler should have >= 3 presets, got "
                   + juce::String(engineCounts[EngineType::Sampler]));
            expect(engineCounts[EngineType::Drum] >= 3,
                   "Drum should have >= 3 presets, got "
                   + juce::String(engineCounts[EngineType::Drum]));
        }

        // ============================================================
        // 2. All preset names are unique
        // ============================================================

        beginTest("All preset names are unique");
        {
            PresetManager pm;
            int count = pm.getPresetCount();
            std::set<juce::String> names;
            bool allUnique = true;
            juce::String duplicate;

            for (int i = 0; i < count; ++i)
            {
                const auto& name = pm.getPreset(i).name;
                if (names.count(name) > 0)
                {
                    allUnique = false;
                    duplicate = name;
                    break;
                }
                names.insert(name);
            }

            expect(allUnique, "Duplicate preset name found: " + duplicate);
        }

        // ============================================================
        // 3. All presets round-trip through serialization
        // ============================================================

        beginTest("All presets round-trip through serialization");
        {
            auto presets = FactoryPresets::getAll();
            for (const auto& original : presets)
            {
                juce::String json = original.toJSON();
                Preset restored = Preset::fromJSON(json);

                expect(restored.name == original.name,
                       "Name mismatch after round-trip: " + original.name);
                expect(restored.category == original.category,
                       "Category mismatch for: " + original.name);
                expect(restored.engineType == original.engineType,
                       "EngineType mismatch for: " + original.name);
                expect(restored.isFactory == original.isFactory,
                       "isFactory mismatch for: " + original.name);
                expect(restored.soundParams.size() == original.soundParams.size(),
                       "Param count mismatch for: " + original.name
                       + " (expected " + juce::String((int)original.soundParams.size())
                       + ", got " + juce::String((int)restored.soundParams.size()) + ")");

                // Verify each param value round-trips
                for (const auto& [id, value] : original.soundParams)
                {
                    auto it = restored.soundParams.find(id);
                    if (it == restored.soundParams.end())
                    {
                        expect(false, "Missing param " + juce::String(id)
                               + " in restored preset: " + original.name);
                    }
                    else
                    {
                        expectWithinAbsoluteError(it->second, value, 0.001f,
                            "Param " + juce::String(id) + " value mismatch for: "
                            + original.name);
                    }
                }
            }
        }

        // ============================================================
        // 4. No preset uses param IDs outside valid range for its engine
        // ============================================================

        beginTest("No preset uses param IDs outside valid range");
        {
            auto presets = FactoryPresets::getAll();
            for (const auto& preset : presets)
            {
                int maxValidId = getMaxParamId(preset.engineType);

                for (const auto& [id, value] : preset.soundParams)
                {
                    expect(id >= 0,
                           "Negative param ID " + juce::String(id)
                           + " in preset: " + preset.name);
                    // (A drum kit may also set its pads' kick-machine and cymbal-machine pages.)
                    expect(id <= maxValidId
                           || (preset.engineType == EngineType::Drum && (DrumEngine::isKickParam(id) || DrumEngine::isCymbalParam(id))),
                           "Param ID " + juce::String(id) + " exceeds max ("
                           + juce::String(maxValidId) + ") for "
                           + engineTypeName(preset.engineType)
                           + " in preset: " + preset.name);
                }
            }
        }

        // ============================================================
        // 5. Total preset count is >= 50
        // ============================================================

        beginTest("Total preset count is >= 50");
        {
            PresetManager pm;
            int count = pm.getPresetCount();
            expect(count >= 50,
                   "Expected >= 50 presets, got " + juce::String(count));
        }

        // ============================================================
        // 6. Kit presets have correct engine type
        // ============================================================

        beginTest("Kit presets have correct engine type");
        {
            PresetManager pm;
            int count = pm.getPresetCount();

            for (int i = 0; i < count; ++i)
            {
                const auto& preset = pm.getPreset(i);
                if (preset.category == "KIT")
                {
                    expect(preset.engineType == EngineType::Drum,
                           "Kit preset '" + preset.name
                           + "' should use Drum engine, but uses "
                           + engineTypeName(preset.engineType));
                }
            }
        }

        // ============================================================
        // 7. FactoryPresets::getAll() returns at least 45 presets
        // ============================================================

        beginTest("FactoryPresets::getAll() returns >= 45 presets");
        {
            auto presets = FactoryPresets::getAll();
            expect((int)presets.size() >= 45,
                   "Expected >= 45 presets from getAll(), got "
                   + juce::String((int)presets.size()));
        }

        // ============================================================
        // 8. All factory presets are marked as factory
        // ============================================================

        beginTest("All factory presets are marked isFactory");
        {
            auto presets = FactoryPresets::getAll();
            for (const auto& p : presets)
            {
                expect(p.isFactory,
                       "Preset '" + p.name + "' should be marked isFactory");
            }
        }

        // ============================================================
        // 9. No trademark references in preset names
        // ============================================================

        beginTest("No trademark references in preset names");
        {
            // Brands that must not appear per project guardrails
            juce::StringArray banned = {
                "Elektron", "Korg", "Moog", "Sequential", "Oberheim",
                "Arturia", "Waldorf", "PPG", "Roland", "Yamaha", "DX7",
                "Jupiter", "Juno", "Prophet", "Minimoog"
            };

            auto presets = FactoryPresets::getAll();
            for (const auto& p : presets)
            {
                for (const auto& brand : banned)
                {
                    expect(!p.name.containsIgnoreCase(brand),
                           "Preset name '" + p.name
                           + "' contains banned trademark: " + brand);
                }
            }
        }
    }

private:
    // Maximum valid param ID for each engine type
    static int getMaxParamId(EngineType type)
    {
        switch (type)
        {
            case EngineType::UnifiedVoice: return UnifiedParam::MAX_ID - 1;
            // These four have the per-voice filter (FILTR / F ENV pages, ids 100-115), where a
            // preset keeps its cutoff and its mod strip amount (WHL), ...
            case EngineType::FM:
            case EngineType::MacroOsc:
            case EngineType::Wavetable:
            case EngineType::WaveSeq:      return EngineLfoSection::STRIP_BASE - 1;   // ... and with the two LFOs
                                                                                      // (158-173), not the STRIP page
            case EngineType::Sampler:      return EngineLfoSection::STRIP_BASE - 1;   // its cells (0-55) and its two LFOs
            case EngineType::Drum:         return 127; // 15 voices * 8 + 7
            case EngineType::MIDITrack:    return 29;
            default:                       return 29;
        }
    }

    static juce::String engineTypeName(EngineType type)
    {
        switch (type)
        {
            case EngineType::UnifiedVoice: return "UnifiedVoice";
            case EngineType::FM:           return "FM";
            case EngineType::MacroOsc:     return "MacroOsc";
            case EngineType::Wavetable:    return "Wavetable";
            case EngineType::WaveSeq:      return "WaveSeq";
            case EngineType::Sampler:      return "Sampler";
            case EngineType::Drum:         return "Drum";
            case EngineType::MIDITrack:    return "MIDITrack";
            default:                       return "Unknown";
        }
    }
};

static FactoryPresetCoverageTests factoryPresetCoverageTests;
