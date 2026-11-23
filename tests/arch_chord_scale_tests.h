#pragma once
// Tests for: ARCH memory in projects, Scale + Up/Down page stepping, chord set save/load.

#include "../src/engine/UnifiedVoiceEngine.h"
#include "../src/controller/ChordPads.h"
#include "../src/controller/CustomScaleManager.h"
#include "../src/project/ChordSetManager.h"
#include "../src/project/Project.h"
#include "../src/project/ProjectBridge.h"
#include "../src/ui/ScreenManager.h"
#include "../src/ui/screens/ChordPadScreen.h"

class ArchChordScaleTests : public juce::UnitTest
{
public:
    ArchChordScaleTests() : juce::UnitTest("ARCH Chord Scale", "Neptune") {}

    // A dummy screen for the ScreenManager tests.
    struct DummyScreen : public Screen
    {
        DummyScreen(juce::String n = "TRACKS") : nm(std::move(n)) {}
        juce::String getName() const override { return nm; }
        void render(DisplayRenderer&) override {}
        juce::String nm;
    };

    void runTest() override
    {
        // ---- ARCH memory: snapshot and restore across ARCH switches ----------------------------
        beginTest("ARCH memory: switching preserves tweaks");
        {
            UnifiedVoiceEngine engine;
            engine.prepareToPlay(44100.0, 512);

            // Start at INIT (ARCH = 0).
            engine.setParameter(UnifiedParam::ARCH, 0.0f);
            expect(engine.getParameter(UnifiedParam::ARCH) == 0.0f);

            // Tweak cutoff while on INIT.
            engine.setParameter(UnifiedParam::CUTOFF, 2000.0f);
            const float tweakedCutoff = engine.getParameter(UnifiedParam::CUTOFF);
            expectWithinAbsoluteError(tweakedCutoff, 2000.0f, 0.01f);

            // Switch to ARCH 1 (Mono VA).
            engine.setParameter(UnifiedParam::ARCH, 1.0f);

            // Tweak resonance on Mono VA.
            engine.setParameter(UnifiedParam::RESONANCE, 0.75f);

            // Switch back to INIT: the tweaked cutoff should be restored.
            engine.setParameter(UnifiedParam::ARCH, 0.0f);
            expectWithinAbsoluteError(engine.getParameter(UnifiedParam::CUTOFF), tweakedCutoff, 0.01f,
                                      "INIT's tweaked cutoff restored");

            // Switch back to ARCH 1: the tweaked resonance should be restored.
            engine.setParameter(UnifiedParam::ARCH, 1.0f);
            expectWithinAbsoluteError(engine.getParameter(UnifiedParam::RESONANCE), 0.75f, 0.01f,
                                      "Mono VA's tweaked resonance restored");
        }

        beginTest("ARCH memory: unvisited slot applies factory defaults");
        {
            UnifiedVoiceEngine engine;
            engine.prepareToPlay(44100.0, 512);
            // Switch directly to ARCH 2 (never visited: factory defaults).
            engine.setParameter(UnifiedParam::ARCH, 2.0f);
            const float factoryRes = engine.getParameter(UnifiedParam::RESONANCE);
            // Switch to INIT, then back to ARCH 2: same defaults (the slot was now visited with
            // factory values, so restoring them gives the same thing).
            engine.setParameter(UnifiedParam::ARCH, 0.0f);
            engine.setParameter(UnifiedParam::ARCH, 2.0f);
            expectWithinAbsoluteError(engine.getParameter(UnifiedParam::RESONANCE), factoryRes, 0.01f,
                                      "factory defaults unchanged on round trip");
        }

        beginTest("ARCH memory: capture / apply for projects");
        {
            UnifiedVoiceEngine engine;
            engine.prepareToPlay(44100.0, 512);
            engine.setParameter(UnifiedParam::ARCH, 0.0f);
            engine.setParameter(UnifiedParam::CUTOFF, 3000.0f);
            engine.setParameter(UnifiedParam::ARCH, 1.0f);
            engine.setParameter(UnifiedParam::RESONANCE, 0.9f);

            // Capture.
            auto flat = engine.captureArchMemory();
            expect(!flat.empty(), "captured memory is not empty");

            // Apply to a fresh engine.
            UnifiedVoiceEngine engine2;
            engine2.prepareToPlay(44100.0, 512);
            engine2.applyArchMemory(flat);

            // Switch to INIT and check the tweaked cutoff.
            engine2.setParameter(UnifiedParam::ARCH, 0.0f);
            expectWithinAbsoluteError(engine2.getParameter(UnifiedParam::CUTOFF), 3000.0f, 0.01f,
                                      "INIT cutoff restored in fresh engine");
            // Switch to ARCH 1 and check the tweaked resonance.
            engine2.setParameter(UnifiedParam::ARCH, 1.0f);
            expectWithinAbsoluteError(engine2.getParameter(UnifiedParam::RESONANCE), 0.9f, 0.01f,
                                      "Mono VA resonance restored in fresh engine");
        }

        beginTest("ARCH memory: ONE memory - a session that goes on and a project loaded again bring back the same cells, "
                  "the router's too (slots, STRIP, the pressure slots), which the PARAM screen's own memory kept and the "
                  "engine's did not");
        {
            // Every cell of the engine, as the ARCH memory is asked to keep them.
            const auto sound = [](UnifiedVoiceEngine& e)
            {
                std::map<int, float> out;
                for (int id = 0; id < EngineParamRange::END; ++id)
                    if (EngineParamRange::contains(id))
                        out[id] = e.getParameter(id);
                return out;
            };
            UnifiedVoiceEngine live;
            live.prepareToPlay(44100.0, 512);
            live.setParameter(UnifiedParam::ARCH, 1.0f);
            // Dialled in on ARCH 1: a page cell, an LFO's cell, a free slot, a strip route, a pressure slot, a stage curve.
            const int slotAmt = ModSlotParam::id(0, ModSlotParam::AMT), slotSrc = ModSlotParam::id(0, ModSlotParam::SRC),
                      slotDest = ModSlotParam::id(0, ModSlotParam::DEST);
            const int pressDest = PressureSlotParam::id(1, PressureSlotParam::DEST_OFFSET), pressAmt = PressureSlotParam::id(1, PressureSlotParam::AMT_OFFSET);
            const int curve = StageCurveParam::id(0, StageCurveParam::Decay);
            const int lfoRate = ModParam::lfoParamId(1, ModParam::LFO_RATE);
            live.setParameter(UnifiedParam::RESONANCE, 0.83f);
            live.setParameter(lfoRate, 3.25f);
            live.setParameter(slotSrc, 3.0f);
            live.setParameter(slotDest, 2.0f);
            live.setParameter(slotAmt, -0.4f);
            live.setParameter(pressDest, 3.0f);
            live.setParameter(pressAmt, 0.6f);
            live.setParameter(curve, 0.7f);
            const auto dialled = sound(live);
            expectWithinAbsoluteError(dialled.at(slotAmt), -0.4f, 1.0e-6f, "(the slot took its amount)");
            expectWithinAbsoluteError(dialled.at(pressAmt), 0.6f, 1.0e-6f, "(the pressure slot took its amount)");
            expectWithinAbsoluteError(dialled.at(curve), 0.7f, 1.0e-6f, "(the stage took its curve)");

            // Away to ARCH 2 (its starting sound: the slot is not there), where the project is saved.
            live.setParameter(UnifiedParam::ARCH, 2.0f);
            const auto away = sound(live);
            const auto memory = live.captureArchMemory();

            // The project loaded again: a fresh engine, the sound as it was saved, then the memory.
            UnifiedVoiceEngine loaded;
            loaded.prepareToPlay(44100.0, 512);
            loaded.setParameter(UnifiedParam::ARCH, away.at(UnifiedParam::ARCH));
            for (int pass = 0; pass < 3; ++pass)
                for (auto& [id, value] : away)
                    if (id != UnifiedParam::ARCH)
                        loaded.setParameter(id, value);
            loaded.applyArchMemory(memory);

            // Back to ARCH 1, in both.
            live.setParameter(UnifiedParam::ARCH, 1.0f);
            loaded.setParameter(UnifiedParam::ARCH, 1.0f);
            const auto a = sound(live), b = sound(loaded);
            int lost = 0, differ = 0;
            juce::String which;
            for (auto& [id, value] : dialled)
            {
                if (a.at(id) != value) { ++lost; which << " " << id; }
                if (a.at(id) != b.at(id)) { ++differ; which << " (" << id << ")"; }
            }
            expectEquals(lost, 0, "the session brings back every cell as it was dialled in:" + which);
            expectEquals(differ, 0, "and the loaded project the same ones:" + which);
            expectWithinAbsoluteError(b.at(slotAmt), -0.4f, 1.0e-6f, "the free slot, after a reload");
            expectWithinAbsoluteError(b.at(pressAmt), 0.6f, 1.0e-6f, "the pressure slot, after a reload");
            expectWithinAbsoluteError(b.at(curve), 0.7f, 1.0e-6f, "the stage curve, after a reload");
            expectWithinAbsoluteError(b.at(UnifiedParam::RESONANCE), 0.83f, 1.0e-6f, "a page cell, after a reload");

            // The memory holds every engine cell but ARCH itself, within what a project reads (2048 a slot).
            expect(memory.size() > 4 && (int) std::lround(memory[3]) == (int) dialled.size() - 1,
                   "a slot holds every cell but ARCH: " + juce::String(memory.size() > 3 ? (int) std::lround(memory[3]) : -1));
            expect((int) dialled.size() < 2048);
        }

        beginTest("ARCH memory: empty list (old project) is harmless");
        {
            UnifiedVoiceEngine engine;
            engine.prepareToPlay(44100.0, 512);
            engine.applyArchMemory({});     // should not crash or change anything
            engine.setParameter(UnifiedParam::ARCH, 1.0f);
            // No crash, the factory preset is applied.
            expect(engine.getParameter(UnifiedParam::ARCH) == 1.0f);
        }

        // ---- ARCH memory in Project serialization -----------------------------------------------
        beginTest("TrackData serialises archMemory round trip");
        {
            TrackData td;
            td.engineType = EngineType::UnifiedVoice;
            td.archMemory = { 1.0f, 4.0f, 1.0f, 2.0f, 100.0f, 3000.0f, 200.0f, 0.9f,
                              0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            auto v = td.toVar();
            auto td2 = TrackData::fromVar(v);
            expectEquals((int) td2.archMemory.size(), (int) td.archMemory.size(),
                         "archMemory size round trips");
            for (size_t i = 0; i < td.archMemory.size(); ++i)
                expectWithinAbsoluteError(td2.archMemory[i], td.archMemory[i], 0.001f);
        }

        beginTest("Old project without archMemory loads cleanly");
        {
            TrackData td;
            td.engineType = EngineType::UnifiedVoice;
            // Serialise without archMemory (simulating an old project).
            auto v = td.toVar();
            auto td2 = TrackData::fromVar(v);
            expect(td2.archMemory.empty(), "no archMemory in old project");
        }

        // ---- Scale + Up/Down page stepping -----------------------------------------------------
        beginTest("Scale + Up/Down steps scale pages via ScreenManager");
        {
            ScreenManager mgr;
            mgr.setHelpBusyWindow(0);
            mgr.addScreen(std::make_unique<DummyScreen>());

            int scalePage = 0;
            const int pageCount = 5;
            juce::StringArray pageNames { "CHROMATIC", "C MAJOR", "A MINOR", "BLUES", "PENTATONIC" };

            mgr.setScalePageStep([&](int direction) -> juce::String
            {
                scalePage = ((scalePage + direction) % pageCount + pageCount) % pageCount;
                return pageNames[scalePage];
            });

            // Press Scale (hold).
            mgr.controlChangeReceived(1, 110, 127);
            expect(!mgr.isScaleLocked(), "lock not toggled on press (deferred to release)");

            // Press Down while Scale held: steps forward.
            mgr.controlChangeReceived(1, 107, 127);
            mgr.controlChangeReceived(1, 107, 0);
            expectEquals(scalePage, 1, "stepped forward to page 1");
            expect(mgr.isScaleLocked(), "scale lock turned on by the step");

            // Press Down again.
            mgr.controlChangeReceived(1, 107, 127);
            mgr.controlChangeReceived(1, 107, 0);
            expectEquals(scalePage, 2, "stepped forward to page 2");

            // Press Up while Scale held: steps backward.
            mgr.controlChangeReceived(1, 106, 127);
            mgr.controlChangeReceived(1, 106, 0);
            expectEquals(scalePage, 1, "stepped backward to page 1");

            // Release Scale: since Up/Down was used, the lock should NOT toggle off.
            mgr.controlChangeReceived(1, 110, 0);
            expect(mgr.isScaleLocked(), "lock stays on after Scale+Up/Down release");
        }

        beginTest("Scale tap still toggles lock");
        {
            ScreenManager mgr;
            mgr.setHelpBusyWindow(0);
            mgr.addScreen(std::make_unique<DummyScreen>());
            mgr.setScalePageStep([](int) { return juce::String("PAGE"); });

            expect(!mgr.isScaleLocked(), "starts unlocked");

            // Tap Scale (press + release, no Up/Down).
            mgr.controlChangeReceived(1, 110, 127);
            mgr.controlChangeReceived(1, 110, 0);
            expect(mgr.isScaleLocked(), "tap turns lock on");

            mgr.controlChangeReceived(1, 110, 127);
            mgr.controlChangeReceived(1, 110, 0);
            expect(!mgr.isScaleLocked(), "tap turns lock off");
        }

        // ---- Chord set save / load ---------------------------------------------------------------
        beginTest("Chord set save and load round trip");
        {
            auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("neptune_chord_test_" + juce::String(juce::Random::getSystemRandom().nextInt()));
            tempDir.createDirectory();

            ChordSetManager mgr(tempDir);

            ChordPads pads;
            pads.store(0, { { 60, 1.0f }, { 64, 0.8f }, { 67, 0.6f } });
            pads.store(1, { { 72, 1.0f } });
            pads.strumMs = 50.0f;

            expect(mgr.save(0, pads), "save succeeds");
            expect(mgr.slotExists(0), "slot 0 exists");
            expect(!mgr.slotExists(1), "slot 1 does not exist");

            ChordPads loaded;
            expect(mgr.load(0, loaded), "load succeeds");
            expectEquals(loaded.get(0).count, 3, "pad 0 has 3 notes");
            expectEquals(loaded.get(0).notes[0].pitch, 60, "first note is C4");
            expectEquals(loaded.get(1).count, 1, "pad 1 has 1 note");
            expectWithinAbsoluteError(loaded.strumMs, 50.0f, 0.01f, "strum time round trips");

            // Load from an empty slot fails.
            ChordPads empty;
            expect(!mgr.load(5, empty), "load from empty slot fails");

            tempDir.deleteRecursively();
        }

        beginTest("ChordPadScreen SETS page: slot, load, save encoders");
        {
            ChordPads pads;
            ChordPadScreen screen(pads);

            bool saved = false;
            bool loaded = false;
            screen.setChordSetCallbacks(
                [&](int, const ChordPads&) { saved = true; return true; },
                [&](int, ChordPads&) { loaded = true; return true; },
                [](int) { return false; },
                32);

            // The screen should have 3 pages now.
            expectEquals(screen.getPageCount(), 3, "three pages with sets callbacks");

            // Navigate to the SETS page (page 2).
            screen.onArrowRight();      // 0 -> 1
            screen.onArrowRight();      // 1 -> 2
            expectEquals(screen.getPageIndex(), 2, "on the SETS page");

            // Turn SLOT knob.
            screen.onEncoderChange(0, 1);  // step up
            // Two letters a knob (a bar screen's column is sixteen pixels: "SLOT" was drawn "SLO", half a
            // pixel off the glass); the readout says the whole name.
            const char* const sets[8] = { "SL", "", "LD", "", "", "", "", "SV" };
            for (int i = 0; i < 8; ++i)
            {
                expectEquals(screen.getEncoderName(i), juce::String(sets[i]));
                expect(screen.getEncoderName(i).length() <= 2);
            }
            expectEquals(screen.getEncoderReadout(0), juce::String("SLOT 2"));
            expect(screen.getEncoderReadout(2).startsWith("LOAD") && screen.getEncoderReadout(7).startsWith("SAVE"));
            expectEquals(screen.getSubPage(), juce::String("SETS C2"));

            // SAVE: four clockwise clicks (value 1 = one step up).
            // TurnConfirm requires >= 300 ms between first and last click.
            for (int i = 0; i < 4; ++i)
            {
                screen.onEncoderChange(7, 1);
                UiClock::advance(120);         // 120 ms between clicks, total 360 ms > 300 ms
            }
            expect(saved, "save callback was called after four clicks");

            // LOAD: four clockwise clicks.
            for (int i = 0; i < 4; ++i)
            {
                screen.onEncoderChange(2, 1);
                UiClock::advance(120);
            }
            expect(loaded, "load callback was called after four clicks");
        }

        beginTest("ChordPadScreen without SETS callbacks has 2 pages");
        {
            ChordPads pads;
            ChordPadScreen screen(pads);
            expectEquals(screen.getPageCount(), 2, "two pages without sets callbacks");
        }
    }
};

static ArchChordScaleTests archChordScaleTestsInstance;
