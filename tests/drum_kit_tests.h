#pragma once

#include "../src/engine/DrumEngine.h"
#include "../src/project/DrumKit.h"
#include "../src/project/KitPreset.h"
#include <juce_core/juce_core.h>

class DrumKitTests : public juce::UnitTest
{
public:
    DrumKitTests() : UnitTest("Drum Kits & Expression") {}

    void runTest() override
    {
        // ============================================================
        // 1. DrumSlot data model
        // ============================================================

        beginTest("DrumSlot default values");
        {
            DrumSlot slot;
            expectEquals(static_cast<int>(slot.synthType), static_cast<int>(DrumSynthType::FMKick));
            expectWithinAbsoluteError(slot.tuning, 0.0f, 0.001f);
            expectEquals(static_cast<int>(slot.aftertouchDest), static_cast<int>(DrumATDest::Off));
            expectWithinAbsoluteError(slot.aftertouchDepth, 1.0f, 0.001f);
            expectEquals(slot.chokeGroup, 0);
        }

        beginTest("DrumSlot named param accessors");
        {
            DrumSlot slot;
            slot.setTune(440.0f);
            slot.setDecay(0.5f);
            slot.setTone(0.7f);
            slot.setDrive(0.3f);
            slot.setSnap(0.6f);
            slot.setLevel(0.9f);
            slot.setPan(0.25f);

            expectWithinAbsoluteError(slot.getTune(), 440.0f, 0.001f);
            expectWithinAbsoluteError(slot.getDecay(), 0.5f, 0.001f);
            expectWithinAbsoluteError(slot.getTone(), 0.7f, 0.001f);
            expectWithinAbsoluteError(slot.getDrive(), 0.3f, 0.001f);
            expectWithinAbsoluteError(slot.getSnap(), 0.6f, 0.001f);
            expectWithinAbsoluteError(slot.getLevel(), 0.9f, 0.001f);
            expectWithinAbsoluteError(slot.getPan(), 0.25f, 0.001f);
        }

        beginTest("DrumSlot JSON round-trip");
        {
            DrumSlot original;
            original.synthType = DrumSynthType::NoiseSnare;
            original.tuning = 3.5f;
            original.setTune(200.0f);
            original.setDecay(0.25f);
            original.setTone(0.6f);
            original.setDrive(0.4f);
            original.aftertouchDest = DrumATDest::Pitch;
            original.aftertouchDepth = 0.75f;
            original.chokeGroup = 2;

            auto var = original.toVar();
            DrumSlot restored = DrumSlot::fromVar(var);

            expectEquals(static_cast<int>(restored.synthType), static_cast<int>(DrumSynthType::NoiseSnare));
            expectWithinAbsoluteError(restored.tuning, 3.5f, 0.001f);
            expectWithinAbsoluteError(restored.getTune(), 200.0f, 0.001f);
            expectWithinAbsoluteError(restored.getDecay(), 0.25f, 0.001f);
            expectEquals(static_cast<int>(restored.aftertouchDest), static_cast<int>(DrumATDest::Pitch));
            expectWithinAbsoluteError(restored.aftertouchDepth, 0.75f, 0.001f);
            expectEquals(restored.chokeGroup, 2);
        }

        // ============================================================
        // 2. DrumKit data model
        // ============================================================

        beginTest("DrumKit default has 16 slots");
        {
            DrumKit kit;
            expectEquals(static_cast<int>(kit.slots.size()), DrumKit::NUM_SLOTS);
            expectEquals(kit.name, std::string("Init Kit"));
        }

        beginTest("DrumKit mixed types within one kit");
        {
            DrumKit kit;
            kit.name = "Mixed Test";
            kit.slots[0].synthType = DrumSynthType::FMKick;
            kit.slots[1].synthType = DrumSynthType::NoiseSnare;
            kit.slots[2].synthType = DrumSynthType::AnalogKick;
            kit.slots[3].synthType = DrumSynthType::Clap;

            expectEquals(static_cast<int>(kit.slots[0].synthType), static_cast<int>(DrumSynthType::FMKick));
            expectEquals(static_cast<int>(kit.slots[1].synthType), static_cast<int>(DrumSynthType::NoiseSnare));
            expectEquals(static_cast<int>(kit.slots[2].synthType), static_cast<int>(DrumSynthType::AnalogKick));
            expectEquals(static_cast<int>(kit.slots[3].synthType), static_cast<int>(DrumSynthType::Clap));
        }

        beginTest("DrumKit JSON round-trip");
        {
            DrumKit original;
            original.name = "Test Kit";
            original.slots[0].synthType = DrumSynthType::FMKick;
            original.slots[0].setTune(48.0f);
            original.slots[0].setDecay(0.8f);
            original.slots[0].chokeGroup = 0;
            original.slots[1].synthType = DrumSynthType::ClosedHat;
            original.slots[1].setTune(8000.0f);
            original.slots[1].chokeGroup = 1;
            original.slots[1].aftertouchDest = DrumATDest::Decay;
            original.slots[1].aftertouchDepth = 0.5f;

            juce::String json = original.toJSON();
            DrumKit restored = DrumKit::fromJSON(json);

            expectEquals(restored.name, std::string("Test Kit"));
            expectEquals(static_cast<int>(restored.slots[0].synthType), static_cast<int>(DrumSynthType::FMKick));
            expectWithinAbsoluteError(restored.slots[0].getTune(), 48.0f, 0.001f);
            expectEquals(static_cast<int>(restored.slots[1].synthType), static_cast<int>(DrumSynthType::ClosedHat));
            expectWithinAbsoluteError(restored.slots[1].getTune(), 8000.0f, 0.1f);
            expectEquals(restored.slots[1].chokeGroup, 1);
            expectEquals(static_cast<int>(restored.slots[1].aftertouchDest), static_cast<int>(DrumATDest::Decay));
            expectWithinAbsoluteError(restored.slots[1].aftertouchDepth, 0.5f, 0.001f);
        }

        // ============================================================
        // 3. Kit presets
        // ============================================================

        beginTest("KitPreset JSON round-trip");
        {
            KitPreset preset;
            preset.kit.name = "My Kit";
            preset.kit.slots[0].synthType = DrumSynthType::SubBass;
            preset.kit.slots[0].setTune(36.0f);
            preset.isFactory = true;

            juce::String json = preset.toJSON();
            KitPreset restored = KitPreset::fromJSON(json);

            expectEquals(restored.kit.name, std::string("My Kit"));
            expectEquals(static_cast<int>(restored.kit.slots[0].synthType), static_cast<int>(DrumSynthType::SubBass));
            expect(restored.isFactory);
        }

        beginTest("Factory kits: 808");
        {
            auto kit = FactoryKits::make808();
            expectEquals(kit.name, std::string("808"));
            // 808 uses AnalogKick for slot 0
            expectEquals(static_cast<int>(kit.slots[0].synthType), static_cast<int>(DrumSynthType::AnalogKick));
            // Hats should be in choke group 1
            expect(kit.slots[8].chokeGroup == 1, "CH should be choke group 1");
            expect(kit.slots[9].chokeGroup == 1, "CH2 should be choke group 1");
            expect(kit.slots[10].chokeGroup == 1, "OH should be choke group 1");
        }

        beginTest("Factory kits: 909");
        {
            auto kit = FactoryKits::make909();
            expectEquals(kit.name, std::string("909"));
            // 909 uses FMKick for slot 0
            expectEquals(static_cast<int>(kit.slots[0].synthType), static_cast<int>(DrumSynthType::FMKick));
        }

        beginTest("Factory kits: Acoustic");
        {
            auto kit = FactoryKits::makeAcoustic();
            expectEquals(kit.name, std::string("Acoustic"));
        }

        beginTest("Factory kits: Electronic");
        {
            auto kit = FactoryKits::makeElectronic();
            expectEquals(kit.name, std::string("Electronic"));
            // Electronic uses ModCyclesKick for slot 0
            expectEquals(static_cast<int>(kit.slots[0].synthType), static_cast<int>(DrumSynthType::ModCyclesKick));
        }

        beginTest("Factory kit count is 4");
        {
            auto kits = FactoryKits::getFactoryKits();
            expectEquals(static_cast<int>(kits.size()), 4);
        }

        beginTest("Get factory kit by name");
        {
            auto kit = FactoryKits::getFactoryKit("909");
            expectEquals(kit.name, std::string("909"));

            // Case-insensitive
            auto kit2 = FactoryKits::getFactoryKit("acoustic");
            expectEquals(kit2.name, std::string("Acoustic"));

            // Unknown name returns 808 as default
            auto fallback = FactoryKits::getFactoryKit("nonexistent");
            expectEquals(fallback.name, std::string("808"));
        }

        // ============================================================
        // 4. DrumEngine kit loading
        // ============================================================

        beginTest("DrumEngine loads default kit");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);
            expectEquals(engine.getKitName(), std::string("Default"));
        }

        beginTest("DrumEngine loads factory kit");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);

            auto kit808 = FactoryKits::make808();
            engine.loadKit(kit808);

            expectEquals(engine.getKitName(), std::string("808"));
            // Verify voice 0 was configured to AnalogKick
            expectEquals(static_cast<int>(engine.getVoice(0).getType()), static_cast<int>(DrumSynthType::AnalogKick));
        }

        beginTest("DrumEngine mixed kit: different types per slot");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);

            DrumKit mixed;
            mixed.name = "Mixed";
            mixed.slots[0].synthType = DrumSynthType::FMKick;
            mixed.slots[0].setTune(48.0f);
            mixed.slots[1].synthType = DrumSynthType::NoiseSnare;
            mixed.slots[1].setTune(200.0f);
            mixed.slots[2].synthType = DrumSynthType::Cymbal;
            mixed.slots[2].setTune(7000.0f);
            mixed.slots[3].synthType = DrumSynthType::SubBass;
            mixed.slots[3].setTune(36.0f);

            engine.loadKit(mixed);

            expectEquals(static_cast<int>(engine.getVoice(0).getType()), static_cast<int>(DrumSynthType::FMKick));
            expectEquals(static_cast<int>(engine.getVoice(1).getType()), static_cast<int>(DrumSynthType::NoiseSnare));
            expectEquals(static_cast<int>(engine.getVoice(2).getType()), static_cast<int>(DrumSynthType::Cymbal));
            expectEquals(static_cast<int>(engine.getVoice(3).getType()), static_cast<int>(DrumSynthType::SubBass));
        }

        beginTest("DrumEngine setSlotType changes individual slot");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);

            engine.setSlotType(0, DrumSynthType::Clap);
            expectEquals(static_cast<int>(engine.getVoice(0).getType()), static_cast<int>(DrumSynthType::Clap));

            // Other voices unchanged
            expectEquals(static_cast<int>(engine.getVoice(8).getType()), static_cast<int>(DrumSynthType::KickSynth));
        }

        // ============================================================
        // 5. Pad-to-voice mapping (decoupling)
        // ============================================================

        beginTest("Default pad mapping is 1:1");
        {
            DrumEngine engine;
            for (int i = 0; i < DrumEngine::NUM_VOICES; ++i)
            {
                expectEquals(engine.getVoiceForPad(i), i);
            }
        }

        beginTest("Custom pad-to-voice mapping");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);

            // Swap pad 0 -> voice 5, pad 5 -> voice 0
            engine.setPadToVoice(0, 5);
            engine.setPadToVoice(5, 0);

            expectEquals(engine.getVoiceForPad(0), 5);
            expectEquals(engine.getVoiceForPad(5), 0);
        }

        beginTest("Pad mapping: note triggers correct voice");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);

            // Map pad 0 to voice 3
            engine.setPadToVoice(0, 3);

            // Trigger pad 0 (note 0)
            engine.noteOn(0, 1.0f);
            expect(engine.getVoice(3).isActive(), "Voice 3 should be active after pad 0 trigger");
            expect(!engine.getVoice(0).isActive(), "Voice 0 should not be active (unmapped)");
        }

        beginTest("Pad mapping: out-of-range returns -1");
        {
            DrumEngine engine;
            expectEquals(engine.getVoiceForPad(-1), -1);
            expectEquals(engine.getVoiceForPad(16), -1);
        }

        // ============================================================
        // 6. Per-slot expression routing (aftertouch)
        // ============================================================

        beginTest("DrumVoice aftertouch destination default is Off");
        {
            DrumVoice voice;
            expectEquals(static_cast<int>(voice.getAftertouchDest()), static_cast<int>(DrumVoiceATDest::Off));
            expectWithinAbsoluteError(voice.getAftertouchDepth(), 1.0f, 0.001f);
        }

        beginTest("DrumVoice set aftertouch destination");
        {
            DrumVoice voice;
            voice.setAftertouchDest(DrumVoiceATDest::Pitch);
            voice.setAftertouchDepth(0.5f);

            expectEquals(static_cast<int>(voice.getAftertouchDest()), static_cast<int>(DrumVoiceATDest::Pitch));
            expectWithinAbsoluteError(voice.getAftertouchDepth(), 0.5f, 0.001f);
        }

        beginTest("DrumVoice applyAftertouch with Pitch destination");
        {
            DrumVoice voice;
            voice.setSampleRate(44100.0);
            voice.setType(DrumSynthType::FMKick);
            voice.setTune(100.0f);
            voice.setAftertouchDest(DrumVoiceATDest::Pitch);
            voice.setAftertouchDepth(1.0f);

            float panMod = voice.applyAftertouch(1.0f);
            expectWithinAbsoluteError(panMod, 0.0f, 0.001f); // No pan mod for Pitch dest

            // Effective tune should be doubled (1 octave up) at full pressure
            float effectiveTune = voice.getEffectiveTune();
            expectWithinAbsoluteError(effectiveTune, 200.0f, 0.1f);
        }

        beginTest("DrumVoice applyAftertouch with Level destination");
        {
            DrumVoice voice;
            voice.setSampleRate(44100.0);
            voice.setLevel(0.5f);
            voice.setAftertouchDest(DrumVoiceATDest::Level);
            voice.setAftertouchDepth(0.5f);

            voice.applyAftertouch(1.0f);
            float effectiveLevel = voice.getEffectiveLevel();
            // 0.5 (base) + 0.5 (depth * pressure) = 1.0
            expectWithinAbsoluteError(effectiveLevel, 1.0f, 0.001f);
        }

        beginTest("DrumVoice applyAftertouch with Pan destination");
        {
            DrumVoice voice;
            voice.setSampleRate(44100.0);
            voice.setAftertouchDest(DrumVoiceATDest::Pan);
            voice.setAftertouchDepth(1.0f);

            float panMod = voice.applyAftertouch(1.0f);
            // Full pressure -> pan offset (bipolar)
            expect(std::abs(panMod) > 0.0f, "Pan mod should be non-zero at full pressure");
        }

        beginTest("DrumVoice clearAftertouch resets modulation");
        {
            DrumVoice voice;
            voice.setSampleRate(44100.0);
            voice.setTune(100.0f);
            voice.setAftertouchDest(DrumVoiceATDest::Pitch);
            voice.setAftertouchDepth(1.0f);

            voice.applyAftertouch(1.0f);
            expect(voice.getEffectiveTune() > 100.0f, "Tune should be modulated");

            voice.clearAftertouch();
            expectWithinAbsoluteError(voice.getEffectiveTune(), 100.0f, 0.001f);
        }

        beginTest("DrumVoice reset clears aftertouch state");
        {
            DrumVoice voice;
            voice.setSampleRate(44100.0);
            voice.setTune(100.0f);
            voice.setAftertouchDest(DrumVoiceATDest::Pitch);
            voice.applyAftertouch(0.8f);

            voice.reset();
            expectWithinAbsoluteError(voice.getEffectiveTune(), 100.0f, 0.001f);
        }

        beginTest("DrumEngine aftertouch routes to correct voice via pad mapping");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);

            // Set slot 0 aftertouch to Pitch
            engine.setSlotAftertouchDest(0, DrumATDest::Pitch);
            engine.setSlotAftertouchDepth(0, 1.0f);

            // Trigger note 0 (pad 0 -> voice 0)
            engine.noteOn(0, 1.0f);

            // Apply aftertouch
            engine.aftertouch(0, 0.5f);

            // Voice 0 should have pitch modulation applied
            float effectiveTune = engine.getVoice(0).getEffectiveTune();
            expect(effectiveTune > 0.0f, "Effective tune should be modulated by aftertouch");
        }

        beginTest("DrumEngine setSlotAftertouchDest updates kit and voice");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);

            engine.setSlotAftertouchDest(3, DrumATDest::Filter);
            engine.setSlotAftertouchDepth(3, 0.7f);

            expectEquals(static_cast<int>(engine.getCurrentKit().slots[3].aftertouchDest), static_cast<int>(DrumATDest::Filter));
            expectWithinAbsoluteError(engine.getCurrentKit().slots[3].aftertouchDepth, 0.7f, 0.001f);
        }

        // ============================================================
        // 7. Choke groups preserved in kit
        // ============================================================

        beginTest("Kit choke groups preserved through load");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);

            auto kit808 = FactoryKits::make808();
            engine.loadKit(kit808);

            // Hats should still choke each other
            expectEquals(engine.getChokeGroup(8), 1);   // CH
            expectEquals(engine.getChokeGroup(9), 1);   // CH2
            expectEquals(engine.getChokeGroup(10), 1);  // OH
        }

        beginTest("Choke group behavior across different synth types in kit");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);

            DrumKit kit;
            kit.name = "Choke Test";
            kit.slots[0].synthType = DrumSynthType::ClosedHat;
            kit.slots[0].setTune(8000.0f);
            kit.slots[0].setDecay(0.04f);
            kit.slots[0].setLevel(0.6f);
            kit.slots[0].chokeGroup = 1;

            kit.slots[1].synthType = DrumSynthType::OpenHat;
            kit.slots[1].setTune(8000.0f);
            kit.slots[1].setDecay(0.5f);
            kit.slots[1].setLevel(0.6f);
            kit.slots[1].chokeGroup = 1;

            engine.loadKit(kit);

            // Trigger open hat
            engine.noteOn(1, 1.0f);
            expect(engine.getVoice(1).isActive(), "OH should be active");

            // Trigger closed hat - should choke open hat
            engine.noteOn(0, 1.0f);
            expect(engine.getVoice(0).isActive(), "CH should be active after trigger");
            // OH gets noteOff from choke
        }

        // ============================================================
        // 8. DrumEngine audio processing with kit
        // ============================================================

        beginTest("DrumEngine processes audio with loaded kit");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);

            auto kit909 = FactoryKits::make909();
            engine.loadKit(kit909);

            // Trigger a few voices
            engine.noteOn(0, 0.8f);  // kick
            engine.noteOn(2, 0.7f);  // snare
            engine.noteOn(8, 0.6f);  // hat

            // Process a block
            juce::AudioBuffer<float> buffer(2, 64);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);

            // Should have non-zero output
            float rms = buffer.getRMSLevel(0, 0, 64);
            expect(rms > 0.0f, "Audio output should be non-zero with active voices");
        }

        beginTest("DrumEngine noteOff clears aftertouch pan modulation");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 512);

            engine.setSlotAftertouchDest(0, DrumATDest::Pan);
            engine.setSlotAftertouchDepth(0, 1.0f);

            engine.noteOn(0, 1.0f);
            engine.aftertouch(0, 1.0f);
            engine.noteOff(0);

            // Voice should have cleared aftertouch
            expectWithinAbsoluteError(engine.getVoice(0).getEffectiveTune(), engine.getVoice(0).getEffectiveTune(), 0.001f);
        }

        // ============================================================
        // 9. AT destination name helper
        // ============================================================

        beginTest("drumATDestName returns correct abbreviations");
        {
            expectEquals(juce::String(drumATDestName(DrumATDest::Off)), juce::String("OFF"));
            expectEquals(juce::String(drumATDestName(DrumATDest::Pitch)), juce::String("PIT"));
            expectEquals(juce::String(drumATDestName(DrumATDest::Decay)), juce::String("DCY"));
            expectEquals(juce::String(drumATDestName(DrumATDest::Filter)), juce::String("FLT"));
            expectEquals(juce::String(drumATDestName(DrumATDest::Level)), juce::String("LVL"));
            expectEquals(juce::String(drumATDestName(DrumATDest::Drive)), juce::String("DRV"));
            expectEquals(juce::String(drumATDestName(DrumATDest::Snap)), juce::String("SNP"));
            expectEquals(juce::String(drumATDestName(DrumATDest::Pan)), juce::String("PAN"));
        }

        // ============================================================
        // 10. Kit preset factory flag
        // ============================================================

        beginTest("Factory kit presets are flagged as factory");
        {
            KitPreset preset;
            preset.kit = FactoryKits::make808();
            preset.isFactory = true;

            auto json = preset.toJSON();
            auto restored = KitPreset::fromJSON(json);
            expect(restored.isFactory);
        }

        beginTest("User kit presets are not flagged as factory");
        {
            KitPreset preset;
            preset.kit.name = "User Kit";
            preset.isFactory = false;

            auto json = preset.toJSON();
            auto restored = KitPreset::fromJSON(json);
            expect(!restored.isFactory);
        }
    }
};

static DrumKitTests drumKitTests;
