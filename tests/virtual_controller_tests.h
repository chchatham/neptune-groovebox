#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <set>
#include <map>
#include "../src/ui/PanelDimensions.h"
#include "../src/ui/VirtualControllerComponent.h"
#include "../src/ui/ScreenManager.h"
#include "../src/controller/LaunchkeyController.h"

// The on-screen controller must send what the hardware sends: the numbers ScreenManager and
// LaunchkeyController listen for (Func and Settings once sent CCs nothing listened to).
class VirtualControllerTests : public juce::UnitTest
{
public:
    VirtualControllerTests() : UnitTest("Virtual Controller") {}

    struct Msg { int channel, number, value; };

    // A mouse event at a place in the component, as the window would deliver it.
    static juce::MouseEvent mouseAt(juce::Component& c, juce::Point<float> at, bool rightButton, juce::Point<float> downAt)
    {
        const auto mods = rightButton ? juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier)
                                      : juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, mods,
                                juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                juce::MouseInputSource::defaultTiltY, &c, &c, now, downAt, now, 1, at != downAt);
    }

    void runTest() override
    {
        const juce::ScopedJuceInitialiser_GUI gui;
        VirtualControllerComponent vc;
        std::vector<Msg> ccs, ons, offs;
        std::vector<int> bends, mods;
        vc.onCC = [&](int ch, int cc, int v) { ccs.push_back({ ch, cc, v }); };
        vc.onNoteOn = [&](int ch, int n, int v) { ons.push_back({ ch, n, v }); };
        vc.onNoteOff = [&](int ch, int n) { offs.push_back({ ch, n, 0 }); };
        vc.onPitchBend = [&](int v) { bends.push_back(v); };
        vc.onModStrip = [&](int v) { mods.push_back(v); };

        beginTest("Every button sends the hardware's CC, 127 on press and 0 on release");
        {
            struct Want { const char* label; int channel, cc; };
            const Want wants[] = {
                { "Func",     ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_FUNC },
                { "Settings", ScreenManager::BUTTON_CHANNEL, ScreenManager::NP_CC_SETTINGS },
                { "Arp",      ScreenManager::BUTTON_CHANNEL, ScreenManager::NP_CC_ARP },
                { "Scale",    ScreenManager::BUTTON_CHANNEL, ScreenManager::NP_CC_SCALE },
                { "Shift",    ScreenManager::SHIFT_CHANNEL,  ScreenManager::CC_SHIFT },
            };
            for (const auto& w : wants)
            {
                ccs.clear();
                const int idx = vc.elementNamed(w.label);
                expect(idx >= 0, w.label);
                vc.press(idx);
                vc.release(idx);
                expectEquals((int) ccs.size(), 2, w.label);
                if (ccs.size() == 2)
                {
                    expectEquals(ccs[0].channel, w.channel, w.label);
                    expectEquals(ccs[0].number, w.cc, w.label);
                    expectEquals(ccs[0].value, 127, w.label);
                    expectEquals(ccs[1].number, w.cc, w.label);
                    expectEquals(ccs[1].value, 0, w.label);
                }
            }
            expectEquals(VirtualControllerComponent::CC_PATTERN_KEY, ScreenManager::CC_PATTERN_KEY);
            expectEquals(VirtualControllerComponent::CC_UP, ScreenManager::CC_ARROW_UP);
            expectEquals(VirtualControllerComponent::CC_DOWN, ScreenManager::CC_ARROW_DOWN);
            expectEquals(VirtualControllerComponent::CC_PLAY, ScreenManager::CC_PLAY);
            expectEquals(VirtualControllerComponent::CC_RECORD, ScreenManager::CC_RECORD);
            expectEquals(VirtualControllerComponent::CC_BANK_UP, ScreenManager::CC_BANK_UP);
            expectEquals(VirtualControllerComponent::CC_BANK_DOWN, ScreenManager::CC_BANK_DOWN);
        }

        beginTest("Func held on the virtual controller is Func in ScreenManager");
        {
            ScreenManager sm;
            vc.onCC = [&](int ch, int cc, int v) { sm.controlChangeReceived(ch, cc, v); };
            vc.press(vc.elementNamed("Func"));
            expect(sm.isFuncHeld());
            vc.release(vc.elementNamed("Func"));
            expect(! sm.isFuncHeld());
            vc.onCC = [&](int ch, int cc, int v) { ccs.push_back({ ch, cc, v }); };
        }

        beginTest("The picture ends above the keys: no control sends a key's note, the octave buttons send nothing");
        {
            // Keys reach the app from the MIDI port, tagged LaunchkeyController::KEYS_PORT_CHANNEL; the panel
            // has none. Its only notes are the sixteen pads, on the pad channel, with the pad numbers.
            std::set<int> padNotes;
            for (int i = 0; i < vc.numElements(); ++i)
            {
                ons.clear(); offs.clear();
                vc.press(i);
                vc.release(i);
                for (const auto& m : ons)
                {
                    expectEquals(m.channel, 1, vc.elementName(i));
                    expect(m.channel != LaunchkeyController::KEYS_PORT_CHANNEL);
                    expect((m.number >= LaunchkeyController::PAD_TOP_ROW_START && m.number < LaunchkeyController::PAD_TOP_ROW_START + 8)
                           || (m.number >= LaunchkeyController::PAD_BOTTOM_ROW_START && m.number < LaunchkeyController::PAD_BOTTOM_ROW_START + 8),
                           vc.elementName(i));
                    expectEquals(m.value, 100);
                    padNotes.insert(m.number);
                }
                expectEquals((int) offs.size(), (int) ons.size(), vc.elementName(i));
            }
            expectEquals((int) padNotes.size(), 16);
            expect(vc.elementNamed("C2") < 0, "no keys");
            expectEquals(vc.getHeight(), (int) std::lround(PanelDim::keysY * VirtualControllerComponent::PX_PER_MM) + VirtualControllerComponent::MARGIN);

            ccs.clear(); ons.clear(); offs.clear();
            for (const char* name : { "Oct -", "Oct +" })
            {
                expect(vc.elementNamed(name) >= 0, name);
                vc.press(vc.elementNamed(name));
                vc.release(vc.elementNamed(name));
            }
            expect(ccs.empty() && ons.empty() && offs.empty(), "octave is the hardware's own business");
        }

        beginTest("Every control is where the dimension table puts it, and a click there sends its message");
        {
            namespace D = PanelDim;
            using VC = VirtualControllerComponent;
            auto centre = [](const D::Rect& r) { return VC::mmRect(r).getCentre(); };
            auto click = [&](juce::Point<float> at, bool right = false)
            {
                vc.mouseDown(mouseAt(vc, at, right, at));
                vc.mouseUp(mouseAt(vc, at, right, at));
            };

            struct Btn { D::Rect at; int channel, cc; };
            const Btn buttons[] = {
                { D::BTN_SHIFT, ScreenManager::SHIFT_CHANNEL, ScreenManager::CC_SHIFT },
                { D::BTN_SETTINGS, ScreenManager::BUTTON_CHANNEL, ScreenManager::NP_CC_SETTINGS },
                { D::BTN_PLAY, ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_PLAY },
                { D::BTN_REC, ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_RECORD },
                { D::BTN_ARP, ScreenManager::BUTTON_CHANNEL, ScreenManager::NP_CC_ARP },
                { D::BTN_SCALE, ScreenManager::BUTTON_CHANNEL, ScreenManager::NP_CC_SCALE },
                { D::BTN_UP, ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_ARROW_UP },
                { D::BTN_DOWN, ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_ARROW_DOWN },
                { D::BTN_BANK_UP, ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_BANK_UP },
                { D::BTN_BANK_DN, ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_BANK_DOWN },
                { D::BTN_PATTERN, ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_PATTERN_KEY },
                { D::BTN_FUNC, ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_FUNC },
            };
            for (const auto& b : buttons)
            {
                ccs.clear();
                click(centre(b.at));
                const juce::String what = "cc " + juce::String(b.cc);
                expectEquals((int) ccs.size(), 2, what);
                if (ccs.size() == 2)
                {
                    expectEquals(ccs[0].channel, b.channel, what);
                    expectEquals(ccs[0].number, b.cc, what);
                    expectEquals(ccs[0].value, 127, what);
                    expectEquals(ccs[1].channel, b.channel, what);
                    expectEquals(ccs[1].number, b.cc, what);
                    expectEquals(ccs[1].value, 0, what);
                }
            }

            // pads: note on with the velocity on mouse down, note off on mouse up
            for (int row = 0; row < 2; ++row)
                for (int col = 0; col < 8; ++col)
                {
                    ons.clear(); offs.clear();
                    const auto at = centre(D::pad(row, col));
                    const int note = (row == 0 ? LaunchkeyController::PAD_TOP_ROW_START : LaunchkeyController::PAD_BOTTOM_ROW_START) + col;
                    vc.mouseDown(mouseAt(vc, at, false, at));
                    expectEquals((int) ons.size(), 1);
                    expect(offs.empty());
                    vc.mouseUp(mouseAt(vc, at, false, at));
                    expectEquals((int) offs.size(), 1);
                    if (ons.size() == 1 && offs.size() == 1)
                    {
                        expectEquals(ons[0].channel, 1);
                        expectEquals(ons[0].number, note);
                        expectEquals(ons[0].value, 100);
                        expectEquals(offs[0].channel, 1);
                        expectEquals(offs[0].number, note);
                    }
                }

            // knobs: the wheel and a drag send the app's internal step format (1 = up, 127 = down)
            for (int i = 0; i < 8; ++i)
            {
                const juce::Point<float> at(VC::mmX(D::encCx(i)), VC::mmY(D::encCy));
                ccs.clear();
                juce::MouseWheelDetails wheel;
                wheel.deltaX = 0.0f; wheel.deltaY = 0.2f; wheel.isReversed = false; wheel.isSmooth = false; wheel.isInertial = false;
                vc.mouseWheelMove(mouseAt(vc, at, false, at), wheel);
                expectEquals((int) ccs.size(), 1);
                wheel.deltaY = -0.4f;
                vc.mouseWheelMove(mouseAt(vc, at, false, at), wheel);
                expectEquals((int) ccs.size(), 3);
                vc.mouseDown(mouseAt(vc, at, false, at));
                vc.mouseDrag(mouseAt(vc, at.translated(0.0f, -6.0f), false, at));     // up = clockwise
                vc.mouseDrag(mouseAt(vc, at.translated(0.0f, -2.0f), false, at));     // back down
                vc.mouseUp(mouseAt(vc, at.translated(0.0f, -2.0f), false, at));
                expectEquals((int) ccs.size(), 8);
                if (ccs.size() == 8)
                {
                    const int want[8] = { 1, 127, 127, 1, 1, 1, 127, 127 };
                    for (int k = 0; k < 8; ++k)
                    {
                        expectEquals(ccs[(size_t) k].channel, 16);
                        expectEquals(ccs[(size_t) k].number, 0x15 + i);
                        expectEquals(ccs[(size_t) k].value, want[k]);
                    }
                }
            }

            // strips: a drag sends the position; letting go centres the pitch strip only
            bends.clear(); mods.clear(); ccs.clear();
            const auto pitch = VC::mmRect(D::STRIP_PITCH), mod = VC::mmRect(D::STRIP_MOD);
            vc.mouseDown(mouseAt(vc, { pitch.getCentreX(), pitch.getY() }, false, { pitch.getCentreX(), pitch.getY() }));
            vc.mouseDrag(mouseAt(vc, { pitch.getCentreX(), pitch.getBottom() }, false, { pitch.getCentreX(), pitch.getY() }));
            vc.mouseUp(mouseAt(vc, { pitch.getCentreX(), pitch.getBottom() }, false, { pitch.getCentreX(), pitch.getY() }));
            expectEquals((int) bends.size(), 3);
            if (bends.size() == 3)
            {
                expectEquals(bends[0], 16383);
                expectEquals(bends[1], 0);
                expectEquals(bends[2], 8192);
            }
            vc.mouseDown(mouseAt(vc, { mod.getCentreX(), mod.getBottom() - 0.5f }, false, { mod.getCentreX(), mod.getBottom() - 0.5f }));
            vc.mouseDrag(mouseAt(vc, { mod.getCentreX(), mod.getY() }, false, { mod.getCentreX(), mod.getBottom() }));
            vc.mouseUp(mouseAt(vc, { mod.getCentreX(), mod.getY() }, false, { mod.getCentreX(), mod.getBottom() }));
            expectEquals((int) mods.size(), 2);
            if (mods.size() == 2)
            {
                expectEquals(mods[0], 0);
                expectEquals(mods[1], 127);
            }
            expect(ccs.empty(), "strips do not go through the button path");
            bends.clear(); mods.clear();

            // a right click holds: Shift held, then Up, is the hardware's Shift + Up (CC 102)
            ccs.clear();
            click(centre(D::BTN_SHIFT), true);
            expect(vc.isHeld(vc.elementNamed("Shift")));
            click(centre(D::BTN_UP));
            click(centre(D::BTN_DOWN));
            click(centre(D::BTN_SHIFT), true);
            expect(! vc.isHeld(vc.elementNamed("Shift")));
            expectEquals((int) ccs.size(), 6);
            if (ccs.size() == 6)
            {
                expectEquals(ccs[0].number, ScreenManager::CC_SHIFT);  expectEquals(ccs[0].value, 127);
                expectEquals(ccs[1].number, 102);                      expectEquals(ccs[1].value, 127);
                expectEquals(ccs[2].number, 102);                      expectEquals(ccs[2].value, 0);
                expectEquals(ccs[3].number, 103);                      expectEquals(ccs[3].value, 127);
                expectEquals(ccs[4].number, 103);                      expectEquals(ccs[4].value, 0);
                expectEquals(ccs[5].number, ScreenManager::CC_SHIFT);  expectEquals(ccs[5].value, 0);
            }

            // nothing overlaps, everything is on the body and above the keys, and the body's gaps are dead
            for (int i = 0; i < vc.numElements(); ++i)
            {
                const auto b = vc.elementBounds(i);
                expect(b.getX() >= VC::mmX(0.0) && b.getRight() <= VC::mmX(D::bodyW) && b.getY() >= VC::mmY(0.0)
                       && b.getBottom() <= VC::mmY(D::keysY), vc.elementName(i));
                expectEquals(vc.elementAt(b.getCentreX(), b.getCentreY()), i, vc.elementName(i));
                for (int j = i + 1; j < vc.numElements(); ++j)
                    expect(! b.intersects(vc.elementBounds(j)), vc.elementName(i) + " / " + vc.elementName(j));
            }
            expectEquals(vc.elementAt(VC::mmX(D::padX0 + D::padW + (D::padPitch - D::padW) / 2), VC::mmY(D::padTopY0 + 5.0)), -1);
            expectEquals(vc.elementAt(VC::mmX(D::bodyW / 2), VC::mmY((D::fY0 + D::fY1) / 2)), -1);
        }

        beginTest("A pad colour is kept, and painting works with and without a live display");
        {
            vc.setPadColor(3, 255, 40, 0);
            vc.setPadColor(99, 1, 2, 3);       // ignored
            DisplayRenderer renderer;
            renderer.setPixel(5, 5, true);
            vc.setDisplay(&renderer);
            const auto shot = vc.createComponentSnapshot(vc.getLocalBounds(), true, 1.0f);
            expectEquals(shot.getWidth(), VirtualControllerComponent::CTRL_W);
            // the lit pad is orange-red in the middle; its unlit neighbour is grey rubber
            const auto lit = shot.getPixelAt((int) VirtualControllerComponent::mmX(PanelDim::padCx(3)), (int) VirtualControllerComponent::mmY(PanelDim::padTopY0 + PanelDim::padH / 2));
            const auto dark = shot.getPixelAt((int) VirtualControllerComponent::mmX(PanelDim::padCx(4)), (int) VirtualControllerComponent::mmY(PanelDim::padTopY0 + PanelDim::padH / 2));
            expect(lit.getRed() > 200 && lit.getBlue() < 140, lit.toString());
            expect(std::abs((int) dark.getRed() - (int) dark.getBlue()) < 20 && dark.getRed() < 130, dark.toString());
            vc.setDisplay(nullptr);
            vc.setPadColor(3, 0, 0, 0);
        }

        beginTest("The pitch strip returns to centre, the mod strip keeps its value");
        {
            vc.moveStrip(vc.pitchStripElement(), 1.0f);
            vc.moveStrip(vc.pitchStripElement(), 0.0f);
            vc.releaseStrip(vc.pitchStripElement());
            expectEquals((int) bends.size(), 3);
            if (bends.size() == 3)
            {
                expectEquals(bends[0], 16383);
                expectEquals(bends[1], 0);
                expectEquals(bends[2], 8192);
            }
            vc.moveStrip(vc.modStripElement(), 1.0f);
            vc.releaseStrip(vc.modStripElement());
            expectEquals((int) mods.size(), 1);
            if (mods.size() == 1)
                expectEquals(mods[0], 127);
            expect(ccs.empty() || ccs.back().number != 1, "strips do not go through the button path");
        }
    }
};

static VirtualControllerTests virtualControllerTests;

// The screen's panel and the printed overlay are drawn from the same numbers and carry the same words:
// src/ui/PanelDimensions.h against the DIM table of docs/overlay/overlay.html.
class PanelDimensionsTests : public juce::UnitTest
{
public:
    PanelDimensionsTests() : UnitTest("Panel Dimensions") {}

    static juce::String overlaySource()
    {
        return juce::File(__FILE__).getParentDirectory().getParentDirectory()
                   .getChildFile("docs/overlay/overlay.html").loadFileAsString();
    }

    // name -> value from the `const DIM = { ... };` block (comments dropped).
    static std::map<juce::String, double> overlayTable(const juce::String& html)
    {
        std::map<juce::String, double> table;
        const int start = html.indexOf("const DIM = {");
        const int end = start < 0 ? -1 : html.indexOf(start, "};");
        if (start < 0 || end < 0)
            return table;
        juce::StringArray lines;
        lines.addLines(html.substring(start + 13, end));
        for (auto line : lines)
        {
            line = line.upToFirstOccurrenceOf("//", false, false);
            juce::StringArray items;
            items.addTokens(line, ",", {});
            for (const auto& item : items)
            {
                const auto name = item.upToFirstOccurrenceOf(":", false, false).trim();
                const auto value = item.fromFirstOccurrenceOf(":", false, false).trim();
                if (name.isNotEmpty() && value.isNotEmpty() && value.containsOnly("0123456789.-"))
                    table[name] = value.getDoubleValue();
            }
        }
        return table;
    }

    void runTest() override
    {
        const auto html = overlaySource();

        beginTest("The C++ table and the overlay's DIM table hold the same numbers");
        {
            expect(html.isNotEmpty(), "docs/overlay/overlay.html not found");
            const auto table = overlayTable(html);
            expectEquals((int) table.size(), (int) PanelDim::TABLE_SIZE, "a name is in one table only");
            expect(table.size() > 50);
            for (const auto& e : PanelDim::TABLE)
            {
                const auto it = table.find(e.name);
                expect(it != table.end(), juce::String(e.name) + " is not in the overlay's table");
                if (it != table.end())
                    expectWithinAbsoluteError(it->second, e.mm, 1.0e-9, e.name);
            }
        }

        beginTest("The parser reads what is there (it would see a changed value)");
        {
            const auto table = overlayTable("const DIM = {\n  a: 1.5,  // D  x: 9\n  b: 2, c: 3.25,\n  d: 4\n};");
            expectEquals((int) table.size(), 4);
            expectWithinAbsoluteError(table.at("a"), 1.5, 1.0e-12);
            expectWithinAbsoluteError(table.at("c"), 3.25, 1.0e-12);
            expectWithinAbsoluteError(table.at("d"), 4.0, 1.0e-12);
            expect(overlayTable(html.replace("padPitch:    22.10", "padPitch:    22.20")).at("padPitch") > 22.15);
        }

        beginTest("The controls derived from the table are the overlay's");
        {
            namespace D = PanelDim;
            expectWithinAbsoluteError(D::padCx(0), 129.9 + 9.3, 1.0e-9);
            expectWithinAbsoluteError(D::pad(1, 7).x + D::pad(1, 7).w - D::padX0, 173.3, 1.0e-9);    // the overlay's MEASURE (6)
            expectWithinAbsoluteError(D::BTN_REC.x, 67.3 + 22.1, 1.0e-9);
            expectWithinAbsoluteError(D::BTN_OCT_UP.y, 35.3 + 2 * 14.75, 1.0e-9);
            expectWithinAbsoluteError(D::encoder(3).cx(), D::pad(0, 3).cx(), 1.0e-9);
            expect(D::BTN_OCT_DN.y + D::BTN_OCT_DN.h < D::fY0 && D::pad(1, 0).y + D::padH < D::fY0, "the foot strips clear the controls");
            expect(D::fY1 < D::keysY, "the overlay stays above the keys");
        }

        beginTest("Every word on the screen's overlay is a word of the printed one");
        {
            const juce::ScopedJuceInitialiser_GUI gui;
            const auto words = VirtualControllerComponent::overlayWords();
            // counted on the printed sheets by the overlay itself ("Words on the six pieces")
            expectEquals(words.size(), 42);
            for (const auto& w : words)
                expect(html.contains("'" + w + "'") || html.contains("'" + w + " ") || html.contains(" " + w + "'"), w);
            for (const char* w : { "PARAMS", "TRACK", "FX", "WAVEFORM", "FILTER", "NEPTUNE", "KNOB", "PAGE", "JUMP", "PATTERN", "PASTE",
                                   "COARSE", "FINE", "TAP", "TEMPO", "LATCH", "SCREEN", "REWIND", "SILENCE", "CLEAR", "FILL",
                                   "COPY", "KEY", "SPLIT", "PAD", "HOLD", "MUTE" })
                expect(words.contains(w), w);
        }
    }
};

static PanelDimensionsTests panelDimensionsTests;
