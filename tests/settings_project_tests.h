#pragma once

// ---------------------------------------------------------------------------
// SETTINGS screen (SETUP / MASTR / PROJ) and whole-project save / load.
// ---------------------------------------------------------------------------

#include "../src/ui/screens/SettingsScreen.h"
#include "../src/project/ProjectBridge.h"
#include "../src/project/ProjectManager.h"

// ===========================================================================
// PROJ page: slots and the turn-to-confirm actions
// ===========================================================================

class SettingsProjectPageTests : public juce::UnitTest
{
public:
    SettingsProjectPageTests() : UnitTest("SettingsScreen Project Page") {}

    struct Fake
    {
        bool used[SettingsScreen::NUM_SLOTS] = {};
        int current = -1;
        int loads = 0, saves = 0, saveAs = 0, news = 0;
        int lastSlot = -1;
        bool autosave = true;

        SettingsScreen::Hooks hooks()
        {
            SettingsScreen::Hooks h;
            h.slotUsed = [this](int s) { return used[s]; };
            h.currentSlot = [this] { return current; };
            h.loadSlot = [this](int s) { if (!used[s]) return false; ++loads; lastSlot = s; current = s; return true; };
            h.saveToSlot = [this](int s) { ++saveAs; lastSlot = s; used[s] = true; current = s; return true; };
            h.saveCurrent = [this] { if (current < 0) return false; ++saves; return true; };
            h.newProject = [this] { ++news; current = -1; };
            h.getAutosave = [this] { return autosave; };
            h.setAutosave = [this](bool on) { autosave = on; };
            return h;
        }
    };

    static void toProjectPage(SettingsScreen& screen)
    {
        while (screen.getName() != "PROJ")
            screen.onArrowRight();
    }

    void runTest() override
    {
        beginTest("Three pages; PROJ cells are SLOT LOAD SAVE SVAS / NEW AUTO");
        {
            SettingsScreen screen;
            expectEquals(screen.getPageCount(), 3);
            expectEquals(screen.getName(), juce::String("SETUP"));
            toProjectPage(screen);
            const char* expected[8] = { "SLOT", "LOAD", "SAVE", "SVAS", "NEW", "AUTO", "---", "---" };
            for (int i = 0; i < 8; ++i)
                expectEquals(screen.getEncoderName(i), juce::String(expected[i]));
            expect(!screen.usesEncoderBar());
        }

        beginTest("An action needs four clockwise steps; fewer, or turning back, does nothing");
        {
            Fake fake;
            SettingsScreen screen(fake.hooks());
            screen.setClock([t = juce::int64(0)]() mutable { return t += 100; });   // 100 ms between clicks
            toProjectPage(screen);

            for (int i = 0; i < SettingsScreen::ACTION_STEPS - 1; ++i)
                screen.onEncoderChange(3, 1);                    // SVAS, three steps
            expectEquals(fake.saveAs, 0);
            screen.onEncoderChange(3, 127);                      // back: cancelled
            for (int i = 0; i < SettingsScreen::ACTION_STEPS - 1; ++i)
                screen.onEncoderChange(3, 1);
            expectEquals(fake.saveAs, 0);
            screen.onEncoderChange(3, 1);                        // fourth in a row
            expectEquals(fake.saveAs, 1);
            expectEquals(fake.lastSlot, 0);

            // Touching another cell in between cancels too.
            screen.onEncoderChange(4, 1);
            screen.onEncoderChange(4, 1);
            screen.onEncoderChange(5, 1);
            screen.onEncoderChange(4, 1);
            screen.onEncoderChange(4, 1);
            expectEquals(fake.news, 0);
        }

        beginTest("Save-as to a chosen slot, then load it back; the status line follows");
        {
            Fake fake;
            SettingsScreen screen(fake.hooks());
            screen.setClock([t = juce::int64(0)]() mutable { return t += 100; });   // 100 ms between clicks
            toProjectPage(screen);
            expect(screen.getStatusLine().contains("P01 EMPTY"));
            expect(screen.getStatusLine().contains("UNSAVED"));

            screen.onEncoderChange(0, 4);                        // SLOT -> P05
            expectEquals(screen.getSelectedSlot(), 4);
            for (int i = 0; i < SettingsScreen::ACTION_STEPS; ++i)
                screen.onEncoderChange(3, 1);
            expectEquals(fake.saveAs, 1);
            expectEquals(fake.lastSlot, 4);
            expect(screen.getStatusLine().contains("P05 USED"));
            expect(screen.getStatusLine().contains("NOW P05"));

            // SAVE now writes the current project without asking for a slot.
            for (int i = 0; i < SettingsScreen::ACTION_STEPS; ++i)
                screen.onEncoderChange(2, 1);
            expectEquals(fake.saves, 1);
            expectEquals(fake.saveAs, 1);

            // Loading an empty slot fails and loads nothing.
            screen.onEncoderChange(0, 1);                        // P06
            for (int i = 0; i < SettingsScreen::ACTION_STEPS; ++i)
                screen.onEncoderChange(1, 1);
            expectEquals(fake.loads, 0);

            screen.onEncoderChange(0, 127);                      // back to P05
            for (int i = 0; i < SettingsScreen::ACTION_STEPS; ++i)
                screen.onEncoderChange(1, 1);
            expectEquals(fake.loads, 1);
        }

        beginTest("SAVE on a never-saved project uses the selected slot; NEW and AUTO work");
        {
            Fake fake;
            SettingsScreen screen(fake.hooks());
            screen.setClock([t = juce::int64(0)]() mutable { return t += 100; });   // 100 ms between clicks
            toProjectPage(screen);
            screen.onEncoderChange(0, 2);                        // P03
            for (int i = 0; i < SettingsScreen::ACTION_STEPS; ++i)
                screen.onEncoderChange(2, 1);
            expectEquals(fake.saveAs, 1);
            expectEquals(fake.lastSlot, 2);

            for (int i = 0; i < SettingsScreen::ACTION_STEPS; ++i)
                screen.onEncoderChange(4, 1);
            expectEquals(fake.news, 1);

            screen.onEncoderChange(5, 127);
            expect(!fake.autosave);
            screen.onEncoderChange(5, 1);
            expect(fake.autosave);
        }

        beginTest("Renders every page");
        {
            Fake fake;
            SettingsScreen screen(fake.hooks());
            screen.setClock([t = juce::int64(0)]() mutable { return t += 100; });   // 100 ms between clicks
            DisplayRenderer display;
            for (int p = 0; p < screen.getPageCount(); ++p)
            {
                screen.render(display);
                screen.onArrowRight();
            }
            expect(true);
        }
    }
};

static SettingsProjectPageTests settingsProjectPageTests;

// ===========================================================================
// SETUP and MASTR pages, and the whole project through a file
// ===========================================================================

class SettingsClockPageTests : public juce::UnitTest
{
public:
    SettingsClockPageTests() : UnitTest("SettingsScreen Clock Page") {}

    void runTest() override
    {
        beginTest("SETUP: tempo, swing, quantize, clock, level and pattern reach their hooks");
        {
            float tempo = 120.0f, swing = 0.0f, quant = 0.0f, level = 0.8f;
            int clock = 0, pattern = 0;
            SettingsScreen::Hooks h;
            h.getTempo = [&] { return tempo; };       h.setTempo = [&](float v) { tempo = v; };
            h.getSwing = [&] { return swing; };       h.setSwing = [&](float v) { swing = v; };
            h.getQuantize = [&] { return quant; };    h.setQuantize = [&](float v) { quant = v; };
            h.getClockMode = [&] { return clock; };   h.setClockMode = [&](int v) { clock = v; };
            h.getMasterLevel = [&] { return level; }; h.setMasterLevel = [&](float v) { level = v; };
            h.getPattern = [&] { return pattern; };   h.setPattern = [&](int v) { pattern = v; };
            SettingsScreen screen(std::move(h));

            const char* expected[8] = { "BPM", "SWNG", "QNT", "CLK", "MVOL", "PAT", "HELP", "---" };
            for (int i = 0; i < 8; ++i)
                expectEquals(screen.getEncoderName(i), juce::String(expected[i]));

            screen.onEncoderChange(0, 5);
            expectWithinAbsoluteError(tempo, 125.0f, 0.01f);
            screen.onEncoderChange(1, 10);
            expect(swing > 0.05f);
            screen.onEncoderChange(2, 10);
            expect(quant > 0.05f);
            screen.onEncoderChange(3, 3);
            expectEquals(clock, 3);
            screen.onEncoderChange(4, 118);
            expect(level < 0.75f);
            screen.onEncoderChange(5, 2);
            expectEquals(pattern, 2);
        }

        beginTest("MASTR: the eight cells set the master EQ, compressor and limiter");
        {
            AudioEngine audio;
            SettingsScreen::Hooks h;
            h.getMaster = [&](int i) { return audio.getMasterParam(i); };
            h.setMaster = [&](int i, float v) { audio.setMasterParam(i, v); };
            SettingsScreen screen(std::move(h));
            screen.onArrowRight();
            expectEquals(screen.getName(), juce::String("MASTR"));
            const char* expected[8] = { "LOW", "MID", "MFRQ", "HIGH", "THR", "RATI", "MAKE", "CEIL" };
            for (int i = 0; i < 8; ++i)
                expectEquals(screen.getEncoderName(i), juce::String(expected[i]));

            screen.onEncoderChange(0, 20);
            expect(audio.getMasterEQ().getParameter(0) > 3.0f, "Low shelf gain raised");
            screen.onEncoderChange(4, 78);     // threshold down
            expect(audio.getMasterComp().getThreshold() < -10.0f);
            screen.onEncoderChange(5, 30);
            expect(audio.getMasterComp().getRatio() > 2.0f);
        }

        beginTest("A whole project survives save to a folder and load into a fresh instrument");
        {
            auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("neptune_project_roundtrip_test");
            dir.deleteRecursively();

            // ---- build some state --------------------------------------------------------------
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            SongMode song;
            CustomScaleManager scales;
            AftertouchRouter pressure;
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                audio.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));

            seq.setTempo(97.0f);
            audio.setMasterLevel(0.6f);
            audio.setMasterParam(4, -18.0f);
            audio.setEngineType(1, EngineType::Drum);
            audio.setEngineType(2, EngineType::Wavetable);
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
            {
                seq.getPattern(p).getTrack(1).engineType = EngineType::Drum;
                seq.getPattern(p).getTrack(2).engineType = EngineType::Wavetable;
            }
            audio.setTrackParameter(0, UnifiedParam::CUTOFF, 1234.0f);
            audio.setTrackParameter(0, UnifiedParam::MODEL_A, 2.0f);                              // ladder
            audio.setTrackParameter(0, MixParam::VOLUME, 0.5f);
            audio.setTrackParameter(0, MixParam::PAN, -0.4f);
            audio.setTrackParameter(0, TrackFxParam::TALK_BASE + TalkBlock::Mix, 0.7f);
            audio.setTrackParameter(1, DrumEngine::PARAMS_PER_VOICE * 8 + DrumEngine::DECAY, 1.25f);  // kick decay
            audio.setTrackParameter(2, WavetableEngine::TableSelect, 5.0f);
            audio.addInsertEffect(0, EffectType::DelayEffect);
            audio.setTrackParameter(0, AudioEngine::EFFECT_PARAM_BASE + 1, 0.8f);                 // delay feedback
            audio.setSendBusEffect(0, EffectType::HallReverbEffect);

            auto& step = seq.getPattern(0).getTrack(0).getStep(3);
            step.trig = true;
            step.notes = { { 64, 0.9f }, { 67, 0.8f } };
            step.length = 2.5f;
            step.trigCondition.type = TrigCondition::Probability;
            step.trigCondition.probability = 0.4f;
            step.paramLocks[UnifiedParam::CUTOFF] = 300.0f;
            seq.getPattern(0).getTrack(0).setLength(32);
            seq.getPattern(0).getTrack(0).getStep(20).trig = true;
            seq.getPattern(5).getTrack(1).getStep(7).trig = true;
            seq.getPattern(0).getTrack(3).muted = true;

            song.addEntry(5, 3);
            song.addEntry(0, 1);
            song.setLoopMode(false);
            scales.setRootNote(0, 62);
            pressure.setRoute(0, { AftertouchRouter::Destination::FilterCutoff, 0.6f, false });

            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Down);
            arp.setOctaveRange(3);
            arp.setGate(0.25f);
            arp.setDice(0.6f);
            arp.setVariation(42);

            // ---- save ---------------------------------------------------------------------------
            ProjectManager saver;
            saver.onCapture = [&] { return ProjectBridge::capture({ seq, audio, &song, &scales, &pressure,
                                                                    [&](int t) { return t == 2 ? &arp : nullptr; } }, "ROUNDTRIP"); };
            expect(saver.saveProjectAs(dir), "Saved");
            expect(dir.getChildFile("project.json").existsAsFile());
            expect(saver.saveProject(), "A saved project can simply be saved again");

            // ---- load into a completely fresh instrument ------------------------------------------
            SequencerCore seq2;
            AudioEngine audio2;
            SongMode song2;
            CustomScaleManager scales2;
            AftertouchRouter pressure2;
            Arpeggiator arp2;
            ProjectManager loader;
            loader.onRebuild = [&](const ProjectData& d) { ProjectBridge::apply(d, { seq2, audio2, &song2, &scales2, &pressure2,
                                                                                      [&](int t) { return t == 2 ? &arp2 : nullptr; } }); };
            expect(loader.loadProject(dir), "Loaded");

            expect(arp2.getMode() == Arpeggiator::Mode::Down);
            expectEquals(arp2.getOctaveRange(), 3);
            expectWithinAbsoluteError(arp2.getGate(), 0.25f, 0.001f);
            expectWithinAbsoluteError(arp2.getDice(), 0.6f, 0.001f);
            expectEquals(arp2.getVariation(), 42);

            expectWithinAbsoluteError(seq2.getTempo(), 97.0f, 0.01f);
            expectWithinAbsoluteError(audio2.getMasterLevel(), 0.6f, 0.001f);
            expectWithinAbsoluteError(audio2.getMasterParam(4), -18.0f, 0.01f);

            expect(audio2.getEngine(0) != nullptr && audio2.getEngine(0)->getType() == EngineType::UnifiedVoice);
            expect(audio2.getEngine(1) != nullptr && audio2.getEngine(1)->getType() == EngineType::Drum);
            expect(audio2.getEngine(2) != nullptr && audio2.getEngine(2)->getType() == EngineType::Wavetable);
            expectWithinAbsoluteError(audio2.getTrackParameter(0, UnifiedParam::CUTOFF), 1234.0f, 0.5f);
            expectEquals((int) audio2.getTrackParameter(0, UnifiedParam::MODEL_A), 2);
            expectWithinAbsoluteError(audio2.getTrackParameter(0, MixParam::VOLUME), 0.5f, 0.001f);
            expectWithinAbsoluteError(audio2.getTrackParameter(0, MixParam::PAN), -0.4f, 0.001f);
            expectWithinAbsoluteError(audio2.getTrackParameter(0, TrackFxParam::TALK_BASE + TalkBlock::Mix), 0.7f, 0.001f);
            expectWithinAbsoluteError(audio2.getTrackParameter(1, DrumEngine::PARAMS_PER_VOICE * 8 + DrumEngine::DECAY), 1.25f, 0.001f);
            expectEquals((int) audio2.getTrackParameter(2, WavetableEngine::TableSelect), 5);

            EffectType insertType;
            expect(audio2.getInsertEffectType(0, 0, insertType) && insertType == EffectType::DelayEffect, "Insert effect restored");
            expectWithinAbsoluteError(audio2.getTrackParameter(0, AudioEngine::EFFECT_PARAM_BASE + 1), 0.8f, 0.001f);
            expect(audio2.getSendBusType(0) == EffectType::HallReverbEffect);

            auto& t0 = seq2.getPattern(0).getTrack(0);
            expectEquals(t0.length, 32);
            expect(t0.getStep(20).trig);
            auto& s3 = t0.getStep(3);
            expect(s3.trig);
            expectEquals((int) s3.notes.size(), 2);
            expectEquals(s3.notes[1].pitch, 67);
            expectWithinAbsoluteError(s3.length, 2.5f, 0.001f);
            expect(s3.trigCondition.type == TrigCondition::Probability);
            expectWithinAbsoluteError(s3.trigCondition.probability, 0.4f, 0.001f);
            expectWithinAbsoluteError(s3.paramLocks[UnifiedParam::CUTOFF], 300.0f, 0.5f);
            expect(seq2.getPattern(5).getTrack(1).getStep(7).trig, "Other patterns are stored too");
            expect(seq2.getPattern(0).getTrack(3).muted);
            expect(!seq2.getPattern(0).getTrack(0).getStep(4).trig);

            expectEquals(song2.getEntryCount(), 2);
            expectEquals(song2.getEntry(0).patternIndex, 5);
            expectEquals(song2.getEntry(0).repeatCount, 3);
            expect(!song2.getLoopMode());
            expectEquals(scales2.getRootNote(0), 62);
            expect(pressure2.getRoute(0).dest == AftertouchRouter::Destination::FilterCutoff);
            expectWithinAbsoluteError(pressure2.getRoute(0).depth, 0.6f, 0.001f);

            // ---- "new" clears it again -----------------------------------------------------------
            loader.newProject();
            expect(!seq2.getPattern(0).getTrack(0).getStep(3).trig, "New project has no steps");
            expectEquals(seq2.getPattern(0).getTrack(0).length, 16);
            expectEquals(song2.getEntryCount(), 0);
            expect(audio2.getInsertChain(0) == nullptr || audio2.getInsertChain(0)->getEffectCount() == 0);
            expectWithinAbsoluteError(audio2.getTrackParameter(0, MixParam::VOLUME), MixParam::DEFAULT_VOLUME, 0.001f);
            expectWithinAbsoluteError(audio2.getTrackParameter(0, TrackFxParam::TALK_BASE + TalkBlock::Mix), 0.0f, 0.001f);

            dir.deleteRecursively();
        }
    }
};

static SettingsClockPageTests settingsClockPageTests;
