#pragma once
#include <juce_core/juce_core.h>
#include "control_grammar_tests.h"
#include "../src/controller/CustomScaleManager.h"
#include "../src/ui/PadLEDFeedback.h"
#include "../src/ui/screens/StepViewScreen.h"
#include "../src/ui/screens/DrumPadScreen.h"

// Live recording (Record + Play) and the Scale button's feedback, driven by the hardware's own
// messages: Record CC 117, Play CC 115 and Scale CC 110 on channel 1, keys from the keybed port,
// pads as notes on channel 1.
namespace lrtest
{
    using cgtest::Rig;
    enum : int { PLAY = 115, RECORD = 117, SCALE = 110, PATTERN_KEY = 104 };

    static constexpr double STEP = 5512.5;      // samples per step at 120 BPM, 44.1 kHz

    // A screen standing in for STEP: it notes which step it was asked to show.
    struct StepRec : cgtest::Rec
    {
        StepRec() : Rec("STEP") {}
        int shown = -1;
        void showStep(int stepIndex) override { shown = stepIndex; }
    };

    // The keys as the app wires them (main.cpp, KeyboardToEngine): the selected track, recorded
    // while the sequencer records and runs.
    struct RecordingKeys : ScreenManager::KeyboardListener
    {
        SequencerCore& seq;
        juce::StringArray log;
        explicit RecordingKeys(SequencerCore& s) : seq(s) {}
        void keyboardNoteOn(int note, int velocity) override
        {
            log.add("on" + juce::String(note));
            if (seq.isRecording() && seq.isPlaying())
                seq.recordNote(seq.getSelectedTrack(), note, (float) velocity / 127.0f);
        }
        void keyboardNoteOff(int note) override { log.add("off" + juce::String(note)); }
    };

    // Runs the sequencer and notes every trig with the sample it sounded at (to the block).
    struct Clock
    {
        struct Trig { juce::int64 at; int track; int pitch; };
        SequencerCore& seq;
        juce::int64 now = 0;
        std::vector<Trig> trigs;

        explicit Clock(SequencerCore& s) : seq(s)
        {
            seq.setTrigCallback([this](int track, const Step& step)
            {
                for (auto& n : step.notes)
                    trigs.push_back({ now, track, n.pitch });
            });
        }
        void runTo(double steps)
        {
            const auto target = (juce::int64) std::llround(steps * STEP);
            while (now < target)
            {
                const int n = (int) juce::jmin((juce::int64) 16, target - now);
                seq.processBlock(n);
                now += n;
            }
        }
        void restart() { now = 0; trigs.clear(); }
        // When `pitch` sounded for the first time, in steps (-1 = never).
        double firstAt(int pitch) const
        {
            for (auto& t : trigs)
                if (t.pitch == pitch)
                    return (double) t.at / STEP;
            return -1.0;
        }
    };

    inline void recordPlay(Rig& r) { r.press(RECORD); r.tap(PLAY); r.release(RECORD); }

    inline const Step* findNote(const SequencerCore& seq, int track, int pitch, int* index = nullptr)
    {
        const auto& tr = seq.getPattern().getTrack(track);
        for (int s = 0; s < tr.length; ++s)
            for (auto& n : tr.steps[(size_t) s].notes)
                if (tr.steps[(size_t) s].trig && n.pitch == pitch)
                {
                    if (index != nullptr) *index = s;
                    return &tr.steps[(size_t) s];
                }
        return nullptr;
    }

    inline bool is(const Screen::PadColour& c, int r, int g, int b) { return c.r == r && c.g == g && c.b == b; }
}

class LiveRecordTests : public juce::UnitTest
{
public:
    LiveRecordTests() : UnitTest("Live Record") {}

    void runTest() override
    {
        using namespace lrtest;
        using Mode = SequencerCore::RecordMode;

        beginTest("Record + Play: recording starts from the top; again = free <-> quantized; the mode is remembered");
        {
            Rig r;
            Clock clock(r.seq);

            r.press(RECORD);
            expect(!r.seq.isRecording(), "Record alone does nothing until it is let go");
            r.tap(PLAY);
            expect(r.seq.isPlaying());
            expect(r.seq.isRecording());
            expectEquals(r.seq.getCurrentStep(0), 0);
            expect(r.seq.getRecordMode() == Mode::Free, "free is the default");
            expectEquals(r.text(), juce::String("REC FREE"));
            r.release(RECORD);
            expect(r.seq.isRecording(), "letting Record go after Record + Play is not a plain Record");
            expect(r.seq.isPlaying());

            clock.runTo(3.5);
            recordPlay(r);
            expect(r.seq.getRecordMode() == Mode::Quant);
            expect(r.seq.isPlaying() && r.seq.isRecording());
            expectEquals(r.seq.getCurrentStep(0), 3, "the transport is not restarted");
            expectEquals(r.text(), juce::String("REC QUANT"));

            // The mode stays in the name bar on every screen after the message has gone.
            r.now += 5000;
            expectEquals(r.mgr.composeNameBar().tags, juce::String("REC QUANT"));
            r.mgr.setActiveScreen(1);
            expectEquals(r.mgr.composeNameBar().tags, juce::String("REC QUANT"));
            r.b->sub = "A LONG STATUS";
            expectEquals(r.mgr.composeNameBar().tags, juce::String("Q"), "short of room: one letter, Q = quantized");
            recordPlay(r);
            expect(r.seq.getRecordMode() == Mode::Free);
            r.now += 5000;
            expectEquals(r.mgr.composeNameBar().tags, juce::String("R"));
            recordPlay(r);
            expect(r.seq.getRecordMode() == Mode::Quant);

            // Play: stop, and recording is over. The next Record + Play is in the mode last used.
            r.tap(PLAY);
            expect(!r.seq.isPlaying());
            expect(!r.seq.isRecording());
            r.now += 5000;
            expect(r.mgr.composeNameBar().tags.isEmpty());
            recordPlay(r);
            expect(r.seq.isPlaying() && r.seq.isRecording());
            expect(r.seq.getRecordMode() == Mode::Quant, "remembered");
            expectEquals(r.seq.getCurrentStep(0), 0, "from the top");
        }

        beginTest("Record + Play while already playing switches recording on without restarting");
        {
            Rig r;
            Clock clock(r.seq);
            r.tap(PLAY);
            clock.runTo(5.5);
            recordPlay(r);
            expect(r.seq.isPlaying() && r.seq.isRecording());
            expectEquals(r.seq.getCurrentStep(0), 5);
            expect(r.seq.getRecordMode() == Mode::Free, "entering does not change the mode");
        }

        beginTest("Plain Record arms / disarms when let go; armed and stopped, Play records; Play + Record");
        {
            Rig r;
            r.press(RECORD);
            expect(!r.seq.isRecording());
            r.release(RECORD);
            expect(r.seq.isRecording());
            expect(!r.seq.isPlaying(), "plain Record does not start the transport");
            expect(r.mgr.getNameBarTags().contains("REC FREE"));

            // Armed and stopped: Record + Play starts, and does not change the mode.
            recordPlay(r);
            expect(r.seq.isPlaying() && r.seq.isRecording());
            expect(r.seq.getRecordMode() == Mode::Free);

            // Plain Record while recording: recording off, the transport keeps running.
            r.tap(RECORD);
            expect(!r.seq.isRecording());
            expect(r.seq.isPlaying());
            expectEquals(r.text(), juce::String("REC OFF"));
            r.tap(PLAY);
            expect(!r.seq.isPlaying());

            // Play a moment before Record (the two pressed "together"): the same result.
            r.press(PLAY);
            expect(r.seq.isPlaying());
            r.press(RECORD);
            expect(r.seq.isRecording());
            r.release(PLAY);
            r.release(RECORD);
            expect(r.seq.isPlaying() && r.seq.isRecording(), "neither release undoes it");

            // ... but when that Play stopped the transport, Record does nothing more.
            r.press(PLAY);
            expect(!r.seq.isPlaying() && !r.seq.isRecording());
            r.press(RECORD);
            r.release(PLAY);
            r.release(RECORD);
            expect(!r.seq.isPlaying() && !r.seq.isRecording());
        }

        beginTest("Func, > and Shift with Record / Play keep their meaning");
        {
            Rig r;
            r.seq.getPattern().getTrack(0).getStep(2).trig = true;

            r.func(true);
            r.press(RECORD);
            expectEquals(r.text(), juce::String("COPY T1 STEPS"));
            r.tap(PLAY);
            expectEquals(r.text(), juce::String("CLEAR T1 STEPS"));
            r.release(RECORD);
            r.func(false);
            expect(!r.seq.isRecording() && !r.seq.isPlaying());

            r.press(PATTERN_KEY);
            r.press(RECORD);
            expectEquals(r.text(), juce::String("KEEP P1"));
            r.tap(PLAY);
            expectEquals(r.text(), juce::String("RELOAD P1"));
            r.release(RECORD);
            r.release(PATTERN_KEY);
            expect(!r.seq.isRecording() && !r.seq.isPlaying());

            r.shift(true);
            r.press(RECORD);
            expect(r.seq.isFillActive());
            r.tap(PLAY);                        // Shift + Play: stop and return
            r.release(RECORD);
            r.shift(false);
            expect(!r.seq.isFillActive());
            expect(!r.seq.isRecording() && !r.seq.isPlaying());

            // Shift + Play ends a recording as plain Play does.
            recordPlay(r);
            expect(r.seq.isRecording());
            r.shift(true); r.tap(PLAY); r.shift(false);
            expect(!r.seq.isRecording() && !r.seq.isPlaying());
        }

        beginTest("Keys record: FREE keeps the timing, QUANT lands on the grid; a chord stays together");
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                const bool quant = (pass == 1);
                Rig r;
                Clock clock(r.seq);
                RecordingKeys keys(r.seq);
                r.mgr.setKeyboardListener(&keys);
                r.seq.getPattern().getTrack(0).getStep(2).length = 2.0f;

                recordPlay(r);
                if (quant)
                    recordPlay(r);
                expect((r.seq.getRecordMode() == Mode::Quant) == quant);

                clock.runTo(2.3);  r.keyOn(60); r.keyOff(60);       // a little late on step 3
                clock.runTo(5.7);  r.keyOn(64); r.keyOff(64);       // a little early on step 7
                clock.runTo(9.1);  r.keyOn(48); r.keyOn(55);        // two keys together
                r.keyOff(48); r.keyOff(55);
                clock.runTo(15.8); r.keyOn(67); r.keyOff(67);       // early on step 1: wraps round

                int at = -1;
                const Step* a = findNote(r.seq, 0, 60, &at);
                expect(a != nullptr);
                expectEquals(at, 2);
                if (a != nullptr)
                {
                    expectWithinAbsoluteError(a->microTiming, quant ? 0.0f : 0.3f, 0.01f);
                    expectWithinAbsoluteError(a->length, 2.0f, 1.0e-6f, "the step's length is kept");
                }
                const Step* b = findNote(r.seq, 0, 64, &at);
                expect(b != nullptr);
                expectEquals(at, 6, "nearest step, not the step the playhead is on");
                if (b != nullptr)
                    expectWithinAbsoluteError(b->microTiming, quant ? 0.0f : -0.3f, 0.01f);
                const Step* c = findNote(r.seq, 0, 67, &at);
                expect(c != nullptr);
                expectEquals(at, 0, "the end of the loop rounds to step 1");
                if (c != nullptr)
                    expectWithinAbsoluteError(c->microTiming, quant ? 0.0f : -0.2f, 0.01f);
                const Step* chord = findNote(r.seq, 0, 48, &at);
                expect(chord != nullptr && chord == findNote(r.seq, 0, 55));
                expectEquals(at, 9);
                if (chord != nullptr)
                    expectEquals((int) chord->notes.size(), 2);

                // What was played by hand is not played a second time by the sequencer in the same
                // pass (the early note on step 7 would otherwise sound twice).
                expectEquals((int) clock.trigs.size(), 0);

                // Play: stop, recording over. Play again: it plays back, FREE as played, QUANT on the grid.
                r.tap(PLAY);
                expect(!r.seq.isPlaying() && !r.seq.isRecording());
                clock.restart();
                r.tap(PLAY);
                expect(r.seq.isPlaying() && !r.seq.isRecording());
                clock.runTo(16.5);
                const double tol = 20.0 / STEP;
                expectWithinAbsoluteError(clock.firstAt(67), 0.0, tol, "step 1 sounds as the transport starts");
                expectWithinAbsoluteError(clock.firstAt(60), quant ? 2.0 : 2.3, tol);
                expectWithinAbsoluteError(clock.firstAt(64), quant ? 6.0 : 5.7, tol);
                expectWithinAbsoluteError(clock.firstAt(48), quant ? 9.0 : 9.1, tol);
                expectWithinAbsoluteError(clock.firstAt(55), quant ? 9.0 : 9.1, tol);
                int count67 = 0;
                for (auto& t : clock.trigs)
                    if (t.pitch == 67) ++count67;
                expectEquals(count67, 2, "second time round, the early note is early (free) or on the bar line (quant)");
                if (count67 == 2)
                    expectWithinAbsoluteError((double) clock.trigs.back().at / STEP, quant ? 16.0 : 15.8, tol);
                r.tap(PLAY);
            }
        }

        beginTest("Overdub: a second pass adds notes and leaves the others; QN pulls free notes to the grid on playback only");
        {
            Rig r;
            Clock clock(r.seq);
            RecordingKeys keys(r.seq);
            r.mgr.setKeyboardListener(&keys);
            recordPlay(r);
            clock.runTo(2.3);  r.keyOn(60); r.keyOff(60);
            clock.runTo(16.0 + 4.2); r.keyOn(62); r.keyOff(62);
            expect(findNote(r.seq, 0, 60) != nullptr && findNote(r.seq, 0, 62) != nullptr);
            int played60 = 0;
            for (auto& t : clock.trigs)
                if (t.pitch == 60) ++played60;
            expectEquals(played60, 1, "the first pass's note plays in the second pass");
            r.tap(PLAY);

            r.seq.setQuantizeAmount(1.0f);
            clock.restart();
            r.tap(PLAY);
            clock.runTo(8.0);
            expectWithinAbsoluteError(clock.firstAt(60), 2.0, 20.0 / STEP);
            expectWithinAbsoluteError(findNote(r.seq, 0, 60)->microTiming, 0.3f, 0.01f, "the recording is unchanged");
        }

        beginTest("While recording a light runs across the pads with the clock; the pads keep their job");
        {
            Rig r;
            Clock clock(r.seq);
            r.a->ownColours = true;             // a screen with its own pad colours (as DRUMS, SCALE)
            std::array<Screen::PadColour, 16> c;
            PadManager pads;

            r.tap(PLAY);
            clock.runTo(2.5);
            expect(r.mgr.getPadFrame(c) == ScreenManager::PadSource::Screen);
            for (auto& colour : c)
                expect(is(colour, 1, 2, 3), "playing without recording: no light");
            expectEquals(r.mgr.getRecordLightPad(), -1);
            r.tap(PLAY);

            clock.restart();
            recordPlay(r);
            int sendsBefore = 0;
            for (int step = 0; step < 18; ++step)
            {
                clock.runTo(step + 0.5);
                expect(r.mgr.getPadFrame(c) == ScreenManager::PadSource::Screen);
                for (int pad = 0; pad < 16; ++pad)
                {
                    if (pad == step % 16) expect(is(c[(size_t) pad], 127, 0, 0), "the light is on pad " + juce::String(step % 16 + 1));
                    else                  expect(is(c[(size_t) pad], 1, 2, 3));
                }
                expectEquals(r.mgr.getRecordLightPad(), step % 16);

                // As the 30 Hz refresh sends it: only the two pads that changed.
                std::array<PadManager::RGB, 16> rgb;
                for (size_t i = 0; i < 16; ++i) rgb[i] = { c[i].r, c[i].g, c[i].b };
                pads.showRGB(rgb);
                pads.showRGB(rgb);
                if (step > 0)
                    expectEquals(pads.getSendCount() - sendsBefore, 2);
                sendsBefore = pads.getSendCount();
            }

            // A pad still does what the screen says, and a red pad shows the light in white.
            r.padOn(5);
            expect(r.a->saw("pad5"));
            r.padOff(5);
            r.a->colour = { 127, 0, 0 };
            expect(r.mgr.getPadFrame(c) == ScreenManager::PadSource::Screen);
            expect(is(c[(size_t) r.mgr.getRecordLightPad()], 127, 127, 127));

            // A hold layer is not drawn over; a screen without colours of its own leaves the pads
            // to the pad manager, which asks for the light's pad.
            r.shift(true);
            expect(r.mgr.getPadFrame(c) == ScreenManager::PadSource::Layer);
            for (auto& colour : c)
                expect(!is(colour, 127, 0, 0));
            r.shift(false);
            r.a->ownColours = false;
            expect(r.mgr.getPadFrame(c) == ScreenManager::PadSource::PadMode);
            expect(r.mgr.getRecordLightPad() >= 0);

            // The light follows the selected track's own length and speed.
            r.seq.getPattern().getTrack(1).setLength(4);
            r.seq.setSelectedTrack(1);
            clock.runTo(30.5);
            expectEquals(r.mgr.getRecordLightPad(), r.seq.getCurrentStep(1));
            expect(r.mgr.getRecordLightPad() < 4);

            r.tap(PLAY);
            expectEquals(r.mgr.getRecordLightPad(), -1);
        }

        beginTest("Leaving recording lays the last recorded track out on STEP, on the page of its first recorded step");
        {
            Rig r;
            Clock clock(r.seq);
            RecordingKeys keys(r.seq);
            r.mgr.setKeyboardListener(&keys);
            auto* step = new StepRec();
            r.mgr.addScreen(std::unique_ptr<Screen>(step));         // index 2
            r.seq.getPattern().getTrack(2).setLength(64);

            // Nothing recorded: stay where you are.
            recordPlay(r);
            clock.runTo(3.0);
            r.tap(PLAY);
            expectEquals(r.mgr.getActiveIndex(), 0);
            expectEquals(step->shown, -1);

            // Track 1 first, then track 3 (steps 41 and 21): track 3 is what is shown, from step 21.
            clock.restart();
            recordPlay(r);
            clock.runTo(1.1);  r.keyOn(50); r.keyOff(50);
            r.seq.setSelectedTrack(2);
            clock.runTo(40.2); r.keyOn(60); r.keyOff(60);
            clock.runTo(64.0 + 20.1); r.keyOn(62); r.keyOff(62);
            r.seq.setSelectedTrack(0);          // looking at another track when recording ends
            int trackShown = -1;
            r.mgr.setTrackSelectionCallback([&](int t) { trackShown = t; });
            r.tap(PLAY);
            expect(!r.seq.isPlaying() && !r.seq.isRecording());
            expectEquals(r.mgr.getActiveIndex(), 2, "STEP");
            expectEquals(r.seq.getSelectedTrack(), 2);
            expectEquals(trackShown, 2, "the app hears about the track change");
            expectEquals(step->shown, 20);
            expectEquals(r.text(), juce::String("REC OFF"));

            // A new take forgets the old one. Ended with Record alone: the transport keeps running.
            r.mgr.setActiveScreen(0);
            step->shown = -1;
            clock.restart();
            recordPlay(r);
            clock.runTo(2.0);
            r.tap(RECORD);
            expect(r.seq.isPlaying() && !r.seq.isRecording());
            expectEquals(r.mgr.getActiveIndex(), 0, "nothing was recorded in this take");
            recordPlay(r);                       // already playing: recording on
            clock.runTo(5.1); r.keyOn(65); r.keyOff(65);
            r.tap(RECORD);
            expect(r.seq.isPlaying() && !r.seq.isRecording());
            expectEquals(r.mgr.getActiveIndex(), 2);
            expectEquals(step->shown, 5);
            r.tap(PLAY);
        }

        beginTest("The real STEP screen: recorded notes show on the pads as they are recorded, then the right page");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            ScreenManager mgr;
            mgr.setSequencer(&seq);
            PadManager pads;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);
            Clock clock(seq);
            RecordingKeys keys(seq);
            mgr.setKeyboardListener(&keys);
            mgr.addScreen(std::make_unique<cgtest::Rec>("A"));
            auto* stepView = new StepViewScreen(seq, pads, leds);
            mgr.addScreen(std::unique_ptr<Screen>(stepView));
            mgr.setStepScreen(1);
            mgr.setActiveScreen(1);
            seq.getPattern().getTrack(0).setLength(32);
            auto press = [&](int cc) { mgr.controlChangeReceived(1, cc, 127); };
            auto release = [&](int cc) { mgr.controlChangeReceived(1, cc, 0); };
            const int kb = LaunchkeyController::KEYS_PORT_CHANNEL;

            press(RECORD); press(PLAY); release(PLAY); release(RECORD);
            clock.runTo(2.2);
            expectEquals(leds.getStepColor(2, 0), (int) PadColors::STEP_PLAYHEAD);
            mgr.noteOnReceived(kb, 60, 100);
            mgr.noteOffReceived(kb, 60);
            expectEquals(leds.getStepColor(2, 0), (int) PadColors::STEP_PLAYHEAD_ACTIVE, "at once, under the playhead");
            clock.runTo(4.5);
            expectEquals(leds.getStepColor(2, 0), (int) PadColors::STEP_ACTIVE);
            expectEquals(leds.getStepColor(4, 0), (int) PadColors::STEP_PLAYHEAD);
            press(PLAY); release(PLAY);
            expectEquals(stepView->getCurrentPage(), 0);

            // A take on the second page only, ended from another screen.
            seq.getPattern().getTrack(0).clearAllSteps();
            mgr.setActiveScreen(0);
            clock.restart();
            press(RECORD); press(PLAY); release(PLAY); release(RECORD);
            clock.runTo(21.1);
            mgr.noteOnReceived(kb, 62, 100);
            mgr.noteOffReceived(kb, 62);
            press(PLAY); release(PLAY);
            expectEquals(mgr.getActiveIndex(), 1);
            expectEquals(stepView->getCurrentPage(), 1);
            expectEquals(leds.getStepColor(5, stepView->getCurrentPage() * 16), (int) PadColors::STEP_ACTIVE,
                         "step 22 is the sixth pad of the page");

            // Hold that step and turn: its detail opens, as for any step (parameter locks from here).
            mgr.noteOnReceived(1, LaunchkeyController::PAD_TOP_ROW_START + 5, 100);
            expectEquals(stepView->getHeldStepIndex(), 21);
            expect(seq.getPattern().getTrack(0).getStep(21).trig, "holding does not remove the recorded step");
            mgr.noteOffReceived(1, LaunchkeyController::PAD_TOP_ROW_START + 5);
        }

        beginTest("Drum pads record on DRUMS in both modes and play back; two pads together share the step");
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                const bool quant = (pass == 1);
                SequencerCore seq;
                seq.prepareToPlay(44100.0, 512);
                AudioEngine audio;
                audio.setSequencer(&seq);
                auto drums = std::make_shared<DrumEngine>();
                drums->prepareToPlay(44100.0, 256);
                audio.addEngine(1, drums);
                seq.getPattern().getTrack(1).engineType = EngineType::Drum;
                // As in main.cpp: pads go through the pad ratchet; every hit it makes is recorded.
                audio.onPadHit = [&](int track, int note, float velocity)
                {
                    if (seq.isRecording() && seq.isPlaying())
                        seq.recordNote(track, note, velocity);
                };

                ScreenManager mgr;
                mgr.setSequencer(&seq);
                auto* drumScreen = new DrumPadScreen(drums.get());
                DrumPadScreen::PadHooks hooks;
                hooks.down = [&](int pad, float velocity) { audio.padNoteOn(1, pad, pad, velocity); };
                hooks.up = [&](int pad) { audio.padNoteOff(pad); };
                drumScreen->setPadHooks(std::move(hooks));
                mgr.addScreen(std::unique_ptr<Screen>(drumScreen));         // 0 DRUMS
                auto* step = new StepRec();
                mgr.addScreen(std::unique_ptr<Screen>(step));               // 1 STEP
                Clock clock(seq);
                int drumHits = 0;
                seq.setTrigCallback([&](int track, const Step& s)
                {
                    for (auto& n : s.notes)
                    {
                        clock.trigs.push_back({ clock.now, track, n.pitch });
                        if (auto* e = audio.getEngine(track)) { e->noteOn(n.pitch, n.velocity, -1); ++drumHits; }
                    }
                });

                auto press = [&](int cc) { mgr.controlChangeReceived(1, cc, 127); };
                auto release = [&](int cc) { mgr.controlChangeReceived(1, cc, 0); };
                auto hit = [&](int pad)
                {
                    const int note = pad < 8 ? LaunchkeyController::PAD_TOP_ROW_START + pad
                                             : LaunchkeyController::PAD_BOTTOM_ROW_START + pad - 8;
                    mgr.noteOnReceived(1, note, 110);
                    audio.processGenerators(64);
                    mgr.noteOffReceived(1, note);
                    audio.processGenerators(64);
                };

                press(RECORD); press(PLAY); release(PLAY); release(RECORD);
                if (quant) { press(RECORD); press(PLAY); release(PLAY); release(RECORD); }
                expect(seq.isRecording() && seq.isPlaying());
                expectEquals(seq.getSelectedTrack(), 0, "the drum track is not the selected track");

                clock.runTo(4.2);
                hit(8);                                     // lower-left pad
                clock.runTo(7.8);
                hit(3); hit(10);                            // two pads on step 9

                int at = -1;
                const Step* kick = findNote(seq, 1, 8, &at);
                expect(kick != nullptr, "the pad hit is on the drum track");
                expectEquals(at, 4);
                if (kick != nullptr)
                    expectWithinAbsoluteError(kick->microTiming, quant ? 0.0f : 0.2f, 0.02f);
                const Step* pair = findNote(seq, 1, 3, &at);
                expect(pair != nullptr && pair == findNote(seq, 1, 10));
                expectEquals(at, 8);
                if (pair != nullptr)
                    expectWithinAbsoluteError(pair->microTiming, quant ? 0.0f : -0.2f, 0.02f);
                expect(findNote(seq, 0, 8) == nullptr, "nothing on the selected track");

                // The pads kept playing while the light ran over them.
                std::array<Screen::PadColour, 16> c;
                expect(mgr.getPadFrame(c) == ScreenManager::PadSource::Screen);
                expect(mgr.getRecordLightPad() >= 0);

                press(PLAY); release(PLAY);
                expectEquals(mgr.getActiveIndex(), 1, "STEP");
                expectEquals(seq.getSelectedTrack(), 1, "with the drum track selected");
                expectEquals(step->shown, 4);

                clock.restart();
                drumHits = 0;
                press(PLAY); release(PLAY);
                clock.runTo(12.0);
                const double tol = 20.0 / STEP;
                expectWithinAbsoluteError(clock.firstAt(8), quant ? 4.0 : 4.2, tol);
                expectWithinAbsoluteError(clock.firstAt(3), quant ? 8.0 : 7.8, tol);
                expectWithinAbsoluteError(clock.firstAt(10), quant ? 8.0 : 7.8, tol);
                expectEquals(drumHits, 3);
                expect(drums->getVoice(3).isActive() || drums->getVoice(10).isActive() || drums->getVoice(8).isActive(),
                       "the drum voices are sounding");
                press(PLAY); release(PLAY);
            }
        }

        beginTest("A pinned pad screen stays pinned when recording ends (STEP is shown; the pads are still the pinned ones)");
        {
            Rig r("DRUMS", "STEP");
            Clock clock(r.seq);
            RecordingKeys keys(r.seq);
            r.mgr.setKeyboardListener(&keys);
            r.mgr.setPinnable(0, true);
            r.mgr.setShiftPadScreen(9, 0);
            r.shift(true); r.padHeld(9); r.shift(false);
            expectEquals(r.mgr.getPinnedScreen(), 0);
            recordPlay(r);
            clock.runTo(1.1); r.keyOn(60); r.keyOff(60);
            r.tap(PLAY);
            expectEquals(r.mgr.getActiveIndex(), 1);
            expectEquals(r.mgr.getPinnedScreen(), 0);
        }

        beginTest("Scale button: a message on every screen, a SCALE tag while it is on, keys snapped to the scale");
        {
            Rig r("A", "AT ROUTE");
            cgtest::Keys keys;
            r.mgr.setKeyboardListener(&keys);
            CustomScaleManager scales;
            int major = -1;
            for (int p = 0; p < scales.getPageCount(); ++p)
                if (juce::String(scales.getPageName(p)) == "Major") major = p;
            expect(major >= 0);
            scales.setActivePage(major);
            const int root = scales.getRootNote();
            r.mgr.setScaleNameProvider([&] { return GrammarHooks::scaleLabel(scales); });
            r.mgr.setKeySnap([&](int note) { return GrammarHooks::snapToScale(scales, note); });
            int toggles = 0;
            r.mgr.setScaleToggleCallback([&](bool) { ++toggles; });
            const juce::String label = GrammarHooks::scaleLabel(scales);
            expect(label.endsWith(" MAJOR"));

            // Unlocked: keys play what they are.
            r.keyOn(root + 1); r.keyOff(root + 1);
            expectEquals(keys.log.joinIntoString(","), "on" + juce::String(root + 1) + ",off" + juce::String(root + 1));
            keys.log.clear();

            r.mgr.controlChangeReceived(1, SCALE, 127);
            r.mgr.controlChangeReceived(1, SCALE, 0);
            expect(r.mgr.isScaleLocked());
            expectEquals(toggles, 1);
            expectEquals(r.text(), "SCALE ON " + label, "the longest wording that fits beside the screen name");
            r.now += 1900;
            expectEquals(r.text(), "SCALE ON " + label, "still there after 1.9 s");
            r.now += 200;
            expect(!r.text().startsWith("SCALE"), "gone after 2.1 s");
            expect(r.mgr.getNameBarTags().contains("SCALE"));
            expectEquals(r.mgr.composeNameBar().tags, juce::String("SCALE"), "and the tag stays");

            // A semitone above the root is not in a major scale: the key plays the root, and its
            // release lets go of what it played. In-scale keys are untouched.
            r.keyOn(root + 1); r.keyOff(root + 1);
            r.keyOn(root + 4); r.keyOff(root + 4);
            r.keyOn(root + 6); r.keyOff(root + 6);
            expectEquals(keys.log.joinIntoString(","),
                         "on" + juce::String(root) + ",off" + juce::String(root)
                         + ",on" + juce::String(root + 4) + ",off" + juce::String(root + 4)
                         + ",on" + juce::String(root + 5) + ",off" + juce::String(root + 5));
            keys.log.clear();

            // A narrower screen name: a shorter wording, still whole.
            r.mgr.setActiveScreen(1);
            r.keyOn(root + 1);                  // held across the unlock
            r.mgr.controlChangeReceived(1, SCALE, 127);
            r.mgr.controlChangeReceived(1, SCALE, 0);
            expect(!r.mgr.isScaleLocked());
            expectEquals(r.text(), juce::String("SCALE OFF"));
            expect(!r.mgr.getNameBarTags().contains("SCALE"));
            r.keyOff(root + 1);
            expectEquals(keys.log.joinIntoString(","), "on" + juce::String(root) + ",off" + juce::String(root));
            r.mgr.controlChangeReceived(1, SCALE, 127);
            r.mgr.controlChangeReceived(1, SCALE, 0);
            expectEquals(r.text(), juce::String("SCALE ON"));
            r.mgr.setActiveScreen(0);
            r.mgr.controlChangeReceived(1, SCALE, 127);
            expectEquals(r.text(), juce::String("SCALE LOCK OFF"));

            // The SCALE screen's LK cell (setScaleLocked) and the button are one state.
            r.mgr.setScaleLocked(true);
            expect(r.mgr.isScaleLocked());
            expect(r.mgr.getNameBarTags().contains("SCALE"));
            r.mgr.controlChangeReceived(1, SCALE, 127);
            expect(!r.mgr.isScaleLocked());
            expectEquals(toggles, 6);

            // Shift + Scale still opens the SCALE screen and does not touch the lock.
            Rig s("A", "SCALE");
            s.shift(true);
            s.mgr.controlChangeReceived(1, SCALE, 127);
            s.shift(false);
            expectEquals(s.mgr.getActiveIndex(), 1);
            expect(!s.mgr.isScaleLocked());

            // A chromatic page leaves every key alone.
            CustomScaleManager chromatic;
            for (int p = 0; p < chromatic.getPageCount(); ++p)
                if (juce::String(chromatic.getPageName(p)) == "Chrom") chromatic.setActivePage(p);
            for (int note = 40; note < 60; ++note)
                expectEquals(GrammarHooks::snapToScale(chromatic, note), note);
        }
    }
};

static LiveRecordTests LiveRecordTestsInstance;
