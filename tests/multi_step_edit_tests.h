#pragma once

#include "held_step_pages_tests.h"

// Several steps held at once are edited together (owner's request, 2026-11-11): the pages show the
// step on the lowest pad, and what a knob or a key gives that step is given to every held step -
// the same value, not the same change. On STEP (TRIG page and lock pages) and on PARAM / TRK FX /
// FLTR, whose pads hold steps for locks.
class MultiStepEditTests : public juce::UnitTest
{
public:
    MultiStepEditTests() : UnitTest("Multi Step Edit") {}

    void runTest() override
    {
        using heldsteptest::Rig;
        const int RESO = UnifiedParam::RESONANCE;

        beginTest("STEP, lock pages: a turn locks every held step to the value shown; > + turn removes it from all");
        {
            Rig r;
            r.step(9).trig = true;
            r.step(9).notes.push_back({ 60, 1.0f });
            r.step(9).paramLocks[RESO] = 0.05f;             // one of them already has a different lock
            r.padDown(2);
            r.padDown(5);
            r.padDown(9);
            r.wait();
            r.turn(1, 10);
            expect(r.step(2).paramLocks.count(RESO) > 0);
            const float shown = r.step(2).paramLocks[RESO];
            expectWithinAbsoluteError(r.step(5).paramLocks[RESO], shown, 1.0e-6f);
            expectWithinAbsoluteError(r.step(9).paramLocks[RESO], shown, 1.0e-6f, "the value shown, not its own plus ten clicks");
            expect(r.step(3).paramLocks.empty() && r.step(4).paramLocks.empty(), "steps between the held ones are untouched");
            expect(r.step(2).trig && r.step(5).trig && r.step(9).trig);

            r.turn(1, -3);
            expectWithinAbsoluteError(r.step(5).paramLocks[RESO], r.step(2).paramLocks[RESO], 1.0e-6f);
            expect(r.step(2).paramLocks[RESO] < shown);

            // One let go: it keeps what it has, the others move on.
            r.padUp(5);
            const float kept = r.step(5).paramLocks[RESO];
            r.turn(1, 5);
            expectWithinAbsoluteError(r.step(5).paramLocks[RESO], kept, 1.0e-6f);
            expectWithinAbsoluteError(r.step(9).paramLocks[RESO], r.step(2).paramLocks[RESO], 1.0e-6f);
            expect(r.step(2).paramLocks[RESO] > kept);

            // The step shown let go: the next lowest is shown and edited.
            r.padUp(2);
            r.padDown(5);
            r.turn(1, 2);
            expectWithinAbsoluteError(r.step(9).paramLocks[RESO], r.step(5).paramLocks[RESO], 1.0e-6f);

            // Hold > and turn: the lock goes from every held step.
            r.mgr.controlChangeReceived(1, ScreenManager::CC_PATTERN_KEY, 127);
            r.turn(1, 1);
            r.mgr.controlChangeReceived(1, ScreenManager::CC_PATTERN_KEY, 0);
            expect(r.step(5).paramLocks.count(RESO) == 0 && r.step(9).paramLocks.count(RESO) == 0);
            expect(r.step(2).paramLocks.count(RESO) > 0, "a step no longer held keeps its lock");
            r.padUp(5);
            r.padUp(9);
        }

        beginTest("STEP, TRIG page: VEL, LEN, MICR, COND and PROB go to every held step; a key sets every step's note");
        {
            Rig r;
            r.padDown(1);
            r.padDown(6);
            r.wait();
            r.bankPrev();                                    // MAIN -> TRIG
            expectEquals(r.names().upToFirstOccurrenceOf(" ", false, false), juce::String("NOTE"));

            r.turn(1, -20);                                  // VEL
            expect(r.step(1).notes[0].velocity < 1.0f);
            expectWithinAbsoluteError(r.step(6).notes[0].velocity, r.step(1).notes[0].velocity, 1.0e-6f);
            r.turn(2, 4);                                    // LEN
            expect(r.step(1).length > 1.0f);
            expectWithinAbsoluteError(r.step(6).length, r.step(1).length, 1.0e-6f);
            r.turn(3, 6);                                    // MICR
            expect(r.step(1).microTiming > 0.0f);
            expectWithinAbsoluteError(r.step(6).microTiming, r.step(1).microTiming, 1.0e-6f);
            r.turn(4, 1);                                    // COND: ALL -> PROB
            expect(r.step(1).trigCondition.type == TrigCondition::Probability);
            expect(r.step(6).trigCondition.type == TrigCondition::Probability);
            r.turn(5, 10);                                   // PROB
            expect(r.step(1).trigCondition.probability > 0.5f);
            expectWithinAbsoluteError(r.step(6).trigCondition.probability, r.step(1).trigCondition.probability, 1.0e-6f);
            r.turn(0, 7);                                    // NOTE
            expectEquals(r.step(1).notes[0].pitch, 67);
            expectEquals(r.step(6).notes[0].pitch, 67);

            r.mgr.noteOnReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 50, 90);
            r.mgr.noteOffReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 50);
            expectEquals(r.step(1).notes[0].pitch, 50);
            expectEquals(r.step(6).notes[0].pitch, 50);

            // > + turn: the cell's default, on both.
            r.mgr.controlChangeReceived(1, ScreenManager::CC_PATTERN_KEY, 127);
            r.turn(2, 1);
            r.mgr.controlChangeReceived(1, ScreenManager::CC_PATTERN_KEY, 0);
            expectWithinAbsoluteError(r.step(1).length, 1.0f, 1.0e-6f);
            expectWithinAbsoluteError(r.step(6).length, 1.0f, 1.0e-6f);
            r.padUp(1);
            r.padUp(6);
            expect(r.step(1).trig && r.step(6).trig, "edited steps are not switched off when let go");
        }

        beginTest("STEP, LCKS with several held: every held step's locks go, as one undo step");
        {
            Rig r;
            for (int s : { 2, 4, 7 })
            {
                r.step(s).trig = true;
                r.step(s).notes.push_back({ 60, 1.0f });
                r.step(s).paramLocks[RESO] = 0.5f;
            }
            r.step(7).paramLocks.clear();                    // the step shown may be the one without locks
            r.step(0).trig = true;
            r.step(0).notes.push_back({ 60, 1.0f });
            const int undos = r.mgr.getUndoHistory().undoCount();
            r.padDown(0);                                    // shown: no locks
            r.padDown(2);
            r.padDown(4);
            r.wait();
            r.bankPrev();                                    // TRIG
            r.turn(7, -1);                                   // LCKS down
            expect(r.step(2).paramLocks.empty() && r.step(4).paramLocks.empty());
            expectEquals(r.mgr.getUndoHistory().undoCount(), undos + 1);
            r.padUp(0); r.padUp(2); r.padUp(4);
            expect(r.step(0).trig && r.step(2).trig && r.step(4).trig);
        }

        beginTest("PARAM: pads hold steps; a turn locks all of them, > + turn unlocks all, and all light as held");
        {
            Rig r;
            r.mgr.setActiveScreen(Rig::PARAM);
            r.padDown(1);
            r.padDown(3);
            r.wait();
            r.turn(1, 8);                                    // MAIN: RESO
            expect(r.step(1).paramLocks.count(RESO) > 0);
            expect(r.step(3).paramLocks.count(RESO) > 0);
            expectWithinAbsoluteError(r.step(3).paramLocks[RESO], r.step(1).paramLocks[RESO], 1.0e-6f);
            expect(r.step(2).paramLocks.empty());

            std::array<Screen::PadColour, 16> pads;
            expect(r.param->getPadColours(pads));
            auto same = [](const Screen::PadColour& a, const Screen::PadColour& b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
            expect(same(pads[1], pads[3]), "both held pads have the held colour");
            expect(!same(pads[1], pads[2]));

            r.mgr.controlChangeReceived(1, ScreenManager::CC_PATTERN_KEY, 127);
            r.turn(1, 1);
            r.mgr.controlChangeReceived(1, ScreenManager::CC_PATTERN_KEY, 0);
            expect(r.step(1).paramLocks.empty() && r.step(3).paramLocks.empty());
            r.padUp(1);
            r.padUp(3);
        }

        beginTest("TRK FX: the same on an effect cell");
        {
            Rig r;
            r.mgr.setActiveScreen(Rig::TRKFX);
            r.padDown(0);
            r.padDown(5);
            r.wait();
            r.turn(0, 12);
            expectEquals((int) r.step(0).paramLocks.size(), 1);
            expect(r.step(5).paramLocks == r.step(0).paramLocks);
            r.padUp(0);
            r.padUp(5);
        }
    }
};

static MultiStepEditTests multiStepEditTests;
