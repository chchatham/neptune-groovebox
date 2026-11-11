#pragma once

#include <juce_core/juce_core.h>

struct ControllerProfile
{
    juce::String name;

    // SysEx identity
    std::vector<juce::uint8> sysexHeader;

    // MIDI port naming patterns
    juce::String midiPortPattern;
    juce::String dawPortPattern;

    // DAW mode messages
    juce::uint8 dawEnableStatus = 0x9F;
    juce::uint8 dawEnableData1 = 0x0C;
    juce::uint8 dawEnableData2 = 0x7F;
    juce::uint8 dawDisableData2 = 0x00;

    // Pad layout
    int padRows = 2;
    int padCols = 8;
    int padTopRowStart = 0x60;
    int padBottomRowStart = 0x70;
    int padDAWChannel = 1;
    int padDrumChannel = 10;

    // Encoder layout
    int encoderCount = 8;
    int encoderCCStart = 0x15;
    int encoderChannel = 16;

    // Display
    int displayWidth = 128;
    int displayHeight = 64;
    int bitmapBytesPerRow = 19;
    int bitmapBitsPerByte = 7;

    // Keyboard
    int keyCount = 25;
    bool hasAftertouch = false; // keys — unknown for MK4, verify
    bool hasPadPolyAftertouch = true;

    // Strips
    bool hasPitchStrip = true;
    bool hasModStrip = true;

    // Faders (49/61 only)
    bool hasFaders = false;
    int faderCount = 0;

    static ControllerProfile launchkeyMini25MK4()
    {
        ControllerProfile p;
        p.name = "Launchkey Mini 25 MK4";
        p.sysexHeader = { 0x00, 0x20, 0x29, 0x02, 0x13 };
        p.midiPortPattern = "Launchkey Mini";
        p.dawPortPattern = "DAW";
        p.keyCount = 25;
        return p;
    }
};
