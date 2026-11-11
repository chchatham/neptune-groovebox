#pragma once

#include "help_card_tests.h"
#include "../src/project/UndoHistory.h"
#include "../src/project/ProjectBridge.h"

// The undo history: every clear / paste / reset, the screens' own one-click destructive actions,
// > + Play and each recording pass can be taken back (Func + Shift + Play) and done again (Func +
// Shift + Record), up to fifteen of them, whatever was played or turned in between. A whole pattern
// is cleared or pasted over only by a held button; TRK FX clears and copies the track's effects.
// Driven as the app is, on the help-card rig (the app's thirteen screens on a real AudioEngine).
namespace undotest
{
    using namespace helptest;
    using Plan = ScreenManager::EditPlan;

    struct URig : Rig
    {
        URig() { fill(); }

        // The whole live state as the project file would hold it.
        juce::String state()
        {
            ProjectBridge::Live live { seq, audio };
            return juce::JSON::toString(ProjectBridge::capture(live, "undo").toVar());
        }

        void mgrTick() { const juce::ScopedLock ui(mgr.getLock()); mgr.tick(); }
        void undo() { shift(true); func(true); tap(PLAY);   func(false); shift(false); }
        void redo() { shift(true); func(true); tap(RECORD); func(false); shift(false); }
        void edit(int cc) { func(true); tap(cc); func(false); }
        // A whole-pattern edit: the button held for `ms`, with the refresh running.
        void heldEdit(int cc, juce::int64 ms = ScreenManager::HOLD_MS + 20)
        {
            func(true);
            press(cc);
            for (juce::int64 t = 0; t < ms; t += 20) { UiClock::advance(20); mgrTick(); }
            release(cc);
            func(false);
        }
        int undos() const { return mgr.getUndoHistory().undoCount(); }
        int redos() const { return mgr.getUndoHistory().redoCount(); }
        juce::String undoLine() { return mgr.getHelpCard(Key::Func, 0).lines[4].text; }
        juce::String redoLine() { return mgr.getHelpCard(Key::Func, 0).lines[5].text; }

        void set(int t, int id, float value)
        {
            audio.setTrackParameter(t, id, value);
            track(t).soundParams[id] = value;
        }

        void pageKey(int index) { func(true); pad(index); func(false); }
        // Bank down until the active screen's name bar shows this page.
        bool goToPage(const juce::String& name)
        {
            for (int i = 0; i < 24; ++i)
            {
                if (mgr.getActiveScreen()->getName() == name || mgr.getActiveScreen()->getSubPage().startsWith(name))
                    return true;
                tap(BANK_DOWN);
            }
            return false;
        }
    };
}

class UndoHistoryTests : public juce::UnitTest
{
public:
    UndoHistoryTests() : UnitTest("Undo History") {}

    void runTest() override
    {
        using namespace undotest;
        using Kind = ScreenManager::EditPlan::Kind;

        beginTest("The history itself: fifteen levels, the sixteenth drops the oldest, a new action ends redo");
        {
            UndoHistory h;
            for (int i = 1; i <= 16; ++i)
            {
                UndoEntry e;
                e.verb = "A" + juce::String(i);
                h.push(std::move(e));
            }
            expectEquals(h.undoCount(), UndoHistory::MAX_LEVELS);
            expectEquals(UndoHistory::MAX_LEVELS, 15);
            expectEquals(h.undos().front().verb, juce::String("A2"), "the oldest went");
            expectEquals(h.undos().back().verb, juce::String("A16"));
            h.undone(); h.undone();
            expectEquals(h.undoCount(), 13);
            expectEquals(h.redoCount(), 2);
            expectEquals(h.redos().back().verb, juce::String("A15"), "the one undone last is redone first");
            h.redone();
            expectEquals(h.undos().back().verb, juce::String("A15"));
            UndoEntry e;
            e.verb = "NEW";
            h.push(std::move(e));
            expectEquals(h.redoCount(), 0, "a new action: nothing to redo");
            UndoEntry labelled;
            labelled.verb = "CLEAR";
            labelled.names = { "T3 STEPS", "STEPS" };
            expectEquals(labelled.labels().joinIntoString("|"), juce::String("CLEAR T3 STEPS|CLEAR STEPS|CLEAR"));
        }

        beginTest("Sixteen clears: fifteen can be taken back, in order, each exactly");
        {
            URig r;
            r.mgr.setActiveScreen(STEP);
            juce::StringArray before;           // the state before clear n
            for (int n = 0; n < 16; ++n)
            {
                r.track(0).getStep(n).trig = true;
                r.track(0).getStep(n).notes = { { 40 + n, 0.5f } };
                before.add(r.state());
                r.edit(PLAY);
                expectEquals(r.stepsText(0), juce::String(), "cleared");
                r.somethingElse();              // each is a clear of its own, not "the same again"
            }
            expectEquals(r.undos(), 15);
            for (int n = 15; n >= 1; --n)
            {
                r.undo();
                expectEquals(r.state(), before[n], "undo " + juce::String(16 - n));
            }
            expectEquals(r.undos(), 0);
            expectEquals(r.redos(), 15);
            const auto oldest = r.state();
            r.undo();
            expectEquals(r.text(), juce::String("NOTHING TO UNDO"));
            expectEquals(r.state(), oldest, "the first clear was pushed out: it stays");
            for (int n = 1; n <= 15; ++n)
                r.redo();
            expectEquals(r.stepsText(0), juce::String(), "all redone");
            r.redo();
            expectEquals(r.text(), juce::String("NOTHING TO REDO"));
        }

        beginTest("Undo and redo are exact for every scope (the whole captured state compared)");
        {
            struct Case { const char* name; int screen; bool held; int cc; const char* label; };
            const Case cases[] = {
                { "step clear",        STEP,   true,  PLAY,        "CLEAR STEP 1" },
                { "step paste",        STEP,   true,  PATTERN_KEY, "PASTE STEP 1" },
                { "locks clear",       PARAM,  true,  PLAY,        "CLEAR S1 LOCKS" },
                { "locks clear FLTR",  FLTR,   true,  PLAY,        "CLEAR S1 LOCKS" },
                { "track steps clear", STEP,   false, PLAY,        "CLEAR T1 STEPS" },
                { "track steps paste", STEP,   false, PATTERN_KEY, "PASTE T1 STEPS" },
                { "pattern clear",     TRACKS, false, PLAY,        "CLEAR PATTERN 1" },
                { "pattern paste",     TRACKS, false, PATTERN_KEY, "PASTE PATTERN 1" },
                { "engine reset",      PARAM,  false, PLAY,        "RESET T1 ENGINE" },
                { "engine reset FLTR", FLTR,   false, PLAY,        "RESET T1 ENGINE" },
                { "sound paste",       PARAM,  false, PATTERN_KEY, "PASTE T1 SOUND" },
                { "effects clear",     TRKFX,  false, PLAY,        "CLEAR T1 FX" },
                { "effects paste",     TRKFX,  false, PATTERN_KEY, "PASTE T1 FX" } };

            for (auto& c : cases)
            {
                const juce::String where = c.name;
                URig r;
                // Effects on tracks 1 and 3, so that clearing and pasting them changes something.
                r.audio.addInsertEffect(0, static_cast<EffectType>(1));
                r.set(0, TrackFxParam::DIRT_BASE + DirtBlock::Drive, 0.6f);
                r.audio.addInsertEffect(2, static_cast<EffectType>(3));
                r.set(2, TrackFxParam::MODFX_BASE + 1, 0.4f);
                r.mgr.setActiveScreen(c.screen);

                // Something to paste: copied from somewhere that differs from where it lands.
                if (c.cc == PATTERN_KEY)
                {
                    if (c.screen == STEP && c.held)
                    {
                        r.track(0).getStep(3).notes = { { 72, 0.3f }, { 76, 0.3f } };
                        r.padDown(3); r.wait(1000); r.edit(RECORD); r.padUp(3);
                    }
                    else if (c.screen == TRACKS)
                    {
                        r.edit(RECORD);
                        r.track(0).getStep(9).trig = true;
                        r.track(4).getStep(2).trig = true;
                        r.seq.getPattern().swing = 0.3f;
                    }
                    else
                    {
                        r.seq.setSelectedTrack(c.screen == PARAM ? 1 : 2);      // PARAM: the drum kit, another engine
                        r.edit(RECORD);
                        r.seq.setSelectedTrack(0);
                    }
                    r.somethingElse();
                }
                if (c.held) { r.padDown(0); r.wait(1000); }

                const auto before = r.state();
                const auto plan = r.mgr.getEditPlan(c.cc == PLAY ? Edit::Clear : Edit::Paste);
                expectEquals(plan.wordings()[0], juce::String(c.label), where);
                if (plan.hold) r.heldEdit(c.cc);
                else           r.edit(c.cc);
                const auto after = r.state();
                expect(after != before, where + ": it changed something");
                expectEquals(r.undos(), 1, where);
                expectEquals(r.mgr.getEditPlan(Edit::Undo).wordings()[0], "UNDO " + juce::String(c.label), where);
                r.somethingElse();

                r.undo();
                expectEquals(r.state(), before, where + ": undone exactly");
                expectEquals(r.undos(), 0, where);
                expectEquals(r.mgr.getEditPlan(Edit::Redo).wordings()[0], "REDO " + juce::String(c.label), where);
                r.redo();
                expectEquals(r.state(), after, where + ": redone exactly");
                r.undo();
                expectEquals(r.state(), before, where + ": and undone again");
                if (c.held) r.padUp(0);
            }
        }

        beginTest("An action stays undoable through knob turns, notes, the transport, pages and screens");
        {
            URig r;
            r.mgr.setActiveScreen(STEP);
            const auto steps = r.stepsText(0);
            r.edit(PLAY);
            expectEquals(r.stepsText(0), juce::String());

            r.mgr.setActiveScreen(PARAM);
            r.tap(BANK_DOWN);
            r.turn(0, 3); r.turn(5, -2);                            // knobs
            r.mgr.noteOnReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 60, 100);      // a key
            r.mgr.noteOffReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 60);
            r.mgr.setActiveScreen(DRUMS);
            r.pad(9); r.pad(2);                                     // pads that play
            r.tap(PLAY);                                            // the transport runs
            expect(r.seq.isPlaying());
            r.seq.processBlock(4096);
            r.tap(PLAY);
            expect(!r.seq.isPlaying());
            r.tap(UP); r.tap(DOWN);
            r.shift(true); r.pad(5); r.shift(false);                // another screen (SONG)
            r.tap(SETTINGS_BUTTON);
            UiClock::advance(60000);

            expectEquals(r.undos(), 1);
            expectEquals(r.undoLine(), juce::String("UNDO: CLEAR T1 STEPS"), "the card names it, on any screen");
            expectEquals(r.redoLine(), juce::String("KNOB TURNS: NO UNDO"));
            const float turned = r.audio.getTrackParameter(0, r.moved.id);
            r.undo();
            expectEquals(r.text(), juce::String("UNDO CLEAR T1 STEPS"), "the confirmation takes the whole name bar");
            expectEquals(r.stepsText(0), steps, "the steps are back");
            expectEquals(r.audio.getTrackParameter(0, r.moved.id), turned, "knob turns are not undo steps");
            expectEquals(r.undoLine(), juce::String("NOTHING TO UNDO"));
            expectEquals(r.redoLine(), juce::String("REDO: CLEAR T1 STEPS"));
            r.redo();
            expectEquals(r.text(), juce::String("REDO CLEAR T1 STEPS"));
            expectEquals(r.stepsText(0), juce::String());
        }

        beginTest("The same chord again, at once, still takes the edit back; after anything else it is the edit again");
        {
            URig r;
            r.mgr.setActiveScreen(STEP);
            const auto before = r.state();
            r.func(true);
            r.tap(PLAY);
            expect(r.state() != before);
            expect(r.mgr.getEditPlan(Edit::Clear).kind == Kind::Undo);
            r.tap(PLAY);
            expectEquals(r.text(), juce::String("UNDO CLEAR T1 STEPS"));
            expectEquals(r.state(), before);
            expectEquals(r.undos(), 0);
            expectEquals(r.redos(), 1, "it is the history's undo: redo brings the clear back");
            r.func(false);
            r.redo();
            expectEquals(r.stepsText(0), juce::String());

            // Something else in between: the chord is a clear again (of nothing: no new step).
            r.undo();
            r.edit(PLAY);
            r.somethingElse();
            expect(r.mgr.getEditPlan(Edit::Clear).kind == Kind::Steps);
            r.edit(PLAY);
            expectEquals(r.text(), juce::String("CLEAR T1 STEPS"));
            expectEquals(r.undos(), 1, "clearing what is empty is not a step");
            r.undo();
            expectEquals(r.state(), before);

            // A copy again: the clipboard goes back, the history is not involved.
            r.func(true);
            r.tap(RECORD); r.tap(RECORD);
            expectEquals(r.text(), juce::String("UNDO COPY"));
            r.func(false);
            expectEquals(r.undos(), 0);
        }

        beginTest("A project loaded or started empties the history; a target that is gone is dropped");
        {
            URig r;
            r.mgr.setActiveScreen(STEP);
            r.edit(PLAY);
            r.undo(); r.redo(); r.undo();
            expectEquals(r.undos() + r.redos(), 1);
            r.mgr.projectLoaded();
            expectEquals(r.undos() + r.redos(), 0);
            expect(r.mgr.getEditPlan(Edit::Undo).kind == Kind::Nothing);
            expect(r.mgr.getEditPlan(Edit::Redo).kind == Kind::Nothing);
            r.redo();
            expectEquals(r.text(), juce::String("NOTHING TO REDO"));

            // Track 3's steps cleared, then track 1's; then track 1 gets another engine. The clear of
            // track 1 held that engine's locks: it is passed over (and dropped), track 3's is undone.
            r.seq.setSelectedTrack(2);
            r.edit(PLAY);
            r.somethingElse();
            r.seq.setSelectedTrack(0);
            r.edit(PLAY);
            expectEquals(r.undos(), 2);
            r.audio.setEngineType(0, EngineType::FM);
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                r.seq.getPattern(p).getTrack(0).engineType = EngineType::FM;
            expectEquals(r.undoLine(), juce::String("UNDO: CLEAR T3 STEPS"));
            r.undo();
            expectEquals(r.text(), juce::String("UNDO CLEAR T3 STEPS"));
            expectEquals(r.stepsText(2), juce::String("5t0 "));
            expectEquals(r.stepsText(0), juce::String(), "track 1 stays as it is");
            expectEquals(r.undos(), 0);
            expectEquals(r.redos(), 1);

            // A sound step whose engine is gone.
            URig s;
            s.mgr.setActiveScreen(PARAM);
            s.edit(PLAY);
            expectEquals(s.undos(), 1);
            s.audio.setEngineType(0, EngineType::MacroOsc);
            const auto now = s.state();
            s.undo();
            expectEquals(s.text(), juce::String("NOTHING TO UNDO"));
            expectEquals(s.state(), now);
        }

        beginTest("TRK FX: clear resets the effects - blocks, insert slots, arp locks - and nothing else");
        {
            URig r;
            const int drive = TrackFxParam::DIRT_BASE + DirtBlock::Drive;
            r.set(0, drive, 0.7f);
            r.set(0, TrackFxParam::TALK_BASE + 1, 0.5f);
            r.set(0, TrackFxParam::MODFX_BASE + 1, 0.35f);
            r.audio.addInsertEffect(0, static_cast<EffectType>(0));
            r.audio.addInsertEffect(0, static_cast<EffectType>(2));
            const auto spec = r.audio.getInsertChain(0)->getEffect(0)->getParamSpec(0);
            const float insertValue = spec.minVal + (spec.maxVal - spec.minVal) * 0.25f;
            r.set(0, AudioEngine::EFFECT_PARAM_BASE, insertValue);
            r.set(0, AudioEngine::ARP_LOCK_BASE + 0, 0.5f);
            r.set(0, AudioEngine::ARP_LOCK_BASE + 2, 3.0f);
            r.track(0).soundParams[RangeParam::LO_BASE + drive] = 0.1f;             // a pressure range on an effect
            r.track(0).soundParams[RangeParam::HI_BASE + r.moved.id] = r.moved.maxVal;   // and one on the engine
            r.set(0, MixParam::PAN, -0.25f);
            r.set(0, MixParam::SEND_BASE, 0.3f);
            r.track(0).setLength(8);
            r.track(0).speedMultiplier = 2.0f;

            r.mgr.setActiveScreen(TRKFX);
            const auto before = r.state();
            const auto steps = r.stepsText(0);
            expectEquals(r.card(Key::Func, 0).lines[0].text, juce::String("REC  COPY  T1 FX"));
            expectEquals(r.card(Key::Func, 0).lines[1].text, juce::String("PLAY CLEAR T1 FX"));
            r.edit(PLAY);
            expectEquals(r.text(), juce::String("CLEAR T1 FX"));

            const TrackFx defaults;
            for (int id = TrackFxParam::DIRT_BASE; id < TrackFxParam::END; ++id)
                expectEquals(r.audio.getTrackParameter(0, id), defaults.get(id), "block cell " + juce::String(id));
            expectEquals(r.audio.getTrackParameter(0, drive), 0.0f, "DIRT is off");
            expectEquals(r.audio.getInsertChain(0)->getEffectCount(), 0, "no insert slots");
            expect(r.audio.getTrackParameter(0, AudioEngine::ARP_LOCK_BASE + 0) < 0.0f, "arp gate lock off");
            expect(r.audio.getTrackParameter(0, AudioEngine::ARP_LOCK_BASE + 2) < 0.5f, "arp ratchet lock off");
            expectEquals((int) r.track(0).soundParams.count(RangeParam::LO_BASE + drive), 0, "its pressure range went with it");

            expectEquals((int) r.track(0).soundParams.count(RangeParam::HI_BASE + r.moved.id), 1, "the engine's range stays");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, r.moved.id), r.movedValue, 1.0e-5f, "the engine stays");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, MixParam::VOLUME), 0.4f, 1.0e-6f, "VOL stays");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, MixParam::PAN), -0.25f, 1.0e-6f, "PAN stays");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, MixParam::SEND_BASE), 0.3f, 1.0e-6f, "the sends stay");
            expectEquals(r.track(0).length, 8, "LEN stays");
            expectEquals(r.track(0).speedMultiplier, 2.0f, "SPD stays");
            expectEquals(r.stepsText(0), steps, "the steps stay");
            expect(r.audio.getEngine(0)->getType() == EngineType::UnifiedVoice);

            // What was set after the clear stays when the clear is taken back: only the effects return.
            r.somethingElse();
            r.set(0, MixParam::VOLUME, 0.9f);
            r.undo();
            expectEquals(r.text(), juce::String("UNDO CLEAR T1 FX"));
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, MixParam::VOLUME), 0.9f, 1.0e-6f, "a later knob turn is kept");
            r.set(0, MixParam::VOLUME, 0.4f);
            expectEquals(r.state(), before, "everything else is back exactly");
            expectEquals(r.audio.getInsertChain(0)->getEffectCount(), 2);
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, AudioEngine::EFFECT_PARAM_BASE), insertValue, 1.0e-4f);

            // Copy / paste are the same set: track 3 gets track 1's effects and keeps its own engine and mixer.
            r.edit(RECORD);
            expectEquals(r.text(), juce::String("COPY T1 FX"));
            r.seq.setSelectedTrack(2);
            r.set(2, MixParam::VOLUME, 0.55f);
            const float engineValue = r.audio.getTrackParameter(2, r.moved.id);
            r.edit(PATTERN_KEY);
            expectEquals(r.text(), juce::String("PASTE T3 FX"));
            expectEquals(r.audio.getInsertChain(2)->getEffectCount(), 2);
            EffectType type;
            expect(r.audio.getInsertEffectType(2, 1, type) && type == static_cast<EffectType>(2));
            expectWithinAbsoluteError(r.audio.getTrackParameter(2, drive), 0.7f, 1.0e-6f);
            expectWithinAbsoluteError(r.audio.getTrackParameter(2, AudioEngine::EFFECT_PARAM_BASE), insertValue, 1.0e-4f);
            expectWithinAbsoluteError(r.audio.getTrackParameter(2, MixParam::VOLUME), 0.55f, 1.0e-6f, "the mixer is not pasted");
            expectWithinAbsoluteError(r.audio.getTrackParameter(2, r.moved.id), engineValue, 1.0e-5f, "nor the engine");

            // PARAM stays the engine: effects untouched.
            r.seq.setSelectedTrack(0);
            r.mgr.setActiveScreen(PARAM);
            expectEquals(r.card(Key::Func, 0).lines[1].text, juce::String("PLAY RESET T1 ENGINE"));
            r.edit(PLAY);
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, r.moved.id), r.movedDefault, 1.0e-4f);
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, drive), 0.7f, 1.0e-6f);
            expectEquals(r.audio.getInsertChain(0)->getEffectCount(), 2);
        }

        beginTest("A whole pattern: Func + Play must be held; a short press does nothing and says so");
        {
            URig r;
            const auto before = r.state();
            expectEquals(r.card(Key::Func, 0).lines[1].text, juce::String("PLAY CLEAR PAT 1 HOLD"));
            expectEquals(r.card(Key::Func, 0).lines[2].text, juce::String(">    PASTE (EMPTY)"));
            expect(r.mgr.getEditPlan(Edit::Clear).hold && !r.mgr.getEditPlan(Edit::Copy).hold);

            // Short.
            r.func(true);
            r.press(PLAY);
            expect(r.mgr.isEditHeld());
            expectEquals(r.text(), juce::String("CLEAR >...."));
            UiClock::advance(200); r.mgrTick();
            expectEquals(r.text(), juce::String("CLEAR >>>.."), "it fills while held");
            expectEquals(r.state(), before);
            r.release(PLAY);
            expect(!r.mgr.isEditHeld());
            expectEquals(r.text(), juce::String("HOLD TO CLEAR"));
            expectEquals(r.state(), before, "let go early: nothing");
            expectEquals(r.undos(), 0);
            expect(!r.seq.isPlaying(), "and Play did not start the transport");

            // Long: the refresh clears it when the time is up, while the button is still down.
            r.press(PLAY);
            UiClock::advance(ScreenManager::HOLD_MS - 20); r.mgrTick();
            expectEquals(r.state(), before, "not yet");
            expectEquals(r.text(), juce::String("CLEAR >>>>>"));
            UiClock::advance(40); r.mgrTick();
            expect(!r.mgr.isEditHeld());
            expectEquals(r.text(), juce::String("CLEAR PAT 1"));
            juce::String all;
            for (int t = 0; t < r.seq.getPattern().getTrackCount(); ++t) all << r.stepsText(t);
            expectEquals(all, juce::String(), "every track's steps");
            r.release(PLAY);
            expectEquals(r.undos(), 1);
            // The same chord again takes it back at once (an undo destroys nothing: no hold).
            expect(!r.mgr.getEditPlan(Edit::Clear).hold);
            r.tap(PLAY);
            expectEquals(r.state(), before);
            r.func(false);

            // Without the refresh (or between two of them) the release decides.
            r.func(true);
            r.press(PLAY);
            UiClock::advance(ScreenManager::HOLD_MS + 5);
            r.release(PLAY);
            r.func(false);
            expect(r.state() != before, "held long enough");
            r.undo();
            expectEquals(r.state(), before);

            // Letting Func go ends the hold as letting Play go does.
            r.func(true);
            r.press(PLAY);
            UiClock::advance(100);
            r.func(false);
            expect(!r.mgr.isEditHeld());
            expectEquals(r.state(), before);
            r.release(PLAY);

            // The card stays through the hold and shows it filling; smaller scopes act at the press.
            r.func(true);
            r.wait();
            expect(r.showing(Key::Func));
            r.press(PLAY);
            expect(r.showing(Key::Func), "the held button leaves the card");
            UiClock::advance(250); r.mgrTick();
            expectEquals(r.card(Key::Func, 0).lines[1].text, juce::String("PLAY >>>.. PATTERN 1"));
            expectEquals(cardFault(r.card(Key::Func, 0)), juce::String());
            r.release(PLAY);
            r.func(false);
            expectEquals(r.state(), before);

            // Paste is held too; a paste with nothing copied is not (it does nothing anyway).
            r.edit(PATTERN_KEY);
            expectEquals(r.text(), juce::String("NONE COPIED"));
            r.edit(RECORD);
            expectEquals(r.text(), juce::String("COPY PAT 1"), "copy acts at the press");
            r.somethingElse();
            r.track(0).getStep(11).trig = true;
            const auto edited = r.state();
            expectEquals(r.card(Key::Func, 0).lines[2].text, juce::String(">    PASTE PAT 1 HOLD"));
            r.edit(PATTERN_KEY);
            expectEquals(r.text(), juce::String("HOLD TO PASTE"));
            expectEquals(r.state(), edited);
            r.heldEdit(PATTERN_KEY);
            expectEquals(r.state(), before, "the copy is back");
            expect(!r.mgr.isPatternKeyHeld());

            r.mgr.setActiveScreen(STEP);
            r.func(true);
            r.press(PLAY);
            expect(!r.mgr.isEditHeld());
            expectEquals(r.stepsText(0), juce::String(), "a track's steps: at the press");
            r.release(PLAY);
            r.func(false);
        }

        beginTest("> + Play (reload the kept copy) is an undo step");
        {
            URig r;
            r.press(PATTERN_KEY); r.tap(RECORD); r.release(PATTERN_KEY);        // keep
            r.track(0).getStep(12).trig = true;
            r.track(5).getStep(1).trig = true;
            const auto edited = r.state();
            r.press(PATTERN_KEY); r.tap(PLAY); r.release(PATTERN_KEY);          // reload
            expectEquals(r.text(), juce::String("RELOAD P1"));
            expect(r.state() != edited);
            expectEquals(r.undoLine(), juce::String("UNDO: RELOAD PAT 1"));
            r.undo();
            expectEquals(r.state(), edited, "the edits the reload threw away are back");
        }

        beginTest("FX TYPE: the effect it replaced comes back with its values; one scroll through the list is one step");
        {
            URig r;
            r.mgr.setActiveScreen(TRKFX);
            r.pageKey(5);
            expectEquals(r.mgr.getActiveScreen()->getPageIndex(), 5);
            r.turn(1, 2);                                   // TYPE: an effect into slot 1
            expectEquals(r.audio.getInsertChain(0)->getEffectCount(), 1);
            EffectType first;
            expect(r.audio.getInsertEffectType(0, 0, first));
            UiClock::advance(5000);
            r.turn(4, 7); r.turn(3, -20);                   // its first cell, and WET
            const auto dialled = r.state();
            expectEquals(r.undos(), 1);
            UiClock::advance(5000);

            r.turn(1, 1); UiClock::advance(100);
            r.turn(1, 1); UiClock::advance(100);
            r.turn(1, 1);
            EffectType now;
            expect(r.audio.getInsertEffectType(0, 0, now) && now != first);
            expect(r.state() != dialled);
            expectEquals(r.undos(), 2, "three clicks, one step");
            expectEquals(r.undoLine(), juce::String("UNDO: SET T1 FX1 TYPE"));

            r.undo();
            expect(r.audio.getInsertEffectType(0, 0, now) && now == first);
            expectEquals(r.state(), dialled, "the effect and what was dialled on it");
            r.redo();
            expect(r.audio.getInsertEffectType(0, 0, now) && now != first);
            r.undo();
            r.undo();
            expectEquals(r.audio.getInsertChain(0)->getEffectCount(), 0, "and the step before: no effect");

            // OFF removes the effect: also a step.
            r.redo();
            UiClock::advance(5000);
            r.turn(1, -40);
            expectEquals(r.audio.getInsertChain(0)->getEffectCount(), 0);
            r.undo();
            expectEquals(r.audio.getInsertChain(0)->getEffectCount(), 1);
        }

        beginTest("LCKS on a held step's TRIG page: the locks it cleared come back");
        {
            URig r;
            r.mgr.setActiveScreen(STEP);
            r.track(0).getStep(0).paramLocks[r.moved.id + 1] = 0.25f;
            const auto before = r.state();
            r.padDown(0);
            r.wait(1000);
            r.tap(BANK_UP);                                 // TRIG is one page back from MAIN
            expectEquals(r.mgr.getActiveScreen()->getName(), juce::String("TRIG"));
            r.turn(7, -1);
            expect(r.track(0).getStep(0).paramLocks.empty(), "LCKS turned down clears them");
            expect(r.track(0).getStep(0).trig);
            expectEquals(r.undos(), 1);
            expectEquals(r.undoLine(), juce::String("UNDO: CLEAR S1 LOCKS"));
            r.turn(7, -1);
            expectEquals(r.undos(), 1, "nothing left to clear: no step");
            r.padUp(0);
            r.undo();
            expectEquals(r.state(), before);
            r.redo();
            expect(r.track(0).getStep(0).paramLocks.empty());

            // Hold step + > + turn LCKS is the same clear.
            r.undo();
            r.padDown(0);
            r.wait(1000);
            r.press(PATTERN_KEY); r.turn(7, 1); r.release(PATTERN_KEY);
            r.padUp(0);
            expect(r.track(0).getStep(0).paramLocks.empty());
            r.undo();
            expectEquals(r.state(), before);
        }

        beginTest("PRES page: clearing a step's recorded pressure is an undo step");
        {
            URig r;
            r.mgr.setActiveScreen(STEP);
            auto& st = r.track(0).getStep(0);
            st.trig = true;
            st.notes = { Note { 60, 0.8f, {} } };
            st.notes[0].lane.points = { 0, 60, 120, 200, 120, 60 };
            expect(st.hasPressure());
            const auto before = r.state();
            const int undosBefore = r.undos();
            r.padDown(0);
            r.wait(1000);
            r.tap(BANK_UP);                                 // TRIG
            r.tap(BANK_UP);                                 // round to the last page: PRES
            for (int i = 0; i < 4; ++i) { r.turn(2, 1); r.wait(120); }   // CLR, four clicks
            expect(!r.track(0).getStep(0).hasPressure(), "CLR removed the pressure");
            expectEquals(r.undos(), undosBefore + 1);
            r.padUp(0);
            r.undo();
            expectEquals(r.state(), before);
            expect(r.track(0).getStep(0).hasPressure(), "undo brings the swell back");
        }

        beginTest("ARP: EVOL turned back and >PAT are undo steps");
        {
            URig r;
            r.mgr.setActiveScreen(ARP);
            auto* arpScreen = dynamic_cast<ArpScreen*>(r.mgr.getActiveScreen());
            expect(arpScreen != nullptr);
            if (arpScreen == nullptr) return;
            auto run = [&] { for (int i = 0; i < 8; ++i) r.arp.processBlock(512, 44100.0, 120.0f); };
            r.arp.setEnabled(true);
            r.arp.noteOn(60, 0.8f); r.arp.noteOn(64, 0.8f); r.arp.noteOn(67, 0.8f);
            r.arp.setSpice(1.0f);
            run();
            expect(r.goToPage("MUTA"));

            r.turn(4, 1);                                   // EVOL: mutate
            run();
            expect(r.arp.getMutationGeneration() > 0);
            const auto mutated = r.arp.captureMutation();
            auto same = [](const MutateSystem::NoteState& a, const MutateSystem::NoteState& b)
            {
                if (a.generation != b.generation || a.committed != b.committed) return false;
                for (int i = 0; i < MutateSystem::MAX_NOTE_SLOTS; ++i)
                    if (a.current[i].pitchOffset != b.current[i].pitchOffset || a.current[i].muted != b.current[i].muted
                        || a.current[i].velocityScale != b.current[i].velocityScale
                        || a.ancestor[i].pitchOffset != b.ancestor[i].pitchOffset)
                        return false;
                return true;
            };
            bool anyMutation = false;
            for (auto& slot : mutated.current)
                anyMutation = anyMutation || !slot.isIdentity();
            expect(anyMutation, "the mutation changed a note");
            expectEquals(r.undos(), 0, "mutating destroys nothing");

            r.turn(4, -1);                                  // EVOL back: the mutation is dropped
            run();
            expectEquals(r.arp.getMutationGeneration(), 0);
            expect(!same(r.arp.captureMutation(), mutated));
            expectEquals(r.undos(), 1);
            expectEquals(r.undoLine(), juce::String("UNDO: EVOL T1 TO 0"));
            r.undo();
            expectEquals(r.arp.getMutationGeneration(), mutated.generation, "shown at once, before the audio side has done it");
            run();
            expect(same(r.arp.captureMutation(), mutated), "the mutation is back");
            r.redo();
            run();
            expectEquals(r.arp.getMutationGeneration(), 0);
            r.arp.setEnabled(false);

            // >PAT (four clicks): the track's steps it overwrote.
            const auto before = r.state();
            arpScreen->exportToPattern = [&r]
            {
                r.track(0).setLength(4);
                r.track(0).clearAllSteps();
                r.track(0).getStep(1).trig = true;
                return true;
            };
            for (int click = 0; click < 4; ++click) { r.turn(6, 1); UiClock::advance(120); }
            expectEquals(r.track(0).length, 4, "written");
            expectEquals(r.undoLine(), juce::String("UNDO: WRITE ARP > T1"));
            r.undo();
            expectEquals(r.state(), before);
        }

        beginTest("A recording pass is one undo step: a bad take can be taken back, also while it runs");
        {
            URig r;
            const auto before = r.state();
            auto recordPlay = [&] { r.press(RECORD); r.tap(PLAY); r.release(RECORD); };
            auto play = [&](int trackIndex, int note)
            {
                r.seq.recordNote(trackIndex, note, 0.9f);
                r.seq.processBlock(12000);                  // a few steps on
            };

            // A pass in which nothing was played is not a step.
            recordPlay();
            expect(r.seq.isRecording() && r.seq.isPlaying());
            r.seq.processBlock(12000);
            r.tap(PLAY);
            expect(!r.seq.isPlaying() && !r.seq.isRecording());
            expectEquals(r.undos(), 0);
            expectEquals(r.state(), before);

            // A take on two tracks.
            r.mgr.setActiveScreen(TRACKS);
            recordPlay();
            play(0, 61); play(0, 63);
            r.seq.setSelectedTrack(3);
            play(3, 50);
            expectEquals(r.undos(), 0, "one step, when it ends");
            expectEquals(r.undoLine(), juce::String("UNDO: REC TAKE PAT 1"), "but the card already offers it");
            r.tap(PLAY);                                    // stop: the take is in the history
            const auto take = r.state();
            expect(take != before);
            expectEquals(r.undos(), 1);
            expectEquals(r.undoLine(), juce::String("UNDO: REC TAKE PAT 1"));
            r.mgr.setActiveScreen(DRUMS);
            r.undo();
            expectEquals(r.text(), juce::String("UNDO REC TAKE PAT 1"));
            r.seq.setSelectedTrack(0);
            expectEquals(r.state(), before, "the whole take, on both tracks");
            r.redo();
            r.seq.setSelectedTrack(3);
            expectEquals(r.state(), take);
            r.undo();
            r.seq.setSelectedTrack(0);

            // Undo while recording: what was recorded so far goes, recording goes on, and what is
            // played after that is the next step.
            r.mgr.setActiveScreen(TRACKS);
            recordPlay();
            play(0, 70);
            r.undo();
            expect(r.seq.isRecording() && r.seq.isPlaying(), "still recording");
            {
                // (The transport has moved: compare the steps.)
                bool found = false;
                for (auto& st : r.track(0).steps)
                    for (auto& n : st.notes) found = found || n.pitch == 70;
                expect(!found, "the bad take is gone");
            }
            play(0, 72);
            r.tap(PLAY);
            bool has72 = false;
            for (auto& st : r.track(0).steps)
                for (auto& n : st.notes) has72 = has72 || n.pitch == 72;
            expect(has72);
            expectEquals(r.undos(), 1, "the second take; the first was undone and then replaced");
            r.undo();
            r.seq.setSelectedTrack(0);
            expectEquals(r.state(), before);

            // An edit during a take splits it: the take so far, the edit, the rest.
            r.mgr.setActiveScreen(STEP);
            recordPlay();
            play(0, 80);
            r.edit(PLAY);                                   // clear the track's steps while recording
            play(0, 82);
            r.tap(PLAY);
            expectEquals(r.undos(), 3);
            r.undo(); r.undo(); r.undo();
            r.seq.setSelectedTrack(0);
            expectEquals(r.state(), before);
        }

        beginTest("The EDIT card: the undo and redo lines on every screen, never too long");
        {
            URig r;
            for (int screen = 0; screen < NUM_SCREENS; ++screen)
            {
                r.mgr.setActiveScreen(screen);
                const auto card = r.card(Key::Func, 0);
                expectEquals(card.lines[3].text, juce::String("WITH SHIFT: PLAY, REC"), screenNames[screen]);
                expectEquals(card.lines[4].text, juce::String("NOTHING TO UNDO"), screenNames[screen]);
                expectEquals(card.lines[5].text, juce::String("KNOB TURNS: NO UNDO"), screenNames[screen]);
                expectEquals(cardFault(card), juce::String(), screenNames[screen]);
            }
            // The longest names there are: pattern 16, track 8, step 1024.
            r.seq.selectPattern(15, SequencerCore::ChangeMode::Immediate);
            r.mgr.setActiveScreen(TRACKS);
            r.track(0).getStep(0).trig = true;
            expectEquals(r.card(Key::Func, 0).lines[1].text, juce::String("PLAY CLEAR P16 HOLD"));
            r.heldEdit(PLAY);
            expectEquals(r.undoLine(), juce::String("UNDO: CLEAR PAT 16"));
            expectEquals(r.text(), juce::String("CLEAR PAT 16"));
            r.somethingElse();
            r.undo();
            expectEquals(r.text(), juce::String("UNDO CLEAR PAT 16"));
            expectEquals(r.redoLine(), juce::String("REDO: CLEAR PAT 16"));
            for (int screen = 0; screen < NUM_SCREENS; ++screen)
            {
                r.mgr.setActiveScreen(screen);
                expectEquals(cardFault(r.card(Key::Func, 0)), juce::String(), screenNames[screen]);
            }
            // Func + Shift + Play is undo, not clear and not "stop"; Func + Shift + Record is redo, not copy or fill.
            r.mgr.setActiveScreen(STEP);
            r.seq.setSelectedTrack(7);
            r.track(7).setLength(1024);
            r.track(7).getStep(1023).trig = true;
            const auto before = r.state();
            r.redo();
            expect(!r.mgr.isFillHeld() && !r.seq.isRecording());
            expect(r.state() != before, "the pattern clear, redone");
            r.undo();
            expect(!r.seq.isPlaying());
            expectEquals(r.state(), before);
        }
    }
};

static UndoHistoryTests undoHistoryTests;
