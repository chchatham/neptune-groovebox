#pragma once

#include "MIDIClock.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <memory>

// A virtual MIDI port, "Neptune Clock In", that any program on this Mac can send MIDI clock to
// (in Ableton Live: Settings -> Link, Tempo & MIDI -> Output "Neptune Clock In" -> Sync on).
// Clock, Start / Stop / Continue and song position go to the MIDIClock, which follows them when
// SETTINGS -> CLK is EXT or AUTO; everything else arriving on the port is ignored.
class MIDIRouter : private juce::MidiInputCallback
{
public:
    static constexpr const char* CLOCK_PORT_NAME = "Neptune Clock In";

    MIDIRouter() = default;
    ~MIDIRouter() override { close(); }

    // The clock the port's messages go to (open() sets it too).
    void attach(MIDIClock* clock) { midiClock = clock; }

    void open(MIDIClock* clock)
    {
        attach(clock);
#if JUCE_MAC || JUCE_LINUX
        virtualInput = juce::MidiInput::createNewDevice(CLOCK_PORT_NAME, this);
        if (virtualInput)
        {
            virtualInput->start();
            juce::Logger::writeToLog("[MIDIRouter] Virtual input opened: Neptune Clock In");
        }
        else
            juce::Logger::writeToLog("[MIDIRouter] Failed to create virtual MIDI input");
#endif
    }

    void close()
    {
        if (virtualInput)
        {
            virtualInput->stop();
            virtualInput.reset();
        }
    }

    bool isClockInputOpen() const { return virtualInput != nullptr; }

    // What the port does with a message (MIDI thread). Public for the tests.
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& msg) override
    {
        if (midiClock == nullptr)
            return;
        if (msg.isMidiClock() || msg.isMidiStart() || msg.isMidiStop()
            || msg.isMidiContinue() || msg.isSongPositionPointer())
            midiClock->handleMidiMessage(msg);
    }

private:
    MIDIClock* midiClock = nullptr;
    std::unique_ptr<juce::MidiInput> virtualInput;
};
