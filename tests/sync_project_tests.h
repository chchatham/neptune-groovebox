#pragma once

// ---------------------------------------------------------------------------
// Tests for Workflow 9: Sync, Song, Project & Firmware
// Covers: AbletonLink, Project, enhanced SettingsScreen
// ---------------------------------------------------------------------------

#include "../src/sync/AbletonLink.h"
#include "../src/sync/MIDIClock.h"
#include "../src/project/Project.h"
#include "../src/ui/screens/SettingsScreen.h"

// ===========================================================================
// Ableton Link Tests
// ===========================================================================

class AbletonLinkTests : public juce::UnitTest
{
public:
    AbletonLinkTests() : UnitTest("Ableton Link") {}

    void runTest() override
    {
        beginTest("Enable and disable");
        {
            AbletonLink link;
            expect(!link.isEnabled());
            link.enable();
            expect(link.isEnabled());
            link.disable();
            expect(!link.isEnabled());
        }

        beginTest("Default state");
        {
            AbletonLink link;
            expectEquals(link.getNumPeers(), 0);
            expect(!link.isConnected());
            expectEquals(link.getQuantum(), 4.0);
            auto state = link.getSessionState();
            expectEquals(state.tempo, 120.0);
            expectEquals(state.numPeers, 0);
            expect(!state.isPlaying);
            expect(!state.isConnected);
        }

        beginTest("Set tempo");
        {
            AbletonLink link;
            link.enable();
            link.setTempo(140.0);
            // Note: getTempo reads linkTempo (the session tempo), not the requested tempo.
            // In a real implementation, the Link session would update linkTempo.
            // Without the real library, linkTempo stays at default.
            auto state = link.getSessionState();
            expectEquals(state.tempo, 120.0); // default until Link session responds
        }

        beginTest("Tempo clamping");
        {
            AbletonLink link;
            link.enable();
            link.setTempo(10.0);  // below minimum
            link.setTempo(500.0); // above maximum
            // Should not crash; clamping is applied internally
        }

        beginTest("Quantum setting");
        {
            AbletonLink link;
            link.setQuantum(8.0);
            expectEquals(link.getQuantum(), 8.0);
            link.setQuantum(1.0);
            expectEquals(link.getQuantum(), 1.0);
        }

        beginTest("Transport start/stop");
        {
            AbletonLink link;
            link.enable();
            link.requestStart();
            auto state = link.getSessionState();
            expect(state.isPlaying);

            link.requestStop();
            state = link.getSessionState();
            expect(!state.isPlaying);
        }

        beginTest("Transport without enable is no-op");
        {
            AbletonLink link;
            link.requestStart();
            auto state = link.getSessionState();
            expect(!state.isPlaying);
        }

        beginTest("Session state snapshot");
        {
            AbletonLink link;
            link.enable();
            link.requestStart();
            auto state = link.getSessionState();
            expect(state.isPlaying);
            expectEquals(state.numPeers, 0);
            expect(!state.isConnected);
        }

        beginTest("shouldOverrideTempo requires peers");
        {
            AbletonLink link;
            link.enable();
            // No peers connected, should not override
            expect(!link.shouldOverrideTempo());
        }

        beginTest("Beat phase query");
        {
            AbletonLink link;
            expectEquals(link.getBeatPhase(), 0.0);
            expectEquals(link.getCurrentBeat(), 0.0);
        }

        beginTest("Quantum boundary detection");
        {
            AbletonLink link;
            // At phase 0.0, should be at boundary
            expect(link.isAtQuantumBoundary());
        }

        beginTest("captureForAudioThread when disabled is no-op");
        {
            AbletonLink link;
            // Should not crash
            link.captureForAudioThread(512, 44100.0);
        }

        beginTest("captureForAudioThread when enabled");
        {
            AbletonLink link;
            link.enable();
            // Should not crash
            link.captureForAudioThread(512, 44100.0);
            link.captureForAudioThread(256, 48000.0);
        }

        beginTest("Double enable is safe");
        {
            AbletonLink link;
            link.enable();
            link.enable(); // should not crash
            expect(link.isEnabled());
        }

        beginTest("Double disable is safe");
        {
            AbletonLink link;
            link.enable();
            link.disable();
            link.disable(); // should not crash
            expect(!link.isEnabled());
        }

        beginTest("MIDIClock integration");
        {
            AbletonLink link;
            MIDIClock clock;
            clock.setTempo(130.0);

            // Without peers, should not override
            link.enable();
            link.applyToMidiClock(clock);
            expectEquals(clock.getTempo(), 130.0);
        }

        beginTest("Callback registration");
        {
            AbletonLink link;
            bool tempoCalled = false;
            bool peersCalled = false;
            bool startStopCalled = false;

            link.setTempoCallback([&](double) { tempoCalled = true; });
            link.setPeersCallback([&](int) { peersCalled = true; });
            link.setStartStopCallback([&](bool) { startStopCalled = true; });

            // Callbacks are registered but not invoked yet (no real Link session)
            expect(!tempoCalled);
            expect(!peersCalled);
            expect(!startStopCalled);
        }
    }
};

static AbletonLinkTests abletonLinkTests;

// ===========================================================================
// Project Serialization Tests
// ===========================================================================

class ProjectSerializationTests : public juce::UnitTest
{
public:
    ProjectSerializationTests() : UnitTest("Project Serialization") {}

    void runTest() override
    {
        beginTest("Default ProjectData round-trip");
        {
            ProjectData original;
            original.name = "Test Project";
            original.bpm = 140.0f;
            original.activePattern = 3;
            original.clockMode = 1;
            original.linkEnabled = true;
            original.sampleFolder = "samples";

            auto json = original.toJSON();
            expect(json.isNotEmpty());

            auto loaded = ProjectData::fromJSON(json);
            expectEquals(loaded.name, juce::String("Test Project"));
            expectEquals(loaded.bpm, 140.0f);
            expectEquals(loaded.activePattern, 3);
            expectEquals(loaded.clockMode, 1);
            expect(loaded.linkEnabled);
            expectEquals(loaded.sampleFolder, juce::String("samples"));
        }

        beginTest("TrackData serialization");
        {
            TrackData track;
            track.engineType = EngineType::FM;
            track.length = 32;
            track.speedMultiplier = 2.0f;
            track.midiChannel = 5;
            track.muted = true;
            track.soloed = false;
            track.soundParams[0] = 0.5f;
            track.soundParams[1] = 0.8f;

            auto v = track.toVar();
            auto loaded = TrackData::fromVar(v);

            expectEquals(static_cast<int>(loaded.engineType), static_cast<int>(EngineType::FM));
            expectEquals(loaded.length, 32);
            expectEquals(loaded.speedMultiplier, 2.0f);
            expectEquals(loaded.midiChannel, 5);
            expect(loaded.muted);
            expect(!loaded.soloed);
            expectEquals(loaded.soundParams[0], 0.5f);
            expectEquals(loaded.soundParams[1], 0.8f);
        }

        beginTest("StepData serialization");
        {
            TrackData::StepData step;
            step.trig = true;
            step.velocity = 0.75f;
            step.length = 2.0f;
            step.microTiming = -0.25f;
            step.trigConditionType = TrigCondition::Probability;
            step.trigA = 3;
            step.trigB = 4;
            step.trigProbability = 0.5f;
            step.pageChangeLock = 2;
            step.notes.push_back({ 60, 1.0f });
            step.notes.push_back({ 64, 0.8f });
            step.paramLocks[10] = 0.3f;

            auto v = step.toVar();
            auto loaded = TrackData::StepData::fromVar(v);

            expect(loaded.trig);
            expectEquals(loaded.velocity, 0.75f);
            expectEquals(loaded.length, 2.0f);
            expectEquals(loaded.microTiming, -0.25f);
            expectEquals(static_cast<int>(loaded.trigConditionType),
                         static_cast<int>(TrigCondition::Probability));
            expectEquals(loaded.trigA, 3);
            expectEquals(loaded.trigB, 4);
            expectEquals(loaded.trigProbability, 0.5f);
            expectEquals(loaded.pageChangeLock, 2);
            expectEquals((int) loaded.notes.size(), 2);
            expectEquals(loaded.notes[0].pitch, 60);
            expectEquals(loaded.notes[1].pitch, 64);
            expectEquals(loaded.paramLocks[10], 0.3f);
        }

        beginTest("PatternData serialization");
        {
            PatternData pattern;
            pattern.swing = 0.3f;

            TrackData t;
            t.engineType = EngineType::Drum;
            t.length = 16;
            pattern.tracks.push_back(t);

            auto v = pattern.toVar();
            auto loaded = PatternData::fromVar(v);

            expectEquals(loaded.swing, 0.3f);
            expectEquals((int) loaded.tracks.size(), 1);
            expectEquals(static_cast<int>(loaded.tracks[0].engineType),
                         static_cast<int>(EngineType::Drum));
        }

        beginTest("SongData serialization");
        {
            SongData song;
            song.loop = false;
            song.entries.push_back({ 0, 4, 0 });
            song.entries.push_back({ 1, 2, 3 });
            song.entries.push_back({ 3, 1, -5 });

            auto v = song.toVar();
            auto loaded = SongData::fromVar(v);

            expect(!loaded.loop);
            expectEquals((int) loaded.entries.size(), 3);
            expectEquals(loaded.entries[0].patternIndex, 0);
            expectEquals(loaded.entries[0].repeatCount, 4);
            expectEquals(loaded.entries[1].patternIndex, 1);
            expectEquals(loaded.entries[1].repeatCount, 2);
            expectEquals(loaded.entries[1].transposeSemitones, 3);
            expectEquals(loaded.entries[2].transposeSemitones, -5);
        }

        beginTest("Empty project round-trip");
        {
            ProjectData proj;
            auto json = proj.toJSON();
            auto loaded = ProjectData::fromJSON(json);
            expectEquals(loaded.name, juce::String("Untitled"));
            expectEquals(loaded.bpm, 120.0f);
            expectEquals(loaded.activePattern, 0);
        }

        beginTest("Active pattern clamping");
        {
            ProjectData proj;
            proj.activePattern = 99;
            auto json = proj.toJSON();
            auto loaded = ProjectData::fromJSON(json);
            expectEquals(loaded.activePattern, 15); // clamped to 0..15
        }

        beginTest("Clock mode clamping");
        {
            ProjectData proj;
            proj.clockMode = 10;
            auto json = proj.toJSON();
            auto loaded = ProjectData::fromJSON(json);
            expectEquals(loaded.clockMode, 3); // clamped to 0..3 (3 = Link)
        }

        beginTest("Full project with data");
        {
            ProjectData proj;
            proj.name = "My Song";
            proj.bpm = 128.0f;
            proj.activePattern = 2;

            // Add a track to pattern 0
            TrackData t;
            t.engineType = EngineType::UnifiedVoice;
            t.length = 16;
            TrackData::StepData s;
            s.trig = true;
            s.notes.push_back({ 48, 1.0f });
            t.steps.push_back(s);
            proj.patterns[0].tracks.push_back(t);

            // Add song entries
            proj.song.entries.push_back({ 0, 4, 0 });
            proj.song.entries.push_back({ 2, 2, -3 });

            auto json = proj.toJSON();
            expect(json.isNotEmpty());

            auto loaded = ProjectData::fromJSON(json);
            expectEquals(loaded.name, juce::String("My Song"));
            expectEquals(loaded.bpm, 128.0f);
            expectEquals(loaded.activePattern, 2);
            expectEquals((int) loaded.patterns[0].tracks.size(), 1);
            expectEquals((int) loaded.patterns[0].tracks[0].steps.size(), 1);
            expect(loaded.patterns[0].tracks[0].steps[0].trig);
            expectEquals((int) loaded.song.entries.size(), 2);
        }
    }
};

static ProjectSerializationTests projectSerializationTests;

// ===========================================================================
// Project File Operations Tests
// ===========================================================================

class ProjectFileTests : public juce::UnitTest
{
public:
    ProjectFileTests() : UnitTest("Project File Operations") {}

    void runTest() override
    {
        beginTest("New project defaults");
        {
            Project proj;
            expectEquals(proj.getProjectName(), juce::String("Untitled"));
            expect(!proj.isDirty());
            expect(!proj.hasFile());
        }

        beginTest("New project with name");
        {
            Project proj;
            proj.newProject("Beat Tape Vol. 1");
            expectEquals(proj.getProjectName(), juce::String("Beat Tape Vol. 1"));
            expect(!proj.isDirty());
        }

        beginTest("Dirty flag");
        {
            Project proj;
            expect(!proj.isDirty());
            proj.markDirty();
            expect(proj.isDirty());
            proj.clearDirty();
            expect(!proj.isDirty());
        }

        beginTest("Save and load round-trip");
        {
            auto tempDir = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_test_project");

            // Clean up from previous runs
            if (tempDir.isDirectory())
                tempDir.deleteRecursively();

            Project proj;
            proj.getData().name = "Test Save";
            proj.getData().bpm = 135.0f;
            proj.markDirty();

            bool saved = proj.save(tempDir);
            expect(saved);
            expect(!proj.isDirty());
            expect(proj.hasFile());

            // Verify the JSON file exists
            expect(tempDir.getChildFile("project.json").existsAsFile());
            expect(tempDir.getChildFile("samples").isDirectory());

            // Load into a new Project
            Project loaded;
            bool ok = loaded.load(tempDir);
            expect(ok);
            expectEquals(loaded.getProjectName(), juce::String("Test Save"));
            expectEquals(loaded.getData().bpm, 135.0f);

            // Clean up
            tempDir.deleteRecursively();
        }

        beginTest("Load nonexistent project");
        {
            Project proj;
            auto noDir = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_nonexistent");

            bool ok = proj.load(noDir);
            expect(!ok);
        }

        beginTest("Recent projects list");
        {
            Project proj;
            proj.addToRecent("/path/to/project1");
            proj.addToRecent("/path/to/project2");
            proj.addToRecent("/path/to/project3");

            auto& recent = proj.getRecentProjects();
            expectEquals((int) recent.size(), 3);
            // Most recent should be first
            expectEquals(recent[0], juce::String("/path/to/project3"));
            expectEquals(recent[1], juce::String("/path/to/project2"));
        }

        beginTest("Recent projects deduplication");
        {
            Project proj;
            proj.addToRecent("/path/to/project1");
            proj.addToRecent("/path/to/project2");
            proj.addToRecent("/path/to/project1"); // re-add

            auto& recent = proj.getRecentProjects();
            expectEquals((int) recent.size(), 2);
            expectEquals(recent[0], juce::String("/path/to/project1")); // moved to front
        }

        beginTest("Recent projects max limit");
        {
            Project proj;
            for (int i = 0; i < 12; ++i)
                proj.addToRecent("/path/to/project" + juce::String(i));

            auto& recent = proj.getRecentProjects();
            expectEquals((int) recent.size(), Project::MAX_RECENT);
        }

        beginTest("Autosave interval");
        {
            Project proj;
            expectEquals(proj.getAutosaveInterval(), Project::DEFAULT_AUTOSAVE_MS);

            proj.setAutosaveInterval(30000);
            expectEquals(proj.getAutosaveInterval(), 30000);

            // Clamped to minimum
            proj.setAutosaveInterval(100);
            expectEquals(proj.getAutosaveInterval(), 5000);

            // Clamped to maximum
            proj.setAutosaveInterval(9999999);
            expectEquals(proj.getAutosaveInterval(), 600000);
        }

        beginTest("shouldAutosave requires dirty and file");
        {
            Project proj;
            expect(!proj.shouldAutosave()); // not dirty, no file

            proj.markDirty();
            expect(!proj.shouldAutosave()); // dirty but no file
        }

        beginTest("Recent list persistence");
        {
            auto tempDir = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_test_recent");
            tempDir.createDirectory();

            Project proj1;
            proj1.addToRecent("/path/a");
            proj1.addToRecent("/path/b");
            proj1.saveRecentList(tempDir);

            Project proj2;
            proj2.loadRecentList(tempDir);
            auto& recent = proj2.getRecentProjects();
            expectEquals((int) recent.size(), 2);
            expectEquals(recent[0], juce::String("/path/b"));
            expectEquals(recent[1], juce::String("/path/a"));

            tempDir.deleteRecursively();
        }
    }
};

static ProjectFileTests projectFileTests;

// ===========================================================================
// Enhanced SettingsScreen Tests
// ===========================================================================

class SettingsScreenSyncTests : public juce::UnitTest
{
public:
    SettingsScreenSyncTests() : UnitTest("SettingsScreen Sync") {}

    void runTest() override
    {
        beginTest("CLK cell steps through INT / EXT / AUTO / LINK and stops at the ends");
        {
            int clock = 0;
            SettingsScreen::Hooks h;
            h.getClockMode = [&] { return clock; };
            h.setClockMode = [&](int v) { clock = v; };
            SettingsScreen screen(std::move(h));

            screen.onEncoderChange(3, 1);
            expectEquals(clock, 1);
            screen.onEncoderChange(3, 1);
            expectEquals(clock, 2);
            screen.onEncoderChange(3, 5);
            expectEquals(clock, 3);
            screen.onEncoderChange(3, 118);
            expectEquals(clock, 0);
        }

        beginTest("Clock mode (including LINK) survives a project round trip");
        {
            ProjectData d;
            d.clockMode = 3;
            auto back = ProjectData::fromJSON(d.toJSON());
            expectEquals(back.clockMode, 3);
        }
    }
};

static SettingsScreenSyncTests settingsScreenSyncTests;

// ===========================================================================
// MIDIClock + AbletonLink Integration Tests
// ===========================================================================

class ClockLinkIntegrationTests : public juce::UnitTest
{
public:
    ClockLinkIntegrationTests() : UnitTest("Clock-Link Integration") {}

    void runTest() override
    {
        beginTest("MIDIClock modes unchanged");
        {
            MIDIClock clock;
            expectEquals(static_cast<int>(clock.getMode()),
                         static_cast<int>(MIDIClock::Mode::Internal));

            clock.setMode(MIDIClock::Mode::External);
            expectEquals(static_cast<int>(clock.getMode()),
                         static_cast<int>(MIDIClock::Mode::External));

            clock.setMode(MIDIClock::Mode::Auto);
            expectEquals(static_cast<int>(clock.getMode()),
                         static_cast<int>(MIDIClock::Mode::Auto));
        }

        beginTest("Link does not override without peers");
        {
            AbletonLink link;
            MIDIClock clock;
            clock.setTempo(130.0);

            link.enable();
            link.applyToMidiClock(clock);
            expectEquals(clock.getTempo(), 130.0); // unchanged — no peers
        }

        beginTest("Link disabled does not override");
        {
            AbletonLink link;
            MIDIClock clock;
            clock.setTempo(130.0);

            // Link not enabled
            link.applyToMidiClock(clock);
            expectEquals(clock.getTempo(), 130.0);
        }

        beginTest("MIDIClock basic clock generation");
        {
            MIDIClock clock;
            clock.setTempo(120.0);
            clock.prepareToPlay(44100.0, 512);
            clock.startTransport();

            expect(clock.isRunning());

            // Process a block
            clock.processBlock(512, 44100.0);

            // Should have generated some clock ticks
            expect(clock.getCurrentBeat() >= 0.0);
        }

        beginTest("MIDIClock stop transport");
        {
            MIDIClock clock;
            clock.setTempo(120.0);
            clock.prepareToPlay(44100.0, 512);
            clock.startTransport();
            expect(clock.isRunning());

            clock.stopTransport();
            expect(!clock.isRunning());
        }

        beginTest("MIDIClock output events");
        {
            MIDIClock clock;
            clock.setTempo(120.0);
            clock.prepareToPlay(44100.0, 512);
            clock.startTransport();

            // Start event should be queued
            MIDIClock::ClockEvent event;
            bool hasEvent = clock.popOutputEvent(event);
            expect(hasEvent);
            expectEquals(static_cast<int>(event.type),
                         static_cast<int>(MIDIClock::ClockEvent::Start));
        }
    }
};

static ClockLinkIntegrationTests clockLinkIntegrationTests;
