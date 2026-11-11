#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../src/ui/OLEDMirrorComponent.h"
#include "../src/controller/DisplayRenderer.h"

class OLEDMirrorTests : public juce::UnitTest
{
public:
    OLEDMirrorTests() : UnitTest("OLED Mirror") {}

    void runTest() override
    {
        DisplayRenderer display;

        beginTest("Component creates with correct dimensions");
        {
            OLEDMirrorComponent mirror(display);
            expectEquals(mirror.getWidth(), OLEDMirrorComponent::TOTAL_W);
            expectEquals(mirror.getHeight(), OLEDMirrorComponent::TOTAL_H);
            expectEquals(OLEDMirrorComponent::TOTAL_W,
                         DisplayRenderer::WIDTH * OLEDMirrorComponent::PIXEL_SCALE
                         + OLEDMirrorComponent::BEZEL_PAD * 2);
            expectEquals(OLEDMirrorComponent::TOTAL_H,
                         DisplayRenderer::HEIGHT * OLEDMirrorComponent::PIXEL_SCALE
                         + OLEDMirrorComponent::BEZEL_PAD * 2);
        }

        beginTest("Scale factor is 4x");
        {
            expectEquals(OLEDMirrorComponent::PIXEL_SCALE, 4);
            expectEquals(OLEDMirrorComponent::DISPLAY_W, 512);
            expectEquals(OLEDMirrorComponent::DISPLAY_H, 256);
        }

        beginTest("References DisplayRenderer");
        {
            OLEDMirrorComponent mirror(display);
            expect(&mirror.getDisplayRenderer() == &display);
        }

        beginTest("Renders non-blank when display has content");
        {
            display.clear();
            display.drawString(10, 10, "NEPTUNE");
            display.drawRect(0, 0, 128, 64);

            bool anyPixelOn = false;
            const auto& pixels = display.getPixels();
            for (int y = 0; y < DisplayRenderer::HEIGHT && !anyPixelOn; ++y)
                for (int x = 0; x < DisplayRenderer::WIDTH && !anyPixelOn; ++x)
                    if (pixels[y][x])
                        anyPixelOn = true;

            expect(anyPixelOn);
        }

        beginTest("Clear display means no pixels lit");
        {
            display.clear();

            bool anyPixelOn = false;
            const auto& pixels = display.getPixels();
            for (int y = 0; y < DisplayRenderer::HEIGHT && !anyPixelOn; ++y)
                for (int x = 0; x < DisplayRenderer::WIDTH && !anyPixelOn; ++x)
                    if (pixels[y][x])
                        anyPixelOn = true;

            expect(!anyPixelOn);
        }

        beginTest("Refresh rate can be changed");
        {
            OLEDMirrorComponent mirror(display);
            mirror.setRefreshRate(60);
            mirror.setRefreshRate(15);
            mirror.setRefreshRate(0);
        }
    }
};

static OLEDMirrorTests oledMirrorTests;
