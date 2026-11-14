// ============================================================================
// MIDI Track Sequencer Output Tests
// ============================================================================
// Add to main_test.cpp:
//   #include "midi_track_output_tests.h"
//   static MIDITrackOutputTests midiTrackOutputTests;
//   Add to test filter: || name == "MIDI Track Output"
//
// COORDINATOR NOTE — AudioEngine MIDI output wiring needed:
// In AudioEngine::audioDeviceIOCallbackWithContext(), the per-track
// processBlock currently uses a local `juce::MidiBuffer emptyMidi` that is
// discarded after each engine renders (line ~334). For MIDITrack engines,
// the MIDI output must be forwarded to a physical MIDI output device.
//
// Suggested approach:
//   1. Add a member `juce::MidiBuffer midiOutputBuffer;` to AudioEngine.
//   2. Add a setter `void setMidiOutput(juce::MidiOutput* output)`.
//   3. In the per-track loop, after calling e->processBlock(trackBuf, emptyMidi),
//      check if `e->getType() == EngineType::MIDITrack` and if so,
//      append emptyMidi's contents into midiOutputBuffer.
//   4. After the track loop, send midiOutputBuffer to the MidiOutput device
//      and clear it.
//
// This keeps the audio thread allocation-free (MidiBuffer uses pre-allocated
// memory) and respects the lock-free design.
// ============================================================================

#pragma once

#include "../src/engine/MIDITrack.h"
#include "../src/sequencer/SequencerCore.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

class MIDITrackOutputTests : public juce::UnitTest
{
public:
    MIDITrackOutputTests() : UnitTest("MIDI Track Output") {}

    void runTest() override
    {
        // ------------------------------------------------------------------
        // Test 1: MIDI track trig produces noteOn in MIDI output buffer
        // ------------------------------------------------------------------
        beginTest("MIDI track trig produces noteOn in MIDI output buffer");
        {
            // Simulate what happens when the sequencer fires a trig on a
            // MIDI track: the trig callback calls engine->noteOn(), and the
            // next processBlock drains the ring buffer into a MidiBuffer.
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            // Simulate sequencer trig: note 64, velocity 0.75
            engine.noteOn(64, 0.75f, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            expect(!midiOut.isEmpty(), "processBlock should produce MIDI output after noteOn");

            bool foundNoteOn = false;
            int noteOnChannel = -1;
            int noteOnNote = -1;
            int noteOnVel = -1;

            for (auto meta : midiOut)
            {
                auto msg = meta.getMessage();
                if (msg.isNoteOn() && msg.getNoteNumber() == 64)
                {
                    foundNoteOn = true;
                    noteOnChannel = msg.getChannel();
                    noteOnNote = msg.getNoteNumber();
                    noteOnVel = msg.getVelocity();
                }
            }

            expect(foundNoteOn, "Should find noteOn for pitch 64 in MIDI output");
            expectEquals(noteOnNote, 64);
            // 0.75 * 127 = 95.25 -> 95
            expectEquals(noteOnVel, 95);
            expectEquals(noteOnChannel, 1); // default channel
        }

        // ------------------------------------------------------------------
        // Test 2: MIDI track trig produces noteOff after step length
        // ------------------------------------------------------------------
        beginTest("MIDI track trig produces noteOff after step length");
        {
            // The sequencer schedules note-offs via its PendingNoteOff system.
            // When the scheduled time elapses, onNoteOff callback fires
            // engine->noteOff(). Verify this produces a noteOff in MIDI output.
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            // First: trigger the note
            engine.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            // Verify noteOn was produced
            bool hadNoteOn = false;
            for (auto meta : midiOut)
                if (meta.getMessage().isNoteOn() && meta.getMessage().getNoteNumber() == 60)
                    hadNoteOn = true;
            expect(hadNoteOn, "Precondition: noteOn should have been produced");

            // Now: simulate the sequencer firing noteOff after step duration
            engine.noteOff(60, -1);

            buffer.clear();
            juce::MidiBuffer midiOut2;
            engine.processBlock(buffer, midiOut2);

            bool foundNoteOff = false;
            for (auto meta : midiOut2)
            {
                auto msg = meta.getMessage();
                if (msg.isNoteOff() && msg.getNoteNumber() == 60)
                    foundNoteOff = true;
            }
            expect(foundNoteOff, "Should find noteOff for pitch 60 after step duration");
        }

        // ------------------------------------------------------------------
        // Test 3: CC parameter locks generate CC messages
        // ------------------------------------------------------------------
        beginTest("CC parameter locks generate CC messages");
        {
            // When the sequencer fires a trig with paramLocks, it calls
            // engine->setParameter() for each lock. For MIDITrackEngine,
            // parameters 0-127 map to CC numbers.
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            // Simulate param locks: CC1 (mod wheel) = 0.5, CC74 (filter) = 0.8
            engine.setParameter(1, 0.5f);    // CC1 -> value 63
            engine.setParameter(74, 0.8f);   // CC74 -> value 101

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            bool foundCC1 = false;
            bool foundCC74 = false;
            int cc1Value = -1;
            int cc74Value = -1;

            for (auto meta : midiOut)
            {
                auto msg = meta.getMessage();
                if (msg.isController())
                {
                    if (msg.getControllerNumber() == 1)
                    {
                        foundCC1 = true;
                        cc1Value = msg.getControllerValue();
                    }
                    if (msg.getControllerNumber() == 74)
                    {
                        foundCC74 = true;
                        cc74Value = msg.getControllerValue();
                    }
                }
            }

            expect(foundCC1, "Should find CC1 message from param lock");
            expect(foundCC74, "Should find CC74 message from param lock");
            expectEquals(cc1Value, 63);    // 0.5 * 127 = 63.5 -> clamped to 63
            expectEquals(cc74Value, 101);  // 0.8 * 127 = 101.6 -> clamped to 101
        }

        // ------------------------------------------------------------------
        // Test 4: Multiple notes produce ordered MIDI output
        // ------------------------------------------------------------------
        beginTest("Multiple notes produce ordered MIDI output");
        {
            // When a polyphonic step fires, the sequencer calls noteOn for
            // each note in the step's notes array. All should appear in the
            // MIDI output in the order they were pushed.
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            // Push a chord: C4, E4, G4
            engine.noteOn(60, 0.9f, -1);  // C4
            engine.noteOn(64, 0.7f, -1);  // E4
            engine.noteOn(67, 0.5f, -1);  // G4

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            // Collect all noteOn messages in order
            std::vector<int> noteNumbers;
            std::vector<int> velocities;
            for (auto meta : midiOut)
            {
                auto msg = meta.getMessage();
                if (msg.isNoteOn())
                {
                    noteNumbers.push_back(msg.getNoteNumber());
                    velocities.push_back(msg.getVelocity());
                }
            }

            expectEquals((int) noteNumbers.size(), 3);

            if (noteNumbers.size() == 3)
            {
                // Verify all three notes present in order
                expectEquals(noteNumbers[0], 60);
                expectEquals(noteNumbers[1], 64);
                expectEquals(noteNumbers[2], 67);

                // Verify velocities: 0.9*127=114, 0.7*127=88, 0.5*127=63
                expectEquals(velocities[0], 114);
                expectEquals(velocities[1], 88);
                expectEquals(velocities[2], 63);
            }
        }

        // ------------------------------------------------------------------
        // Test 5: Note-off for re-triggered note cancels previous pending note
        // ------------------------------------------------------------------
        beginTest("Re-triggered note sends note-off then note-on");
        {
            // When the same note is re-triggered (e.g., consecutive steps with
            // the same pitch), the sequencer's scheduleNoteOff cancels the
            // old pending note-off. The engine should receive noteOff then
            // noteOn to cleanly re-trigger. Verify both appear in output.
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            // First trigger
            engine.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut1;
            engine.processBlock(buffer, midiOut1);

            // Re-trigger same note: noteOff followed by noteOn
            engine.noteOff(60, -1);
            engine.noteOn(60, 0.8f, -1);

            buffer.clear();
            juce::MidiBuffer midiOut2;
            engine.processBlock(buffer, midiOut2);

            // Collect events in order
            bool foundNoteOff = false;
            bool foundNoteOn = false;
            int noteOffSample = -1;
            int noteOnSample = -1;

            for (auto meta : midiOut2)
            {
                auto msg = meta.getMessage();
                if (msg.getNoteNumber() == 60)
                {
                    if (msg.isNoteOff())
                    {
                        foundNoteOff = true;
                        noteOffSample = meta.samplePosition;
                    }
                    if (msg.isNoteOn())
                    {
                        foundNoteOn = true;
                        noteOnSample = meta.samplePosition;
                    }
                }
            }

            expect(foundNoteOff, "Re-trigger should produce noteOff for old note");
            expect(foundNoteOn, "Re-trigger should produce noteOn for new note");

            // Both events are at sample position 0 (ring buffer is drained at
            // block start), but noteOff should precede noteOn in iteration order
            // because noteOff was pushed first.
            expect(noteOffSample <= noteOnSample,
                   "noteOff should appear at or before noteOn in the MIDI buffer");
        }

        // ------------------------------------------------------------------
        // Test 6: MIDI channel is respected in output messages
        // ------------------------------------------------------------------
        beginTest("MIDI channel is respected in all output message types");
        {
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);
            engine.setMidiChannel(5);

            // Push note-on, CC, and note-off
            engine.noteOn(72, 0.6f, -1);
            engine.setParameter(7, 0.9f);  // CC7 (volume)
            engine.noteOff(72, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            int messageCount = 0;
            for (auto meta : midiOut)
            {
                auto msg = meta.getMessage();
                expectEquals(msg.getChannel(), 5);
                messageCount++;
            }

            expect(messageCount >= 3,
                   "Should have at least 3 messages (noteOn, CC, noteOff)");
        }

        // ------------------------------------------------------------------
        // Test 7: allNotesOff sends 128 note-offs
        // ------------------------------------------------------------------
        beginTest("allNotesOff sends note-off for all 128 MIDI notes");
        {
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);
            engine.setMidiChannel(3);

            engine.allNotesOff();

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            int noteOffCount = 0;
            std::vector<bool> noteSeen(128, false);
            for (auto meta : midiOut)
            {
                auto msg = meta.getMessage();
                if (msg.isNoteOff())
                {
                    noteOffCount++;
                    int n = msg.getNoteNumber();
                    if (n >= 0 && n < 128)
                        noteSeen[(size_t) n] = true;
                    expectEquals(msg.getChannel(), 3);
                }
            }

            expectEquals(noteOffCount, 128);
            for (int i = 0; i < 128; ++i)
                expect(noteSeen[(size_t) i],
                       "Should have note-off for note " + juce::String(i));
        }

        // ------------------------------------------------------------------
        // Test 8: Ring buffer handles full cycle without corruption
        // ------------------------------------------------------------------
        beginTest("Ring buffer handles many events across multiple blocks");
        {
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            int totalNoteOns = 0;
            int totalNoteOffs = 0;

            // Push events across multiple processBlock calls to stress the
            // ring buffer wrap-around
            for (int cycle = 0; cycle < 10; ++cycle)
            {
                // Push 20 note-on events
                for (int n = 0; n < 20; ++n)
                    engine.noteOn(40 + n, 0.5f, -1);

                juce::AudioBuffer<float> buffer(2, 512);
                buffer.clear();
                juce::MidiBuffer midiOut;
                engine.processBlock(buffer, midiOut);

                for (auto meta : midiOut)
                    if (meta.getMessage().isNoteOn())
                        totalNoteOns++;

                // Push 20 note-off events
                for (int n = 0; n < 20; ++n)
                    engine.noteOff(40 + n, -1);

                buffer.clear();
                juce::MidiBuffer midiOut2;
                engine.processBlock(buffer, midiOut2);

                for (auto meta : midiOut2)
                    if (meta.getMessage().isNoteOff())
                        totalNoteOffs++;
            }

            expectEquals(totalNoteOns, 200);
            expectEquals(totalNoteOffs, 200);
        }

        // ------------------------------------------------------------------
        // Test 9: Sequencer trig callback integration with MIDITrack
        // ------------------------------------------------------------------
        beginTest("Sequencer trig callback fires noteOn on MIDITrack engine");
        {
            // End-to-end: set up a SequencerCore with a trig callback that
            // routes to a MIDITrackEngine, verify output after playback.
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            seq.setTempo(120.0f);

            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);
            engine.setMidiChannel(10);

            // Configure step 0 with a trig
            auto& track = seq.getPattern().getTrack(0);
            track.getStep(0).trig = true;
            track.getStep(0).notes.push_back({36, 1.0f}); // kick drum on ch 10

            // Wire trig callback to engine
            seq.setTrigCallback([&engine](int trackIndex, const Step& step) {
                if (trackIndex == 0)
                {
                    // Apply param locks
                    for (auto& [paramId, value] : step.paramLocks)
                        engine.setParameter(paramId, value);

                    // Fire notes
                    for (auto& note : step.notes)
                        engine.noteOn(note.pitch, note.velocity, -1);
                }
            });

            seq.setNoteOffCallback([&engine](int trackIndex, int note) {
                if (trackIndex == 0)
                    engine.noteOff(note, -1);
            });

            // Start playback and advance enough for step 0 to fire
            seq.start();

            // Process enough blocks for at least one step advance
            // At 120 BPM, 1 step = 1/4 beat = 0.125s = 5512.5 samples
            // We need to process a few blocks to get past step 0
            bool foundNoteOn = false;
            for (int block = 0; block < 30; ++block)
            {
                seq.processBlock(512);

                juce::AudioBuffer<float> buffer(2, 512);
                buffer.clear();
                juce::MidiBuffer midiOut;
                engine.processBlock(buffer, midiOut);

                for (auto meta : midiOut)
                {
                    auto msg = meta.getMessage();
                    if (msg.isNoteOn() && msg.getNoteNumber() == 36)
                    {
                        foundNoteOn = true;
                        expectEquals(msg.getChannel(), 10);
                    }
                }

                if (foundNoteOn) break;
            }

            expect(foundNoteOn,
                   "Sequencer trig should produce noteOn on MIDI track engine");
        }

        // ------------------------------------------------------------------
        // Test 10: Param locks on MIDI track generate CC alongside noteOn
        // ------------------------------------------------------------------
        beginTest("Param locks on MIDI track generate CC alongside noteOn");
        {
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);
            engine.setMidiChannel(2);

            // Simulate what the trig callback does: apply param locks then noteOn
            // This mirrors how the sequencer fires: locks first, then notes
            engine.setParameter(11, 0.6f);  // CC11 (expression) = 76
            engine.setParameter(1, 0.3f);   // CC1 (mod wheel) = 38
            engine.noteOn(60, 0.8f, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            bool foundCC11 = false;
            bool foundCC1 = false;
            bool foundNoteOn = false;

            for (auto meta : midiOut)
            {
                auto msg = meta.getMessage();
                expectEquals(msg.getChannel(), 2);

                if (msg.isController() && msg.getControllerNumber() == 11)
                {
                    foundCC11 = true;
                    expectEquals(msg.getControllerValue(), 76);
                }
                if (msg.isController() && msg.getControllerNumber() == 1)
                {
                    foundCC1 = true;
                    expectEquals(msg.getControllerValue(), 38);
                }
                if (msg.isNoteOn() && msg.getNoteNumber() == 60)
                    foundNoteOn = true;
            }

            expect(foundCC11, "Should find CC11 from param lock");
            expect(foundCC1, "Should find CC1 from param lock");
            expect(foundNoteOn, "Should find noteOn alongside CC param locks");
        }

        // ------------------------------------------------------------------
        // Test 11: getPendingEventCount reflects queued events
        // ------------------------------------------------------------------
        beginTest("getPendingEventCount tracks queued events accurately");
        {
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            expectEquals(engine.getPendingEventCount(), 0);

            engine.noteOn(60, 1.0f, -1);
            expectEquals(engine.getPendingEventCount(), 1);

            engine.noteOn(64, 1.0f, -1);
            engine.noteOff(60, -1);
            expectEquals(engine.getPendingEventCount(), 3);

            // Drain
            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            expectEquals(engine.getPendingEventCount(), 0);
        }

        // ------------------------------------------------------------------
        // Test 12: MIDI output buffer produces no audio
        // ------------------------------------------------------------------
        beginTest("MIDI track produces zero audio output regardless of events");
        {
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            // Fill the engine with various events
            engine.noteOn(60, 1.0f, -1);
            engine.noteOn(64, 1.0f, -1);
            engine.setParameter(74, 0.9f);
            engine.noteOff(60, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            float maxAbs = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buffer.getReadPointer(ch);
                for (int i = 0; i < 512; ++i)
                    maxAbs = std::max(maxAbs, std::abs(data[i]));
            }

            expectEquals(maxAbs, 0.0f);
            expect(!midiOut.isEmpty(), "MIDI output should have events");
        }
    }
};

static MIDITrackOutputTests midiTrackOutputTests;
