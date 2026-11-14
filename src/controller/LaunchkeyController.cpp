#include <algorithm>
#include "LaunchkeyController.h"

LaunchkeyController::LaunchkeyController()
{
}

LaunchkeyController::~LaunchkeyController()
{
    exitDawMode();
    disconnect();
}

bool LaunchkeyController::scanAndConnect()
{
    if (isConnected())
        return true;

    if (findAndOpenPorts())
    {
        connected = true;
        listeners.call(&Listener::launchkeyConnected);
        startTimer(2000);
        return true;
    }
    return false;
}

void LaunchkeyController::disconnect()
{
    stopTimer();

    if (dawModeActive)
        exitDawMode();

    {
        juce::ScopedLock sl(midiLock);
        dawInput.reset();
        dawOutput.reset();
        midiInput.reset();
        midiOutput.reset();
    }

    if (connected)
    {
        connected = false;
        listeners.call(&Listener::launchkeyDisconnected);
    }
}

bool LaunchkeyController::enterDawMode()
{
    if (!dawOutput)
        return false;

    auto msg = juce::MidiMessage(0x9F, 0x0C, 0x7F);
    dawOutput->sendMessageNow(msg);
    dawModeActive = true;
    listeners.call(&Listener::dawModeEntered);
    return true;
}

void LaunchkeyController::exitDawMode()
{
    if (!dawOutput || !dawModeActive)
        return;

    auto msg = juce::MidiMessage(0x9F, 0x0C, 0x00);
    dawOutput->sendMessageNow(msg);
    dawModeActive = false;
    listeners.call(&Listener::dawModeExited);
}

void LaunchkeyController::enableFeatureControls()
{
    if (!dawOutput)
        return;
    auto msg = juce::MidiMessage(0x9F, 0x0B, 0x7F);
    dawOutput->sendMessageNow(msg);
}

void LaunchkeyController::disableFeatureControls()
{
    if (!dawOutput)
        return;
    auto msg = juce::MidiMessage(0x9F, 0x0B, 0x00);
    dawOutput->sendMessageNow(msg);
}

void LaunchkeyController::setFeatureControl(int cc, int value)
{
    if (!dawOutput)
        return;
    auto msg = juce::MidiMessage::controllerEvent(7, cc, value);
    dawOutput->sendMessageNow(msg);
}

void LaunchkeyController::disableFirmwareArp()
{
    setFeatureControl(FC_ARP_ONOFF, 0);
    juce::Logger::writeToLog("[LK] Firmware arp disabled");
}

std::vector<juce::MidiMessage> LaunchkeyController::neptuneConnectMessages()
{
    return {
        // Keys on channel 1 (ScreenManager treats channel-1 non-pad notes as the keyboard) and
        // polyphonic pad aftertouch (needs Neptune firmware NPB+ to be sent in the DAW pad layout).
        juce::MidiMessage::controllerEvent(7, FC_KEYS_CHANNEL, 0),
        juce::MidiMessage::controllerEvent(7, FC_PAD_AFTERTOUCH, 2),
        // The device's own MIDI clock output off. Its stock default is on: the firmware then sends F8
        // (24 a beat at its internal tempo, 120 BPM) on the MIDI port for as long as it is powered. The
        // app is the clock master and never takes clock from its controller. The setting is stored on
        // the device; it is sent on every connect all the same (a reset device comes back with it on).
        juce::MidiMessage::controllerEvent(7, FC_MIDI_CLOCK_OUT, 0),
        // Endless encoders as relative steps; without this the firmware sends running absolute values.
        juce::MidiMessage::controllerEvent(7, FC_ENCODER_RELATIVE, 127),
    };
}

void LaunchkeyController::configureNeptuneFirmware()
{
    if (dawOutput)
        for (const auto& msg : neptuneConnectMessages())
            dawOutput->sendMessageNow(msg);
    relativeEncoders = true;
    juce::Logger::writeToLog("[LK] Keys channel 1, pad aftertouch polyphonic, device clock output off, encoders relative");
}

void LaunchkeyController::disableFirmwareScale()
{
    setFeatureControl(FC_SCALE_MODE, 0);
    juce::Logger::writeToLog("[LK] Firmware scale mode disabled");
}

void LaunchkeyController::setButtonLED(int controlId, int r, int g, int b)
{
    if (!midiOutput) return;
    juce::uint32 color = ((juce::uint32)(r & 0xFF) << 16)
                       | ((juce::uint32)(g & 0xFF) << 8)
                       | (juce::uint32)(b & 0xFF);
    // Build the debug SysEx: call led_set(controlId, color) on the MIDI port.
    // Format: 00 20 29 00 7D <addr:5><a0:5><a1:5> (5×7-bit encoding, MSB first; addr with Thumb bit).
    std::vector<juce::uint8> body { 0x00, 0x20, 0x29, 0x00, 0x7D };
    for (juce::uint32 v : { FW_LED_SET | 1u, (juce::uint32) controlId, color })
        for (int s = 28; s >= 0; s -= 7)
            body.push_back((juce::uint8) ((v >> s) & 0x7F));
    const juce::ScopedLock sl(midiLock);
    midiOutput->sendMessageNow(juce::MidiMessage::createSysExMessage(body.data(), (int) body.size()));
}

void LaunchkeyController::illuminateNavigationButtons()
{
    for (int id : { BTN_UP, BTN_DOWN, BTN_PATTERN, BTN_FUNC })
        setButtonLED(id, 63, 63, 63);
    juce::Logger::writeToLog("[LK] Navigation button LEDs illuminated");
}

void LaunchkeyController::setPadColor(int padIndex, int paletteColor)
{
    if (!dawOutput)
        return;
    auto msg = juce::MidiMessage::noteOn(1, padIndex, (juce::uint8) paletteColor);
    dawOutput->sendMessageNow(msg);
}

void LaunchkeyController::setPadFlashing(int padIndex, int paletteColor)
{
    if (!dawOutput)
        return;
    auto msg = juce::MidiMessage::noteOn(2, padIndex, (juce::uint8) paletteColor);
    dawOutput->sendMessageNow(msg);
}

void LaunchkeyController::setPadPulsing(int padIndex, int paletteColor)
{
    if (!dawOutput)
        return;
    auto msg = juce::MidiMessage::noteOn(3, padIndex, (juce::uint8) paletteColor);
    dawOutput->sendMessageNow(msg);
}

void LaunchkeyController::setPadRGB(int padIndex, int r, int g, int b)
{
    sendSysEx({ 0x01, 0x43, (juce::uint8) padIndex,
                (juce::uint8) r, (juce::uint8) g, (juce::uint8) b });
}

void LaunchkeyController::sendBitmap(const juce::uint8* bitmapData, int size, bool stationary)
{
    if (!dawOutput || size != BITMAP_DATA_SIZE)
        return;

    std::vector<juce::uint8> sysexPayload;
    sysexPayload.reserve(SYSEX_HEADER_SIZE + 2 + BITMAP_DATA_SIZE);

    for (int i = 0; i < SYSEX_HEADER_SIZE; ++i)
        sysexPayload.push_back(SYSEX_HEADER[i]);

    sysexPayload.push_back(0x09);
    sysexPayload.push_back(stationary ? TARGET_STATIONARY : TARGET_TEMPORARY);

    for (int i = 0; i < size; ++i)
        sysexPayload.push_back(bitmapData[i]);

    auto msg = juce::MidiMessage::createSysExMessage(sysexPayload.data(),
                                                      (int) sysexPayload.size());
    juce::ScopedLock sl(midiLock);
    dawOutput->sendMessageNow(msg);
}

void LaunchkeyController::configureDisplay(int target, int config)
{
    sendSysEx({ 0x04, (juce::uint8) target, (juce::uint8) config });
}

void LaunchkeyController::setDisplayText(int target, int field, const juce::String& text)
{
    std::vector<juce::uint8> data;
    data.push_back(0x06);
    data.push_back((juce::uint8) target);
    data.push_back((juce::uint8) field);

    for (int i = 0; i < text.length(); ++i)
    {
        auto ch = (juce::uint8) text[i];
        if (ch >= 0x20 && ch <= 0x7E)
            data.push_back(ch);
    }

    sendSysEx(data);
}

void LaunchkeyController::setPadMode(int mode)
{
    if (!dawOutput)
        return;
    auto msg = juce::MidiMessage::controllerEvent(7, 0x1D, mode);
    dawOutput->sendMessageNow(msg);
}

void LaunchkeyController::setEncoderMode(int mode)
{
    if (!dawOutput)
        return;
    auto msg = juce::MidiMessage::controllerEvent(7, 0x1E, mode);
    dawOutput->sendMessageNow(msg);
}

void LaunchkeyController::addListener(Listener* listener)
{
    listeners.add(listener);
}

void LaunchkeyController::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

void LaunchkeyController::handleIncomingMidiMessage(juce::MidiInput* source,
                                                     const juce::MidiMessage& message)
{
    if (message.isSysEx())
    {
        auto* data = message.getSysExData();
        int size = message.getSysExDataSize();

        // The display's "ready for the next frame" reply is the header followed by 09 and nothing
        // else (measured on the device: F0 00 20 29 02 13 09 F7, about 75 ms after a frame). An
        // earlier version of this code waited for 09 7F, which never comes, so every frame sat out
        // the time-out.
        if (size == SYSEX_HEADER_SIZE + 1 && data[SYSEX_HEADER_SIZE] == 0x09
            && std::equal(data, data + SYSEX_HEADER_SIZE, SYSEX_HEADER))
        {
            listeners.call(&Listener::bitmapAckReceived);
            return;
        }

        if (size >= SYSEX_HEADER_SIZE + 2)
        {
            bool headerMatch = true;
            for (int i = 0; i < SYSEX_HEADER_SIZE; ++i)
            {
                if (data[i] != SYSEX_HEADER[i])
                {
                    headerMatch = false;
                    break;
                }
            }

            if (headerMatch && data[SYSEX_HEADER_SIZE] == 0x09
                && data[SYSEX_HEADER_SIZE + 1] == 0x7F)
            {
                listeners.call(&Listener::bitmapAckReceived);
                return;
            }
        }

        listeners.call(&Listener::sysExReceived, data, size);
        return;
    }

    // MIDI clock, start / stop / continue, song position and the like from the controller's own ports
    // are never a clock or transport source for the app (Play arrives as CC 115).
    if (isClockOrSystemMessage(message))
        return;

    int channel = message.getChannel();

    // Keys arrive on the MIDI port, pads on the DAW port. With the octave buttons the keys can reach the
    // pad note numbers (96-103, 112-119), so notes from the MIDI port are tagged with a channel number no
    // MIDI message can carry; listeners treat that as "keyboard, never a pad".
    if (source != nullptr && source == midiInput.get()
        && (message.isNoteOn() || message.isNoteOff() || message.isAftertouch()))
        channel = KEYS_PORT_CHANNEL;

    if (message.isNoteOn())
    {
        listeners.call(&Listener::noteOnReceived, channel,
                       message.getNoteNumber(), (int) message.getVelocity());
    }
    else if (message.isNoteOff())
    {
        listeners.call(&Listener::noteOffReceived, channel,
                       message.getNoteNumber());
    }
    else if (message.isController())
    {
        int cc = message.getControllerNumber();
        int val = message.getControllerValue();

        if (channel == 7 && cc == 0x1D)
            listeners.call(&Listener::padModeChanged, val);
        else if (channel == 7 && cc == 0x1E)
            listeners.call(&Listener::encoderModeChanged, val);
        else if (channel == 16 && cc >= ENCODER_REL_CC_FIRST && cc < ENCODER_REL_CC_FIRST + 8)
        {
            // Relative encoder output (feature 0x45 on): CC 0x55-0x5C, value 64 + steps (63 = one step
            // down, 65 = one step up). Listeners expect CC 0x15-0x1C with a two's-complement step count.
            int steps = val - 64;
            if (steps != 0)
                listeners.call(&Listener::controlChangeReceived, 16, cc - 0x40,
                               steps > 0 ? steps : 128 + steps);
        }
        else if (relativeEncoders && channel == 16 && cc >= 0x15 && cc <= 0x1C)
        {
            // Absolute position reports from the firmware; ignored once relative output is on.
        }
        else
            listeners.call(&Listener::controlChangeReceived, channel, cc, val);
    }
    else if (message.isAftertouch())
    {
        listeners.call(&Listener::aftertouchReceived, channel,
                       message.getNoteNumber(), message.getAfterTouchValue());
    }
    else if (message.isPitchWheel())
    {
        listeners.call(&Listener::pitchBendReceived, message.getPitchWheelValue());
    }
}

void LaunchkeyController::timerCallback()
{
    if (!isConnected())
        return;

    auto inputs = juce::MidiInput::getAvailableDevices();
    bool found = false;
    for (auto& d : inputs)
    {
        if (isLaunchkeyDevice(d.name))
        {
            found = true;
            break;
        }
    }

    if (!found)
        disconnect();
}

void LaunchkeyController::sendSysEx(const std::vector<juce::uint8>& data)
{
    if (!dawOutput)
        return;

    std::vector<juce::uint8> fullMsg;
    fullMsg.reserve(SYSEX_HEADER_SIZE + data.size());

    for (int i = 0; i < SYSEX_HEADER_SIZE; ++i)
        fullMsg.push_back(SYSEX_HEADER[i]);

    fullMsg.insert(fullMsg.end(), data.begin(), data.end());

    auto msg = juce::MidiMessage::createSysExMessage(fullMsg.data(), (int) fullMsg.size());

    juce::ScopedLock sl(midiLock);
    dawOutput->sendMessageNow(msg);
}

void LaunchkeyController::sendRawMidi(const juce::MidiMessage& msg)
{
    if (!dawOutput)
        return;

    juce::ScopedLock sl(midiLock);
    dawOutput->sendMessageNow(msg);
}

bool LaunchkeyController::isLaunchkeyDevice(const juce::String& name) const
{
    return name.containsIgnoreCase("Launchkey") && name.containsIgnoreCase("Mini");
}

bool LaunchkeyController::findAndOpenPorts()
{
    auto inputs = juce::MidiInput::getAvailableDevices();
    auto outputs = juce::MidiOutput::getAvailableDevices();

    juce::MidiDeviceInfo midiInInfo, dawInInfo;
    juce::MidiDeviceInfo midiOutInfo, dawOutInfo;
    bool foundMidiIn = false, foundDawIn = false;
    bool foundMidiOut = false, foundDawOut = false;

    for (auto& d : inputs)
    {
        if (!isLaunchkeyDevice(d.name))
            continue;

        if (d.name.containsIgnoreCase("DAW") || d.name.containsIgnoreCase("MIDIIN2"))
        {
            dawInInfo = d;
            foundDawIn = true;
        }
        else if (!foundMidiIn)
        {
            midiInInfo = d;
            foundMidiIn = true;
        }
    }

    for (auto& d : outputs)
    {
        if (!isLaunchkeyDevice(d.name))
            continue;

        if (d.name.containsIgnoreCase("DAW") || d.name.containsIgnoreCase("MIDIOUT2"))
        {
            dawOutInfo = d;
            foundDawOut = true;
        }
        else if (!foundMidiOut)
        {
            midiOutInfo = d;
            foundMidiOut = true;
        }
    }

    if (!foundDawIn || !foundDawOut)
    {
        juce::Logger::writeToLog("Launchkey MK4 DAW ports not found.");

        if (foundMidiIn && foundMidiOut)
        {
            dawInInfo = midiInInfo;
            dawOutInfo = midiOutInfo;
            foundDawIn = foundDawOut = true;
            foundMidiIn = foundMidiOut = false;
        }
        else
        {
            return false;
        }
    }

    dawInput = juce::MidiInput::openDevice(dawInInfo.identifier, this);
    if (!dawInput)
        return false;
    dawInput->start();

    dawOutput = juce::MidiOutput::openDevice(dawOutInfo.identifier);
    if (!dawOutput)
    {
        dawInput.reset();
        return false;
    }

    if (foundMidiIn)
    {
        midiInput = juce::MidiInput::openDevice(midiInInfo.identifier, this);
        if (midiInput)
            midiInput->start();
    }

    if (foundMidiOut)
        midiOutput = juce::MidiOutput::openDevice(midiOutInfo.identifier);

    juce::Logger::writeToLog("Launchkey MK4 connected.");
    if (dawInput)
        juce::Logger::writeToLog("  DAW In:  " + dawInInfo.name);
    if (dawOutput)
        juce::Logger::writeToLog("  DAW Out: " + dawOutInfo.name);
    if (midiInput)
        juce::Logger::writeToLog("  MIDI In:  " + midiInInfo.name);
    if (midiOutput)
        juce::Logger::writeToLog("  MIDI Out: " + midiOutInfo.name);

    return true;
}
