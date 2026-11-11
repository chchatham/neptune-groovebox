#pragma once

#include "help_card_tests.h"

// Help cards for the other hold keys: > (patterns), Up / Down (tracks) and Record. The same rule as
// Shift and Func (tests/help_card_tests.h, whose rig this uses): held alone past the delay the
// display becomes that key's card; anything else takes it away and still acts; the bank buttons
// page it; letting go gives the screen back. What differs per key is whether letting go still acts
// after the card: Up / Down do not page, Record still switches recording.
namespace helplayers
{
    using namespace helptest;

    inline int ccOf(Key key)
    {
        return key == Key::Pattern ? (int) PATTERN_KEY : (key == Key::Track ? (int) UP : (int) RECORD);
    }
    inline juce::String nameOf(Key key) { return ScreenManager::getHelpKeyName(key); }

    // Every line of a card as one text, for "does it say ...".
    inline juce::String allLines(const Card& c)
    {
        juce::String out;
        for (auto& line : c.lines) out << line.text << "\n";
        return out;
    }
}

class HelpCardLayersTests : public juce::UnitTest
{
public:
    HelpCardLayersTests() : UnitTest("Help Card Layers") {}

    void runTest() override
    {
        using namespace helplayers;
        using Mode = ScreenManager::HelpMode;
        static const Key layerKeys[] = { Key::Pattern, Key::Track, Key::Record };

        beginTest("One table: five keys with cards, their names, and only Record acts when let go after its card");
        {
            Rig r;
            expectEquals((int) Key::NumKeys, 5);
            expectEquals(r.mgr.getHelpCardCount(Key::Shift), 3);
            expectEquals(r.mgr.getHelpCardCount(Key::Func), 3);
            expectEquals(r.mgr.getHelpCardCount(Key::Pattern), 2);
            expectEquals(r.mgr.getHelpCardCount(Key::Track), 2);
            expectEquals(r.mgr.getHelpCardCount(Key::Record), 1);
            expectEquals(r.mgr.getHelpCardCount(Key::None), 0);
            expectEquals(nameOf(Key::Pattern), juce::String(">"));
            expectEquals(nameOf(Key::Track), juce::String("Up / Down"));
            expectEquals(nameOf(Key::Record), juce::String("Record"));
            for (int k = 0; k < (int) Key::NumKeys; ++k)
                expect(ScreenManager::helpKeyActsOnReleaseAfterCard((Key) k) == ((Key) k == Key::Record));
            expectEquals(r.card(Key::Pattern, 0).title, juce::String("> PATTERNS"));
            expectEquals(r.card(Key::Pattern, 1).title, juce::String("> BUTTONS, KNOBS"));
            expectEquals(r.card(Key::Track, 0).title, juce::String("UP DOWN: TRACKS"));
            expectEquals(r.card(Key::Track, 1).title, juce::String("UP DOWN: KNOBS"));
            expectEquals(r.card(Key::Record, 0).title, juce::String("RECORD"));
            expectEquals(r.card(Key::Pattern, 1).pages, juce::String("2/2"));
            expectEquals(r.card(Key::Record, 0).pages, juce::String(), "one card: no page mark");
        }

        beginTest("No card before the delay, the card after it, and the screen back exactly on release");
        for (const Key key : layerKeys)
            for (const int cc : { ccOf(key), key == Key::Track ? (int) DOWN : ccOf(key) })
            {
                Rig r;
                r.fill();
                const auto screen = r.picture();

                r.press(cc);
                r.tick();
                expect(r.mgr.getHelpCardKey() == Key::None, nameOf(key) + ": nothing at the press");
                expect(r.picture() == screen);
                r.wait(ScreenManager::HELP_DELAY_MS - 50);
                expect(r.mgr.getHelpCardKey() == Key::None, nameOf(key) + ": nothing before the delay");
                expect(r.picture() == screen);
                r.wait(60);
                expect(r.showing(key), nameOf(key) + ": the card after 350 ms, from the refresh alone");
                const auto cardPicture = r.picture();
                expect(cardPicture != screen, "the card replaces the picture");
                r.wait(5000);
                expect(r.showing(key), "and it stays while the key is held");
                expect(r.picture() == cardPicture);

                r.release(cc);
                expect(r.mgr.getHelpCardKey() == Key::None);
                expectEquals(r.mgr.getActiveIndex(), (int) TRACKS);
                expectEquals(r.home->getPageIndex(), 0);
                if (key == Key::Record)
                {
                    // Record let go does what Record does - also after its card.
                    expect(r.mgr.isRecordArmed(), "Record after its card: recording on, as always");
                    r.tap(RECORD);
                    expect(!r.mgr.isRecordArmed());
                    r.wait(3000);
                }
                expect(r.picture() == screen, nameOf(key) + ": letting go gives the screen back exactly");
            }

        beginTest("HELP OFF never shows a card, INST shows it at the press");
        for (const Key key : layerKeys)
        {
            Rig r;
            r.mgr.setHelpMode(Mode::Off);
            r.press(ccOf(key));
            r.wait(3000);
            expect(r.mgr.getHelpCardKey() == Key::None, nameOf(key) + ": OFF: never");
            r.release(ccOf(key));
            if (key == Key::Record) r.tap(RECORD);

            r.mgr.setHelpMode(Mode::Instant);
            r.press(ccOf(key));
            expect(r.showing(key), nameOf(key) + ": INST: at the press");
            r.release(ccOf(key));
            expect(r.mgr.getHelpCardKey() == Key::None);
        }

        beginTest("> : every continuation still acts and takes the card away for the rest of the hold");
        {
            Rig r;
            r.fill();
            // > + pad, stopped: that pattern at once.
            r.press(PATTERN_KEY);
            r.wait();
            expect(r.showing(Key::Pattern));
            r.pad(5);
            expectEquals(r.seq.getActivePatternIndex(), 5, "> + pad 6 goes to pattern 6");
            expect(r.mgr.getHelpCardKey() == Key::None, "the pad takes the card away");
            r.wait(5000);
            expect(r.mgr.getHelpCardKey() == Key::None, "gone for the rest of this hold");
            r.pad(0);
            expectEquals(r.seq.getActivePatternIndex(), 0);
            r.release(PATTERN_KEY);

            // > + Record keeps a copy, > + Play reloads it: neither arms recording nor starts the transport.
            r.press(PATTERN_KEY);
            r.wait();
            expect(r.showing(Key::Pattern), "the next hold shows it again");
            expectEquals(r.card(Key::Pattern, 1).lines[3].text, juce::String(" AS IT WAS LOADED"));
            r.track(0).getStep(9).trig = true;
            r.tap(RECORD);
            expect(r.mgr.getHelpCardKey() == Key::None);
            expectEquals(r.text(), juce::String("KEEP P1"));
            expect(!r.mgr.isRecordArmed());
            expectEquals(r.card(Key::Pattern, 1).lines[3].text, juce::String(" FROM THE KEPT COPY"));
            r.track(0).getStep(9).trig = false;
            r.tap(PLAY);
            expect(r.track(0).getStep(9).trig, "> + Play: the kept copy is back");
            expect(!r.seq.isPlaying());
            r.release(PATTERN_KEY);
            expect(!r.mgr.isRecordArmed() && !r.seq.isPlaying());

            // > + pad while playing: queued for the end of the loop, and the card says which.
            r.seq.start();
            r.press(PATTERN_KEY);
            r.wait();
            expectEquals(r.card(Key::Pattern, 0).lines[5].text, juce::String("PAD: NEXT AT LOOP END"));
            r.pad(11);
            expectEquals(r.seq.getPendingPattern(), 11);
            expectEquals(r.seq.getActivePatternIndex(), 0);
            r.release(PATTERN_KEY);
            r.press(PATTERN_KEY);
            r.wait();
            expect(r.showing(Key::Pattern));
            const auto queued = r.card(Key::Pattern, 0);
            expectEquals(queued.lines[4].text, juce::String("PATTERN 1  NEXT 12*"));
            expect(queued.padRows[1].labels[3].marked && queued.padRows[1].labels[3].text == "12");
            r.release(PATTERN_KEY);
            r.seq.stop();

            // > + turn with a step held: that knob's lock goes. (PARAM: the pads are the steps.)
            r.mgr.setActiveScreen(PARAM);
            r.padDown(0);
            r.wait(500);
            expectEquals((int) r.track(0).getStep(0).paramLocks.size(), 1);
            r.press(PATTERN_KEY);
            r.wait();
            expect(r.showing(Key::Pattern), "a held step does not stand in the way");
            expectEquals(r.card(Key::Pattern, 1).lines[4].text, juce::String("KNOB     REMOVE LOCK"));
            const float soundBefore = r.audio.getTrackParameter(0, r.moved.id);
            for (int page = 0; page < r.param->getPageCount(); ++page)
            {
                r.param->setCurrentPageIndex(page);
                for (int e = 0; e < 8; ++e)
                {
                    r.turn(e);
                    expect(r.mgr.getHelpCardKey() == Key::None, "the turn takes the card away");
                }
            }
            expectEquals((int) r.track(0).getStep(0).paramLocks.size(), 0, "> + turn removed the lock");
            expectEquals(r.audio.getTrackParameter(0, r.moved.id), soundBefore, "and changed no value");
            r.release(PATTERN_KEY);
            r.padUp(0);
        }

        beginTest("Up / Down: pads and knobs still act under the card; a quick press still pages; a hold that showed the card does not");
        {
            Rig r;
            r.fill();
            // Up + top pad selects, Up + bottom pad mutes.
            r.press(UP);
            r.wait();
            expect(r.showing(Key::Track));
            r.pad(2);
            expectEquals(r.seq.getSelectedTrack(), 2, "Up + top pad 3 selects track 3");
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.pad(8 + 3);
            expect(r.track(3).muted, "Up + bottom pad 4 mutes track 4");
            r.wait(5000);
            expect(r.mgr.getHelpCardKey() == Key::None, "gone for the rest of this hold");
            r.release(UP);
            r.press(DOWN);
            r.wait();
            expect(r.showing(Key::Track), "Down is the same key");
            r.pad(8 + 3);
            expect(!r.track(3).muted);
            r.pad(0);
            r.release(DOWN);
            expectEquals(r.seq.getSelectedTrack(), 0);

            // Up + turn: every track.
            const float v0 = r.audio.getTrackParameter(0, MixParam::VOLUME), v5 = r.audio.getTrackParameter(5, MixParam::VOLUME);
            r.press(UP);
            r.wait();
            r.turn(0, -1);
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.release(UP);
            expect(r.audio.getTrackParameter(0, MixParam::VOLUME) < v0 && r.audio.getTrackParameter(5, MixParam::VOLUME) < v5,
                   "Up + turn changed every track");

            // The tap: STEP with two pages of steps.
            r.track(0).length = 32;
            r.mgr.setActiveScreen(STEP);
            auto stepPage = [&r] { return r.steps->getSubPage(); };
            expectEquals(stepPage(), juce::String("S 1-16"));
            r.press(DOWN);
            r.wait(120);
            r.release(DOWN);
            expectEquals(stepPage(), juce::String("S 17-32"), "a quick press of Down pages");
            r.press(UP);
            r.wait(300);
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.release(UP);
            expectEquals(stepPage(), juce::String("S 1-16"), "also a slowish one that saw no card");

            // The hold that showed the card: the card took the place of the tap.
            const auto before = r.picture();
            r.press(DOWN);
            r.wait();
            expect(r.showing(Key::Track));
            expectEquals(r.card(Key::Track, 1).lines[2].text, juce::String("TAP      STEP PAGE"));
            r.release(DOWN);
            expectEquals(stepPage(), juce::String("S 1-16"), "a hold that showed the card does not page");
            expect(r.picture() == before, "the screen is back exactly");

            // The card was up, then another button was used: still no paging on release.
            r.press(DOWN);
            r.wait();
            r.tap(SETTINGS_BUTTON);
            expect(r.mgr.getHelpCardKey() == Key::None);
            expectEquals(r.mgr.getActiveIndex(), (int) SETTINGS, "the button acted");
            r.mgr.setActiveScreen(STEP);
            r.release(DOWN);
            expectEquals(stepPage(), juce::String("S 1-16"));

            // Both buttons: the second takes the card away; neither pages after a card.
            r.press(DOWN);
            r.wait();
            r.press(UP);
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.release(DOWN);
            r.release(UP);
            expectEquals(stepPage(), juce::String("S 1-16"));

            // HELP OFF: as before the cards existed, however long the press.
            r.mgr.setHelpMode(Mode::Off);
            r.press(DOWN);
            r.wait(2000);
            r.release(DOWN);
            expectEquals(stepPage(), juce::String("S 17-32"), "OFF: a long press pages too");
            // INST: the card is up at once, but a quick press is still a tap.
            r.mgr.setHelpMode(Mode::Instant);
            r.press(UP);
            expect(r.showing(Key::Track));
            r.wait(120);
            r.release(UP);
            expectEquals(stepPage(), juce::String("S 1-16"), "INST: a quick press pages");
            r.press(DOWN);
            r.wait();
            r.release(DOWN);
            expectEquals(stepPage(), juce::String("S 1-16"), "INST: a hold does not");
            // INST, a quick chord: the pad acts and the release does not page (a pad was used).
            r.press(DOWN);
            r.pad(1);
            r.release(DOWN);
            expectEquals(r.seq.getSelectedTrack(), 1);
            r.mgr.setHelpMode(Mode::Delayed);
        }

        beginTest("Record: alone it still arms when let go, after a short hold and after the card; Record + Play still records");
        {
            Rig r;
            r.press(RECORD);
            r.wait(120);
            expect(r.mgr.getHelpCardKey() == Key::None);
            expect(!r.mgr.isRecordArmed(), "nothing at the press");
            r.release(RECORD);
            expect(r.mgr.isRecordArmed(), "a short hold: recording on at the release");
            expect(!r.seq.isPlaying());
            expectEquals(r.text(), juce::String("REC FREE"));

            r.wait(3000);
            r.press(RECORD);
            r.wait();
            expect(r.showing(Key::Record));
            // (Every state's card is also checked for length and font.)
            auto recordCard = [this, &r]
            {
                const auto c = r.card(Key::Record, 0);
                expectEquals(cardFault(c), juce::String());
                return c;
            };
            auto card = recordCard();
            expectEquals(card.lines[0].text, juce::String("LET GO   RECORD OFF"), "armed: letting go switches it off");
            expectEquals(card.lines[2].text, juce::String("+ PLAY   RECORD NOW"));
            r.release(RECORD);
            expect(!r.mgr.isRecordArmed(), "after the card too");

            // Record + Play under the card.
            r.wait(3000);
            r.press(RECORD);
            r.wait();
            card = recordCard();
            expectEquals(card.lines[0].text, juce::String("LET GO   RECORD ON"));
            expectEquals(card.lines[1].text, juce::String(" THEN PLAY RECORDS"));
            expectEquals(card.lines[3].text, juce::String(" FROM THE TOP"));
            expectEquals(card.lines[4].text, juce::String("AGAIN    FREE / QUANT"));
            expectEquals(card.lines[5].text, juce::String(" NOW: REC FREE"));
            r.tap(PLAY);
            expect(r.mgr.getHelpCardKey() == Key::None, "Play takes the card away");
            expect(r.mgr.isRecordArmed() && r.seq.isPlaying(), "and starts recording");
            expect(r.seq.getRecordMode() == SequencerCore::RecordMode::Free);
            r.tap(PLAY);
            expect(r.seq.getRecordMode() == SequencerCore::RecordMode::Quant, "again: quantized");
            r.release(RECORD);
            expect(r.mgr.isRecordArmed() && r.seq.isPlaying(), "the release after the chord is not a plain Record");

            // While recording the card says what the chord does now.
            r.press(RECORD);
            r.wait();
            card = recordCard();
            expectEquals(card.lines[0].text, juce::String("LET GO   RECORD OFF"));
            expectEquals(card.lines[1].text, juce::String(" PLAYING GOES ON"));
            expectEquals(card.lines[2].text, juce::String("+ PLAY   FREE / QUANT"));
            expectEquals(card.lines[3].text, juce::String(" NOW: REC QUANT"));
            r.release(RECORD);
            expect(!r.mgr.isRecordArmed() && r.seq.isPlaying(), "Record alone ends recording and keeps playing");

            // Playing, not recording.
            r.press(RECORD);
            r.wait();
            card = recordCard();
            expectEquals(card.lines[1].text, juce::String(" RECORDS FROM NOW"));
            expectEquals(card.lines[2].text, juce::String("+ PLAY   RECORD ON"));
            expectEquals(card.lines[3].text, juce::String(" PLAYING GOES ON"));
            r.release(RECORD);
            r.tap(PLAY);
            expect(!r.seq.isPlaying() && !r.mgr.isRecordArmed());
        }

        beginTest("While a card shows, the bank buttons page that key's cards and do nothing else");
        for (const Key key : layerKeys)
        {
            Rig r;
            r.fill();
            r.tap(BANK_DOWN);                               // TRACKS on its second page (PAN)
            expectEquals(r.home->getPageIndex(), 1);
            const auto state = r.allText();
            const int count = r.mgr.getHelpCardCount(key);
            const int cc = ccOf(key);

            r.press(cc);
            r.wait();
            expect(r.showing(key));
            expectEquals(r.mgr.getHelpCardPage(key), 0);
            std::array<Screen::PadColour, 16> ledsBefore, ledsNow;
            const auto source = r.mgr.getPadFrame(ledsBefore);
            std::vector<DisplayRenderer::PixelBuffer> pictures;
            for (int i = 0; i < count; ++i)
            {
                pictures.push_back(r.picture());
                r.tap(BANK_DOWN);
                expect(r.showing(key), nameOf(key) + ": paging does not dismiss");
                expectEquals(r.mgr.getHelpCardPage(key), (i + 1) % count, "round and round");
                expectEquals(r.home->getPageIndex(), 1, "the screen's own page stays");
                expect(r.mgr.getPadFrame(ledsNow) == source);
            }
            if (count > 1)
            {
                expect(pictures[0] != pictures[1], "two different cards");
                r.tap(BANK_UP);
                expectEquals(r.mgr.getHelpCardPage(key), count - 1, "back from the first is the last");
            }
            r.release(cc);
            if (key == Key::Record) { r.tap(RECORD); r.wait(3000); }
            expectEquals(r.home->getPageIndex(), 1);
            expectEquals(r.allText(), state, "nothing else happened");
            expectEquals(r.seq.getSelectedTrack(), 0);
            expectEquals(r.seq.getActivePatternIndex(), 0);

            // The page is remembered for the next hold of the same key, each key has its own, and
            // another screen starts at the first card again.
            if (count > 1)
            {
                r.press(cc);
                r.wait();
                expectEquals(r.mgr.getHelpCardPage(key), count - 1);
                expect(r.picture() == pictures[(size_t) count - 1]);
                r.release(cc);
                expectEquals(r.mgr.getHelpCardPage(key == Key::Pattern ? Key::Track : Key::Pattern), 0);
                expectEquals(r.mgr.getHelpCardPage(Key::Shift), 0);
                r.mgr.setActiveScreen(STEP);
                r.press(cc);
                r.wait();
                expectEquals(r.mgr.getHelpCardPage(key), 0);
                r.release(cc);
            }

            // Before the card shows a bank button keeps its meaning (and that hold shows no card).
            r.mgr.setActiveScreen(TRACKS);
            const int page = r.home->getPageIndex();
            r.press(cc);
            r.wait(100);
            r.tap(BANK_DOWN);
            expectEquals(r.home->getPageIndex(), (page + 1) % (int) HomeScreen::NUM_PAGES);
            r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.release(cc);
        }

        beginTest("Held alone: keys and strips leave the card; a key joined by, or joining, another hold shows none");
        {
            Rig r;
            for (const Key key : layerKeys)
            {
                r.press(ccOf(key));
                r.wait();
                r.mgr.noteOnReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 60, 100);
                r.mgr.noteOffReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 60);
                r.mgr.controlChangeReceived(1, 1, 64);      // the mod strip
                expect(r.showing(key), nameOf(key) + ": a key or a strip does not dismiss");
                // A modifier joins: this card goes, and the modifier (not alone) gets none.
                r.shift(true);
                expect(r.mgr.getHelpCardKey() == Key::None, nameOf(key) + ": Shift joins, the card goes");
                r.wait(1000);
                expect(r.mgr.getHelpCardKey() == Key::None);
                r.shift(false);
                r.wait(1000);
                expect(r.mgr.getHelpCardKey() == Key::None, "and it does not come back in this hold");
                r.release(ccOf(key));
                if (key == Key::Record) { r.tap(RECORD); r.wait(3000); }
                r.mgr.setActiveScreen(TRACKS);
            }
            expect(!r.mgr.isRecordArmed());

            // Under Shift or Func these keys are chords, not layers: no card of their own.
            r.shift(true);
            r.press(PATTERN_KEY); r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None, "Shift + >");
            r.release(PATTERN_KEY);
            r.shift(false);
            r.shift(true);
            r.press(RECORD); r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None, "Shift + Record is fill");
            expect(r.mgr.isFillHeld());
            r.release(RECORD);
            r.shift(false);
            expect(!r.mgr.isRecordArmed());
            r.func(true);
            r.press(UP); r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None, "Func + Up");
            expect(!r.mgr.isTrackLayerHeld());
            r.release(UP);
            r.press(PATTERN_KEY); r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None, "Func + > is paste");
            expect(!r.mgr.isPatternKeyHeld());
            r.release(PATTERN_KEY);
            r.func(false);

            // Two layers: the second takes the first one's card away and gets none.
            r.press(PATTERN_KEY);
            r.wait();
            expect(r.showing(Key::Pattern));
            r.press(UP);
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None, "> then Up: no card for either");
            r.release(UP);
            r.release(PATTERN_KEY);
            r.press(UP);
            r.press(RECORD);
            r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None, "Up then Record");
            r.release(RECORD);
            r.release(UP);
            expect(r.mgr.isRecordArmed(), "(that Record was still a plain Record)");
            r.tap(RECORD);
            // Play down first: Record is not alone.
            r.press(PLAY);
            r.press(RECORD);
            r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None, "Play then Record");
            r.release(RECORD);
            r.release(PLAY);
            r.tap(PLAY);
            expect(!r.seq.isPlaying());

            // A held pad does not stand in the way, and its pressure does not dismiss.
            r.mgr.setActiveScreen(STEP);
            r.padDown(2);
            r.wait(1000);
            for (const Key key : { Key::Pattern, Key::Track })
            {
                r.press(ccOf(key));
                r.wait();
                expect(r.showing(key), nameOf(key) + ": with a step held");
                r.mgr.aftertouchReceived(1, Rig::noteOf(2), 90);
                expect(r.showing(key));
                r.release(ccOf(key));
                expect(r.steps->isDetailActive(), "the held step's pages are still up");
            }
            r.padUp(2);
        }

        beginTest("> PATTERNS and > BUTTONS, KNOBS say what is there: current, queued, used and empty patterns, the kept copy, the held step");
        {
            Rig r;
            r.fill();                                       // pattern 1 has trigs
            r.seq.getPattern(6).getTrack(0).getStep(0).trig = true;
            auto card = r.card(Key::Pattern, 0);
            expectEquals((int) card.padRows.size(), 2);
            expectEquals(card.padRows[0].line, 0);
            expectEquals(card.padRows[1].line, 2);
            for (int pad = 0; pad < 16; ++pad)
            {
                const auto& label = card.padRows[(size_t) (pad / 8)].labels[(size_t) (pad % 8)];
                const juce::String expected = (pad == 0 || pad == 6) ? juce::String(pad + 1) : juce::String(".");
                expectEquals(label.text, expected, "pad " + juce::String(pad + 1));
                expect(label.inverted == (pad == 0), "only the current pattern is inverted");
                expect(!label.marked);
            }
            expectEquals(card.lines[4].text, juce::String("PATTERN 1"));
            expectEquals(card.lines[5].text, juce::String("PAD: GO TO PATTERN"));

            // An empty pattern that is the current one still shows its number.
            r.seq.selectPattern(15, SequencerCore::ChangeMode::Immediate);
            card = r.card(Key::Pattern, 0);
            expectEquals(card.padRows[1].labels[7].text, juce::String("16"));
            expect(card.padRows[1].labels[7].inverted && !card.padRows[0].labels[0].inverted);
            expectEquals(card.padRows[0].labels[0].text, juce::String("1"));
            expectEquals(card.lines[4].text, juce::String("PATTERN 16"));
            expectEquals(r.card(Key::Pattern, 1).lines[1].text, juce::String(" OF PATTERN 16"));

            // The pad lights and the card agree on which pads carry something.
            r.press(PATTERN_KEY);
            std::array<Screen::PadColour, 16> leds;
            expect(r.mgr.getPadFrame(leds) == ScreenManager::PadSource::Layer);
            for (int pad = 0; pad < 16; ++pad)
            {
                const bool lit = leds[(size_t) pad].r + leds[(size_t) pad].g + leds[(size_t) pad].b > 0;
                expect(lit == (card.padRows[(size_t) (pad / 8)].labels[(size_t) (pad % 8)].text != "."), "pad " + juce::String(pad + 1));
            }
            r.release(PATTERN_KEY);
            r.seq.selectPattern(0, SequencerCore::ChangeMode::Immediate);

            // The knob lines: a screen without steps, one with, and a step held.
            expectEquals(r.card(Key::Pattern, 1).lines[0].text, juce::String("REC      KEEP A COPY"));
            expectEquals(r.card(Key::Pattern, 1).lines[2].text, juce::String("PLAY     RELOAD IT"));
            expectEquals(r.card(Key::Pattern, 1).lines[4].text, juce::String("KNOB     NO CHANGE"), "TRACKS");
            for (int screen : { (int) STEP, (int) PARAM, (int) FLTR, (int) TRKFX })
            {
                r.mgr.setActiveScreen(screen);
                expectEquals(r.card(Key::Pattern, 1).lines[4].text, juce::String("HOLD A STEP + KNOB:"), screenNames[screen]);
                expectEquals(r.card(Key::Pattern, 1).lines[5].text, juce::String(" REMOVE THAT LOCK"), screenNames[screen]);
                r.padDown(0);
                r.wait(1000);
                expect(r.mgr.getActiveScreen()->isStepHeld(), screenNames[screen]);
                expectEquals(r.card(Key::Pattern, 1).lines[4].text, juce::String("KNOB     REMOVE LOCK"), screenNames[screen]);
                expectEquals(r.card(Key::Pattern, 1).lines[5].text, juce::String(" ON THE HELD STEP"), screenNames[screen]);
                r.padUp(0);
            }

            // Nothing kept and nothing loaded: the card says there is nothing to reload, and there is not.
            Rig fresh;
            fresh.mgr.setSequencer(&fresh.seq);             // (forgets the copies made at "project loaded")
            expectEquals(fresh.card(Key::Pattern, 1).lines[3].text, juce::String(" (NOTHING TO RELOAD)"));
            fresh.press(PATTERN_KEY);
            fresh.tap(PLAY);
            fresh.release(PATTERN_KEY);
            expectEquals(fresh.text(), juce::String("NO COPY"));
        }

        beginTest("UP DOWN: TRACKS and UP DOWN: KNOBS say what is there: selected track, engines, mute and solo, the tap, the all-tracks turn");
        {
            Rig r;
            r.fill();
            r.seq.setSelectedTrack(2);
            r.track(4).muted = true;
            r.track(6).soloed = true;
            r.track(7).muted = true;
            r.track(7).soloed = true;
            const auto card = r.card(Key::Track, 0);
            expectEquals((int) card.padRows.size(), 2);
            const int tracks = juce::jmin(8, r.seq.getPattern().getTrackCount());
            expectEquals(tracks, 8);
            for (int t = 0; t < tracks; ++t)
            {
                const auto& top = card.padRows[0].labels[(size_t) t];
                const auto& bottom = card.padRows[1].labels[(size_t) t];
                expectEquals(top.text, juce::String(t + 1) + (t == 1 ? "DR" : "VA"), "track and engine");
                expect(top.inverted == (t == 2), "the selected track is inverted");
                expectEquals(bottom.text, juce::String(t >= 6 ? "SOLO" : (t == 4 ? "MUTE" : "ON")), "track " + juce::String(t + 1));
            }
            expectEquals(card.lines[4].text, juce::String("TOP: SELECT A TRACK"));
            expectEquals(card.lines[5].text, juce::String("BOTTOM: MUTE ON / OFF"));

            // The pad lights and the card agree.
            r.press(UP);
            std::array<Screen::PadColour, 16> leds;
            expect(r.mgr.getPadFrame(leds) == ScreenManager::PadSource::Layer);
            for (int t = 0; t < 8; ++t)
            {
                expect((leds[(size_t) t].r > 100) == card.padRows[0].labels[(size_t) t].inverted);
                const auto& c = leds[(size_t) (8 + t)];
                const juce::String shown = c.b > 100 ? "SOLO" : (c.r > 100 ? "MUTE" : "ON");
                expectEquals(card.padRows[1].labels[(size_t) t].text, shown);
            }
            r.release(UP);
            r.seq.setSelectedTrack(0);

            // The KNOBS card per screen: what a tap moves, and whether a turn reaches every track.
            struct Expect { int screen; const char* tap; bool all; };
            static const Expect expected[] = {
                { TRACKS, "TAP      NOTHING HERE", true },  { STEP,   "TAP      STEP PAGE",   false },
                { PARAM,  "TAP      STEP PAGE",    true },  { DRUMS,  "TAP      NOTHING HERE", false },
                { PRESET, "TAP      PRESET LIST",  false }, { SETTINGS, "TAP      NOTHING HERE", false },
                { FLTR,   "TAP      STEP PAGE",    true },  { ARP,    "TAP      NOTHING HERE", false },
                { SCALE,  "TAP      SCALE PAGE",   false }, { SONG,   "TAP      SONG ROW",    false },
                { ATROUTE, "TAP      NOTHING HERE", false }, { WAVE,  "TAP      NOTHING HERE", false },
                { TRKFX,  "TAP      STEP PAGE",    true } };
            for (auto& e : expected)
            {
                r.mgr.setActiveScreen(e.screen);
                const auto knobs = r.card(Key::Track, 1);
                expectEquals(knobs.lines[2].text, juce::String(e.tap), screenNames[e.screen]);
                expectEquals(knobs.lines[3].text, juce::String(juce::String(e.tap).contains("NOTHING") ? "" : " UP = BACK, DOWN = ON"));
                expectEquals(knobs.lines[0].text, juce::String(e.all ? "KNOB     ALL TRACKS" : "KNOB     NO CHANGE"), screenNames[e.screen]);
                expectEquals(knobs.lines[1].text, juce::String(e.all ? " THAT HAVE THIS CELL" : " (THIS TRACK ONLY)"));
                expectEquals(knobs.lines[4].text, juce::String(e.screen == TRACKS ? "SOLO: LET GO, THEN" : "SOLO: TRACKS SCREEN,"));
                expectEquals(knobs.lines[5].text, juce::String(" FUNC + BOTTOM PAD"));
            }
            // TRACKS on ENG: a knob only chooses its own track's engine. PARAM with a step held: a lock.
            r.mgr.setActiveScreen(TRACKS);
            r.func(true); r.tap(BANK_DOWN); r.func(false);
            expectEquals(r.home->getPageIndex(), (int) HomeScreen::ENG);
            expectEquals(r.card(Key::Track, 1).lines[0].text, juce::String("KNOB     NO CHANGE"));
            r.mgr.setActiveScreen(PARAM);
            r.padDown(0);
            r.wait(500);
            expectEquals(r.card(Key::Track, 1).lines[0].text, juce::String("KNOB     NO CHANGE"), "a held step: the turn is a lock on this track");
            r.padUp(0);

            // What the card says a tap does is what a tap does (no card: quick presses).
            r.mgr.setActiveScreen(SCALE);
            const int scalePage = r.scales.getActivePage();
            expect(r.scales.getPageCount() > scalePage + 1, "the scale pads have a next page");
            r.tap(DOWN);
            expectEquals(r.scales.getActivePage(), scalePage + 1);
            r.tap(UP);
            expectEquals(r.scales.getActivePage(), scalePage);
        }

        beginTest("No new card is clipped, overlaps or runs over 21 characters: every screen, with and without a held step, stopped and playing");
        {
            int cards = 0;
            for (int variant = 0; variant < 3; ++variant)
            {
                Rig r;
                r.fill();
                DisplayRenderer::DrawAudit drawing;
                r.display.setDrawAudit(&drawing);
                if (variant == 2)
                {
                    // The widest of everything: pattern 16 current with 15 queued while playing, every
                    // track soloed or muted, the last track selected.
                    for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                        r.seq.getPattern(p).getTrack(0).getStep(0).trig = true;
                    r.seq.selectPattern(15, SequencerCore::ChangeMode::Immediate);
                    for (int t = 0; t < r.seq.getPattern().getTrackCount(); ++t)
                        (t % 2 ? r.track(t).muted : r.track(t).soloed) = true;
                    r.seq.setSelectedTrack(r.seq.getPattern().getTrackCount() - 1);
                    r.seq.start();
                    r.seq.selectPattern(14, SequencerCore::ChangeMode::NextLoop);
                    expectEquals(r.seq.getPendingPattern(), 14);
                }
                for (int screen = 0; screen < NUM_SCREENS; ++screen)
                {
                    r.mgr.setActiveScreen(screen);
                    if (variant >= 1)
                    {
                        r.padDown(0);
                        r.wait(1000);
                    }
                    for (const Key key : layerKeys)
                    {
                        r.press(ccOf(key));
                        r.wait();
                        expect(r.showing(key), juce::String(screenNames[screen]) + " " + nameOf(key));
                        for (int page = 0; page < r.mgr.getHelpCardCount(key); ++page)
                        {
                            const juce::String where = juce::String(screenNames[screen]) + " variant " + juce::String(variant)
                                                     + " " + nameOf(key) + " " + juce::String(page + 1);
                            expectEquals(r.mgr.getHelpCardPage(key), page, where);
                            const auto card = r.card(key, page);
                            expectEquals(cardFault(card), juce::String(), where);
                            for (auto& line : card.lines)
                                expect(line.text.length() <= 21, where + ": " + line.text);
                            for (auto& row : card.padRows)
                                for (auto& label : row.labels)
                                    expect(label.text.length() + (label.marked ? 1 : 0) <= 4, where + ": " + label.text);
                            r.tick();
                            expect(!drawing.overText.any(), where + ": text and other drawing share pixels");
                            expect(!drawing.clipped.any(), where + ": text runs off the glass");
                            ++cards;
                            r.tap(BANK_DOWN);
                        }
                        r.release(ccOf(key));
                        if (key == Key::Record)
                        {
                            r.tap(RECORD);                  // (its release switched recording on)
                            expect(!r.mgr.isRecordArmed(), juce::String(screenNames[screen]));
                            r.mgr.setActiveScreen(screen);
                        }
                    }
                    if (variant >= 1)
                        r.padUp(0);
                }
                if (r.seq.isPlaying()) r.seq.stop();
                r.display.setDrawAudit(nullptr);
            }
            expectEquals(cards, 3 * NUM_SCREENS * 5);
        }

        // NEPTUNE_HELP_DUMP=<file>: the new cards as text, beside the Shift / Func dump ("<file>.layers").
        if (const auto dump = juce::SystemStats::getEnvironmentVariable("NEPTUNE_HELP_DUMP", {}); dump.isNotEmpty())
        {
            juce::String out;
            const auto pictures = juce::File(dump).getSiblingFile("cards");
            pictures.createDirectory();
            auto add = [&out, &pictures](Rig& r, const juce::String& label)
            {
                for (const Key key : layerKeys)
                {
                    for (int page = 0; page < r.mgr.getHelpCardCount(key); ++page)
                        out << "== " << label << ", " << nameOf(key) << " " << (page + 1) << "\n"
                            << HelpCardTests::mockup(r.card(key, page)) << "\n";
                    if (key == Key::Record)
                        continue;                           // (its release would switch recording)
                    r.press(ccOf(key));
                    r.wait();
                    for (int page = 0; page < r.mgr.getHelpCardCount(key); ++page)
                    {
                        pictures.getChildFile(label + " " + (key == Key::Pattern ? "Pattern" : "Track") + " " + juce::String(page + 1) + ".pbm")
                                .replaceWithText(HelpCardTests::pbm(r.picture()));
                        r.tap(BANK_DOWN);
                    }
                    r.release(ccOf(key));
                }
            };
            for (int held = 0; held < 2; ++held)
                for (int screen = 0; screen < NUM_SCREENS; ++screen)
                {
                    Rig r;
                    r.fill();
                    r.seq.getPattern(6).getTrack(0).getStep(0).trig = true;
                    r.track(4).muted = true;
                    r.track(6).soloed = true;
                    r.mgr.setActiveScreen(screen);
                    if (held) { r.padDown(0); r.wait(1000); }
                    add(r, juce::String(screenNames[screen]) + (held ? " + pad 1 held" : ""));
                    if (held) r.padUp(0);
                }
            {
                Rig r;
                r.fill();
                r.press(PATTERN_KEY); r.tap(RECORD); r.release(PATTERN_KEY);
                r.press(RECORD); r.tap(PLAY); r.release(RECORD);
                r.seq.selectPattern(6, SequencerCore::ChangeMode::NextLoop);
                add(r, "TRACKS, recording, pattern 7 queued, a copy kept");
                r.press(RECORD);
                r.wait();
                pictures.getChildFile("TRACKS recording Record 1.pbm").replaceWithText(HelpCardTests::pbm(r.picture()));
                r.release(RECORD);
                r.seq.stop();
            }
            juce::File(dump + ".layers").replaceWithText(out);
        }
    }
};

static HelpCardLayersTests helpCardLayersTests;
