#pragma once

#include "../src/controller/LaunchkeyController.h"
#include "../src/sync/MIDIClock.h"
#include <juce_core/juce_core.h>

// The controller and MIDI clock. The Launchkey sends MIDI clock by itself (device setting "MIDI Clock
// Out", feature control 0x6E, on by default): the app switches that off at connect, and never treats
// the controller's own ports as a clock or transport source, whatever the CLK mode.
namespace clocktest
{
    // Counts everything the controller hands to its listeners.
    struct CountingListener : LaunchkeyController::Listener
    {
        int calls = 0;
        void noteOnReceived(int, int, int) override { ++calls; }
        void noteOffReceived(int, int) override { ++calls; }
        void controlChangeReceived(int, int, int) override { ++calls; }
        void aftertouchReceived(int, int, int) override { ++calls; }
        void pitchBendReceived(int) override { ++calls; }
        void sysExReceived(const juce::uint8*, int) override { ++calls; }
        void bitmapAckReceived() override { ++calls; }
        void padModeChanged(int) override { ++calls; }
        void encoderModeChanged(int) override { ++calls; }
    };

    static const char* const controllerPorts[] = { "Launchkey Mini MK4 25 MIDI Out", "Launchkey Mini MK4 25 DAW Out" };
    static const char* const otherPort = "Drum Machine MIDI 1";
}

class ControllerClockTests : public juce::UnitTest
{
public:
    ControllerClockTests() : UnitTest("Controller Clock") {}

    void runTest() override
    {
        using Mode = MIDIClock::Mode;
        const Mode modes[] = { Mode::Internal, Mode::External, Mode::Auto };

        beginTest("The connect sequence switches the device's clock output off");
        {
            const auto msgs = LaunchkeyController::neptuneConnectMessages();
            int clockOff = 0, relative = 0;
            for (const auto& m : msgs)
            {
                expect(m.isController(), "feature controls are control changes");
                expectEquals(m.getChannel(), 7, "feature controls go out on channel 7");
                if (m.getControllerNumber() == 0x6E)
                {
                    ++clockOff;
                    expectEquals(m.getControllerValue(), 0, "MIDI Clock Out is set to off");
                    expectEquals((int) m.getRawData()[0], 0xB6);
                }
                if (m.getControllerNumber() == 0x45 && m.getControllerValue() == 127)
                    ++relative;
            }
            expectEquals(clockOff, 1, "exactly one clock-output message");
            expectEquals(relative, 1, "relative encoders are still requested");
            expectEquals(LaunchkeyController::FC_MIDI_CLOCK_OUT, 0x6E);
        }

        beginTest("Clock and transport bytes from the controller reach no listener");
        {
            LaunchkeyController controller;
            clocktest::CountingListener listener;
            controller.addListener(&listener);
            juce::MidiInputCallback& in = controller;

            const juce::MidiMessage dropped[] = {
                juce::MidiMessage(0xF8), juce::MidiMessage(0xFA), juce::MidiMessage(0xFB), juce::MidiMessage(0xFC),
                juce::MidiMessage(0xFE), juce::MidiMessage::songPositionPointer(16)
            };
            for (const auto& m : dropped)
            {
                expect(LaunchkeyController::isClockOrSystemMessage(m));
                in.handleIncomingMidiMessage(nullptr, m);
            }
            expectEquals(listener.calls, 0, "no clock, start, continue, stop or song position is passed on");

            // Ordinary messages still arrive (Play is CC 115 on the DAW port).
            expect(! LaunchkeyController::isClockOrSystemMessage(juce::MidiMessage::controllerEvent(1, 115, 127)));
            in.handleIncomingMidiMessage(nullptr, juce::MidiMessage::controllerEvent(1, 115, 127));
            in.handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOn(1, 96, (juce::uint8) 100));
            expectEquals(listener.calls, 2);

            // The display's ready reply is a SysEx and is not taken for a system message.
            const juce::uint8 ready[] = { 0x00, 0x20, 0x29, 0x02, 0x13, 0x09 };
            const auto readyMsg = juce::MidiMessage::createSysExMessage(ready, 6);
            expect(! LaunchkeyController::isClockOrSystemMessage(readyMsg));
            in.handleIncomingMidiMessage(nullptr, readyMsg);
            expectEquals(listener.calls, 3);
            controller.removeListener(&listener);
        }

        beginTest("Clock from the controller's ports is ignored in every CLK mode");
        {
            for (auto mode : modes)
                for (auto* port : clocktest::controllerPorts)
                {
                    MIDIClock clock;
                    clock.prepareToPlay(48000.0, 64);
                    clock.setMode(mode);
                    clock.setTempo(97.0);

                    expect(MIDIClock::isControllerPort(port));
                    expect(! clock.handleMidiMessageFromPort(port, juce::MidiMessage(0xFA)));
                    for (int i = 0; i < 96; ++i)   // 48 ticks a second, as the device sends them
                    {
                        clock.processBlock(1000, 48000.0);
                        expect(! clock.handleMidiMessageFromPort(port, juce::MidiMessage(0xF8)));
                    }
                    expect(! clock.handleMidiMessageFromPort(port, juce::MidiMessage(0xFA)));

                    const juce::String where = juce::String("mode ") + juce::String((int) mode) + ", " + port;
                    expect(! clock.isExternalClockDetected(), where);
                    expect(! clock.isRunning(), where + ": a start from the controller starts nothing");
                    expect(clock.isAutoModeSending(), where + ": AUTO keeps the internal clock");
                    expectEquals(clock.getDetectedTempo(), 0.0, where);
                    expectEquals(clock.getTempo(), 97.0, where);
                }
        }

        beginTest("Clock from another port is honoured in External and Auto, not in Internal");
        {
            for (auto mode : modes)
            {
                MIDIClock clock;
                clock.prepareToPlay(48000.0, 64);
                clock.setMode(mode);

                expect(! MIDIClock::isControllerPort(clocktest::otherPort));
                for (int i = 0; i < 96; ++i)   // 1000 samples a tick at 48 kHz = 120 BPM
                {
                    clock.processBlock(1000, 48000.0);
                    expect(clock.handleMidiMessageFromPort(clocktest::otherPort, juce::MidiMessage(0xF8)));
                }
                clock.processBlock(64, 48000.0);
                clock.handleMidiMessageFromPort(clocktest::otherPort, juce::MidiMessage(0xFA));

                const juce::String where = juce::String("mode ") + juce::String((int) mode);
                if (mode == Mode::Internal)
                {
                    expect(! clock.isExternalClockDetected(), where);
                    expect(! clock.isRunning(), where);
                }
                else
                {
                    expect(clock.isExternalClockDetected(), where);
                    expect(clock.isRunning(), where + ": an external start runs the clock");
                    expectWithinAbsoluteError(clock.getDetectedTempo(), 120.0, 1.0, where);
                    if (mode == Mode::Auto)
                        expect(! clock.isAutoModeSending(), where + ": AUTO follows the external clock");
                }
            }
        }
    }
};

static ControllerClockTests controllerClockTests;
