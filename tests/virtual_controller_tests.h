#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
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

        beginTest("Keys are tagged as keys, also where their number is a pad's");
        {
            expectEquals(VirtualControllerComponent::KEYS_CHANNEL, LaunchkeyController::KEYS_PORT_CHANNEL);
            const int key = vc.elementNamed("C2");   // the lowest key, note 48 (middle C is C3)
            expect(key >= 0);
            const int octUp = vc.elementNamed("Oct +");
            for (int i = 0; i < 3; ++i) vc.press(octUp);
            for (int i = 0; i < 3; ++i) vc.press(octUp);   // clamped at +3
            ons.clear(); offs.clear();
            vc.press(key);
            vc.release(key);
            expectEquals((int) ons.size(), 1);
            if (ons.size() == 1)
            {
                expectEquals(ons[0].number, 48 + 36);
                expectEquals(ons[0].channel, LaunchkeyController::KEYS_PORT_CHANNEL);
            }
            expectEquals((int) offs.size(), 1);
            if (offs.size() == 1)
                expectEquals(offs[0].channel, LaunchkeyController::KEYS_PORT_CHANNEL);
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
