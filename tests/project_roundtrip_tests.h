#pragma once

// ---------------------------------------------------------------------------
// Round-trip serialization tests for enriched ProjectData.
//
// Verifies that every field survives a toJSON -> fromJSON cycle:
//   - Steps with param locks, pressure lanes, trig conditions, micro-timing
//   - Tracks with independent lengths, speed multipliers, arp settings
//   - Multiple patterns with varying track counts
//   - Song mode entries
//   - Custom scale pages
//   - Aftertouch routing config
//   - Send bus config
//   - Master level
// ---------------------------------------------------------------------------

#include "../src/project/Project.h"

class ProjectRoundtripTests : public juce::UnitTest
{
public:
    ProjectRoundtripTests() : juce::UnitTest("Project Roundtrip") {}

    void runTest() override
    {
        // -------------------------------------------------------------------
        beginTest("Basic pattern round-trip: create, serialize, deserialize, compare");
        {
            ProjectData orig;
            orig.name = "RT Basic";
            orig.bpm = 135.5f;
            orig.activePattern = 3;
            orig.clockMode = 1;
            orig.linkEnabled = true;
            orig.masterLevel = 0.65f;

            // Set up pattern 3 with 4 tracks
            auto& pat = orig.patterns[3];
            pat.swing = 0.42f;
            pat.tracks.resize(4);
            for (int t = 0; t < 4; ++t)
            {
                auto& trk = pat.tracks[static_cast<size_t>(t)];
                trk.engineType = static_cast<EngineType>(t % 3);
                trk.length = 16 + t * 8;
                trk.speedMultiplier = 1.0f + (float) t * 0.25f;
                trk.midiChannel = t + 1;
                trk.muted = (t == 1);
                trk.soloed = (t == 2);
                trk.steps.resize(static_cast<size_t>(trk.length));

                // Put a trig on step 0
                trk.steps[0].trig = true;
                trk.steps[0].velocity = 0.8f;
                trk.steps[0].length = 0.75f;
                trk.steps[0].notes.push_back({ 60 + t, 0.9f });
            }

            // Round-trip
            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            expectEquals(loaded.name, juce::String("RT Basic"));
            expectWithinAbsoluteError(loaded.bpm, 135.5f, 0.01f);
            expectEquals(loaded.activePattern, 3);
            expectEquals(loaded.clockMode, 1);
            expect(loaded.linkEnabled);
            expectWithinAbsoluteError(loaded.masterLevel, 0.65f, 0.01f);

            auto& lp = loaded.patterns[3];
            expectWithinAbsoluteError(lp.swing, 0.42f, 0.01f);
            expectEquals((int) lp.tracks.size(), 4);

            for (int t = 0; t < 4; ++t)
            {
                auto& trk = lp.tracks[static_cast<size_t>(t)];
                expectEquals(static_cast<int>(trk.engineType), t % 3);
                expectEquals(trk.length, 16 + t * 8);
                expectWithinAbsoluteError(trk.speedMultiplier, 1.0f + (float) t * 0.25f, 0.01f);
                expectEquals(trk.midiChannel, t + 1);
                expect(trk.muted == (t == 1));
                expect(trk.soloed == (t == 2));
                expect(trk.steps[0].trig);
                expectWithinAbsoluteError(trk.steps[0].velocity, 0.8f, 0.01f);
                expectEquals((int) trk.steps[0].notes.size(), 1);
                expectEquals(trk.steps[0].notes[0].pitch, 60 + t);
            }
        }

        // -------------------------------------------------------------------
        beginTest("Step with param locks round-trips correctly");
        {
            ProjectData orig;
            auto& pat = orig.patterns[0];
            pat.tracks.resize(1);
            auto& trk = pat.tracks[0];
            trk.steps.resize(4);

            // Step 0: multiple param locks
            auto& s0 = trk.steps[0];
            s0.trig = true;
            s0.paramLocks[10] = 0.75f;
            s0.paramLocks[20] = 0.33f;
            s0.paramLocks[30] = 1.0f;

            // Step 1: no param locks (should stay empty)
            trk.steps[1].trig = true;

            // Step 2: single param lock
            trk.steps[2].trig = true;
            trk.steps[2].paramLocks[42] = 0.5f;

            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            auto& ls0 = loaded.patterns[0].tracks[0].steps[0];
            expectEquals((int) ls0.paramLocks.size(), 3);
            expectWithinAbsoluteError(ls0.paramLocks[10], 0.75f, 0.001f);
            expectWithinAbsoluteError(ls0.paramLocks[20], 0.33f, 0.001f);
            expectWithinAbsoluteError(ls0.paramLocks[30], 1.0f, 0.001f);

            expect(loaded.patterns[0].tracks[0].steps[1].paramLocks.empty());

            auto& ls2 = loaded.patterns[0].tracks[0].steps[2];
            expectEquals((int) ls2.paramLocks.size(), 1);
            expectWithinAbsoluteError(ls2.paramLocks[42], 0.5f, 0.001f);
        }

        // -------------------------------------------------------------------
        beginTest("Step with pressure lane round-trips correctly");
        {
            ProjectData orig;
            auto& pat = orig.patterns[0];
            pat.tracks.resize(1);
            auto& trk = pat.tracks[0];
            trk.steps.resize(2);

            // Step 0: has pressure lane with specific values
            auto& s0 = trk.steps[0];
            s0.trig = true;
            s0.pressureScale = 1.5f;
            s0.notes.push_back({ 60, 0.8f, {} });
            s0.notes.push_back({ 64, 0.7f, {} });
            for (int i = 0; i < 16; ++i)
                s0.notes[0].lane.points.push_back((uint8_t) (i * 17));
            s0.notes[0].lane.shift = 2;

            // Step 1: no pressure lane
            trk.steps[1].trig = true;
            trk.steps[1].notes.push_back({ 60, 0.8f, {} });

            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            auto& ls0 = loaded.patterns[0].tracks[0].steps[0];
            expectWithinAbsoluteError(ls0.pressureScale, 1.5f, 0.001f);
            expectEquals((int) ls0.notes.size(), 2);
            expect(ls0.notes[0].lane == s0.notes[0].lane, "the lane comes back point for point, with its spacing");
            expectEquals((int) ls0.notes[0].lane.shift, 2);
            expect(ls0.notes[1].lane.empty(), "a note without pressure has no lane");

            auto& ls1 = loaded.patterns[0].tracks[0].steps[1];
            expect(ls1.notes[0].lane.empty());
            expectWithinAbsoluteError(ls1.pressureScale, 1.0f, 0.001f);
        }

        // -------------------------------------------------------------------
        beginTest("Step with trig conditions round-trips correctly");
        {
            ProjectData orig;
            auto& pat = orig.patterns[0];
            pat.tracks.resize(1);
            auto& trk = pat.tracks[0];
            trk.steps.resize(4);

            // Always
            trk.steps[0].trig = true;
            trk.steps[0].trigConditionType = TrigCondition::Always;

            // Probability
            trk.steps[1].trig = true;
            trk.steps[1].trigConditionType = TrigCondition::Probability;
            trk.steps[1].trigProbability = 0.75f;

            // A:B pattern (3:4)
            trk.steps[2].trig = true;
            trk.steps[2].trigConditionType = TrigCondition::A_B;
            trk.steps[2].trigA = 3;
            trk.steps[2].trigB = 4;

            // Fill
            trk.steps[3].trig = true;
            trk.steps[3].trigConditionType = TrigCondition::Fill;

            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            auto& lSteps = loaded.patterns[0].tracks[0].steps;

            expectEquals(static_cast<int>(lSteps[0].trigConditionType), static_cast<int>(TrigCondition::Always));

            expectEquals(static_cast<int>(lSteps[1].trigConditionType), static_cast<int>(TrigCondition::Probability));
            expectWithinAbsoluteError(lSteps[1].trigProbability, 0.75f, 0.001f);

            expectEquals(static_cast<int>(lSteps[2].trigConditionType), static_cast<int>(TrigCondition::A_B));
            expectEquals(lSteps[2].trigA, 3);
            expectEquals(lSteps[2].trigB, 4);

            expectEquals(static_cast<int>(lSteps[3].trigConditionType), static_cast<int>(TrigCondition::Fill));
        }

        // -------------------------------------------------------------------
        beginTest("Track with independent length and speed multiplier");
        {
            ProjectData orig;
            auto& pat = orig.patterns[0];
            pat.tracks.resize(4);

            // Each track gets a different length and speed
            int lengths[] = { 7, 16, 32, 64 };
            float speeds[] = { 0.5f, 1.0f, 2.0f, 0.25f };

            for (int t = 0; t < 4; ++t)
            {
                pat.tracks[static_cast<size_t>(t)].length = lengths[t];
                pat.tracks[static_cast<size_t>(t)].speedMultiplier = speeds[t];
                pat.tracks[static_cast<size_t>(t)].steps.resize(static_cast<size_t>(lengths[t]));
            }

            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            auto& lp = loaded.patterns[0];
            expectEquals((int) lp.tracks.size(), 4);

            for (int t = 0; t < 4; ++t)
            {
                expectEquals(lp.tracks[static_cast<size_t>(t)].length, lengths[t]);
                expectWithinAbsoluteError(lp.tracks[static_cast<size_t>(t)].speedMultiplier, speeds[t], 0.001f);
                expectEquals((int) lp.tracks[static_cast<size_t>(t)].steps.size(), lengths[t]);
            }
        }

        // -------------------------------------------------------------------
        beginTest("Multiple patterns with different track counts");
        {
            ProjectData orig;

            // Pattern 0: 2 tracks
            orig.patterns[0].tracks.resize(2);
            orig.patterns[0].swing = 0.15f;
            for (auto& t : orig.patterns[0].tracks)
                t.steps.resize(16);

            // Pattern 1: 8 tracks
            orig.patterns[1].tracks.resize(8);
            orig.patterns[1].swing = 0.5f;
            for (auto& t : orig.patterns[1].tracks)
                t.steps.resize(32);

            // Pattern 7: 1 track with 64 steps
            orig.patterns[7].tracks.resize(1);
            orig.patterns[7].tracks[0].length = 64;
            orig.patterns[7].tracks[0].steps.resize(64);

            // Pattern 15: 16 tracks
            orig.patterns[15].tracks.resize(16);
            for (auto& t : orig.patterns[15].tracks)
                t.steps.resize(8);

            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            expectEquals((int) loaded.patterns[0].tracks.size(), 2);
            expectWithinAbsoluteError(loaded.patterns[0].swing, 0.15f, 0.001f);

            expectEquals((int) loaded.patterns[1].tracks.size(), 8);
            expectWithinAbsoluteError(loaded.patterns[1].swing, 0.5f, 0.001f);
            expectEquals((int) loaded.patterns[1].tracks[0].steps.size(), 32);

            expectEquals((int) loaded.patterns[7].tracks.size(), 1);
            expectEquals(loaded.patterns[7].tracks[0].length, 64);
            expectEquals((int) loaded.patterns[7].tracks[0].steps.size(), 64);

            expectEquals((int) loaded.patterns[15].tracks.size(), 16);
        }

        // -------------------------------------------------------------------
        beginTest("Song mode entries round-trip");
        {
            ProjectData orig;
            orig.song.loop = false;
            orig.song.entries.push_back({ 0, 4, 0 });
            orig.song.entries.push_back({ 2, 2, 3 });
            orig.song.entries.push_back({ 5, 1, -5 });
            orig.song.entries.push_back({ 15, 999, -12 });

            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            expect(!loaded.song.loop);
            expectEquals((int) loaded.song.entries.size(), 4);

            expectEquals(loaded.song.entries[0].patternIndex, 0);
            expectEquals(loaded.song.entries[0].repeatCount, 4);
            expectEquals(loaded.song.entries[0].transposeSemitones, 0);

            expectEquals(loaded.song.entries[1].patternIndex, 2);
            expectEquals(loaded.song.entries[1].repeatCount, 2);
            expectEquals(loaded.song.entries[1].transposeSemitones, 3);

            expectEquals(loaded.song.entries[2].patternIndex, 5);
            expectEquals(loaded.song.entries[2].repeatCount, 1);
            expectEquals(loaded.song.entries[2].transposeSemitones, -5);

            expectEquals(loaded.song.entries[3].patternIndex, 15);
            expectEquals(loaded.song.entries[3].repeatCount, 999);
            expectEquals(loaded.song.entries[3].transposeSemitones, -12);
        }

        // -------------------------------------------------------------------
        beginTest("Custom scale pages round-trip");
        {
            ProjectData orig;

            ScalePage page1;
            page1.name = "Pentatonic";
            page1.rootNote = 48;
            page1.isFactory = false;
            for (int i = 0; i < 16; ++i)
                page1.padNotes[static_cast<size_t>(i)] = 48 + i * 2;

            ScalePage page2;
            page2.name = "Chromatic";
            page2.rootNote = 60;
            page2.isFactory = true;
            for (int i = 0; i < 16; ++i)
                page2.padNotes[static_cast<size_t>(i)] = 60 + i;

            orig.customScalePages.push_back(page1);
            orig.customScalePages.push_back(page2);

            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            expectEquals((int) loaded.customScalePages.size(), 2);

            auto& lp1 = loaded.customScalePages[0];
            expectEquals(juce::String(lp1.name), juce::String("Pentatonic"));
            expectEquals(lp1.rootNote, 48);
            expect(!lp1.isFactory);
            for (int i = 0; i < 16; ++i)
                expectEquals(lp1.padNotes[static_cast<size_t>(i)], 48 + i * 2);

            auto& lp2 = loaded.customScalePages[1];
            expectEquals(juce::String(lp2.name), juce::String("Chromatic"));
            expectEquals(lp2.rootNote, 60);
            expect(lp2.isFactory);
            for (int i = 0; i < 16; ++i)
                expectEquals(lp2.padNotes[static_cast<size_t>(i)], 60 + i);
        }

        // -------------------------------------------------------------------
        beginTest("Per-track arp settings round-trip");
        {
            ProjectData orig;
            auto& pat = orig.patterns[0];
            pat.tracks.resize(2);
            pat.tracks[0].steps.resize(16);
            pat.tracks[1].steps.resize(16);

            // Track 0: complex arp config
            auto& arp0 = pat.tracks[0].arpSettings;
            arp0.mode = 3;          // Random
            arp0.rate = 5;          // ThirtySecond
            arp0.octaveRange = 3;
            arp0.gate = 0.8f;
            arp0.swing = 0.25f;
            arp0.latch = true;
            arp0.enabled = true;
            arp0.strumEnabled = true;
            arp0.strumTime = 45.0f;
            arp0.strumDirection = 2;
            arp0.strumVelCurve = 0.7f;
            arp0.ratchetEnabled = true;
            arp0.ratchetCount = 3;
            arp0.ratchetDecay = 0.4f;
            arp0.noteRepeats = 2;
            arp0.glideTime = 0.5f;
            arp0.humanizeEnabled = true;
            arp0.humanizeAmount = 0.3f;
            arp0.pressureTarget = 1;  // OctaveRange
            arp0.pressureAmount = 0.6f;
            arp0.macroInteraction = 2; // LockWins

            // Track 1: default arp (should round-trip as defaults)

            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            auto& la0 = loaded.patterns[0].tracks[0].arpSettings;
            expectEquals(la0.mode, 3);
            expectEquals(la0.rate, 5);
            expectEquals(la0.octaveRange, 3);
            expectWithinAbsoluteError(la0.gate, 0.8f, 0.001f);
            expectWithinAbsoluteError(la0.swing, 0.25f, 0.001f);
            expect(la0.latch);
            expect(la0.enabled);
            expect(la0.strumEnabled);
            expectWithinAbsoluteError(la0.strumTime, 45.0f, 0.01f);
            expectEquals(la0.strumDirection, 2);
            expectWithinAbsoluteError(la0.strumVelCurve, 0.7f, 0.001f);
            expect(la0.ratchetEnabled);
            expectEquals(la0.ratchetCount, 3);
            expectWithinAbsoluteError(la0.ratchetDecay, 0.4f, 0.001f);
            expectEquals(la0.noteRepeats, 2);
            expectWithinAbsoluteError(la0.glideTime, 0.5f, 0.001f);
            expect(la0.humanizeEnabled);
            expectWithinAbsoluteError(la0.humanizeAmount, 0.3f, 0.001f);
            expectEquals(la0.pressureTarget, 1);
            expectWithinAbsoluteError(la0.pressureAmount, 0.6f, 0.001f);
            expectEquals(la0.macroInteraction, 2);

            // Track 1: defaults survived
            auto& la1 = loaded.patterns[0].tracks[1].arpSettings;
            expectEquals(la1.mode, 0);
            expect(!la1.enabled);
            expect(!la1.latch);
        }

        // -------------------------------------------------------------------
        beginTest("Aftertouch routing round-trip");
        {
            ProjectData orig;

            AftertouchRouteData::RouteEntry r0;
            r0.destination = 1;  // Volume
            r0.depth = 0.75f;
            r0.bipolar = false;

            AftertouchRouteData::RouteEntry r1;
            r1.destination = 2;  // FilterCutoff
            r1.depth = 0.5f;
            r1.bipolar = true;

            orig.aftertouchRouting.routes.push_back(r0);
            orig.aftertouchRouting.routes.push_back(r1);

            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            expectEquals((int) loaded.aftertouchRouting.routes.size(), 2);

            expectEquals(loaded.aftertouchRouting.routes[0].destination, 1);
            expectWithinAbsoluteError(loaded.aftertouchRouting.routes[0].depth, 0.75f, 0.001f);
            expect(!loaded.aftertouchRouting.routes[0].bipolar);

            expectEquals(loaded.aftertouchRouting.routes[1].destination, 2);
            expectWithinAbsoluteError(loaded.aftertouchRouting.routes[1].depth, 0.5f, 0.001f);
            expect(loaded.aftertouchRouting.routes[1].bipolar);
        }

        // -------------------------------------------------------------------
        beginTest("Send bus config round-trip");
        {
            ProjectData orig;

            orig.sendBuses.push_back({ 6, 1.0f });   // ReverbEffect, full
            orig.sendBuses.push_back({ 5, 0.7f });   // DelayEffect, 70%
            orig.sendBuses.push_back({ 0, 0.5f });   // ChorusEffect, 50%

            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            expectEquals((int) loaded.sendBuses.size(), 3);

            expectEquals(loaded.sendBuses[0].effectType, 6);
            expectWithinAbsoluteError(loaded.sendBuses[0].sendLevel, 1.0f, 0.001f);

            expectEquals(loaded.sendBuses[1].effectType, 5);
            expectWithinAbsoluteError(loaded.sendBuses[1].sendLevel, 0.7f, 0.001f);

            expectEquals(loaded.sendBuses[2].effectType, 0);
            expectWithinAbsoluteError(loaded.sendBuses[2].sendLevel, 0.5f, 0.001f);
        }

        // -------------------------------------------------------------------
        beginTest("Step micro-timing and page change lock round-trip");
        {
            ProjectData orig;
            auto& pat = orig.patterns[0];
            pat.tracks.resize(1);
            auto& trk = pat.tracks[0];
            trk.steps.resize(4);

            trk.steps[0].trig = true;
            trk.steps[0].microTiming = -0.5f;
            trk.steps[0].pageChangeLock = 3;

            trk.steps[1].trig = true;
            trk.steps[1].microTiming = 0.25f;
            trk.steps[1].pageChangeLock = -1;

            trk.steps[2].trig = true;
            trk.steps[2].microTiming = 0.5f;
            trk.steps[2].pageChangeLock = 7;

            trk.steps[3].trig = false;
            trk.steps[3].microTiming = 0.0f;

            auto json = orig.toJSON();
            auto loaded = ProjectData::fromJSON(json);

            auto& ls = loaded.patterns[0].tracks[0].steps;
            expectWithinAbsoluteError(ls[0].microTiming, -0.5f, 0.001f);
            expectEquals(ls[0].pageChangeLock, 3);

            expectWithinAbsoluteError(ls[1].microTiming, 0.25f, 0.001f);
            expectEquals(ls[1].pageChangeLock, -1);

            expectWithinAbsoluteError(ls[2].microTiming, 0.5f, 0.001f);
            expectEquals(ls[2].pageChangeLock, 7);

            expectWithinAbsoluteError(ls[3].microTiming, 0.0f, 0.001f);
        }

        // -------------------------------------------------------------------
        beginTest("Full project round-trip via file save/load");
        {
            // Build a complex project with all enriched fields
            ProjectData orig;
            orig.name = "Full RT";
            orig.bpm = 142.0f;
            orig.activePattern = 5;
            orig.masterLevel = 0.9f;

            // Pattern 5: 2 tracks with varied steps
            auto& pat = orig.patterns[5];
            pat.swing = 0.3f;
            pat.tracks.resize(2);

            auto& t0 = pat.tracks[0];
            t0.engineType = EngineType::FM;
            t0.length = 8;
            t0.speedMultiplier = 2.0f;
            t0.steps.resize(8);
            t0.steps[0].trig = true;
            t0.steps[0].velocity = 0.95f;
            t0.steps[0].notes.push_back({ 72, 0.9f });
            t0.steps[0].notes.push_back({ 76, 0.7f });
            t0.steps[0].paramLocks[1] = 0.5f;
            t0.steps[0].microTiming = -0.25f;
            t0.steps[0].trigConditionType = TrigCondition::A_B;
            t0.steps[0].trigA = 2;
            t0.steps[0].trigB = 3;
            t0.steps[0].notes[0].lane.points = { 0, 26, 230 };
            t0.steps[0].pageChangeLock = 2;
            t0.arpSettings.mode = 2;
            t0.arpSettings.enabled = true;
            t0.arpSettings.ratchetEnabled = true;
            t0.arpSettings.ratchetCount = 4;

            auto& t1 = pat.tracks[1];
            t1.engineType = EngineType::Sampler;
            t1.length = 32;
            t1.steps.resize(32);
            t1.soundParams[0] = 0.5f;
            t1.soundParams[1] = 0.75f;

            // Song entries
            orig.song.loop = true;
            orig.song.entries.push_back({ 5, 2, 0 });
            orig.song.entries.push_back({ 0, 4, -3 });

            // Scale pages
            ScalePage sp;
            sp.name = "MyScale";
            sp.rootNote = 36;
            for (int i = 0; i < 16; ++i)
                sp.padNotes[static_cast<size_t>(i)] = 36 + i;
            orig.customScalePages.push_back(sp);

            // Aftertouch
            orig.aftertouchRouting.routes.push_back({ 3, 0.8f, true });

            // Send buses
            orig.sendBuses.push_back({ 6, 1.0f });
            orig.sendBuses.push_back({ 5, 0.6f });

            // Save to temp dir
            juce::File tempDir = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_full_rt_test");
            tempDir.deleteRecursively();
            tempDir.createDirectory();

            auto jsonFile = tempDir.getChildFile("project.json");
            jsonFile.replaceWithText(orig.toJSON());

            // Load back
            auto loadedJson = jsonFile.loadFileAsString();
            auto loaded = ProjectData::fromJSON(loadedJson);

            // Verify everything
            expectEquals(loaded.name, juce::String("Full RT"));
            expectWithinAbsoluteError(loaded.bpm, 142.0f, 0.01f);
            expectEquals(loaded.activePattern, 5);
            expectWithinAbsoluteError(loaded.masterLevel, 0.9f, 0.01f);

            auto& lp = loaded.patterns[5];
            expectWithinAbsoluteError(lp.swing, 0.3f, 0.01f);
            expectEquals((int) lp.tracks.size(), 2);

            auto& lt0 = lp.tracks[0];
            expectEquals(static_cast<int>(lt0.engineType), static_cast<int>(EngineType::FM));
            expectEquals(lt0.length, 8);
            expectWithinAbsoluteError(lt0.speedMultiplier, 2.0f, 0.001f);
            expect(lt0.steps[0].trig);
            expectEquals((int) lt0.steps[0].notes.size(), 2);
            expectEquals(lt0.steps[0].notes[0].pitch, 72);
            expectEquals(lt0.steps[0].notes[1].pitch, 76);
            expectWithinAbsoluteError(lt0.steps[0].paramLocks[1], 0.5f, 0.001f);
            expectWithinAbsoluteError(lt0.steps[0].microTiming, -0.25f, 0.001f);
            expectEquals(static_cast<int>(lt0.steps[0].trigConditionType), static_cast<int>(TrigCondition::A_B));
            expectEquals(lt0.steps[0].trigA, 2);
            expectEquals(lt0.steps[0].trigB, 3);
            expect(lt0.steps[0].notes[0].lane.points == std::vector<uint8_t>({ 0, 26, 230 }));
            expect(lt0.steps[0].notes[1].lane.empty());
            expectEquals(lt0.steps[0].pageChangeLock, 2);
            expectEquals(lt0.arpSettings.mode, 2);
            expect(lt0.arpSettings.enabled);
            expect(lt0.arpSettings.ratchetEnabled);
            expectEquals(lt0.arpSettings.ratchetCount, 4);

            auto& lt1 = lp.tracks[1];
            expectEquals(static_cast<int>(lt1.engineType), static_cast<int>(EngineType::Sampler));
            expectEquals(lt1.length, 32);
            expectWithinAbsoluteError(lt1.soundParams[0], 0.5f, 0.001f);
            expectWithinAbsoluteError(lt1.soundParams[1], 0.75f, 0.001f);

            // Song
            expect(loaded.song.loop);
            expectEquals((int) loaded.song.entries.size(), 2);
            expectEquals(loaded.song.entries[0].patternIndex, 5);
            expectEquals(loaded.song.entries[1].transposeSemitones, -3);

            // Scale pages
            expectEquals((int) loaded.customScalePages.size(), 1);
            expectEquals(juce::String(loaded.customScalePages[0].name), juce::String("MyScale"));
            expectEquals(loaded.customScalePages[0].rootNote, 36);

            // Aftertouch
            expectEquals((int) loaded.aftertouchRouting.routes.size(), 1);
            expectEquals(loaded.aftertouchRouting.routes[0].destination, 3);
            expect(loaded.aftertouchRouting.routes[0].bipolar);

            // Send buses
            expectEquals((int) loaded.sendBuses.size(), 2);
            expectEquals(loaded.sendBuses[0].effectType, 6);
            expectWithinAbsoluteError(loaded.sendBuses[1].sendLevel, 0.6f, 0.001f);

            // Cleanup
            tempDir.deleteRecursively();
        }

        // -------------------------------------------------------------------
        beginTest("Backward compatibility: v1 project without new fields loads with defaults");
        {
            // Simulate a v1 project JSON (no arp, no pressure, no scale pages, etc.)
            juce::String v1Json = R"({
                "version": 1,
                "name": "OldProject",
                "bpm": 120.0,
                "activePattern": 0,
                "clockMode": 0,
                "linkEnabled": false,
                "sampleFolder": "samples",
                "patterns": [{
                    "swing": 0.0,
                    "tracks": [{
                        "engine": 0,
                        "length": 16,
                        "speed": 1.0,
                        "midiCh": 1,
                        "muted": false,
                        "soloed": false,
                        "params": [],
                        "steps": [{
                            "trig": true,
                            "vel": 0.8,
                            "len": 1.0,
                            "micro": 0.0,
                            "trigType": 0,
                            "trigA": 1,
                            "trigB": 2,
                            "trigProb": 1.0,
                            "pageLock": -1,
                            "notes": [{"pitch": 60, "vel": 1.0}]
                        }]
                    }]
                }],
                "song": {"entries": [], "loop": true}
            })";

            auto loaded = ProjectData::fromJSON(v1Json);

            // Core fields load correctly
            expectEquals(loaded.name, juce::String("OldProject"));
            expectWithinAbsoluteError(loaded.bpm, 120.0f, 0.01f);

            // New fields get safe defaults
            expectWithinAbsoluteError(loaded.masterLevel, 0.8f, 0.01f);  // default
            expect(loaded.customScalePages.empty());
            expect(loaded.aftertouchRouting.routes.empty());
            expect(loaded.sendBuses.empty());

            // Track arp settings get defaults
            auto& arp = loaded.patterns[0].tracks[0].arpSettings;
            expectEquals(arp.mode, 0);
            expect(!arp.enabled);
            expectEquals(arp.octaveRange, 1);

            // Steps without pressure lane get default
            auto& step = loaded.patterns[0].tracks[0].steps[0];
            expect(step.notes.empty() || step.notes[0].lane.empty());
            expectWithinAbsoluteError(step.pressureScale, 1.0f, 0.001f);
            expect(step.trig);
            expectEquals((int) step.notes.size(), 1);
        }
    }
};

static ProjectRoundtripTests projectRoundtripTests;
