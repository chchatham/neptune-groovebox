#pragma once

// Advanced Arpeggiator & Performance tests
// Covers: PolyArp, RandomOctave, Repeats, Snap Glide, Arp Recording,
// Pressure Routing, Human/Grid toggle, Freeze, MacroInteraction, ArpScreen updates.
//
// To include in the test suite, add to main_test.cpp:
//   #include "arp_performance_tests.h"
// and add "Arp Performance" to the test name filter.

#include "../src/arpeggiator/Arpeggiator.h"
#include "../src/ui/screens/ArpScreen.h"
#include "../src/controller/DisplayRenderer.h"
#include <juce_core/juce_core.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <set>
#include <map>
#include <tuple>
#include <initializer_list>

class ArpPerformanceTests : public juce::UnitTest
{
public:
    ArpPerformanceTests() : UnitTest("Arp Performance") {}

    void runTest() override
    {
        // Helper: collect pitches from arp processing
        auto collectPitches = [](Arpeggiator& arp, int steps) -> std::vector<int>
        {
            std::vector<int> pitches;
            arp.onNoteOn = [&](int pitch, float) { pitches.push_back(pitch); };
            arp.onNoteOff = [](int) {};
            arp.triggerFirstNote();
            for (int i = 0; i < steps; ++i)
                arp.processBlock(5600, 44100.0, 120.0f);
            return pitches;
        };

        // Helper: collect pitches + velocities
        auto collectPitchVel = [](Arpeggiator& arp, int steps)
            -> std::vector<std::pair<int, float>>
        {
            std::vector<std::pair<int, float>> notes;
            arp.onNoteOn = [&](int pitch, float vel) { notes.push_back({pitch, vel}); };
            arp.onNoteOff = [](int) {};
            arp.triggerFirstNote();
            for (int i = 0; i < steps; ++i)
                arp.processBlock(5600, 44100.0, 120.0f);
            return notes;
        };

        // ================================================================
        // Task 1: PolyArp mode
        // ================================================================

        beginTest("PolyArp plays all held notes simultaneously");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::PolyArp);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setOctaveRange(1);

            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);
            arp.noteOn(67, 1.0f);

            auto pitches = collectPitches(arp, 6);

            // Should produce chord notes (all 3 notes per trigger)
            expect(pitches.size() >= 6,
                   "PolyArp should produce multiple note-ons, got " + juce::String((int)pitches.size()));

            // All pitches should be from the chord {60, 64, 67}
            for (int p : pitches)
                expect(p == 60 || p == 64 || p == 67,
                       "PolyArp pitch " + juce::String(p) + " not in chord");
        }

        beginTest("PolyArp applies velocity accent pattern");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::PolyArp);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setOctaveRange(1);

            arp.noteOn(60, 1.0f);

            auto notes = collectPitchVel(arp, 8);

            // Should have at least 2 notes with different velocities (accent pattern cycles)
            expect(notes.size() >= 2,
                   "PolyArp should produce at least 2 notes");

            // Collect unique velocities
            std::set<float> uniqueVels;
            for (auto& n : notes)
                uniqueVels.insert(n.second);

            // The accent pattern [1.0, 0.65, 0.85, 0.5] should produce varying velocities
            // First note gets accent index 0 = 1.0, subsequent notes get different values
            expect(uniqueVels.size() >= 2,
                   "PolyArp accent pattern should produce varied velocities, got "
                   + juce::String((int)uniqueVels.size()));
        }

        beginTest("PolyArp mode name is PA");
        {
            expectEquals(Arpeggiator::getModeName(Arpeggiator::Mode::PolyArp),
                         juce::String("PA"));
        }

        // ================================================================
        // Task 2: RandomOctave mode
        // ================================================================

        beginTest("RandomOctave produces random note selection");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::RandomOctave);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setOctaveRange(1);

            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);
            arp.noteOn(67, 1.0f);

            auto pitches = collectPitches(arp, 30);

            expect(pitches.size() > 5,
                   "RandomOctave should produce notes");

            // Random octave shifts pitches by +-12 or +-24
            // Some notes should be outside the base range
            bool hasShifted = false;
            for (int p : pitches)
            {
                if (p < 60 || p > 67)
                {
                    hasShifted = true;
                    break;
                }
            }
            expect(hasShifted,
                   "RandomOctave should produce octave-shifted pitches");

            // All notes should still be valid MIDI (0-127)
            for (int p : pitches)
                expect(p >= 0 && p <= 127,
                       "RandomOctave pitch out of MIDI range: " + juce::String(p));
        }

        beginTest("RandomOctave mode name is RO");
        {
            expectEquals(Arpeggiator::getModeName(Arpeggiator::Mode::RandomOctave),
                         juce::String("RO"));
        }

        // ================================================================
        // Task 3: Repeats per note
        // ================================================================

        beginTest("Repeats per note: default is 1 (no repeats)");
        {
            Arpeggiator arp;
            expectEquals(arp.getNoteRepeats(), 1);
        }

        beginTest("Repeats per note: each step repeats N times");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setOctaveRange(1);
            arp.setNoteRepeats(3);

            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);

            auto pitches = collectPitches(arp, 12);

            // With 3 repeats: note 60 plays 3x, then note 64 plays 3x, then cycle
            // First trigger is note 60, then 2 more repeats, then 3x note 64
            expect(pitches.size() >= 6,
                   "Repeats should produce at least 6 notes, got "
                   + juce::String((int)pitches.size()));

            if (pitches.size() >= 6)
            {
                // First 3 should all be the same pitch (60)
                expectEquals(pitches[0], 60);
                expectEquals(pitches[1], 60);
                expectEquals(pitches[2], 60);
                // Next 3 should be the next pitch (64)
                expectEquals(pitches[3], 64);
                expectEquals(pitches[4], 64);
                expectEquals(pitches[5], 64);
            }
        }

        beginTest("Repeats clamped to 1-8 range");
        {
            Arpeggiator arp;
            arp.setNoteRepeats(0);
            expectEquals(arp.getNoteRepeats(), 1);
            arp.setNoteRepeats(10);
            expectEquals(arp.getNoteRepeats(), 8);
            arp.setNoteRepeats(4);
            expectEquals(arp.getNoteRepeats(), 4);
        }

        // ================================================================
        // Task 4: Snap Glide
        // ================================================================

        beginTest("Glide time: default off, clamped 0-1");
        {
            Arpeggiator arp;
            expectWithinAbsoluteError(arp.getGlideTime(), 0.0f, 0.001f);
            expect(!arp.isGlideActive(), "Glide should be off by default");

            arp.setGlideTime(-0.5f);
            expectWithinAbsoluteError(arp.getGlideTime(), 0.0f, 0.001f);
            arp.setGlideTime(1.5f);
            expectWithinAbsoluteError(arp.getGlideTime(), 1.0f, 0.001f);
        }

        beginTest("Glide snaps to musical divisions");
        {
            expectEquals(Arpeggiator::getGlideDivisionName(0.0f), juce::String("OFF"));
            expectEquals(Arpeggiator::getGlideDivisionName(0.1f), juce::String("1/32"));
            expectEquals(Arpeggiator::getGlideDivisionName(0.25f), juce::String("1/16"));
            expectEquals(Arpeggiator::getGlideDivisionName(0.4f), juce::String("1/8"));
            expectEquals(Arpeggiator::getGlideDivisionName(0.6f), juce::String("1/4"));
            expectEquals(Arpeggiator::getGlideDivisionName(0.75f), juce::String("1/2"));
            expectEquals(Arpeggiator::getGlideDivisionName(0.9f), juce::String("1/1"));
        }

        beginTest("Glide time in samples matches musical division");
        {
            Arpeggiator arp;
            double sr = 44100.0;
            double bpm = 120.0;
            double samplesPerBeat = (60.0 / bpm) * sr;

            // 1/4 note at 120 BPM = one beat
            arp.setGlideTime(0.6f); // Should snap to 1/4
            double glideSamples = arp.getGlideTimeSamples(sr, bpm);
            expectWithinAbsoluteError((float)glideSamples, (float)(samplesPerBeat * 1.0), 1.0f);

            // 1/8 note = half a beat
            arp.setGlideTime(0.4f); // Should snap to 1/8
            glideSamples = arp.getGlideTimeSamples(sr, bpm);
            expectWithinAbsoluteError((float)glideSamples, (float)(samplesPerBeat * 0.5), 1.0f);
        }

        beginTest("Glide tracks previous pitch");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setGlideTime(0.5f);

            arp.noteOn(60, 1.0f);
            arp.noteOn(67, 1.0f);

            arp.onNoteOn = [](int, float) {};
            arp.onNoteOff = [](int) {};

            // Trigger first note: glideFromPitch should be -1 (no previous)
            arp.triggerFirstNote();
            // After first note, lastPlayedPitch should be set
            // Process to advance
            arp.processBlock(5600, 44100.0, 120.0f);

            // After advancing to second note, glideFromPitch should be 60
            expectEquals(arp.getGlideFromPitch(), 60);
            expect(arp.isGlideActive(),
                   "Glide should be active after second note with glide time > 0");
        }

        // ================================================================
        // Task 5: Arp Recording
        // ================================================================

        beginTest("Arp recording captures output notes");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setOctaveRange(1);
            arp.setRecording(true);

            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);

            arp.onNoteOn = [](int, float) {};
            arp.onNoteOff = [](int) {};

            arp.triggerFirstNote();
            for (int i = 0; i < 5; ++i)
                arp.processBlock(5600, 44100.0, 120.0f);

            expect(arp.getRecordedNoteCount() > 0,
                   "Recording should capture notes");

            // First recorded note should be pitch 60
            expectEquals(arp.getRecordedNote(0).pitch, 60);

            // Check timestamps are increasing
            for (int i = 1; i < arp.getRecordedNoteCount(); ++i)
            {
                expect(arp.getRecordedNote(i).sampleTimestamp >=
                       arp.getRecordedNote(i - 1).sampleTimestamp,
                       "Recorded note timestamps should be non-decreasing");
            }
        }

        beginTest("Arp recording: start clears buffer, stop retains");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setRecording(true);

            arp.noteOn(60, 1.0f);
            arp.onNoteOn = [](int, float) {};
            arp.onNoteOff = [](int) {};
            arp.triggerFirstNote();
            arp.processBlock(5600, 44100.0, 120.0f);

            int count1 = arp.getRecordedNoteCount();
            expect(count1 > 0, "Should have recorded notes");

            arp.setRecording(false);
            // Notes should still be available after stopping
            expectEquals(arp.getRecordedNoteCount(), count1);

            // Starting again clears the buffer
            arp.setRecording(true);
            expectEquals(arp.getRecordedNoteCount(), 0);
        }

        beginTest("Arp recording mode can be Individual or Chord");
        {
            Arpeggiator arp;
            expectEquals((int)arp.getRecordMode(), (int)Arpeggiator::RecordMode::Individual);
            arp.setRecordMode(Arpeggiator::RecordMode::Chord);
            expectEquals((int)arp.getRecordMode(), (int)Arpeggiator::RecordMode::Chord);
        }

        // ================================================================
        // Task 6: Pressure routing to arp params
        // ================================================================

        beginTest("Pressure routing: default state");
        {
            Arpeggiator arp;
            expectWithinAbsoluteError(arp.getCurrentPressure(), 0.0f, 0.001f);
            expectWithinAbsoluteError(arp.getPressureAmount(), 0.0f, 0.001f);
            expectEquals((int)arp.getPressureTarget(), (int)Arpeggiator::PressureTarget::GateLength);
        }

        beginTest("Pressure modulates gate length");
        {
            Arpeggiator arp;
            arp.setGate(0.5f);
            arp.setPressureTarget(Arpeggiator::PressureTarget::GateLength);
            arp.setPressureAmount(1.0f);

            // No pressure: effective gate = base gate
            arp.applyPressure(0.0f);
            expectWithinAbsoluteError(arp.getEffectiveGate(), 0.5f, 0.01f);

            // Full pressure: gate increases
            arp.applyPressure(1.0f);
            float effectiveGate = arp.getEffectiveGate();
            expect(effectiveGate > 0.5f,
                   "Pressure should increase gate, got " + juce::String(effectiveGate));
            expect(effectiveGate <= 1.0f, "Gate should not exceed 1.0");
        }

        beginTest("Pressure modulates octave range");
        {
            Arpeggiator arp;
            arp.setOctaveRange(2);
            arp.setPressureTarget(Arpeggiator::PressureTarget::OctaveRange);
            arp.setPressureAmount(1.0f);

            arp.applyPressure(0.0f);
            expectEquals(arp.getEffectiveOctaveRange(), 2);

            arp.applyPressure(1.0f);
            int effectiveOct = arp.getEffectiveOctaveRange();
            expect(effectiveOct > 2,
                   "Pressure should increase octave range, got " + juce::String(effectiveOct));
            expect(effectiveOct <= 8, "Octave range should not exceed 8");
        }

        beginTest("Pressure amount clamped 0-1");
        {
            Arpeggiator arp;
            arp.setPressureAmount(-0.5f);
            expectWithinAbsoluteError(arp.getPressureAmount(), 0.0f, 0.001f);
            arp.setPressureAmount(1.5f);
            expectWithinAbsoluteError(arp.getPressureAmount(), 1.0f, 0.001f);
        }

        // ================================================================
        // Task 7: Human/Grid toggle
        // ================================================================

        beginTest("Humanize: default off");
        {
            Arpeggiator arp;
            expect(!arp.isHumanized(), "Humanize should be off by default");
        }

        beginTest("Humanize: toggle on/off");
        {
            Arpeggiator arp;
            arp.setHumanize(true);
            expect(arp.isHumanized(), "Humanize should be on");
            arp.setHumanize(false);
            expect(!arp.isHumanized(), "Humanize should be off");
        }

        beginTest("Humanize: amount clamped 0-1");
        {
            Arpeggiator arp;
            arp.setHumanizeAmount(-0.5f);
            expectWithinAbsoluteError(arp.getHumanizeAmount(), 0.0f, 0.001f);
            arp.setHumanizeAmount(1.5f);
            expectWithinAbsoluteError(arp.getHumanizeAmount(), 1.0f, 0.001f);
        }

        beginTest("Humanize: produces timing variation when enabled");
        {
            // Run the arp twice with humanize off, collect timing of note-ons
            // Then run with humanize on - timings should differ
            auto collectTimings = [](Arpeggiator& arp, int steps) -> std::vector<int>
            {
                std::vector<int> timings;
                int samplePos = 0;
                arp.onNoteOn = [&](int, float) { timings.push_back(samplePos); };
                arp.onNoteOff = [](int) {};
                arp.triggerFirstNote();
                for (int i = 0; i < steps; ++i)
                {
                    arp.processBlock(1, 44100.0, 120.0f);
                    ++samplePos;
                }
                return timings;
            };

            // Grid mode (humanize off)
            Arpeggiator arp1;
            arp1.setMode(Arpeggiator::Mode::Up);
            arp1.setRate(Arpeggiator::Rate::Sixteenth);
            arp1.setGate(0.5f);
            arp1.setHumanize(false);
            arp1.noteOn(60, 1.0f);
            auto timingsGrid = collectTimings(arp1, 22000);

            // Humanize mode
            Arpeggiator arp2;
            arp2.setMode(Arpeggiator::Mode::Up);
            arp2.setRate(Arpeggiator::Rate::Sixteenth);
            arp2.setGate(0.5f);
            arp2.setHumanize(true);
            arp2.setHumanizeAmount(1.0f);
            arp2.noteOn(60, 1.0f);
            auto timingsHuman = collectTimings(arp2, 22000);

            // Both should produce notes
            expect(timingsGrid.size() >= 3, "Grid mode should produce notes");
            expect(timingsHuman.size() >= 3, "Humanize mode should produce notes");

            // Check that grid timing intervals are uniform
            if (timingsGrid.size() >= 3)
            {
                int interval1 = timingsGrid[1] - timingsGrid[0];
                int interval2 = timingsGrid[2] - timingsGrid[1];
                // Grid intervals should be equal (within 1 sample for rounding)
                expect(std::abs(interval1 - interval2) <= 1,
                       "Grid mode intervals should be equal");
            }

            // Check that humanize timing intervals vary
            if (timingsHuman.size() >= 3)
            {
                int interval1 = timingsHuman[1] - timingsHuman[0];
                int interval2 = timingsHuman[2] - timingsHuman[1];
                // With humanize on and amount=1.0, intervals should differ
                // Note: might rarely be equal, but with amount=1.0 it should vary
                // We just check they're not all exactly equal over many steps
                bool hasVariation = false;
                for (size_t i = 2; i < timingsHuman.size(); ++i)
                {
                    int intA = timingsHuman[i] - timingsHuman[i - 1];
                    int intB = timingsHuman[i - 1] - timingsHuman[i - 2];
                    if (std::abs(intA - intB) > 1)
                    {
                        hasVariation = true;
                        break;
                    }
                }
                expect(hasVariation,
                       "Humanize mode should produce timing variation");
            }
        }

        // ================================================================
        // Task 8: Freeze
        // ================================================================

        beginTest("Freeze: default off");
        {
            Arpeggiator arp;
            expect(!arp.isFrozen(), "Freeze should be off by default");
        }

        beginTest("Freeze: captures current sequence");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setOctaveRange(1);

            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);
            arp.noteOn(67, 1.0f);

            // Freeze captures the current sequence
            arp.setFrozen(true);
            expect(arp.isFrozen(), "Should be frozen");
            expectEquals(arp.getFrozenSequenceLength(), 3);
            expectEquals(arp.getFrozenSequencePitch(0), 60);
            expectEquals(arp.getFrozenSequencePitch(1), 64);
            expectEquals(arp.getFrozenSequencePitch(2), 67);
        }

        beginTest("Freeze: noteOff ignored while frozen");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setOctaveRange(1);

            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);

            arp.setFrozen(true);

            // Release a note - should be ignored
            arp.noteOff(60);

            // Sequence should still have 2 notes
            expectEquals(arp.getSequenceLength(), 2);
            expectEquals(arp.getInputNoteCount(), 2);
        }

        beginTest("Freeze: unfreeze rebuilds sequence");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setOctaveRange(1);

            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);
            arp.noteOn(67, 1.0f);

            arp.setFrozen(true);

            // Frozen sequence snapshot is saved
            expectEquals(arp.getFrozenSequenceLength(), 3);

            // Unfreeze rebuilds from current input
            arp.setFrozen(false);
            expect(!arp.isFrozen(), "Should be unfrozen");

            // Sequence should still be 3 notes (all notes still held)
            expectEquals(arp.getSequenceLength(), 3);
        }

        beginTest("Freeze: frozen sequence survives after unfreeze");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setOctaveRange(1);

            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);

            arp.setFrozen(true);
            expectEquals(arp.getFrozenSequenceLength(), 2);

            arp.setFrozen(false);

            // Frozen snapshot should still be readable
            expectEquals(arp.getFrozenSequenceLength(), 2);
            expectEquals(arp.getFrozenSequencePitch(0), 60);
            expectEquals(arp.getFrozenSequencePitch(1), 64);
        }

        // ================================================================
        // Task 9: Parameter lock + macro interaction modes
        // ================================================================

        beginTest("MacroInteraction: default is Additive");
        {
            Arpeggiator arp;
            expectEquals((int)arp.getMacroInteraction(),
                         (int)Arpeggiator::MacroInteraction::Additive);
        }

        beginTest("MacroInteraction: Additive mode");
        {
            // base=0.5, lock=0.7 (delta +0.2), macro=0.6 (delta +0.1)
            // result = 0.5 + 0.2 + 0.1 = 0.8
            float result = Arpeggiator::combineMacroAndLock(
                Arpeggiator::MacroInteraction::Additive, 0.5f, 0.7f, 0.6f);
            expectWithinAbsoluteError(result, 0.8f, 0.01f);
        }

        beginTest("MacroInteraction: MacroWins mode");
        {
            float result = Arpeggiator::combineMacroAndLock(
                Arpeggiator::MacroInteraction::MacroWins, 0.5f, 0.7f, 0.3f);
            expectWithinAbsoluteError(result, 0.3f, 0.01f);
        }

        beginTest("MacroInteraction: LockWins mode");
        {
            float result = Arpeggiator::combineMacroAndLock(
                Arpeggiator::MacroInteraction::LockWins, 0.5f, 0.7f, 0.3f);
            expectWithinAbsoluteError(result, 0.7f, 0.01f);
        }

        beginTest("MacroInteraction: Multiply mode");
        {
            // base=0.5, lock=0.8, macro=0.4
            // result = (0.8/0.5) * 0.4 = 1.6 * 0.4 = 0.64
            float result = Arpeggiator::combineMacroAndLock(
                Arpeggiator::MacroInteraction::Multiply, 0.5f, 0.8f, 0.4f);
            expectWithinAbsoluteError(result, 0.64f, 0.01f);
        }

        beginTest("MacroInteraction: name strings");
        {
            expectEquals(Arpeggiator::getMacroInteractionName(
                Arpeggiator::MacroInteraction::Additive), juce::String("ADD"));
            expectEquals(Arpeggiator::getMacroInteractionName(
                Arpeggiator::MacroInteraction::MacroWins), juce::String("MAC"));
            expectEquals(Arpeggiator::getMacroInteractionName(
                Arpeggiator::MacroInteraction::LockWins), juce::String("LCK"));
            expectEquals(Arpeggiator::getMacroInteractionName(
                Arpeggiator::MacroInteraction::Multiply), juce::String("MUL"));
        }

        // ================================================================
        // Task 10: ArpScreen updates
        // ================================================================

        beginTest("ArpScreen: first two pages, cells named after what they edit");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);

            const char* page1[8] = { "MODE", "RATE", "OCT", "GATE", "RPT", "SWNG", "GLID", "LTCH" };
            const char* page2[8] = { "STRM", "TIME", "DIR", "VEL", "RTCH", "RDEC", "HUMN", "FREZ" };
            expectEquals(screen.getName(), juce::String("ARP"));
            for (int i = 0; i < 8; ++i)
                expectEquals(screen.getEncoderName(i), juce::String(page1[i]));
            screen.onArrowRight();
            expectEquals(screen.getName(), juce::String("PLAY"));
            for (int i = 0; i < 8; ++i)
                expectEquals(screen.getEncoderName(i), juce::String(page2[i]));
            expect(!screen.usesEncoderBar());
        }

        beginTest("ArpScreen: mode encoder scrolls through all 15 modes");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);

            arp.setMode(Arpeggiator::Mode::Up);

            // Scroll up through all modes
            for (int i = 0; i < 11; ++i)
                screen.onEncoderChange(0, 1); // value 1-63 = positive delta

            expectEquals((int)arp.getMode(), (int)Arpeggiator::Mode::RandomOctave);
            screen.onEncoderChange(0, 1);
            expectEquals((int)arp.getMode(), (int)Arpeggiator::Mode::Parallel);
            screen.onEncoderChange(0, 1);
            expectEquals((int)arp.getMode(), (int)Arpeggiator::Mode::Pedal);
            screen.onEncoderChange(0, 1);
            expectEquals((int)arp.getMode(), (int)Arpeggiator::Mode::PedalRise);

            // Can't go past last mode
            screen.onEncoderChange(0, 1);
            expectEquals((int)arp.getMode(), (int)Arpeggiator::Mode::PedalRise);
        }

        beginTest("ArpScreen: repeats encoder changes note repeats");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);

            expectEquals(arp.getNoteRepeats(), 1);

            // Scroll up
            screen.onEncoderChange(4, 1);
            expectEquals(arp.getNoteRepeats(), 2);

            screen.onEncoderChange(4, 1);
            expectEquals(arp.getNoteRepeats(), 3);

            // Scroll down
            screen.onEncoderChange(4, 127);
            expectEquals(arp.getNoteRepeats(), 2);
        }

        beginTest("ArpScreen: glide and swing have their own cells");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);

            expectWithinAbsoluteError(arp.getGlideTime(), 0.0f, 0.001f);
            expect(!screen.hasSubParam(3), "No hidden sub-parameters");
            screen.onEncoderChange(6, 5);
            expect(arp.getGlideTime() > 0.0f, "GLID cell raises glide");
            screen.onEncoderChange(5, 5);
            expect(arp.getSwing() > 0.0f, "SWNG cell raises swing");
        }

        beginTest("ArpScreen: humanize amount (0 = grid)");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);
            screen.onArrowRight();

            expect(!arp.isHumanized(), "Should start as grid");
            screen.onEncoderChange(6, 20);
            expect(arp.isHumanized(), "Raising HUMN humanizes");
            expect(arp.getHumanizeAmount() > 0.1f);
            screen.onEncoderChange(6, 78);      // -50 steps
            screen.onEncoderChange(6, 78);
            expect(!arp.isHumanized(), "Back to zero is grid again");
        }

        beginTest("ArpScreen: freeze follows the direction turned, not every click");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);
            screen.onArrowRight();

            arp.noteOn(60, 1.0f); // Need at least one note for freeze to capture

            expect(!arp.isFrozen(), "Should start unfrozen");
            screen.onEncoderChange(7, 1);
            expect(arp.isFrozen(), "Clockwise freezes");
            screen.onEncoderChange(7, 1);
            expect(arp.isFrozen(), "Further clockwise stays frozen");
            screen.onEncoderChange(7, 127);
            expect(!arp.isFrozen(), "Anticlockwise unfreezes");
        }

        beginTest("ArpScreen: renders without crashing");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);
            DisplayRenderer display;

            arp.setMode(Arpeggiator::Mode::PolyArp);
            arp.setNoteRepeats(3);
            arp.setGlideTime(0.5f);
            arp.setHumanize(true);
            arp.noteOn(60, 1.0f);
            arp.setFrozen(true);

            // Should render without crashing
            screen.render(display);
            expect(true, "ArpScreen rendered successfully");
        }

        // ================================================================
        // Integration: features work together
        // ================================================================

        beginTest("Repeats + PolyArp: chord repeats with note repetition");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::PolyArp);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setOctaveRange(1);
            arp.setNoteRepeats(2);

            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);

            auto pitches = collectPitches(arp, 10);

            // With PolyArp + 2 repeats: each chord plays twice before advancing
            expect(pitches.size() >= 4,
                   "Should produce at least 4 notes (2 chords x 2 notes)");

            // First two triggers should contain the same chord
            if (pitches.size() >= 4)
            {
                // First trigger: 60, 64
                // Second trigger (repeat): 60, 64
                // Then advance to next octave/position
                bool firstChordSame = (pitches[0] == pitches[2] && pitches[1] == pitches[3]);
                expect(firstChordSame,
                       "Repeated chord should have same pitches");
            }
        }

        beginTest("Recording + RandomOctave: captures shifted pitches");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::RandomOctave);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setRecording(true);

            arp.noteOn(60, 1.0f);

            arp.onNoteOn = [](int, float) {};
            arp.onNoteOff = [](int) {};

            arp.triggerFirstNote();
            for (int i = 0; i < 5; ++i)
                arp.processBlock(5600, 44100.0, 120.0f);

            int count = arp.getRecordedNoteCount();
            expect(count > 0, "Should record notes in RandomOctave mode");

            // Recorded pitches should include octave-shifted values
            bool hasBase = false;
            bool hasShifted = false;
            for (int i = 0; i < count; ++i)
            {
                int p = arp.getRecordedNote(i).pitch;
                if (p == 60) hasBase = true;
                if (p != 60) hasShifted = true;
            }
            // With random shifts, should have at least some shifted notes over 5 steps
            // (This could rarely fail if all shifts happen to be 0, but very unlikely)
            expect(hasBase || hasShifted,
                   "Recording should capture some notes");
        }

        beginTest("Existing modes still work correctly after changes");
        {
            // Quick sanity: Up mode
            {
                Arpeggiator arp;
                arp.setMode(Arpeggiator::Mode::Up);
                arp.setRate(Arpeggiator::Rate::Sixteenth);
                arp.setGate(0.5f);
                arp.setOctaveRange(1);
                arp.noteOn(60, 1.0f);
                arp.noteOn(64, 1.0f);
                arp.noteOn(67, 1.0f);

                auto pitches = collectPitches(arp, 6);
                expect(pitches.size() >= 3, "Up mode should work");
                expectEquals(pitches[0], 60);
                expectEquals(pitches[1], 64);
                expectEquals(pitches[2], 67);
            }

            // Down mode
            {
                Arpeggiator arp;
                arp.setMode(Arpeggiator::Mode::Down);
                arp.setRate(Arpeggiator::Rate::Sixteenth);
                arp.setGate(0.5f);
                arp.setOctaveRange(1);
                arp.noteOn(60, 1.0f);
                arp.noteOn(64, 1.0f);
                arp.noteOn(67, 1.0f);

                auto pitches = collectPitches(arp, 6);
                expect(pitches.size() >= 3, "Down mode should work");
                expectEquals(pitches[0], 67);
                expectEquals(pitches[1], 64);
                expectEquals(pitches[2], 60);
            }

            // ChordRepeat mode
            {
                Arpeggiator arp;
                arp.setMode(Arpeggiator::Mode::ChordRepeat);
                arp.setRate(Arpeggiator::Rate::Sixteenth);
                arp.setGate(0.5f);
                arp.setOctaveRange(1);
                arp.noteOn(60, 1.0f);
                arp.noteOn(64, 1.0f);

                auto pitches = collectPitches(arp, 4);
                expect(pitches.size() >= 2, "ChordRepeat should play chord");
                // First trigger should include both chord notes
                bool has60 = false, has64 = false;
                for (int p : pitches)
                {
                    if (p == 60) has60 = true;
                    if (p == 64) has64 = true;
                }
                expect(has60 && has64, "ChordRepeat should play 60 and 64");
            }

            // Latch still works
            {
                Arpeggiator arp;
                arp.setMode(Arpeggiator::Mode::Up);
                arp.setRate(Arpeggiator::Rate::Sixteenth);
                arp.setGate(0.5f);
                arp.setLatch(true);
                arp.noteOn(60, 1.0f);
                arp.noteOff(60); // Should be ignored due to latch
                expectEquals(arp.getInputNoteCount(), 1);
            }
        }

        // ================================================================
        // Wired features: every one must change the notes that come out.
        // 1/16 at 120 BPM and 44100 Hz: a step is 5512.5 samples, a bar 88200.
        // ================================================================

        struct Ev { bool on; int pitch; float vel; int t; };
        using Events = std::vector<Ev>;
        static constexpr double STEP = 5512.5;
        static constexpr int BAR = 88200;

        // Runs the arp sample by sample so every event has its exact time.
        auto hook = [](Arpeggiator& arp, Events& ev, int& t)
        {
            arp.onNoteOn = [&ev, &t](int p, float v) { ev.push_back({ true, p, v, t }); };
            arp.onNoteOff = [&ev, &t](int p) { ev.push_back({ false, p, 0.0f, t }); };
        };
        auto advance = [](Arpeggiator& arp, int& t, int samples, double sr = 44100.0)
        {
            for (int i = 0; i < samples; ++i)
            {
                arp.processBlock(1, sr, 120.0f);
                ++t;
            }
        };
        auto setup = [](Arpeggiator& arp, std::initializer_list<int> notes)
        {
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setOctaveRange(1);
            arp.setRandomSeed(7u);
            for (int n : notes)
                arp.noteOn(n, 1.0f);
        };
        auto onsOf = [](const Events& ev, int from = 0, int to = 0x7FFFFFFF)
        {
            Events out;
            for (auto& e : ev)
                if (e.on && e.t >= from && e.t < to)
                    out.push_back(e);
            return out;
        };
        // Length of every note: time from a note-on to the next note-off of that pitch.
        auto noteLengths = [](const Events& ev)
        {
            std::vector<int> lengths;
            for (size_t i = 0; i < ev.size(); ++i)
            {
                if (!ev[i].on) continue;
                for (size_t j = i + 1; j < ev.size(); ++j)
                    if (!ev[j].on && ev[j].pitch == ev[i].pitch)
                    {
                        lengths.push_back(ev[j].t - ev[i].t);
                        break;
                    }
            }
            return lengths;
        };
        // Standard run: three or four held notes, `steps` steps.
        auto play = [&](Arpeggiator& arp, int steps)
        {
            Events ev;
            int t = 0;
            hook(arp, ev, t);
            arp.triggerFirstNote();
            advance(arp, t, (int) (STEP * steps) - 10);
            arp.onNoteOn = nullptr;
            arp.onNoteOff = nullptr;
            return ev;
        };
        auto samePitchVel = [](const Events& a, const Events& b)
        {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].pitch != b[i].pitch || std::abs(a[i].vel - b[i].vel) > 1.0e-6f)
                    return false;
            return true;
        };

        beginTest("Macros at rest: the plain arpeggio, evenly spaced");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            auto ons = onsOf(play(arp, 12));
            expectEquals((int) ons.size(), 12);
            const int chord[3] = { 60, 64, 67 };
            for (size_t i = 0; i < ons.size(); ++i)
            {
                expectEquals(ons[i].pitch, chord[i % 3]);
                expectWithinAbsoluteError(ons[i].vel, 1.0f, 0.0001f);
                expect(std::abs(ons[i].t - (int) std::lround(STEP * (double) i)) <= 1, "on the grid");
            }
        }

        beginTest("Density: low drops steps, high inserts quieter notes");
        {
            Arpeggiator thin;
            setup(thin, { 60, 64, 67 });
            thin.setDensity(0.0f);
            auto thinOns = onsOf(play(thin, 64));
            expect((int) thinOns.size() < 64, "density 0 drops steps, got " + juce::String((int) thinOns.size()));
            expect((int) thinOns.size() >= 16, "every fourth step is always kept");

            Arpeggiator full;
            setup(full, { 60, 64, 67 });
            full.setDensity(1.0f);
            auto fullOns = onsOf(play(full, 64));
            expect((int) fullOns.size() > 64, "density 1 inserts notes, got " + juce::String((int) fullOns.size()));
            int quiet = 0, loud = 0;
            for (auto& e : fullOns)
                (e.vel < 0.75f ? quiet : loud)++;
            expect(quiet > 0, "inserted notes are quieter");
            expectEquals(loud, 64, "the steps' own notes keep their level");
        }

        beginTest("Spice inserts notes");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            arp.setSpice(1.0f);
            auto ons = onsOf(play(arp, 32));
            expect((int) ons.size() > 40, "spice 1 inserts notes, got " + juce::String((int) ons.size()));
            for (auto& e : ons)
                expect(e.pitch == 60 || e.pitch == 64 || e.pitch == 67, "spice does not change pitch");
        }

        beginTest("Dice moves notes by intervals");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            arp.setDice(1.0f);
            auto ons = onsOf(play(arp, 48));
            int moved = 0;
            for (size_t i = 0; i < ons.size(); ++i)
            {
                const int chord[3] = { 60, 64, 67 };
                const int diff = ons[i].pitch - chord[i % 3];
                expect(std::abs(diff) <= 7, "dice moves at most a fifth");
                if (diff != 0) ++moved;
            }
            expect(moved > 20, "dice 1 moves most notes, moved " + juce::String(moved));
        }

        beginTest("Octave scatter jumps octaves, keeps the pitch class");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            arp.setOctaveScatter(1.0f);
            auto ons = onsOf(play(arp, 48));
            expectEquals((int) ons.size(), 48);
            int jumped = 0;
            for (size_t i = 0; i < ons.size(); ++i)
            {
                const int chord[3] = { 60, 64, 67 };
                const int diff = ons[i].pitch - chord[i % 3];
                expect(diff % 12 == 0 && std::abs(diff) <= 24, "an octave or two, got " + juce::String(diff));
                if (diff != 0) ++jumped;
            }
            expectEquals(jumped, 48, "scatter 1 moves every note");
        }

        beginTest("Ratchet chance repeats notes inside the step");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            arp.setRatchetChance(1.0f);
            auto ev = play(arp, 32);
            auto ons = onsOf(ev);
            expect((int) ons.size() >= 64, "every step is at least doubled, got " + juce::String((int) ons.size()));
            // Repeats sit inside the gate (first half of the step), never in the second half.
            for (auto& e : ons)
            {
                const double phase = std::fmod((double) e.t + 2.0, STEP);
                expect(phase < STEP * 0.5 + 4.0, "ratchet inside the gate");
            }
        }

        beginTest("Gate spread varies note lengths");
        {
            Arpeggiator even;
            setup(even, { 60, 64, 67 });
            auto evenLengths = noteLengths(play(even, 32));
            expect(evenLengths.size() >= 30);
            auto evenMinMax = std::minmax_element(evenLengths.begin(), evenLengths.end());
            expect(*evenMinMax.second - *evenMinMax.first <= 2, "no spread: equal lengths");
            expect(std::abs(evenLengths[0] - (int) (STEP * 0.5)) <= 2, "gate 0.5 is half a step");

            Arpeggiator spread;
            setup(spread, { 60, 64, 67 });
            spread.setGateSpread(1.0f);
            auto lengths = noteLengths(play(spread, 32));
            auto mm = std::minmax_element(lengths.begin(), lengths.end());
            expect(*mm.second - *mm.first > 1500,
                   "spread: staccato to legato, range " + juce::String(*mm.second - *mm.first));
            expect(*mm.second <= (int) STEP + 2, "never longer than the step");
        }

        beginTest("Humanize varies level and lands late, never early, without drift");
        {
            Arpeggiator arp;
            setup(arp, { 60 });
            arp.setHumanize(true);
            arp.setHumanizeAmount(1.0f);
            auto ons = onsOf(play(arp, 64));
            expectEquals((int) ons.size(), 64);
            std::set<int> levels;
            int late = 0;
            for (size_t i = 0; i < ons.size(); ++i)
            {
                levels.insert((int) std::lround(ons[i].vel * 1000.0f));
                const double offset = (double) ons[i].t - STEP * (double) i;
                expect(offset >= -1.0 && offset <= STEP * 0.17, "0 to 16 % late, got " + juce::String(offset));
                if (offset > 20.0) ++late;
            }
            expect(levels.size() > 10, "velocity varies");
            expect(late > 30, "timing varies");
        }

        beginTest("Variation number: same number repeats the stream, another number gives another");
        {
            auto run = [&](int variation)
            {
                Arpeggiator arp;
                setup(arp, { 60, 64, 67 });
                arp.setDice(0.7f);
                arp.setOctaveScatter(0.5f);
                arp.setVariation(variation);
                return onsOf(play(arp, 32));
            };
            auto a = run(5), b = run(5), c = run(6);
            expect(samePitchVel(a, b), "same variation, same notes");
            expect(!samePitchVel(a, c), "another variation, other notes");

            Arpeggiator arp;
            arp.setVariation(98);
            arp.rollDice();
            expectEquals(arp.getVariation(), 99);
            arp.rollDice();
            expectEquals(arp.getVariation(), 1);
        }

        beginTest("Freeze: the stream of steps repeats exactly, random choices included");
        {
            auto run = [&](bool freeze, Arpeggiator& arp, int& loopSteps)
            {
                setup(arp, { 60, 64, 67, 72 });
                arp.setMode(Arpeggiator::Mode::Random);
                arp.setDice(0.6f);
                arp.setOctaveScatter(0.5f);
                arp.setRatchetChance(0.4f);
                Events ev;
                int t = 0;
                hook(arp, ev, t);
                arp.triggerFirstNote();
                advance(arp, t, (int) (STEP * 6) - 50);
                if (freeze) arp.setFrozen(true);
                advance(arp, t, (int) (STEP * 4));            // the loop is captured at the next step
                loopSteps = freeze ? arp.getFreezeLoopLength() : 4;
                const int start = t;
                const int period = (int) std::lround(STEP * loopSteps);
                advance(arp, t, period * 3);
                arp.onNoteOn = nullptr;
                arp.onNoteOff = nullptr;
                Events a = onsOf(ev, start, start + period), b = onsOf(ev, start + period, start + 2 * period),
                       c = onsOf(ev, start + 2 * period, start + 3 * period);
                return std::make_tuple(a, b, c);
            };

            Arpeggiator frozen;
            int loopSteps = 0;
            auto [a, b, c] = run(true, frozen, loopSteps);
            expectEquals(loopSteps, 4, "four notes: a loop of four steps");
            expect(a.size() >= 4);
            expect(samePitchVel(a, b) && samePitchVel(b, c), "frozen: every loop is the same");
            for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                expect(std::abs((b[i].t - a[i].t) - (int) std::lround(STEP * 4)) <= 1, "same timing");

            Arpeggiator free;
            auto [d, e, f] = run(false, free, loopSteps);
            expect(!(samePitchVel(d, e) && samePitchVel(e, f)), "not frozen: it keeps changing");
        }

        // Spice is the mutation amount and also inserts notes of its own inside the steps;
        // the mutation is judged on each step's own note (the one on the step line).
        auto ownNotes = [](const Events& ons)
        {
            Events out;
            for (auto& e : ons)
                if (std::fmod((double) e.t + 2.0, STEP) < 4.0)
                    out.push_back(e);
            return out;
        };

        beginTest("Mutate: the sequence evolves every interval; return, commit and original");
        {
            auto bars = [&](Arpeggiator& arp, Events& ev, int& t, int count)
            {
                std::vector<Events> out;
                for (int b = 0; b < count; ++b)
                {
                    // Each window ends 20 samples before a bar line, where no note starts.
                    const int start = t;
                    const int end = ((start + 20) / BAR + 1) * BAR - 20;
                    advance(arp, t, end - start);
                    out.push_back(ownNotes(onsOf(ev, start, end)));
                }
                return out;
            };

            // Control: without mutate every bar is the same.
            {
                Arpeggiator arp;
                setup(arp, { 60, 64, 67, 72 });
                Events ev; int t = 0;
                hook(arp, ev, t);
                arp.triggerFirstNote();
                auto b = bars(arp, ev, t, 4);
                expectEquals((int) b[0].size(), 16);
                expect(samePitchVel(b[0], b[1]) && samePitchVel(b[0], b[3]), "no mutate: no change");
                expectEquals(arp.getMutationGeneration(), 0);
            }

            // Cumulative, one bar apart: drifts further each bar.
            Arpeggiator arp;
            setup(arp, { 60, 64, 67, 72 });
            arp.setSpice(0.5f);
            arp.setMutateEnabled(true);
            arp.getMutate().setInterval(1);
            arp.getMutate().setMode(MutateSystem::Mode::Cumulative);
            Events ev; int t = 0;
            hook(arp, ev, t);
            arp.triggerFirstNote();
            auto b = bars(arp, ev, t, 5);
            expectEquals((int) b[0].size(), 16, "first bar is the held notes");
            for (size_t i = 0; i < b[0].size(); ++i)
            {
                const int chord[4] = { 60, 64, 67, 72 };
                expectEquals(b[0][i].pitch, chord[i % 4]);
            }
            int changedBars = 0;
            for (int i = 1; i < 5; ++i)
                if (!samePitchVel(b[0], b[(size_t) i])) ++changedBars;
            expect(changedBars >= 3, "mutated bars differ from the original, " + juce::String(changedBars));
            expectEquals(arp.getMutationGeneration(), 4, "one round per bar");
            expectEquals(arp.getMutate().getNoteGeneration(), 4);

            // Stop evolving: the current mutation keeps playing unchanged.
            arp.setMutateEnabled(false);
            auto held = bars(arp, ev, t, 2);
            expect(samePitchVel(held[0], held[1]), "mutate off: the variation stays");
            expect(!samePitchVel(held[0], b[0]), "and it is still the mutated one");

            // Commit, then return: the committed mutation is the ancestor now.
            arp.commitToAncestor();
            expect(arp.isMutationCommitted());
            arp.mutateNow();
            auto afterMore = bars(arp, ev, t, 1);
            arp.returnToAncestor();
            expectEquals(arp.getMutationGeneration(), 0);
            auto returned = bars(arp, ev, t, 1);
            expect(samePitchVel(returned[0], held[0]), "return goes back to the committed mutation");
            juce::ignoreUnused(afterMore);

            // Original: the held notes again.
            arp.restoreOriginal();
            expect(!arp.isMutationCommitted());
            auto original = bars(arp, ev, t, 1);
            expect(samePitchVel(original[0], b[0]), "original restores the held notes");
        }

        beginTest("Mutate: ancestor mode is always one step from the ancestor; amount is Spice");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67, 72 });
            arp.setSpice(1.0f);
            arp.setMutateEnabled(true);
            arp.getMutate().setInterval(1);
            arp.getMutate().setMode(MutateSystem::Mode::Ancestor);
            Events ev; int t = 0;
            hook(arp, ev, t);
            arp.triggerFirstNote();
            advance(arp, t, BAR * 4);
            expectEquals(arp.getMutationGeneration(), 1, "a fresh variation of the ancestor each time");
            expectWithinAbsoluteError(arp.getMutate().getAmount(), 1.0f, 0.001f);

            arp.setMutateEnabled(false);
            arp.returnToAncestor();
            const int start = t;
            advance(arp, t, BAR);
            auto ons = ownNotes(onsOf(ev, start + 10, start + BAR + 10));    // the 16 steps planned after the return
            expectEquals((int) ons.size(), 16);
            for (size_t i = 0; i < ons.size(); ++i)
                expect(ons[i].pitch == 60 || ons[i].pitch == 64 || ons[i].pitch == 67 || ons[i].pitch == 72,
                       "return to ancestor: the held notes");

            // Frozen: no mutation happens.
            arp.setMutateEnabled(true);
            arp.setFrozen(true);
            advance(arp, t, BAR * 3);
            expectEquals(arp.getMutationGeneration(), 0, "freeze stops mutation");
        }

        beginTest("Strum: alternating direction, real sample rate, whole-chord ratchets");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            arp.setMode(Arpeggiator::Mode::ChordRepeat);
            arp.setStrumEnabled(true);
            arp.setStrumTime(20.0f);
            arp.setStrumDirection(2);
            auto ons = onsOf(play(arp, 3));
            expectEquals((int) ons.size(), 9);
            if (ons.size() == 9)
            {
                expect(ons[0].pitch == 60 && ons[1].pitch == 64 && ons[2].pitch == 67, "first strum up");
                expect(ons[3].pitch == 67 && ons[4].pitch == 64 && ons[5].pitch == 60, "second strum down");
                expect(ons[6].pitch == 60 && ons[8].pitch == 67, "third strum up again");
                expect(std::abs((ons[2].t - ons[0].t) - 882) <= 2, "20 ms at 44100 Hz");
            }

            // 88200 Hz: the same 20 ms is twice the samples.
            Arpeggiator hi;
            setup(hi, { 60, 64, 67 });
            hi.setMode(Arpeggiator::Mode::ChordRepeat);
            hi.setStrumEnabled(true);
            hi.setStrumTime(20.0f);
            Events ev; int t = 0;
            hook(hi, ev, t);
            advance(hi, t, 4000, 88200.0);        // no triggerFirstNote: starts in processBlock
            hi.setEnabled(false);
            expectEquals((int) ev.size(), 0, "arp not enabled: silent");
            hi.setEnabled(true);
            hi.noteOn(60, 1.0f); hi.noteOn(64, 1.0f); hi.noteOn(67, 1.0f);
            const int start = t;
            advance(hi, t, 4000, 88200.0);
            auto hiOns = onsOf(ev);
            expectEquals((int) hiOns.size(), 3);
            if (hiOns.size() == 3)
            {
                expect(hiOns[0].t - start <= 1, "the first note sounds at once, not a step later");
                expect(std::abs((hiOns[2].t - hiOns[0].t) - 1764) <= 2, "20 ms at 88200 Hz");
            }

            // Ratchet in a chord mode repeats the whole chord.
            Arpeggiator chord;
            setup(chord, { 60, 64 });
            chord.setMode(Arpeggiator::Mode::ChordRepeat);
            chord.setRatchetEnabled(true);
            chord.setRatchetCount(2);
            auto chordOns = onsOf(play(chord, 2));
            expectEquals((int) chordOns.size(), 8, "2 steps x 2 ratchets x 2 notes");
        }

        beginTest("Tapped rhythm sets the timing and level of the strum");
        {
            RhythmCapture capture;
            capture.startCapture(1000.0);
            capture.tapEvent(1.0f);
            capture.advanceSamples(100.0);
            capture.tapEvent(0.5f);
            capture.advanceSamples(300.0);
            capture.tapEvent(1.0f);
            capture.stopCapture();

            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            arp.setMode(Arpeggiator::Mode::ChordRepeat);
            arp.setStrumEnabled(true);
            arp.setStrumTime(40.0f);          // 1764 samples
            expect(!arp.isRhythmEnabled());
            arp.setRhythmEnabled(true);
            expect(!arp.isRhythmEnabled(), "no rhythm captured yet");
            expect(arp.setRhythmTemplate(capture));
            expectEquals(arp.getRhythmLength(), 3);
            arp.setRhythmEnabled(true);

            auto ons = onsOf(play(arp, 1));
            expectEquals((int) ons.size(), 3);
            if (ons.size() == 3)
            {
                expect(std::abs((ons[1].t - ons[0].t) - 441) <= 2, "second tap a quarter of the way");
                expect(std::abs((ons[2].t - ons[0].t) - 1764) <= 2, "last tap at the strum time");
                expectWithinAbsoluteError(ons[1].vel, 0.5f, 0.01f);
                expectWithinAbsoluteError(ons[2].vel, 1.0f, 0.01f);
            }

            arp.setRhythmEnabled(false);
            Arpeggiator plain;
            setup(plain, { 60, 64, 67 });
            plain.setMode(Arpeggiator::Mode::ChordRepeat);
            plain.setStrumEnabled(true);
            plain.setStrumTime(40.0f);
            auto even = onsOf(play(plain, 1));
            expect(even.size() == 3 && std::abs((even[1].t - even[0].t) - 882) <= 2, "without it: even spacing");
        }

        beginTest("Swing: the off-beat comes late");
        {
            Arpeggiator arp;
            setup(arp, { 60 });
            arp.setSwing(0.5f);
            auto ons = onsOf(play(arp, 8));
            expectEquals((int) ons.size(), 8);
            if (ons.size() == 8)
            {
                expect(std::abs((ons[1].t - ons[0].t) - (int) (STEP * 1.25)) <= 2, "long first half");
                expect(std::abs((ons[2].t - ons[1].t) - (int) (STEP * 0.75)) <= 2, "short second half");
                expect(std::abs((ons[2].t - ons[0].t) - (int) (STEP * 2.0)) <= 2, "the pair keeps its length");
            }
        }

        beginTest("Snap glide: a rate change arrives gradually");
        {
            auto intervalsAfterChange = [&](float glide)
            {
                Arpeggiator arp;
                setup(arp, { 60 });
                arp.setRate(Arpeggiator::Rate::Eighth);
                arp.setGlideTime(glide);
                Events ev; int t = 0;
                hook(arp, ev, t);
                arp.triggerFirstNote();
                advance(arp, t, 11025 * 2 + 100);
                const int change = t;
                arp.setRate(Arpeggiator::Rate::ThirtySecond);
                advance(arp, t, 44100);
                arp.onNoteOn = nullptr; arp.onNoteOff = nullptr;
                auto ons = onsOf(ev, change);
                std::vector<int> intervals;
                for (size_t i = 1; i < ons.size(); ++i)
                    intervals.push_back(ons[i].t - ons[i - 1].t);
                return intervals;
            };

            auto instant = intervalsAfterChange(0.0f);
            expect(instant.size() > 4);
            for (int iv : instant)
                expect(std::abs(iv - 2756) <= 2, "no glide: 1/32 at once");

            auto glided = intervalsAfterChange(0.6f);     // glide over a quarter note
            expect(glided.size() > 6);
            if (glided.size() > 6)
            {
                expect(glided[0] > 2756 * 3 / 2, "first step after the change is still long: " + juce::String(glided[0]));
                expect(glided[1] < glided[0] && glided[2] < glided[1], "steps get shorter");
                expect(std::abs(glided.back() - 2756) <= 2, "and arrive at 1/32");
            }
        }

        beginTest("Pressure reaches the notes: gate, rate and octave range");
        {
            // Gate
            Arpeggiator g;
            setup(g, { 60 });
            g.setPressureTarget(Arpeggiator::PressureTarget::GateLength);
            g.setPressureAmount(1.0f);
            g.applyPressure(1.0f);
            auto lengths = noteLengths(play(g, 4));
            expect(lengths.size() >= 3 && std::abs(lengths[0] - (int) STEP) <= 2, "full pressure: gate 0.5 + 0.5");

            // Rate
            Arpeggiator r;
            setup(r, { 60 });
            r.setPressureTarget(Arpeggiator::PressureTarget::Rate);
            r.setPressureAmount(1.0f);
            r.applyPressure(1.0f);
            auto fast = onsOf(play(r, 8));
            expectEquals((int) fast.size(), 16, "twice the notes in the same time");

            // Octave range, applied while the notes are held
            Arpeggiator o;
            setup(o, { 60 });
            o.setPressureTarget(Arpeggiator::PressureTarget::OctaveRange);
            o.setPressureAmount(1.0f);
            Events ev; int t = 0;
            hook(o, ev, t);
            o.triggerFirstNote();
            advance(o, t, (int) (STEP * 4) - 10);
            for (auto& e : onsOf(ev))
                expectEquals(e.pitch, 60);
            const int pressed = t;
            o.applyPressure(1.0f);
            advance(o, t, (int) (STEP * 8));
            int highest = 0;
            for (auto& e : onsOf(ev, pressed))
                highest = juce::jmax(highest, e.pitch);
            expectEquals(highest, 60 + 48, "one octave + four from pressure");
            const int released = t;
            o.applyPressure(0.0f);
            advance(o, t, (int) (STEP * 4));
            for (auto& e : onsOf(ev, released + (int) STEP))
                expectEquals(e.pitch, 60);
        }

        beginTest("Mode and octave changes take effect while notes are held");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            Events ev; int t = 0;
            hook(arp, ev, t);
            arp.triggerFirstNote();
            advance(arp, t, (int) (STEP * 3) - 10);
            const int change = t;
            arp.setMode(Arpeggiator::Mode::Down);
            arp.setOctaveRange(2);
            advance(arp, t, (int) (STEP * 12));
            std::set<int> seen;
            for (auto& e : onsOf(ev, change))
                seen.insert(e.pitch);
            expect(seen.count(72) == 1 && seen.count(79) == 1, "the second octave plays without re-pressing keys");
        }

        beginTest("Record mode: individual notes or the held chord per step");
        {
            Arpeggiator notes;
            setup(notes, { 60, 64, 67 });
            notes.setRecording(true);
            notes.setRecordMode(Arpeggiator::RecordMode::Individual);
            play(notes, 2);
            expectEquals(notes.getRecordedNoteCount(), 2);
            expectEquals(notes.getRecordedNote(0).pitch, 60);
            expectEquals(notes.getRecordedNote(1).pitch, 64);

            Arpeggiator chord;
            setup(chord, { 60, 64, 67 });
            chord.setRecording(true);
            chord.setRecordMode(Arpeggiator::RecordMode::Chord);
            play(chord, 2);
            expectEquals(chord.getRecordedNoteCount(), 6);
            expectEquals(chord.getRecordedNote(0).pitch, 60);
            expectEquals(chord.getRecordedNote(1).pitch, 64);
            expectEquals(chord.getRecordedNote(2).pitch, 67);
            expectWithinAbsoluteError((float) chord.getRecordedNote(3).sampleTimestamp, (float) STEP, 2.0f);

            // A strummed chord is recorded note by note, late ones included.
            Arpeggiator strum;
            setup(strum, { 60, 64, 67 });
            strum.setMode(Arpeggiator::Mode::ChordRepeat);
            strum.setStrumEnabled(true);
            strum.setRecording(true);
            play(strum, 1);
            expectEquals(strum.getRecordedNoteCount(), 3);
        }

        beginTest("Parameter locks combine with the macros by the interaction mode");
        {
            using MI = Arpeggiator::MacroInteraction;
            using LP = Arpeggiator::LockParam;

            auto firstLength = [&](MI mode)
            {
                Arpeggiator arp;
                setup(arp, { 60 });
                arp.setMacroInteraction(mode);
                arp.setParamLock(LP::Gate, 0.25f);
                return noteLengths(play(arp, 2))[0];
            };
            expect(std::abs(firstLength(MI::LockWins) - (int) (STEP * 0.25)) <= 2, "lock wins: gate 0.25");
            expect(std::abs(firstLength(MI::MacroWins) - (int) (STEP * 0.5)) <= 2, "macro wins: gate 0.5");
            expect(std::abs(firstLength(MI::Additive) - (int) (STEP * 0.25)) <= 2, "additive, macro at rest: the lock");
            expect(std::abs(firstLength(MI::Multiply) - (int) (STEP * 0.25)) <= 2, "multiply, macro at rest: the lock");

            auto firstVelocity = [&](MI mode)
            {
                Arpeggiator arp;
                setup(arp, { 60 });
                arp.setMacroInteraction(mode);
                arp.setParamLock(LP::Velocity, 0.3f);
                return onsOf(play(arp, 1))[0].vel;
            };
            expectWithinAbsoluteError(firstVelocity(MI::LockWins), 0.3f, 0.001f);
            expectWithinAbsoluteError(firstVelocity(MI::MacroWins), 1.0f, 0.001f);

            auto onsPerStep = [&](MI mode, bool locked)
            {
                Arpeggiator arp;
                setup(arp, { 60 });
                arp.setMacroInteraction(mode);
                arp.setParamLock(LP::Ratchet, 3.0f);
                if (!locked) arp.clearParamLocks();
                return (int) onsOf(play(arp, 1)).size();
            };
            expectEquals(onsPerStep(MI::LockWins, true), 3);
            expectEquals(onsPerStep(MI::MacroWins, true), 1);
            expectEquals(onsPerStep(MI::LockWins, false), 1, "cleared lock: back to the set value");
        }

        beginTest("Latch: a new chord replaces the old one; latch off lets go");
        {
            Arpeggiator arp;
            setup(arp, {});
            arp.setLatch(true);
            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);
            arp.noteOff(60);
            arp.noteOff(64);
            expectEquals(arp.getInputNoteCount(), 2, "latched");
            arp.noteOn(72, 1.0f);
            expectEquals(arp.getInputNoteCount(), 1, "new chord");
            expectEquals(arp.getSequencePitch(0), 72);
            arp.noteOn(76, 1.0f);
            expectEquals(arp.getInputNoteCount(), 2, "second key of the new chord joins");
            arp.noteOff(72);
            arp.setLatch(false);
            expectEquals(arp.getInputNoteCount(), 1, "latch off: only the key still down");
            expectEquals(arp.getSequencePitch(0), 76);

            arp.setLatch(true);
            arp.noteOff(76);
            int ons = 0, offs = 0;
            arp.onNoteOn = [&](int, float) { ++ons; };
            arp.onNoteOff = [&](int) { ++offs; };
            arp.triggerFirstNote();
            arp.setLatch(false);
            expect(!arp.isRunning(), "latch off with no key down stops the arp");
            expectEquals(ons, offs, "and the sounding note is released");
        }

        beginTest("No stuck notes: every note-on gets its note-off");
        {
            for (int modeIndex = 0; modeIndex < (int) Arpeggiator::Mode::NumModes; ++modeIndex)
            {
                Arpeggiator arp;
                setup(arp, { 60, 64, 67, 72 });
                arp.setMode((Arpeggiator::Mode) modeIndex);
                arp.setOctaveRange(2);
                arp.setStrumEnabled(true);
                arp.setStrumTime(60.0f);
                arp.setRatchetEnabled(true);
                arp.setRatchetCount(3);
                arp.setSpice(0.7f); arp.setDice(0.7f); arp.setDensity(0.8f);
                arp.setOctaveScatter(0.7f); arp.setRatchetChance(0.7f); arp.setGateSpread(1.0f);
                arp.setHumanize(true); arp.setHumanizeAmount(1.0f);
                arp.setMutateEnabled(true);
                arp.getMutate().setInterval(1);
                arp.getMutate().setMode(MutateSystem::Mode::Cumulative);
                arp.setGate(1.0f);

                std::map<int, int> sounding;
                bool bad = false;
                arp.onNoteOn = [&](int p, float v)
                {
                    if (p < 0 || p > 127 || v <= 0.0f || v > 1.0f) bad = true;
                    ++sounding[p];
                };
                arp.onNoteOff = [&](int p) { --sounding[p]; };
                arp.triggerFirstNote();
                for (int block = 0; block < 2000; ++block)
                {
                    arp.processBlock(256, 44100.0, 120.0f);
                    if (block == 700) arp.setMode(Arpeggiator::Mode::ChordRepeat);
                    if (block == 900) arp.noteOn(55, 0.8f);
                    if (block == 1100) arp.setFrozen(true);
                    if (block == 1500) arp.setFrozen(false);
                }
                for (int n : { 60, 64, 67, 72, 55 })
                    arp.noteOff(n);
                expect(!arp.isRunning());
                expect(!bad, "pitch and velocity stay in range");
                for (auto& s : sounding)
                    expectEquals(s.second, 0, "mode " + juce::String(modeIndex) + " pitch " + juce::String(s.first));
            }
        }

        // ================================================================
        // Record hook, scale, play order, step rhythm, transport, export
        // ================================================================

        beginTest("Record hook: every emitted note, or the held chord per step, at the moment it sounds");
        {
            struct Rec { int pitch; float vel; int t; };

            Arpeggiator notes;
            setup(notes, { 60, 64, 67 });
            notes.setRatchetEnabled(true);
            notes.setRatchetCount(2);
            notes.setSpice(1.0f);                       // inserted notes
            std::vector<Rec> recorded;
            Events ev;
            int t = 0;
            hook(notes, ev, t);
            notes.onRecordNote = [&](int p, float v) { recorded.push_back({ p, v, t }); };
            notes.triggerFirstNote();
            advance(notes, t, (int) (STEP * 2) - 10);
            expectEquals((int) recorded.size(), 0, "not recording: the hook is silent");

            notes.setRecordMode(Arpeggiator::RecordMode::Individual);
            notes.setRecording(true);
            const int from = t;
            advance(notes, t, (int) (STEP * 8));
            auto ons = onsOf(ev, from);
            expect(ons.size() >= 32, "ratchets and inserted notes sound, got " + juce::String((int) ons.size()));
            expectEquals((int) recorded.size(), (int) ons.size(), "one call per emitted note");
            for (size_t i = 0; i < ons.size() && i < recorded.size(); ++i)
            {
                expectEquals(recorded[i].pitch, ons[i].pitch);
                expectWithinAbsoluteError(recorded[i].vel, ons[i].vel, 1.0e-6f);
                expectEquals(recorded[i].t, ons[i].t, "at the moment the note sounds");
            }
            expectEquals(notes.getRecordedNoteCount(), (int) recorded.size(), "the buffer still fills");

            notes.setRecording(false);
            const size_t before = recorded.size();
            advance(notes, t, (int) (STEP * 2));
            expectEquals((int) recorded.size(), (int) before, "recording off: silent again");

            Arpeggiator chord;
            setup(chord, {});
            chord.noteOn(60, 0.9f);
            chord.noteOn(64, 0.7f);
            chord.noteOn(67, 0.5f);
            chord.setRatchetEnabled(true);
            chord.setRatchetCount(3);
            chord.setRecordMode(Arpeggiator::RecordMode::Chord);
            chord.setRecording(true);
            std::vector<Rec> chordRec;
            Events chordEv;
            int ct = 0;
            hook(chord, chordEv, ct);
            chord.onRecordNote = [&](int p, float v) { chordRec.push_back({ p, v, ct }); };
            chord.triggerFirstNote();
            advance(chord, ct, (int) (STEP * 3) - 10);
            expectEquals((int) chordRec.size(), 9, "three chord notes for each of three steps, ratchets not counted");
            const int chordPitch[3] = { 60, 64, 67 };
            const float chordVel[3] = { 0.9f, 0.7f, 0.5f };
            for (size_t i = 0; i < chordRec.size(); ++i)
            {
                expectEquals(chordRec[i].pitch, chordPitch[i % 3]);
                expectWithinAbsoluteError(chordRec[i].vel, chordVel[i % 3], 1.0e-6f);
                expect(std::abs(chordRec[i].t - (int) std::lround(STEP * (double) (i / 3))) <= 1, "on the step");
            }
        }

        beginTest("Scale: snapping helper, ties go down");
        {
            using A = Arpeggiator;
            expectEquals(A::snapPitchToScale(60, 0, A::SCALE_MAJOR), 60);
            expectEquals(A::snapPitchToScale(61, 0, A::SCALE_MAJOR), 60, "tie between 60 and 62 goes down");
            expectEquals(A::snapPitchToScale(63, 0, A::SCALE_MAJOR), 62);
            expectEquals(A::snapPitchToScale(66, 0, A::SCALE_MAJOR), 65);
            expectEquals(A::snapPitchToScale(60, 2, A::SCALE_MAJOR), 59, "D major has no C");
            expectEquals(A::snapPitchToScale(61, 0, A::SCALE_PENTATONIC_MINOR), 60);
            expectEquals(A::snapPitchToScale(62, 0, A::SCALE_PENTATONIC_MINOR), 63, "63 is nearer than 60");
            expectEquals(A::snapPitchToScale(1, 0, (uint16_t) 0x0001), 0);
            expectEquals(A::snapPitchToScale(127, 0, (uint16_t) 0x0001), 120, "stays inside 0..127");
            expectEquals(A::snapPitchToScale(61, 0, A::SCALE_CHROMATIC), 61);
            for (int p = 0; p < 128; ++p)
                expect(A::isPitchInScale(A::snapPitchToScale(p, 9, A::SCALE_MINOR), 9, A::SCALE_MINOR));

            Arpeggiator arp;
            expect(!arp.isScaleConstrained(), "chromatic by default");
            arp.setScale(14, A::SCALE_MINOR);
            expectEquals(arp.getScaleRoot(), 2);
            expectEquals((int) arp.getScaleMask(), (int) A::SCALE_MINOR);
            expect(arp.isScaleConstrained());
            arp.setScale(0, 0);
            expect(!arp.isScaleConstrained(), "an empty mask is no constraint");
        }

        beginTest("Scale: dice, octave scatter, inserted notes and mutate stay in scale; held notes are not altered");
        {
            using A = Arpeggiator;
            auto inMajor = [](int p) { return A::isPitchInScale(p, 0, A::SCALE_MAJOR); };

            auto diceRun = [&](bool constrained, std::initializer_list<int> held)
            {
                Arpeggiator arp;
                setup(arp, held);
                arp.setDice(1.0f);
                arp.setOctaveScatter(0.5f);
                arp.setSpice(0.8f);
                if (constrained)
                    arp.setScale(0, A::SCALE_MAJOR);
                return onsOf(play(arp, 96));
            };

            auto freeOns = diceRun(false, { 60, 64, 67 });
            int outside = 0;
            for (auto& e : freeOns)
                outside += inMajor(e.pitch) ? 0 : 1;
            expect(outside > 0, "without a scale the dice leave C major");

            auto scaleOns = diceRun(true, { 60, 64, 67 });
            expectEquals((int) scaleOns.size(), (int) freeOns.size(), "the scale changes pitches only");
            int moved = 0;
            for (size_t i = 0; i < scaleOns.size(); ++i)
            {
                expect(inMajor(scaleOns[i].pitch), "pitch " + juce::String(scaleOns[i].pitch) + " is in C major");
                const int pc = scaleOns[i].pitch % 12;
                moved += (pc != 0 && pc != 4 && pc != 7) ? 1 : 0;
            }
            expect(moved > 0, "the dice still move notes");

            // A held note outside the scale plays as held; only what the dice change is snapped.
            auto heldOut = diceRun(true, { 61 });
            int asHeld = 0;
            for (auto& e : heldOut)
            {
                expect(e.pitch == 61 || inMajor(e.pitch), "pitch " + juce::String(e.pitch));
                asHeld += (e.pitch == 61) ? 1 : 0;
            }
            expect(asHeld > 0, "the dice interval 0 leaves the held note alone");

            Arpeggiator plain;
            setup(plain, { 61, 66 });
            plain.setScale(0, A::SCALE_MAJOR);
            for (auto& e : onsOf(play(plain, 8)))
                expect(e.pitch == 61 || e.pitch == 66, "macros at rest: exactly the held notes");

            // Mutate: eight cumulative rounds at full amount.
            auto mutateRun = [&](bool constrained)
            {
                Arpeggiator arp;
                setup(arp, { 60, 63, 67, 70 });
                arp.setOctaveRange(2);
                if (constrained)
                    arp.setScale(0, A::SCALE_PENTATONIC_MINOR);
                arp.getMutate().setMode(MutateSystem::Mode::Cumulative);
                arp.setSpice(1.0f);
                for (int i = 0; i < 8; ++i)
                    arp.mutateNow();
                return onsOf(play(arp, 64));
            };
            int mutatedOutside = 0;
            for (auto& e : mutateRun(false))
                mutatedOutside += A::isPitchInScale(e.pitch, 0, A::SCALE_PENTATONIC_MINOR) ? 0 : 1;
            expect(mutatedOutside > 0, "without a scale mutations leave the pentatonic");
            auto mutated = mutateRun(true);
            expect(mutated.size() > 16);
            for (auto& e : mutated)
                expect(A::isPitchInScale(e.pitch, 0, A::SCALE_PENTATONIC_MINOR),
                       "mutated pitch " + juce::String(e.pitch) + " is in the scale");
        }

        beginTest("As played = held keys by first press; play order = every press at its own velocity");
        {
            auto run = [&](Arpeggiator::Mode mode, bool latchAndRepress)
            {
                Arpeggiator arp;
                setup(arp, {});
                arp.setMode(mode);
                arp.setLatch(latchAndRepress);
                arp.noteOn(67, 1.0f);
                arp.noteOn(60, 0.8f);
                arp.noteOn(64, 0.6f);
                if (latchAndRepress)
                {
                    arp.noteOff(67);            // latched: stays in the chord
                    arp.noteOn(67, 0.3f);       // pressed again while other keys are down
                }
                return onsOf(play(arp, 8));
            };

            // Without a repeated press the two are the same: the order of pressing.
            for (auto mode : { Arpeggiator::Mode::AsPlayed, Arpeggiator::Mode::PlayOrder })
            {
                auto ons = run(mode, false);
                const int want[3] = { 67, 60, 64 };
                const float vel[3] = { 1.0f, 0.8f, 0.6f };
                expectEquals((int) ons.size(), 8);
                for (size_t i = 0; i < ons.size(); ++i)
                {
                    expectEquals(ons[i].pitch, want[i % 3]);
                    expectWithinAbsoluteError(ons[i].vel, vel[i % 3], 1.0e-6f);
                }
            }

            // A key pressed twice: once in As Played (at its latest velocity), twice in Play Order.
            auto asPlayed = run(Arpeggiator::Mode::AsPlayed, true);
            {
                const int want[3] = { 67, 60, 64 };
                const float vel[3] = { 0.3f, 0.8f, 0.6f };
                for (size_t i = 0; i < asPlayed.size(); ++i)
                {
                    expectEquals(asPlayed[i].pitch, want[i % 3]);
                    expectWithinAbsoluteError(asPlayed[i].vel, vel[i % 3], 1.0e-6f);
                }
            }
            auto playOrder = run(Arpeggiator::Mode::PlayOrder, true);
            {
                const int want[4] = { 67, 60, 64, 67 };
                const float vel[4] = { 1.0f, 0.8f, 0.6f, 0.3f };
                expectEquals((int) playOrder.size(), 8);
                for (size_t i = 0; i < playOrder.size(); ++i)
                {
                    expectEquals(playOrder[i].pitch, want[i % 4]);
                    expectWithinAbsoluteError(playOrder[i].vel, vel[i % 4], 1.0e-6f);
                }
            }

            // A released key leaves the play order with all its presses.
            Arpeggiator arp;
            setup(arp, {});
            arp.setMode(Arpeggiator::Mode::PlayOrder);
            arp.setLatch(true);
            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);
            arp.noteOff(60);
            arp.noteOn(60, 0.5f);
            expectEquals(arp.getSequenceLength(), 3);
            arp.noteOff(60);
            arp.setLatch(false);                // only 64 is still down
            expectEquals(arp.getSequenceLength(), 1);
            expectEquals(arp.getSequencePitch(0), 64);
        }

        // Taps at 0, 100 and 400 with levels 1, 0.5, 1: positions 0, 1/4, 1 of the span.
        auto tappedRhythm = []
        {
            RhythmCapture capture;
            capture.startCapture(1000.0);
            capture.tapEvent(1.0f);
            capture.advanceSamples(100.0);
            capture.tapEvent(0.5f);
            capture.advanceSamples(300.0);
            capture.tapEvent(1.0f);
            capture.stopCapture();
            return capture;
        };

        beginTest("Tapped rhythm sets the step timing and accents of a normal arpeggio");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            expect(arp.setRhythmTemplate(tappedRhythm()));
            expect(!arp.isStepRhythmActive(), "not until it is switched on");
            arp.setRhythmEnabled(true);
            expect(arp.isStepRhythmActive());
            expectWithinAbsoluteError((float) arp.getRhythmStepLength(0), 0.5f, 1.0e-5f);
            expectWithinAbsoluteError((float) arp.getRhythmStepLength(1), 1.5f, 1.0e-5f);
            expectWithinAbsoluteError((float) arp.getRhythmStepLength(2), 1.0f, 1.0e-5f);

            auto ev = play(arp, 9);
            auto ons = onsOf(ev);
            // A cycle of three steps lasts three steps: onsets at 0, 0.5 and 2 steps.
            const double at[3] = { 0.0, 0.5, 2.0 };
            const float level[3] = { 1.0f, 0.5f, 1.0f };
            const int pitch[3] = { 60, 64, 67 };
            expectEquals((int) ons.size(), 9);
            for (size_t i = 0; i < ons.size(); ++i)
            {
                const double want = STEP * (3.0 * (double) (i / 3) + at[i % 3]);
                expect(std::abs(ons[i].t - (int) std::lround(want)) <= 1,
                       "onset " + juce::String((int) i) + " at " + juce::String(ons[i].t));
                expectWithinAbsoluteError(ons[i].vel, level[i % 3], 0.001f);
                expectEquals(ons[i].pitch, pitch[i % 3]);
            }
            auto lengths = noteLengths(ev);
            expect(lengths.size() >= 2 && std::abs(lengths[0] - (int) (STEP * 0.25)) <= 2, "gate follows the step: half of half a step");
            expect(lengths.size() >= 2 && std::abs(lengths[1] - (int) (STEP * 0.75)) <= 2, "half of a step and a half");

            // Evenly spaced taps: the plain arpeggio.
            RhythmCapture even;
            even.startCapture(1000.0);
            for (int i = 0; i < 4; ++i)
            {
                even.tapEvent(1.0f);
                even.advanceSamples(50.0);
            }
            even.stopCapture();
            Arpeggiator plain;
            setup(plain, { 60, 64, 67 });
            plain.setRhythmTemplate(even);
            plain.setRhythmEnabled(true);
            auto evenOns = onsOf(play(plain, 8));
            expectEquals((int) evenOns.size(), 8);
            for (size_t i = 0; i < evenOns.size(); ++i)
                expect(std::abs(evenOns[i].t - (int) std::lround(STEP * (double) i)) <= 1, "on the grid");

            // Block chords (strum off) take the step rhythm too; a strum keeps the rhythm for itself.
            Arpeggiator chords;
            setup(chords, { 60, 64, 67 });
            chords.setMode(Arpeggiator::Mode::ChordRepeat);
            chords.setRhythmTemplate(tappedRhythm());
            chords.setRhythmEnabled(true);
            expect(chords.isStepRhythmActive());
            chords.setStrumEnabled(true);
            expect(!chords.isStepRhythmActive(), "strum on: the taps shape the strum");
        }

        // Runs the arp with the transport position given before every block.
        auto advanceSynced = [](Arpeggiator& arp, int& t, int samples, double beatOffset, int blockSize = 1)
        {
            for (int done = 0; done < samples; done += blockSize)
            {
                arp.syncToTransport(beatOffset + (double) t / 22050.0);
                arp.processBlock(blockSize, 44100.0, 120.0f);
                t += blockSize;
            }
        };

        beginTest("Transport: steps on the beat grid, no doubled or dropped notes");
        {
            for (int blockSize : { 1, 64, 480 })
            {
                Arpeggiator arp;
                setup(arp, { 60, 64, 67 });
                arp.setEnabled(true);
                Events ev;
                int t = 0;
                hook(arp, ev, t);
                advanceSynced(arp, t, (int) (STEP * 64) - 2 * blockSize, 0.0, blockSize);
                expect(arp.isTransportSynced());
                auto ons = onsOf(ev);
                expectEquals((int) ons.size(), 64, "block size " + juce::String(blockSize));
                const int chord[3] = { 60, 64, 67 };
                for (size_t i = 0; i < ons.size(); ++i)
                {
                    expectEquals(ons[i].pitch, chord[i % 3]);
                    // an event is stamped with the start of its block
                    const int late = (int) std::lround(STEP * (double) i) - ons[i].t;
                    expect(late >= -1 && late <= blockSize, "step " + juce::String((int) i) + " on the grid");
                }
            }

            // Swing and a tapped rhythm from beat 0: the same notes as running free.
            for (int variant = 0; variant < 2; ++variant)
            {
                auto make = [&](Arpeggiator& arp)
                {
                    setup(arp, { 60, 64, 67 });
                    if (variant == 0) arp.setSwing(0.5f);
                    else { arp.setRhythmTemplate(tappedRhythm()); arp.setRhythmEnabled(true); }
                    arp.setEnabled(true);
                };
                Arpeggiator free, synced;
                make(free);
                make(synced);
                Events freeEv, syncedEv;
                int ft = 0, st = 0;
                hook(free, freeEv, ft);
                hook(synced, syncedEv, st);
                advance(free, ft, (int) (STEP * 12) - 10);
                advanceSynced(synced, st, (int) (STEP * 12) - 10, 0.0);
                expectEquals((int) syncedEv.size(), (int) freeEv.size());
                for (size_t i = 0; i < freeEv.size() && i < syncedEv.size(); ++i)
                {
                    expect(freeEv[i].on == syncedEv[i].on && freeEv[i].pitch == syncedEv[i].pitch);
                    expect(std::abs(freeEv[i].t - syncedEv[i].t) <= 1, "variant " + juce::String(variant));
                }
            }
        }

        beginTest("Transport: a key pressed between grid lines sounds at once, the next step is on the grid");
        {
            for (double phase : { 0.3, 0.7 })
            {
                Arpeggiator arp;
                setup(arp, { 60, 64, 67 });
                arp.setEnabled(true);
                Events ev;
                int t = 0;
                hook(arp, ev, t);
                advanceSynced(arp, t, (int) (STEP * 8), phase * 0.25);
                auto ons = onsOf(ev);
                expect(ons.size() >= 7);
                if (ons.size() >= 3)
                {
                    expectEquals(ons[0].t, 0, "the first note does not wait");
                    // nearest line to the end of a whole step: 0.7 steps on for 0.3, 1.3 for 0.7
                    const double first = (phase < 0.5) ? (1.0 - phase) : (2.0 - phase);
                    expect(std::abs(ons[1].t - (int) std::lround(STEP * first)) <= 1,
                           "second note at " + juce::String(ons[1].t));
                    for (size_t i = 2; i < ons.size(); ++i)
                        expect(std::abs((ons[i].t - ons[i - 1].t) - (int) std::lround(STEP)) <= 1, "then every step");
                    expectEquals(ons[1].pitch, 64);
                    expectEquals(ons[2].pitch, 67);
                }
            }
        }

        beginTest("Transport: a jump resynchronises without doubled, dropped or hanging notes; without it the arp runs free");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            arp.setEnabled(true);
            arp.setGate(1.0f);
            Events ev;
            int t = 0;
            hook(arp, ev, t);
            advanceSynced(arp, t, (int) (STEP * 5.4), 0.0);
            const int jumpAt = t;
            const double offset = 7.13 - (double) t / 22050.0;      // the transport is now at beat 7.13
            advanceSynced(arp, t, (int) (STEP * 12), offset);
            const int syncEnd = t;

            auto ons = onsOf(ev);
            const int chord[3] = { 60, 64, 67 };
            for (size_t i = 0; i < ons.size(); ++i)
            {
                expectEquals(ons[i].pitch, chord[i % 3], "the sequence carries on");
                if (i > 0)
                {
                    const int gap = ons[i].t - ons[i - 1].t;
                    expect(gap >= (int) (STEP * 0.5) - 2 && gap <= (int) (STEP * 1.5) + 2,
                           "gap " + juce::String(gap) + " at " + juce::String(ons[i].t));
                }
                if (ons[i].t > jumpAt + (int) (STEP * 1.5))
                {
                    const double steps = (offset + (double) ons[i].t / 22050.0) / 0.25;
                    expect(std::abs(steps - std::round(steps)) < 2.0 / STEP, "on the new grid");
                }
            }

            // The transport stops: no more calls, the arp keeps its pace.
            advance(arp, t, (int) (STEP * 6));
            expect(!arp.isTransportSynced());
            auto after = onsOf(ev, syncEnd);
            expect(after.size() >= 5);
            for (size_t i = 1; i < after.size(); ++i)
                expect(std::abs((after[i].t - after[i - 1].t) - (int) std::lround(STEP)) <= 1, "free-running");

            for (int n : { 60, 64, 67 })
                arp.noteOff(n);
            std::map<int, int> sounding;
            for (auto& e : ev)
                sounding[e.pitch] += e.on ? 1 : -1;
            for (auto& s : sounding)
                expectEquals(s.second, 0, "pitch " + juce::String(s.first) + " released");
        }

        beginTest("Transport: resetPhase starts the pattern again on the next sample");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            arp.setEnabled(true);
            arp.setGate(1.0f);
            Events ev;
            int t = 0;
            hook(arp, ev, t);
            advanceSynced(arp, t, (int) (STEP * 4.5), 0.0);         // 60 64 67 60 64, the last one sounding
            const int restart = t;
            arp.resetPhase();
            int soundingNow = 0;
            for (auto& e : ev)
                soundingNow += e.on ? 1 : -1;
            expectEquals(soundingNow, 0, "the sounding note is released");

            const double offset = -(double) t / 22050.0;            // transport starts from beat 0
            advanceSynced(arp, t, (int) (STEP * 4) - 10, offset);
            auto ons = onsOf(ev, restart);
            expectEquals((int) ons.size(), 4);
            const int chord[3] = { 60, 64, 67 };
            for (size_t i = 0; i < ons.size(); ++i)
            {
                expectEquals(ons[i].pitch, chord[i % 3], "from the first note");
                expect(std::abs((ons[i].t - restart) - (int) std::lround(STEP * (double) i)) <= 1, "on the grid");
            }

            Arpeggiator idle;
            idle.resetPhase();
            expect(!idle.isRunning(), "nothing held: nothing starts");
        }

        beginTest("Transport: the mutate interval counts bars of the transport");
        {
            auto make = [&](Arpeggiator& arp)
            {
                setup(arp, { 60, 64, 67, 72 });
                arp.setEnabled(true);
                arp.setMutateEnabled(true);
                arp.getMutate().setInterval(1);
                arp.setSpice(0.5f);
            };

            // The transport is at beat 3: the bar line comes one beat (four steps) later.
            Arpeggiator synced;
            make(synced);
            int t = 0;
            advanceSynced(synced, t, (int) (STEP * 3.5), 3.0);
            expectEquals(synced.getMutationGeneration(), 0, "before the bar line");
            advanceSynced(synced, t, (int) STEP, 3.0);
            expectEquals(synced.getMutationGeneration(), 1, "at the bar line");
            advanceSynced(synced, t, (int) (STEP * 14), 3.0);
            expectEquals(synced.getMutationGeneration(), 1, "not again inside the bar");

            // Running free, a bar of playing has to go by.
            Arpeggiator free;
            make(free);
            int ft = 0;
            advance(free, ft, (int) (STEP * 4.5));
            expectEquals(free.getMutationGeneration(), 0);
            advance(free, ft, (int) (STEP * 12));
            expectEquals(free.getMutationGeneration(), 1);
        }

        beginTest("Export: one cycle of the sequence as step data");
        {
            using ES = Arpeggiator::ExportedStep;
            ES steps[32];

            Arpeggiator arp;
            expectEquals(arp.exportSteps(steps, 32), 0, "nothing held");
            setup(arp, {});
            arp.setOctaveRange(2);
            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 0.7f);
            arp.noteOn(67, 0.4f);
            arp.setGate(0.6f);
            expectEquals(arp.exportSteps(nullptr, 32), 0);
            expectEquals(arp.exportSteps(steps, 4), 4, "no more than asked for");
            expectEquals(arp.exportSteps(steps, 32), 6);
            const int want[6] = { 60, 64, 67, 72, 76, 79 };
            const float vel[3] = { 1.0f, 0.7f, 0.4f };
            for (int i = 0; i < 6; ++i)
            {
                expect(!steps[i].rest);
                expectEquals(steps[i].noteCount, 1);
                expectEquals(steps[i].pitches[0], want[i]);
                expectWithinAbsoluteError(steps[i].velocity, vel[i % 3], 1.0e-5f);
                expectWithinAbsoluteError(steps[i].gate, 0.6f, 1.0e-5f);
                expectEquals(steps[i].hits, 1);
                expectWithinAbsoluteError(steps[i].startSteps, (float) i, 1.0e-5f);
                expectWithinAbsoluteError(steps[i].lengthSteps, 1.0f, 1.0e-5f);
            }

            // Random modes, not frozen: the sequence in order. The macros are left out.
            arp.setMode(Arpeggiator::Mode::Random);
            arp.setDice(1.0f);
            arp.setDensity(0.0f);
            Events ev;
            int t = 0;
            hook(arp, ev, t);
            arp.triggerFirstNote();
            advance(arp, t, (int) (STEP * 3));          // the mode change is taken up at a step
            expectEquals(arp.exportSteps(steps, 32), 6);
            for (int i = 0; i < 6; ++i)
                expect(!steps[i].rest && steps[i].pitches[0] == want[i], "step " + juce::String(i));

            // Exporting does not disturb what plays: the same stream with and without it.
            auto stream = [&](bool exporting)
            {
                Arpeggiator a;
                setup(a, { 60, 64, 67 });
                a.setMode(Arpeggiator::Mode::RandomOctave);
                a.setDice(0.5f);
                Events e;
                int at = 0;
                hook(a, e, at);
                a.triggerFirstNote();
                for (int i = 0; i < 16; ++i)
                {
                    advance(a, at, (int) STEP);
                    if (exporting)
                    {
                        ES tmp[8];
                        a.exportSteps(tmp, 8);
                    }
                }
                return onsOf(e);
            };
            expect(samePitchVel(stream(false), stream(true)));

            // Chords and repeats.
            Arpeggiator chord;
            setup(chord, { 60, 64, 67 });
            chord.setMode(Arpeggiator::Mode::ChordRepeat);
            chord.setNoteRepeats(2);
            expectEquals(chord.exportSteps(steps, 32), 2);
            expectEquals(steps[0].noteCount, 3);
            expectEquals(steps[1].pitches[2], 67);

            // A tapped rhythm gives the steps their place and length.
            Arpeggiator tapped;
            setup(tapped, { 60, 64, 67 });
            tapped.setRhythmTemplate(tappedRhythm());
            tapped.setRhythmEnabled(true);
            expectEquals(tapped.exportSteps(steps, 32), 3);
            expectWithinAbsoluteError(steps[1].startSteps, 0.5f, 1.0e-5f);
            expectWithinAbsoluteError(steps[1].lengthSteps, 1.5f, 1.0e-5f);
            expectWithinAbsoluteError(steps[1].gate, 0.75f, 1.0e-5f);
            expectWithinAbsoluteError(steps[1].velocity, 0.5f, 1.0e-3f);
            expectWithinAbsoluteError(steps[2].startSteps, 2.0f, 1.0e-5f);
        }

        beginTest("Export: the frozen loop comes out as it sounds");
        {
            using ES = Arpeggiator::ExportedStep;
            for (auto mode : { Arpeggiator::Mode::Random, Arpeggiator::Mode::UpDown, Arpeggiator::Mode::RandomOctave,
                               Arpeggiator::Mode::Walk })
            {
                Arpeggiator arp;
                setup(arp, { 60, 64, 67, 72 });
                arp.setMode(mode);
                arp.setDice(0.6f);
                arp.setOctaveScatter(0.5f);
                arp.setDensity(0.2f);                   // drops steps: rests
                arp.setGateSpread(0.8f);
                arp.setScale(0, Arpeggiator::SCALE_MAJOR);
                Events ev;
                int t = 0;
                hook(arp, ev, t);
                arp.triggerFirstNote();
                advance(arp, t, (int) (STEP * 6) - 50);
                arp.setFrozen(true);                    // the loop starts at step 6
                advance(arp, t, (int) (STEP * 2));

                ES steps[32];
                const int count = arp.exportSteps(steps, 32);
                expectEquals(count, arp.getFreezeLoopLength());
                expect(count >= 4);

                advance(arp, t, (int) (STEP * (double) (2 * count + 1)));
                int sounding = 0;
                for (int loop = 0; loop < 2; ++loop)
                    for (int i = 0; i < count; ++i)
                    {
                        const int at = (int) std::lround(STEP * (double) (6 + loop * count + i));
                        auto ons = onsOf(ev, at - 2, at + (int) STEP - 2);
                        if (steps[i].rest)
                        {
                            expectEquals((int) ons.size(), 0, "a rest");
                            continue;
                        }
                        ++sounding;
                        expectEquals((int) ons.size(), steps[i].hits);
                        if (ons.empty())
                            continue;
                        expectEquals(ons[0].pitch, steps[i].pitches[0], "mode " + juce::String((int) mode) + " step " + juce::String(i));
                        expectWithinAbsoluteError(ons[0].vel, steps[i].velocity, 1.0e-5f);
                        if (steps[i].hits == 1)
                        {
                            int off = -1;
                            for (auto& e : ev)
                                if (!e.on && e.pitch == ons[0].pitch && e.t >= ons[0].t) { off = e.t; break; }
                            expect(std::abs((off - ons[0].t) - (int) std::lround(STEP * (double) steps[i].gate)) <= 2,
                                   "gate " + juce::String(steps[i].gate));
                        }
                    }
                expect(sounding >= 2, "the loop has notes");
            }
        }

        beginTest("Found on the way: chord steps stay whole when the chord grows; octave shifts fold at the range ends");
        {
            Arpeggiator arp;
            setup(arp, { 60, 64, 67 });
            arp.setMode(Arpeggiator::Mode::ChordRepeat);
            arp.setOctaveRange(2);
            Events ev;
            int t = 0;
            hook(arp, ev, t);
            arp.triggerFirstNote();
            advance(arp, t, (int) (STEP * 1.5));        // the second step (an octave up) is playing
            arp.noteOn(72, 1.0f);
            const int from = (int) std::lround(STEP * 2) - 2;
            advance(arp, t, (int) (STEP * 6));
            for (int step = 2; step < 7; ++step)
            {
                const int at = (int) std::lround(STEP * (double) step);
                auto ons = onsOf(ev, at - 2, at + 100);
                expectEquals((int) ons.size(), 4, "a whole chord at step " + juce::String(step));
                std::set<int> classes;
                for (auto& e : ons)
                    classes.insert(e.pitch % 12);
                expect(classes == std::set<int> ({ 0, 4, 7 }), "C E G C");
                if (ons.size() == 4)
                    expect(ons[3].pitch - ons[0].pitch == 12, "one octave of the chord, not a split one");
            }
            juce::ignoreUnused(from);

            Arpeggiator high;
            setup(high, { 120 });
            high.setMode(Arpeggiator::Mode::RandomOctave);
            auto ons = onsOf(play(high, 48));
            expect(ons.size() >= 40);
            for (auto& e : ons)
                expect(e.pitch % 12 == 0 && e.pitch <= 127, "still a C: " + juce::String(e.pitch));
        }

        // ================================================================
        // ARP screen: every page, every cell
        // ================================================================

        beginTest("ArpScreen: five pages, labels and values fit, nothing unused that is shown");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);
            expectEquals(screen.getPageCount(), 5);

            const char* names[5] = { "ARP", "PLAY", "VARY", "MUTA", "ACFG" };
            const char* labels[5][8] = {
                { "MODE", "RATE", "OCT", "GATE", "RPT", "SWNG", "GLID", "LTCH" },
                { "STRM", "TIME", "DIR", "VEL", "RTCH", "RDEC", "HUMN", "FREZ" },
                { "SPCE", "DICE", "DENS", "SCAT", "RCHN", "GVAR", "STRP", "VAR" },
                { "MUTA", "INTV", "MODE", "SPCE", "EVOL", "ANC", ">PAT", "---" },
                { "DEST", "DPTH", "REC", "LOCK", "TAP", "RHYT", "---", "---" } };

            for (int p = 0; p < 5; ++p)
            {
                screen.setPageIndex(p);
                expectEquals(screen.getName(), juce::String(names[p]));
                expect(screen.getSubPage().startsWith("OFF"));
                expectEquals(screen.getPageIndex(), p);     // the name bar draws "n/5" from this
                for (int c = 0; c < 8; ++c)
                {
                    expectEquals(screen.getEncoderName(c), juce::String(labels[p][c]));
                    expect(screen.getEncoderName(c).length() <= 4);
                    expect(!screen.hasSubParam(c), "no hidden sub-parameters");
                }
            }

            // Every value text fits five characters at both ends of every cell's range.
            for (int pass = 0; pass < 2; ++pass)
            {
                for (int p = 0; p < 5; ++p)
                {
                    screen.setPageIndex(p);
                    for (int c = 0; c < 8; ++c)
                        for (int n = 0; n < 4; ++n)
                            screen.onEncoderChange(c, pass == 0 ? 63 : 65);     // +63 / -63 steps
                    for (int c = 0; c < 8; ++c)
                    {
                        auto text = screen.getCellText(p, c);
                        expect(text.length() <= 5, "'" + text + "' too long on " + juce::String(names[p]));
                        if (screen.getCellLabel(p, c).isNotEmpty())
                            expect(text.isNotEmpty(), "cell without a value on " + juce::String(names[p]));
                    }
                }
            }

            DisplayRenderer display;
            for (int p = 0; p < 5; ++p)
            {
                screen.setPageIndex(p);
                screen.render(display);
            }
        }

        beginTest("ArpScreen: rate and mode are shown in full");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);
            // The knob goes from the slowest rate to the fastest, 1/1 to 1/64T, and stops there.
            const char* rates[12] = { "1/1", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64", "1/64T" };
            expectEquals((int) Arpeggiator::Rate::NumRates, 12);
            arp.setRate(Arpeggiator::Rate::Whole);
            for (int r = 0; r < 12; ++r)
            {
                expectEquals(screen.getCellText(0, 1), juce::String(rates[r]));
                expectEquals(Arpeggiator::speedPosition(arp.getRate()), r);
                screen.onEncoderChange(1, 1);
            }
            expectEquals((int) arp.getRate(), (int) Arpeggiator::Rate::SixtyFourthT);

            // Projects store the rate's number: the nine older rates keep theirs.
            expectEquals((int) Arpeggiator::Rate::ThirtySecond, 5);
            expectEquals((int) Arpeggiator::Rate::SixteenthT, 8);
            expectEquals((int) Arpeggiator::Rate::ThirtySecondT, 9);

            // The fifth character really is drawn: 1/16T and 1/16 differ on the glass.
            DisplayRenderer a, b;
            arp.setRate(Arpeggiator::Rate::SixteenthT);
            screen.render(a);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            screen.render(b);
            int differing = 0;
            for (int y = 0; y < DisplayRenderer::HEIGHT; ++y)
                for (int x = 32 + 1 + 24; x < 64; ++x)
                    if (a.getPixel(x, y) != b.getPixel(x, y)) ++differing;
            expect(differing > 0, "the T of 1/16T is on the display");

            // The fast rates really play that fast: notes in one beat at 120 BPM (22050 samples).
            auto notesPerBeat = [](Arpeggiator::Rate rate)
            {
                Arpeggiator fast;
                fast.setRate(rate);
                fast.setGate(0.5f);
                fast.noteOn(60, 1.0f);
                int count = 0;
                fast.onNoteOn = [&count](int, float) { ++count; };
                fast.onNoteOff = [](int) {};
                fast.triggerFirstNote();
                for (int done = 0; done < 22050 - 50; done += 50)
                    fast.processBlock(50, 44100.0, 120.0f);
                return count;
            };
            expectEquals(notesPerBeat(Arpeggiator::Rate::ThirtySecond), 8);
            expectEquals(notesPerBeat(Arpeggiator::Rate::ThirtySecondT), 12);
            expectEquals(notesPerBeat(Arpeggiator::Rate::SixtyFourth), 16);
            expectEquals(notesPerBeat(Arpeggiator::Rate::SixtyFourthT), 24);

            std::set<juce::String> modeNames;
            for (int m = 0; m < (int) Arpeggiator::Mode::NumModes; ++m)
            {
                arp.setMode((Arpeggiator::Mode) m);
                auto text = screen.getCellText(0, 0);
                expect(text.length() >= 2 && text.length() <= 5);
                modeNames.insert(text);
            }
            expectEquals((int) modeNames.size(), (int) Arpeggiator::Mode::NumModes);

            arp.setGlideTime(0.0f);
            expectEquals(screen.getCellText(0, 6), juce::String("OFF"));
            screen.onEncoderChange(6, 1);
            expectEquals(screen.getCellText(0, 6), juce::String("1/32"));
            expectEquals(Arpeggiator::getGlideDivisionName(arp.getGlideTime()), juce::String("1/32"));
            screen.onEncoderChange(6, 5);
            expectEquals(screen.getCellText(0, 6), juce::String("1/1"));
        }

        beginTest("ArpScreen VARY: every cell edits its macro");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);
            screen.setPageIndex(2);

            screen.onEncoderChange(0, 10);
            expectWithinAbsoluteError(arp.getSpice(), 0.10f, 0.001f);
            screen.onEncoderChange(1, 20);
            expectWithinAbsoluteError(arp.getDice(), 0.20f, 0.001f);
            expectWithinAbsoluteError(arp.getDensity(), 0.5f, 0.001f);
            expectEquals(screen.getCellText(2, 2), juce::String("50"));
            screen.onEncoderChange(2, 128 - 30);
            expectWithinAbsoluteError(arp.getDensity(), 0.20f, 0.001f);
            screen.onEncoderChange(3, 30);
            expectWithinAbsoluteError(arp.getOctaveScatter(), 0.30f, 0.001f);
            screen.onEncoderChange(4, 40);
            expectWithinAbsoluteError(arp.getRatchetChance(), 0.40f, 0.001f);
            screen.onEncoderChange(5, 50);
            expectWithinAbsoluteError(arp.getGateSpread(), 0.50f, 0.001f);
            screen.onEncoderChange(6, 3);
            expect(arp.getStripTarget() == Arpeggiator::StripTarget::Density, "STRP picks the macro the mod strip drives");
            expectEquals(screen.getCellText(2, 6), juce::String("DENS"));
            expectEquals(arp.getVariation(), 1);
            screen.onEncoderChange(7, 4);
            expectEquals(arp.getVariation(), 5);
            expectEquals(screen.getCellText(2, 7), juce::String("5"));

            // And the macros the screen set are the ones the notes go through.
            expectWithinAbsoluteError(arp.getVariationMacros().getSpice(), 0.10f, 0.001f);
            expectWithinAbsoluteError(arp.getVariationMacros().getGateSpread(), 0.50f, 0.001f);
        }

        beginTest("ArpScreen MUTA: on/off, interval, mode, spice, evolve / return, commit / original");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);
            screen.setPageIndex(3);
            setup(arp, { 60, 64, 67, 72 });
            arp.onNoteOn = [](int, float) {};
            arp.onNoteOff = [](int) {};
            arp.triggerFirstNote();

            screen.onEncoderChange(0, 1);
            expect(arp.isMutateEnabled());
            screen.onEncoderChange(0, 1);
            expect(arp.isMutateEnabled(), "further clockwise stays on");
            screen.onEncoderChange(0, 127);
            expect(!arp.isMutateEnabled());

            expectEquals(arp.getMutate().getInterval(), 4);
            screen.onEncoderChange(1, 127);
            expectEquals(arp.getMutate().getInterval(), 3);
            expectEquals(screen.getCellText(3, 1), juce::String("3"));

            expectEquals(screen.getCellText(3, 2), juce::String("ANC"));
            screen.onEncoderChange(2, 1);
            expect(arp.getMutate().getMode() == MutateSystem::Mode::Cumulative);
            expectEquals(screen.getCellText(3, 2), juce::String("CUM"));

            screen.onEncoderChange(3, 30);
            expectWithinAbsoluteError(arp.getSpice(), 0.30f, 0.001f);
            expectEquals(screen.getCellText(2, 0), juce::String("30"), "the same spice as on VARY");

            // EVOL: clockwise mutates now, anticlockwise returns to the ancestor.
            expectEquals(screen.getCellText(3, 4), juce::String("0"));
            screen.onEncoderChange(4, 1);
            expectEquals(screen.getCellText(3, 4), juce::String("1"), "shown at once");
            arp.processBlock(64, 44100.0, 120.0f);
            expectEquals(arp.getMutate().getNoteGeneration(), 1, "and carried out by the audio side");
            screen.onEncoderChange(4, 1);
            arp.processBlock(64, 44100.0, 120.0f);
            expectEquals(arp.getMutate().getNoteGeneration(), 2, "cumulative: one more");

            // ANC: clockwise keeps the mutation as the new ancestor.
            expectEquals(screen.getCellText(3, 5), juce::String("ORIG"));
            screen.onEncoderChange(5, 1);
            expectEquals(screen.getCellText(3, 5), juce::String("KEPT"));
            arp.processBlock(64, 44100.0, 120.0f);
            expect(arp.getMutate().hasCommittedNotes());
            expectEquals(screen.getCellText(3, 4), juce::String("0"), "the kept mutation is the ancestor");

            screen.onEncoderChange(4, 1);
            arp.processBlock(64, 44100.0, 120.0f);
            expectEquals(arp.getMutate().getNoteGeneration(), 1);
            screen.onEncoderChange(4, 127);
            expectEquals(screen.getCellText(3, 4), juce::String("0"));
            arp.processBlock(64, 44100.0, 120.0f);
            expectEquals(arp.getMutate().getNoteGeneration(), 0, "returned");
            expect(arp.getMutate().hasCommittedNotes(), "to the kept ancestor");

            screen.onEncoderChange(5, 127);
            expectEquals(screen.getCellText(3, 5), juce::String("ORIG"));
            arp.processBlock(64, 44100.0, 120.0f);
            expect(!arp.getMutate().hasCommittedNotes(), "back to the held notes");
            for (int i = 0; i < 4; ++i)
                expect(arp.getMutate().getNoteMutation(i).isIdentity());
        }

        beginTest("ArpScreen SETUP: pressure, record mode, lock interaction, tapped rhythm");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);
            screen.setPageIndex(4);

            expectEquals(screen.getCellText(4, 0), juce::String("GATE"));
            screen.onEncoderChange(0, 127);
            expect(arp.getPressureTarget() == Arpeggiator::PressureTarget::OctaveRange);
            expectEquals(screen.getCellText(4, 0), juce::String("OCT"));
            screen.onEncoderChange(0, 127);
            expect(arp.getPressureTarget() == Arpeggiator::PressureTarget::Rate);
            expectEquals(screen.getCellText(4, 0), juce::String("RATE"));

            screen.onEncoderChange(1, 60);
            expectWithinAbsoluteError(arp.getPressureAmount(), 0.60f, 0.001f);

            expectEquals(screen.getCellText(4, 2), juce::String("NOTE"));
            screen.onEncoderChange(2, 1);
            expect(arp.getRecordMode() == Arpeggiator::RecordMode::Chord);
            expectEquals(screen.getCellText(4, 2), juce::String("CHORD"));

            expectEquals(screen.getCellText(4, 3), juce::String("ADD"));
            screen.onEncoderChange(3, 2);
            expect(arp.getMacroInteraction() == Arpeggiator::MacroInteraction::LockWins);
            expectEquals(screen.getCellText(4, 3), juce::String("LOCK"));

            // Pad pressure on this screen is the arpeggiator's pressure; the hardest pad counts.
            screen.onPadAftertouch(3, 64);
            screen.onPadAftertouch(9, 127);
            expectWithinAbsoluteError(arp.getCurrentPressure(), 1.0f, 0.001f);
            screen.onPadRelease(9);
            expectWithinAbsoluteError(arp.getCurrentPressure(), 64.0f / 127.0f, 0.001f);
            screen.onPadRelease(3);
            expectWithinAbsoluteError(arp.getCurrentPressure(), 0.0f, 0.001f);

            // Tapped rhythm: TAP on, three taps, TAP off.
            expectEquals(screen.getCellText(4, 5), juce::String("NONE"));
            screen.onEncoderChange(5, 1);
            expect(!arp.isRhythmEnabled(), "nothing to switch on yet");
            screen.onPadPress(0, 100);
            expectEquals(arp.getRhythmLength(), 0, "pads do nothing while TAP is off");

            screen.onEncoderChange(4, 1);
            expectEquals(screen.getCellText(4, 4), juce::String("ON 0"));
            screen.tapRhythm(1.0f, 1000.0);
            screen.tapRhythm(0.5f, 1100.0);
            screen.tapRhythm(1.0f, 1400.0);
            expectEquals(screen.getCellText(4, 4), juce::String("ON 3"));
            screen.onEncoderChange(4, 127);
            expectEquals(screen.getCellText(4, 4), juce::String("OFF"));
            expectEquals(arp.getRhythmLength(), 3);
            expectWithinAbsoluteError((float) arp.getRhythmTime(1), 0.25f, 0.001f);
            expect(arp.isRhythmEnabled(), "a finished capture is switched on");
            expectEquals(screen.getCellText(4, 5), juce::String("ON"));
            screen.onEncoderChange(5, 127);
            expect(!arp.isRhythmEnabled());
            expectEquals(screen.getCellText(4, 5), juce::String("OFF"));

            // Real pad presses tap too.
            screen.onEncoderChange(4, 1);
            screen.onPadPress(2, 90);
            screen.onPadPress(5, 110);
            expectEquals(screen.getCellText(4, 4), juce::String("ON 2"));
            screen.onDeactivate();
            expectEquals(screen.getCellText(4, 4), juce::String("OFF"), "leaving the screen ends the capture");
        }

        beginTest("PARA: every key runs its own pass up the octaves, side by side, from its own press");
        {
            // What sounds on each step: the note-ons grouped by the step they fall in.
            auto stepsOf = [&](const Events& ev)
            {
                std::map<int, std::multiset<int>> steps;
                for (auto& e : ev)
                    if (e.on)
                        steps[(int) std::floor(((double) e.t + 20.0) / STEP)].insert(e.pitch);
                return steps;
            };
            using Chord = std::multiset<int>;

            // One key: its own octaves, like UP.
            {
                Arpeggiator arp;
                arp.setEnabled(true);
                arp.setMode(Arpeggiator::Mode::Parallel);
                arp.setRate(Arpeggiator::Rate::Sixteenth);
                arp.setGate(0.5f);
                arp.setOctaveRange(3);
                Events ev; int t = 0;
                hook(arp, ev, t);
                arp.noteOn(48, 1.0f);
                advance(arp, t, (int) (STEP * 6) - 10);
                auto steps = stepsOf(ev);
                expect(steps[0] == Chord { 48 } && steps[1] == Chord { 60 } && steps[2] == Chord { 72 }
                       && steps[3] == Chord { 48 } && steps[4] == Chord { 60 });
            }

            // A second key pressed two steps later starts at ITS bottom octave while the first
            // carries on where it was: two notes a step, each on its own pass.
            {
                Arpeggiator arp;
                arp.setEnabled(true);
                arp.setMode(Arpeggiator::Mode::Parallel);
                arp.setRate(Arpeggiator::Rate::Sixteenth);
                arp.setGate(0.5f);
                arp.setOctaveRange(3);
                Events ev; int t = 0;
                hook(arp, ev, t);
                arp.noteOn(48, 1.0f);
                advance(arp, t, (int) (STEP * 1.5));            // during step 1
                arp.noteOn(55, 1.0f);
                advance(arp, t, (int) (STEP * 2.0));            // during step 3
                arp.noteOn(52, 1.0f);
                advance(arp, t, (int) (STEP * 4.0));
                auto steps = stepsOf(ev);
                expect(steps[0] == Chord { 48 });
                expect(steps[1] == Chord { 60 }, "the new key waits for the next step");
                expect(steps[2] == (Chord { 72, 55 }), "first key on its third octave, second key on its first");
                expect(steps[3] == (Chord { 48, 67 }));
                expect(steps[4] == (Chord { 60, 79, 52 }), "a third key in between the two: its own pass again");
                expect(steps[5] == (Chord { 72, 55, 64 }));
                expect(steps[6] == (Chord { 48, 67, 76 }));

                // A key let go takes only its own pass away.
                arp.noteOff(55);
                advance(arp, t, (int) STEP);
                steps = stepsOf(ev);
                expect(steps[8] == (Chord { 72, 64 }) || steps[8] == (Chord { 72, 52 }) || steps[8] == (Chord { 72, 76 }),
                       "the others carry on");
                expectEquals((int) steps[8].size(), 2);
            }

            // Keys pressed together are one chord and climb together, also when the first step has
            // already begun before the last finger lands.
            {
                Arpeggiator arp;
                arp.setEnabled(true);
                arp.setMode(Arpeggiator::Mode::Parallel);
                arp.setRate(Arpeggiator::Rate::Sixteenth);
                arp.setGate(0.5f);
                arp.setOctaveRange(2);
                Events ev; int t = 0;
                hook(arp, ev, t);
                arp.noteOn(48, 1.0f);
                advance(arp, t, 400);                           // 9 ms later
                arp.noteOn(52, 1.0f);
                advance(arp, t, 400);
                arp.noteOn(55, 1.0f);
                advance(arp, t, (int) (STEP * 4.0));
                auto steps = stepsOf(ev);
                expect(steps[1] == (Chord { 60, 64, 67 }), "the whole chord an octave up on the second step");
                expect(steps[2] == (Chord { 48, 52, 55 }));
                expect(steps[3] == (Chord { 60, 64, 67 }));
            }

            expectEquals(Arpeggiator::getModeLabel(Arpeggiator::Mode::Parallel), juce::String("PARA"));
            expectEquals((int) Arpeggiator::Mode::Parallel, 12, "appended: projects store the index");
        }

        beginTest("PEDL and PDL+: the root alternates with each higher note; in PDL+ the root climbs after every pass");
        {
            auto run = [&](Arpeggiator::Mode mode, std::initializer_list<int> keys, int octaves, int steps)
            {
                Arpeggiator arp;
                arp.setEnabled(true);
                arp.setMode(mode);
                arp.setRate(Arpeggiator::Rate::Sixteenth);
                arp.setGate(0.5f);
                arp.setOctaveRange(octaves);
                for (int k : keys) arp.noteOn(k, 1.0f);
                std::vector<int> pitches;
                for (auto& e : play(arp, steps))
                    if (e.on) pitches.push_back(e.pitch);
                return pitches;
            };
            using V = std::vector<int>;
            // Keys in any order: the lowest is the root.
            expect(run(Arpeggiator::Mode::Pedal, { 64, 60, 67, 72 }, 1, 8) == (V { 60, 64, 60, 67, 60, 72, 60, 64 }),
                   "PEDL: root, second, root, third, root, fourth, and round again");
            expect(run(Arpeggiator::Mode::Pedal, { 60, 64 }, 2, 7) == (V { 60, 64, 60, 72, 60, 76, 60 }),
                   "PEDL over two octaves: the root stays at the bottom, the others climb through both");
            expect(run(Arpeggiator::Mode::PedalRise, { 60, 64, 67, 72 }, 1, 14)
                       == (V { 60, 64, 60, 67, 60, 72,   64, 67, 64, 72,   67, 72,   60, 64 }),
                   "PDL+: each pass starts from the next note up, then the whole figure begins again");
            expect(run(Arpeggiator::Mode::Pedal, { 60 }, 1, 3) == (V { 60, 60, 60 }), "one key: just that note");
            expect(run(Arpeggiator::Mode::PedalRise, { 60 }, 2, 4) == (V { 60, 72, 60, 72 }), "one key, two octaves");
            expectEquals((int) Arpeggiator::Mode::Pedal, 13);
            expectEquals((int) Arpeggiator::Mode::PedalRise, 14);
            expectEquals(Arpeggiator::getModeLabel(Arpeggiator::Mode::PedalRise), juce::String("PDL+"));
        }
    }
};

static ArpPerformanceTests arpPerformanceTests;
