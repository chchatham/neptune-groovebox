#pragma once

#include "../src/ui/ScreenManager.h"
#include "../src/ui/UiClock.h"
#include "../src/ui/ParamGrid.h"
#include "../src/ui/PadLEDFeedback.h"
#include "../src/ui/screens/StepViewScreen.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include "../src/ui/screens/TrackFxScreen.h"
#include "../src/ui/screens/FilterViewScreen.h"
#include "../src/controller/DisplayRenderer.h"
#include "../src/controller/LaunchkeyController.h"
#include "../src/controller/PadManager.h"
#include <juce_core/juce_core.h>

// A held step shows its pages by itself: on STEP the track's lock pages open once the press has
// lasted longer than a tap (no knob turned), on the engine's MAIN page, each cell with the value
// that step will play. Driven as the app is: pad notes, knob and bank-button messages through
// ScreenManager, time through UiClock, the picture through the 30 Hz refresh's redraw.
namespace heldsteptest
{
    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        PadManager pads;
        PadLEDFeedback leds;
        LaunchkeyController controller;
        DisplayRenderer display;
        ScreenManager mgr;
        StepViewScreen* steps = nullptr;
        ParamPageScreen* param = nullptr;
        TrackFxScreen* fx = nullptr;
        FilterViewScreen* fltr = nullptr;
        enum { STEP = 0, PARAM, TRKFX, FLTR };

        explicit Rig(EngineType type = EngineType::UnifiedVoice, EngineType second = EngineType::FM)
        {
            seq.prepareToPlay(44100.0, 512);
            audio.addEngine(0, AudioEngine::createEngine(type));
            seq.getPattern().getTrack(0).engineType = type;
            audio.addEngine(1, AudioEngine::createEngine(second));
            seq.getPattern().getTrack(1).engineType = second;
            display.setController(&controller);
            mgr.setSequencer(&seq);
            mgr.setDisplay(&display);

            steps = new StepViewScreen(seq, pads, leds);
            param = new ParamPageScreen(seq, &audio);
            steps->setLockPages(param);
            fx = new TrackFxScreen(seq, &audio);
            fltr = new FilterViewScreen(seq, &audio);
            mgr.addScreen(std::unique_ptr<Screen>(steps));
            mgr.addScreen(std::unique_ptr<Screen>(param));
            mgr.addScreen(std::unique_ptr<Screen>(fx));
            mgr.addScreen(std::unique_ptr<Screen>(fltr));
            mgr.setActiveScreen(STEP);
        }

        static int noteOf(int pad) { return pad < 8 ? LaunchkeyController::PAD_TOP_ROW_START + pad
                                                    : LaunchkeyController::PAD_BOTTOM_ROW_START + (pad - 8); }
        void padDown(int pad) { mgr.noteOnReceived(1, noteOf(pad), 100); }
        void padUp(int pad)   { mgr.noteOffReceived(1, noteOf(pad)); }
        void turn(int encoder, int clicks = 1) { mgr.controlChangeReceived(16, 0x15 + encoder, clicks < 0 ? 128 + clicks : clicks); }
        void button(int cc) { mgr.controlChangeReceived(1, cc, 127); mgr.controlChangeReceived(1, cc, 0); }
        void bankNext() { button(ScreenManager::CC_BANK_DOWN); }
        void bankPrev() { button(ScreenManager::CC_BANK_UP); }
        // The app's 30 Hz refresh, as far as the display goes.
        void tick() { const juce::ScopedLock ui(mgr.getLock()); mgr.requestRedraw(); mgr.redrawIfNeeded(); }
        // The press becomes a hold: time passes and the refresh comes round, nothing else.
        void wait(juce::int64 ms = StepViewScreen::TAP_MS + 1) { UiClock::advance(ms); tick(); }

        Track& track(int t = 0) { return seq.getPattern().getTrack(t); }
        Step& step(int i, int t = 0) { return track(t).getStep(i); }
        juce::String names() const
        {
            juce::StringArray n;
            for (int e = 0; e < 8; ++e) n.add(mgr.getActiveScreen()->getEncoderName(e));
            return n.joinIntoString(" ");
        }
        bool mark(int cell) const
        {
            return display.getPixels()[(size_t) ParamGrid::cellY(cell)][(size_t) (ParamGrid::cellX(cell) + 28)] != 0;
        }
    };
}

class HeldStepPagesTests : public juce::UnitTest
{
public:
    HeldStepPagesTests() : UnitTest("Held Step Pages") {}

    void runTest() override
    {
        using heldsteptest::Rig;
        using Layout = Screen::EncoderLayout;

        beginTest("STEP: a tap adds or removes the trig and never shows the pages");
        {
            Rig r;
            r.padDown(4);
            r.tick();
            expect(!r.steps->isDetailActive());
            expectEquals(r.mgr.composeNameBar().left, juce::String("STEP T1"));
            expect(r.steps->getEncoderLayout() == Layout::Bar);
            UiClock::advance(StepViewScreen::TAP_MS - 100);         // still a tap
            r.tick();
            expect(!r.steps->isDetailActive(), "nothing opens within the tap time");
            r.padUp(4);
            expect(r.step(4).trig, "the tap added a trig");

            r.padDown(4);
            r.tick();
            UiClock::advance(100);
            r.tick();
            expect(!r.steps->isDetailActive());
            r.padUp(4);
            expect(!r.step(4).trig, "a tap on a trig removes it");
            expectEquals(r.mgr.composeNameBar().left, juce::String("STEP T1"));
        }

        beginTest("STEP: a hold shows the engine's MAIN page for that step with no knob turned; release returns to STEP");
        {
            Rig r;
            r.step(4).trig = true;
            r.step(4).notes.push_back({ 64, 0.8f });
            r.padDown(4);
            r.tick();
            expect(!r.steps->isDetailActive());
            r.wait();
            expect(r.steps->isDetailActive(), "the hold alone opens the pages");
            auto bar = r.mgr.composeNameBar();
            expectEquals(bar.left, juce::String("MAIN T1"));
            expectEquals(bar.text, juce::String("S5 LOCK"));
            expectEquals(bar.pages, juce::String("2/") + juce::String(r.param->getPageCount() + 1), "TRIG is page 1");
            expectEquals(r.names(), juce::String("CUT RESO ENV TYPE ATK DEC SUS REL"));
            expect(r.steps->getEncoderLayout() == Layout::Grid);
            expect(!r.steps->usesEncoderBar());
            expect(r.step(4).paramLocks.empty(), "looking locks nothing");
            // Each cell shows the track's own value where the step has no lock, and no mark.
            expectEquals(r.param->getCellReadout(1),
                         juce::String("RESO ") + r.param->getCurrentPage().params[1].format(r.audio.getTrackParameter(0, UnifiedParam::RESONANCE)));
            for (int c = 0; c < 8; ++c)
                expect(!r.mark(c), "no lock mark on cell " + juce::String(c + 1));

            r.padUp(4);
            r.tick();
            expect(!r.steps->isDetailActive());
            expectEquals(r.mgr.composeNameBar().left, juce::String("STEP T1"));
            expect(r.steps->getEncoderLayout() == Layout::Bar);
            expect(r.step(4).trig, "a held step is never removed");
            expectEquals(r.step(4).notes[0].pitch, 64);
        }

        beginTest("STEP: the page that opens, per engine");
        {
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::Sampler,
                               EngineType::MacroOsc, EngineType::Wavetable, EngineType::WaveSeq })
            {
                Rig r(type);
                r.padDown(0);
                r.wait();
                const juce::String which = "engine " + juce::String((int) type);
                expectEquals(r.mgr.composeNameBar().left, juce::String("MAIN T1"), which);
                juce::StringArray n = juce::StringArray::fromTokens(r.names(), " ", {});
                expectEquals(n[0], juce::String("CUT"), which);
                expectEquals(n[1], juce::String("RESO"), which);
                expectEquals(n.joinIntoString(" ", 4, 4), juce::String("ATK DEC SUS REL"), which);
                r.padUp(0);
            }

            // Drums: STEP shows the pad selected on DRUMS; a held step gets that drum beside the
            // ones it has, and the page and the locks are that drum's (drum_lane_tests.h).
            {
                Rig r(EngineType::Drum);
                auto* drums = dynamic_cast<DrumEngine*>(r.audio.getEngine(0));
                expect(drums != nullptr);
                drums->setSelectedVoice(1);
                r.step(2).trig = true;
                r.step(2).notes.push_back({ 3, 1.0f });        // pad 4
                r.padDown(2);
                r.wait();
                expectEquals(r.mgr.composeNameBar().left, juce::String("DR 2 T1"));
                expectEquals(r.names(), juce::String("TUNE DEC TONE DRIV SNAP LEVL PAN TYPE"));
                expectEquals((int) r.step(2).notes.size(), 2, "pad 4 still hits there");
                r.turn(2, 5);
                expect(r.step(2).paramLocks.count(1 * DrumEngine::PARAMS_PER_VOICE + DrumEngine::TONE) > 0, "TONE of pad 2 locked");
                r.padUp(2);
                expectEquals(drums->getSelectedVoice(), 1, "the pad selected on DRUMS is untouched");

                // An empty step gets the selected pad and shows its page.
                r.padDown(6);
                r.wait();
                expectEquals(r.mgr.composeNameBar().left, juce::String("DR 2 T1"));
                r.padUp(6);
                expectEquals(r.step(6).notes[0].pitch, 1);
            }

            // MIDI track: no MAIN, its first page.
            {
                Rig r(EngineType::MIDITrack);
                r.padDown(0);
                r.wait();
                expect(r.steps->isDetailActive());
                expectEquals(r.steps->getDetailPage(), 1);
                expectEquals(r.mgr.composeNameBar().left, r.audio.getEngine(0)->getParamPages()[0].name + " T1");
                expect(r.steps->getEncoderName(0).isNotEmpty() && r.steps->getEncoderName(0) != "---");
                r.padUp(0);
            }
        }

        beginTest("STEP: a locked cell shows the step's value and the lock mark");
        {
            Rig r;
            const float base = r.audio.getTrackParameter(0, UnifiedParam::RESONANCE);
            r.track().soundParams[UnifiedParam::RESONANCE] = base;
            r.step(3).trig = true;
            r.step(3).notes.push_back({ 60, 1.0f });
            r.step(3).paramLocks[UnifiedParam::RESONANCE] = 0.9f;
            r.padDown(3);
            r.wait();
            const auto reso = r.param->getCurrentPage().params[1];
            expectEquals(r.param->getCellReadout(1), juce::String("RESO ") + reso.format(0.9f));
            expect(reso.format(0.9f) != reso.format(base));
            expect(r.mark(1), "RESO carries the lock mark");
            expect(!r.mark(0), "CUT does not");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, UnifiedParam::RESONANCE), base, 1.0e-4f);
            r.padUp(3);

            // Another step of the same track: the track's value, no mark.
            r.padDown(5);
            r.wait();
            expectEquals(r.param->getCellReadout(1), juce::String("RESO ") + reso.format(base));
            expect(!r.mark(1));
            r.padUp(5);
        }

        beginTest("STEP: a turn or a bank button before the tap time opens the pages at once");
        {
            Rig r;
            r.padDown(4);
            r.turn(1, 10);
            expect(r.steps->isDetailActive());
            expectEquals(r.mgr.composeNameBar().left, juce::String("MAIN T1"));
            expect(r.step(4).paramLocks.count(UnifiedParam::RESONANCE) > 0, "the turn is a lock");
            r.padUp(4);
            expect(!r.steps->isDetailActive());
            expect(r.step(4).trig);

            r.padDown(6);
            r.bankNext();
            expect(r.steps->isDetailActive());
            expectEquals(r.steps->getPageIndex(), 2);
            r.padUp(6);
        }

        beginTest("STEP: the bank buttons page TRIG and the sound pages while the step is held");
        {
            Rig r;
            const int n = r.param->getPageCount() + 1;
            r.padDown(4);
            r.wait();
            expectEquals(r.steps->getPageIndex(), 1);
            r.bankNext();
            expectEquals(r.steps->getPageIndex(), 2);
            expectEquals(r.mgr.composeNameBar().text, juce::String("S5 LOCK"));
            r.bankPrev();
            r.bankPrev();
            expectEquals(r.steps->getPageIndex(), 0);
            auto bar = r.mgr.composeNameBar();
            expectEquals(bar.left, juce::String("TRIG T1"));
            expectEquals(bar.text, juce::String("S5"));
            expectEquals(r.names().upToFirstOccurrenceOf(" COND", true, false), juce::String("NOTE VEL LEN MICR COND"));
            r.bankPrev();
            expectEquals(r.steps->getPageIndex(), n - 1, "the pages go round");
            r.bankNext();
            r.bankNext();
            expectEquals(r.steps->getPageIndex(), 1);

            // The page last shown is where the next hold opens; another track or a new visit
            // to STEP starts on MAIN again.
            r.bankNext();
            r.padUp(4);
            r.padDown(5);
            r.wait();
            expectEquals(r.steps->getPageIndex(), 2, "the next hold opens on the page last shown");
            r.padUp(5);

            r.seq.setSelectedTrack(1);
            r.padDown(5);
            r.wait();
            expectEquals(r.steps->getPageIndex(), 1);
            expectEquals(r.mgr.composeNameBar().left, juce::String("MAIN T2"));
            r.bankNext();
            r.padUp(5);

            r.mgr.setActiveScreen(Rig::PARAM);
            r.mgr.setActiveScreen(Rig::STEP);
            r.padDown(5);
            r.wait();
            expectEquals(r.steps->getPageIndex(), 1, "a new visit starts on MAIN");
            r.padUp(5);
        }

        beginTest("STEP: several steps held - the pages show the one on the lowest pad, and stay until the last is let go");
        {
            Rig r;
            r.padDown(5);
            r.tick();
            UiClock::advance(200);
            r.padDown(2);
            r.tick();
            expect(!r.steps->isDetailActive());
            r.wait(200);                                     // the first press is now a hold
            expect(r.steps->isDetailActive());
            expectEquals(r.mgr.composeNameBar().text, juce::String("S3 LOCK"));
            r.turn(1, 10);
            expect(r.step(2).paramLocks.count(UnifiedParam::RESONANCE) > 0);
            expect(r.step(5).paramLocks.count(UnifiedParam::RESONANCE) > 0, "every held step is edited (multi_step_edit_tests.h)");

            r.padUp(2);
            r.tick();
            expect(r.steps->isDetailActive(), "still open: a step is still held");
            expectEquals(r.mgr.composeNameBar().text, juce::String("S6 LOCK"));
            expect(r.step(2).trig && r.step(5).trig);
            r.padUp(5);
            r.tick();
            expect(!r.steps->isDetailActive());
            expectEquals(r.mgr.composeNameBar().left, juce::String("STEP T1"));
        }

        beginTest("PARAM, TRK FX, FLTR: a held step is named first in the name bar and the cells show its values");
        {
            Rig r;
            r.track().setLength(16);
            r.step(11).trig = true;
            r.step(11).notes.push_back({ 60, 1.0f });
            r.step(11).paramLocks[UnifiedParam::RESONANCE] = 0.9f;

            // PARAM, on a page whose name leaves no room for both words: the step is what stays.
            r.mgr.setActiveScreen(Rig::PARAM);
            expect(r.param->selectPageByName("FILTR"));
            const int reso = [&] { const auto page = r.param->getCurrentPage();
                                   for (int i = 0; i < 8; ++i) if (juce::String(page.params[(size_t) i].label) == "RESO") return i;
                                   return -1; }();
            expect(reso >= 0);
            r.tick();
            const juce::String before = r.param->getCellReadout(reso);
            expect(!r.mark(reso));
            r.padDown(11);
            r.tick();
            expectEquals(r.param->getSubPage(), juce::String("S12 FILTR"));
            expect(r.mgr.composeNameBar().text.startsWith("S12"), "the name bar names the step: " + r.mgr.composeNameBar().text);
            expectEquals(r.param->getCellReadout(reso), juce::String("RESO ") + r.param->getCurrentPage().params[(size_t) reso].format(0.9f));
            expect(r.param->getCellReadout(reso) != before);
            expect(r.mark(reso), "the locked cell is marked");
            r.padUp(11);
            r.tick();
            expectEquals(r.param->getCellReadout(reso), before);
            expect(!r.mgr.composeNameBar().text.startsWith("S12"));
            expect(r.step(11).trig, "holding on PARAM never toggles the trig");

            for (int screen : { (int) Rig::TRKFX, (int) Rig::FLTR })
            {
                r.mgr.setActiveScreen(screen);
                r.tick();
                expect(!r.mgr.composeNameBar().text.startsWith("S12"));
                r.padDown(11);
                r.tick();
                expect(r.mgr.composeNameBar().text.startsWith("S12"),
                       "screen " + juce::String(screen) + ": " + r.mgr.composeNameBar().text);
                r.padUp(11);
            }
        }
    }
};

static HeldStepPagesTests heldStepPagesTests;
