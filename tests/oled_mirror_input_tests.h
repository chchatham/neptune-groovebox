#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../src/ui/OLEDMirrorComponent.h"
#include "../src/controller/DisplayRenderer.h"

class OLEDMirrorInputTests : public juce::UnitTest
{
public:
    OLEDMirrorInputTests() : UnitTest("OLED Mirror Input") {}

    void runTest() override
    {
        DisplayRenderer display;

        // ----------------------------------------------------------------
        // Mouse -> pad index tests
        // ----------------------------------------------------------------

        beginTest("Mouse click in top-left pad area maps to pad 0");
        {
            // Pad area starts at logical Y=32 (lower half of 64px display).
            // In component coords: BEZEL_PAD + 32 * PIXEL_SCALE = 12 + 128 = 140
            // Pad 0 is col 0, row 0 of pad area.
            // X in component coords: BEZEL_PAD + 0 = 12 (first pixel of display)
            int mouseX = OLEDMirrorComponent::BEZEL_PAD + 0;
            int mouseY = OLEDMirrorComponent::BEZEL_PAD + OLEDMirrorComponent::PAD_AREA_TOP_Y * OLEDMirrorComponent::PIXEL_SCALE;
            int pad = OLEDMirrorComponent::mouseToPadIndex(mouseX, mouseY);
            expectEquals(pad, 0);
        }

        beginTest("Mouse click in top-right pad area maps to pad 7");
        {
            // Last pad in top row: col 7
            // X = BEZEL_PAD + 7 * 16 * PIXEL_SCALE = 12 + 448 = 460
            int mouseX = OLEDMirrorComponent::BEZEL_PAD + 7 * OLEDMirrorComponent::DISPLAY_PAD_W * OLEDMirrorComponent::PIXEL_SCALE;
            int mouseY = OLEDMirrorComponent::BEZEL_PAD + OLEDMirrorComponent::PAD_AREA_TOP_Y * OLEDMirrorComponent::PIXEL_SCALE;
            int pad = OLEDMirrorComponent::mouseToPadIndex(mouseX, mouseY);
            expectEquals(pad, 7);
        }

        beginTest("Mouse click in bottom-left pad area maps to pad 8");
        {
            // Bottom row, col 0: logical Y = 32 + 16 = 48
            int mouseX = OLEDMirrorComponent::BEZEL_PAD + 0;
            int mouseY = OLEDMirrorComponent::BEZEL_PAD + (OLEDMirrorComponent::PAD_AREA_TOP_Y + OLEDMirrorComponent::DISPLAY_PAD_H) * OLEDMirrorComponent::PIXEL_SCALE;
            int pad = OLEDMirrorComponent::mouseToPadIndex(mouseX, mouseY);
            expectEquals(pad, 8);
        }

        beginTest("Mouse click in bottom-right pad area maps to pad 15");
        {
            // Bottom row, col 7: logical Y = 48, logical X = 7*16 = 112
            int mouseX = OLEDMirrorComponent::BEZEL_PAD + 7 * OLEDMirrorComponent::DISPLAY_PAD_W * OLEDMirrorComponent::PIXEL_SCALE;
            int mouseY = OLEDMirrorComponent::BEZEL_PAD + (OLEDMirrorComponent::PAD_AREA_TOP_Y + OLEDMirrorComponent::DISPLAY_PAD_H) * OLEDMirrorComponent::PIXEL_SCALE;
            int pad = OLEDMirrorComponent::mouseToPadIndex(mouseX, mouseY);
            expectEquals(pad, 15);
        }

        beginTest("Mouse click in upper half of display returns -1 (no pad)");
        {
            // Upper half: logical Y < 32
            int mouseX = OLEDMirrorComponent::BEZEL_PAD + 64;
            int mouseY = OLEDMirrorComponent::BEZEL_PAD + 10; // well within upper half
            int pad = OLEDMirrorComponent::mouseToPadIndex(mouseX, mouseY);
            expectEquals(pad, -1);
        }

        beginTest("Mouse click outside bezel returns -1");
        {
            // Click in the bezel area (before display starts)
            int pad1 = OLEDMirrorComponent::mouseToPadIndex(0, 0);
            expectEquals(pad1, -1);

            // Click past the right edge
            int pad2 = OLEDMirrorComponent::mouseToPadIndex(OLEDMirrorComponent::TOTAL_W, OLEDMirrorComponent::TOTAL_H / 2);
            expectEquals(pad2, -1);

            // Negative coordinates
            int pad3 = OLEDMirrorComponent::mouseToPadIndex(-5, -5);
            expectEquals(pad3, -1);
        }

        beginTest("Mouse click in middle of pad 4 maps correctly");
        {
            // Pad 4 = top row, col 4. Center of pad in logical coords: X = 4*16 + 8 = 72, Y = 32 + 8 = 40
            int mouseX = OLEDMirrorComponent::BEZEL_PAD + 72 * OLEDMirrorComponent::PIXEL_SCALE;
            int mouseY = OLEDMirrorComponent::BEZEL_PAD + 40 * OLEDMirrorComponent::PIXEL_SCALE;
            int pad = OLEDMirrorComponent::mouseToPadIndex(mouseX, mouseY);
            expectEquals(pad, 4);
        }

        // ----------------------------------------------------------------
        // Keyboard -> action tests
        // ----------------------------------------------------------------

        beginTest("Space key maps to Play action");
        {
            MirrorKeyAction action;
            bool found = OLEDMirrorComponent::keyToAction(juce::KeyPress(juce::KeyPress::spaceKey), action);
            expect(found);
            expect(action == MirrorKeyAction::Play);
        }

        beginTest("R key maps to Record action");
        {
            MirrorKeyAction action;
            bool found = OLEDMirrorComponent::keyToAction(juce::KeyPress('R'), action);
            expect(found);
            expect(action == MirrorKeyAction::Record);
        }

        beginTest("Arrow keys map to ArrowLeft/ArrowRight");
        {
            MirrorKeyAction action;
            bool found = OLEDMirrorComponent::keyToAction(juce::KeyPress(juce::KeyPress::leftKey), action);
            expect(found);
            expect(action == MirrorKeyAction::ArrowLeft);

            found = OLEDMirrorComponent::keyToAction(juce::KeyPress(juce::KeyPress::rightKey), action);
            expect(found);
            expect(action == MirrorKeyAction::ArrowRight);
        }

        beginTest("Tab maps to Scale, A maps to Arp");
        {
            MirrorKeyAction action;
            bool found = OLEDMirrorComponent::keyToAction(juce::KeyPress(juce::KeyPress::tabKey), action);
            expect(found);
            expect(action == MirrorKeyAction::Scale);

            found = OLEDMirrorComponent::keyToAction(juce::KeyPress('A'), action);
            expect(found);
            expect(action == MirrorKeyAction::Arp);
        }

        beginTest("Up/Down and brackets map to encoder bank");
        {
            MirrorKeyAction action;
            bool found = OLEDMirrorComponent::keyToAction(juce::KeyPress(juce::KeyPress::upKey), action);
            expect(found);
            expect(action == MirrorKeyAction::EncoderBankUp);

            found = OLEDMirrorComponent::keyToAction(juce::KeyPress(juce::KeyPress::downKey), action);
            expect(found);
            expect(action == MirrorKeyAction::EncoderBankDown);

            found = OLEDMirrorComponent::keyToAction(juce::KeyPress('['), action);
            expect(found);
            expect(action == MirrorKeyAction::EncoderBankUp);

            found = OLEDMirrorComponent::keyToAction(juce::KeyPress(']'), action);
            expect(found);
            expect(action == MirrorKeyAction::EncoderBankDown);
        }

        beginTest("Unmapped key returns false");
        {
            MirrorKeyAction action;
            bool found = OLEDMirrorComponent::keyToAction(juce::KeyPress('Z'), action);
            expect(!found);

            found = OLEDMirrorComponent::keyToAction(juce::KeyPress('0'), action);
            expect(!found);
        }

        // ----------------------------------------------------------------
        // Keyboard -> pad index tests
        // ----------------------------------------------------------------

        beginTest("Number keys 1-8 map to pads 0-7");
        {
            for (int i = 0; i < 8; ++i)
            {
                int pad = OLEDMirrorComponent::keyToPadIndex(juce::KeyPress('1' + i));
                expectEquals(pad, i);
            }
        }

        beginTest("QWERTYUI keys map to pads 8-14");
        {
            // Note: R is handled as Record action first, but keyToPadIndex
            // doesn't know about that priority -- it just maps keys to pads.
            // The component handles priority in keyPressed().
            const char keys[] = { 'Q', 'W', 'E' };
            const int expectedPads[] = { 8, 9, 10 };
            for (int i = 0; i < 3; ++i)
            {
                int pad = OLEDMirrorComponent::keyToPadIndex(juce::KeyPress(keys[i]));
                expectEquals(pad, expectedPads[i]);
            }

            expectEquals(OLEDMirrorComponent::keyToPadIndex(juce::KeyPress('T')), 11);
            expectEquals(OLEDMirrorComponent::keyToPadIndex(juce::KeyPress('Y')), 12);
            expectEquals(OLEDMirrorComponent::keyToPadIndex(juce::KeyPress('U')), 13);
            expectEquals(OLEDMirrorComponent::keyToPadIndex(juce::KeyPress('I')), 14);
        }

        beginTest("Unmapped key returns -1 for pad index");
        {
            int pad = OLEDMirrorComponent::keyToPadIndex(juce::KeyPress('Z'));
            expectEquals(pad, -1);

            pad = OLEDMirrorComponent::keyToPadIndex(juce::KeyPress('9'));
            expectEquals(pad, -1);

            pad = OLEDMirrorComponent::keyToPadIndex(juce::KeyPress('0'));
            expectEquals(pad, -1);
        }

        // ----------------------------------------------------------------
        // Callback wiring tests
        // ----------------------------------------------------------------

        beginTest("Callbacks are null by default and settable");
        {
            OLEDMirrorComponent mirror(display);
            expect(mirror.onPadPress == nullptr);
            expect(mirror.onPadRelease == nullptr);
            expect(mirror.onKeyAction == nullptr);

            int pressedPad = -1;
            float pressedVel = 0.0f;
            mirror.onPadPress = [&](int pad, float vel) {
                pressedPad = pad;
                pressedVel = vel;
            };

            int releasedPad = -1;
            mirror.onPadRelease = [&](int pad) {
                releasedPad = pad;
            };

            // Verify callbacks are now set
            expect(mirror.onPadPress != nullptr);
            expect(mirror.onPadRelease != nullptr);
        }

        beginTest("Component wants keyboard focus");
        {
            OLEDMirrorComponent mirror(display);
            expect(mirror.getWantsKeyboardFocus());
        }
    }
};

static OLEDMirrorInputTests oledMirrorInputTests;
