#pragma once

#include "../src/ui/ScreenManager.h"
#include "../src/ui/ScreenAudit.h"
#include "../src/ui/UiClock.h"
#include "../src/ui/GrammarHooks.h"
#include "../src/ui/PadLEDFeedback.h"
#include "../src/ui/screens/HomeScreen.h"
#include "../src/ui/screens/StepViewScreen.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include "../src/ui/screens/DrumPadScreen.h"
#include "../src/ui/screens/PresetScreen.h"
#include "../src/ui/screens/SettingsScreen.h"
#include "../src/ui/screens/FilterViewScreen.h"
#include "../src/ui/screens/ArpScreen.h"
#include "../src/ui/screens/CustomScaleScreen.h"
#include "../src/ui/screens/SongScreen.h"
#include "../src/ui/screens/AftertouchScreen.h"
#include "../src/ui/screens/WaveformScreen.h"
#include "../src/ui/screens/TrackFxScreen.h"
#include "../src/controller/DisplayRenderer.h"
#include "../src/controller/LaunchkeyController.h"
#include "../src/controller/PadManager.h"
#include "../src/controller/CustomScaleManager.h"
#include "../src/arpeggiator/Arpeggiator.h"
#include "../src/sequencer/SongMode.h"
#include "../src/project/PresetManager.h"
#include <juce_core/juce_core.h>

// The help cards: hold Shift or Func alone and the display says what that key does on this screen.
// Driven as the app is: the controller's own messages into ScreenManager, time through UiClock, the
// picture through the 30 Hz refresh's redraw. The rig holds the app's thirteen screens in the app's
// order, with the app's Shift + pad table, copy contexts, pinnable screens and map names (main.cpp).
namespace helptest
{
    using Key = ScreenManager::HelpKey;
    using Edit = ScreenManager::Edit;
    using Card = ScreenManager::HelpCard;

    enum { TRACKS = 0, STEP, PARAM, DRUMS, PRESET, SETTINGS, FLTR, ARP, SCALE, SONG, ATROUTE, WAVE, TRKFX, NUM_SCREENS };
    static const char* const screenNames[NUM_SCREENS] = { "TRACKS", "STEP", "PARAM", "DRUMS", "PRESET", "SETTINGS", "FLTR", "ARP",
                                                          "SCALE", "SONG", "AT ROUTE", "WAVE", "TRK FX" };
    static const char* const mapNames[NUM_SCREENS] = { "TRKS", "STEP", "PARM", "DRUM", "PRST", "SETT", "FLTR", "ARP",
                                                       "SCAL", "SONG", "ATRT", "WAVE", "TKFX" };
    enum : int { BANK_UP = 51, BANK_DOWN = 52, PATTERN_KEY = 104, UP = 106, DOWN = 107, SETTINGS_BUTTON = 108,
                 PLAY = 115, RECORD = 117 };

    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        PadManager pads;
        PadLEDFeedback leds;
        LaunchkeyController controller;
        DisplayRenderer display;
        PresetManager presets;
        CustomScaleManager scales;
        SongMode song;
        Arpeggiator arp;
        ScreenManager mgr;
        HomeScreen* home = nullptr;
        StepViewScreen* steps = nullptr;
        ParamPageScreen* param = nullptr;
        ParamSpec moved;                // an engine parameter of track 1, moved off its default
        float movedDefault = 0.0f, movedValue = 0.0f;

        Rig()
        {
            seq.prepareToPlay(44100.0, 512);
            audio.setSequencer(&seq);
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                audio.addEngine(t, AudioEngine::createEngine(t == 1 ? EngineType::Drum : EngineType::UnifiedVoice));
            seq.getPattern().getTrack(1).engineType = EngineType::Drum;
            display.setController(&controller);
            mgr.setSequencer(&seq);
            mgr.setDisplay(&display);
            mgr.setEngineHooks(GrammarHooks::make(seq, audio));

            home = new HomeScreen(seq, &audio);
            steps = new StepViewScreen(seq, pads, leds);
            param = new ParamPageScreen(seq, &audio);
            steps->setLockPages(param);
            SettingsScreen::Hooks h;
            h.getHelpMode = [this] { return (int) mgr.getHelpMode(); };
            h.setHelpMode = [this](int mode) { mgr.setHelpMode((ScreenManager::HelpMode) mode); };

            add(home);
            add(steps);
            add(param);
            add(new DrumPadScreen(dynamic_cast<DrumEngine*>(audio.getEngine(1))));
            add(new PresetScreen(seq, &audio, presets));
            add(new SettingsScreen(std::move(h)));
            add(new FilterViewScreen(seq, &audio));
            add(new ArpScreen(arp));
            add(new CustomScaleScreen(scales));
            add(new SongScreen(song, seq));
            add(new AftertouchScreen(audio, [this] { return seq.getSelectedTrack(); }));
            add(new WaveformScreen(seq, &audio));
            add(new TrackFxScreen(seq, &audio));

            static constexpr int map[16] = { PARAM, TRACKS, TRKFX, STEP, PRESET, SONG, SETTINGS, ATROUTE,
                                             STEP, DRUMS, ARP, SCALE, WAVE, FLTR, -1, -1 };
            for (int pad = 0; pad < 16; ++pad)
                mgr.setShiftPadScreen(pad, map[pad]);
            mgr.setButtonScreens(SETTINGS, ARP, SCALE);
            mgr.setStepScreen(STEP);
            mgr.setPinnable(DRUMS, true);
            mgr.setPinnable(SCALE, true);
            mgr.setScreenContext(STEP, ScreenManager::CopyContext::Steps);
            mgr.setScreenContext(TRACKS, ScreenManager::CopyContext::Pattern);
            mgr.setScreenContext(PARAM, ScreenManager::CopyContext::Sound);
            mgr.setScreenContext(FLTR, ScreenManager::CopyContext::Sound);
            mgr.setScreenContext(TRKFX, ScreenManager::CopyContext::Fx);
            for (int i = 0; i < NUM_SCREENS; ++i)
                mgr.setScreenMapName(i, mapNames[i]);
            mgr.projectLoaded();
            mgr.setActiveScreen(TRACKS);
        }

        void add(Screen* s) { mgr.addScreen(std::unique_ptr<Screen>(s)); }

        // Something of every kind the three verbs can act on: trigs on two tracks, a lock on the
        // step that pad 1 holds, an engine parameter off its default, a mixer setting, an effect on.
        void fill()
        {
            for (auto& page : audio.getEngine(0)->getParamPages())
                for (auto& p : page.params)
                    if (!moved.isValid() && p.isValid() && p.kind == ParamKind::Linear && p.maxVal > p.minVal)
                        moved = p;
            movedDefault = audio.getTrackParameter(0, moved.id);
            movedValue = (movedDefault > (moved.minVal + moved.maxVal) * 0.5f) ? moved.minVal : moved.maxVal;
            audio.setTrackParameter(0, moved.id, movedValue);
            track(0).soundParams[moved.id] = movedValue;
            audio.setTrackParameter(0, MixParam::VOLUME, 0.4f);
            track(0).soundParams[MixParam::VOLUME] = 0.4f;
            audio.setTrackParameter(0, TrackFxParam::DIRT_BASE, 0.6f);
            track(0).soundParams[TrackFxParam::DIRT_BASE] = 0.6f;

            track(0).getStep(0).trig = true;
            track(0).getStep(0).paramLocks[moved.id] = movedDefault;
            track(0).getStep(3).trig = true;
            track(0).getStep(3).paramLocks[moved.id] = movedDefault;
            track(2).getStep(5).trig = true;
        }

        Track& track(int t) { return seq.getPattern().getTrack(t); }

        static int noteOf(int pad) { return pad < 8 ? LaunchkeyController::PAD_TOP_ROW_START + pad
                                                    : LaunchkeyController::PAD_BOTTOM_ROW_START + (pad - 8); }
        void padDown(int pad) { mgr.noteOnReceived(1, noteOf(pad), 100); }
        void padUp(int pad)   { mgr.noteOffReceived(1, noteOf(pad)); }
        void pad(int p)       { padDown(p); padUp(p); }
        void press(int cc)    { mgr.controlChangeReceived(1, cc, 127); }
        void release(int cc)  { mgr.controlChangeReceived(1, cc, 0); }
        void tap(int cc)      { press(cc); release(cc); }
        void shift(bool on)   { mgr.controlChangeReceived(7, 63, on ? 127 : 0); }
        void func(bool on)    { mgr.controlChangeReceived(1, 105, on ? 127 : 0); }
        void hold(Key key, bool on) { if (key == Key::Shift) shift(on); else func(on); }
        void turn(int encoder, int clicks = 1) { mgr.controlChangeReceived(16, 0x15 + encoder, clicks < 0 ? 128 + clicks : clicks); }
        // The app's 30 Hz refresh, as far as the display goes.
        void tick() { const juce::ScopedLock ui(mgr.getLock()); mgr.requestRedraw(); mgr.redrawIfNeeded(); }
        void wait(juce::int64 ms = ScreenManager::HELP_DELAY_MS + 10) { UiClock::advance(ms); tick(); }
        DisplayRenderer::PixelBuffer picture() { tick(); return display.getPixels(); }
        juce::String text() const { return mgr.composeNameBar().text; }
        bool showing(Key key) const { return mgr.getHelpCardKey() == key; }
        Card card(Key key, int page) const { return mgr.getHelpCard(key, page); }

        // A modifier held past the delay with a button pressed under it: the card shows, the press
        // takes it away and acts.
        // (A whole-pattern clear or paste needs its button held: then it is held long enough.)
        void chord(Key key, int cc)
        {
            hold(key, true);
            wait();
            press(cc);
            if (mgr.isEditHeld())
                UiClock::advance(ScreenManager::HOLD_MS + 10);
            release(cc);
            hold(key, false);
        }
        // Something that is not an edit, leaving the screen where it was: an armed undo is over.
        void somethingElse() { tap(BANK_DOWN); tap(BANK_UP); }

        // ---- What the three verbs can change, as text ---------------------------------------------
        juce::String stepText(int t, int i)
        {
            auto& s = track(t).getStep(i);
            if (!s.trig && s.paramLocks.empty()) return {};
            return juce::String(i) + (s.trig ? "t" : "-") + juce::String((int) s.paramLocks.size()) + " ";
        }
        juce::String stepsText(int t, int except = -1)
        {
            juce::String out;
            for (int i = 0; i < track(t).length; ++i)
                if (i != except) out << stepText(t, i);
            return out;
        }
        juce::String soundText()
        {
            return juce::String((int) audio.getEngine(0)->getType()) + " " + juce::String(audio.getTrackParameter(0, moved.id), 3)
                 + " " + juce::String(audio.getTrackParameter(0, MixParam::VOLUME), 3)
                 + " " + juce::String(audio.getTrackParameter(0, TrackFxParam::DIRT_BASE), 3);
        }
        juce::String allText()
        {
            juce::String out;
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                out << "T" << t << ": " << stepsText(t) << "| ";
            return out + soundText();
        }
    };

    // What a card says, checked against the glass: no line longer than the display, no pad label
    // longer than its column allows, only characters the font has, pad-label lines free of text.
    inline juce::String cardFault(const Card& c)
    {
        auto fontHas = [](const juce::String& s)
        {
            for (int i = 0; i < s.length(); ++i)
                if (s[i] < 32 || s[i] > 90) return false;
            return true;
        };
        if (c.title.isEmpty()) return "no title";
        if (c.title.length() + 1 + c.pages.length() > ScreenManager::HELP_LINE_CHARS - 1) return "title too long: " + c.title;
        if (!fontHas(c.title) || !fontHas(c.pages)) return "title not in the font";
        for (auto& line : c.lines)
        {
            if (line.text.length() > ScreenManager::HELP_LINE_CHARS) return "line too long: " + line.text;
            if (!fontHas(line.text)) return "not in the font: " + line.text;
        }
        for (auto& row : c.padRows)
        {
            if (row.line < 0 || row.line + 1 >= ScreenManager::HELP_LINES) return "pad row off the card";
            if (c.lines[(size_t) row.line].text.isNotEmpty() || c.lines[(size_t) row.line + 1].text.isNotEmpty())
                return "text on a pad-label line";
            for (auto& label : row.labels)
            {
                if (label.text.length() > ScreenManager::HELP_PAD_CHARS) return "pad label too long: " + label.text;
                if (!fontHas(label.text)) return "pad label not in the font: " + label.text;
            }
        }
        return {};
    }
}

class HelpCardTests : public juce::UnitTest
{
public:
    HelpCardTests() : UnitTest("Help Cards") {}

    void runTest() override
    {
        using namespace helptest;
        using Mode = ScreenManager::HelpMode;

        beginTest("No card before the delay, the card after it, and the screen back exactly on release");
        for (const Key key : { Key::Shift, Key::Func })
        {
            Rig r;
            r.fill();
            expect(r.mgr.getHelpMode() == Mode::Delayed, "0.35 s is the default");
            const auto screen = r.picture();

            r.hold(key, true);
            r.tick();
            expect(r.mgr.getHelpCardKey() == Key::None, "nothing at the press");
            expect(r.picture() == screen);
            r.wait(ScreenManager::HELP_DELAY_MS - 50);
            expect(r.mgr.getHelpCardKey() == Key::None, "nothing before the delay");
            expect(r.picture() == screen);
            r.wait(60);
            expect(r.showing(key), "the card after 350 ms, from the refresh alone");
            const auto cardPicture = r.picture();
            expect(cardPicture != screen, "the card replaces the picture");
            r.wait(5000);
            expect(r.showing(key), "and it stays while the key is held");
            expect(r.picture() == cardPicture);

            r.hold(key, false);
            expect(r.mgr.getHelpCardKey() == Key::None);
            expect(r.picture() == screen, "letting go gives the screen back exactly");
            expectEquals(r.mgr.getActiveIndex(), (int) TRACKS);
            expectEquals(r.home->getPageIndex(), 0);
        }

        beginTest("HELP on SETTINGS -> SETUP: OFF never shows a card, INST shows it at once");
        {
            Rig r;
            r.mgr.setActiveScreen(SETTINGS);
            auto* settings = r.mgr.getActiveScreen();
            expectEquals(settings->getName(), juce::String("SETUP"));
            expectEquals(settings->getEncoderName(6), juce::String("HELP"));

            r.turn(6, -1);                                  // 0.35S -> OFF
            expect(r.mgr.getHelpMode() == Mode::Off);
            for (const Key key : { Key::Shift, Key::Func })
            {
                r.hold(key, true);
                r.wait(3000);
                expect(r.mgr.getHelpCardKey() == Key::None, "OFF: never");
                r.hold(key, false);
            }

            r.turn(6, 2);                                   // -> INST
            expect(r.mgr.getHelpMode() == Mode::Instant);
            for (const Key key : { Key::Shift, Key::Func })
            {
                r.hold(key, true);
                expect(r.showing(key), "INST: at the press");
                r.hold(key, false);
            }
            // Instant, and the chords still act: the first other control takes the card away.
            r.shift(true);
            r.pad(1);
            expectEquals(r.mgr.getActiveIndex(), (int) TRACKS);
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.shift(false);

            r.mgr.setActiveScreen(SETTINGS);
            r.turn(6, -1);
            expect(r.mgr.getHelpMode() == Mode::Delayed);
        }

        beginTest("Any other control takes the card away for the rest of the hold and still acts");
        {
            // Shift + turn: fine, and the readout shows from the first click.
            Rig r;
            const float before = r.audio.getTrackParameter(0, MixParam::VOLUME);
            r.turn(0, -1);
            const float plain = before - r.audio.getTrackParameter(0, MixParam::VOLUME);
            expect(plain > 0.0f);
            r.wait(1000);

            r.shift(true);
            r.wait();
            expect(r.showing(Key::Shift));
            const float mid = r.audio.getTrackParameter(0, MixParam::VOLUME);
            r.turn(0, -1);
            expect(r.mgr.getHelpCardKey() == Key::None, "the turn removes the card");
            const float fine = mid - r.audio.getTrackParameter(0, MixParam::VOLUME);
            expectWithinAbsoluteError(fine, plain * 0.1f, plain * 0.02f, "and is a fine turn");
            expect(r.text().startsWith("T1 VOL"), "the knob's readout is in the name bar: " + r.text());
            r.wait(5000);
            expect(r.mgr.getHelpCardKey() == Key::None, "gone for the rest of this hold");
            r.shift(false);
            r.shift(true);
            r.wait();
            expect(r.showing(Key::Shift), "the next hold shows it again");

            // Shift + pad: go to that screen.
            r.pad(3);                                       // top pad 4 = STEP
            expectEquals(r.mgr.getActiveIndex(), (int) STEP);
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.shift(false);

            // Func + Play: clear, and the confirmation names what was cleared.
            r.fill();
            r.func(true);
            r.wait();
            expect(r.showing(Key::Func));
            r.tap(PLAY);
            expect(r.mgr.getHelpCardKey() == Key::None);
            expect(!r.seq.isPlaying());
            expectEquals(r.stepsText(0), juce::String());
            expectEquals(r.text(), juce::String("CLEAR T1 STEPS"));
            r.wait(1900);
            expectEquals(r.text(), juce::String("CLEAR T1 STEPS"), "shown for about two seconds");
            r.func(false);
            r.wait(200);
            expect(r.text() != "CLEAR T1 STEPS");

            // Func + Settings: tap tempo, not the SETTINGS screen.
            r.func(true);
            r.wait();
            r.tap(SETTINGS_BUTTON);
            expect(r.mgr.getHelpCardKey() == Key::None);
            expectEquals(r.mgr.getActiveIndex(), (int) STEP);
            expectEquals(r.text(), juce::String("TAP"));
            r.func(false);

            // Shift + Up / Down: the track.
            r.shift(true);
            r.wait();
            r.tap(DOWN);
            expectEquals(r.seq.getSelectedTrack(), 1);
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.tap(UP);
            r.shift(false);
            expectEquals(r.seq.getSelectedTrack(), 0);
        }

        beginTest("A fast chord never sees a card: Func + bank before the delay still jumps");
        {
            Rig r;
            r.func(true);
            r.wait(100);
            r.tap(BANK_DOWN);
            expectEquals(r.home->getPageIndex(), (int) HomeScreen::NUM_PAGES - 1, "TRACKS: Func + bank = the last page");
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None, "and that hold shows no card later");
            r.tap(BANK_UP);
            expectEquals(r.home->getPageIndex(), 0, "still the jump: the first page");
            expectEquals(r.mgr.getHelpCardPage(Key::Func), 0);
            r.func(false);

            // PARAM: from the first page of a section with several pages, Func + bank leaves the section.
            r.mgr.setActiveScreen(PARAM);
            const int n = r.param->getPageCount();
            int start = -1;
            for (int i = 0; i + 1 < n && start < 0; ++i)
            {
                r.param->setCurrentPageIndex(i);
                const int key = r.param->getActivePageKey();
                r.param->setCurrentPageIndex(i + 1);
                if (r.param->getActivePageKey() == key) start = i;
            }
            expect(start >= 0, "the synth has a section of two pages");
            r.param->setCurrentPageIndex(start);
            const int section = r.param->getActivePageKey();
            r.func(true);
            r.tap(BANK_DOWN);
            r.func(false);
            expect(r.param->getActivePageKey() != section, "section jump");
            expect(r.param->getPageIndex() > start + 1);
        }

        beginTest("While a card shows, the bank buttons page that key's cards and do nothing else");
        for (const Key key : { Key::Shift, Key::Func })
        {
            Rig r;
            r.fill();
            r.tap(BANK_DOWN);                               // TRACKS on its second page (PAN)
            expectEquals(r.home->getPageIndex(), 1);
            const auto state = r.allText();
            const int count = r.mgr.getHelpCardCount(key);
            expectEquals(count, 3);

            r.hold(key, true);
            r.wait();
            expectEquals(r.mgr.getHelpCardPage(key), 0);
            expectEquals(r.card(key, 0).pages, juce::String("1/3"));
            std::array<Screen::PadColour, 16> ledsBefore, ledsNow;
            const auto source = r.mgr.getPadFrame(ledsBefore);

            std::vector<DisplayRenderer::PixelBuffer> pictures;
            for (int i = 0; i < count; ++i)
            {
                pictures.push_back(r.picture());
                r.tap(BANK_DOWN);
                expect(r.showing(key), "paging does not dismiss");
                expectEquals(r.mgr.getHelpCardPage(key), (i + 1) % count, "round and round");
                expectEquals(r.home->getPageIndex(), 1, "the screen's own page stays");
                expect(r.mgr.getPadFrame(ledsNow) == source, "the pad lights are the key's own, as before");
                for (size_t p = 0; p < 16; ++p)
                    expect(ledsNow[p].r == ledsBefore[p].r && ledsNow[p].g == ledsBefore[p].g && ledsNow[p].b == ledsBefore[p].b);
            }
            expect(pictures[0] != pictures[1] && pictures[1] != pictures[2] && pictures[0] != pictures[2], "three different cards");
            r.tap(BANK_UP);
            expectEquals(r.mgr.getHelpCardPage(key), count - 1, "back from the first is the last");
            expectEquals(r.card(key, count - 1).pages, juce::String("3/3"));
            r.tap(BANK_UP);
            expectEquals(r.mgr.getHelpCardPage(key), 1);
            r.hold(key, false);
            expectEquals(r.home->getPageIndex(), 1);
            expectEquals(r.allText(), state, "nothing else happened");

            // The page is remembered for the next hold of the same key ...
            r.hold(key, true);
            r.wait();
            expectEquals(r.mgr.getHelpCardPage(key), 1);
            expect(r.picture() == pictures[1]);
            r.hold(key, false);
            // ... the other key has its own ...
            const Key other = key == Key::Shift ? Key::Func : Key::Shift;
            expectEquals(r.mgr.getHelpCardPage(other), 0);
            // ... and another screen starts at the first card again.
            r.mgr.setActiveScreen(STEP);
            r.hold(key, true);
            r.wait();
            expectEquals(r.mgr.getHelpCardPage(key), 0);
            r.hold(key, false);
        }

        beginTest("Held alone: keys and pressure leave the card, the other modifier and other holds do not give one");
        {
            Rig r;
            r.shift(true);
            r.wait();
            r.mgr.noteOnReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 60, 100);
            r.mgr.noteOffReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 60);
            r.mgr.controlChangeReceived(1, 1, 64);          // the mod strip
            expect(r.showing(Key::Shift), "a key or a strip does not dismiss");
            r.func(true);
            expect(r.mgr.getHelpCardKey() == Key::None, "Func joins: Shift's card goes");
            r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None, "and Func gets none (it was not pressed alone)");
            r.shift(false);
            r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.func(false);

            r.press(PATTERN_KEY);
            r.func(true);                                   // (> held: Func is not alone)
            r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None);
            r.func(false);
            r.release(PATTERN_KEY);
            r.press(UP);
            r.shift(true);
            r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None, "Up / Down held: no card");
            r.shift(false);
            r.release(UP);

            // A held pad does not stand in the way (the EDIT card of a held step), and the audit's
            // held-step pass - a pad down, a second passing, no modifier - never sees a card.
            r.mgr.setActiveScreen(STEP);
            r.padDown(2);
            r.wait(1000);
            expect(r.mgr.getHelpCardKey() == Key::None);
            expect(r.steps->isDetailActive());
            r.func(true);
            r.wait();
            expect(r.showing(Key::Func));
            r.mgr.aftertouchReceived(1, Rig::noteOf(2), 90);
            expect(r.showing(Key::Func), "pressure on the held pad does not dismiss");
            r.func(false);
            r.padUp(2);
        }

        beginTest("SHIFT + PADS: the map says where each pad goes; the pin line says what the hold does");
        {
            Rig r;
            const auto card = r.card(Key::Shift, 0);
            expectEquals(card.title, juce::String("SHIFT + PADS"));
            expectEquals((int) card.padRows.size(), 2);
            juce::StringArray top, bottom;
            for (auto& l : card.padRows[0].labels) top.add(l.text);
            for (auto& l : card.padRows[1].labels) bottom.add(l.text);
            expectEquals(top.joinIntoString(" "), juce::String("PARM TRKS TKFX STEP PRST SONG SETT ATRT"));
            expectEquals(bottom.joinIntoString(" ").trim(), juce::String("STEP DRUM ARP SCAL WAVE FLTR"));
            expectEquals(card.lines[5].text, juce::String("COPY CLEAR PASTE:FUNC"));
            expect(card.lines[4].text.isEmpty(), "TRACKS cannot be pinned");

            for (int pad = 0; pad < 16; ++pad)
            {
                const auto before = r.card(Key::Shift, 0);
                const auto& label = before.padRows[(size_t) (pad / 8)].labels[(size_t) (pad % 8)];
                const int was = r.mgr.getActiveIndex();
                r.shift(true);
                r.wait();
                r.pad(pad);
                r.shift(false);
                if (label.text.isEmpty())
                    expectEquals(r.mgr.getActiveIndex(), was, "a pad without a name does nothing");
                else
                    expectEquals(juce::String(mapNames[r.mgr.getActiveIndex()]), label.text, "pad " + juce::String(pad + 1));
                // The screen you are on is the one inverted (STEP is on two pads).
                const auto now = r.card(Key::Shift, 0);
                for (int p = 0; p < 16; ++p)
                {
                    const auto& l = now.padRows[(size_t) (p / 8)].labels[(size_t) (p % 8)];
                    expect(l.inverted == (l.text.isNotEmpty() && r.mgr.getShiftPadScreen(p) == r.mgr.getActiveIndex()));
                }
            }

            // DRUMS: pin and unpin, as the card says.
            r.mgr.setActiveScreen(DRUMS);
            expectEquals(r.card(Key::Shift, 0).lines[4].text, juce::String("HOLD THIS PAD: PIN"));
            r.shift(true);
            r.padDown(9);
            UiClock::advance(600);
            r.padUp(9);
            r.shift(false);
            expectEquals(r.mgr.getPinnedScreen(), (int) DRUMS);
            auto pinned = r.card(Key::Shift, 0);
            expectEquals(pinned.lines[4].text, juce::String("HOLD THIS PAD: UNPIN"));
            expect(pinned.padRows[1].labels[1].marked, "the pinned screen's pad carries the mark");
            expect(!pinned.padRows[0].labels[1].marked);
            r.mgr.setActiveScreen(PARAM);
            expectEquals(r.card(Key::Shift, 0).lines[4].text, juce::String("* = ITS PADS PINNED"));
            expectEquals(r.card(Key::Func, 1).lines[4].text, juce::String("PIN: OFF WHILE HELD"));
            r.mgr.setActiveScreen(SCALE);
            expectEquals(r.card(Key::Shift, 0).lines[4].text, juce::String("HOLD THIS PAD: PIN"), "SCALE would take the pin over");
            r.mgr.setActiveScreen(DRUMS);
            r.shift(true);
            r.padDown(9);
            UiClock::advance(600);
            r.padUp(9);
            r.shift(false);
            expectEquals(r.mgr.getPinnedScreen(), -1);
            expectEquals(r.card(Key::Shift, 0).lines[4].text, juce::String("HOLD THIS PAD: PIN"));
        }

        beginTest("SHIFT + KNOBS: the curve line is there exactly where Shift + turn bends a stage");
        {
            Rig r;
            expect(r.card(Key::Shift, 2).lines[0].text.startsWith("KNOB"));
            expect(r.card(Key::Shift, 2).lines[2].text.isEmpty(), "TRACKS has no envelope");
            r.mgr.setActiveScreen(PARAM);
            int with = 0, without = 0;
            for (int page = 0; page < r.param->getPageCount(); ++page)
            {
                r.param->setCurrentPageIndex(page);
                r.shift(true);
                bool bends = false;
                for (int e = 0; e < 8; ++e)
                    bends = bends || r.param->getCurveTurnId(e) >= 0;
                const bool line = r.card(Key::Shift, 2).lines[2].text == "ATK DEC REL: CURVE";
                expect(line == bends, "page " + juce::String(page + 1));
                r.shift(false);
                (bends ? with : without)++;
            }
            expect(with > 0 && without > 0);

            // A held step's lock pages on STEP say the same as PARAM's.
            r.mgr.setActiveScreen(STEP);
            r.padDown(0);
            r.wait(1000);
            expectEquals(r.mgr.composeNameBar().left, juce::String("MAIN T1"));
            expectEquals(r.card(Key::Shift, 2).lines[2].text, juce::String("ATK DEC REL: CURVE"));
            r.tap(BANK_UP);                                 // TRIG
            expect(r.card(Key::Shift, 2).lines[2].text.isEmpty());
            r.padUp(0);
        }

        beginTest("FUNC + PADS and FUNC + KNOBS ETC say what this screen does");
        {
            Rig r;
            // TRACKS: five page keys with the pages' names, solo on the bottom row, first / last page.
            auto pads = r.card(Key::Func, 1);
            expectEquals(pads.lines[0].text, juce::String("TOP ROW: GO TO PAGE"));
            expectEquals((int) pads.padRows.size(), 1);
            juce::StringArray names;
            for (auto& l : pads.padRows[0].labels) names.add(l.text);
            expectEquals(names.joinIntoString(" ").trim(), juce::String("VOL PAN SND1 SND2 ENG"));
            expectEquals(pads.lines[3].text, juce::String("BOTTOM ROW: SOLO"));
            for (int k = 4; k >= 0; --k)
            {
                r.func(true);
                r.wait();
                r.pad(k);
                r.func(false);
                expectEquals(r.home->getPageIndex(), k);
                const auto now = r.card(Key::Func, 1);
                for (int i = 0; i < 8; ++i)
                    expect(now.padRows[0].labels[(size_t) i].inverted == (i == k), "the page you are on is inverted");
            }
            r.func(true);
            r.wait();
            r.pad(8 + 3);
            r.func(false);
            expect(r.track(3).soloed, "Func + bottom pad 4 on TRACKS solos track 4");
            auto knobs = r.card(Key::Func, 2);
            expectEquals(knobs.lines[0].text, juce::String("SETTINGS TAP TEMPO"));
            expectEquals(knobs.lines[1].text, juce::String("KNOB     COARSE STEPS"));
            expectEquals(knobs.lines[2].text, juce::String("BANK     FIRST / LAST"));
            expect(knobs.lines[4].text.isEmpty(), "no pressure ranges on TRACKS");

            // PARAM: one key per section; each goes where its name says; pressure range; section jump.
            r.mgr.setActiveScreen(PARAM);
            pads = r.card(Key::Func, 1);
            const int keys = r.param->getPageKeyCount();
            expect(keys > 1);
            for (int k = 0; k < keys; ++k)
            {
                expect(pads.padRows[0].labels[(size_t) k].text.isNotEmpty());
                expect(r.param->getPageKeyName(k).startsWith(pads.padRows[0].labels[(size_t) k].text.substring(0, 1)));
                r.func(true);
                r.pad(k);
                r.func(false);
                expectEquals(r.param->getActivePageKey(), k);
                expect(r.card(Key::Func, 1).padRows[0].labels[(size_t) k].inverted);
            }
            expectEquals(pads.lines[3].text, juce::String("BOTTOM ROW: NO CHANGE"));
            knobs = r.card(Key::Func, 2);
            expectEquals(knobs.lines[2].text, juce::String("BANK     SECTION JUMP"));
            expectEquals(knobs.lines[4].text, juce::String("UP DOWN  PRESSURE"));

            // DRUMS: no page keys, bank is the plain knob page, no ranges.
            r.mgr.setActiveScreen(DRUMS);
            pads = r.card(Key::Func, 1);
            expectEquals(pads.lines[0].text, juce::String("PADS: AS WITHOUT FUNC"));
            expect(pads.padRows.empty());
            knobs = r.card(Key::Func, 2);
            expectEquals(knobs.lines[2].text, juce::String("BANK     KNOB PAGE"));
            expect(knobs.lines[4].text.isEmpty());

            // SETTINGS and TRK FX name their page keys too (shortened to four letters).
            r.mgr.setActiveScreen(SETTINGS);
            names.clear();
            pads = r.card(Key::Func, 1);
            for (auto& l : pads.padRows[0].labels) names.add(l.text);
            expectEquals(names.joinIntoString(" ").trim(), juce::String("SETP MSTR PROJ CLCK"));
            r.mgr.setActiveScreen(TRKFX);
            names.clear();
            pads = r.card(Key::Func, 1);
            for (auto& l : pads.padRows[0].labels) names.add(l.text);
            expectEquals(names.joinIntoString(" ").trim(), juce::String("TRCK ARPL DIRT TALK MDFX FX"));
        }

        beginTest("FUNC: EDIT says what copy / clear / paste act on, and that is what they then do (13 screens, with and without a held step)");
        {
            // What the PLAY line must name on each screen: no step held / pad 1 held.
            static const char* const expected[NUM_SCREENS][2] = {
                { "CLEAR PAT 1", "CLEAR PAT 1" },               // TRACKS (pad 1 selects track 1 there); held: "... HOLD"
                { "CLEAR T1 STEPS",  "CLEAR STEP 1" },          // STEP
                { "RESET T1 ENGINE", "CLEAR S1 LOCKS" },        // PARAM
                { "", "" }, { "", "" }, { "", "" },             // DRUMS PRESET SETTINGS
                { "RESET T1 ENGINE", "CLEAR S1 LOCKS" },        // FLTR: as PARAM
                { "", "" }, { "", "" }, { "", "" }, { "", "" }, { "", "" },     // ARP SCALE SONG AT ROUTE WAVE
                { "CLEAR T1 FX",     "CLEAR S1 LOCKS" } };      // TRK FX: the track's effects

            for (int screen = 0; screen < NUM_SCREENS; ++screen)
            {
                for (int held = 0; held < 2; ++held)
                {
                    const juce::String where = juce::String(screenNames[screen]) + (held ? " + step" : "");
                    Rig r;
                    r.fill();
                    r.mgr.setActiveScreen(screen);
                    if (held)
                    {
                        r.padDown(0);
                        r.wait(1000);
                    }
                    const auto original = r.allText();

                    // ---- The card, before anything is done ----
                    r.func(true);
                    r.wait();
                    expect(r.showing(Key::Func), where + ": the card shows (a held pad does not prevent it)");
                    const auto card = r.card(Key::Func, 0);
                    r.func(false);
                    expectEquals(card.title, juce::String("FUNC: EDIT"));
                    expectEquals(cardFault(card), juce::String(), where);

                    const bool nothing = card.lines[0].text == "COPY CLEAR PASTE:";
                    const juce::String playLine = nothing ? juce::String() : card.lines[1].text;
                    const juce::String verb = playLine.substring(5, 11).trim();
                    // A whole pattern says that its button must be held.
                    const bool mustHold = playLine.endsWith(" HOLD");
                    const juce::String name = playLine.substring(11).upToLastOccurrenceOf(mustHold ? " HOLD" : "~", false, false).trim();
                    expectEquals(nothing ? juce::String() : verb + " " + name, juce::String(expected[screen][held]), where);
                    expect(mustHold == (screen == TRACKS), where + ": only a whole pattern is held");
                    expect(r.mgr.getEditPlan(Edit::Clear).hold == mustHold, where);
                    if (nothing)
                    {
                        expectEquals(card.lines[1].text, juce::String("NOT ON THIS SCREEN"), where);
                        for (const Edit e : { Edit::Copy, Edit::Clear, Edit::Paste })
                            expect(r.mgr.getEditPlan(e).kind == ScreenManager::EditPlan::Kind::Nothing, where);
                    }
                    else
                    {
                        expect(card.lines[0].text.startsWith("REC  COPY  "), where + ": " + card.lines[0].text);
                        expect(playLine.startsWith("PLAY "), where);
                        expect(!card.lines[0].inverted && !card.lines[1].inverted && !card.lines[2].inverted, where);
                    }

                    // ---- Paste with nothing copied: the card says so, and nothing changes ----
                    // (The lock clipboard is one for all the cell screens and outlives a rig, so for
                    // locks "nothing copied yet" cannot be arranged here.)
                    if (name != "S1 LOCKS")
                    {
                        if (!nothing)
                            expectEquals(card.lines[2].text, juce::String(">    PASTE (EMPTY)"), where + ": nothing copied yet");
                        r.chord(Key::Func, PATTERN_KEY);
                        expectEquals(r.allText(), original, where + ": empty paste");
                        const int room = 20 - r.mgr.getActiveScreen()->getName().length() - 1;
                        juce::String said;
                        for (auto& wording : r.mgr.getEditPlan(Edit::Paste).wordings())
                            if (said.isEmpty() && wording.length() <= room) said = wording;
                        expectEquals(r.text(), said, where);
                        expect(said == (nothing ? "NO PASTE HERE" : "NOTHING COPIED") || room < 14, where + ": " + said);
                        expect(said.startsWith("NO"), where + ": " + said);
                        r.somethingElse();
                    }

                    // ---- Copy: nothing changes; the card then offers the undo, until something else is used ----
                    r.chord(Key::Func, RECORD);
                    expectEquals(r.allText(), original, where + ": copy changes nothing");
                    expect(!r.seq.isRecording(), where);
                    if (nothing)
                        expect(r.text().startsWith("NO COPY"), where + ": " + r.text());
                    else
                    {
                        expect(r.text().startsWith("COPY "), where + ": " + r.text());
                        auto after = r.card(Key::Func, 0);
                        expectEquals(after.lines[0].text, juce::String("REC  UNDO  COPY"), where);
                        expect(after.lines[0].inverted, where + ": the undo line is inverted");
                        expect(!after.lines[2].text.contains("EMPTY"), where + ": paste now has something");
                        r.somethingElse();
                        after = r.card(Key::Func, 0);
                        expect(after.lines[0].text.startsWith("REC  COPY  ") && !after.lines[0].inverted, where + ": undo line gone");
                    }

                    // ---- Clear: exactly what the card named ----
                    r.chord(Key::Func, PLAY);
                    expect(!r.seq.isPlaying(), where);
                    if (nothing)
                    {
                        expectEquals(r.allText(), original, where + ": no clear here");
                        expect(r.text().startsWith("NO CLEAR"), where + ": " + r.text());
                    }
                    else
                    {
                        expect(r.text().startsWith(verb + " "), where + ": the confirmation names it: " + r.text());
                        expect(r.allText() != original, where + ": something was cleared");
                        juce::String others;
                        for (int t = 1; t < r.seq.getPattern().getTrackCount(); ++t) others << r.stepsText(t);
                        if (name == "T1 STEPS")
                        {
                            expectEquals(r.stepsText(0), juce::String(), where);
                            expect(others.isNotEmpty(), where + ": only the selected track");
                            expect(original.endsWith(r.soundText()), where);
                        }
                        else if (name == "PAT 1")
                        {
                            expectEquals(r.stepsText(0) + others, juce::String(), where);
                            expect(original.endsWith(r.soundText()), where);
                        }
                        else if (name == "T1 FX")
                        {
                            expectWithinAbsoluteError(r.audio.getTrackParameter(0, TrackFxParam::DIRT_BASE), 0.0f, 1.0e-6f, where + ": the effect is off");
                            expectWithinAbsoluteError(r.audio.getTrackParameter(0, r.moved.id), r.movedValue, 1.0e-4f, where + ": the engine stays");
                            expectWithinAbsoluteError(r.audio.getTrackParameter(0, MixParam::VOLUME), 0.4f, 1.0e-4f, where + ": the mixer stays");
                            expect(original.startsWith("T0: " + r.stepsText(0)), where + ": the steps stay");
                        }
                        else if (name == "T1 ENGINE")
                        {
                            expectEquals(verb, juce::String("RESET"), where);
                            expectWithinAbsoluteError(r.audio.getTrackParameter(0, r.moved.id), r.movedDefault, 1.0e-4f, where);
                            expectWithinAbsoluteError(r.audio.getTrackParameter(0, MixParam::VOLUME), 0.4f, 1.0e-4f, where + ": the mixer stays");
                            expectWithinAbsoluteError(r.audio.getTrackParameter(0, TrackFxParam::DIRT_BASE), 0.6f, 1.0e-6f, where + ": the effects stay");
                            expect(original.startsWith("T0: " + r.stepsText(0)), where + ": the steps stay");
                        }
                        else if (name == "STEP 1")
                        {
                            expectEquals(r.stepText(0, 0), juce::String(), where);
                            expectEquals(r.stepText(0, 3), juce::String("3t1 "), where + ": the other steps stay");
                            expect(original.endsWith(r.soundText()), where);
                        }
                        else if (name == "S1 LOCKS")
                        {
                            expectEquals(r.stepText(0, 0), juce::String("0t0 "), where + ": the locks go, the trig stays");
                            expectEquals(r.stepText(0, 3), juce::String("3t1 "), where);
                        }
                        else
                            expect(false, where + ": a scope this test does not know: " + name);

                        // The card offers the undo of exactly that, and the same again undoes it.
                        const auto after = r.card(Key::Func, 0);
                        expect(after.lines[1].text.startsWith("PLAY UNDO  " + verb), where + ": " + after.lines[1].text);
                        expect(after.lines[1].inverted && !after.lines[0].inverted, where);
                        expectEquals(after.lines[4].text, "UNDO: " + r.mgr.getEditPlan(Edit::Undo).nameWithin(15), where);
                        expect(after.lines[4].text.startsWith("UNDO: " + verb + " "), where + ": " + after.lines[4].text);
                        r.chord(Key::Func, PLAY);
                        // (The undo's confirmation may take the whole name bar.)
                        expect(r.text().startsWith("UNDO " + verb + " "), where + ": " + r.text());
                        expectEquals(r.allText(), original, where + ": undone");
                        expect(!r.card(Key::Func, 0).lines[1].inverted, where);

                        // ---- Clear again, then paste what was copied: the original is back ----
                        r.chord(Key::Func, PLAY);
                        expect(r.allText() != original, where);
                        r.somethingElse();
                        r.chord(Key::Func, PATTERN_KEY);
                        expect(!r.mgr.isPatternKeyHeld(), where);
                        expect(r.text().startsWith("PASTE "), where + ": " + r.text());
                        expectEquals(r.allText(), original, where + ": paste brings back what copy took");
                    }
                    if (held)
                        r.padUp(0);
                }
            }
        }

        beginTest("An empty paste or a screen without the chord leaves an armed undo alone");
        {
            Rig r;
            r.fill();
            r.mgr.setActiveScreen(STEP);
            const auto original = r.allText();
            r.func(true);
            r.tap(PLAY);
            expectEquals(r.stepsText(0), juce::String());
            r.tap(PATTERN_KEY);                             // nothing copied
            expectEquals(r.text(), juce::String("NOTHING COPIED"));
            expectEquals(r.card(Key::Func, 0).lines[1].text, juce::String("PLAY UNDO  CLEAR"));
            r.tap(PLAY);
            expectEquals(r.text(), juce::String("UNDO CLEAR T1 STEPS"));
            expectEquals(r.allText(), original);
            r.func(false);
        }

        beginTest("Where the chord does nothing the name bar says so, in the longest wording that fits");
        {
            Rig r;
            r.mgr.setActiveScreen(PRESET);
            r.func(true);
            r.tap(PLAY);
            expectEquals(r.text(), juce::String("NO CLEAR HERE"));
            r.tap(RECORD);
            expectEquals(r.text(), juce::String("NO COPY HERE"));
            r.tap(PATTERN_KEY);
            expectEquals(r.text(), juce::String("NO PASTE HERE"));
            r.func(false);
            r.mgr.setActiveScreen(ATROUTE);
            r.func(true);
            r.tap(PLAY);
            expectEquals(r.text(), juce::String("NO CLEAR"), "AT ROUTE leaves room for the short wording");
            r.func(false);

            // Long numbers: the name that fits the card's ten characters.
            ScreenManager::EditPlan plan;
            plan.names = { "S1024 LOCKS", "S1024 LCKS", "S1024" };
            expectEquals(plan.nameWithin(10), juce::String("S1024 LCKS"));
            r.mgr.setActiveScreen(TRACKS);
            r.seq.selectPattern(15, SequencerCore::ChangeMode::Immediate);
            expectEquals(r.card(Key::Func, 0).lines[1].text, juce::String("PLAY CLEAR P16 HOLD"));
        }

        beginTest("No card is clipped or overlaps: every screen, every card, with and without a held step, pinned, undo armed");
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
                    // Pads pinned, the last track, a long track on its last steps, an undo armed.
                    r.mgr.setActiveScreen(DRUMS);
                    r.shift(true); r.padDown(9); UiClock::advance(600); r.padUp(9); r.shift(false);
                    r.seq.setSelectedTrack(r.seq.getPattern().getTrackCount() - 1);
                }
                for (int screen = 0; screen < NUM_SCREENS; ++screen)
                {
                    r.mgr.setActiveScreen(screen);
                    if (variant == 2)
                    {
                        r.func(true); r.tap(PLAY); r.func(false);
                    }
                    if (variant >= 1)
                    {
                        r.padDown(0);
                        r.wait(1000);
                    }
                    for (const Key key : { Key::Shift, Key::Func })
                    {
                        r.hold(key, true);
                        r.wait();
                        expect(r.showing(key), juce::String(screenNames[screen]));
                        for (int page = 0; page < r.mgr.getHelpCardCount(key); ++page)
                        {
                            const juce::String where = juce::String(screenNames[screen]) + " variant " + juce::String(variant)
                                                     + (key == Key::Shift ? " Shift " : " Func ") + juce::String(page + 1);
                            expectEquals(r.mgr.getHelpCardPage(key), page, where);
                            expectEquals(cardFault(r.card(key, page)), juce::String(), where);
                            r.tick();
                            expect(!drawing.overText.any(), where + ": text and other drawing share pixels");
                            expect(!drawing.clipped.any(), where + ": text runs off the glass");
                            ++cards;
                            r.tap(BANK_DOWN);
                        }
                        r.hold(key, false);
                    }
                    if (variant >= 1)
                        r.padUp(0);
                }
                r.display.setDrawAudit(nullptr);
            }
            expectEquals(cards, 3 * NUM_SCREENS * 6);
        }

        beginTest("The screen audit's help pass: every card on every screen, nothing clipped, overlapping or left behind");
        {
            Rig r;
            r.fill();
            const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                    .getChildFile("neptune-help-audit-" + juce::String(juce::Random::getSystemRandom().nextInt(1000000)));
            ScreenAudit audit(r.mgr, r.display);
            // Only the screens whose knob pass is quick and harmless here; the help pass is the same on all.
            for (int screen : { (int) TRACKS, (int) STEP, (int) PARAM, (int) FLTR, (int) DRUMS, (int) SETTINGS, (int) TRKFX })
                audit.targets.push_back({ screen, screenNames[screen], false,
                                          screen == STEP || screen == PARAM || screen == FLTR || screen == TRKFX });
            const auto totals = audit.run(folder);
            const auto report = folder.getChildFile("report.txt").loadFileAsString();
            folder.deleteRecursively();

            // Shift 3, Func 3, > 2, Up / Down 2, Record 1 (tests/help_card_layers_tests.h has the last three).
            expectEquals(totals.helpCards, (7 + 4) * 11, "eleven cards on each visit");
            juce::StringArray helpFindings;
            for (auto& line : juce::StringArray::fromLines(report))
                if (line.contains(" help"))
                    if (!line.contains("help cards,"))
                        helpFindings.add(line);
            expectEquals(helpFindings.joinIntoString("\n"), juce::String(), "no finding names a help card");
            expectEquals(totals.overlap, 0, report);
            expectEquals(totals.clipped, 0, report);
            expectEquals(totals.paging, 0, report);
            expect(r.mgr.getHelpCardKey() == Key::None && !r.mgr.isShiftHeld() && !r.mgr.isFuncHeld());
            expect(!r.mgr.isPatternKeyHeld() && !r.mgr.isTrackLayerHeld() && !r.mgr.isRecordArmed(), "the audit left no key down and recording off");
        }

        // NEPTUNE_HELP_DUMP=<file>: every card of every screen as text (for the notes and the manual).
        if (const auto dump = juce::SystemStats::getEnvironmentVariable("NEPTUNE_HELP_DUMP", {}); dump.isNotEmpty())
        {
            juce::String out;
            juce::File(dump).getSiblingFile("cards").createDirectory();
            for (int held = 0; held < 2; ++held)
                for (int screen = 0; screen < NUM_SCREENS; ++screen)
                {
                    Rig r;
                    r.fill();
                    r.mgr.setActiveScreen(screen);
                    if (held) { r.padDown(0); r.wait(1000); }
                    for (const Key key : { Key::Shift, Key::Func })
                    {
                        r.hold(key, true);
                        r.wait();
                        for (int page = 0; page < r.mgr.getHelpCardCount(key); ++page)
                        {
                            const juce::String label = juce::String(screenNames[screen]) + (held ? " + pad 1 held" : "")
                                                     + (key == Key::Shift ? " Shift " : " Func ") + juce::String(page + 1);
                            out << "== " << label << "\n" << mockup(r.card(key, page)) << "\n";
                            juce::File(dump).getSiblingFile("cards").getChildFile(label + ".pbm").replaceWithText(pbm(r.picture()));
                            r.tap(BANK_DOWN);
                        }
                        r.hold(key, false);
                    }
                    if (held) r.padUp(0);
                }
            juce::File(dump).replaceWithText(out);
        }
    }

    // A card as text. Lines are the display's 21 columns; a line of pad labels is shown with six
    // columns per pad (on the glass each label sits over its pad's column); [x] = inverted.
    static juce::String mockup(const helptest::Card& c)
    {
        juce::StringArray rows;
        rows.add(c.title.paddedRight(' ', 21 - c.pages.length()) + c.pages);
        for (auto& line : c.lines)
            rows.add(line.inverted ? "[" + line.text + "]" : line.text);
        for (auto& row : c.padRows)
            for (int half = 0; half < 2; ++half)
            {
                juce::String text;
                for (int pad = 0; pad < 8; ++pad)
                {
                    const auto& l = row.labels[(size_t) pad];
                    juce::String shown;
                    if (pad % 2 == half && l.text.isNotEmpty())
                        shown = (l.inverted ? "[" : " ") + l.text + (l.marked ? "*" : "") + (l.inverted ? "]" : "");
                    text << shown.paddedRight(' ', 6);
                }
                rows.set(1 + row.line + half, text.trimEnd());
            }
        return "  |" + rows.joinIntoString("\n  |") + "\n";
    }

    static juce::String pbm(const DisplayRenderer::PixelBuffer& px)
    {
        juce::String out = "P1\n128 64\n";
        for (int y = 0; y < 64; ++y)
        {
            for (int x = 0; x < 128; ++x)
                out << (px[(size_t) y][(size_t) x] ? "1 " : "0 ");
            out << "\n";
        }
        return out;
    }
};

static HelpCardTests helpCardTests;
