#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>
#include <functional>
#include <vector>

class LaunchkeyController : public juce::MidiInputCallback,
                            private juce::Timer
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void launchkeyConnected() {}
        virtual void launchkeyDisconnected() {}
        virtual void dawModeEntered() {}
        virtual void dawModeExited() {}
        virtual void noteOnReceived(int /*channel*/, int /*note*/, int /*velocity*/) {}
        virtual void noteOffReceived(int /*channel*/, int /*note*/) {}
        virtual void controlChangeReceived(int /*channel*/, int /*cc*/, int /*value*/) {}
        virtual void aftertouchReceived(int /*channel*/, int /*note*/, int /*pressure*/) {}
        // Pitch strip: 14-bit value, 8192 = centre.
        virtual void pitchBendReceived(int /*value14*/) {}
        virtual void sysExReceived(const juce::uint8* /*data*/, int /*size*/) {}
        virtual void bitmapAckReceived() {}
        virtual void padModeChanged(int /*mode*/) {}
        virtual void encoderModeChanged(int /*mode*/) {}
    };

    LaunchkeyController();
    ~LaunchkeyController() override;

    bool isConnected() const { return midiInput != nullptr && midiOutput != nullptr; }

    // One SysEx (body without F0 / F7) to the controller's MIDI port — the Neptune firmware's debug
    // command lives there (see KeyboardSplit.h).
    void sendMidiPortSysEx(const std::vector<juce::uint8>& body)
    {
        if (midiOutput == nullptr || body.empty()) return;
        const juce::ScopedLock sl(midiLock);
        midiOutput->sendMessageNow(juce::MidiMessage::createSysExMessage(body.data(), (int) body.size()));
    }
    bool isInDawMode() const { return dawModeActive; }

    bool scanAndConnect();
    void disconnect();

    bool enterDawMode();
    void exitDawMode();

    void enableFeatureControls();
    void disableFeatureControls();

    void setFeatureControl(int cc, int value);
    void disableFirmwareArp();
    void disableFirmwareScale();
    void configureNeptuneFirmware();

    // Button LEDs: calls the firmware's led_set(id, 0xRRGGBB) through the debug SysEx (MIDI port).
    // Control ids: 0x10 = >, 0x11 = Func, 0x22 = Up, 0x23 = Down.
    void setButtonLED(int controlId, int r, int g, int b);
    void illuminateNavigationButtons();
    // The feature-control messages configureNeptuneFirmware() sends, in order (DAW port, channel 7).
    static std::vector<juce::MidiMessage> neptuneConnectMessages();
    // System common / real-time messages (MIDI clock F8, start / continue / stop, song position, ...).
    // The controller is never a clock or transport source: such messages from its ports are dropped
    // before any listener sees them.
    static bool isClockOrSystemMessage(const juce::MidiMessage& m)
    {
        return ! m.isSysEx() && m.getRawDataSize() > 0 && m.getRawData()[0] >= 0xF1;
    }

    // Pad LED coloring
    void setPadColor(int padIndex, int paletteColor);
    void setPadFlashing(int padIndex, int paletteColor);
    void setPadPulsing(int padIndex, int paletteColor);
    void setPadRGB(int padIndex, int r, int g, int b);

    // Bitmap display
    void sendBitmap(const juce::uint8* bitmapData, int size, bool stationary = true);

    // Text display
    void configureDisplay(int target, int config);
    void setDisplayText(int target, int field, const juce::String& text);

    // Pad/encoder mode control
    void setPadMode(int mode);
    void setEncoderMode(int mode);

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    // MiniSKU SysEx header
    static constexpr juce::uint8 SYSEX_HEADER[] = { 0x00, 0x20, 0x29, 0x02, 0x13 };
    static constexpr int SYSEX_HEADER_SIZE = 5;

    // DAW pad note indices (channel 1)
    static constexpr int PAD_TOP_ROW_START = 0x60;    // 96
    static constexpr int PAD_BOTTOM_ROW_START = 0x70;  // 112
    static constexpr int PADS_PER_ROW = 8;

    // Bitmap constants
    static constexpr int BITMAP_WIDTH = 128;
    static constexpr int BITMAP_HEIGHT = 64;
    static constexpr int BITMAP_BYTES_PER_ROW = 19;
    static constexpr int BITMAP_DATA_SIZE = BITMAP_BYTES_PER_ROW * BITMAP_HEIGHT; // 1216

    // Display targets
    static constexpr int TARGET_STATIONARY = 0x20;
    static constexpr int TARGET_TEMPORARY = 0x21;

    // Pad modes
    static constexpr int PAD_MODE_DRUM = 0x01;
    static constexpr int PAD_MODE_DAW = 0x02;
    static constexpr int PAD_MODE_USER_CHORDS = 0x04;
    static constexpr int PAD_MODE_CUSTOM1 = 0x05;
    static constexpr int PAD_MODE_CUSTOM2 = 0x06;
    static constexpr int PAD_MODE_CUSTOM3 = 0x07;
    static constexpr int PAD_MODE_CUSTOM4 = 0x08;
    static constexpr int PAD_MODE_ARP_PATTERN = 0x0D;
    static constexpr int PAD_MODE_CHORD_MAP = 0x0E;

    // Feature control CCs (sent on channel 7 to DAW port)
    static constexpr int FC_ARP_ONOFF = 0x49;       // 73
    static constexpr int FC_SCALE_MODE = 0x4A;      // 74
    static constexpr int FC_ARP_TYPE = 0x55;         // 85
    static constexpr int FC_ARP_RATE = 0x56;         // 86
    static constexpr int FC_ARP_OCTAVE = 0x57;       // 87
    static constexpr int FC_ARP_LATCH = 0x58;        // 88
    // Device settings reachable as feature controls (verified on hardware, docs/launchkey-mk4-firmware-map.md 0.9)
    static constexpr int FC_KEYS_CHANNEL = 0x64;     // 0-based MIDI channel of the keybed
    static constexpr int FC_PAD_AFTERTOUCH = 0x6C;   // 0 off, 1 channel, 2 polyphonic
    static constexpr int FC_MIDI_CLOCK_OUT = 0x6E;   // "MIDI Clock Out": 0 off, non-zero on (stock default on: F8 on the MIDI port all the time)
    static constexpr int FC_ENCODER_RELATIVE = 0x45; // 127 = encoders send relative steps on CC 0x55-0x5C, ch16
    static constexpr int ENCODER_REL_CC_FIRST = 0x55;
    static constexpr int KEYS_PORT_CHANNEL = 17;     // tag for notes that came from the MIDI (keybed) port

    // Firmware LED function address (VMA = 0x08008000 + file 0x22612)
    static constexpr juce::uint32 FW_LED_SET = 0x0802A612;
    // Button control ids
    static constexpr int BTN_PATTERN = 0x10;   // > (right of top pad row)
    static constexpr int BTN_FUNC    = 0x11;   // Func (right of bottom pad row)
    static constexpr int BTN_UP      = 0x22;   // Up arrow
    static constexpr int BTN_DOWN    = 0x23;   // Down arrow

    // Encoder modes
    static constexpr int ENCODER_MODE_MIXER = 0x01;
    static constexpr int ENCODER_MODE_PLUGIN = 0x02;
    static constexpr int ENCODER_MODE_SENDS = 0x04;
    static constexpr int ENCODER_MODE_TRANSPORT = 0x05;

private:
    bool relativeEncoders = false;
    void handleIncomingMidiMessage(juce::MidiInput* source,
                                  const juce::MidiMessage& message) override;
    void timerCallback() override;

    void sendSysEx(const std::vector<juce::uint8>& data);
    void sendRawMidi(const juce::MidiMessage& msg);

    bool isLaunchkeyDevice(const juce::String& name) const;
    bool findAndOpenPorts();

    std::unique_ptr<juce::MidiInput> midiInput;
    std::unique_ptr<juce::MidiInput> dawInput;
    std::unique_ptr<juce::MidiOutput> midiOutput;
    std::unique_ptr<juce::MidiOutput> dawOutput;

    bool dawModeActive = false;
    bool connected = false;

    juce::ListenerList<Listener> listeners;
    juce::CriticalSection midiLock;
};
