#pragma once
#include <juce_core/juce_core.h>
#include <map>
#include "../src/controller/KeyboardSplit.h"
#include "../src/ui/ScreenManager.h"
#include "../src/ui/GridScreen.h"
#include "../src/ui/GrammarHooks.h"
#include "../src/ui/screens/HomeScreen.h"
#include "../src/controller/PadManager.h"
#include "../src/engine/AudioEngine.h"
#include "../src/arpeggiator/Arpeggiator.h"

// The control grammar (docs/launchkey-mk4-control-map.md, "CONTROL GRAMMAR"), driven the way the hardware
// drives it: MIDI-level events into ScreenManager, with small screens that record what reached them.
namespace cgtest
{
    // Records every call with the modifiers it saw: "enc0", "enc0 S" (Shift), "pad3 F" (Func).
    class Rec : public Screen
    {
    public:
        explicit Rec(juce::String n) : name(std::move(n)) {}

        juce::String name, sub, readout;
        juce::StringArray log;
        bool stepHeld = false, ownColours = false, handlesEdits = false, bar = true;
        int pageKeys = 0, activeKey = -1, pages = 1, pageIdx = 0;
        PadColour colour { 1, 2, 3 };
        TurnMode lastTurn = TurnMode::Plain;

        void add(const juce::String& what) { log.add(what + (shiftDown() ? " S" : "") + (funcDown() ? " F" : "")); }
        bool saw(const juce::String& what) const { return log.contains(what); }

        juce::String getName() const override { return name; }
        juce::String getSubPage() const override { return sub; }
        void render(DisplayRenderer&) override {}
        void onEncoderChange(int i, int) override { lastTurn = turnMode(); add("enc" + juce::String(i)); }
        void onEncoderChangeAllTracks(int i, int) override { add("all" + juce::String(i)); }
        void onEncoderLockClear(int i) override { add("unlock" + juce::String(i)); }
        void onPadPress(int p, int) override { add("pad" + juce::String(p)); }
        void onPadRelease(int p) override { add("rel" + juce::String(p)); }
        void onPadAftertouch(int p, int) override { add("at" + juce::String(p)); }
        void onArrowLeft() override { add("left"); }
        void onArrowRight() override { add("right"); }
        void onSectionPrev() override { add("secPrev"); }
        void onSectionNext() override { add("secNext"); }
        void onPadPagePrev() override { add("padPrev"); }
        void onPadPageNext() override { add("padNext"); }
        void onFuncDown() override { add("funcDown"); }
        void onFuncUp() override { add("funcUp"); }
        int getPageKeyCount() const override { return pageKeys; }
        void onPageKey(int i) override { activeKey = i; add("key" + juce::String(i)); }
        int getActivePageKey() const override { return activeKey; }
        bool getPadColours(std::array<PadColour, 16>& out) const override
        {
            if (!ownColours) return false;
            out.fill(colour);
            return true;
        }
        juce::String getEncoderReadout(int i) const override { return readout.isEmpty() ? readout : readout + juce::String(i + 1); }
        bool onCopy() override  { if (handlesEdits) add("copy");  return handlesEdits; }
        bool onClear() override { if (handlesEdits) add("clear"); return handlesEdits; }
        bool onPaste() override { if (handlesEdits) add("paste"); return handlesEdits; }
        bool getEditScope(EditScope& scope) const override
        {
            if (handlesEdits) scope.names = { "OWN" };
            return handlesEdits;
        }
        bool isStepHeld() const override { return stepHeld; }
        bool usesEncoderBar() const override { return bar; }
        int getPageCount() const override { return pages; }
        int getPageIndex() const override { return pageIdx; }
    };

    struct Keys : ScreenManager::KeyboardListener
    {
        juce::StringArray log;
        void keyboardNoteOn(int note, int) override { log.add("on" + juce::String(note)); }
        void keyboardNoteOff(int note) override { log.add("off" + juce::String(note)); }
    };

    // A sequencer, a manager on a test clock and two recording screens ("A" at 0, "B" at 1).
    struct Rig
    {
        SequencerCore seq;
        ScreenManager mgr;
        juce::int64 now = 0;
        Rec* a = nullptr;
        Rec* b = nullptr;

        explicit Rig(const juce::String& nameA = "A", const juce::String& nameB = "B")
        {
            seq.prepareToPlay(44100.0, 512);
            mgr.setSequencer(&seq);
            mgr.setClock([this] { return now; });
            a = add(nameA);
            b = add(nameB);
            // As in the app's STEP screen: copy / clear / paste act on the selected track's steps.
            // (A screen with no context set ignores the combination; tested separately.)
            for (int i = 0; i < 2; ++i)
                if ((i == 0 ? nameA : nameB) != "TRACKS" && (i == 0 ? nameA : nameB) != "PARAM" && (i == 0 ? nameA : nameB) != "TRK FX")
                    mgr.setScreenContext(i, ScreenManager::CopyContext::Steps);
        }

        Rec* add(const juce::String& name)
        {
            auto* r = new Rec(name);
            mgr.addScreen(std::unique_ptr<Screen>(r));
            return r;
        }

        static constexpr int TOP = LaunchkeyController::PAD_TOP_ROW_START;
        static constexpr int BOTTOM = LaunchkeyController::PAD_BOTTOM_ROW_START;
        static int padNote(int pad) { return pad < 8 ? TOP + pad : BOTTOM + pad - 8; }

        void press(int cc)   { mgr.controlChangeReceived(1, cc, 127); }
        void release(int cc) { mgr.controlChangeReceived(1, cc, 0); }
        void tap(int cc)     { press(cc); release(cc); }
        void shift(bool on)  { mgr.controlChangeReceived(7, 63, on ? 127 : 0); }
        void func(bool on)   { mgr.controlChangeReceived(1, 105, on ? 127 : 0); }
        void padOn(int pad)  { mgr.noteOnReceived(1, padNote(pad), 100); }
        void padOff(int pad) { mgr.noteOffReceived(1, padNote(pad)); }
        void pad(int pad)    { padOn(pad); padOff(pad); }
        void padHeld(int pad) { padOn(pad); now += 600; padOff(pad); }      // long enough to pin / unpin
        void turn(int encoder, int clicks = 1) { mgr.controlChangeReceived(16, 0x15 + encoder, clicks < 0 ? 128 + clicks : clicks); }
        void keyOn(int note)  { mgr.noteOnReceived(LaunchkeyController::KEYS_PORT_CHANNEL, note, 100); }
        void keyOff(int note) { mgr.noteOffReceived(LaunchkeyController::KEYS_PORT_CHANNEL, note); }
        juce::String text() const { return mgr.composeNameBar().text; }
    };

    enum : int { BANK_UP = 51, BANK_DOWN = 52, PATTERN_KEY = 104, UP = 106, DOWN = 107, SETTINGS = 108,
                 ARP = 109, PLAY = 115, RECORD = 117 };
}

class ControlGrammarTests : public juce::UnitTest
{
public:
    ControlGrammarTests() : UnitTest("Control Grammar") {}

    void runTest() override
    {
        using namespace cgtest;
        using TM = Screen::TurnMode;

        beginTest("Modifiers reach the screen with every call: Shift = fine, Func = coarse");
        {
            Rig r;
            r.turn(0);
            expect(r.a->saw("enc0"));
            expect(r.a->lastTurn == TM::Plain);

            r.shift(true);
            r.turn(1);
            expect(r.a->saw("enc1 S"));
            expect(r.a->lastTurn == TM::Fine);
            r.shift(false);

            r.func(true);
            r.turn(2);
            expect(r.a->saw("enc2 F"));
            expect(r.a->lastTurn == TM::Coarse);
            r.padOn(9);                          // no page keys: Func + pad is the screen's
            expect(r.a->saw("pad9 F"));
            r.func(false);
            r.padOff(9);
            expect(r.a->saw("rel9"));            // the release reports the modifiers of its own moment

            r.turn(3);
            expect(r.a->saw("enc3"));
            expect(r.a->lastTurn == TM::Plain);
        }

        beginTest("Bank buttons page the encoders, on every screen, and nothing else");
        {
            Rig r;
            r.seq.start();
            r.tap(BANK_DOWN);
            r.tap(BANK_UP);
            expectEquals(r.a->log.joinIntoString(","), juce::String("right,left"));
            expectEquals(r.seq.getPendingPattern(), -1);
            expectEquals(r.seq.getActivePatternIndex(), 0);
            r.seq.stop();

            // Func + bank: section jump.
            r.a->log.clear();
            r.func(true);
            r.tap(BANK_DOWN);
            r.tap(BANK_UP);
            r.func(false);
            expect(r.a->saw("secNext F"));
            expect(r.a->saw("secPrev F"));
            expect(!r.a->saw("right F") && !r.a->saw("left F"));

            // While a step is held the bank buttons still page (the screen pages its locks).
            r.a->log.clear();
            r.a->stepHeld = true;
            r.tap(BANK_DOWN);
            r.tap(BANK_UP);
            expectEquals(r.a->log.joinIntoString(","), juce::String("right,left"));
        }

        beginTest("Up / Down page what the pads address, on release, only when nothing was used under them");
        {
            Rig r;
            r.press(DOWN);
            expect(r.a->log.isEmpty(), "nothing on the press");
            expect(r.mgr.isTrackLayerHeld());
            r.release(DOWN);
            expectEquals(r.a->log.joinIntoString(","), juce::String("padNext"));
            r.tap(UP);
            expectEquals(r.a->log.joinIntoString(","), juce::String("padNext,padPrev"));

            // A pad under it: no paging.
            r.a->log.clear();
            r.press(DOWN);
            r.pad(2);
            r.release(DOWN);
            expect(r.a->log.isEmpty());

            // A knob under it: no paging (the turn went to every track).
            r.press(UP);
            r.turn(4);
            r.release(UP);
            expectEquals(r.a->log.joinIntoString(","), juce::String("all4"));

            // The next plain tap pages again.
            r.a->log.clear();
            r.tap(UP);
            expectEquals(r.a->log.joinIntoString(","), juce::String("padPrev"));
        }

        beginTest("Shift + Up / Down = track -1 / +1; Func + Up / Down = the screen's pressure-range ends");
        {
            Rig r;
            int called = -1;
            r.mgr.setTrackSelectionCallback([&](int t) { called = t; });

            r.shift(true);
            r.tap(DOWN);
            expectEquals(r.seq.getSelectedTrack(), 1);
            expectEquals(r.mgr.getSelectedTrack(), 1);
            expectEquals(called, 1);
            r.tap(UP);
            r.tap(UP);
            expectEquals(r.seq.getSelectedTrack(), 7);      // wraps
            r.shift(false);
            expect(r.a->log.isEmpty(), "no paging, no layer");
            expect(!r.mgr.isTrackLayerHeld());

            // The older firmware's dedicated codes still work.
            r.press(103);
            expectEquals(r.seq.getSelectedTrack(), 0);
            r.press(102);
            expectEquals(r.seq.getSelectedTrack(), 7);

            r.func(true);
            r.tap(DOWN);
            r.tap(UP);
            r.func(false);
            expect(r.a->saw("funcDown F"));
            expect(r.a->saw("funcUp F"));
            expect(!r.a->saw("padNext") && !r.a->saw("padPrev") && !r.a->saw("padNext F"));
        }

        beginTest("Hold > + pad queues a pattern; a plain tap of > does nothing");
        {
            Rig r;
            r.tap(PATTERN_KEY);
            expect(r.a->log.isEmpty(), "a tap reaches no screen");
            expectEquals(r.seq.getActivePatternIndex(), 0);
            expectEquals(r.seq.getPendingPattern(), -1);

            // Stopped: at once.
            r.press(PATTERN_KEY);
            expect(r.mgr.isPatternKeyHeld());
            r.pad(4);
            r.release(PATTERN_KEY);
            expectEquals(r.seq.getActivePatternIndex(), 4);
            expect(r.a->log.isEmpty(), "the pad did not reach the screen, nor its release");

            // Playing: the next loop. Bottom row = patterns 9-16.
            r.seq.start();
            r.press(PATTERN_KEY);
            r.pad(11);
            r.release(PATTERN_KEY);
            expectEquals(r.seq.getActivePatternIndex(), 4);
            expectEquals(r.seq.getPendingPattern(), 11);
            r.seq.stop();

            // Works from any screen.
            r.mgr.setActiveScreen(1);
            r.press(PATTERN_KEY);
            r.pad(2);
            r.release(PATTERN_KEY);
            expectEquals(r.seq.getActivePatternIndex(), 2);
            expect(r.b->log.isEmpty());

            // Afterwards pads are the screen's again.
            r.pad(2);
            expect(r.b->saw("pad2") && r.b->saw("rel2"));
        }

        beginTest("Hold > + turn on a held step removes that knob's lock");
        {
            Rig r;
            r.press(PATTERN_KEY);
            r.turn(3);
            expectEquals(r.a->log.joinIntoString(","), juce::String("enc3"));      // no step held: a normal turn
            r.a->stepHeld = true;
            r.turn(5);
            r.release(PATTERN_KEY);
            expect(r.a->saw("unlock5"));
            expect(!r.a->saw("enc5"));
            r.turn(5);
            expect(r.a->saw("enc5"));
        }

        beginTest("Hold > + Record keeps a copy of the pattern, hold > + Play reloads it");
        {
            Rig r;
            auto& track = r.seq.getPattern().getTrack(2);
            track.getStep(3).trig = true;
            track.setLength(12);

            r.press(PATTERN_KEY);
            r.tap(RECORD);
            r.release(PATTERN_KEY);
            expect(!r.seq.isRecording(), "> + Record does not arm recording");
            expectEquals(r.text(), juce::String("KEEP P1"));

            track.getStep(3).trig = false;
            track.getStep(7).trig = true;
            track.setLength(16);
            r.seq.getPattern().swing = 0.5f;

            r.press(PATTERN_KEY);
            r.tap(PLAY);
            r.release(PATTERN_KEY);
            expect(!r.seq.isPlaying(), "> + Play does not start the transport");
            expect(track.getStep(3).trig);
            expect(!track.getStep(7).trig);
            expectEquals(track.length, 12);
            expectEquals(r.seq.getPattern().swing, 0.0f);
            expectEquals(r.text(), juce::String("RELOAD P1"));
        }

        beginTest("> + Play with no copy kept: the pattern as it was at the last project load, else nothing");
        {
            Rig r;
            auto& step = r.seq.getPattern().getTrack(0).getStep(0);

            step.trig = true;
            r.press(PATTERN_KEY);
            r.tap(PLAY);
            r.release(PATTERN_KEY);
            expect(step.trig, "nothing to reload from: unchanged");
            expectEquals(r.text(), juce::String("NO COPY"));

            r.mgr.projectLoaded();          // the state with the trig
            step.trig = false;
            r.seq.getPattern().getTrack(5).getStep(9).trig = true;
            r.press(PATTERN_KEY);
            r.tap(PLAY);
            r.release(PATTERN_KEY);
            expect(step.trig);
            expect(!r.seq.getPattern().getTrack(5).getStep(9).trig);

            // A kept copy is per pattern and wins over the loaded state.
            step.trig = false;
            r.press(PATTERN_KEY);
            r.tap(RECORD);
            r.release(PATTERN_KEY);
            step.trig = true;
            r.press(PATTERN_KEY);
            r.tap(PLAY);
            r.release(PATTERN_KEY);
            expect(!step.trig);
        }

        beginTest("Track layer (hold Up or Down): top pad selects, bottom pad mutes, a knob changes every track");
        {
            Rig r;
            r.mgr.setActiveScreen(1);       // from any screen
            r.press(DOWN);
            r.pad(3);
            expectEquals(r.seq.getSelectedTrack(), 3);
            r.pad(8 + 5);
            expect(r.seq.getPattern().getTrack(5).muted);
            expect(!r.seq.getPattern().getTrack(3).muted);
            r.pad(8 + 5);
            expect(!r.seq.getPattern().getTrack(5).muted);
            r.turn(6);
            r.release(DOWN);
            expectEquals(r.b->log.joinIntoString(","), juce::String("all6"));

            r.press(UP);                    // either arrow
            r.pad(8 + 1);
            r.release(UP);
            expect(r.seq.getPattern().getTrack(1).muted);
            expectEquals(r.b->log.joinIntoString(","), juce::String("all6"));
        }

        beginTest("Func + top-row pad = page keys on screens that have them");
        {
            Rig r;
            r.a->pageKeys = 3;
            r.func(true);
            r.pad(1);
            expect(r.a->saw("key1 F"));
            expect(!r.a->saw("pad1 F") && !r.a->saw("rel1 F"));
            r.pad(5);                           // beyond the screen's keys: nothing
            expect(!r.a->saw("key5 F") && !r.a->saw("pad5 F"));
            r.pad(8 + 2);                       // bottom row stays with the screen
            expect(r.a->saw("pad10 F"));
            r.func(false);

            // A screen without page keys gets Func + top pad as a pad.
            r.mgr.setActiveScreen(1);
            r.func(true);
            r.pad(1);
            r.func(false);
            expect(r.b->saw("pad1 F"));
        }

        beginTest("Shift + assigned pad opens that screen; an unassigned pad does nothing at all");
        {
            Rig r;
            r.mgr.setShiftPadScreen(0, 1);
            r.mgr.setShiftPadScreen(9, 0);
            r.shift(true);
            r.pad(0);
            expectEquals(r.mgr.getActiveIndex(), 1);
            r.pad(5);                           // unassigned
            r.pad(15);
            expectEquals(r.mgr.getActiveIndex(), 1);
            r.pad(9);
            expectEquals(r.mgr.getActiveIndex(), 0);
            r.shift(false);
            for (auto* s : { r.a, r.b })
                for (auto& line : s->log)
                    expect(!line.startsWith("pad") && !line.startsWith("rel"), "no pad reached a screen: " + line);
        }

        beginTest("Pad pin: Shift + the screen's own bottom pad pins its pads; they keep playing elsewhere");
        {
            Rig r("A", "DRUMS");
            r.mgr.setShiftPadScreen(0, 0);
            r.mgr.setShiftPadScreen(1, 1);      // a top-row pad also opens DRUMS
            r.mgr.setShiftPadScreen(9, 1);
            r.mgr.setShiftPadScreen(8, 0);
            r.mgr.setPinnable(1, true);

            // Not on DRUMS yet: the pad just opens it.
            r.shift(true);
            r.pad(9);
            expectEquals(r.mgr.getActiveIndex(), 1);
            expectEquals(r.mgr.getPinnedScreen(), -1);
            r.pad(1);                           // its top-row pad never pins
            expectEquals(r.mgr.getPinnedScreen(), -1);
            r.pad(9);                           // already there, a quick press: nothing (too easy to do by accident)
            expectEquals(r.mgr.getPinnedScreen(), -1);
            r.padHeld(9);                       // held half a second: pin
            expectEquals(r.mgr.getPinnedScreen(), 1);
            expect(r.mgr.getNameBarTags().contains("PIN"));
            r.pad(0);                           // go to A
            r.shift(false);
            expectEquals(r.mgr.getActiveIndex(), 0);

            r.padOn(4);
            r.mgr.aftertouchReceived(1, Rig::padNote(4), 90);
            r.padOff(4);
            expectEquals(r.b->log.joinIntoString(","), juce::String("pad4,at4,rel4"));
            expect(r.a->log.isEmpty(), "the shown screen gets no pads while another is pinned");

            // Not while a layer is active: Func + pad is the shown screen's, Track layer still selects.
            r.func(true);
            r.pad(10);
            r.func(false);
            expect(r.a->saw("pad10 F"));
            r.press(DOWN);
            r.pad(2);
            r.release(DOWN);
            expectEquals(r.seq.getSelectedTrack(), 2);
            expectEquals(r.b->log.size(), 3);

            // A screen that is not pinnable: Shift + its own bottom pad is not a pin.
            r.mgr.setShiftPadScreen(9, 1);
            r.shift(true);
            r.pad(8);
            expectEquals(r.mgr.getPinnedScreen(), 1);       // still the drums
            // Back on DRUMS, the same gesture unpins.
            r.pad(9);
            expectEquals(r.mgr.getActiveIndex(), 1);
            expectEquals(r.mgr.getPinnedScreen(), 1);
            r.pad(9);
            expectEquals(r.mgr.getPinnedScreen(), 1, "a quick press does not unpin either");
            r.padHeld(9);
            expectEquals(r.mgr.getPinnedScreen(), -1);
            r.pad(0);
            r.shift(false);
            expect(!r.mgr.getNameBarTags().contains("PIN"));
            r.a->log.clear();
            r.pad(4);
            expect(r.a->saw("pad4"));
        }

        beginTest("Pad colours: hold layer, then pinned screen, then the screen's own, then the pad mode");
        {
            Rig r;
            std::array<Screen::PadColour, 16> c;
            using Src = ScreenManager::PadSource;
            r.mgr.setShiftPadScreen(0, 0);
            r.mgr.setShiftPadScreen(9, 1);
            r.mgr.setPinnable(1, true);

            expect(r.mgr.getPadFrame(c) == Src::PadMode, "a screen without colours leaves the pad mode in charge");

            r.a->ownColours = true;
            r.a->colour = { 9, 8, 7 };
            expect(r.mgr.getPadFrame(c) == Src::Screen);
            expectEquals((int) c[15].r, 9);

            // Pinned screen's colours win over the shown screen's.
            r.b->ownColours = true;
            r.b->colour = { 50, 60, 70 };
            r.shift(true);
            r.pad(9);
            r.padHeld(9);
            r.pad(0);
            r.shift(false);
            expectEquals(r.mgr.getPinnedScreen(), 1);
            expect(r.mgr.getPadFrame(c) == Src::Pinned);
            expectEquals((int) c[3].g, 60);

            // Shift: the screen map. Current bright, assigned dim, unassigned off.
            r.shift(true);
            expect(r.mgr.getPadFrame(c) == Src::Layer);
            expectEquals((int) c[0].r, 127);
            expect(c[9].r + c[9].g + c[9].b > 0, "assigned pad is lit");
            expect(c[9].r < 127, "but not as the current screen");
            expectEquals(c[5].r + c[5].g + c[5].b, 0);
            r.shift(false);

            // >: patterns. Current bright, queued blinking, with trigs dim, empty off.
            r.seq.getPattern(2).getTrack(0).getStep(0).trig = true;
            r.seq.start();
            r.press(PATTERN_KEY);
            r.pad(5);
            r.now = 0;
            expect(r.mgr.getPadFrame(c) == Src::Layer);
            expectEquals((int) c[0].g, 127);
            expect(c[5].r + c[5].g + c[5].b > 0, "queued: on");
            expect(c[2].g > 0 && c[2].g < 60, "used: dim");
            expectEquals(c[7].r + c[7].g + c[7].b, 0);
            r.now = 150;
            r.mgr.getPadFrame(c);
            expectEquals(c[5].r + c[5].g + c[5].b, 0);      // queued: blinks
            r.release(PATTERN_KEY);
            r.seq.stop();

            // Up / Down: tracks. Selected bright, muted red.
            r.seq.setSelectedTrack(4);
            r.seq.getPattern().getTrack(1).muted = true;
            r.press(UP);
            expect(r.mgr.getPadFrame(c) == Src::Layer);
            expectEquals((int) c[4].r, 127);
            expect(c[0].r > 0 && c[0].r < 60);
            expect(c[9].r == 127 && c[9].g == 0, "muted track is red");
            expect(c[8].r == 0 && c[8].g > 0);
            r.padOn(0);                                      // used: the release must not page
            r.padOff(0);
            r.release(UP);

            // Func on a screen with page keys: keys on the top row, the screen's bottom row.
            r.shift(true); r.pad(9); r.padHeld(9); r.pad(0); r.shift(false);     // unpin
            expectEquals(r.mgr.getPinnedScreen(), -1);
            r.a->pageKeys = 4;
            r.a->activeKey = 2;
            r.func(true);
            expect(r.mgr.getPadFrame(c) == Src::Layer);
            expect(c[2].r > c[0].r, "active key brighter");
            expect(c[0].r > 0);
            expectEquals(c[6].r + c[6].g + c[6].b, 0);
            expectEquals((int) c[12].r, 9);
            r.func(false);
            expect(r.mgr.getPadFrame(c) == Src::Screen);
        }

        beginTest("Pad manager sends full-colour pads only when they change");
        {
            PadManager pads;
            std::array<PadManager::RGB, 16> rgb {};
            pads.showRGB(rgb);
            expectEquals(pads.getSendCount(), 16);
            pads.showRGB(rgb);
            expectEquals(pads.getSendCount(), 16);
            rgb[3] = { 1, 2, 3 };
            rgb[9] = { 4, 5, 6 };
            pads.showRGB(rgb);
            expectEquals(pads.getSendCount(), 18);
            pads.refreshPadLEDs();              // back to the mode's palette colours: all of them
            expectEquals(pads.getSendCount(), 34);
            pads.refreshPadLEDs();
            expectEquals(pads.getSendCount(), 34);
            pads.showRGB(rgb);
            expectEquals(pads.getSendCount(), 50);
        }

        beginTest("Name bar: T n on every screen, page mark, tags, the knob readout after a turn");
        {
            Rig r;
            r.a->sub = "STATUS";
            expectEquals(r.mgr.composeNameBar().left, juce::String("A T1"));
            expectEquals(r.text(), juce::String("STATUS"));
            expect(r.mgr.composeNameBar().pages.isEmpty());

            r.shift(true); r.tap(DOWN); r.tap(DOWN); r.shift(false);
            expectEquals(r.mgr.composeNameBar().left, juce::String("A T3"));
            r.mgr.setActiveScreen(1);
            expectEquals(r.mgr.composeNameBar().left, juce::String("B T3"));
            r.mgr.setActiveScreen(0);

            r.a->pages = 5;
            r.a->pageIdx = 1;
            expectEquals(r.mgr.composeNameBar().pages, juce::String("2/5"));

            // Readout: bar screens, for 0.7 s after the turn, in place of the status text.
            r.a->readout = "CUT K";
            r.now = 1000;
            r.turn(3);
            expectEquals(r.text(), juce::String("CUT K4"));
            expectEquals(r.mgr.composeNameBar().pages, juce::String("2/5"));
            // A long one is shown whole: it pushes out the page mark, then the track number...
            r.a->readout = "CUTOFF 1.2k K";
            expectEquals(r.text(), juce::String("CUTOFF 1.2k K4"));
            expect(r.mgr.composeNameBar().pages.isEmpty());
            expectEquals(r.mgr.composeNameBar().left, juce::String("A T3"));
            r.a->readout = "FILTER CUTOFF 1.2k";
            expectEquals(r.text(), juce::String("FILTER CUTOFF 1.2k"));
            expectEquals(r.mgr.composeNameBar().left, juce::String("A"));
            // ...and only then is cut, on the right.
            r.a->readout = "FILTER CUTOFF 12.5kHz";
            expectEquals(r.text(), juce::String("FILTER CUTOFF 12.5"));
            r.a->readout = "CUT K";
            r.now = 1700;
            expectEquals(r.text(), juce::String("CUT K4"));
            r.now = 1701;
            expectEquals(r.text(), juce::String("STATUS"));

            // No readout on a grid screen, or when the screen has none for that knob.
            r.a->bar = false;
            r.turn(3);
            expectEquals(r.text(), juce::String("STATUS"));
            r.a->bar = true;
            r.a->readout = {};
            r.turn(3);
            expectEquals(r.text(), juce::String("STATUS"));

            // It draws.
            DisplayRenderer display;
            r.mgr.setDisplay(&display);
            r.mgr.requestRedraw();
            r.mgr.redrawIfNeeded();
            bool any = false;
            for (int x = 0; x < 128; ++x)
                for (int y = 0; y < 9; ++y)
                    any = any || display.getPixels()[(size_t) y][(size_t) x];
            expect(any);
        }

        beginTest("Name bar tags: ARP, LATCH, PIN, FILL; words when they fit, first letters when not");
        {
            Rig r;
            bool arp[8] = {}, latch[8] = {};
            ScreenManager::EngineHooks h;
            h.arpEnabled = [&](int t) { return arp[t]; };
            h.setArpEnabled = [&](int t, bool on) { arp[t] = on; };
            h.arpLatch = [&](int t) { return latch[t]; };
            h.setArpLatch = [&](int t, bool on) { latch[t] = on; };
            r.mgr.setEngineHooks(h);

            expect(r.mgr.getNameBarTags().isEmpty());
            arp[0] = true;
            expectEquals(r.mgr.getNameBarTags().joinIntoString(" "), juce::String("ARP"));
            r.seq.setSelectedTrack(1);
            expect(r.mgr.getNameBarTags().isEmpty(), "the tag is the selected track's arp");
            r.seq.setSelectedTrack(0);
            latch[0] = true;
            r.shift(true);
            r.press(RECORD);
            expectEquals(r.mgr.getNameBarTags().joinIntoString(" "), juce::String("ARP LATCH FILL"));
            expectEquals(r.mgr.composeNameBar().tags, juce::String("ARP LATCH FILL"));

            r.a->sub = "STATUS";                 // no room for both: letters, and the text stays whole
            expectEquals(r.mgr.composeNameBar().tags, juce::String("ALF"));
            expectEquals(r.text(), juce::String("STATUS"));
            r.release(RECORD);
            r.shift(false);
            expectEquals(r.mgr.composeNameBar().tags, juce::String("AL"));
            expectEquals(r.text(), juce::String("STATUS"));
            r.a->sub = "OK";
            expectEquals(r.mgr.composeNameBar().tags, juce::String("ARP LATCH"));
            expectEquals(r.text(), juce::String("OK"));
        }

        beginTest("Arp button: first the ARP screen, pressed again there it switches the selected track's arp");
        {
            Rig r("A", "ARP");
            bool arp[8] = {};
            ScreenManager::EngineHooks h;
            h.arpEnabled = [&](int t) { return arp[t]; };
            h.setArpEnabled = [&](int t, bool on) { arp[t] = on; };
            r.mgr.setEngineHooks(h);
            int notified = 0;
            r.mgr.setArpToggleCallback([&](bool) { ++notified; });
            r.seq.setSelectedTrack(2);

            r.tap(ARP);
            expectEquals(r.mgr.getActiveIndex(), 1);
            expect(!arp[2], "the first press only opens the screen");
            expect(!r.mgr.getNameBarTags().contains("ARP"));

            r.tap(ARP);
            expect(arp[2]);
            expect(!arp[0]);
            expect(r.mgr.isArpEnabled());
            expect(r.mgr.getNameBarTags().contains("ARP"));
            expectEquals(notified, 1);

            r.tap(ARP);
            expect(!arp[2]);

            // By index too (screen names change with their page).
            r.b->name = "MAIN";
            r.mgr.setButtonScreens(-1, 1, -1);
            r.mgr.setActiveScreen(0);
            r.tap(ARP);
            expectEquals(r.mgr.getActiveIndex(), 1);
            expect(!arp[2]);
            r.tap(ARP);
            expect(arp[2]);
        }

        beginTest("Shift + Arp: the arp's latch while the arp is on");
        {
            Rig r;
            bool arp[8] = {}, latch[8] = {};
            ScreenManager::EngineHooks h;
            h.arpEnabled = [&](int t) { return arp[t]; };
            h.setArpEnabled = [&](int t, bool on) { arp[t] = on; };
            h.arpLatch = [&](int t) { return latch[t]; };
            h.setArpLatch = [&](int t, bool on) { latch[t] = on; };
            r.mgr.setEngineHooks(h);
            arp[0] = true;

            r.shift(true); r.tap(ARP); r.shift(false);
            expect(latch[0]);
            expect(!r.mgr.isKeyLatchOn());
            expect(r.mgr.isLatchOn());
            expect(r.mgr.getNameBarTags().contains("LATCH"));
            expectEquals(r.mgr.getActiveIndex(), 0);         // Shift + Arp does not open the ARP screen
            expect(arp[0], "nor switch the arp");

            r.shift(true); r.tap(ARP); r.shift(false);
            expect(!latch[0]);
            expect(!r.mgr.getNameBarTags().contains("LATCH"));
        }

        beginTest("Shift + Arp with the arp off: key latch, also when armed with no keys held");
        {
            Rig r;
            Keys keys;
            r.mgr.setKeyboardListener(&keys);

            // Armed with nothing held: the next keys played latch.
            r.shift(true); r.tap(ARP); r.shift(false);
            expect(r.mgr.isKeyLatchOn());
            expect(r.mgr.getNameBarTags().contains("LATCH"));
            r.keyOn(60);
            r.keyOn(64);
            r.keyOff(60);
            r.keyOff(64);
            expectEquals(keys.log.joinIntoString(","), juce::String("on60,on64"));

            // The first key after all were released starts a new chord.
            r.keyOn(67);
            expectEquals(keys.log.joinIntoString(","), juce::String("on60,on64,off60,off64,on67"));
            r.keyOff(67);
            expectEquals(keys.log.size(), 5);

            // Latch off lets go.
            r.shift(true); r.tap(ARP); r.shift(false);
            expect(!r.mgr.isKeyLatchOn());
            expect(keys.log.contains("off67"));
            expect(!r.mgr.getNameBarTags().contains("LATCH"));
            r.keyOn(72);
            r.keyOff(72);
            expect(keys.log.contains("off72"), "no latch: keys release normally");

            // Keys already held when latch is switched on stay when released.
            keys.log.clear();
            r.keyOn(50);
            r.shift(true); r.tap(ARP); r.shift(false);
            r.keyOff(50);
            expectEquals(keys.log.joinIntoString(","), juce::String("on50"));
            r.shift(true); r.tap(ARP); r.shift(false);
            expectEquals(keys.log.joinIntoString(","), juce::String("on50,off50"));
        }

        beginTest("Play = start / stop; Shift + Play = stop and return to the start");
        {
            Rig r;
            r.tap(PLAY);
            expect(r.seq.isPlaying());
            for (int i = 0; i < 100; ++i)
                r.seq.processBlock(512);
            expect(r.seq.getCurrentStep(0) > 0);
            r.tap(PLAY);
            expect(!r.seq.isPlaying());
            expect(r.seq.getCurrentStep(0) > 0, "a plain stop stays where it is");

            // Already stopped: reset the position.
            r.shift(true); r.tap(PLAY); r.shift(false);
            expect(!r.seq.isPlaying());
            expectEquals(r.seq.getCurrentStep(0), 0);

            // Playing: stop and return.
            r.tap(PLAY);
            for (int i = 0; i < 100; ++i)
                r.seq.processBlock(512);
            r.shift(true); r.tap(PLAY); r.shift(false);
            expect(!r.seq.isPlaying());
            expectEquals(r.seq.getCurrentStep(0), 0);
            expectWithinAbsoluteError(r.seq.getBeatPosition(), 0.0, 1.0e-9);
        }

        beginTest("Shift + Record held = FILL while held");
        {
            Rig r;
            r.tap(RECORD);
            expect(r.seq.isRecording(), "plain Record arms recording");
            r.tap(RECORD);
            expect(!r.seq.isRecording());

            r.shift(true);
            r.press(RECORD);
            expect(r.seq.isFillActive());
            expect(r.mgr.isFillHeld());
            expect(!r.seq.isRecording());
            expect(r.mgr.getNameBarTags().contains("FILL"));
            r.shift(false);                     // Shift let go first: the fill lasts as long as Record
            expect(r.seq.isFillActive());
            r.release(RECORD);
            expect(!r.seq.isFillActive());
            expect(!r.mgr.getNameBarTags().contains("FILL"));
        }

        beginTest("Func + Record / Play / > on a step screen: the selected track's steps; again = undo");
        {
            Rig r;
            auto& pat = r.seq.getPattern();
            pat.getTrack(0).getStep(2).trig = true;
            pat.getTrack(0).setLength(8);

            r.func(true);
            r.tap(RECORD);
            expectEquals(r.text(), juce::String("COPY T1 STEPS"));
            expect(!r.seq.isRecording());

            r.seq.setSelectedTrack(1);
            r.tap(PATTERN_KEY);
            expect(!r.mgr.isPatternKeyHeld(), "Func + > is paste, not the pattern key");
            expect(pat.getTrack(1).getStep(2).trig);
            expectEquals(pat.getTrack(1).length, 8);
            expectEquals(r.text(), juce::String("PASTE T2 STEPS"));

            r.tap(PATTERN_KEY);                 // the same again: undo
            expect(!pat.getTrack(1).getStep(2).trig);
            expectEquals(pat.getTrack(1).length, 16);
            expectEquals(r.text(), juce::String("UNDO PASTE T2 STEPS"), "what an undo took back may fill the name bar");

            r.tap(PATTERN_KEY);                 // and a third time pastes again
            expect(pat.getTrack(1).getStep(2).trig);

            r.seq.setSelectedTrack(0);
            r.tap(PLAY);
            expect(!r.seq.isPlaying());
            expect(!pat.getTrack(0).getStep(2).trig);
            expect(pat.getTrack(1).getStep(2).trig, "only the selected track");
            expectEquals(r.text(), juce::String("CLEAR T1 STEPS"));
            r.tap(PLAY);
            expect(pat.getTrack(0).getStep(2).trig, "undone");

            // Not "immediately afterwards" once something else was used: it is a new clear.
            r.tap(PLAY);
            expect(!pat.getTrack(0).getStep(2).trig);
            r.func(false);
            r.turn(0);
            r.func(true);
            r.tap(PLAY);
            expect(!pat.getTrack(0).getStep(2).trig);
            expectEquals(r.text(), juce::String("CLEAR T1 STEPS"));

            // Copy twice = the copy is undone: the clipboard is what it was (track 0's first copy).
            r.seq.setSelectedTrack(1);
            r.tap(RECORD);
            r.tap(RECORD);
            expectEquals(r.text(), juce::String("UNDO COPY"));
            r.seq.setSelectedTrack(3);
            r.tap(PATTERN_KEY);
            expectEquals(pat.getTrack(3).length, 8);
            r.func(false);
        }

        beginTest("Paste with nothing copied says so and changes nothing");
        {
            Rig r;
            r.seq.getPattern().getTrack(0).getStep(1).trig = true;
            r.func(true);
            r.tap(PATTERN_KEY);
            r.func(false);
            expectEquals(r.text(), juce::String("NOTHING COPIED"));
            expect(r.seq.getPattern().getTrack(0).getStep(1).trig);
            r.now += 2001;
            expectEquals(r.text(), juce::String(), "the message lasts two seconds");
        }

        beginTest("Copy / clear / paste on TRACKS: the whole pattern");
        {
            Rig r("TRACKS", "B");
            r.seq.getPattern().getTrack(0).getStep(0).trig = true;
            r.seq.getPattern().getTrack(6).getStep(9).trig = true;
            r.seq.getPattern().swing = 0.25f;

            r.func(true);
            r.tap(RECORD);
            expectEquals(r.text(), juce::String("COPY PAT 1"));
            r.func(false);

            r.press(PATTERN_KEY); r.pad(3); r.release(PATTERN_KEY);
            expectEquals(r.seq.getActivePatternIndex(), 3);

            // A whole pattern is pasted over or cleared only by a held button (half a second).
            auto held = [&r](int cc) { r.press(cc); r.now += ScreenManager::HOLD_MS + 10; r.release(cc); };
            r.func(true);
            r.tap(PATTERN_KEY);
            expectEquals(r.text(), juce::String("HOLD TO PASTE"));
            expect(!r.seq.getPattern(3).getTrack(0).getStep(0).trig, "a short press pastes nothing");
            held(PATTERN_KEY);
            expectEquals(r.text(), juce::String("PASTE PAT 4"));
            expect(r.seq.getPattern(3).getTrack(0).getStep(0).trig);
            expect(r.seq.getPattern(3).getTrack(6).getStep(9).trig);
            expectEquals(r.seq.getPattern(3).swing, 0.25f);
            r.tap(PATTERN_KEY);                 // the same again: undo, at the press
            expect(!r.seq.getPattern(3).getTrack(0).getStep(0).trig, "undone");
            expect(!r.seq.getPattern(3).getTrack(6).getStep(9).trig);
            held(PATTERN_KEY);

            r.tap(PLAY);
            expectEquals(r.text(), juce::String("HOLD TO CLEAR"));
            expect(r.seq.getPattern(3).getTrack(0).getStep(0).trig, "a short press clears nothing");
            expect(!r.seq.isPlaying());
            held(PLAY);
            expectEquals(r.text(), juce::String("CLEAR PAT 4"));
            expect(!r.seq.getPattern(3).getTrack(0).getStep(0).trig);
            expect(!r.seq.getPattern(3).getTrack(6).getStep(9).trig);
            expect(r.seq.getPattern(0).getTrack(0).getStep(0).trig, "other patterns untouched");
            r.tap(PLAY);
            expect(r.seq.getPattern(3).getTrack(6).getStep(9).trig, "undone");
            r.func(false);
        }

        beginTest("Copy / clear / paste on PARAM: the selected track's sound; on TRK FX: its effects");
        {
            Rig r("PARAM", "TRK FX");
            auto& pat = r.seq.getPattern();
            const int dirt = 240, insert = 200;     // an effect-block cell and an insert-slot cell (the id table)
            pat.getTrack(0).engineType = EngineType::FM;
            pat.getTrack(0).soundParams[5] = 0.7f;
            pat.getTrack(0).soundParams[MixParam::VOLUME] = 0.4f;
            pat.getTrack(0).soundParams[dirt] = 0.6f;
            pat.getTrack(0).soundParams[insert] = 0.3f;
            pat.getTrack(0).soundParams[RangeParam::LO_BASE + dirt] = 0.1f;
            pat.getTrack(0).getStep(0).trig = true;

            r.func(true);
            r.tap(RECORD);
            expectEquals(r.text(), juce::String("COPY T1 SOUND"));

            r.seq.setSelectedTrack(1);
            r.tap(PATTERN_KEY);
            expectEquals(r.text(), juce::String("PASTE T2 SOUND"));
            expect(pat.getTrack(1).engineType == EngineType::FM);
            expectEquals(pat.getTrack(1).soundParams[5], 0.7f);
            expectEquals(pat.getTrack(1).soundParams[dirt], 0.6f, "the whole sound: effects too");
            expect(r.seq.getPattern(7).getTrack(1).engineType == EngineType::FM, "one sound per track, in every pattern");
            expect(!pat.getTrack(1).getStep(0).trig, "steps are not part of the sound");

            r.tap(PATTERN_KEY);                 // undo
            expect(pat.getTrack(1).engineType == EngineType::UnifiedVoice);
            expectEquals((int) pat.getTrack(1).soundParams.count(5), 0);

            // Clear on PARAM: the engine's parameters go, the mixer and the effects stay.
            r.seq.setSelectedTrack(0);
            r.tap(PLAY);
            expectEquals(r.text(), juce::String("RESET T1 ENG"));
            expect(pat.getTrack(0).engineType == EngineType::FM);
            expectEquals((int) pat.getTrack(0).soundParams.count(5), 0);
            expectEquals(pat.getTrack(0).soundParams[MixParam::VOLUME], 0.4f);
            expectEquals(pat.getTrack(0).soundParams[dirt], 0.6f);
            expect(pat.getTrack(0).getStep(0).trig);
            r.tap(PLAY);
            expectEquals(pat.getTrack(0).soundParams[5], 0.7f);
            r.func(false);

            // TRK FX: the three act on the effects only.
            r.mgr.setActiveScreen(1);
            r.func(true);
            r.tap(RECORD);
            expectEquals(r.text(), juce::String("COPY T1 FX"));
            r.tap(PLAY);
            expectEquals(r.text(), juce::String("CLEAR T1 FX"));
            expectEquals((int) pat.getTrack(0).soundParams.count(dirt), 0);
            expectEquals((int) pat.getTrack(0).soundParams.count(insert), 0);
            expectEquals((int) pat.getTrack(0).soundParams.count(RangeParam::LO_BASE + dirt), 0, "and their pressure ranges");
            expectEquals(pat.getTrack(0).soundParams[5], 0.7f, "the engine stays");
            expectEquals(pat.getTrack(0).soundParams[MixParam::VOLUME], 0.4f, "the mixer stays");
            expect(pat.getTrack(0).getStep(0).trig);
            r.tap(PLAY);                        // undo
            expectEquals(pat.getTrack(0).soundParams[dirt], 0.6f);
            expectEquals(pat.getTrack(0).soundParams[insert], 0.3f);

            r.seq.setSelectedTrack(2);
            pat.getTrack(2).soundParams[MixParam::VOLUME] = 0.9f;
            r.tap(PATTERN_KEY);
            expectEquals(r.text(), juce::String("PASTE T3 FX"));
            expectEquals(pat.getTrack(2).soundParams[dirt], 0.6f);
            expectEquals(pat.getTrack(2).soundParams[RangeParam::LO_BASE + dirt], 0.1f);
            expect(pat.getTrack(2).engineType == EngineType::UnifiedVoice, "the engine is not pasted");
            expectEquals((int) pat.getTrack(2).soundParams.count(5), 0);
            expectEquals(pat.getTrack(2).soundParams[MixParam::VOLUME], 0.9f, "nor the mixer");
            r.func(false);
        }

        beginTest("Sound copy / paste / clear reach the real engine (GrammarHooks)");
        {
            Rig r("PARAM", "B");
            AudioEngine ae;
            ae.setSequencer(&r.seq);
            for (int t = 0; t < r.seq.getPattern().getTrackCount(); ++t)
                ae.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));
            float clockTempo = 0.0f;
            r.mgr.setEngineHooks(GrammarHooks::make(r.seq, ae, [&](float bpm) { clockTempo = bpm; }));

            // Track 0: FM with one parameter moved off its default.
            ae.setEngineType(0, EngineType::FM);
            r.seq.getPattern().getTrack(0).engineType = EngineType::FM;
            ParamSpec moved;
            for (auto& page : ae.getEngine(0)->getParamPages())
                for (auto& p : page.params)
                    if (!moved.isValid() && p.isValid() && p.kind == ParamKind::Linear && p.maxVal > p.minVal)
                        moved = p;
            expect(moved.isValid());
            const float defaultValue = ae.getTrackParameter(0, moved.id);
            const float target = (defaultValue > (moved.minVal + moved.maxVal) * 0.5f) ? moved.minVal : moved.maxVal;
            ae.setTrackParameter(0, moved.id, target);
            r.seq.getPattern().getTrack(0).soundParams[moved.id] = target;
            ae.setTrackParameter(0, MixParam::PAN, -0.5f);

            r.func(true);
            r.tap(RECORD);
            r.seq.setSelectedTrack(1);
            r.tap(PATTERN_KEY);
            expect(ae.getEngine(1)->getType() == EngineType::FM);
            expectWithinAbsoluteError(ae.getTrackParameter(1, moved.id), target, 1.0e-4f);
            expectWithinAbsoluteError(ae.getTrackParameter(1, MixParam::PAN), -0.5f, 1.0e-4f);
            expect(r.seq.getPattern().getTrack(1).engineType == EngineType::FM);

            r.tap(PATTERN_KEY);                 // undo
            expect(ae.getEngine(1)->getType() == EngineType::UnifiedVoice);
            expectWithinAbsoluteError(ae.getTrackParameter(1, MixParam::PAN), 0.0f, 1.0e-4f);

            r.seq.setSelectedTrack(0);
            r.tap(PLAY);                        // clear: engine defaults, mixer kept
            expect(ae.getEngine(0)->getType() == EngineType::FM);
            expectWithinAbsoluteError(ae.getTrackParameter(0, moved.id), defaultValue, 1.0e-4f);
            expectWithinAbsoluteError(ae.getTrackParameter(0, MixParam::PAN), -0.5f, 1.0e-4f);
            r.tap(PLAY);                        // undo
            expectWithinAbsoluteError(ae.getTrackParameter(0, moved.id), target, 1.0e-4f);
            r.func(false);

            // The same hooks drive the Arp button and the arp latch on the real arpeggiators.
            Arpeggiator arp;
            ae.setTrackArpeggiator(0, &arp);
            r.tap(ARP);                         // no ARP screen in this rig: it switches at once
            expect(arp.isEnabled());
            r.shift(true); r.tap(ARP); r.shift(false);
            expect(arp.getLatch());
            expect(!r.mgr.isKeyLatchOn());
            ae.setTrackArpeggiator(0, nullptr);

            // ...and tap tempo tells the clock.
            r.func(true);
            r.now = 10000; r.tap(SETTINGS);
            r.now = 10400; r.tap(SETTINGS);
            r.func(false);
            expectWithinAbsoluteError(clockTempo, 150.0f, 0.01f);
        }

        beginTest("A screen that reports an edit scope gets copy / clear / paste; the default applies when it does not");
        {
            Rig r;
            r.a->handlesEdits = true;
            r.seq.getPattern().getTrack(0).getStep(0).trig = true;
            r.func(true);
            r.tap(RECORD);
            expect(r.a->saw("copy F"));
            expectEquals(r.text(), juce::String("COPY OWN"));
            r.turn(0);
            r.tap(PLAY);
            expect(r.a->saw("clear F"));
            expect(r.seq.getPattern().getTrack(0).getStep(0).trig, "the default clear did not run");
            r.turn(0);
            r.tap(PATTERN_KEY);
            expect(r.a->saw("paste F"));

            // Nothing went to the app's clipboard.
            r.a->handlesEdits = false;
            r.turn(0);
            r.tap(PATTERN_KEY);
            expectEquals(r.text(), juce::String("NOTHING COPIED"));
            r.func(false);
        }

        beginTest("Func + Settings = tap tempo; Settings alone opens SETTINGS");
        {
            Rig r("A", "SETTINGS");
            r.seq.setTempo(90.0f);
            r.func(true);
            r.now = 5000; r.tap(SETTINGS);
            expectEquals(r.seq.getTempo(), 90.0f, "one tap is not a tempo");
            expectEquals(r.mgr.getActiveIndex(), 0);
            r.now = 5500; r.tap(SETTINGS);
            expectWithinAbsoluteError(r.seq.getTempo(), 120.0f, 0.01f);
            expectEquals(r.text(), juce::String("120 BPM"));

            // The average of the taps.
            r.now = 6100; r.tap(SETTINGS);          // intervals 500, 600 -> 550 ms
            expectWithinAbsoluteError(r.seq.getTempo(), 60000.0f / 550.0f, 0.01f);

            // More than two seconds later: a new count.
            r.now = 9000; r.tap(SETTINGS);
            expectWithinAbsoluteError(r.seq.getTempo(), 60000.0f / 550.0f, 0.01f);
            r.now = 9250; r.tap(SETTINGS);
            expectWithinAbsoluteError(r.seq.getTempo(), 240.0f, 0.01f);
            r.func(false);

            r.tap(SETTINGS);
            expectEquals(r.mgr.getActiveIndex(), 1);
            expectWithinAbsoluteError(r.seq.getTempo(), 240.0f, 0.01f);
        }

        steppingTests();
        keyboardSplitTests();
        tracksTests();
    }

private:
    void near(float actual, float expected, const juce::String& what = {})
    {
        expectWithinAbsoluteError(actual, expected, 1.0e-4f, what);
    }

    // ParamSpec::turned — every parameter kind, plain / fine / coarse.
    // The firmware side is faked: a word store that answers peeks and takes pokes, as the device does.
    void keyboardSplitTests()
    {
        beginTest("Copy / clear / paste do nothing on a screen that has nothing to copy");
        {
            using namespace cgtest;
            Rig r;
            r.mgr.setScreenContext(0, ScreenManager::CopyContext::None);
            auto& track = r.seq.getPattern().getTrack(r.seq.getSelectedTrack());
            track.steps[0].trig = true;
            r.func(true); r.tap(PLAY); r.func(false);          // Func + Play = clear
            expect(track.steps[0].trig, "the track's steps are untouched");
        }

        beginTest("Keyboard split: Shift + Oct - (a transpose in the firmware) steps the lower octave down; octave presses keep the split");
        {
            std::map<juce::uint32, juce::uint32> mem;
            bool shift = false;
            juce::int64 now = 0;
            juce::String message;
            KeyboardSplit split;
            // What the firmware does when an octave button is pressed, plain or with Shift.
            auto firmwareWritesKeys = [&]
            {
                const int oct = (int) (juce::int32) mem[KeyboardSplit::OCTAVE], tr = (int) (juce::int32) mem[KeyboardSplit::TRANSPOSE];
                for (int k = 0; k < 25; ++k)
                    mem[KeyboardSplit::KEY_MAP + 4 * (juce::uint32) k] = KeyboardSplit::KEY_WORD | (juce::uint32) (48 + 12 * oct + tr + k);
                for (int k = 0; k < 25; ++k)
                    mem[KeyboardSplit::KEY_RECORDS + KeyboardSplit::KEY_RECORD_SIZE * (juce::uint32) k] = (juce::uint32) (48 + 12 * oct + tr + k);
                mem[KeyboardSplit::STORED_WORD] = 0x13u | ((juce::uint32) (tr & 0xFF) << 8);
                for (int i = 0; i < 128; i += 4)
                {
                    juce::uint32 w = 0;
                    for (int b = 0; b < 4; ++b) w |= (juce::uint32) ((48 + 12 * oct + tr + i + b) & 0xFF) << (8 * b);
                    mem[KeyboardSplit::NOTE_TABLE + (juce::uint32) i] = w;
                }
            };
            auto octaveButton = [&](int delta) { mem[KeyboardSplit::OCTAVE] = (juce::uint32) ((int) mem[KeyboardSplit::OCTAVE] + delta); firmwareWritesKeys(); };
            auto shiftOctave = [&](int delta) { mem[KeyboardSplit::TRANSPOSE] = (juce::uint32) ((int) mem[KeyboardSplit::TRANSPOSE] + delta); firmwareWritesKeys(); };
            auto note = [&](int key) { return (int) (mem[KeyboardSplit::KEY_MAP + 4 * (juce::uint32) key] & 0xFF); };
            int sent = 0;
            split.clock = [&] { return now; };
            split.isShiftHeld = [&] { return shift; };
            split.showMessage = [&](const juce::String& t) { message = t; };
            split.send = [&](const std::vector<juce::uint8>& m)
            {
                ++sent;
                auto word = [&](int at) { juce::uint32 v = 0; for (int i = 0; i < 5; ++i) v = (v << 7) | m[(size_t) (at + i)]; return v; };
                const juce::uint32 fn = word(5) & ~1u, a0 = word(10), a1 = word(15);
                juce::uint32 r0 = 0;
                if (fn == KeyboardSplit::PEEK32) r0 = mem[a0];
                else if (fn == KeyboardSplit::POKE32) { mem[a0] = a1; r0 = a0; }      // a poke's reply is the address
                juce::uint8 reply[10] = { 0x00, 0x20, 0x29, 0x00, 0x7D };
                for (int i = 0; i < 5; ++i) reply[5 + i] = (juce::uint8) ((r0 >> (28 - 7 * i)) & 0x7F);
                split.sysExReceived(reply, 10);
            };

            firmwareWritesKeys();
            split.poll();
            expect(split.isOctaveKnown() && split.getOctave() == 0);
            const int quiet = sent;
            now = 5000; split.poll(); split.poll();
            expectEquals(sent, quiet, "nothing is sent while there is no split and Shift is up");

            // Shift + Oct -: the firmware transposes everything down a semitone; the app undoes that
            // and shifts the lower octave down an octave instead.
            shift = true; shiftOctave(-1);
            expectEquals(note(12), 59);
            split.poll();
            expectEquals(split.getSplit(), 1);
            expectEquals((int) mem[KeyboardSplit::TRANSPOSE], 0, "the transpose is cancelled");
            expectEquals(note(0), 36);  expectEquals(note(11), 47);      // lower octave one octave down
            expectEquals(note(12), 60); expectEquals(note(24), 72);      // the rest back in tune
            expectEquals(message, juce::String("SPLIT -1 OCT"));
            expectEquals((int) (mem[KeyboardSplit::NOTE_TABLE] & 0xFF), 48, "the firmware's note table is back in tune too");
            expectEquals((int) mem[KeyboardSplit::KEY_RECORDS], 48, "and the per-key records it reloads on connect");
            expectEquals((int) mem[KeyboardSplit::KEY_RECORDS + 20 * 12], 60);
            expectEquals((int) mem[KeyboardSplit::STORED_WORD], 0x13, "and its stored copy of the transpose, leaving the rest of that word alone");
            expectEquals((int) ((mem[KeyboardSplit::NOTE_TABLE + 12] >> 8) & 0xFF), 61);
            shiftOctave(-1); split.poll();
            shiftOctave(-1); split.poll();
            expectEquals(split.getSplit(), 3);
            expectEquals(note(0), 12); expectEquals(note(12), 60);

            // Shift let go a moment before the poll still counts.
            shift = false; now += 300; shiftOctave(-1); split.poll();
            expectEquals(split.getSplit(), 0, "after 3 it wraps to no split");
            expectEquals(message, juce::String("SPLIT OFF"));
            expectEquals(note(0), 48); expectEquals(note(12), 60);

            // Two presses between polls count twice. Shift + Oct + steps back.
            shift = true; shiftOctave(-1); shiftOctave(-1); split.poll();
            expectEquals(split.getSplit(), 2);
            shiftOctave(1); split.poll();
            expectEquals(split.getSplit(), 1);

            // A plain octave press moves everything; the split is written again.
            shift = false; now += 5000;
            octaveButton(1);
            expectEquals(note(0), 60, "the firmware has just removed the split");
            split.poll();
            expectEquals(split.getOctave(), 1);
            expectEquals(note(0), 48); expectEquals(note(12), 72);       // lower octave an octave below the rest

            // Notes never leave the MIDI range; quitting restores the firmware's own notes.
            expectEquals(KeyboardSplit::noteOfKey(0, -3, 3), 0);
            split.restore();
            expectEquals(note(0), 60); expectEquals(split.getSplit(), 0);
        }
    }

    void steppingTests()
    {
        const auto P = TurnStep::Plain, F = TurnStep::Fine, C = TurnStep::Coarse;
        const auto Lin = ParamKind::Linear, Log = ParamKind::Log, Int = ParamKind::Int, Ch = ParamKind::Choice;

        beginTest("Stepping: unipolar continuous — 1 %, 0.1 %, snap to quarters and the default");
        {
            ParamSpec s { "LVL", 1, 0.0f, 1.0f, 0.3f, Lin };
            near(s.turned(0.5f, 1, P), 0.51f);
            near(s.turned(0.5f, -3, P), 0.47f);
            near(s.turned(0.5f, 1, F), 0.501f);
            near(s.turned(0.5f, -1, F), 0.499f);
            near(s.turned(0.1f, 1, C), 0.25f);
            near(s.turned(0.25f, 1, C), 0.3f, "the default is a snap point");
            near(s.turned(0.3f, 1, C), 0.5f);
            near(s.turned(0.5f, -1, C), 0.3f);
            near(s.turned(0.1f, 3, C), 0.5f, "three clicks, three snap points");
            near(s.turned(0.9f, 1, C), 1.0f);
            near(s.turned(1.0f, 1, C), 1.0f);
            near(s.turned(0.1f, -1, C), 0.0f);
            near(s.turned(0.0f, -1, C), 0.0f);
            near(s.turned(0.5f, 0, C), 0.5f);
            near(s.turned(7.0f, 0, P), 1.0f, "always inside the range");

            // Any range: 200..1000, default at the bottom.
            ParamSpec t { "X", 1, 200.0f, 1000.0f, 200.0f, Lin };
            near(t.turned(200.0f, 1, P), 208.0f);
            near(t.turned(200.0f, 1, F), 200.8f);
            near(t.turned(200.0f, 1, C), 400.0f);
            near(t.turned(450.0f, 1, C), 600.0f);
        }

        beginTest("Stepping: bipolar continuous — snap to min, quarters, centre, max");
        {
            ParamSpec s { "PAN", 1, -1.0f, 1.0f, 0.0f, Lin };
            near(s.turned(0.0f, 1, P), 0.02f);
            near(s.turned(0.0f, 1, F), 0.002f);
            near(s.turned(0.1f, 1, C), 0.5f);
            near(s.turned(0.5f, 1, C), 1.0f);
            near(s.turned(0.1f, -1, C), 0.0f);
            near(s.turned(0.0f, -1, C), -0.5f);
            near(s.turned(-0.5f, -1, C), -1.0f);
            near(s.turned(-1.0f, -1, C), -1.0f);
            near(s.turned(-0.3f, 1, C), 0.0f, "centre");
        }

        beginTest("Stepping: a value on a grid (stepSize) — one step, a tenth of a step");
        {
            ParamSpec s { "RAT", 1, 0.0f, 10.0f, 1.0f, Lin, "x", nullptr, 0, 0.5f };
            near(s.turned(2.0f, 1, P), 2.5f);
            near(s.turned(2.2f, 1, P), 2.5f, "plain lands on the grid");
            near(s.turned(2.0f, 1, F), 2.05f);
            near(s.turned(2.0f, -2, F), 1.9f);
            near(s.turned(2.0f, 1, C), 2.5f, "coarse: quarters of the range");
            near(s.turned(2.5f, 1, C), 5.0f);
        }

        beginTest("Stepping: time on a log scale — per cent of the scale, quarters of it");
        {
            ParamSpec s { "ATK", 1, 0.001f, 10.0f, 0.001f, Log, "s" };
            expect(s.turned(0.1f, 1, P) > 0.1f);
            near(s.toNorm(s.turned(0.1f, 1, P)) - s.toNorm(0.1f), 0.01f);
            near(s.toNorm(s.turned(0.1f, 1, F)) - s.toNorm(0.1f), 0.001f);
            near(s.toNorm(s.turned(0.002f, 1, C)), 0.25f);
            near(s.toNorm(s.turned(0.002f, 2, C)), 0.5f);
            near(s.turned(0.002f, -1, C), 0.001f);
        }

        beginTest("Stepping: frequencies by octaves");
        {
            ParamSpec s { "CUT", 1, 20.0f, 20000.0f, 1000.0f, Log, "Hz" };
            near(s.toNorm(s.turned(440.0f, 1, P)) - s.toNorm(440.0f), 0.01f);
            expect(s.turned(440.0f, 1, F) > 440.0f && s.turned(440.0f, 1, F) < s.turned(440.0f, 1, P));
            expectWithinAbsoluteError(s.turned(440.0f, 1, C), 880.0f, 0.01f);
            expectWithinAbsoluteError(s.turned(440.0f, 2, C), 1760.0f, 0.01f);
            expectWithinAbsoluteError(s.turned(440.0f, -1, C), 220.0f, 0.01f);
            expectWithinAbsoluteError(s.turned(15000.0f, 1, C), 20000.0f, 0.01f);
            expectWithinAbsoluteError(s.turned(30.0f, -1, C), 20.0f, 0.01f);
        }

        beginTest("Stepping: semitones and notes by octaves, keeping the pitch class");
        {
            ParamSpec tune { "TUNE", 1, -24.0f, 24.0f, 0.0f, Int, "st" };
            near(tune.turned(7.0f, 1, P), 8.0f);
            near(tune.turned(7.0f, 1, F), 8.0f, "a whole number cannot move by less than one");
            near(tune.turned(7.0f, 1, C), 19.0f);
            near(tune.turned(19.0f, 1, C), 19.0f, "no room for another octave: stays");
            near(tune.turned(7.0f, -2, C), -17.0f);
            near(tune.turned(0.0f, 2, C), 24.0f);

            ParamSpec note { "NOTE", 1, 0.0f, 127.0f, 60.0f, Int, "note" };
            near(note.turned(60.0f, 1, C), 72.0f);
            near(note.turned(60.0f, -1, C), 48.0f);
            near(note.turned(120.0f, 1, C), 120.0f);
            near(note.turned(60.0f, 1, P), 61.0f);

            ParamSpec fineTune { "TUNE", 1, -24.0f, 24.0f, 0.0f, Lin, "st" };       // continuous semitones
            near(fineTune.turned(0.5f, 1, C), 12.5f);
            near(fineTune.turned(0.5f, -1, C), -11.5f);
            near(fineTune.turned(0.0f, 1, P), 0.48f);
        }

        beginTest("Stepping: BPM — 1, 0.1 with Shift, 10 with Func");
        {
            ParamSpec s { "BPM", 1, 20.0f, 300.0f, 120.0f, Lin, "bpm" };
            near(s.turned(120.0f, 1, P), 121.0f);
            near(s.turned(120.0f, -1, P), 119.0f);
            near(s.turned(120.0f, 1, F), 120.1f);
            near(s.turned(120.0f, -3, F), 119.7f);
            near(s.turned(120.3f, 1, P), 121.0f, "plain goes back to whole numbers");
            near(s.turned(120.0f, 1, C), 130.0f);
            near(s.turned(120.0f, -2, C), 100.0f);
            near(s.turned(295.0f, 1, C), 300.0f);
            near(s.turned(25.0f, -1, C), 20.0f);
            expectEquals(s.format(120.0f), juce::String("120"));
            expectEquals(s.format(120.1f), juce::String("120.1"));

            ParamSpec whole { "BPM", 1, 20.0f, 300.0f, 120.0f, Int, "bpm" };       // an Int cell behaves the same
            near(whole.turned(120.0f, 1, C), 130.0f);
            near(whole.turned(120.0f, 1, F), 120.1f);
        }

        beginTest("Stepping: lengths and counts by 4");
        {
            ParamSpec s { "LEN", 1, 1.0f, 64.0f, 16.0f, Int };
            near(s.turned(13.0f, 1, P), 14.0f);
            near(s.turned(13.0f, 1, F), 14.0f);
            near(s.turned(13.0f, 1, C), 16.0f);
            near(s.turned(16.0f, 1, C), 20.0f);
            near(s.turned(13.0f, -1, C), 12.0f);
            near(s.turned(12.0f, -1, C), 8.0f);
            near(s.turned(3.0f, -1, C), 1.0f, "the bottom of the range is a stop");
            near(s.turned(1.0f, 1, C), 4.0f);
            near(s.turned(62.0f, 1, C), 64.0f);
            near(s.turned(64.0f, 1, C), 64.0f);
            near(s.turned(8.0f, 3, C), 20.0f);

            ParamSpec bipolar { "OFS", 1, -7.0f, 7.0f, 0.0f, Int };
            near(bipolar.turned(0.0f, 1, C), 4.0f);
            near(bipolar.turned(4.0f, 1, C), 7.0f);
            near(bipolar.turned(0.0f, -2, C), -7.0f);
        }

        beginTest("Stepping: division lists snap to the straight values; other lists move by 4");
        {
            static const char* const divs[] = { "1/4", "1/4T", "1/8", "1/8D", "1/8T", "1/16", "1/16T" };
            ParamSpec d { "DIV", 1, 0.0f, 6.0f, 2.0f, Ch, "", divs, 7 };
            expect(d.isDivisionList());
            near(d.turned(0.0f, 1, P), 1.0f, "plain visits every entry");
            near(d.turned(0.0f, 1, F), 1.0f);
            near(d.turned(0.0f, 1, C), 2.0f);
            near(d.turned(2.0f, 1, C), 5.0f);
            near(d.turned(5.0f, 1, C), 5.0f, "no straight value above: stays");
            near(d.turned(5.0f, -1, C), 2.0f);
            near(d.turned(4.0f, -1, C), 2.0f, "from a triplet to the straight value below");
            near(d.turned(1.0f, 2, C), 5.0f);
            near(d.turned(6.0f, -3, C), 0.0f);

            static const char* const bars[] = { "1/16", "1/8T", "1/8", "1BAR", "2BAR" };
            ParamSpec b { "DIV", 1, 0.0f, 4.0f, 0.0f, Ch, "", bars, 5 };
            near(b.turned(0.0f, 1, C), 2.0f);
            near(b.turned(2.0f, 2, C), 4.0f, "bars are straight values");

            static const char* const waves[] = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J" };
            ParamSpec w { "WAVE", 1, 0.0f, 9.0f, 0.0f, Ch, "", waves, 10 };
            expect(!w.isDivisionList());
            near(w.turned(1.0f, 1, P), 2.0f);
            near(w.turned(1.0f, 1, F), 2.0f);
            near(w.turned(1.0f, 1, C), 5.0f);
            near(w.turned(5.0f, 1, C), 9.0f);
            near(w.turned(9.0f, 1, C), 9.0f);
            near(w.turned(2.0f, -1, C), 0.0f);

            ParamSpec onOff { "ON", 1, 0.0f, 1.0f, 0.0f, Ch, "", ParamChoices::offOn, 2 };
            near(onOff.turned(0.0f, 1, C), 1.0f);
            near(onOff.turned(1.0f, -1, F), 0.0f);
            near(onOff.turned(1.0f, 1, P), 1.0f);
        }

        beginTest("Grid screens step by the same rule, from the modifiers");
        {
            class Grid : public GridScreen
            {
            public:
                float level = 0.5f, count = 13.0f;
                Grid()
                {
                    Page page;
                    page.name = "G";
                    page.cells[0] = cell({ "LVL", 0, 0.0f, 1.0f, 0.3f }, [this] { return level; }, [this](float v) { level = v; });
                    page.cells[1] = cell({ "LEN", 0, 1.0f, 64.0f, 16.0f, ParamKind::Int }, [this] { return count; }, [this](float v) { count = v; });
                    gridPages.push_back(page);
                }
                juce::String getName() const override { return "G"; }
            };

            ScreenManager mgr;
            auto* g = new Grid();
            mgr.addScreen(std::unique_ptr<Screen>(g));
            mgr.controlChangeReceived(16, 0x15, 1);
            near(g->level, 0.51f);
            mgr.controlChangeReceived(7, 63, 127);
            mgr.controlChangeReceived(16, 0x15, 1);
            mgr.controlChangeReceived(7, 63, 0);
            near(g->level, 0.511f);
            mgr.controlChangeReceived(1, 105, 127);
            mgr.controlChangeReceived(16, 0x15, 1);
            near(g->level, 0.75f);
            mgr.controlChangeReceived(16, 0x16, 1);
            near(g->count, 16.0f);
            mgr.controlChangeReceived(16, 0x16, 127);
            near(g->count, 12.0f);
            mgr.controlChangeReceived(1, 105, 0);
            near(ParamGrid::turned({ "LEN", 0, 1.0f, 64.0f, 16.0f, ParamKind::Int }, 12.0f, 127, TurnStep::Plain), 11.0f);
            near(ParamGrid::stepped({ "LVL", 0, 0.0f, 1.0f, 0.3f }, 0.5f, 126, true), 0.498f);
        }
    }

    // TRACKS: the paged mixer, driven through ScreenManager.
    void tracksTests()
    {
        using namespace cgtest;

        struct Mixer
        {
            SequencerCore seq;
            AudioEngine ae;
            ScreenManager mgr;
            PadManager pads;
            HomeScreen* home = nullptr;
            juce::int64 now = 0;

            Mixer()
            {
                seq.prepareToPlay(44100.0, 512);
                ae.setSequencer(&seq);
                for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                    ae.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));
                mgr.setSequencer(&seq);
                mgr.setClock([this] { return now; });
                auto h = std::make_unique<HomeScreen>(seq, &ae);
                home = h.get();
                home->setPadManager(&pads);
                mgr.addScreen(std::move(h));
            }

            void cc(int number, bool down) { mgr.controlChangeReceived(1, number, down ? 127 : 0); }
            void tap(int number) { cc(number, true); cc(number, false); }
            void shift(bool on) { mgr.controlChangeReceived(7, 63, on ? 127 : 0); }
            void turn(int encoder, int clicks = 1) { mgr.controlChangeReceived(16, 0x15 + encoder, clicks < 0 ? 128 + clicks : clicks); }
            void pad(int p) { mgr.noteOnReceived(1, Rig::padNote(p), 100); mgr.noteOffReceived(1, Rig::padNote(p)); }
            float param(int track, int id, float fallback) const
            {
                return ParamConstants::getTrackParam(seq.getPattern().getTrack(track), id, fallback);
            }
        };

        beginTest("Name bar: a status text that does not fit keeps its leading words");
        {
            struct Long : Screen
            {
                juce::String sub;
                juce::String getName() const override { return "PARAM"; }
                juce::String getSubPage() const override { return sub; }
                int getPageCount() const override { return 11; }
                int getPageIndex() const override { return 3; }
                void render(DisplayRenderer&) override {}
            };
            ScreenManager mgr;
            auto owned = std::make_unique<Long>();
            auto* screen = owned.get();
            mgr.addScreen(std::move(owned));
            mgr.setActiveScreen(0);
            // "PARAM T1" + "4/11" leave six characters.
            screen->sub = "ANALG";
            expectEquals(mgr.composeNameBar().text, juce::String("ANALG"));
            screen->sub = "ANALG 17+";
            expectEquals(mgr.composeNameBar().text, juce::String("ANALG"), "Not ALG 17+");
            screen->sub = "FM 17+";
            expectEquals(mgr.composeNameBar().text, juce::String("FM 17+"));
            screen->sub = "LONGWORDS";
            expectEquals(mgr.composeNameBar().text, juce::String("GWORDS"), "One long word keeps its end, as before");
        }

        beginTest("TRACKS: bank buttons step five pages (VOL, PAN, SND1, SND2, ENG), never the pattern");
        {
            Mixer m;
            m.seq.start();
            expectEquals(m.home->getPageCount(), 5);
            expectEquals(m.mgr.composeNameBar().left, juce::String("TRACKS T1"));
            expectEquals(m.mgr.composeNameBar().text, juce::String("VOL P1"));
            expectEquals(m.mgr.composeNameBar().pages, juce::String("1/5"));

            // The bank buttons go round: on from the last page is the first, back from the first is the last.
            const char* names[] = { "PAN", "SND1", "SND2", "ENG", "VOL" };
            for (int i = 0; i < 5; ++i)
            {
                m.tap(BANK_DOWN);
                expect(m.mgr.composeNameBar().text.startsWith(names[i]), names[i]);
            }
            expectEquals(m.home->getPageIndex(), 0);
            m.tap(BANK_UP);
            expectEquals(m.home->getPageIndex(), 4);
            expectEquals(m.mgr.composeNameBar().pages, juce::String("5/5"));
            expectEquals(m.seq.getPendingPattern(), -1);
            expectEquals(m.seq.getActivePatternIndex(), 0);
            m.tap(BANK_UP);
            expectEquals(m.home->getPageIndex(), 3);

            // Func + bank: first / last page. Func + top pad: straight to a page.
            m.cc(105, true);
            m.tap(BANK_UP);
            expectEquals(m.home->getPageIndex(), 0);
            m.tap(BANK_DOWN);
            expectEquals(m.home->getPageIndex(), 4);
            m.pad(1);
            expectEquals(m.home->getPageIndex(), 1);
            expectEquals(m.seq.getSelectedTrack(), 0);
            m.cc(105, false);

            // The pattern number stays visible, with the queued one.
            m.cc(PATTERN_KEY, true); m.pad(2); m.cc(PATTERN_KEY, false);
            expectEquals(m.mgr.composeNameBar().text, juce::String("P1>3"));
            m.seq.stop();
            expect(m.home->usesEncoderBar());
        }

        beginTest("TRACKS: knob N = track N on every page; fine and coarse by the stepping rule");
        {
            Mixer m;
            m.turn(2, 5);
            near(m.param(2, MixParam::VOLUME, -1.0f), 0.85f);
            near(m.ae.getTrackParameter(2, MixParam::VOLUME), 0.85f);
            near(m.param(0, MixParam::VOLUME, 0.8f), 0.8f);
            m.shift(true);
            m.turn(2, 1);
            m.shift(false);
            near(m.param(2, MixParam::VOLUME, -1.0f), 0.851f);
            expect(m.seq.getPattern().getTrack(2).engineType == EngineType::UnifiedVoice, "Shift + turn is no longer the engine");
            m.cc(105, true);
            m.turn(2, -1);
            near(m.param(2, MixParam::VOLUME, -1.0f), 0.8f, "coarse: down to the default");
            m.turn(2, -1);
            near(m.param(2, MixParam::VOLUME, -1.0f), 0.75f);
            m.cc(105, false);
            expectEquals((int) m.seq.getPattern().getTrack(2).soundParams.count(MixParam::PAN), 0, "Func + turn is no longer pan");

            m.tap(BANK_DOWN);                   // PAN
            m.turn(5, -10);
            near(m.param(5, MixParam::PAN, 9.0f), -0.2f);
            near(m.ae.getTrackParameter(5, MixParam::PAN), -0.2f);
            expectEquals(m.home->getEncoderName(5), juce::String("L20"));
            expectEquals(m.home->getEncoderName(0), juce::String("C"));

            m.tap(BANK_DOWN);                   // SND1
            m.turn(7, 30);
            near(m.param(7, MixParam::SEND_BASE, -1.0f), 0.3f);
            near(m.ae.getTrackParameter(7, MixParam::SEND_BASE), 0.3f);
            m.tap(BANK_DOWN);                   // SND2
            m.turn(0, 12);
            near(m.param(0, MixParam::SEND_BASE + 1, -1.0f), 0.12f);
            near(m.param(0, MixParam::SEND_BASE, 0.0f), 0.0f);
        }

        beginTest("TRACKS: the name bar reads out the knob just turned (\"T3 VOL 81\")");
        {
            Mixer m;
            m.now = 100;
            m.turn(2, 1);
            expectEquals(m.mgr.composeNameBar().text, juce::String("T3 VOL 81"));
            m.now = 900;
            expectEquals(m.mgr.composeNameBar().text, juce::String("VOL P1"));
            m.tap(BANK_DOWN);
            m.turn(4, 10);
            expectEquals(m.mgr.composeNameBar().text, juce::String("T5 PAN R20"));
            for (int i = 0; i < 3; ++i) m.tap(BANK_DOWN);
            m.turn(1, 1);
            expectEquals(m.mgr.composeNameBar().text, juce::String("T2 ENG >FM"));
        }

        beginTest("TRACKS ENG: a knob chooses a pending engine, the track's top pad commits it, leaving cancels");
        {
            Mixer m;
            for (int i = 0; i < 4; ++i) m.tap(BANK_DOWN);
            expectEquals(m.home->getPageIndex(), (int) HomeScreen::ENG);

            m.turn(3, 2);
            m.turn(5, 1);
            expectEquals(m.home->getPendingEngine(3), (int) EngineType::MacroOsc);
            expectEquals(m.home->getPendingEngine(5), (int) EngineType::FM);
            expect(m.ae.getEngine(3)->getType() == EngineType::UnifiedVoice, "shown, not applied");
            expect(m.seq.getPattern().getTrack(3).engineType == EngineType::UnifiedVoice);

            // The pad of another track commits nothing (it selects).
            m.pad(0);
            expect(m.ae.getEngine(3)->getType() == EngineType::UnifiedVoice);
            expectEquals(m.seq.getSelectedTrack(), 0);

            m.pad(3);
            expect(m.ae.getEngine(3)->getType() == EngineType::MacroOsc);
            expect(m.seq.getPattern().getTrack(3).engineType == EngineType::MacroOsc);
            expectEquals(m.home->getPendingEngine(3), -1);
            expectEquals(m.home->getPendingEngine(5), (int) EngineType::FM, "the other choice still waits");
            expect(m.ae.getEngine(5)->getType() == EngineType::UnifiedVoice);

            // Turning back to the current engine is no choice at all.
            m.turn(5, -1);
            expectEquals(m.home->getPendingEngine(5), -1);
            m.turn(5, 1);

            // Leaving the page cancels: by bank button...
            m.tap(BANK_UP);
            m.tap(BANK_DOWN);
            expectEquals(m.home->getPendingEngine(5), -1);
            m.pad(5);
            expect(m.ae.getEngine(5)->getType() == EngineType::UnifiedVoice);

            // ...and by leaving the screen.
            m.turn(6, 3);
            expectEquals(m.home->getPendingEngine(6), (int) EngineType::Wavetable);
            m.mgr.setActiveScreen(0);
            expectEquals(m.home->getPendingEngine(6), -1);

            // Coarse moves through the list by 4.
            m.home->setPage(HomeScreen::ENG);
            m.cc(105, true);
            m.turn(6, 1);
            m.cc(105, false);
            expectEquals(m.home->getPendingEngine(6), (int) EngineType::WaveSeq);
        }

        beginTest("TRACKS pads: top selects, bottom mutes, Func + bottom solos; its own pad colours");
        {
            Mixer m;
            m.pad(4);
            expectEquals(m.seq.getSelectedTrack(), 4);
            m.pad(8 + 1);
            expect(m.seq.getPattern().getTrack(1).muted);
            m.cc(105, true);
            m.pad(8 + 2);
            m.cc(105, false);
            expect(m.seq.getPattern().getTrack(2).soloed);
            expect(!m.seq.getPattern().getTrack(2).muted);

            std::array<Screen::PadColour, 16> c;
            expect(m.mgr.getPadFrame(c) == ScreenManager::PadSource::Screen);
            expectEquals((int) c[4].r, 127);
            expect(c[0].r < 60);
            expect(c[9].r > 0 && c[9].g == 0, "muted: red");
            expect(c[10].b == 127, "solo: blue");
            expect(c[8].g > 0 && c[8].r == 0, "sounding: green");

            // A pending engine marks its pad.
            m.home->setPage(HomeScreen::ENG);
            m.turn(6, 1);
            m.mgr.getPadFrame(c);
            expect(c[6].r == 127 && c[6].b == 0);
        }

        beginTest("TRACKS: Track layer + turn changes that page's value on every track");
        {
            Mixer m;
            m.seq.getPattern().getTrack(3).soundParams[MixParam::VOLUME] = 0.5f;
            m.cc(DOWN, true);
            m.turn(0, -10);
            m.cc(DOWN, false);
            near(m.param(0, MixParam::VOLUME, 0.8f), 0.7f);
            near(m.param(7, MixParam::VOLUME, 0.8f), 0.7f);
            near(m.param(3, MixParam::VOLUME, 0.8f), 0.4f, "each from its own value");
            near(m.ae.getTrackParameter(7, MixParam::VOLUME), 0.7f);
            expectEquals(m.home->getPageIndex(), 0, "and Down did not also page");
        }
    }
};
static ControlGrammarTests ControlGrammarTestsInstance;
