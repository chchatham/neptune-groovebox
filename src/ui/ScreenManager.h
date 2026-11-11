#pragma once

#include <atomic>
#include "UiClock.h"
#include "../controller/DisplayRenderer.h"
#include "../controller/LaunchkeyController.h"
#include "../engine/ParamSpec.h"
#include "../sequencer/SequencerCore.h"
#include "../project/UndoHistory.h"
#include <juce_core/juce_core.h>
#include <array>
#include <functional>
#include <memory>
#include <vector>

class Screen
{
public:
    virtual ~Screen() = default;

    virtual juce::String getName() const = 0;
    virtual juce::String getSubPage() const { return {}; }

    virtual void render(DisplayRenderer& display) = 0;

    virtual void onEncoderChange(int encoderIndex, int value) { juce::ignoreUnused(encoderIndex, value); }

    // ================= Control grammar (docs/launchkey-mk4-control-map.md "CONTROL GRAMMAR") =================
    // Modifiers: ScreenManager sets them before every call into a screen. Shift + turn is fine, Func +
    // turn is coarse / snap, on every screen.
    enum class TurnMode { Plain, Fine, Coarse };
    void setModifiers(bool shift, bool func) { modShift = shift; modFunc = func; }
    bool shiftDown() const { return modShift; }
    bool funcDown() const { return modFunc; }
    TurnMode turnMode() const { return modFunc ? TurnMode::Coarse : (modShift ? TurnMode::Fine : TurnMode::Plain); }
    // The same, as the argument of ParamSpec::turned(current, clicks, mode).
    TurnStep turnStep() const { return modFunc ? TurnStep::Coarse : (modShift ? TurnStep::Fine : TurnStep::Plain); }

    // Paging. Bank buttons (beside the knobs) = onArrowLeft / onArrowRight = encoder page, nothing else.
    // Up / Down (beside the pads) = what the pads address (step page, scale page, list row).
    virtual void onPadPagePrev() {}
    virtual void onPadPageNext() {}
    // Func + Down / Up: the low / high pressure-range end of the knob turned last (PARAM, TRK FX).
    virtual void onFuncDown() {}
    virtual void onFuncUp() {}
    // Func + top-row pad: page keys. A screen with sections reports how many (max 8); key n jumps to
    // section n, and the same key again steps through that section's pages. -1 = none active.
    virtual int getPageKeyCount() const { return 0; }
    virtual void onPageKey(int index) { juce::ignoreUnused(index); }
    virtual int getActivePageKey() const { return -1; }

    // Pad colours: a screen that returns true owns all 16 (0..127 per channel; top row 0-7, bottom
    // row 8-15). False = the pad manager's own mode draws them (TRACKS, STEP).
    struct PadColour { juce::uint8 r = 0, g = 0, b = 0; };
    virtual bool getPadColours(std::array<PadColour, 16>& out) const { juce::ignoreUnused(out); return false; }

    // Bar screens: the full name and value of knob i ("CUTOFF 1.2k"), shown in the name bar just
    // after it is turned. Empty = nothing to show.
    virtual juce::String getEncoderReadout(int encoderIndex) const { juce::ignoreUnused(encoderIndex); return {}; }

    // Func + Record / Play / >: copy, clear, paste. Return true when the screen handled it; false
    // lets the app apply its default for the context (step, track, pattern, sound).
    virtual bool onCopy() { return false; }
    virtual bool onClear() { return false; }
    virtual bool onPaste() { return false; }
    // What this screen takes for itself with those three right now (a held step, its locks): the
    // names of it, longest first ("S7 LOCKS", "S7 LCKS"), and whether its paste has something to
    // paste. ScreenManager asks this - never the handlers - to decide who acts, and the help card
    // shows the same answer; while it returns true onCopy / onClear / onPaste must act.
    // `step` is the step of the selected track they change (so that exactly that step is kept for
    // the undo); -1 = the screen does not say, and the whole track's steps are kept.
    struct EditScope { juce::StringArray names; bool canPaste = true; int step = -1; };
    virtual bool getEditScope(EditScope& scope) const { juce::ignoreUnused(scope); return false; }

    // ---- Undo: a screen's own one-click destructive actions --------------------------------------
    // Copy / clear / paste are made undo steps by ScreenManager. Anything else a screen destroys at
    // once (an insert effect replaced, a step's locks cleared from a cell, the arpeggiator written
    // into the pattern) is wrapped in an UndoMark: the state named by the point is kept before the
    // change and, if the change changed it, the action joins the undo history. Without a host
    // (a screen used by itself in a test) a mark does nothing.
    struct UndoPoint
    {
        enum class Scope { Step, TrackSteps, Pattern, Sound, Custom };
        Scope scope = Scope::TrackSteps;
        int track = -1;                 // -1 = the selected track
        int step = -1;
        juce::String verb;              // "CLEAR"
        juce::StringArray names;        // "S7 LOCKS", "S7 LCKS", "S7": longest first
        int coalesceKey = 0;            // not 0: quick repeats with this key are one undo step
        // Scope::Custom: the screen's own state as numbers (a bounded list).
        std::function<std::vector<float>()> capture;
        std::function<void(const std::vector<float>&)> apply;
    };
    struct UndoHost
    {
        virtual ~UndoHost() = default;
        virtual void undoBegin(const UndoPoint& point) = 0;
        virtual void undoEnd() = 0;
        virtual int undoSelectedTrack() const = 0;
    };
    void setUndoHost(UndoHost* host) { undoHost = host; }
    struct UndoMark
    {
        UndoMark(const Screen& screen, const UndoPoint& point) : host(screen.undoHost) { if (host) host->undoBegin(point); }
        ~UndoMark() { if (host) host->undoEnd(); }
        UndoMark(const UndoMark&) = delete;
        UndoMark& operator=(const UndoMark&) = delete;
    private:
        UndoHost* host;
    };
    // "T3", for the names of an undo point (T1 without a host).
    juce::String undoTrackName() const { return "T" + juce::String((undoHost ? undoHost->undoSelectedTrack() : 0) + 1); }

    // ---- Help cards (a hold key held alone): what this screen adds to them -----------------------
    // The name of page key n, four characters at most ("VOL", "FLTR").
    virtual juce::String getPageKeyName(int index) const { juce::ignoreUnused(index); return {}; }
    // What Func + a bank button does when pressed at once (before the card shows).
    enum class BankJump { Page, FirstLast, Section };
    struct HelpFacts
    {
        bool stageCurves = false;       // Shift + turn on ATK / DEC / REL bends that stage (this page has such cells)
        bool pressureRange = false;     // Func + Up / Down set a pressure-range end
        BankJump bankJump = BankJump::Page;
        juce::String funcBottomRow;     // what Func + a bottom-row pad does ("SOLO"); empty = nothing special
        juce::String tapUpDown;         // what a tap of Up / Down moves here ("STEP PAGE"); empty = nothing
        bool allTracksTurn = false;     // Up / Down held + turn changes every track that has the cell
        bool stepLocks = false;         // a step can be held here, and > + turn then removes a lock
    };
    virtual void getHelpFacts(HelpFacts& facts) const { juce::ignoreUnused(facts); }
    // Hold a step + hold > + turn: remove that knob's lock on the held step.
    virtual void onEncoderLockClear(int encoderIndex) { juce::ignoreUnused(encoderIndex); }
    // Hold Up or Down ("Track") + turn: the same parameter on every track that has it. Default: just this track.
    virtual void onEncoderChangeAllTracks(int encoderIndex, int value) { onEncoderChange(encoderIndex, value); }
    // True while a step pad is held for editing (bank buttons then page the locks; > + turn clears one).
    virtual bool isStepHeld() const { return false; }

    // Four clicks clockwise on one knob, spread over at least 300 ms: the confirmation for destructive
    // actions. Call step() for every click of a knob that has such an action; it returns true once.
    struct TurnConfirm
    {
        static constexpr int STEPS = 4;
        static constexpr juce::int64 MIN_MS = 300;
        int knob = -1, steps = 0;
        juce::int64 firstMs = 0;
        // `now` is passed in tests; otherwise the clock is read.
        bool step(int knobIndex, int delta, juce::int64 now = -1)
        {
            if (now < 0) now = UiClock::nowMs();
            if (knobIndex != knob || delta <= 0) { knob = knobIndex; steps = 0; }
            if (delta <= 0) return false;
            if (steps == 0) firstMs = now;
            ++steps;
            if (steps >= STEPS && now - firstMs >= MIN_MS) { steps = 0; knob = -1; return true; }
            if (steps > STEPS) steps = STEPS;      // all clicks arrived too fast: wait for one more, later
            return false;
        }
        void cancel() { knob = -1; steps = 0; }
        juce::String progress() const { return juce::String::repeatedString(">", juce::jmin(steps, STEPS - 1)); }
    };
    virtual void onPadPress(int padIndex, int velocity) { juce::ignoreUnused(padIndex, velocity); }
    virtual void onPadRelease(int padIndex) { juce::ignoreUnused(padIndex); }
    virtual void onPadAftertouch(int padIndex, int pressure) { juce::ignoreUnused(padIndex, pressure); }
    virtual void onButtonPress(int buttonCC) { juce::ignoreUnused(buttonCC); }
    virtual void onButtonRelease(int buttonCC) { juce::ignoreUnused(buttonCC); }
    virtual void onArrowLeft() {}
    virtual void onArrowRight() {}
    // Up / Down arrows. By default they page like the encoder-bank buttons; screens with grouped
    // pages use them to jump between groups.
    virtual void onSectionPrev() { onArrowLeft(); }
    virtual void onSectionNext() { onArrowRight(); }
    virtual void onEnter() {}
    virtual void onActivate() {}
    virtual void onDeactivate() {}
    virtual void onShiftChanged(bool held) { juce::ignoreUnused(held); }
    // A screen that lays steps out on the pads shows the page that holds this step (after recording).
    virtual void showStep(int stepIndex) { juce::ignoreUnused(stepIndex); }

    virtual bool usesEncoderBar() const { return true; }

    // ---- The shared visual standard (drawn by ScreenManager, the same on every screen) ----------
    // Pages: a screen with more than one page reports them here and "n/N" appears at the right end
    // of the name bar. Screens do not write page numbers into their own text.
    virtual int getPageCount() const { return 1; }
    virtual int getPageIndex() const { return 0; }

    // Where the eight encoders are shown, so the one just turned can be highlighted:
    //   Bar   the 8-column bar at the bottom; encoder N is column N
    //   Grid  the 4 x 2 cell grid; encoders 1-4 top row, 5-8 bottom row
    //   None  the screen shows no per-encoder labels (e.g. a list browser)
    enum class EncoderLayout { Bar, Grid, None };
    virtual EncoderLayout getEncoderLayout() const
    {
        return usesEncoderBar() ? EncoderLayout::Bar : EncoderLayout::None;
    }
    virtual juce::String getEncoderName(int encoderIndex) const { juce::ignoreUnused(encoderIndex); return {}; }

    // Obsolete (the Shift + turn sub-parameter overlay is gone: Shift + turn is a fine turn).
    // ScreenManager never calls these; they remain only so existing overrides and tests compile.
    virtual void onSubParamChange(int encoderIndex, int value) { juce::ignoreUnused(encoderIndex, value); }
    virtual juce::String getSubParamName(int encoderIndex) const { juce::ignoreUnused(encoderIndex); return {}; }
    virtual bool hasSubParam(int encoderIndex) const { juce::ignoreUnused(encoderIndex); return false; }
    virtual float getSubParamNormalizedValue(int encoderIndex) const { juce::ignoreUnused(encoderIndex); return 0.0f; }

private:
    bool modShift = false, modFunc = false;
    UndoHost* undoHost = nullptr;
public:
    virtual bool wantsKeyboardEvents() const { return false; }
    virtual void onKeyboardNoteOn(int note, int velocity) { juce::ignoreUnused(note, velocity); }
    virtual void onKeyboardNoteOff(int note) { juce::ignoreUnused(note); }
};

class ScreenManager : public LaunchkeyController::Listener, private Screen::UndoHost
{
public:
    struct KeyboardListener
    {
        virtual ~KeyboardListener() = default;
        virtual void keyboardNoteOn(int note, int velocity) = 0;
        virtual void keyboardNoteOff(int note) = 0;
    };

    // What the buttons need from the sound side (arpeggiators, engines, the clock). The app binds
    // them to the AudioEngine (ui/GrammarHooks.h); any that is missing is simply skipped.
    struct EngineHooks
    {
        std::function<bool(int track)> arpEnabled;
        std::function<void(int track, bool on)> setArpEnabled;
        std::function<bool(int track)> arpLatch;
        std::function<void(int track, bool on)> setArpLatch;
            // Everything that defines how a track sounds (engine parameters, mixer, effects), read from
        // the engine / written back with a fresh engine of `type`. One sound per track, all patterns.
        std::function<ParamMap(int track)> captureSound;
        std::function<void(int track, EngineType type, const ParamMap& params)> applySound;
        std::function<void(float bpm)> tempoChanged;      // after tap tempo set the sequencer's tempo
        // The effects part of a sound: which ids belong to it (the DIRT / TALK / MODFX blocks, the
        // insert slots with their types and switches, the arp locks, and pressure ranges on those),
        // and their cleared values written into a sound. The mixer (VOL, PAN, sends) is not part of it.
        std::function<bool(int id)> isFxParam;
        std::function<void(ParamMap& sound)> fxDefaults;
        // The engine a track really has (the pattern's note of it otherwise).
        std::function<EngineType(int track)> engineType;
        // Runs `work` with the audio thread kept out (AudioEngine::structure()): whatever replaces
        // steps wholesale goes through it.
        std::function<void(const std::function<void()>& work)> structureLocked;
    };

    ScreenManager();

    void setDisplay(DisplayRenderer* display) { this->display = display; }
    void setController(LaunchkeyController* ctrl) { controller = ctrl; }
    void setKeyboardListener(KeyboardListener* l) { keyboardListener = l; }
    // The sequencer gives the layers their data (patterns, tracks, mutes, fill, tempo). Without one
    // the pattern key, the Track layer's mute and copy / clear / paste do nothing.
    void setSequencer(SequencerCore* seq);
    void setEngineHooks(EngineHooks hooks) { engineHooks = std::move(hooks); }
    // Tests pass their own clock (milliseconds).
    void setClock(std::function<juce::int64()> fn) { clock = std::move(fn); }

    using TrackSelectionCallback = std::function<void(int trackIndex)>;
    void setTrackSelectionCallback(TrackSelectionCallback cb) { trackSelectionCallback = std::move(cb); }

    // Called after the Arp button switched the selected track's arpeggiator (it is already done).
    using ArpToggleCallback = std::function<void(bool enabled)>;
    void setArpToggleCallback(ArpToggleCallback cb) { arpToggleCallback = std::move(cb); }

    using ScaleToggleCallback = std::function<void(bool enabled)>;
    void setScaleToggleCallback(ScaleToggleCallback cb) { scaleToggleCallback = std::move(cb); }

    // Shift + pad opens a screen. A pad without a screen does nothing at all while Shift is held.
    void setShiftPadScreen(int padIndex, int screenIndex)
    {
        if (padIndex >= 0 && padIndex < 16)
            shiftPadScreenIndex[padIndex] = screenIndex;
    }
    int getShiftPadScreen(int padIndex) const
    {
        return (padIndex >= 0 && padIndex < 16) ? shiftPadScreenIndex[padIndex] : -1;
    }

    // Which screens the Settings / Arp / Scale buttons open. Screen names change with their page
    // ("SETUP", "MAIN"), so the app gives indices; without them the screens are looked up by the
    // names "SETTINGS", "ARP", "SCALE".
    void setButtonScreens(int settingsIndex, int arpIndex, int scaleIndex)
    {
        settingsScreenIndex = settingsIndex;
        arpScreenIndex = arpIndex;
        scaleScreenIndex = scaleIndex;
    }

    // The screen that lays the steps out on the pads (STEP): shown when recording ends. Without it
    // the screen named "STEP" is used.
    void setStepScreen(int index) { stepScreenIndex = index; }
    // Scale lock: the name shown with it ("C MINOR") and what a key plays while it is on. The
    // app binds both to the scale pages; without them the lock is only a flag.
    void setScaleNameProvider(std::function<juce::String()> fn) { scaleName = std::move(fn); }
    void setKeySnap(std::function<int(int note)> fn) { keySnap = std::move(fn); }

    // What Func + Record / Play / > act on when the screen itself declines (returns false).
    // What Func + Record / Play / > act on when no step is held: nothing (the default), the selected
    // track's steps (STEP), the whole pattern (TRACKS), the selected track's sound (PARAM, FLTR) or
    // its effects (TRK FX).
    enum class CopyContext { None, Steps, Pattern, Sound, Fx };
    void setScreenContext(int screenIndex, CopyContext context);
    // Screens whose playing pads can be pinned (DRUMS, SCALE).
    void setPinnable(int screenIndex, bool pinnable);
    // The screen's name on the Shift help card's pad map, four characters at most ("PARM"). Without
    // one, the name the screen had when it was added, shortened.
    void setScreenMapName(int screenIndex, const juce::String& name);
    int getPinnedScreen() const { return pinnedIndex; }

    bool isArpEnabled() const { return arpOn(currentTrack()); }
    bool isScaleLocked() const { return scaleLocked; }
    // Same as pressing the Scale button until it reads `locked` (used by the SCALE screen's LK knob).
    void setScaleLocked(bool locked)
    {
        if (locked == scaleLocked) return;
        scaleLocked = locked;
        if (scaleToggleCallback) scaleToggleCallback(scaleLocked);
        needsRedraw = true;
    }

    void setTrackCount(int count) { trackCount = count; }
    void setSelectedTrack(int track) { selectedTrack = track; }
    int getSelectedTrack() const { return currentTrack(); }

    void addScreen(std::unique_ptr<Screen> screen);
    void setActiveScreen(int index);
    void setActiveScreen(const juce::String& name);
    Screen* getActiveScreen() const { return activeScreen; }
    int getActiveIndex() const { return activeIndex; }
    int getScreenCount() const { return (int) screens.size(); }

    void navigateNext();
    void navigatePrev();

    void requestRedraw();
    void redrawIfNeeded();

    // Controller messages arrive on the MIDI thread; the refresh timer and the Mac window run on
    // the message thread. Everything that changes or draws screen state holds this lock, so a
    // picture is never drawn by two threads at once or while a handler is half way through.
    // The message handlers take it themselves; other callers (the refresh timer) take it around
    // their work. Never call into the screens from the audio thread.
    juce::CriticalSection& getLock() { return uiLock; }

    void configureEncoderDisplays();
    void updateEncoderDisplayNames();

    // ---- Help cards ------------------------------------------------------------------------------
    // While a hold key (Shift, Func, >, Up / Down, Record) is held alone, a card takes the place of
    // the screen's picture: what that key does here. It appears once the key has been down for the
    // help delay with no other control pressed since, and any other control removes it for the rest
    // of that hold (and still acts as always). The one exception: while a card shows, the two bank
    // buttons page through that key's cards and do nothing else. Keys, strips and pad pressure
    // neither show nor remove it. A key that acts when it is let go (Up / Down: the tap) does not
    // act after a hold that showed its card, unless its table row says so (Record).
    enum class HelpMode { Off, Delayed, Instant };
    static constexpr juce::int64 HELP_DELAY_MS = 350;
    void setHelpMode(HelpMode mode) { helpMode = mode; needsRedraw = true; }
    HelpMode getHelpMode() const { return helpMode; }

    // Pattern = ">", Track = Up or Down (either button, one layer).
    enum class HelpKey { None = -1, Shift = 0, Func, Pattern, Track, Record, NumKeys };
    static constexpr int HELP_LINES = 6;            // body lines under the title
    static constexpr int HELP_LINE_CHARS = 21;
    static constexpr int HELP_PAD_CHARS = 4;
    struct HelpCard
    {
        juce::String title, pages;                  // "SHIFT + PADS", "1/3"
        struct Line { juce::String text; bool inverted = false; };
        std::array<Line, HELP_LINES> lines;
        // Names in the eight pad columns, on two lines so that neighbours do not touch: pads 1 3 5 7
        // on `line`, pads 2 4 6 8 on the line after it. Those two lines carry no other text.
        struct PadLabel { juce::String text; bool inverted = false, marked = false; };      // marked = "*" after it
        struct PadRow { int line = 0; std::array<PadLabel, 8> labels; };
        std::vector<PadRow> padRows;
    };
    // The key whose card is showing now (None = the screen's own picture), how many cards that key
    // has, which one is up, and the card itself.
    HelpKey getHelpCardKey() const;
    int getHelpCardCount(HelpKey key) const;
    int getHelpCardPage(HelpKey key) const;
    HelpCard getHelpCard(HelpKey key, int page) const;
    // The key's name ("Shift", ">", "Up / Down"), and whether letting it go after its card showed
    // still does what a plain press of it does (Record) - for the audit and the tests.
    static const char* getHelpKeyName(HelpKey key);
    static bool helpKeyActsOnReleaseAfterCard(HelpKey key);

    // Func + Record / Play / > = copy / clear / paste; with Shift held as well, Play = undo and
    // Record = redo. What one of them would do at this moment, worked out by the function that the
    // action itself goes by (so the EDIT card cannot say anything else): who acts, the verb ("COPY",
    // "CLEAR", "RESET", "PASTE", "UNDO", "REDO") and the names of what it acts on, longest first
    // ("T3 STEPS"; after UNDO / REDO: the action taken back or done again, "CLEAR T3 STEPS").
    enum class Edit { None, Copy, Clear, Paste, Undo, Redo };
    struct EditPlan
    {
        enum class Kind { Nothing, Undo, Redo, ScreenOwn, Steps, Pattern, Sound, Fx };
        Kind kind = Kind::Nothing;
        juce::String verb;
        juce::StringArray names;
        bool empty = false;                         // a paste with nothing copied: it will change nothing
        bool hold = false;                          // a whole pattern: the button must be held for HOLD_MS
        bool undoesCopy = false;                    // Undo of a copy: the clipboard goes back, no history entry
        int track = -1, step = -1;                  // what is kept for the undo (step -1 = the track's steps)
        // "CLEAR T3 STEPS", "CLEAR STEPS", "CLEAR": for the name bar, longest first.
        juce::StringArray wordings() const;
        // The longest name that fits in `maxChars`.
        juce::String nameWithin(int maxChars) const;
        // While the button is held: "CLEAR PATTERN 5 >>>..", ...; let go too early: "HOLD TO CLEAR PATTERN", ...
        juce::StringArray holdWordings(const juce::String& progress) const;
        juce::StringArray holdRefusal() const;
    };
    EditPlan getEditPlan(Edit edit) const;

    // ---- Undo history ----------------------------------------------------------------------------
    // Every clear, paste and reset, the screens' own one-click destructive actions (Screen::UndoMark),
    // > + Play and each recording pass are entries of it; Func + Shift + Play takes the newest back,
    // Func + Shift + Record does it again. It is emptied when a project is loaded or started.
    const UndoHistory& getUndoHistory() const { return history; }

    // A whole pattern is cleared or pasted over only when the button is held this long.
    static constexpr juce::int64 HOLD_MS = 500;
    static constexpr int HOLD_MARKS = 5;
    bool isEditHeld() const { return holdEdit != Edit::None; }
    // ">>>..": how far a held edit is (empty when none is held).
    juce::String holdProgress() const;

    // The app's 30 Hz refresh calls this (under getLock()): a held edit acts when its time is up,
    // and a recording pass that began or ended without a button of ours is noticed.
    void tick();

    // ---- State of the grammar (also for tests) ---------------------------------------------------
    bool isShiftHeld() const { return shiftHeld; }
    // A short message in the name bar (as after copy / paste), for things outside the screens.
    void showStatus(const juce::String& text) { showMessage(text); }
    // Recording: armed (it records while the transport runs), and the pad the running light is on
    // right now (-1 = no light: not recording, or stopped). The light is the selected track's step
    // within its 16-step page.
    bool isRecordArmed() const { return sequencer != nullptr ? sequencer->isRecording() : recordLocal; }
    bool isTransportRunning() const { return sequencer != nullptr ? sequencer->isPlaying() : playLocal; }
    int getRecordLightPad() const;
    bool isFuncHeld() const { return funcHeld; }
    bool isPatternKeyHeld() const { return patternKeyHeld; }
    bool isTrackLayerHeld() const { return trackKeysHeld != 0; }
    bool isKeyLatchOn() const { return holdActive; }
    bool isLatchOn() const;         // key latch, or the selected track's arp latch while that arp is on
    bool isFillHeld() const { return fillHeld; }

    // Pad colours for this moment. A hold layer wins (Shift = screen map, > = patterns, Up / Down =
    // tracks, Func = page keys on screens that have them), then the pinned screen, then the active
    // screen's own colours. PadMode = none of them: the pad manager's mode draws the pads.
    enum class PadSource { Layer, Pinned, Screen, PadMode };
    PadSource getPadFrame(std::array<Screen::PadColour, 16>& out) const;

    // Name bar, as text: left part ("TRACKS T1"), middle (tags, then the message / knob readout /
    // the screen's status text) and the page mark ("2/5"). Drawn from exactly this.
    struct NameBar { juce::String left, tags, text, pages; };
    NameBar composeNameBar() const;
    // "REC FREE" / "REC QUANT", "ARP", "LATCH", "SCALE", "PIN", "FILL" (those that are on)
    juce::StringArray getNameBarTags() const;

    // Call after a project was loaded (and once at start): > + Play reloads a pattern from here
    // until > + Record keeps a newer copy.
    void projectLoaded();

    struct TransportListener
    {
        virtual ~TransportListener() = default;
        virtual void playPressed() = 0;
        virtual void stopPressed() = 0;
        virtual void recordPressed() = 0;
    };

    void setTransportListener(TransportListener* l) { transportListener = l; }

    // LaunchkeyController::Listener
    void controlChangeReceived(int channel, int cc, int value) override;
    void noteOnReceived(int channel, int note, int velocity) override;
    void noteOffReceived(int channel, int note) override;
    void aftertouchReceived(int channel, int note, int pressure) override;
    void padModeChanged(int mode) override;

    // Button CCs — verified from hardware MIDI capture (Neptune firmware NPD).
    static constexpr int CC_SHIFT = 63;         // channel 7
    static constexpr int SHIFT_CHANNEL = 7;
    static constexpr int CC_PATTERN_KEY = 104;  // ">", right of the top pad row
    static constexpr int CC_FUNC = 105;
    static constexpr int CC_ARROW_UP = 106;
    static constexpr int CC_ARROW_DOWN = 107;
    static constexpr int CC_SHIFT_UP = 102;     // older firmware's Shift+Up — previous track
    static constexpr int CC_SHIFT_DOWN = 103;   // older firmware's Shift+Down — next track
    static constexpr int BUTTON_CHANNEL = 1;
    static constexpr int CC_PLAY = 0x73;        // 115
    static constexpr int CC_RECORD = 0x75;      // 117
    static constexpr int CC_BANK_UP = 51;       // small button right of the encoders (upper)
    static constexpr int CC_BANK_DOWN = 52;     // (lower)
    // Settings / Arp / Scale: plain buttons on channel 1, 127 on press and 0 on release.
    static constexpr int NP_CC_SETTINGS = 108;
    static constexpr int NP_CC_ARP = 109;
    static constexpr int NP_CC_SCALE = 110;
    // Internal identity of the Arp / Scale buttons (also used by the virtual controller and tests).
    static constexpr int CC_ARP = 0x49;          // 73, channel 7
    static constexpr int CC_SCALE = 0x4A;        // 74, channel 7
    static constexpr int FEATURE_CHANNEL = 7;

    static constexpr juce::int64 READOUT_MS = 700;       // knob readout / highlight after a turn
    static constexpr juce::int64 MESSAGE_MS = 1000;      // "COPY TRK", "126 BPM"
    static constexpr juce::int64 LONG_MESSAGE_MS = 2000; // "SCALE LOCK ON", "REC QUANT"
    static constexpr juce::int64 TAP_RESET_MS = 2000;

private:
    void renderFrame();
    void drawNameBar(DisplayRenderer& disp);
    void drawEncoderHighlight(DisplayRenderer& disp);

    juce::int64 nowMs() const { return clock ? clock() : UiClock::nowMs(); }
    // Every call into a screen goes through this, so the screen always knows the modifiers.
    Screen* mods(Screen* s) const { if (s != nullptr) s->setModifiers(shiftHeld, funcHeld); return s; }
    Screen* screenAt(int index) const
    {
        return (index >= 0 && index < (int) screens.size()) ? screens[(size_t) index].get() : nullptr;
    }
    int findScreen(const juce::String& name) const;
    int roleScreen(int configured, const char* name) const { return configured >= 0 ? configured : findScreen(name); }

    int currentTrack() const { return sequencer != nullptr ? sequencer->getSelectedTrack() : selectedTrack; }
    int currentTrackCount() const { return sequencer != nullptr ? sequencer->getPattern().getTrackCount() : trackCount; }
    void selectTrack(int track);
    bool arpOn(int track) const { return engineHooks.arpEnabled ? engineHooks.arpEnabled(track) : arpEnabled; }

    void handlePadPress(int padIndex, int velocity);
    void handleArpButton();
    void handleLatch();
    void handlePlay();
    void handleRecord(bool pressed);
    void tapTempo();
    void showMessage(const juce::String& text, juce::int64 durationMs = MESSAGE_MS);
    // The first of these that fits beside the screen's name is shown. `wide`: it may take the
    // whole name bar, the screen's name included (what an undo took back, on whatever screen).
    void showMessage(const juce::StringArray& alternatives, juce::int64 durationMs, bool wide = false);

    // Transport and recording (Play, Record, Record + Play).
    void toggleTransport();
    void setRecordArmed(bool armed);
    void recordPlayPressed();
    void leaveRecording();
    juce::String recordModeText() const;
    void scaleLockChanged();

    // Copy / clear / paste, undo / redo.
    void handleEdit(Edit edit, int cc = -1);
    void performEdit(Edit edit, const EditPlan& plan);
    void releaseHeldEdit(bool timeIsUp);
    static const char* editVerb(Edit edit);
    struct SoundClip { EngineType type = EngineType::UnifiedVoice; ParamMap params; };
    SoundClip captureSoundOf(int track) const;
    void applySoundTo(int track, const SoundClip& clip);
    bool isFxParam(int id) const;
    EngineType engineTypeOf(int track) const;
    void locked(const std::function<void()>& work);

    // Undo history (Screen::UndoHost is how the screens' own actions join it).
    void undoBegin(const Screen::UndoPoint& point) override;
    void undoEnd() override;
    int undoSelectedTrack() const override { return currentTrack(); }
    bool captureBefore(const Screen::UndoPoint& point, UndoEntry& entry) const;
    bool entryChanged(UndoEntry& entry) const;
    bool entryValid(const UndoEntry& entry, bool redo) const;
    void swapEntry(UndoEntry& entry, bool redo);
    const UndoEntry* nextValid(bool redo) const;
    void performUndo(bool redo);
    void afterControl();
    void openRecordingPass();
    void closeRecordingPass();
    std::vector<int> passChanges() const;
    UndoEntry passHeader() const;
    void buildFuncUndo(HelpCard& card) const;

    // Help cards.
    bool holdDownBesides(HelpKey key) const;
    void helpKeyChanged(HelpKey key, bool pressed);
    bool helpCardTookHold(HelpKey key) const;
    void dismissHelp();
    void drawHelpCard(DisplayRenderer& disp, const HelpCard& card) const;
    juce::String mapNameOf(int screenIndex) const;
    void buildShiftPads(HelpCard& card) const;
    void buildShiftButtons(HelpCard& card) const;
    void buildShiftKnobs(HelpCard& card) const;
    void buildFuncEdit(HelpCard& card) const;
    void buildFuncPads(HelpCard& card) const;
    void buildFuncKnobs(HelpCard& card) const;
    void buildPatternPads(HelpCard& card) const;
    void buildPatternButtons(HelpCard& card) const;
    void buildTrackPads(HelpCard& card) const;
    void buildTrackKnobs(HelpCard& card) const;
    void buildRecord(HelpCard& card) const;
    // One row per hold key: its name, how long it must be held alone before its card shows, whether
    // letting it go after the card showed still does the key's own release action, and its cards.
    // "Alone" is the same for every key (holdDownBesides). A new hold layer gets cards by adding a
    // HelpKey, its row, and one helpKeyChanged() call where the key goes down and one where it
    // comes up.
    struct HelpCardDef { const char* title; void (ScreenManager::*build)(HelpCard&) const; };
    struct HelpKeyDef
    {
        HelpKey key;
        const char* name;
        juce::int64 delayMs;
        bool releaseActsAfterCard;
        std::vector<HelpCardDef> cards;
    };
    static const HelpKeyDef& helpKeyDef(HelpKey key);
    static const std::vector<HelpCardDef>& helpCardsFor(HelpKey key) { return helpKeyDef(key).cards; }
    void restorePattern(const Pattern& from);
    void keepPatternCopy();
    void reloadPattern();

    DisplayRenderer* display = nullptr;
    LaunchkeyController* controller = nullptr;
    KeyboardListener* keyboardListener = nullptr;
    TransportListener* transportListener = nullptr;
    SequencerCore* sequencer = nullptr;
    EngineHooks engineHooks;
    std::function<juce::int64()> clock;
    TrackSelectionCallback trackSelectionCallback;
    ArpToggleCallback arpToggleCallback;
    ScaleToggleCallback scaleToggleCallback;
    int selectedTrack = 0;
    int trackCount = 8;
    bool arpEnabled = false;        // only used when no engine hooks are set
    bool scaleLocked = false;
    int shiftPadScreenIndex[16] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    int settingsScreenIndex = -1, arpScreenIndex = -1, scaleScreenIndex = -1, stepScreenIndex = -1;
    std::function<juce::String()> scaleName;
    std::function<int(int)> keySnap;
    std::array<juce::int8, 128> keySounding {};     // what each held key plays (scale lock), -1 = itself

    std::vector<std::unique_ptr<Screen>> screens;
    struct ScreenTraits { CopyContext context = CopyContext::None; bool pinnable = false; juce::String mapName; };
    std::vector<ScreenTraits> traits;
    Screen* activeScreen = nullptr;
    int activeIndex = -1;
    std::atomic<bool> needsRedraw { false };
    juce::CriticalSection uiLock;

    static constexpr int NAME_BAR_HEIGHT = 10;

    // Modifiers and hold layers.
    bool shiftHeld = false;
    int pinPad = -1;
    juce::int64 pinPressMs = 0;
    static constexpr juce::int64 PIN_HOLD_MS = 500;
    bool funcHeld = false;
    bool scaleHeld = false;
    bool patternKeyHeld = false;
    int trackKeysHeld = 0;          // bit 0 = Up, bit 1 = Down, held as the Track layer
    bool trackLayerUsed = false;    // a pad or knob was used under it: its release does not page
    bool fillHeld = false;
    bool recordHeld = false;        // plain Record is down: Play makes it Record + Play
    bool recordUsed = false;        // ... and it did: the release is not a plain Record
    bool playHeld = false;
    bool recordLocal = false, playLocal = false;    // only used without a sequencer

    // Help cards: the hold key that went down alone and when; any other control dismisses its card
    // for the rest of that hold (helpShown: it had been up for the reader by then). The page of each
    // key's card set is kept until the screen changes.
    HelpMode helpMode = HelpMode::Delayed;
    HelpKey helpKey = HelpKey::None;
    juce::int64 helpPressMs = 0;
    bool helpDismissed = false;
    bool helpShown = false;
    std::array<int, (size_t) HelpKey::NumKeys> helpPage {};

    // Pads: where each held pad's press went (its release and pressure follow), null = swallowed
    // by a layer.
    std::array<Screen*, 16> padTarget {};
    std::array<bool, 16> padDown {};
    int pinnedIndex = -1;

    // The encoder turned last and when, for the highlight and the name-bar readout.
    int lastEncoderIndex = -1;
    juce::int64 lastEncoderMs = 0;

    juce::StringArray message;      // alternatives, longest first
    juce::int64 messageMs = 0;
    juce::int64 messageDurationMs = MESSAGE_MS;
    bool messageWide = false;

    // Tap tempo.
    static constexpr int MAX_TAPS = 8;
    std::array<juce::int64, MAX_TAPS> taps {};
    int tapCount = 0;

    // Copy / clear / paste.
    struct StepsClip { std::vector<Step> steps; int length = 16; float speed = 1.0f; };
    struct Clipboards
    {
        bool hasPattern = false, hasSound = false, hasSteps = false, hasFx = false;
        Pattern pattern { 0 };
        SoundClip sound;
        StepsClip steps;
        ParamMap fx;
    };
    Clipboards clips, clipsBeforeCopy;
    // The same combination again, with no other control in between, takes the edit back: a copy
    // (the clipboard as it was), or the history's newest entry if it is the one that edit made.
    Edit lastEdit = Edit::None;
    int armedSerial = -1;

    // The undo history, the action being recorded into it (between undoBegin and undoEnd), and the
    // recording pass that is running (the pattern as it was when it began).
    UndoHistory history;
    UndoEntry pendingEntry;
    int pendingDepth = 0;
    bool pendingOpen = false;
    int lastPushedSerial = -1;
    bool passOpen = false, passSuspended = false;
    Pattern passPattern { 0 };
    int passPatternIndex = -1;

    // A whole-pattern clear or paste waiting for its button to have been held long enough.
    Edit holdEdit = Edit::None;
    int holdCc = -1;
    juce::int64 holdStartMs = 0;
    juce::StringArray holdNames;

    // > + Record / > + Play.
    std::vector<Pattern> keptPatterns, loadedPatterns;
    std::vector<bool> keptValid, loadedValid;

    // Latch: keys stay down after their release until the next chord starts or latch goes off.
    bool holdActive = false;
    static constexpr int MAX_HELD_NOTES = 16;
    int heldNotes[MAX_HELD_NOTES] = {};
    int heldNoteCount = 0;
    int physicallyHeldNotes[MAX_HELD_NOTES] = {};
    int physicallyHeldCount = 0;

    void addPhysicalNote(int note);
    void removePhysicalNote(int note);
    bool isLatchedNote(int note) const;
    void sendKeyOff(int note);
    void releaseHeldNotes();
};
