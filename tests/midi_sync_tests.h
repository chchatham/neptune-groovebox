#pragma once

#include <juce_core/juce_core.h>
#include "../src/sync/MIDIClock.h"
#include "../src/sync/MIDIRouter.h"
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"

class MIDISyncTests : public juce::UnitTest
{
public:
    MIDISyncTests() : juce::UnitTest("MIDI Sync", "Sync") {}

    void runTest() override
    {
        beginTest("MIDIClock rejects controller ports");
        {
            expect(MIDIClock::isControllerPort("Launchkey Mini MK4 DAW Out"));
            expect(MIDIClock::isControllerPort("Launchkey Mini MK4 MIDI Out"));
            expect(!MIDIClock::isControllerPort("IAC Driver Bus 1"));
            expect(!MIDIClock::isControllerPort("Neptune Clock In"));
        }

        beginTest("External clock ticks advance the beat");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::External);
            clock.prepareToPlay(48000.0, 512);

            // Start message.
            clock.handleMidiMessage(juce::MidiMessage(0xFA));
            expect(clock.isRunning());

            // 24 clock ticks = 1 beat.
            for (int i = 0; i < 24; ++i)
            {
                clock.processBlock(100, 48000.0);
                clock.handleMidiMessage(juce::MidiMessage(0xF8));
            }

            expectWithinAbsoluteError(clock.getCurrentBeat(), 1.0, 0.01);
        }

        beginTest("External clock detects tempo");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::External);
            clock.prepareToPlay(48000.0, 512);
            clock.handleMidiMessage(juce::MidiMessage(0xFA));

            // At 120 BPM, one beat = 0.5 s = 24000 samples at 48 kHz.
            // So one tick every 1000 samples.
            for (int i = 0; i < 48; ++i)
            {
                clock.processBlock(1000, 48000.0);
                clock.handleMidiMessage(juce::MidiMessage(0xF8));
            }

            double detected = clock.getDetectedTempo();
            expectWithinAbsoluteError(detected, 120.0, 2.0);
        }

        beginTest("handleMidiMessageFromPort rejects Launchkey");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::External);
            clock.prepareToPlay(48000.0, 512);

            expect(!clock.handleMidiMessageFromPort("Launchkey Mini MK4 DAW Out",
                                                     juce::MidiMessage(0xFA)));
            expect(!clock.isRunning());

            expect(clock.handleMidiMessageFromPort("IAC Driver Bus 1",
                                                    juce::MidiMessage(0xFA)));
            expect(clock.isRunning());
        }

        beginTest("Stop message stops external clock");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::External);
            clock.prepareToPlay(48000.0, 512);

            clock.handleMidiMessage(juce::MidiMessage(0xFA));
            expect(clock.isRunning());

            clock.handleMidiMessage(juce::MidiMessage(0xFC));
            expect(!clock.isRunning());
        }

        beginTest("Song position pointer sets beat position");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::External);
            clock.prepareToPlay(48000.0, 512);

            // SPP value 4 = 4 sixteenth-notes = 1 beat.
            clock.handleMidiMessage(juce::MidiMessage::songPositionPointer(4));
            expectWithinAbsoluteError(clock.getCurrentBeat(), 1.0, 0.01);
        }

        beginTest("Auto mode follows external clock when detected");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::Auto);
            clock.prepareToPlay(48000.0, 512);

            expect(clock.isAutoModeSending());

            clock.handleMidiMessage(juce::MidiMessage(0xF8));
            clock.processBlock(512, 48000.0);
            expect(!clock.isAutoModeSending());
        }

        beginTest("The clock port passes clock, transport and song position on, and nothing else");
        {
            MIDIRouter router;
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::External);
            clock.prepareToPlay(48000.0, 64);
            // Not opened: no port is made in the test; the port's handler is called as the port would.
            router.handleIncomingMidiMessage(nullptr, juce::MidiMessage(0xFA));
            expect(! clock.isRunning(), "a router without a clock does nothing");

            MIDIRouter wired;
            wired.attach(&clock);
            wired.handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
            expect(! clock.isRunning());
            wired.handleIncomingMidiMessage(nullptr, juce::MidiMessage(0xFA));
            expect(clock.isRunning());
            wired.handleIncomingMidiMessage(nullptr, juce::MidiMessage(0xF8));
            expectEquals(clock.getExternalTickCount(), 1);
            wired.handleIncomingMidiMessage(nullptr, juce::MidiMessage(0xFC));
            expect(! clock.isRunning());
        }

        beginTest("CLK EXT: Start, tempo and Stop of an external clock move the sequencer, and it stays on the clock's beat");
        {
            Follow f(MIDIClock::Mode::External);
            f.run(0.5);
            expect(! f.seq.isPlaying(), "nothing plays before the clock's Start");

            f.start();
            f.run(0.02, 97.3);
            expect(f.seq.isPlaying(), "the first tick after Start starts the transport");

            f.run(30.0, 97.3);
            expectWithinAbsoluteError((double) f.seq.getTempo(), 97.3, 0.5);
            expectWithinAbsoluteError(f.seq.getBeatPosition(), f.clockBeat(), 0.03);

            // The clock's tempo changes: the sequencer goes with it and is on the beat again.
            f.run(30.0, 140.0);
            expectWithinAbsoluteError((double) f.seq.getTempo(), 140.0, 0.7);
            expectWithinAbsoluteError(f.seq.getBeatPosition(), f.clockBeat(), 0.03);

            f.stop();
            f.run(0.05);
            expect(! f.seq.isPlaying(), "Stop stops it");

            // Start again: from the top, on the beat.
            f.start();
            f.run(10.0, 120.0);
            expect(f.seq.isPlaying());
            expectWithinAbsoluteError(f.seq.getBeatPosition(), f.clockBeat(), 0.03);
        }

        beginTest("CLK EXT: Play on the controller still works without a clock");
        {
            Follow f(MIDIClock::Mode::External);
            f.seq.setTempo(111.0f);
            f.seq.start();
            f.run(1.0);
            expect(f.seq.isPlaying());
            expectEquals(f.seq.getTempo(), 111.0f);
        }

        beginTest("CLK INT: an external clock is ignored");
        {
            Follow f(MIDIClock::Mode::Internal);
            f.seq.setTempo(111.0f);
            f.start();
            f.run(3.0, 90.0);
            expect(! f.seq.isPlaying());
            expectEquals(f.seq.getTempo(), 111.0f);
        }

        beginTest("CLK AUTO: follows a clock while it comes in, keeps playing at that tempo when it goes away, sends again");
        {
            Follow f(MIDIClock::Mode::Auto);
            f.seq.setTempo(111.0f);
            expect(f.clock.isAutoModeSending());
            f.start();
            f.run(10.0, 90.0);
            expect(! f.clock.isAutoModeSending());
            expect(f.seq.isPlaying());
            expectWithinAbsoluteError((double) f.seq.getTempo(), 90.0, 0.5);

            f.run(2.0);                          // the cable is pulled: no Stop, no ticks
            expect(f.clock.isAutoModeSending(), "after a second without ticks this clock sends again");
            expect(f.seq.isPlaying(), "and the music goes on");
            expectWithinAbsoluteError((double) f.seq.getTempo(), 90.0, 3.0);

            f.run(5.0, 126.0);                   // and comes back
            expect(! f.clock.isAutoModeSending());
            expectWithinAbsoluteError((double) f.seq.getTempo(), 126.0, 0.7);

            // A tempo change keeps the sequencer where it is in its beat (nothing jumps).
            SequencerCore seq;
            seq.prepareToPlay(48000.0, 64);
            seq.setTempo(90.0f);
            seq.start();
            for (int i = 0; i < 200; ++i) seq.processBlock(64);
            const double before = seq.getBeatPosition();
            seq.setTempo(180.0f);
            seq.processBlock(64);
            expectWithinAbsoluteError(seq.getBeatPosition(), before + 64.0 / 48000.0 * 3.0, 0.0001);
        }
    }

private:
    // A sequencer and an audio engine fed 64-sample blocks at 48 kHz, and an external clock whose
    // ticks arrive between blocks with the time stamps a MIDI input gives them (and 1 ms of jitter).
    struct Follow
    {
        explicit Follow(MIDIClock::Mode mode)
        {
            clock.setMode(mode);
            seq.prepareToPlay(48000.0, 64);
            audio.setSequencer(&seq);
            audio.setMIDIClock(&clock);
            audio.prepare(48000.0, 64);
            clock.prepareToPlay(48000.0, 64);
        }

        void start() { clock.handleMidiMessage(stamped(0xFA, now)); ticksSent = 0; nextTick = now; sinceStart = 0.0; }
        void stop()  { clock.handleMidiMessage(stamped(0xFC, now)); }

        // `bpm` 0: no ticks.
        void run(double seconds, double bpm = 0.0)
        {
            const double end = now + seconds;
            if (bpm > 0.0 && nextTick < now) nextTick = now;
            while (now < end)
            {
                while (bpm > 0.0 && nextTick <= now)
                {
                    const double jitter = ((ticksSent * 7919) % 200 - 100) * 0.00001;      // +- 1 ms
                    clock.handleMidiMessage(stamped(0xF8, nextTick + jitter));
                    if (ticksSent > 0) sinceStart += lastBpm > 0.0 ? 1.0 / 24.0 : 0.0;
                    ++ticksSent;
                    lastBpm = bpm;
                    lastTickAt = nextTick;
                    nextTick += 60.0 / bpm / 24.0;
                }
                audio.renderBlock(left, right, 64);
                now += 64.0 / 48000.0;
            }
        }

        // Where the clock is, in quarter notes since the first tick after Start.
        double clockBeat() const { return sinceStart + (now - lastTickAt) * lastBpm / 60.0; }

        static juce::MidiMessage stamped(int status, double seconds)
        {
            juce::MidiMessage m(status);
            m.setTimeStamp(seconds);
            return m;
        }

        MIDIClock clock;
        SequencerCore seq;
        AudioEngine audio;
        float left[64] {}, right[64] {};
        double now = 1000.0, nextTick = 0.0, lastTickAt = 0.0, lastBpm = 0.0, sinceStart = 0.0;
        int ticksSent = 0;
    };
};

static MIDISyncTests midiSyncTests;
