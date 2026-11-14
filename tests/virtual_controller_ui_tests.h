#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../src/ui/VirtualControllerComponent.h"
#include "../src/ui/PanelDimensions.h"

class VirtualControllerUITests : public juce::UnitTest
{
public:
    VirtualControllerUITests() : UnitTest("Virtual Controller UI") {}

    void runTest() override
    {
        const juce::ScopedJuceInitialiser_GUI gui;
        VirtualControllerComponent vc;

        beginTest("Button labels render without crashing and the snapshot differs from a blank panel");
        {
            vc.setPadColor(0, 255, 0, 0);
            const auto shot = vc.createComponentSnapshot(vc.getLocalBounds(), true, 1.0f);
            expectEquals(shot.getWidth(), VirtualControllerComponent::CTRL_W);
            expectEquals(shot.getHeight(), VirtualControllerComponent::CTRL_H);

            // The Shift button area should have label pixels (brighter than pure dark grey cap)
            const auto shiftBounds = vc.elementBounds(vc.elementNamed("Shift"));
            const int sx = (int) shiftBounds.getCentreX(), sy = (int) shiftBounds.getCentreY();
            const auto px = shot.getPixelAt(sx, sy);
            expect(px.getRed() > 20 || px.getGreen() > 20 || px.getBlue() > 20,
                   "Shift button area should have some content: " + px.toString());
        }

        beginTest("Pressed pad is visually different from unpressed pad");
        {
            vc.setPadColor(2, 0, 200, 0);
            vc.setPadColor(3, 0, 200, 0);

            // Snapshot with pad 2 unpressed
            const auto before = vc.createComponentSnapshot(vc.getLocalBounds(), true, 1.0f);
            const auto padBounds = vc.elementBounds(vc.elementNamed("Pad 3"));
            const int px = (int) padBounds.getCentreX(), py = (int) padBounds.getCentreY();
            const auto beforePixel = before.getPixelAt(px, py);

            // Press pad 2 and snapshot
            vc.press(vc.elementNamed("Pad 3"));
            const auto after = vc.createComponentSnapshot(vc.getLocalBounds(), true, 1.0f);
            const auto afterPixel = after.getPixelAt(px, py);
            vc.release(vc.elementNamed("Pad 3"));

            // The pressed pad should look different (sunk, with highlight border)
            const int diffR = std::abs(beforePixel.getRed() - afterPixel.getRed());
            const int diffG = std::abs(beforePixel.getGreen() - afterPixel.getGreen());
            const int diffB = std::abs(beforePixel.getBlue() - afterPixel.getBlue());
            expect(diffR + diffG + diffB > 5, "pressed pad should differ visually");
        }

        beginTest("Unlit pressed pad gets a white flash");
        {
            vc.setPadColor(5, 0, 0, 0);
            const auto padBounds = vc.elementBounds(vc.elementNamed("Pad 6"));
            const int px = (int) padBounds.getCentreX(), py = (int) padBounds.getCentreY();

            const auto before = vc.createComponentSnapshot(vc.getLocalBounds(), true, 1.0f);
            const auto beforePixel = before.getPixelAt(px, py);

            vc.press(vc.elementNamed("Pad 6"));
            const auto after = vc.createComponentSnapshot(vc.getLocalBounds(), true, 1.0f);
            const auto afterPixel = after.getPixelAt(px, py);
            vc.release(vc.elementNamed("Pad 6"));

            // The unlit pressed pad should be brighter (white flash)
            const int brightnessChange = (afterPixel.getRed() + afterPixel.getGreen() + afterPixel.getBlue())
                                       - (beforePixel.getRed() + beforePixel.getGreen() + beforePixel.getBlue());
            expect(brightnessChange > 10, "unlit pressed pad should get brighter with white flash");
        }

        beginTest("Play button has a triangle (brighter centre), Rec has a red circle");
        {
            const auto shot = vc.createComponentSnapshot(vc.getLocalBounds(), true, 1.0f);
            const auto playBounds = vc.elementBounds(vc.elementNamed("Play"));
            const auto recBounds = vc.elementBounds(vc.elementNamed("Rec"));

            // Play centre should have some white/bright pixels (the triangle)
            const auto playPx = shot.getPixelAt((int) playBounds.getCentreX(), (int) playBounds.getCentreY());
            expect(playPx.getRed() > 40 || playPx.getGreen() > 40 || playPx.getBlue() > 40,
                   "Play should have icon pixels: " + playPx.toString());

            // Rec centre should have red pixels (the circle)
            const auto recPx = shot.getPixelAt((int) recBounds.getCentreX(), (int) recBounds.getCentreY());
            expect(recPx.getRed() > recPx.getBlue() + 20,
                   "Rec should have red icon: " + recPx.toString());
        }

        beginTest("Strip position indicator changes with position");
        {
            const auto stripBounds = vc.elementBounds(vc.modStripElement());
            // Capture at bottom position
            vc.moveStrip(vc.modStripElement(), 0.0f);
            const auto shotLow = vc.createComponentSnapshot(vc.getLocalBounds(), true, 1.0f);
            // Capture at top position (active drag needed for the fill region)
            vc.moveStrip(vc.modStripElement(), 1.0f);
            const auto shotHigh = vc.createComponentSnapshot(vc.getLocalBounds(), true, 1.0f);

            // The strip mark should have moved: scan a column of pixels for any difference
            const int sx = (int) stripBounds.getCentreX();
            int maxDiff = 0;
            for (int py = (int) stripBounds.getY(); py < (int) stripBounds.getBottom(); ++py)
            {
                const auto pxL = shotLow.getPixelAt(sx, py);
                const auto pxH = shotHigh.getPixelAt(sx, py);
                const int d = std::abs(pxL.getRed() - pxH.getRed())
                            + std::abs(pxL.getGreen() - pxH.getGreen())
                            + std::abs(pxL.getBlue() - pxH.getBlue());
                if (d > maxDiff) maxDiff = d;
            }
            expect(maxDiff > 3, "strip position should cause visible difference");
        }

        beginTest("Up and Down buttons render with chevrons");
        {
            const auto shot = vc.createComponentSnapshot(vc.getLocalBounds(), true, 1.0f);
            const auto upBounds = vc.elementBounds(vc.elementNamed("Up"));
            const auto downBounds = vc.elementBounds(vc.elementNamed("Down"));

            // Both should have visible content at their centres
            const auto upPx = shot.getPixelAt((int) upBounds.getCentreX(), (int) upBounds.getCentreY());
            const auto dnPx = shot.getPixelAt((int) downBounds.getCentreX(), (int) downBounds.getCentreY());
            expect(upPx.getRed() > 20 || upPx.getGreen() > 20 || upPx.getBlue() > 20,
                   "Up button should have chevron pixels");
            expect(dnPx.getRed() > 20 || dnPx.getGreen() > 20 || dnPx.getBlue() > 20,
                   "Down button should have chevron pixels");
        }

        beginTest("Pattern key renders with a right chevron");
        {
            const auto shot = vc.createComponentSnapshot(vc.getLocalBounds(), true, 1.0f);
            const auto patBounds = vc.elementBounds(vc.elementNamed("Pattern"));
            const auto px = shot.getPixelAt((int) patBounds.getCentreX(), (int) patBounds.getCentreY());
            expect(px.getRed() > 20 || px.getGreen() > 20 || px.getBlue() > 20,
                   "Pattern button should have chevron pixels");
        }
    }
};

static VirtualControllerUITests virtualControllerUITests;
