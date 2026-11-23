#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include "../src/engine/AudioEngine.h"
#include "../src/engine/PerfFx.h"
#include "../src/ui/screens/PerfFxScreen.h"
#include "../src/ui/ScreenManager.h"
#include "../src/ui/GrammarHooks.h"
#include "../src/project/ProjectBridge.h"
#include "fast_measure.h"

// PERF banks and recorded pad moves.
class PerfBanksMovesTests : public juce::UnitTest
{
public:
    PerfBanksMovesTests() : UnitTest("Perf Banks Moves") {}

    using Signal = std::vector<float>;

    // The PERF screen inside a ScreenManager, driven by controller messages.
    struct Plain : Screen
    {
        juce::String getName() const override { return "OTHER"; }
        void render(DisplayRenderer&) override {}
    };
    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        ScreenManager mgr;
        PerfFxScreen* screen = nullptr;
        juce::int64 now = 1000;

        Rig()
        {
            seq.prepareToPlay(48000.0, 64);
            audio.setSequencer(&seq);
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                audio.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.prepare(48000.0, 64);
            mgr.setSequencer(&seq);
            mgr.setClock([this] { return now; });
            mgr.setHelpBusyWindow(0);
            mgr.setEngineHooks(GrammarHooks::make(seq, audio));
            mgr.addScreen(std::make_unique<Plain>());
            auto s = std::make_unique<PerfFxScreen>(audio, [this] { return seq.getSelectedTrack(); },
                                                    [this] { return seq.getPattern().getTrackCount(); });
            s->setSequencer(&seq);
            screen = s.get();
            mgr.addScreen(std::move(s));
            mgr.setShiftPadScreen(14, 1);           // Shift + bottom pad 7
            mgr.setPinnable(1, true);
            mgr.projectLoaded();
        }

        PerfFxRack& rack() { return audio.getPerfFx(); }
        static int padNote(int pad) { return pad < 8 ? LaunchkeyController::PAD_TOP_ROW_START + pad
                                                     : LaunchkeyController::PAD_BOTTOM_ROW_START + pad - 8; }
        void padOn(int pad, int velocity = 100) { mgr.noteOnReceived(1, padNote(pad), velocity); }
        void padOff(int pad) { mgr.noteOffReceived(1, padNote(pad)); }
        void padPressure(int pad, int value) { mgr.aftertouchReceived(1, padNote(pad), value); }
        void shift(bool on) { mgr.controlChangeReceived(7, 63, on ? 127 : 0); }
        void func(bool on)  { mgr.controlChangeReceived(1, 105, on ? 127 : 0); }
        void tap(int cc)    { mgr.controlChangeReceived(1, cc, 127); mgr.controlChangeReceived(1, cc, 0); }
        void turn(int knob, int clicks) { mgr.controlChangeReceived(16, 0x15 + knob, clicks < 0 ? 128 + clicks : clicks); }
        void edit(int cc) { func(true); tap(cc); func(false); }
        void undo() { shift(true); func(true); tap(115); func(false); shift(false); }
        // Up / Down buttons.
        void up()   { tap(106); }
        void down()  { tap(107); }
    };

    void runTest() override
    {
        beginTest("Bank count constant");
        expect(PerfFx::NUM_BANKS == 4);
        expect(PerfFxRack::TOTAL_PADS == 64);

        beginTest("Bank switching");
        {
            Rig rig;
            auto& rack = rig.rack();
            expect(rack.getBank() == 0);

            // Put an effect on pad 0, bank 0.
            rack.setKind(0, 0, PerfFx::DUB);
            expect(rack.get(0, 0).kind == PerfFx::DUB);

            // Switch to bank 1.
            rack.setBank(1);
            expect(rack.getBank() == 1);
            expect(rack.get(0, 0).kind == PerfFx::NONE);  // bank 1 is empty

            // Put a freeze on bank 1 pad 0.
            rack.setKind(0, 0, PerfFx::FRZ);
            expect(rack.get(0, 0).kind == PerfFx::FRZ);

            // Switch back to bank 0: the dub is still there.
            rack.setBank(0);
            expect(rack.get(0, 0).kind == PerfFx::DUB);

            // Bank 1's freeze is still there.
            rack.setBank(1);
            expect(rack.get(0, 0).kind == PerfFx::FRZ);
        }

        beginTest("Bank switching through the screen (Up / Down)");
        {
            Rig rig;
            auto& rack = rig.rack();
            rig.mgr.setActiveScreen(1);    // go to PERF
            rack.setKind(0, 0, PerfFx::DUB);

            // Down switches to bank 1.
            rig.down();
            expectEquals(rack.getBank(), 1);
            expectEquals(rig.screen->getShownBank(), 1);

            // Up wraps to bank 0.
            rig.up();
            expectEquals(rack.getBank(), 0);

            // Three more Downs go to bank 3.
            rig.down(); rig.down(); rig.down();
            expectEquals(rack.getBank(), 3);
            rig.down();
            expectEquals(rack.getBank(), 0);
        }

        beginTest("Bank name in name bar");
        {
            Rig rig;
            rig.mgr.setActiveScreen(1);    // go to PERF

            juce::String sub = rig.screen->getSubPage();
            expect(sub.startsWith("B1"), "initial: " + sub);

            rig.down();
            sub = rig.screen->getSubPage();
            expect(sub.startsWith("B2"), "after down: " + sub);
        }

        beginTest("toFlat / fromFlat preserves banks");
        {
            Rig rig;
            auto& rack = rig.rack();

            // Bank 0 pad 0: DUB.
            rack.setKind(0, 0, PerfFx::DUB);

            // Bank 2 pad 3: FRZ.
            rack.setBank(2);
            rack.setKind(3, 0, PerfFx::FRZ);

            auto flat = rack.toFlat();
            expect(flat.size() > 6);

            // The header: version 2, 4 banks.
            expectEquals((int) flat[0], 2);
            expectEquals((int) flat[1], PerfFx::NUM_BANKS);

            // Clear and restore.
            rack.setBank(0);
            rack.silence();
            for (int p = 0; p < PerfFx::PADS; ++p)
                rack.clear(p);
            rack.setBank(2);
            for (int p = 0; p < PerfFx::PADS; ++p)
                rack.clear(p);

            expect(rack.fromFlat(flat));

            // Check bank 0 pad 0.
            rack.setBank(0);
            expect(rack.get(0, 0).kind == PerfFx::DUB);

            // Check bank 2 pad 3.
            rack.setBank(2);
            expect(rack.get(3, 0).kind == PerfFx::FRZ);
        }

        beginTest("Old one-bank format loads into bank 0");
        {
            Rig rig;
            auto& rack = rig.rack();

            // Build a one-bank flat (the old format had banks=1).
            rack.setKind(5, 0, PerfFx::RPT);
            auto flat = rack.toFlat();
            // Fake the old header: banks = 1.
            flat[1] = 1.0f;
            // Truncate to just the first 16 pads.
            const size_t perPad = (size_t) PerfFx::PAD_FIELDS + (size_t) PerfFx::LAYERS * (size_t) PerfFx::LAYER_FIELDS;
            flat.resize(6 + 16 * perPad);

            // Clear everything.
            for (int b = 0; b < PerfFx::NUM_BANKS; ++b)
            {
                rack.setBank(b);
                for (int p = 0; p < PerfFx::PADS; ++p)
                    rack.clear(p);
            }

            expect(rack.fromFlat(flat));
            rack.setBank(0);
            expect(rack.get(5, 0).kind == PerfFx::RPT);

            // Bank 1 should be empty.
            rack.setBank(1);
            expect(rack.get(5, 0).kind == PerfFx::NONE);
        }

        beginTest("Absolute pad access across banks");
        {
            Rig rig;
            auto& rack = rig.rack();

            // Bank 2 pad 5 = absolute pad 37.
            rack.setBank(2);
            rack.setKind(5, 0, PerfFx::DUB);

            rack.setBank(0);  // switch away
            // pressAbs should work regardless of active bank.
            rack.pressAbs(2 * PerfFx::PADS + 5, 0);
            expect(rack.isDownAbs(2 * PerfFx::PADS + 5));
            rack.releaseAbs(2 * PerfFx::PADS + 5);
            expect(! rack.isDownAbs(2 * PerfFx::PADS + 5));
        }

        beginTest("MoveLane add and serialise");
        {
            PerfFx::MoveLane lane;
            expect(lane.empty());

            PerfFx::Move m;
            m.stepTime = 1.5f;
            m.pad = 3;
            m.bank = 0;
            m.type = PerfFx::MOVE_PRESS;
            m.pressure = 100;
            m.selectedTrack = 2;
            lane.add(m);

            m.stepTime = 2.0f;
            m.type = PerfFx::MOVE_PRESSURE;
            m.pressure = 200;
            lane.add(m);

            m.stepTime = 3.0f;
            m.type = PerfFx::MOVE_RELEASE;
            lane.add(m);

            expectEquals((int) lane.moves.size(), 3);

            // Serialise and restore.
            auto flat = lane.toFlat();
            PerfFx::MoveLane restored;
            expect(restored.fromFlat(flat));
            expectEquals((int) restored.moves.size(), 3);
            expectEquals(restored.moves[0].stepTime, 1.5f);
            expectEquals((int) restored.moves[0].pad, 3);
            expectEquals((int) restored.moves[0].type, (int) PerfFx::MOVE_PRESS);
            expectEquals((int) restored.moves[1].type, (int) PerfFx::MOVE_PRESSURE);
            expectEquals((int) restored.moves[2].type, (int) PerfFx::MOVE_RELEASE);
        }

        beginTest("MoveLane in pattern and project");
        {
            Rig rig;
            auto& rack = rig.rack();
            auto& pat = rig.seq.getPattern();

            // Add a move to the pattern.
            PerfFx::Move m;
            m.stepTime = 4.0f;
            m.pad = 1;
            m.bank = 0;
            m.type = PerfFx::MOVE_PRESS;
            m.selectedTrack = 0;
            pat.perfMoves.add(m);
            expect(! pat.perfMoves.empty());

            // Set up an effect so the project has something to save.
            rack.setKind(0, 0, PerfFx::DUB);

            // Capture to ProjectData.
            ProjectBridge::Live live { rig.seq, rig.audio };
            ProjectData data = ProjectBridge::capture(live, "test");

            // The pattern should have perfMoves.
            expect(! data.patterns[0].perfMoves.empty());

            // Clear and restore.
            pat.perfMoves.clear();
            expect(pat.perfMoves.empty());

            ProjectBridge::apply(data, live);

            expect(! rig.seq.getPattern().perfMoves.empty());
            expectEquals((int) rig.seq.getPattern().perfMoves.moves.size(), 1);
            expectEquals(rig.seq.getPattern().perfMoves.moves[0].stepTime, 4.0f);
        }

        beginTest("Recording perf moves while playing");
        {
            Rig rig;
            auto& rack = rig.rack();

            // Set up an effect on pad 0.
            rack.setKind(0, 0, PerfFx::DUB);

            // Start playing + recording.
            rig.seq.setRecording(true);
            rig.seq.start();
            expect(rig.seq.isPlaying());
            expect(rig.seq.isRecording());

            // Run a few blocks to advance the playhead.
            for (int i = 0; i < 10; ++i)
                rig.seq.processBlock(64);

            // Go to PERF screen.
            rig.shift(true);
            rig.padOn(14);
            rig.padOff(14);
            rig.shift(false);

            // Press pad 0 (which records).
            rig.padOn(0, 100);
            // Send some pressure.
            rig.padPressure(0, 64);
            // Release.
            rig.padOff(0);

            // The moves wait for the audio thread: the next block puts them into the lane.
            expect(rig.seq.getPattern().perfMoves.empty(), "nothing is written from the pads' thread");
            rig.seq.processBlock(64);

            // Check moves were recorded: press, pressure, release, in the order played.
            const auto& moves = rig.seq.getPattern().perfMoves.moves;
            expectEquals((int) moves.size(), 3);
            if (moves.size() == 3)
                expect(moves[0].type == PerfFx::MOVE_PRESS && moves[1].type == PerfFx::MOVE_PRESSURE && moves[2].type == PerfFx::MOVE_RELEASE,
                       "press, pressure, release in the order played");

            bool hasPress = false, hasRelease = false;
            for (auto& mv : moves)
            {
                if (mv.type == PerfFx::MOVE_PRESS) hasPress = true;
                if (mv.type == PerfFx::MOVE_RELEASE) hasRelease = true;
            }
            expect(hasPress);
            expect(hasRelease);

            rig.seq.stop();
        }

        beginTest("Playback fires recorded moves");
        {
            Rig rig;
            auto& rack = rig.rack();
            rack.setKind(0, 0, PerfFx::DUB);
            rack.prepare(48000.0, 64);

            // Record a press at step 2 and release at step 4.
            auto& lane = rig.seq.getPattern().perfMoves;
            PerfFx::Move m;
            m.pad = 0;
            m.bank = 0;
            m.selectedTrack = 0;

            m.stepTime = 2.0f;
            m.type = PerfFx::MOVE_PRESS;
            lane.add(m);

            m.stepTime = 4.0f;
            m.type = PerfFx::MOVE_RELEASE;
            lane.add(m);

            // Set up the playback callback.
            bool pressed = false, released = false;
            rig.seq.setPerfMoveCallback([&](const PerfFx::Move& move)
            {
                if (move.type == PerfFx::MOVE_PRESS)
                {
                    pressed = true;
                    rack.pressAbs((int) move.bank * PerfFx::PADS + (int) move.pad, (int) move.selectedTrack);
                }
                else if (move.type == PerfFx::MOVE_RELEASE)
                {
                    released = true;
                    rack.releaseAbs((int) move.bank * PerfFx::PADS + (int) move.pad);
                }
            });

            // Start and run enough blocks for 5 steps at 120 BPM.
            // At 48000 Hz, 120 BPM: 1 step = 48000 / (120/60) / 4 = 6000 samples.
            rig.seq.start();
            const int blocksFor5Steps = (6000 * 5) / 64 + 1;
            for (int i = 0; i < blocksFor5Steps; ++i)
                rig.seq.processBlock(64);

            expect(pressed);
            expect(released);

            rig.seq.stop();
        }

        beginTest("Recording takes no lock and no memory: a burst of pad messages between two blocks is in the lane after the next one, in the order played");
        {
            Rig rig;
            int locks = 0;
            rig.seq.setStructureLock([&locks](const std::function<void()>& work) { ++locks; work(); });
            rig.seq.setRecording(true);
            rig.seq.start();
            for (int i = 0; i < 10; ++i)
                rig.seq.processBlock(64);
            const auto& lane = rig.seq.getPattern().perfMoves;
            const size_t room = lane.moves.capacity();
            expect(room >= (size_t) PerfFx::MoveLane::MAX_MOVES);

            PerfFx::Move m;
            m.type = PerfFx::MOVE_PRESSURE;
            for (int i = 0; i < 500; ++i)
            {
                m.pad = (uint8_t) (i % 16);
                m.pressure = (uint8_t) (i % 251);
                rig.seq.recordPerfMove(m);
            }
            expectEquals(locks, 0, "recordPerfMove takes the structure lock");
            expect(lane.empty());
            rig.seq.processBlock(64);
            expectEquals((int) lane.moves.size(), 500);
            bool inOrder = lane.moves.size() == 500;
            for (size_t i = 0; inOrder && i < 500; ++i)
                inOrder = lane.moves[i].pressure == (uint8_t) (i % 251) && lane.moves[i].pad == (uint8_t) (i % 16);
            expect(inOrder, "the moves of one block are in the lane in the order they were played");
            expectEquals(rig.seq.getPerfMovesLost(), 0);
            expect(lane.moves.capacity() == room, "the lane took no memory");

            // More than the ring holds while no block is played: the rest is lost and counted,
            // nothing is written past the ring.
            for (int i = 0; i < 2000; ++i)
                rig.seq.recordPerfMove(m);
            rig.seq.processBlock(64);
            expectEquals((int) lane.moves.size(), 500 + 1024);
            expectEquals(rig.seq.getPerfMovesLost(), 2000 - 1024);

            // A lane that is full takes no more, and takes no memory for it.
            for (int round = 0; round < 4; ++round)
            {
                for (int i = 0; i < 1000; ++i)
                    rig.seq.recordPerfMove(m);
                rig.seq.processBlock(64);
            }
            expectEquals((int) lane.moves.size(), PerfFx::MoveLane::MAX_MOVES);
            expect(lane.moves.capacity() == room, "a full lane took no memory");
            expectEquals(locks, 0);
            rig.seq.stop();

            // A copy of a lane, and of a pattern, has the room too (the audio thread may write it).
            PerfFx::MoveLane small;
            small.add(m);
            const PerfFx::MoveLane copy(small);
            expect(copy.moves.size() == 1 && copy.moves.capacity() >= (size_t) PerfFx::MoveLane::MAX_MOVES);
            Pattern pattern;
            pattern.perfMoves.add(m);
            const Pattern second = pattern, third = pattern.duplicate();
            expect(second.perfMoves.moves.capacity() >= (size_t) PerfFx::MoveLane::MAX_MOVES
                   && third.perfMoves.moves.capacity() >= (size_t) PerfFx::MoveLane::MAX_MOVES);
        }

        beginTest("The lane has no clock of its own: a move recorded into an empty lane is played back at the step it was recorded at");
        {
            // (The playhead the lane had of its own stood still while the lane was empty, so the
            // first take of a pattern came back late by the time it was begun at.)
            Rig rig;
            rig.seq.setRecording(true);
            rig.seq.start();
            // 120 BPM at 48 kHz: 6000 samples a step. Into step 3 (18000 .. 24000).
            int blocks = 0;
            for (; blocks < 300; ++blocks)
                rig.seq.processBlock(64);
            expectEquals(rig.seq.getCurrentStep(0), 3);
            PerfFx::Move m;
            m.type = PerfFx::MOVE_PRESS;
            rig.seq.recordPerfMove(m);
            rig.seq.processBlock(64);
            const auto& lane = rig.seq.getPattern().perfMoves;
            expectEquals((int) lane.moves.size(), 1);
            if (lane.moves.size() == 1)
                expectWithinAbsoluteError(lane.moves[0].stepTime, 300.0f * 64.0f / 6000.0f, 0.001f);

            std::vector<int> firedAtStep;
            rig.seq.setPerfMoveCallback([&](const PerfFx::Move&) { firedAtStep.push_back(rig.seq.getCurrentStep(0)); });
            // Two more times round the sixteen steps: it is played once each time, at step 3.
            for (int i = 0; i < 2 * 1500; ++i)
                rig.seq.processBlock(64);
            expectEquals((int) firedAtStep.size(), 2);
            for (int step : firedAtStep)
                expectEquals(step, 3);
            rig.seq.stop();
        }

        beginTest("Moves are played in time order, also in the block that goes round the pattern's end; REC QUANT puts a move on its step");
        {
            Rig rig;
            auto& lane = rig.seq.getPattern().perfMoves;
            PerfFx::Move m;
            m.type = PerfFx::MOVE_PRESS;
            m.pad = 2; m.stepTime = 0.003f;  lane.add(m);       // just after the top
            m.pad = 1; m.stepTime = 15.997f; lane.add(m);       // just before the end
            std::vector<int> order;
            rig.seq.setPerfMoveCallback([&](const PerfFx::Move& move) { order.push_back((int) move.pad); });
            rig.seq.start();
            // Blocks of 70: the 1372nd covers samples 95970 .. 96040, both sides of the end (96000).
            for (int i = 0; i < 1371; ++i)
                rig.seq.processBlock(70);
            // (the move at the top was played as the transport started)
            expect(order == std::vector<int> { 2 }, "before the block round the end: the move at the top, once");
            order.clear();
            rig.seq.processBlock(70);
            expect(order == std::vector<int> { 1, 2 }, "round the end: the move before it, then the move after it");
            rig.seq.stop();

            // Moves at one time stay in the order they were put in.
            PerfFx::MoveLane same;
            for (int i = 0; i < 5; ++i)
            {
                m.stepTime = 4.0f;
                m.pressure = (uint8_t) i;
                same.add(m);
            }
            bool kept = true;
            for (int i = 0; i < 5; ++i)
                kept = kept && same.moves[(size_t) i].pressure == (uint8_t) i;
            expect(kept, "moves at one time keep their order");

            // REC QUANT: the nearest step, and the step past the last is the first.
            Rig quant;
            quant.seq.setRecordMode(SequencerCore::RecordMode::Quant);
            quant.seq.setRecording(true);
            quant.seq.start();
            for (int i = 0; i < 1480; ++i)          // 94720 samples: step 15.79
                quant.seq.processBlock(64);
            quant.seq.recordPerfMove(m);
            quant.seq.processBlock(64);
            const auto& q = quant.seq.getPattern().perfMoves;
            expectEquals((int) q.moves.size(), 1);
            if (q.moves.size() == 1)
                expectEquals(q.moves[0].stepTime, 0.0f);
            quant.seq.stop();
        }

        beginTest("Another pattern: a pad the last pattern's lane holds down is let go");
        {
            Rig rig;
            PerfFx::Move m;
            m.type = PerfFx::MOVE_PRESS;
            m.pad = 4;
            m.stepTime = 1.0f;
            rig.seq.getPattern().perfMoves.add(m);
            int presses = 0, releases = 0, releasedPad = -1;
            rig.seq.setPerfMoveCallback([&](const PerfFx::Move& move)
            {
                if (move.type == PerfFx::MOVE_PRESS) ++presses;
                if (move.type == PerfFx::MOVE_RELEASE) { ++releases; releasedPad = (int) move.pad; }
            });
            rig.seq.start();
            for (int i = 0; i < 200; ++i)
                rig.seq.processBlock(64);
            expectEquals(presses, 1);
            expectEquals(releases, 0);
            rig.seq.selectPattern(1, SequencerCore::ChangeMode::Immediate);
            rig.seq.processBlock(64);
            expectEquals(releases, 1);
            expectEquals(releasedPad, 4);
            rig.seq.stop();
            expectEquals(releases, 1, "let go once, not again on stop");
        }

        beginTest("Transport stop releases playback pads");
        {
            Rig rig;
            auto& rack = rig.rack();
            rack.setKind(0, 0, PerfFx::DUB);
            rack.prepare(48000.0, 64);

            // Record a press at step 1, no release.
            auto& lane = rig.seq.getPattern().perfMoves;
            PerfFx::Move m;
            m.pad = 0;
            m.bank = 0;
            m.selectedTrack = 0;
            m.stepTime = 1.0f;
            m.type = PerfFx::MOVE_PRESS;
            lane.add(m);

            bool releaseSent = false;
            rig.seq.setPerfMoveCallback([&](const PerfFx::Move& move)
            {
                if (move.type == PerfFx::MOVE_PRESS)
                    rack.pressAbs(0, 0);
                else if (move.type == PerfFx::MOVE_RELEASE)
                {
                    releaseSent = true;
                    rack.releaseAbs(0);
                }
            });

            rig.seq.start();
            // Run past step 1.
            for (int i = 0; i < 200; ++i)
                rig.seq.processBlock(64);

            // Stop: should release all playback pads.
            rig.seq.stop();
            expect(releaseSent);
        }

        beginTest("Live finger wins over playback");
        {
            Rig rig;
            auto& rack = rig.rack();
            rack.setKind(0, 0, PerfFx::DUB);
            rack.prepare(48000.0, 64);

            // Record a press at step 2.
            auto& lane = rig.seq.getPattern().perfMoves;
            PerfFx::Move m;
            m.pad = 0;
            m.bank = 0;
            m.selectedTrack = 0;
            m.stepTime = 2.0f;
            m.type = PerfFx::MOVE_PRESS;
            lane.add(m);

            bool playbackPressFired = false;
            rig.seq.setPerfMoveCallback([&](const PerfFx::Move& move)
            {
                if (move.type == PerfFx::MOVE_PRESS)
                    playbackPressFired = true;
            });

            // Mark pad 0 as live-held.
            rig.seq.setPerfPadLive(0, true);

            rig.seq.start();
            // Run past step 2.
            for (int i = 0; i < 300; ++i)
                rig.seq.processBlock(64);

            // The playback press should have been skipped.
            expect(! playbackPressFired);

            rig.seq.stop();
        }

        beginTest("Clear perf moves");
        {
            Rig rig;
            auto& lane = rig.seq.getPattern().perfMoves;

            PerfFx::Move m;
            m.stepTime = 1.0f;
            m.type = PerfFx::MOVE_PRESS;
            lane.add(m);
            expect(! lane.empty());

            rig.seq.clearPerfMoves();
            expect(lane.empty());
        }

        beginTest("Layer selector on PRESS page");
        {
            Rig rig;
            auto& rack = rig.rack();
            rig.mgr.setActiveScreen(1);    // go to PERF

            // Put effects on two layers.
            rack.setKind(0, 0, PerfFx::DUB);
            rack.setKind(0, 1, PerfFx::FRZ);
            rig.padOn(0); rig.padOff(0);   // select pad 0

            expect(rig.screen->getShownLayer() == 0);

            // Go to PRESS page (bank down = page right).
            rig.tap(52);
            // Turn knob 2 (the LY cell) to go to layer B.
            rig.turn(2, 1);
            expectEquals(rig.screen->getShownLayer(), 1);
        }

        beginTest("Clear take from the controller (TAKE cell on MORE, four clicks, undoable)");
        {
            Rig rig;
            auto& rack = rig.rack();
            rack.setKind(0, 0, PerfFx::DUB);
            rig.mgr.setActiveScreen(1);       // go to PERF
            rig.padOn(0); rig.padOff(0);      // select pad 0

            // Add some moves to the pattern.
            PerfFx::Move m;
            m.stepTime = 1.0f; m.type = PerfFx::MOVE_PRESS; m.pad = 0; m.bank = 0;
            rig.seq.getPattern().perfMoves.add(m);
            m.stepTime = 2.0f; m.type = PerfFx::MOVE_RELEASE;
            rig.seq.getPattern().perfMoves.add(m);
            expect(! rig.seq.getPattern().perfMoves.empty());

            // Go to MORE page (three bank-downs = three page rights from FX).
            rig.tap(52); rig.tap(52); rig.tap(52);
            expectEquals(rig.screen->getPageIndex(), 3);

            // A single click on knob 7 (the TAKE cell) does nothing.
            rig.turn(6, 1);
            expect(! rig.seq.getPattern().perfMoves.empty(), "one click does not clear");

            // Four clicks clear the take.
            UiClock::advance(150);
            rig.turn(6, 1);
            UiClock::advance(150);
            rig.turn(6, 1);
            UiClock::advance(150);
            rig.turn(6, 1);
            UiClock::advance(150);
            rig.turn(6, 1);
            expect(rig.seq.getPattern().perfMoves.empty(), "four clicks: take cleared");

            // Undo brings the take back.
            rig.undo();
            expect(! rig.seq.getPattern().perfMoves.empty(), "undo restores the take");
            expectEquals((int) rig.seq.getPattern().perfMoves.moves.size(), 2);
        }

        beginTest("Clear take says NO TAKE when nothing is recorded");
        {
            Rig rig;
            auto& rack = rig.rack();
            rack.setKind(0, 0, PerfFx::DUB);
            rig.mgr.setActiveScreen(1);
            rig.padOn(0); rig.padOff(0);
            expect(rig.seq.getPattern().perfMoves.empty());

            // Go to MORE page.
            rig.tap(52); rig.tap(52); rig.tap(52);
            // Turn the TAKE knob: should say NO TAKE.
            rig.turn(6, 1);
            expect(rig.screen->getSubPage().contains("NO TAKE") || rig.seq.getPattern().perfMoves.empty());
        }

        beginTest("Ringing layers across bank switch");
        {
            // A ringing layer on bank 0 should keep sounding when switching to bank 1.
            Rig rig;
            auto& rack = rig.rack();

            // Bank 0 pad 0: a DUB thrown.
            rack.setKind(0, 0, PerfFx::DUB);
            PerfFx::Layer lay = rack.get(0, 0);
            lay.mode = PerfFx::THROW;
            rack.setLayer(0, 0, lay);
            rack.prepare(48000.0, 64);

            // Press and release (the throw rings).
            rack.press(0, 0);
            PerfFxRack::Context ctx;
            ctx.bpm = 120.0f;
            ctx.beatsPerSample = 2.0 / 48000.0;
            ctx.playing = false;
            Signal buf(64, 0.1f);
            Signal bufR(64, 0.1f);
            rack.beginBlock(ctx, 64);
            rack.process(0, buf.data(), bufR.data(), 64);
            rack.release(0);

            // The pad should still be sounding (ringing).
            expect(rack.isSounding(0));

            // Switch to bank 1.
            rack.setBank(1);

            // The pad on bank 0 should still be sounding (absolute pad 0).
            expect(rack.isSoundingAbs(0));
        }
    }
};

static PerfBanksMovesTests perfBanksMovesTests;
