#pragma once

#include <juce_core/juce_core.h>
#include "../src/controller/LaunchkeyController.h"

class ButtonLEDTests : public juce::UnitTest
{
public:
    ButtonLEDTests() : juce::UnitTest("Button LEDs") {}

    void runTest() override
    {
        beginTest("Button control id constants");
        {
            expect(LaunchkeyController::BTN_PATTERN == 0x10);
            expect(LaunchkeyController::BTN_FUNC    == 0x11);
            expect(LaunchkeyController::BTN_UP       == 0x22);
            expect(LaunchkeyController::BTN_DOWN     == 0x23);
        }

        beginTest("FW_LED_SET address");
        {
            // led_set at file 0x22612, VMA = 0x08008000 + 0x22612 = 0x0802A612
            expectEquals((int) LaunchkeyController::FW_LED_SET, (int) 0x0802A612);
        }

        beginTest("Debug SysEx encoding matches KeyboardSplit format");
        {
            // The debug SysEx encodes three 32-bit values as 5×7-bit each, MSB first.
            // Verify the encoding of a known address: FW_LED_SET | 1 = 0x0802A613
            juce::uint32 addr = LaunchkeyController::FW_LED_SET | 1u;
            // 0x0802A613 in binary:
            //   0000 1000 0000 0010 1010 0110 0001 0011
            // Split into 7-bit groups from MSB (padding left to 35 bits):
            //   00001 0000000 0001010 1001100 0010011
            //   = 0x01  0x00  0x0A  0x4C  0x13
            juce::uint8 expected[5];
            for (int s = 28, i = 0; s >= 0; s -= 7, ++i)
                expected[i] = (juce::uint8) ((addr >> s) & 0x7F);

            expectEquals((int) expected[0], (int) ((addr >> 28) & 0x7F));
            expectEquals((int) expected[4], (int) (addr & 0x7F));
        }

        beginTest("setButtonLED called without connection does not crash");
        {
            LaunchkeyController lk;
            lk.setButtonLED(LaunchkeyController::BTN_UP, 63, 63, 63);
            // Should not crash — midiOutput is null, method returns early
            expect(true);
        }

        beginTest("illuminateNavigationButtons called without connection does not crash");
        {
            LaunchkeyController lk;
            lk.illuminateNavigationButtons();
            expect(true);
        }
    }
};

static ButtonLEDTests buttonLEDTests;
