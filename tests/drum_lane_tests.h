#pragma once

#include "held_step_pages_tests.h"
#include "../src/engine/DrumLane.h"

// A drum track on STEP is entered one drum at a time (the pad selected on DRUMS), and any number
// of the kit's sixteen drums can hit on the same step (owner's requirement, 2026-11-11: before, a
// tap put one drum on a step and the next tap took the whole step away).
class DrumLaneTests : public juce::UnitTest
{
public:
    DrumLaneTests() : UnitTest("Drum Lanes") {}

    static juce::String padsOf(const Step& step)
    {
        juce::StringArray s;
        for (auto& n : step.notes) s.add(juce::String(DrumEngine::padForNote(n.pitch)));
        return s.joinIntoString(" ");
    }

    void runTest() override
    {
        using heldsteptest::Rig;
        auto tap = [](Rig& r, int pad) { r.padDown(pad); UiClock::advance(60); r.padUp(pad); UiClock::advance(60); };

        beginTest("STEP: each drum is tapped in on its own; they share steps; a tap takes only its own drum away");
        {
            Rig r(EngineType::Drum);
            auto* drums = dynamic_cast<DrumEngine*>(r.audio.getEngine(0));
            expect(drums != nullptr);
            if (drums == nullptr) return;

            drums->setSelectedVoice(8);                      // kick
            tap(r, 0); tap(r, 4);
            drums->setSelectedVoice(2);                      // another drum
            tap(r, 0); tap(r, 2);
            drums->setSelectedVoice(11);
            tap(r, 0);
            expectEquals(padsOf(r.step(0)), juce::String("8 2 11"));
            expectEquals(padsOf(r.step(4)), juce::String("8"));
            expectEquals(padsOf(r.step(2)), juce::String("2"));
            expect(r.step(0).trig && r.step(2).trig && r.step(4).trig);

            drums->setSelectedVoice(2);
            tap(r, 0);                                       // pad 2's hit leaves step 1
            expectEquals(padsOf(r.step(0)), juce::String("8 11"));
            expect(r.step(0).trig, "the other drums still play");
            tap(r, 4);                                       // the kick is on step 5, pad 2 is not: it joins
            expectEquals(padsOf(r.step(4)), juce::String("8 2"));

            drums->setSelectedVoice(8);
            tap(r, 0);
            drums->setSelectedVoice(11);
            tap(r, 0);
            expect(r.step(0).notes.empty() && !r.step(0).trig, "the step goes off with its last drum");
            tap(r, 0);
            expectEquals(padsOf(r.step(0)), juce::String("11"), "and comes back with only the drum tapped in");
        }

        beginTest("All sixteen drums on one step: the sequencer plays sixteen notes and the kit sounds sixteen voices");
        {
            Rig r(EngineType::Drum);
            auto* drums = dynamic_cast<DrumEngine*>(r.audio.getEngine(0));
            if (drums == nullptr) { expect(false); return; }
            for (int pad = 0; pad < DrumEngine::NUM_VOICES; ++pad)
            {
                drums->setSelectedVoice(pad);
                tap(r, 0);
            }
            expectEquals((int) r.step(0).notes.size(), 16);

            int played = 0;
            r.seq.setTrigCallback([&](int track, const Step& step)
            {
                if (track != 0) return;
                for (auto& n : step.notes) { drums->noteOn(n.pitch, n.velocity); ++played; }
            });
            r.seq.start();
            r.seq.processBlock(64);
            r.seq.stop();
            expectEquals(played, 16);
            int sounding = 0;
            for (int pad = 0; pad < DrumEngine::NUM_VOICES; ++pad)
                sounding += drums->getVoice(pad).isActive() ? 1 : 0;
            expectEquals(sounding, 16, "one voice per pad: no drum is cut by another on the same step");
        }

        beginTest("Pad lights: lit where the selected drum hits, dim yellow where only others do");
        {
            Rig r(EngineType::Drum);
            auto* drums = dynamic_cast<DrumEngine*>(r.audio.getEngine(0));
            if (drums == nullptr) { expect(false); return; }
            r.leds.setSequencer(&r.seq);
            r.leds.setDrumEngine(drums);
            drums->setSelectedVoice(8);
            tap(r, 0); tap(r, 1);
            drums->setSelectedVoice(3);
            tap(r, 1); tap(r, 2);
            expectEquals(r.leds.getColor(0, PadManager::Mode::Step), (int) PadColors::DIM_YELLOW);
            expectEquals(r.leds.getColor(1, PadManager::Mode::Step), (int) PadColors::STEP_ACTIVE);
            expectEquals(r.leds.getColor(2, PadManager::Mode::Step), (int) PadColors::STEP_ACTIVE);
            expectEquals(r.leds.getColor(3, PadManager::Mode::Step), (int) PadColors::STEP_EMPTY);
            drums->setSelectedVoice(8);
            expectEquals(r.leds.getColor(0, PadManager::Mode::Step), (int) PadColors::STEP_ACTIVE);
            expectEquals(r.leds.getColor(2, PadManager::Mode::Step), (int) PadColors::DIM_YELLOW);
        }

        beginTest("A held step: a key adds its drum; VEL is the selected drum's own; a hold never removes");
        {
            Rig r(EngineType::Drum);
            auto* drums = dynamic_cast<DrumEngine*>(r.audio.getEngine(0));
            if (drums == nullptr) { expect(false); return; }
            drums->setSelectedVoice(8);
            r.padDown(3);
            r.wait();
            r.mgr.noteOnReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 48, 64);     // a key: C = pad 9 (index 8)... already there
            r.mgr.noteOffReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 48);
            expectEquals(padsOf(r.step(3)), juce::String("8"));
            r.mgr.noteOnReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 50, 64);     // D = the third bottom pad
            r.mgr.noteOffReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 50);
            expectEquals(padsOf(r.step(3)), juce::String("8 10"));

            r.bankPrev();                                    // TRIG
            r.turn(1, -30);                                  // VEL
            expect(r.step(3).notes[0].velocity < 0.9f, "the selected drum's velocity moved");
            expectWithinAbsoluteError(r.step(3).notes[1].velocity, 64.0f / 127.0f, 1.0e-4f, "the other drum's did not");
            r.padUp(3);
            expectEquals(padsOf(r.step(3)), juce::String("8 10"));

            // The other drum's lane: its own velocity on the same step.
            drums->setSelectedVoice(10);
            r.padDown(3);
            r.wait();
            r.turn(1, 20);
            expect(r.step(3).notes[1].velocity > 64.0f / 127.0f + 0.1f, "this drum's velocity moved");
            expect(r.step(3).notes[0].velocity < 0.9f);
            r.padUp(3);
            expectEquals(padsOf(r.step(3)), juce::String("8 10"), "a hold with an edit removes nothing");
        }

        beginTest("Other engines: a step is still one trig, tapped on and off whole");
        {
            Rig r;
            tap(r, 0);
            expect(r.step(0).trig);
            expectEquals((int) r.step(0).notes.size(), 1);
            tap(r, 0);
            expect(!r.step(0).trig);
            tap(r, 0);
            expect(r.step(0).trig);
            expectEquals(r.step(0).notes[0].pitch, 60);
        }
    }
};

static DrumLaneTests drumLaneTests;
