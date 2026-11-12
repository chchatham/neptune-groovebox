#include "ScreenManager.h"
#include "ParamGrid.h"
#include "ScreenLayout.h"
#include "BitmapFont.h"
#include "NoteUtils.h"

namespace
{
    using Colour = Screen::PadColour;
    constexpr Colour OFF        { 0, 0, 0 };
    constexpr Colour DIM        { 10, 10, 10 };
    constexpr Colour BRIGHT     { 127, 127, 127 };
    constexpr Colour PIN        { 0, 100, 127 };
    constexpr Colour PAT_NOW    { 0, 127, 0 };
    constexpr Colour PAT_QUEUED { 127, 90, 0 };
    constexpr Colour PAT_USED   { 0, 14, 4 };
    constexpr Colour MUTED      { 127, 0, 0 };
    constexpr Colour UNMUTED    { 0, 18, 0 };
    constexpr Colour SOLOED     { 0, 0, 127 };
    constexpr Colour KEY        { 14, 8, 0 };
    constexpr Colour KEY_ACTIVE { 127, 80, 0 };
    constexpr Colour REC_LIGHT  { 127, 0, 0 };      // the running light while recording

    int padIndexOfNote(int note)
    {
        if (note >= LaunchkeyController::PAD_TOP_ROW_START
            && note < LaunchkeyController::PAD_TOP_ROW_START + LaunchkeyController::PADS_PER_ROW)
            return note - LaunchkeyController::PAD_TOP_ROW_START;
        if (note >= LaunchkeyController::PAD_BOTTOM_ROW_START
            && note < LaunchkeyController::PAD_BOTTOM_ROW_START + LaunchkeyController::PADS_PER_ROW)
            return note - LaunchkeyController::PAD_BOTTOM_ROW_START + LaunchkeyController::PADS_PER_ROW;
        return -1;
    }

    bool patternHasTrigs(const Pattern& pattern)
    {
        for (auto& track : pattern.tracks)
            for (int s = 0; s < track.length && s < (int) track.steps.size(); ++s)
                if (track.steps[(size_t) s].trig)
                    return true;
        return false;
    }

    // A name cut to `maxChars` for the help cards: spaces go, then vowels from the right (never the
    // first letter), then the end. "TRACK" -> "TRCK", "MODFX" -> "MDFX", "AT ROUTE" -> "ATRT".
    juce::String shortName(juce::String name, int maxChars)
    {
        name = name.removeCharacters(" ").toUpperCase();
        for (int i = name.length() - 1; i > 0 && name.length() > maxChars; --i)
            if (juce::String("AEIOU").containsChar(name[i]))
                name = name.substring(0, i) + name.substring(i + 1);
        return name.substring(0, maxChars);
    }

    // What a pattern holds of its own: the steps, lengths and speeds of its tracks and the swing.
    // The sound of a track is one per track for all patterns and is not part of this.
    void copyPatternData(const Pattern& from, Pattern& to)
    {
        to.swing = from.swing;
        const int count = juce::jmin(from.getTrackCount(), to.getTrackCount());
        for (int t = 0; t < count; ++t)
        {
            auto& dst = to.tracks[(size_t) t];
            const auto& src = from.tracks[(size_t) t];
            dst.steps = src.steps;
            dst.length = src.length;
            dst.speedMultiplier = src.speedMultiplier;
        }
    }
}

ScreenManager::ScreenManager()
{
    keySounding.fill(-1);
}

void ScreenManager::setSequencer(SequencerCore* seq)
{
    sequencer = seq;
    keptPatterns.clear();
    keptValid.assign((size_t) SequencerCore::MAX_PATTERNS, false);
    loadedPatterns.clear();
    loadedValid.assign((size_t) SequencerCore::MAX_PATTERNS, false);
    if (sequencer != nullptr)
    {
        keptPatterns.resize((size_t) SequencerCore::MAX_PATTERNS, Pattern(0));
        loadedPatterns.resize((size_t) SequencerCore::MAX_PATTERNS, Pattern(0));
    }
}

void ScreenManager::projectLoaded()
{
    // Nothing of the project that was there can be taken back into this one.
    history.clear();
    lastEdit = Edit::None;
    armedSerial = -1;
    holdEdit = Edit::None;
    holdCc = -1;
    passOpen = passSuspended = false;
    passPattern = Pattern(0);
    if (sequencer == nullptr)
        return;
    for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
    {
        loadedPatterns[(size_t) p] = sequencer->getPattern(p).duplicate();
        loadedValid[(size_t) p] = true;
        keptValid[(size_t) p] = false;
    }
}

void ScreenManager::addScreen(std::unique_ptr<Screen> screen)
{
    // Default context for copy / clear / paste, from the screen's name when it is added.
    ScreenTraits t;
    const auto name = screen->getName();
    if (name == "TRACKS")                        t.context = CopyContext::Pattern;
    else if (name == "PARAM" || name == "FLTR")   t.context = CopyContext::Sound;
    else if (name == "TRK FX")                    t.context = CopyContext::Fx;
    else if (name == "STEP")                      t.context = CopyContext::Steps;
    t.mapName = shortName(name, HELP_PAD_CHARS);
    traits.push_back(t);
    screen->setUndoHost(this);
    screens.push_back(std::move(screen));

    if (activeScreen == nullptr && !screens.empty())
        setActiveScreen(0);
}

void ScreenManager::setScreenContext(int screenIndex, CopyContext context)
{
    if (screenIndex >= 0 && screenIndex < (int) traits.size())
        traits[(size_t) screenIndex].context = context;
}

void ScreenManager::setPinnable(int screenIndex, bool pinnable)
{
    if (screenIndex >= 0 && screenIndex < (int) traits.size())
        traits[(size_t) screenIndex].pinnable = pinnable;
}

void ScreenManager::setScreenMapName(int screenIndex, const juce::String& name)
{
    if (screenIndex >= 0 && screenIndex < (int) traits.size())
        traits[(size_t) screenIndex].mapName = shortName(name, HELP_PAD_CHARS);
}

int ScreenManager::findScreen(const juce::String& name) const
{
    for (int i = 0; i < (int) screens.size(); ++i)
        if (screens[(size_t) i]->getName() == name)
            return i;
    return -1;
}

void ScreenManager::setActiveScreen(int index)
{
    if (index < 0 || index >= (int) screens.size())
        return;

    if (activeScreen)
        mods(activeScreen)->onDeactivate();

    if (index != activeIndex)
        helpPage.fill(0);           // another screen: each key's cards start at the first again
    activeIndex = index;
    activeScreen = screens[(size_t) index].get();
    mods(activeScreen)->onActivate();
    lastEncoderIndex = -1;
    needsRedraw = true;
    updateEncoderDisplayNames();
}

void ScreenManager::setActiveScreen(const juce::String& name)
{
    setActiveScreen(findScreen(name));
}

void ScreenManager::navigateNext()
{
    if (screens.empty())
        return;
    setActiveScreen((activeIndex + 1) % (int) screens.size());
}

void ScreenManager::navigatePrev()
{
    if (screens.empty())
        return;
    int next = activeIndex - 1;
    if (next < 0)
        next = (int) screens.size() - 1;
    setActiveScreen(next);
}

void ScreenManager::requestRedraw()
{
    needsRedraw = true;
}

void ScreenManager::redrawIfNeeded()
{
    if (!needsRedraw || !display || !activeScreen)
        return;

    // If another thread is in a handler or drawing, leave the flag set: the refresh timer draws
    // within a thirtieth of a second. (Trying, not waiting, also rules out a lock-order deadlock
    // with code that asks for a redraw while it holds an engine lock.)
    const juce::ScopedTryLock ui(uiLock);
    if (!ui.isLocked())
        return;
    needsRedraw = false;
    renderFrame();
}

void ScreenManager::selectTrack(int track)
{
    const int count = currentTrackCount();
    if (count <= 0)
        return;
    track = ((track % count) + count) % count;
    selectedTrack = track;
    if (sequencer != nullptr)
        sequencer->setSelectedTrack(track);
    if (trackSelectionCallback)
        trackSelectionCallback(track);
    needsRedraw = true;
}

void ScreenManager::showMessage(const juce::String& text, juce::int64 durationMs)
{
    showMessage(juce::StringArray(text), durationMs);
}

void ScreenManager::showMessage(const juce::StringArray& alternatives, juce::int64 durationMs, bool wide)
{
    message = alternatives;
    messageMs = nowMs();
    messageDurationMs = durationMs;
    messageWide = wide;
    needsRedraw = true;
}

bool ScreenManager::isLatchOn() const
{
    if (holdActive)
        return true;
    const int t = currentTrack();
    return arpOn(t) && engineHooks.arpLatch && engineHooks.arpLatch(t);
}

// =================================================================================================
// Buttons and encoders
// =================================================================================================
void ScreenManager::controlChangeReceived(int channel, int cc, int value)
{
    const juce::ScopedLock ui(uiLock);
    const bool pressed = value > 0;
    // Whatever this message does: afterwards, see whether a recording pass began or ended with it.
    struct After { ScreenManager& m; ~After() { m.afterControl(); } } after { *this };

    // Neptune firmware buttons: translate to the internal Arp / Scale identity.
    if (channel == BUTTON_CHANNEL)
    {
        if (cc == NP_CC_ARP)        { channel = FEATURE_CHANNEL; cc = CC_ARP; }
        else if (cc == NP_CC_SCALE) { channel = FEATURE_CHANNEL; cc = CC_SCALE; }
    }

    // ---- Modifiers -------------------------------------------------------------------------------
    if (channel == SHIFT_CHANNEL && cc == CC_SHIFT)
    {
        shiftHeld = pressed;
        helpKeyChanged(HelpKey::Shift, pressed);
        if (activeScreen)
            mods(activeScreen)->onShiftChanged(shiftHeld);
        needsRedraw = true;
        redrawIfNeeded();
        return;
    }

    if (channel == BUTTON_CHANNEL && cc == CC_FUNC)
    {
        funcHeld = pressed;
        if (!pressed)
            releaseHeldEdit(false);     // a held clear / paste ends with Func as with its button
        helpKeyChanged(HelpKey::Func, pressed);
        if (activeScreen)
        {
            // Screens read funcDown(); the button itself is still passed on for older code.
            mods(activeScreen);
            if (pressed) activeScreen->onButtonPress(cc);
            else         activeScreen->onButtonRelease(cc);
        }
        needsRedraw = true;
        redrawIfNeeded();
        return;
    }

    // ---- Help card -------------------------------------------------------------------------------
    // While a card shows, the bank buttons page through that key's cards (round and round) and do
    // nothing else. Any other button or knob removes the card for the rest of the hold and then
    // acts as always. (The strips and anything unknown leave it alone.)
    const bool bankButton = channel == BUTTON_CHANNEL && (cc == CC_BANK_UP || cc == CC_BANK_DOWN);
    if (bankButton && getHelpCardKey() != HelpKey::None)
    {
        if (pressed)
        {
            const auto key = getHelpCardKey();
            const int count = getHelpCardCount(key);
            helpPage[(size_t) key] = (getHelpCardPage(key) + (cc == CC_BANK_UP ? count - 1 : 1)) % juce::jmax(1, count);
            needsRedraw = true;
            redrawIfNeeded();
        }
        return;
    }
    {
        const bool button = (channel == BUTTON_CHANNEL
                             && (cc == NP_CC_SETTINGS || cc == CC_PATTERN_KEY || cc == CC_ARROW_UP || cc == CC_ARROW_DOWN
                                 || cc == CC_SHIFT_UP || cc == CC_SHIFT_DOWN || cc == CC_PLAY || cc == CC_RECORD || bankButton))
                            || (channel == FEATURE_CHANNEL && (cc == CC_ARP || cc == CC_SCALE));
        const bool encoder = (channel == 16 || channel == 1) && cc >= 0x15 && cc <= 0x1C;
        // A button that is about to be held for a whole-pattern clear / paste leaves the card: its
        // line shows the hold filling.
        const bool startsHold = channel == BUTTON_CHANNEL && funcHeld && !shiftHeld
                                && ((cc == CC_PLAY && !patternKeyHeld && getEditPlan(Edit::Clear).hold)
                                    || (cc == CC_PATTERN_KEY && getEditPlan(Edit::Paste).hold));
        if (pressed && (button || encoder) && !startsHold)
            dismissHelp();
    }

    // ---- Settings: open the screen; Func + Settings = tap tempo -----------------------------------
    if (channel == BUTTON_CHANNEL && cc == NP_CC_SETTINGS)
    {
        if (pressed)
        {
            if (funcHeld)
                tapTempo();
            else
            {
                lastEdit = Edit::None;
                setActiveScreen(roleScreen(settingsScreenIndex, "SETTINGS"));
            }
            needsRedraw = true;
            redrawIfNeeded();
        }
        return;
    }

    // ---- Scale: scale lock; Shift + Scale = the SCALE screen ---------------------------------------
    if (channel == FEATURE_CHANNEL && cc == CC_SCALE)
    {
        scaleHeld = pressed;
        if (pressed)
        {
            lastEdit = Edit::None;
            if (shiftHeld)
            {
                setActiveScreen(roleScreen(scaleScreenIndex, "SCALE"));
            }
            else
            {
                scaleLocked = !scaleLocked;
                if (scaleToggleCallback)
                    scaleToggleCallback(scaleLocked);
                juce::Logger::writeToLog("[Scale] " + juce::String(scaleLocked ? "ON" : "OFF"));
                scaleLockChanged();
            }
            needsRedraw = true;
            redrawIfNeeded();
        }
        return;
    }

    // ---- Arp: the ARP screen, then arp on / off; Shift + Arp = latch -------------------------------
    if (channel == FEATURE_CHANNEL && cc == CC_ARP)
    {
        if (pressed)
        {
            lastEdit = Edit::None;
            if (shiftHeld) handleLatch();
            else           handleArpButton();
            needsRedraw = true;
            redrawIfNeeded();
        }
        return;
    }

    // ---- Older firmware's Shift+Up / Shift+Down codes: track -1 / +1 --------------------------------
    if (channel == BUTTON_CHANNEL && (cc == CC_SHIFT_UP || cc == CC_SHIFT_DOWN))
    {
        if (pressed)
        {
            lastEdit = Edit::None;
            selectTrack(currentTrack() + (cc == CC_SHIFT_UP ? -1 : 1));
            redrawIfNeeded();
        }
        return;
    }

    // ---- Up / Down (beside the pads) -------------------------------------------------------------
    //   Shift +   track -1 / +1
    //   Func +    the screen's onFuncUp / onFuncDown
    //   held      the Track layer (pads select / mute tracks, a knob changes every track)
    //   tapped    onPadPagePrev / onPadPageNext, on release, if nothing was used under it
    if (channel == BUTTON_CHANNEL && (cc == CC_ARROW_UP || cc == CC_ARROW_DOWN))
    {
        const bool up = (cc == CC_ARROW_UP);
        const int bit = up ? 1 : 2;
        if (pressed)
        {
            lastEdit = Edit::None;
            if (shiftHeld)
                selectTrack(currentTrack() + (up ? -1 : 1));
            else if (funcHeld)
            {
                if (activeScreen)
                {
                    if (up) mods(activeScreen)->onFuncUp();
                    else    mods(activeScreen)->onFuncDown();
                }
            }
            else
            {
                if (trackKeysHeld == 0)
                {
                    trackLayerUsed = false;
                    helpKeyChanged(HelpKey::Track, true);
                }
                trackKeysHeld |= bit;
            }
        }
        else if ((trackKeysHeld & bit) != 0)
        {
            // A hold that showed the Track card was a look at the card, not a tap: it does not page.
            if (helpCardTookHold(HelpKey::Track))
                trackLayerUsed = true;
            trackKeysHeld &= ~bit;
            if (trackKeysHeld == 0)
                helpKeyChanged(HelpKey::Track, false);
            if (!trackLayerUsed && activeScreen)
            {
                if (up) mods(activeScreen)->onPadPagePrev();
                else    mods(activeScreen)->onPadPageNext();
                trackLayerUsed = true;      // the other arrow, if also held, does not page as well
            }
        }
        needsRedraw = true;
        redrawIfNeeded();
        return;
    }

    // ---- Bank buttons (beside the knobs): encoder page -1 / +1 and nothing else --------------------
    //   Func + bank: section jump (first / last page on screens without sections)
    if (channel == BUTTON_CHANNEL && (cc == CC_BANK_UP || cc == CC_BANK_DOWN))
    {
        if (pressed && activeScreen)
        {
            lastEdit = Edit::None;
            const bool previous = (cc == CC_BANK_UP);
            mods(activeScreen);
            if (funcHeld)
            {
                if (previous) activeScreen->onSectionPrev();
                else          activeScreen->onSectionNext();
            }
            else
            {
                if (previous) activeScreen->onArrowLeft();
                else          activeScreen->onArrowRight();
            }
            needsRedraw = true;
            redrawIfNeeded();
        }
        return;
    }

    // ---- ">": the pattern key. A plain tap does nothing. Func + ">" = paste -------------------------
    if (channel == BUTTON_CHANNEL && cc == CC_PATTERN_KEY)
    {
        if (pressed && funcHeld)
            handleEdit(Edit::Paste, cc);
        else
        {
            if (!pressed && holdCc == cc)
                releaseHeldEdit(false);
            patternKeyHeld = pressed;
            helpKeyChanged(HelpKey::Pattern, pressed);
        }
        needsRedraw = true;
        redrawIfNeeded();
        return;
    }

    // ---- Encoders (channel 16; channel 1 as fallback), two's-complement steps ----------------------
    if ((channel == 16 || channel == 1) && cc >= 0x15 && cc <= 0x1C)
    {
        if (!activeScreen)
            return;
        const int encoderIndex = cc - 0x15;
        lastEdit = Edit::None;
        mods(activeScreen);

        if (trackKeysHeld != 0)
        {
            trackLayerUsed = true;
            activeScreen->onEncoderChangeAllTracks(encoderIndex, value);
        }
        else if (patternKeyHeld && activeScreen->isStepHeld())
        {
            activeScreen->onEncoderLockClear(encoderIndex);
        }
        else
        {
            activeScreen->onEncoderChange(encoderIndex, value);
        }

        lastEncoderIndex = encoderIndex;
        lastEncoderMs = nowMs();
        needsRedraw = true;
        redrawIfNeeded();
        return;
    }

    // ---- Play / Record -----------------------------------------------------------------------------
    if (channel == BUTTON_CHANNEL && (cc == CC_PLAY || cc == CC_RECORD))
    {
        if (cc == CC_PLAY)
        {
            playHeld = pressed;
            if (pressed) handlePlay();
            else if (holdCc == cc) releaseHeldEdit(false);
        }
        else
            handleRecord(pressed);
        needsRedraw = true;
        redrawIfNeeded();
        return;
    }

    // Anything else goes to the active screen as a button.
    if (!activeScreen)
        return;
    mods(activeScreen);
    if (pressed)
    {
        activeScreen->onButtonPress(cc);
        needsRedraw = true;
        redrawIfNeeded();
    }
    else
    {
        activeScreen->onButtonRelease(cc);
    }
}

void ScreenManager::handleArpButton()
{
    // First press: the ARP screen. Pressed again there: the selected track's arpeggiator on / off.
    const int arpIndex = roleScreen(arpScreenIndex, "ARP");
    if (arpIndex >= 0 && arpIndex != activeIndex)
    {
        setActiveScreen(arpIndex);
        return;
    }

    const int track = currentTrack();
    const bool on = !arpOn(track);
    if (engineHooks.setArpEnabled) engineHooks.setArpEnabled(track, on);
    else                           arpEnabled = on;
    if (arpToggleCallback)
        arpToggleCallback(on);
    juce::Logger::writeToLog("[Arp] " + juce::String(on ? "ON" : "OFF"));
}

void ScreenManager::handleLatch()
{
    // The arp's latch while the selected track's arp is on, the key latch otherwise.
    const int track = currentTrack();
    if (arpOn(track) && engineHooks.setArpLatch)
    {
        const bool on = !(engineHooks.arpLatch && engineHooks.arpLatch(track));
        engineHooks.setArpLatch(track, on);
        return;
    }

    if (holdActive)
    {
        holdActive = false;
        releaseHeldNotes();
    }
    else
    {
        // Keys held now latch when they are released; with none held, the next ones played do.
        holdActive = true;
        heldNoteCount = 0;
    }
}

void ScreenManager::handlePlay()
{
    if (patternKeyHeld)
    {
        reloadPattern();
        return;
    }
    if (funcHeld)
    {
        // Func + Play = clear; Func + Shift + Play = undo (Shift + Play is "back to the start").
        handleEdit(shiftHeld ? Edit::Undo : Edit::Clear, CC_PLAY);
        return;
    }

    lastEdit = Edit::None;

    // Record held + Play: live recording.
    if (recordHeld)
    {
        recordUsed = true;
        recordPlayPressed();
        return;
    }

    if (shiftHeld)
    {
        // Stop and return to the start; when already stopped, just reset the position.
        if (transportListener)  transportListener->stopPressed();
        else if (sequencer)     sequencer->stop();
        playLocal = false;
        if (sequencer)          sequencer->resetPosition();
        leaveRecording();
        return;
    }

    // Stopping also ends recording: the next Play plays back what was recorded.
    const bool wasRunning = isTransportRunning();
    toggleTransport();
    if (wasRunning)
        leaveRecording();
    else
        showCountIn();
}

void ScreenManager::toggleTransport()
{
    playLocal = !playLocal;
    if (transportListener)
        transportListener->playPressed();
    else if (sequencer)
    {
        // (requestStart: with recording armed and a count-in set, the bars are counted in first)
        if (sequencer->isRunning()) sequencer->stop();
        else                        sequencer->requestStart();
    }
}

void ScreenManager::setRecordArmed(bool armed)
{
    if (armed == isRecordArmed())
        return;
    recordLocal = armed;
    if (transportListener)
        transportListener->recordPressed();
    else if (sequencer)
        sequencer->setRecording(armed);
}

// A count-in has begun (Play with recording armed and CNT set): the name bar says so.
bool ScreenManager::showCountIn()
{
    if (sequencer == nullptr || !sequencer->isCountingIn())
        return false;
    const int bars = sequencer->getCountInBars();
    showMessage(bars == 1 ? juce::String("COUNT IN 1 BAR") : "COUNT IN " + juce::String(bars) + " BARS", LONG_MESSAGE_MS);
    return true;
}

juce::String ScreenManager::recordModeText() const
{
    const bool quant = sequencer != nullptr && sequencer->getRecordMode() == SequencerCore::RecordMode::Quant;
    return quant ? "REC QUANT" : "REC FREE";
}

// Record + Play. Not recording yet: recording goes on and the transport starts from the top (when
// it is already running it keeps running). While recording: free <-> quantized.
void ScreenManager::recordPlayPressed()
{
    const bool wasRecording = isRecordArmed() && isTransportRunning();
    if (wasRecording && sequencer != nullptr)
    {
        using Mode = SequencerCore::RecordMode;
        sequencer->setRecordMode(sequencer->getRecordMode() == Mode::Quant ? Mode::Free : Mode::Quant);
    }
    else
    {
        setRecordArmed(true);
        if (!isTransportRunning())
            toggleTransport();
    }
    if (!showCountIn())
        showMessage(recordModeText(), LONG_MESSAGE_MS);
}

// Recording goes off. If anything was recorded, the track recorded last is selected and laid out
// on the STEP screen, on the page that holds its first recorded step, ready for parameter locks.
void ScreenManager::leaveRecording()
{
    if (!isRecordArmed())
        return;
    setRecordArmed(false);
    showMessage("REC OFF");

    if (sequencer == nullptr)
        return;
    const int track = sequencer->getLastRecordedTrack();
    if (track < 0 || track >= currentTrackCount())
        return;
    const int stepScreen = roleScreen(stepScreenIndex, "STEP");
    if (stepScreen < 0)
        return;
    if (track != currentTrack())
        selectTrack(track);
    if (stepScreen != activeIndex)
        setActiveScreen(stepScreen);
    mods(screenAt(stepScreen))->showStep(juce::jmax(0, sequencer->getFirstRecordedStep(track)));
    needsRedraw = true;
}

int ScreenManager::getRecordLightPad() const
{
    if (sequencer == nullptr || !sequencer->isRecording() || !sequencer->isPlaying())
        return -1;
    return sequencer->getCurrentStep(currentTrack()) % 16;
}

void ScreenManager::scaleLockChanged()
{
    // Shown on every screen: the button has no light of its own.
    const auto name = scaleName ? scaleName().trim() : juce::String();
    juce::StringArray texts;
    if (scaleLocked)
    {
        if (name.isNotEmpty())
        {
            texts.add("SCALE LOCK ON " + name);
            texts.add("SCALE ON " + name);
            texts.add("LOCK " + name);
        }
        texts.add("SCALE LOCK ON");
        texts.add("SCALE ON");
    }
    else
    {
        texts.add("SCALE LOCK OFF");
        texts.add("SCALE OFF");
    }
    showMessage(texts, LONG_MESSAGE_MS);
}

void ScreenManager::handleRecord(bool pressed)
{
    if (!pressed)
    {
        // Fill ends with the button, whatever happened to Shift in the meantime.
        if (fillHeld)
        {
            fillHeld = false;
            if (sequencer && !sequencer->isFillLatched())
                sequencer->setFillActive(false);
        }
        // Plain Record acts when it is let go, so that Record + Play is not also a plain Record.
        if (recordHeld)
        {
            recordHeld = false;
            helpKeyChanged(HelpKey::Record, false);     // (its card never takes the release: see the table)
            if (!recordUsed)
            {
                if (isRecordArmed())
                    leaveRecording();
                else
                {
                    setRecordArmed(true);
                    showMessage(recordModeText(), LONG_MESSAGE_MS);
                }
            }
        }
        return;
    }

    if (patternKeyHeld)
        keepPatternCopy();
    else if (funcHeld)
        handleEdit(shiftHeld ? Edit::Redo : Edit::Copy, CC_RECORD);     // with Shift: redo
    else if (shiftHeld)
    {
        lastEdit = Edit::None;
        fillHeld = true;
        if (sequencer)
            sequencer->setFillActive(true);
    }
    else if (playHeld)
    {
        // Play went down a moment before Record: the same as Record + Play when that Play started
        // the transport. (If it stopped the transport, nothing more happens.)
        lastEdit = Edit::None;
        if (isTransportRunning() && !isRecordArmed())
        {
            setRecordArmed(true);
            showMessage(recordModeText(), LONG_MESSAGE_MS);
        }
    }
    else
    {
        lastEdit = Edit::None;
        recordHeld = true;
        recordUsed = false;
        helpKeyChanged(HelpKey::Record, true);
    }
}

void ScreenManager::tapTempo()
{
    // Average of the last taps; two are enough; a pause of two seconds starts over.
    const auto now = nowMs();
    if (tapCount > 0 && now - taps[(size_t) (tapCount - 1)] > TAP_RESET_MS)
        tapCount = 0;
    if (tapCount == MAX_TAPS)
    {
        for (int i = 1; i < MAX_TAPS; ++i)
            taps[(size_t) (i - 1)] = taps[(size_t) i];
        --tapCount;
    }
    taps[(size_t) tapCount++] = now;

    if (tapCount < 2)
    {
        showMessage("TAP");
        return;
    }

    const double interval = (double) (taps[(size_t) (tapCount - 1)] - taps[0]) / (double) (tapCount - 1);
    if (interval <= 0.0)
        return;
    const float bpm = juce::jlimit(20.0f, 300.0f, (float) (60000.0 / interval));
    if (sequencer)
        sequencer->setTempo(bpm);
    if (engineHooks.tempoChanged)
        engineHooks.tempoChanged(bpm);
    showMessage(juce::String((int) std::lround(bpm)) + " BPM");
}

// =================================================================================================
// > + Record keeps a temporary copy of the pattern; > + Play reloads it (or the pattern as it was
// when the project was loaded, if no copy was kept).
// =================================================================================================
void ScreenManager::keepPatternCopy()
{
    if (sequencer == nullptr)
        return;
    const int index = sequencer->getActivePatternIndex();
    keptPatterns[(size_t) index] = sequencer->getPattern().duplicate();
    keptValid[(size_t) index] = true;
    lastEdit = Edit::None;
    showMessage("KEEP P" + juce::String(index + 1));
}

void ScreenManager::reloadPattern()
{
    if (sequencer == nullptr)
        return;
    const int index = sequencer->getActivePatternIndex();
    lastEdit = Edit::None;
    const Pattern* from = keptValid[(size_t) index] ? &keptPatterns[(size_t) index]
                        : loadedValid[(size_t) index] ? &loadedPatterns[(size_t) index] : nullptr;
    if (from == nullptr)
    {
        showMessage("NO COPY");
        return;
    }
    // It replaces the whole pattern at once (it is played in time), so it is an undo step.
    Screen::UndoPoint point;
    point.scope = Screen::UndoPoint::Scope::Pattern;
    point.verb = "RELOAD";
    const juce::String p = juce::String(index + 1);
    point.names = { "PATTERN " + p, "PAT " + p, "P" + p };
    undoBegin(point);
    restorePattern(*from);
    undoEnd();
    showMessage("RELOAD P" + p);
}

// The data is replaced track by track in place (same as an edit from a screen); the pattern object
// the audio thread reads is never swapped out.
void ScreenManager::restorePattern(const Pattern& from)
{
    if (sequencer != nullptr)
        locked([&] { copyPatternData(from, sequencer->getPattern()); });
}

// =================================================================================================
// Func + Record / Play / > = copy / clear / paste; with Shift as well, Play = undo and Record =
// redo. getEditPlan() works out what one of them does at this moment - the newest undoable action
// (the dedicated chords, or the same edit chord again with nothing in between); else what the
// screen takes for itself (a held step); else the default for the screen's context - and
// performEdit() does exactly that. The help card and the name-bar confirmation are written from the
// same plan. A plan that replaces a whole pattern is carried out only once its button has been
// held for HOLD_MS (handleEdit starts the hold, tick() or the release ends it).
// =================================================================================================
const char* ScreenManager::editVerb(Edit edit)
{
    switch (edit)
    {
        case Edit::Copy:  return "COPY";
        case Edit::Clear: return "CLEAR";
        case Edit::Paste: return "PASTE";
        case Edit::Undo:  return "UNDO";
        case Edit::Redo:  return "REDO";
        case Edit::None:  break;
    }
    return "";
}

ScreenManager::EditPlan ScreenManager::getEditPlan(Edit edit) const
{
    EditPlan plan;
    if (activeScreen == nullptr || edit == Edit::None)
        return plan;
    plan.verb = editVerb(edit);
    const bool takeRunning = passOpen && !passChanges().empty();

    // ---- Undo / redo: the newest action that can still be applied --------------------------------
    // A recording pass that is running is the newest action as far as it has got; while it has
    // recorded anything there is nothing to redo (a new action ends what could be redone).
    if (edit == Edit::Undo || edit == Edit::Redo)
    {
        if (takeRunning)
        {
            if (edit == Edit::Undo)
            {
                plan.kind = EditPlan::Kind::Undo;
                plan.names = passHeader().labels();
            }
            return plan;
        }
        if (const auto* entry = nextValid(edit == Edit::Redo))
        {
            plan.kind = edit == Edit::Undo ? EditPlan::Kind::Undo : EditPlan::Kind::Redo;
            plan.names = entry->labels();
        }
        return plan;
    }

    // ---- The same combination again, at once: it takes that edit back -----------------------------
    if (lastEdit == edit)
    {
        if (edit == Edit::Copy)
        {
            plan.kind = EditPlan::Kind::Undo;
            plan.verb = "UNDO";
            plan.names.add("COPY");
            plan.undoesCopy = true;
            return plan;
        }
        if (!takeRunning && !history.undos().empty() && history.undos().back().serial == armedSerial
            && entryValid(history.undos().back(), false))
        {
            plan.kind = EditPlan::Kind::Undo;
            plan.verb = "UNDO";
            plan.names = history.undos().back().labels();
            return plan;
        }
    }

    const int track = currentTrack();
    plan.track = track;

    Screen::EditScope own;
    if (mods(activeScreen)->getEditScope(own))
    {
        plan.kind = EditPlan::Kind::ScreenOwn;
        plan.names = own.names;
        plan.step = own.step;
        plan.empty = edit == Edit::Paste && !own.canPaste;
        return plan;
    }

    if (sequencer == nullptr || track < 0 || track >= sequencer->getPattern().getTrackCount())
        return plan;
    const auto context = (activeIndex >= 0 && activeIndex < (int) traits.size())
                             ? traits[(size_t) activeIndex].context : CopyContext::None;
    const juce::String t = "T" + juce::String(track + 1);
    const juce::String p = juce::String(sequencer->getActivePatternIndex() + 1);

    switch (context)
    {
        case CopyContext::None:
            break;                  // this screen has nothing to copy: the combination does nothing here
        case CopyContext::Steps:
            plan.kind = EditPlan::Kind::Steps;
            plan.names = { t + " STEPS", "STEPS" };
            plan.empty = edit == Edit::Paste && !clips.hasSteps;
            break;
        case CopyContext::Pattern:
            plan.kind = EditPlan::Kind::Pattern;
            plan.names = { "PATTERN " + p, "PAT " + p, "P" + p };
            plan.empty = edit == Edit::Paste && !clips.hasPattern;
            // Every track's steps at one press is too much for a slip of the finger beside Play.
            plan.hold = edit != Edit::Copy && !plan.empty;
            break;
        case CopyContext::Sound:
            plan.kind = EditPlan::Kind::Sound;
            if (edit == Edit::Clear)
            {
                // The engine's own defaults; mixer and effects stay.
                plan.verb = "RESET";
                plan.names = { t + " ENGINE", t + " ENG" };
            }
            else
                plan.names = { t + " SOUND", t + " SND" };
            plan.empty = edit == Edit::Paste && !clips.hasSound;
            break;
        case CopyContext::Fx:
            // The track's effects: the DIRT / TALK / MODFX blocks, the insert slots, the arp locks.
            // The engine and the mixer (VOL, PAN, sends, LEN, SPD on the TRACK page) are not touched.
            plan.kind = EditPlan::Kind::Fx;
            plan.names = { t + " FX", "FX" };
            plan.empty = edit == Edit::Paste && !clips.hasFx;
            break;
    }
    return plan;
}

juce::StringArray ScreenManager::EditPlan::wordings() const
{
    if (kind == Kind::Nothing)
    {
        if (verb == "UNDO" || verb == "REDO")
            return { "NOTHING TO " + verb, "NO " + verb };
        return { "NO " + verb + " HERE", "NO " + verb };
    }
    if (empty)
        return { "NOTHING COPIED", "NONE COPIED", "EMPTY" };
    juce::StringArray out;
    for (auto& name : names)
        out.add(verb + " " + name);
    out.add(verb);
    return out;
}

juce::String ScreenManager::EditPlan::nameWithin(int maxChars) const
{
    for (auto& name : names)
        if (name.length() <= maxChars)
            return name;
    return names.isEmpty() ? juce::String() : names[names.size() - 1].substring(0, maxChars);
}

juce::StringArray ScreenManager::EditPlan::holdWordings(const juce::String& progress) const
{
    juce::StringArray out;
    for (auto& name : names)
        out.add(verb + " " + name + " " + progress);
    out.add(verb + " " + progress);
    out.add(progress);
    return out;
}

juce::StringArray ScreenManager::EditPlan::holdRefusal() const
{
    juce::StringArray out;
    if (kind == Kind::Pattern)
        out.add("HOLD TO " + verb + " PATTERN");
    out.add("HOLD TO " + verb);
    out.add("HOLD");
    return out;
}

juce::String ScreenManager::holdProgress() const
{
    if (holdEdit == Edit::None)
        return {};
    const int marks = juce::jlimit(1, HOLD_MARKS, 1 + (int) ((nowMs() - holdStartMs) * HOLD_MARKS / HOLD_MS));
    return juce::String::repeatedString(">", marks) + juce::String::repeatedString(".", HOLD_MARKS - marks);
}

// The chord was pressed. A plan that needs the hold starts it; every other plan is carried out now.
void ScreenManager::handleEdit(Edit edit, int cc)
{
    if (activeScreen == nullptr)
        return;
    const auto plan = getEditPlan(edit);
    if (plan.hold && cc >= 0)
    {
        holdEdit = edit;
        holdCc = cc;
        holdStartMs = nowMs();
        holdNames = plan.names;
        needsRedraw = true;
        return;
    }
    performEdit(edit, plan);
}

// The held button (or Func) was let go, or its time is up. Long enough: the edit, if it is still
// the one the hold began for. Too short: nothing changes and the name bar says why.
void ScreenManager::releaseHeldEdit(bool timeIsUp)
{
    const Edit edit = holdEdit;
    if (edit == Edit::None)
        return;
    const bool longEnough = timeIsUp || nowMs() - holdStartMs >= HOLD_MS;
    holdEdit = Edit::None;
    holdCc = -1;
    needsRedraw = true;

    const auto plan = getEditPlan(edit);
    if (!longEnough)
    {
        showMessage(plan.holdRefusal(), LONG_MESSAGE_MS);
        return;
    }
    if (plan.hold && plan.names == holdNames)
        performEdit(edit, plan);
}

void ScreenManager::tick()
{
    if (holdEdit != Edit::None)
    {
        if (nowMs() - holdStartMs >= HOLD_MS)
            releaseHeldEdit(true);
        needsRedraw = true;         // the progress marks move
    }
    afterControl();
}

ScreenManager::SoundClip ScreenManager::captureSoundOf(int t) const
{
    SoundClip clip;
    if (sequencer == nullptr || t < 0 || t >= sequencer->getPattern().getTrackCount())
        return clip;
    const auto& tr = sequencer->getPattern().getTrack(t);
    clip.type = engineTypeOf(t);
    clip.params = engineHooks.captureSound ? engineHooks.captureSound(t) : tr.soundParams;
    return clip;
}

void ScreenManager::applySoundTo(int t, const SoundClip& clip)
{
    if (sequencer == nullptr || t < 0 || t >= sequencer->getPattern().getTrackCount())
        return;
    if (engineHooks.applySound)
    {
        engineHooks.applySound(t, clip.type, clip.params);
        return;
    }
    // No engine attached: the stored data is all there is. One sound per track, in every pattern.
    for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
    {
        if (t >= sequencer->getPattern(p).getTrackCount()) continue;
        auto& tr = sequencer->getPattern(p).getTrack(t);
        tr.engineType = clip.type;
        tr.soundParams = clip.params;
    }
}

EngineType ScreenManager::engineTypeOf(int t) const
{
    if (engineHooks.engineType)
        return engineHooks.engineType(t);
    if (sequencer != nullptr && t >= 0 && t < sequencer->getPattern().getTrackCount())
        return sequencer->getPattern().getTrack(t).engineType;
    return EngineType::UnifiedVoice;
}

bool ScreenManager::isFxParam(int id) const
{
    if (engineHooks.isFxParam)
        return engineHooks.isFxParam(id);
    // No engine attached: the id table's effect ranges (insert slots 200-239, the effect blocks
    // 240-263, arp locks 270-272, insert switches 280-311) and pressure ranges on them.
    const int target = RangeParam::isRangeParam(id) ? RangeParam::targetOf(id) : id;
    return target >= 200 && target < 312;
}

void ScreenManager::locked(const std::function<void()>& work)
{
    if (engineHooks.structureLocked) engineHooks.structureLocked(work);
    else                             work();
}

void ScreenManager::performEdit(Edit edit, const EditPlan& plan)
{
    if (activeScreen == nullptr)
        return;
    using Kind = EditPlan::Kind;

    // ---- Nothing to do here: say so (nothing changes, an armed undo stays armed) ---------------------
    if (plan.kind == Kind::Nothing)
    {
        showMessage(plan.wordings(), LONG_MESSAGE_MS, edit == Edit::Undo || edit == Edit::Redo);
        return;
    }

    // ---- Undo / redo (the dedicated chords, or the same combination again) ---------------------------
    if (plan.kind == Kind::Undo || plan.kind == Kind::Redo)
    {
        lastEdit = Edit::None;
        if (plan.undoesCopy) clips = clipsBeforeCopy;
        else                 performUndo(plan.kind == Kind::Redo);
        showMessage(plan.wordings(), LONG_MESSAGE_MS, true);
        return;
    }

    // ---- A paste with nothing copied: say so and change nothing --------------------------------------
    if (plan.empty)
    {
        if (plan.kind == Kind::ScreenOwn)
            mods(activeScreen)->onPaste();      // (the screen notes that the held step was used)
        showMessage(plan.wordings(), LONG_MESSAGE_MS);
        return;
    }

    const int track = plan.track;
    auto fxOf = [this](const ParamMap& sound)
    {
        ParamMap fx;
        for (auto& [id, value] : sound)
            if (isFxParam(id))
                fx[id] = value;
        return fx;
    };
    auto withoutFx = [this](SoundClip clip)
    {
        for (auto it = clip.params.begin(); it != clip.params.end();)
            it = isFxParam(it->first) ? clip.params.erase(it) : std::next(it);
        return clip;
    };

    // ---- Copy: only the clipboard changes (the same again puts it back) ------------------------------
    if (edit == Edit::Copy)
    {
        clipsBeforeCopy = clips;
        switch (plan.kind)
        {
            case Kind::ScreenOwn:   mods(activeScreen)->onCopy(); break;
            case Kind::Pattern:     clips.pattern = sequencer->getPattern().duplicate(); clips.hasPattern = true; break;
            case Kind::Sound:       clips.sound = captureSoundOf(track); clips.hasSound = true; break;
            case Kind::Fx:          clips.fx = fxOf(captureSoundOf(track).params); clips.hasFx = true; break;
            case Kind::Steps:
            {
                const auto& tr = sequencer->getPattern().getTrack(track);
                clips.steps = { tr.steps, tr.length, tr.speedMultiplier };
                clips.hasSteps = true;
                break;
            }
            case Kind::Nothing: case Kind::Undo: case Kind::Redo: break;
        }
        lastEdit = edit;
        showMessage(plan.wordings(), LONG_MESSAGE_MS);
        return;
    }

    // ---- Clear / paste: one undo step, holding exactly what the plan says it changes -----------------
    Screen::UndoPoint point;
    point.track = track;
    point.step = plan.step;
    point.verb = plan.verb;
    point.names = plan.names;
    switch (plan.kind)
    {
        case Kind::ScreenOwn:   point.scope = plan.step >= 0 ? Screen::UndoPoint::Scope::Step : Screen::UndoPoint::Scope::TrackSteps; break;
        case Kind::Steps:       point.scope = Screen::UndoPoint::Scope::TrackSteps; break;
        case Kind::Pattern:     point.scope = Screen::UndoPoint::Scope::Pattern; break;
        default:                point.scope = Screen::UndoPoint::Scope::Sound; break;
    }
    undoBegin(point);

    switch (plan.kind)
    {
        case Kind::ScreenOwn:
            // A held step, a list row, ...: the screen said it takes these (getEditScope).
            mods(activeScreen);
            if (edit == Edit::Clear) activeScreen->onClear();
            else                     activeScreen->onPaste();
            break;

        case Kind::Pattern:
            locked([&]
            {
                auto& pattern = sequencer->getPattern();
                if (edit == Edit::Clear) pattern.clear();
                else                     copyPatternData(clips.pattern, pattern);
            });
            break;

        case Kind::Sound:
            if (edit == Edit::Clear)
            {
                // The engine's own defaults; mixer and effects stay.
                SoundClip cleared = captureSoundOf(track);
                for (auto it = cleared.params.begin(); it != cleared.params.end();)
                {
                    const bool engineParam = EngineParamRange::contains(it->first);
                    const bool engineRange = RangeParam::isRangeParam(it->first)
                                             && EngineParamRange::contains(RangeParam::targetOf(it->first));
                    it = (engineParam || engineRange) ? cleared.params.erase(it) : std::next(it);
                }
                applySoundTo(track, cleared);
            }
            else
                applySoundTo(track, clips.sound);
            break;

        case Kind::Fx:
        {
            // The sound as it is, with its effects taken out and the cleared ones (the blocks at
            // their defaults = off, no insert slots, arp locks off) or the copied ones put in.
            SoundClip sound = withoutFx(captureSoundOf(track));
            if (edit == Edit::Clear)
            {
                if (engineHooks.fxDefaults)
                    engineHooks.fxDefaults(sound.params);
            }
            else
                for (auto& [id, value] : clips.fx)
                    sound.params[id] = value;
            applySoundTo(track, sound);
            break;
        }

        case Kind::Steps:
            locked([&]
            {
                auto& tr = sequencer->getPattern().getTrack(track);
                if (edit == Edit::Clear) tr.clearAllSteps();
                else
                {
                    tr.steps = clips.steps.steps;
                    tr.length = clips.steps.length;
                    tr.speedMultiplier = clips.steps.speed;
                }
            });
            break;

        case Kind::Nothing: case Kind::Undo: case Kind::Redo:
            break;
    }

    undoEnd();
    lastEdit = edit;
    armedSerial = lastPushedSerial;     // -1 when nothing changed: then "again" is simply the edit again
    showMessage(plan.wordings(), LONG_MESSAGE_MS);
}

// =================================================================================================
// The undo history. An action is recorded between undoBegin (the state it is about to change is
// kept) and undoEnd (if it did change, the entry is pushed). Undo and redo swap an entry's kept
// state with the live one, so a round trip is exact; a Sound entry puts back only the parameters
// its action changed. An entry whose track or engine is no longer what it was is passed over and
// dropped. A recording pass is one entry: the pattern is kept when recording starts running and
// the tracks that differ when it stops are the take.
// =================================================================================================
void ScreenManager::undoBegin(const Screen::UndoPoint& point)
{
    if (pendingDepth++ > 0)
        return;                         // an action inside an action is part of it
    if (passOpen)
    {
        // What was recorded up to here is its own step, before this one.
        closeRecordingPass();
        passSuspended = true;
    }
    pendingEntry = UndoEntry();
    pendingOpen = captureBefore(point, pendingEntry);
}

void ScreenManager::undoEnd()
{
    if (pendingDepth <= 0 || --pendingDepth > 0)
        return;
    lastPushedSerial = -1;
    if (pendingOpen && entryChanged(pendingEntry))
    {
        const auto now = nowMs();
        auto* target = history.coalesceTarget(pendingEntry.coalesceKey, now);
        if (target != nullptr && target->scope == UndoEntry::Scope::Sound && pendingEntry.scope == UndoEntry::Scope::Sound
            && target->track == pendingEntry.track && target->typeAfter == pendingEntry.typeBefore)
        {
            // The same control again within a moment: one step, from where it began to where it is now.
            target->after = std::move(pendingEntry.after);
            target->typeAfter = pendingEntry.typeAfter;
            target->touchedMs = now;
            history.redos().clear();
            lastPushedSerial = target->serial;
        }
        else
        {
            pendingEntry.touchedMs = now;
            lastPushedSerial = history.push(std::move(pendingEntry));
        }
    }
    pendingOpen = false;
    pendingEntry = UndoEntry();
    if (passSuspended)
    {
        passSuspended = false;
        afterControl();                 // still recording: the take goes on from here
    }
}

bool ScreenManager::captureBefore(const Screen::UndoPoint& point, UndoEntry& entry) const
{
    using Scope = Screen::UndoPoint::Scope;
    entry.verb = point.verb;
    entry.names = point.names;
    entry.coalesceKey = point.coalesceKey;

    if (point.scope == Scope::Custom)
    {
        if (!point.capture || !point.apply)
            return false;
        entry.scope = UndoEntry::Scope::Custom;
        entry.capture = point.capture;
        entry.apply = point.apply;
        entry.blob = point.capture();
        return true;
    }

    if (sequencer == nullptr)
        return false;
    const auto& pattern = sequencer->getPattern();
    const int track = point.track >= 0 ? point.track : currentTrack();
    if (track < 0 || track >= pattern.getTrackCount())
        return false;

    if (point.scope == Scope::Sound)
    {
        entry.scope = UndoEntry::Scope::Sound;
        entry.track = track;
        auto clip = captureSoundOf(track);
        entry.typeBefore = clip.type;
        entry.before = std::move(clip.params);
        return true;
    }

    entry.scope = UndoEntry::Scope::Steps;
    entry.pattern = sequencer->getActivePatternIndex();
    auto part = [&](int t, int step)
    {
        UndoEntry::Part p;
        p.track = t;
        p.step = step;
        p.engine = engineTypeOf(t);
        const auto& tr = pattern.getTrack(t);
        if (step >= 0)
            p.steps = { tr.steps[(size_t) step] };
        else
        {
            p.steps = tr.steps;
            p.length = tr.length;
            p.speed = tr.speedMultiplier;
        }
        return p;
    };

    if (point.scope == Scope::Step)
    {
        if (point.step < 0 || point.step >= (int) pattern.getTrack(track).steps.size())
            return false;
        entry.parts.push_back(part(track, point.step));
    }
    else if (point.scope == Scope::TrackSteps)
        entry.parts.push_back(part(track, -1));
    else
    {
        for (int t = 0; t < pattern.getTrackCount(); ++t)
            entry.parts.push_back(part(t, -1));
        entry.hasSwing = true;
        entry.swing = pattern.swing;
    }
    return true;
}

// Did the action change what was kept? (For a Sound entry this also notes the state it left.)
bool ScreenManager::entryChanged(UndoEntry& entry) const
{
    switch (entry.scope)
    {
        case UndoEntry::Scope::Custom:
            // The screen marks only a real change, and its state may change a moment later (the
            // arpeggiator's mutation is carried out by the audio side): always a step.
            return true;

        case UndoEntry::Scope::Sound:
        {
            auto clip = captureSoundOf(entry.track);
            entry.typeAfter = clip.type;
            entry.after = std::move(clip.params);
            return entry.typeAfter != entry.typeBefore || entry.after != entry.before;
        }

        case UndoEntry::Scope::Steps:
        {
            if (sequencer == nullptr)
                return false;
            const auto& pattern = sequencer->getPattern(entry.pattern);
            if (entry.hasSwing && entry.swing != pattern.swing)
                return true;
            for (auto& part : entry.parts)
            {
                if (part.track >= pattern.getTrackCount())
                    continue;
                const auto& tr = pattern.getTrack(part.track);
                if (part.step >= 0)
                {
                    if (part.step >= (int) tr.steps.size() || !UndoData::sameStep(tr.steps[(size_t) part.step], part.steps[0]))
                        return true;
                }
                else if (tr.length != part.length || tr.speedMultiplier != part.speed || !UndoData::sameSteps(tr.steps, part.steps))
                    return true;
            }
            return false;
        }
    }
    return false;
}

// Can the entry still be applied? Its tracks must exist with the engines they had (a step's locks
// and a sound's parameters are that engine's ids).
bool ScreenManager::entryValid(const UndoEntry& entry, bool redo) const
{
    if (entry.scope == UndoEntry::Scope::Custom)
        return entry.capture != nullptr && entry.apply != nullptr;
    if (sequencer == nullptr)
        return false;

    if (entry.scope == UndoEntry::Scope::Sound)
        return entry.track >= 0 && entry.track < sequencer->getPattern().getTrackCount()
            && engineTypeOf(entry.track) == (redo ? entry.typeBefore : entry.typeAfter);

    if (entry.pattern < 0 || entry.pattern >= SequencerCore::MAX_PATTERNS)
        return false;
    const auto& pattern = sequencer->getPattern(entry.pattern);
    for (auto& part : entry.parts)
    {
        if (part.track < 0 || part.track >= pattern.getTrackCount() || engineTypeOf(part.track) != part.engine)
            return false;
        if (part.step >= 0 && (part.step >= (int) pattern.getTrack(part.track).steps.size() || part.steps.empty()))
            return false;
    }
    return true;
}

void ScreenManager::swapEntry(UndoEntry& entry, bool redo)
{
    switch (entry.scope)
    {
        case UndoEntry::Scope::Custom:
        {
            auto now = entry.capture();
            entry.apply(entry.blob);
            entry.blob = std::move(now);
            break;
        }

        case UndoEntry::Scope::Sound:
        {
            const auto current = captureSoundOf(entry.track);
            const ParamMap& from = redo ? entry.before : entry.after;      // what the track has of the action now
            const ParamMap& to = redo ? entry.after : entry.before;
            SoundClip target;
            target.type = redo ? entry.typeAfter : entry.typeBefore;
            if (entry.typeBefore != entry.typeAfter)
            {
                // Another engine: the whole sound goes back, and what is there now is what the
                // opposite step returns to.
                target.params = to;
                (redo ? entry.before : entry.after) = current.params;
            }
            else
            {
                // The same engine: exactly the parameters the action changed. Whatever else was
                // set since stays as it is.
                target.params = current.params;
                auto restore = [&](int id)
                {
                    const auto f = from.find(id), t = to.find(id);
                    if (f != from.end() && t != to.end() && f->second == t->second)
                        return;
                    if (t != to.end()) target.params[id] = t->second;
                    else               target.params.erase(id);
                };
                for (auto& [id, value] : from) { juce::ignoreUnused(value); restore(id); }
                for (auto& [id, value] : to)   { juce::ignoreUnused(value); restore(id); }
            }
            applySoundTo(entry.track, target);
            break;
        }

        case UndoEntry::Scope::Steps:
            locked([&]
            {
                auto& pattern = sequencer->getPattern(entry.pattern);
                if (entry.hasSwing)
                    std::swap(pattern.swing, entry.swing);
                for (auto& part : entry.parts)
                {
                    auto& tr = pattern.getTrack(part.track);
                    if (part.step >= 0)
                        std::swap(tr.steps[(size_t) part.step], part.steps[0]);
                    else
                    {
                        std::swap(tr.steps, part.steps);
                        std::swap(tr.length, part.length);
                        std::swap(tr.speedMultiplier, part.speed);
                    }
                }
            });
            break;
    }
}

const UndoEntry* ScreenManager::nextValid(bool redo) const
{
    const auto& stack = redo ? history.redos() : history.undos();
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        if (entryValid(*it, redo))
            return &*it;
    return nullptr;
}

void ScreenManager::performUndo(bool redo)
{
    // A take that is running ends here as a step of its own (and so is the one an undo takes back).
    closeRecordingPass();

    auto& stack = redo ? history.redos() : history.undos();
    while (!stack.empty() && !entryValid(stack.back(), redo))
        stack.pop_back();               // its track or engine is gone: dropped
    if (!stack.empty())
    {
        swapEntry(stack.back(), redo);
        if (redo) history.redone();
        else      history.undone();
    }
    armedSerial = -1;
    afterControl();
    needsRedraw = true;
}

// After every controller message and on every refresh: has a recording pass begun or ended?
void ScreenManager::afterControl()
{
    if (sequencer == nullptr || pendingDepth > 0 || passSuspended)
        return;
    const bool recording = sequencer->isRecording() && sequencer->isPlaying();
    if (recording && !passOpen)      openRecordingPass();
    else if (!recording && passOpen) closeRecordingPass();
}

void ScreenManager::openRecordingPass()
{
    if (sequencer == nullptr)
        return;
    passPattern = sequencer->getPattern().duplicate();
    passPatternIndex = sequencer->getActivePatternIndex();
    passOpen = true;
}

// The tracks of the pattern the pass began on that are not as they were then.
std::vector<int> ScreenManager::passChanges() const
{
    std::vector<int> tracks;
    if (!passOpen || sequencer == nullptr)
        return tracks;
    const auto& now = sequencer->getPattern(passPatternIndex);
    const int count = juce::jmin(now.getTrackCount(), passPattern.getTrackCount());
    for (int t = 0; t < count; ++t)
        if (!UndoData::sameTrackSteps(now.getTrack(t), passPattern.getTrack(t)))
            tracks.push_back(t);
    return tracks;
}

UndoEntry ScreenManager::passHeader() const
{
    UndoEntry entry;
    const juce::String p = juce::String(passPatternIndex + 1);
    entry.verb = "REC";
    entry.names = { "TAKE PATTERN " + p, "TAKE PAT " + p, "TAKE P" + p, "TAKE" };
    return entry;
}

void ScreenManager::closeRecordingPass()
{
    if (!passOpen)
        return;
    const auto tracks = passChanges();
    passOpen = false;
    if (!tracks.empty())
    {
        UndoEntry entry = passHeader();
        entry.scope = UndoEntry::Scope::Steps;
        entry.pattern = passPatternIndex;
        for (int t : tracks)
        {
            UndoEntry::Part part;
            auto& was = passPattern.getTrack(t);
            part.track = t;
            part.engine = engineTypeOf(t);
            part.steps = std::move(was.steps);
            part.length = was.length;
            part.speed = was.speedMultiplier;
            entry.parts.push_back(std::move(part));
        }
        entry.touchedMs = nowMs();
        history.push(std::move(entry));
        lastEdit = Edit::None;
    }
    passPattern = Pattern(0);
}

// =================================================================================================
// Pads
// =================================================================================================
void ScreenManager::handlePadPress(int padIndex, int velocity)
{
    padDown[(size_t) padIndex] = true;
    padTarget[(size_t) padIndex] = nullptr;
    lastEdit = Edit::None;
    dismissHelp();
    const bool topRow = padIndex < LaunchkeyController::PADS_PER_ROW;
    const int column = padIndex % LaunchkeyController::PADS_PER_ROW;

    // Shift + pad: go to a screen (or pin / unpin its pads). A pad without a screen does nothing.
    if (shiftHeld)
    {
        const int target = shiftPadScreenIndex[padIndex];
        if (target < 0 || target >= (int) screens.size())
            return;
        if (target == activeIndex && !topRow && traits[(size_t) target].pinnable)
        {
            // Pin / unpin needs a deliberate hold of this pad (see handlePadRelease): pressing the
            // pad of the screen you are already on is too natural to act on its own.
            pinPad = padIndex;
            pinPressMs = nowMs();
        }
        else
            setActiveScreen(target);
        needsRedraw = true;
        return;
    }

    // Hold > + pad: queue pattern 1-16 (the next loop while playing, at once when stopped).
    if (patternKeyHeld)
    {
        if (sequencer != nullptr)
            sequencer->selectPattern(padIndex, sequencer->isPlaying() ? SequencerCore::ChangeMode::NextLoop
                                                                      : SequencerCore::ChangeMode::Immediate);
        needsRedraw = true;
        return;
    }

    // Hold Up or Down ("Track") + pad: top row selects a track, bottom row mutes it.
    if (trackKeysHeld != 0)
    {
        trackLayerUsed = true;
        if (column < currentTrackCount())
        {
            if (topRow)
                selectTrack(column);
            else if (sequencer != nullptr)
            {
                auto& tr = sequencer->getPattern().getTrack(column);
                tr.muted = !tr.muted;
            }
        }
        needsRedraw = true;
        return;
    }

    // Func + top-row pad: page keys, on screens that have them.
    if (funcHeld && topRow && activeScreen != nullptr && activeScreen->getPageKeyCount() > 0)
    {
        if (column < activeScreen->getPageKeyCount())
            mods(activeScreen)->onPageKey(column);
        needsRedraw = true;
        return;
    }

    // The pinned screen's pads keep playing on every screen (not under Func: that is the screen's).
    Screen* target = (pinnedIndex >= 0 && !funcHeld) ? screenAt(pinnedIndex) : activeScreen;
    padTarget[(size_t) padIndex] = target;
    if (target != nullptr)
    {
        mods(target)->onPadPress(padIndex, velocity);
        needsRedraw = true;
    }
}

void ScreenManager::noteOnReceived(int channel, int note, int velocity)
{
    const juce::ScopedLock ui(uiLock);
    // Notes tagged as coming from the keybed port are never pads, whatever their number.
    const bool fromKeysPort = (channel == LaunchkeyController::KEYS_PORT_CHANNEL);
    if (fromKeysPort)
        channel = 1;

    const int padIndex = fromKeysPort ? -1 : padIndexOfNote(note);
    if (padIndex >= 0)
    {
        handlePadPress(padIndex, velocity);
        redrawIfNeeded();
        return;
    }

    // Keyboard notes — channel 1
    if (channel == 1)
    {
        // Latch: the first key after every key was released starts a new chord.
        if (holdActive && physicallyHeldCount == 0 && heldNoteCount > 0)
            releaseHeldNotes();

        addPhysicalNote(note);

        if (activeScreen && activeScreen->wantsKeyboardEvents())
        {
            mods(activeScreen)->onKeyboardNoteOn(note, velocity);
            needsRedraw = true;
            redrawIfNeeded();
        }
        else if (keyboardListener)
        {
            // Scale lock: the key plays the nearest note of the scale; its release follows it.
            int sounding = note;
            if (scaleLocked && keySnap && note >= 0 && note < 128)
            {
                sounding = juce::jlimit(0, 127, keySnap(note));
                keySounding[(size_t) note] = (juce::int8) sounding;
            }
            keyboardListener->keyboardNoteOn(sounding, velocity);
        }
    }
}

void ScreenManager::aftertouchReceived(int channel, int note, int pressure)
{
    const juce::ScopedLock ui(uiLock);
    // Pad pressure (polyphonic aftertouch on the DAW port) follows the pad's press. Keybed-port
    // messages are never pads.
    if (channel == LaunchkeyController::KEYS_PORT_CHANNEL)
        return;
    const int padIndex = padIndexOfNote(note);
    if (padIndex < 0)
        return;

    Screen* target = padDown[(size_t) padIndex] ? padTarget[(size_t) padIndex]
                                                : (pinnedIndex >= 0 ? screenAt(pinnedIndex) : activeScreen);
    if (target != nullptr)
        mods(target)->onPadAftertouch(padIndex, pressure);
}

void ScreenManager::noteOffReceived(int channel, int note)
{
    const juce::ScopedLock ui(uiLock);
    const bool fromKeysPort = (channel == LaunchkeyController::KEYS_PORT_CHANNEL);
    if (fromKeysPort)
        channel = 1;

    const int padIndex = fromKeysPort ? -1 : padIndexOfNote(note);
    if (padIndex >= 0)
    {
        // The release goes where the press went (a press taken by a layer has no release).
        Screen* target = padDown[(size_t) padIndex] ? padTarget[(size_t) padIndex]
                                                    : (pinnedIndex >= 0 ? screenAt(pinnedIndex) : activeScreen);
        padDown[(size_t) padIndex] = false;
        padTarget[(size_t) padIndex] = nullptr;
        if (padIndex == pinPad)
        {
            // Shift + this screen's own pad, held for half a second: pin or unpin its pads.
            const int screen = shiftPadScreenIndex[padIndex];
            if (shiftHeld && screen == activeIndex && nowMs() - pinPressMs >= PIN_HOLD_MS)
                pinnedIndex = (pinnedIndex == screen) ? -1 : screen;
            pinPad = -1;
        }
        if (target != nullptr)
            mods(target)->onPadRelease(padIndex);

        // The screen may draw held pads differently (step view, drum pads): show the release.
        needsRedraw = true;
        redrawIfNeeded();
        return;
    }

    if (channel == 1)
    {
        removePhysicalNote(note);

        // Latch: the key stays down.
        if (holdActive)
        {
            if (isLatchedNote(note))
                return;
            if (heldNoteCount < MAX_HELD_NOTES)
            {
                heldNotes[heldNoteCount++] = note;
                return;
            }
        }

        sendKeyOff(note);
    }
}

void ScreenManager::sendKeyOff(int note)
{
    if (activeScreen && activeScreen->wantsKeyboardEvents())
    {
        mods(activeScreen)->onKeyboardNoteOff(note);
        needsRedraw = true;
        redrawIfNeeded();
    }
    else if (keyboardListener)
    {
        int sounding = note;
        if (note >= 0 && note < 128 && keySounding[(size_t) note] >= 0)
        {
            sounding = keySounding[(size_t) note];
            keySounding[(size_t) note] = -1;
        }
        keyboardListener->keyboardNoteOff(sounding);
    }
}

void ScreenManager::addPhysicalNote(int note)
{
    for (int i = 0; i < physicallyHeldCount; ++i)
        if (physicallyHeldNotes[i] == note) return;
    if (physicallyHeldCount < MAX_HELD_NOTES)
        physicallyHeldNotes[physicallyHeldCount++] = note;
}

void ScreenManager::removePhysicalNote(int note)
{
    for (int i = 0; i < physicallyHeldCount; ++i)
    {
        if (physicallyHeldNotes[i] == note)
        {
            physicallyHeldNotes[i] = physicallyHeldNotes[--physicallyHeldCount];
            return;
        }
    }
}

bool ScreenManager::isLatchedNote(int note) const
{
    for (int i = 0; i < heldNoteCount; ++i)
        if (heldNotes[i] == note)
            return true;
    return false;
}

// Lets go of every latched note whose key is not down right now.
void ScreenManager::releaseHeldNotes()
{
    const int count = heldNoteCount;
    heldNoteCount = 0;
    for (int i = 0; i < count; ++i)
    {
        const int note = heldNotes[i];
        bool physicallyHeld = false;
        for (int j = 0; j < physicallyHeldCount; ++j)
            if (physicallyHeldNotes[j] == note)
                physicallyHeld = true;
        if (!physicallyHeld)
            sendKeyOff(note);
    }
}

void ScreenManager::padModeChanged(int mode)
{
    if (controller && mode != LaunchkeyController::PAD_MODE_DAW)
        controller->setPadMode(LaunchkeyController::PAD_MODE_DAW);
}

void ScreenManager::configureEncoderDisplays()
{
    if (!controller)
        return;

    for (int i = 0; i < 8; ++i)
        controller->configureDisplay(0x15 + i, 0x00);
}

void ScreenManager::updateEncoderDisplayNames()
{
    if (!controller || !activeScreen)
        return;

    for (int i = 0; i < 8; ++i)
        controller->configureDisplay(0x15 + i, 0x00);
}

// =================================================================================================
// Pad colours
// =================================================================================================
ScreenManager::PadSource ScreenManager::getPadFrame(std::array<Screen::PadColour, 16>& out) const
{
    out.fill(OFF);
    const bool blinkOn = ((nowMs() / 150) % 2) == 0;

    // Shift: the screen map. Current screen bright, other assigned pads dim, unassigned off.
    if (shiftHeld)
    {
        for (int pad = 0; pad < 16; ++pad)
        {
            const int target = shiftPadScreenIndex[pad];
            if (target < 0 || target >= (int) screens.size())
                continue;
            out[(size_t) pad] = (target == pinnedIndex && pad >= 8) ? PIN
                              : (target == activeIndex ? BRIGHT : DIM);
        }
        return PadSource::Layer;
    }

    // >: patterns. Current bright, queued blinking, with trigs dim, empty off.
    if (patternKeyHeld && sequencer != nullptr)
    {
        for (int pad = 0; pad < 16 && pad < sequencer->getPatternCount(); ++pad)
        {
            if (pad == sequencer->getActivePatternIndex())   out[(size_t) pad] = PAT_NOW;
            else if (pad == sequencer->getPendingPattern())  out[(size_t) pad] = blinkOn ? PAT_QUEUED : OFF;
            else if (patternHasTrigs(sequencer->getPattern(pad))) out[(size_t) pad] = PAT_USED;
        }
        return PadSource::Layer;
    }

    // Up / Down: tracks. Top row: selected bright, others dim. Bottom row: muted red.
    if (trackKeysHeld != 0)
    {
        const int count = juce::jmin(8, currentTrackCount());
        for (int t = 0; t < count; ++t)
        {
            out[(size_t) t] = (t == currentTrack()) ? BRIGHT : DIM;
            if (sequencer != nullptr)
            {
                const auto& tr = sequencer->getPattern().getTrack(t);
                out[(size_t) (8 + t)] = tr.soloed ? SOLOED : (tr.muted ? MUTED : UNMUTED);
            }
        }
        return PadSource::Layer;
    }

    // Func on a screen with page keys: the keys on the top row; the bottom row stays the screen's.
    if (funcHeld && activeScreen != nullptr && activeScreen->getPageKeyCount() > 0)
    {
        std::array<Screen::PadColour, 16> own {};
        if (mods(activeScreen)->getPadColours(own))
            for (size_t pad = 8; pad < 16; ++pad)
                out[pad] = own[pad];
        const int count = juce::jmin(8, activeScreen->getPageKeyCount());
        const int active = activeScreen->getActivePageKey();
        for (int k = 0; k < count; ++k)
            out[(size_t) k] = (k == active) ? KEY_ACTIVE : KEY;
        return PadSource::Layer;
    }

    // While recording, the step position runs across whatever the pads show (they keep doing
    // what they do). Pads drawn by the pad manager's mode get theirs there (STEP has its playhead).
    auto withRecordLight = [this, &out](PadSource source)
    {
        const int light = getRecordLightPad();
        if (light >= 0)
        {
            auto& c = out[(size_t) light];
            const bool alreadyRed = c.r >= 100 && c.g < 40 && c.b < 40;
            c = alreadyRed ? BRIGHT : REC_LIGHT;
        }
        return source;
    };

    if (auto* pinned = screenAt(pinnedIndex))
        if (mods(pinned)->getPadColours(out))
            return withRecordLight(PadSource::Pinned);

    if (activeScreen != nullptr && mods(activeScreen)->getPadColours(out))
        return withRecordLight(PadSource::Screen);

    out.fill(OFF);
    return PadSource::PadMode;
}

// =================================================================================================
// Display
// =================================================================================================
void ScreenManager::renderFrame()
{
    if (!display || !activeScreen)
        return;

    mods(activeScreen);
    display->clear();

    // A help card takes the place of the whole picture (the screen is not drawn under it).
    if (const auto key = getHelpCardKey(); key != HelpKey::None)
    {
        drawHelpCard(*display, getHelpCard(key, getHelpCardPage(key)));
        display->sendFrame();
        return;
    }

    drawNameBar(*display);
    activeScreen->render(*display);
    if (activeScreen->usesEncoderBar())
        ScreenLayout::drawEncoderBar(*display, *activeScreen);
    drawEncoderHighlight(*display);

    display->sendFrame();
}

juce::StringArray ScreenManager::getNameBarTags() const
{
    juce::StringArray tags;
    if (isRecordArmed())       tags.add(recordModeText());
    if (arpOn(currentTrack())) tags.add("ARP");
    if (isLatchOn())           tags.add("LATCH");
    if (scaleLocked)           tags.add("SCALE");
    if (pinnedIndex >= 0)      tags.add("PIN");
    if (fillHeld || (sequencer != nullptr && sequencer->isFillActive())) tags.add("FILL");
    return tags;
}

// Name bar, the same on every screen, 20 characters wide:
//   left    screen / page name and the selected track ("TRACKS T1")
//   tags    REC FREE / REC QUANT, ARP, LATCH, SCALE, PIN, FILL — as words when there is room, else as
//           their first letters ("RALSPF"; quantized recording is "Q")
//   text    the screen's status text, right-aligned (when there is no room: its leading words)
//   pages   "n/N" at the far right whenever the screen has more than one page
// For a moment the text is something else: a confirmation ("COPY TRK", "126 BPM") for a second, or,
// for 0.7 s after a turn on a bar screen, that knob's name and value. Those must be readable whole,
// so if they do not fit they push out, in this order: the tag words (letters stay), the tags, the
// page mark, the track number.
ScreenManager::NameBar ScreenManager::composeNameBar() const
{
    NameBar bar;
    if (activeScreen == nullptr)
        return bar;

    const int total = (DisplayRenderer::WIDTH - 4) / BitmapFont::TOTAL_CHAR_WIDTH;
    const auto name = activeScreen->getName();
    bar.left = name + " T" + juce::String(currentTrack() + 1);

    const int pageCount = activeScreen->getPageCount();
    if (pageCount > 1)
        bar.pages = juce::String(activeScreen->getPageIndex() + 1) + "/" + juce::String(pageCount);

    const auto tagList = getNameBarTags();
    const auto tagWords = tagList.joinIntoString(" ");
    juce::String tagLetters;
    for (auto& tag : tagList)
        tagLetters += (tag == "REC QUANT") ? juce::String("Q") : tag.substring(0, 1);

    auto room = [total](const juce::String& left, const juce::String& tags, const juce::String& pages)
    {
        return total - left.length() - 1 - (tags.isNotEmpty() ? tags.length() + 1 : 0)
                     - (pages.isNotEmpty() ? pages.length() + 1 : 0);
    };

    // A held edit filling, a confirmation, or the readout of the knob just turned.
    const auto now = nowMs();
    juce::String transient;
    bool wide = false;
    auto longestThatFits = [&](const juce::StringArray& wordings, int limit)
    {
        // (the last one if none does)
        for (auto& wording : wordings)
            if (wording.length() <= limit) return wording;
        return wordings.isEmpty() ? juce::String() : wordings[wordings.size() - 1];
    };
    if (holdEdit != Edit::None)
        transient = longestThatFits(getEditPlan(holdEdit).holdWordings(holdProgress()), total - name.length() - 1);
    else if (!message.isEmpty() && now - messageMs <= messageDurationMs)
    {
        // The longest wording that fits beside the screen's name; a wide one may take the whole bar.
        wide = messageWide;
        transient = longestThatFits(message, wide ? total : total - name.length() - 1);
    }
    else if (lastEncoderIndex >= 0 && now - lastEncoderMs <= READOUT_MS
             && activeScreen->getEncoderLayout() == Screen::EncoderLayout::Bar)
        transient = activeScreen->getEncoderReadout(lastEncoderIndex);

    if (transient.isNotEmpty())
    {
        struct Layout { juce::String left, tags, pages; };
        const Layout layouts[] = { { bar.left, tagWords, bar.pages }, { bar.left, tagLetters, bar.pages },
                                   { bar.left, {}, bar.pages }, { bar.left, {}, {} }, { name, {}, {} } };
        const Layout* chosen = nullptr;
        for (auto& l : layouts)
            if (chosen == nullptr && room(l.left, l.tags, l.pages) >= transient.length()) chosen = &l;
        if (chosen == nullptr && wide)
        {
            // Not even beside the bare name: the message alone.
            bar.left.clear();
            bar.pages.clear();
            bar.text = transient.substring(0, total);
            return bar;
        }
        if (chosen == nullptr)
            chosen = &layouts[4];
        bar.left = chosen->left;
        bar.tags = chosen->tags;
        bar.pages = chosen->pages;
        bar.text = transient.substring(0, juce::jmax(0, room(bar.left, bar.tags, bar.pages)));
        return bar;
    }

    bar.text = activeScreen->getSubPage();
    bar.tags = (room(bar.left, tagWords, bar.pages) >= bar.text.length()) ? tagWords : tagLetters;
    bar.tags = bar.tags.substring(0, juce::jmax(0, room(bar.left, {}, bar.pages)));
    const int left = room(bar.left, bar.tags, bar.pages);
    if (left <= 0)
        bar.text.clear();
    else if (bar.text.length() > left)
    {
        // The text starts with what matters most (the page name): keep as many whole words from
        // its start as fit ("ANALG 17+" -> "ANALG", never "ALG 17+"). One word that is too long
        // by itself keeps its end, as before.
        juce::String kept;
        for (auto& word : juce::StringArray::fromTokens(bar.text, " ", {}))
        {
            const juce::String next = kept.isEmpty() ? word : kept + " " + word;
            if (next.length() > left) break;
            kept = next;
        }
        bar.text = kept.isNotEmpty() ? kept : bar.text.substring(bar.text.length() - left);
    }
    return bar;
}

void ScreenManager::drawNameBar(DisplayRenderer& disp)
{
    const auto bar = composeNameBar();
    const int charW = BitmapFont::TOTAL_CHAR_WIDTH;

    disp.drawString(2, 1, bar.left);
    if (bar.tags.isNotEmpty())
        disp.drawString(2 + (bar.left.length() + 1) * charW, 1, bar.tags);

    int rightEdge = DisplayRenderer::WIDTH - 2;
    if (bar.pages.isNotEmpty())
    {
        rightEdge -= bar.pages.length() * charW;
        disp.drawString(rightEdge, 1, bar.pages);
        rightEdge -= charW;     // one character of space before it
    }
    if (bar.text.isNotEmpty())
        disp.drawString(rightEdge - bar.text.length() * charW, 1, bar.text);

    disp.drawHLine(0, NAME_BAR_HEIGHT, DisplayRenderer::WIDTH);
}

// For a moment after an encoder is turned, its label is shown inverted: in the bottom bar for
// bar-layout screens, on the cell's label for grid screens. It shows which knob is which without
// any permanent decoration.
void ScreenManager::drawEncoderHighlight(DisplayRenderer& disp)
{
    if (lastEncoderIndex < 0 || lastEncoderIndex >= 8)
        return;
    if (nowMs() - lastEncoderMs > READOUT_MS)
        return;

    switch (activeScreen->getEncoderLayout())
    {
        case Screen::EncoderLayout::Bar:
            disp.invertRegion(lastEncoderIndex * ScreenLayout::COLUMN_W, ScreenLayout::STATUS_BAR_Y + 1,
                              ScreenLayout::COLUMN_W, ScreenLayout::STATUS_BAR_H - 1);
            break;
        case Screen::EncoderLayout::Grid:
            disp.invertRegion(ParamGrid::cellX(lastEncoderIndex), ParamGrid::cellY(lastEncoderIndex) - 1, 31, 9);
            break;
        case Screen::EncoderLayout::None:
            break;
    }
}

// =================================================================================================
// Help cards: hold a hold key alone (Shift, Func, >, Up / Down, Record) and the display says what
// that key does here. One rule for all of them; what differs per key is its row in helpKeyDef().
// =================================================================================================
const ScreenManager::HelpKeyDef& ScreenManager::helpKeyDef(HelpKey key)
{
    //   key, name, delay, "letting go after the card still does the key's release action", cards
    static const std::vector<HelpKeyDef> table = {
        { HelpKey::Shift,   "Shift",     HELP_DELAY_MS, false, { { "SHIFT + PADS",    &ScreenManager::buildShiftPads },
                                                                 { "SHIFT + BUTTONS", &ScreenManager::buildShiftButtons },
                                                                 { "SHIFT + KNOBS",   &ScreenManager::buildShiftKnobs } } },
        { HelpKey::Func,    "Func",      HELP_DELAY_MS, false, { { "FUNC: EDIT",       &ScreenManager::buildFuncEdit },
                                                                 { "FUNC + PADS",      &ScreenManager::buildFuncPads },
                                                                 { "FUNC + KNOBS ETC", &ScreenManager::buildFuncKnobs } } },
        // ">": nothing happens when it is let go.
        { HelpKey::Pattern, ">",         HELP_DELAY_MS, false, { { "> PATTERNS",       &ScreenManager::buildPatternPads },
                                                                 { "> BUTTONS, KNOBS", &ScreenManager::buildPatternButtons } } },
        // Up / Down: a tap pages on release; a hold that showed the card does not.
        { HelpKey::Track,   "Up / Down", HELP_DELAY_MS, false, { { "UP DOWN: TRACKS",  &ScreenManager::buildTrackPads },
                                                                 { "UP DOWN: KNOBS",   &ScreenManager::buildTrackKnobs } } },
        // Record: one chord, not a layer; letting go switches recording on / off as always.
        { HelpKey::Record,  "Record",    HELP_DELAY_MS, true,  { { "RECORD",           &ScreenManager::buildRecord } } },
    };
    static const HelpKeyDef none { HelpKey::None, "", HELP_DELAY_MS, false, {} };
    for (auto& row : table)
        if (row.key == key)
            return row;
    return none;
}

const char* ScreenManager::getHelpKeyName(HelpKey key) { return helpKeyDef(key).name; }
bool ScreenManager::helpKeyActsOnReleaseAfterCard(HelpKey key) { return helpKeyDef(key).releaseActsAfterCard; }

// "Alone": no other modifier or hold key is down when the key goes down (Play and Scale count,
// although they have no cards). Held pads do not count, so a held step can be asked about ("what
// would clear do now?", "what does > do to this step?").
bool ScreenManager::holdDownBesides(HelpKey key) const
{
    return (key != HelpKey::Shift && shiftHeld) || (key != HelpKey::Func && funcHeld)
        || (key != HelpKey::Pattern && patternKeyHeld) || (key != HelpKey::Track && trackKeysHeld != 0)
        || (key != HelpKey::Record && recordHeld) || playHeld || fillHeld || scaleHeld;
}

// Another control was used: the card goes for the rest of this hold. If it had been up for the
// reader (not just the flash of an INST card under a quick chord), the hold is remembered as one
// that showed a card.
void ScreenManager::dismissHelp()
{
    if (helpKey != HelpKey::None && !helpDismissed && helpMode != HelpMode::Off
        && nowMs() - helpPressMs >= helpKeyDef(helpKey).delayMs)
        helpShown = true;
    helpDismissed = true;
}

// A hold key went down or up. It gets a card only when it went down alone.
void ScreenManager::helpKeyChanged(HelpKey key, bool pressed)
{
    if (!pressed)
    {
        if (helpKey == key)
        {
            helpKey = HelpKey::None;
            helpShown = false;
        }
        return;
    }
    if (holdDownBesides(key))
    {
        dismissHelp();              // also takes away the card of the key that is already down
        return;
    }
    helpKey = key;
    helpPressMs = nowMs();
    helpDismissed = false;
    helpShown = false;
}

// True when this hold of `key` showed its card, so that letting the key go does nothing else
// (asked just before the release is handled). With HELP on INST the card is up from the press, but
// a press shorter than the key's delay is still a tap.
bool ScreenManager::helpCardTookHold(HelpKey key) const
{
    if (helpKey != key || helpMode == HelpMode::Off || helpKeyDef(key).releaseActsAfterCard)
        return false;
    return helpShown || (!helpDismissed && nowMs() - helpPressMs >= helpKeyDef(key).delayMs);
}

ScreenManager::HelpKey ScreenManager::getHelpCardKey() const
{
    if (helpKey == HelpKey::None || helpDismissed || helpMode == HelpMode::Off || activeScreen == nullptr)
        return HelpKey::None;
    if (helpMode == HelpMode::Delayed && nowMs() - helpPressMs < helpKeyDef(helpKey).delayMs)
        return HelpKey::None;
    return helpKey;
}

int ScreenManager::getHelpCardCount(HelpKey key) const
{
    return (int) helpCardsFor(key).size();
}

int ScreenManager::getHelpCardPage(HelpKey key) const
{
    const int count = getHelpCardCount(key);
    if (count <= 0 || key == HelpKey::None)
        return 0;
    return juce::jlimit(0, count - 1, helpPage[(size_t) key]);
}

ScreenManager::HelpCard ScreenManager::getHelpCard(HelpKey key, int page) const
{
    HelpCard card;
    const auto& defs = helpCardsFor(key);
    if (activeScreen == nullptr || page < 0 || page >= (int) defs.size())
        return card;
    card.title = defs[(size_t) page].title;
    if (defs.size() > 1)            // (one card: no page mark, as on a screen with one page)
        card.pages = juce::String(page + 1) + "/" + juce::String((int) defs.size());
    mods(activeScreen);
    (this->*defs[(size_t) page].build)(card);
    return card;
}

juce::String ScreenManager::mapNameOf(int screenIndex) const
{
    return (screenIndex >= 0 && screenIndex < (int) traits.size()) ? traits[(size_t) screenIndex].mapName : juce::String();
}

// SHIFT + PADS: the screen map, as the pad lights show it. The current screen inverted, the pinned
// pad screen marked "*".
void ScreenManager::buildShiftPads(HelpCard& card) const
{
    HelpCard::PadRow top, bottom;
    top.line = 0;
    bottom.line = 2;
    for (int pad = 0; pad < 16; ++pad)
    {
        const int target = shiftPadScreenIndex[pad];
        if (target < 0 || target >= (int) screens.size())
            continue;
        auto& label = (pad < 8 ? top : bottom).labels[(size_t) (pad % 8)];
        label.text = mapNameOf(target);
        label.inverted = (target == activeIndex);
        label.marked = (target == pinnedIndex && pad >= 8);
    }
    card.padRows = { top, bottom };

    const bool pinnable = activeIndex >= 0 && activeIndex < (int) traits.size() && traits[(size_t) activeIndex].pinnable;
    if (pinnable)
        card.lines[4].text = (pinnedIndex == activeIndex) ? "HOLD THIS PAD: UNPIN" : "HOLD THIS PAD: PIN";
    else if (pinnedIndex >= 0)
        card.lines[4].text = "* = ITS PADS PINNED";
    card.lines[5].text = "COPY CLEAR PASTE:FUNC";
}

// SHIFT + BUTTONS, in the order the buttons sit on the unit (left to right).
void ScreenManager::buildShiftButtons(HelpCard& card) const
{
    card.lines[0].text = "OCT - +  KEY SPLIT";
    card.lines[1].text = "ARP      LATCH";
    card.lines[2].text = "SCALE    SCALE SCREEN";
    card.lines[3].text = "UP DOWN  TRACK - +";
    card.lines[4].text = "PLAY     STOP, REWIND";
    card.lines[5].text = "REC HOLD FILL";
}

void ScreenManager::buildShiftKnobs(HelpCard& card) const
{
    Screen::HelpFacts facts;
    activeScreen->getHelpFacts(facts);
    card.lines[0].text = "KNOB     FINE";
    card.lines[1].text = " A TENTH OF A STEP";
    if (facts.stageCurves)
        card.lines[2].text = "ATK DEC REL: CURVE";
    card.lines[3].text = "BANK     KNOB PAGE";
    card.lines[4].text = " AT ONCE. NOW: CARDS";
}

// FUNC: EDIT. The three verbs at fixed places, each with what it would act on right now
// (getEditPlan, which the action itself goes by); under them the two chords with Shift and what
// they would take back / do again.
void ScreenManager::buildFuncEdit(HelpCard& card) const
{
    static const char* const buttons[3] = { "REC  ", "PLAY ", ">    " };
    const Edit edits[3] = { Edit::Copy, Edit::Clear, Edit::Paste };
    EditPlan plans[3];
    bool any = false;
    for (int i = 0; i < 3; ++i)
    {
        plans[i] = getEditPlan(edits[i]);
        any = any || plans[i].kind != EditPlan::Kind::Nothing;
    }
    buildFuncUndo(card);

    if (!any)
    {
        card.lines[0].text = "COPY CLEAR PASTE:";
        card.lines[1].text = "NOT ON THIS SCREEN";
        card.lines[2].text = "(STEP, TRACKS, PARAM)";
        return;
    }

    const int nameRoom = HELP_LINE_CHARS - 5 - 6;
    for (int i = 0; i < 3; ++i)
    {
        const auto& plan = plans[i];
        auto& line = card.lines[(size_t) i];
        if (plan.kind == EditPlan::Kind::Nothing)
            line.text = juce::String(buttons[i]) + juce::String(editVerb(edits[i])).paddedRight(' ', 6) + "NOT HERE";
        else if (plan.hold && holdEdit == edits[i])
            // The button is down: the verb's place fills, and at the end the pattern is cleared.
            line.text = juce::String(buttons[i]) + holdProgress().paddedRight(' ', 6) + plan.nameWithin(nameRoom);
        else if (plan.hold)
            line.text = juce::String(buttons[i]) + plan.verb.paddedRight(' ', 6) + plan.nameWithin(nameRoom - 5) + " HOLD";
        else
            line.text = juce::String(buttons[i]) + plan.verb.paddedRight(' ', 6)
                      + (plan.empty ? juce::String("(EMPTY)") : plan.nameWithin(nameRoom));
        line.inverted = plan.kind == EditPlan::Kind::Undo;
    }
}

// The last three lines of the EDIT card, on every screen: Func + Shift + Play / Record and what
// each would do now. Knob turns are not undo steps; the card says so where there is room.
void ScreenManager::buildFuncUndo(HelpCard& card) const
{
    const auto undo = getEditPlan(Edit::Undo), redo = getEditPlan(Edit::Redo);
    const int room = HELP_LINE_CHARS - 6;
    card.lines[3].text = "WITH SHIFT: PLAY, REC";
    card.lines[4].text = undo.kind == EditPlan::Kind::Nothing ? juce::String("NOTHING TO UNDO") : "UNDO: " + undo.nameWithin(room);
    card.lines[5].text = redo.kind == EditPlan::Kind::Nothing ? juce::String("KNOB TURNS: NO UNDO") : "REDO: " + redo.nameWithin(room);
}

// FUNC + PADS: the page keys on the top row (named by the screen), and what else the screen does.
void ScreenManager::buildFuncPads(HelpCard& card) const
{
    Screen::HelpFacts facts;
    activeScreen->getHelpFacts(facts);
    const int keys = juce::jmin(8, activeScreen->getPageKeyCount());
    const bool pinElsewhere = pinnedIndex >= 0 && pinnedIndex != activeIndex;
    int next = 0;

    if (keys > 0)
    {
        card.lines[0].text = "TOP ROW: GO TO PAGE";
        HelpCard::PadRow row;
        row.line = 1;
        const int active = activeScreen->getActivePageKey();
        for (int k = 0; k < keys; ++k)
        {
            auto name = shortName(activeScreen->getPageKeyName(k), HELP_PAD_CHARS);
            row.labels[(size_t) k].text = name.isNotEmpty() ? name : juce::String(k + 1);
            row.labels[(size_t) k].inverted = (k == active);
        }
        card.padRows.push_back(row);
        card.lines[3].text = "BOTTOM ROW: " + (facts.funcBottomRow.isNotEmpty() ? facts.funcBottomRow : juce::String("NO CHANGE"));
        next = 4;
    }
    else
    {
        card.lines[0].text = facts.funcBottomRow.isNotEmpty() ? "BOTTOM ROW: " + facts.funcBottomRow
                                                              : juce::String("PADS: AS WITHOUT FUNC");
        next = 1;
    }
    // Under Func the pads are the screen's own, also while another screen's pads are pinned.
    if (pinElsewhere)
        card.lines[(size_t) next].text = "PIN: OFF WHILE HELD";
}

// FUNC + KNOBS ETC, in the order the controls sit on the unit.
void ScreenManager::buildFuncKnobs(HelpCard& card) const
{
    Screen::HelpFacts facts;
    activeScreen->getHelpFacts(facts);
    card.lines[0].text = "SETTINGS TAP TEMPO";
    card.lines[1].text = "KNOB     COARSE STEPS";
    card.lines[2].text = juce::String("BANK     ") + (facts.bankJump == Screen::BankJump::Section ? "SECTION JUMP"
                                                     : facts.bankJump == Screen::BankJump::FirstLast ? "FIRST / LAST"
                                                                                                      : "KNOB PAGE");
    card.lines[3].text = " AT ONCE. NOW: CARDS";
    if (facts.pressureRange)
    {
        card.lines[4].text = "UP DOWN  PRESSURE";
        card.lines[5].text = " RANGE OF LAST KNOB";
    }
}

// > PATTERNS: the sixteen patterns over the sixteen pads, as the pad lights show them. The current
// one inverted, the queued one marked "*", a pattern without trigs a dot.
void ScreenManager::buildPatternPads(HelpCard& card) const
{
    HelpCard::PadRow top, bottom;
    top.line = 0;
    bottom.line = 2;
    const int count = sequencer != nullptr ? juce::jmin(16, sequencer->getPatternCount()) : 0;
    const int current = sequencer != nullptr ? sequencer->getActivePatternIndex() : -1;
    const int queued = sequencer != nullptr ? sequencer->getPendingPattern() : -1;
    for (int pad = 0; pad < count; ++pad)
    {
        auto& label = (pad < 8 ? top : bottom).labels[(size_t) (pad % 8)];
        const bool shown = pad == current || pad == queued || patternHasTrigs(sequencer->getPattern(pad));
        label.text = shown ? juce::String(pad + 1) : juce::String(".");
        label.inverted = (pad == current);
        label.marked = (pad == queued && pad != current);
    }
    card.padRows = { top, bottom };

    if (sequencer == nullptr)
        return;
    card.lines[4].text = "PATTERN " + juce::String(current + 1);
    if (queued >= 0 && queued != current)
        card.lines[4].text << "  NEXT " << (queued + 1) << "*";
    card.lines[5].text = sequencer->isPlaying() ? "PAD: NEXT AT LOOP END" : "PAD: GO TO PATTERN";
}

// > BUTTONS, KNOBS: Record keeps a copy of the pattern, Play reloads it (or the pattern as the
// project had it), and with a step held a turn removes that knob's lock.
void ScreenManager::buildPatternButtons(HelpCard& card) const
{
    Screen::HelpFacts facts;
    activeScreen->getHelpFacts(facts);
    const int index = sequencer != nullptr ? sequencer->getActivePatternIndex() : -1;
    const bool kept = index >= 0 && index < (int) keptValid.size() && keptValid[(size_t) index];
    const bool loaded = index >= 0 && index < (int) loadedValid.size() && loadedValid[(size_t) index];

    card.lines[0].text = "REC      KEEP A COPY";
    card.lines[1].text = index >= 0 ? " OF PATTERN " + juce::String(index + 1) : juce::String(" OF THE PATTERN");
    card.lines[2].text = "PLAY     RELOAD IT";
    card.lines[3].text = kept ? " FROM THE KEPT COPY" : (loaded ? " AS IT WAS LOADED" : " (NOTHING TO RELOAD)");
    if (activeScreen->isStepHeld())
    {
        card.lines[4].text = "KNOB     REMOVE LOCK";
        card.lines[5].text = " ON THE HELD STEP";
    }
    else if (facts.stepLocks)
    {
        card.lines[4].text = "HOLD A STEP + KNOB:";
        card.lines[5].text = " REMOVE THAT LOCK";
    }
    else
        card.lines[4].text = "KNOB     NO CHANGE";
}

// UP DOWN: TRACKS. Tracks 1-8 over the top pads with their engines, the selected one inverted;
// under each, what its bottom pad shows: MUTE, SOLO or ON.
void ScreenManager::buildTrackPads(HelpCard& card) const
{
    HelpCard::PadRow top, bottom;
    top.line = 0;
    bottom.line = 2;
    const int count = juce::jmin(8, currentTrackCount());
    for (int t = 0; t < count; ++t)
    {
        auto& label = top.labels[(size_t) t];
        label.text = juce::String(t + 1);
        label.inverted = (t == currentTrack());
        if (sequencer != nullptr)
        {
            const auto& tr = sequencer->getPattern().getTrack(t);
            label.text << NoteUtils::engineAbbrev((int) tr.engineType);
            bottom.labels[(size_t) t].text = tr.soloed ? "SOLO" : (tr.muted ? "MUTE" : "ON");
        }
    }
    card.padRows = { top, bottom };
    card.lines[4].text = "TOP: SELECT A TRACK";
    card.lines[5].text = "BOTTOM: MUTE ON / OFF";
}

// UP DOWN: KNOBS: the turn that reaches every track, what a tap of the buttons does on this
// screen, and where solo is (it is not in this layer).
void ScreenManager::buildTrackKnobs(HelpCard& card) const
{
    Screen::HelpFacts facts;
    activeScreen->getHelpFacts(facts);
    if (facts.allTracksTurn)
    {
        card.lines[0].text = "KNOB     ALL TRACKS";
        card.lines[1].text = " THAT HAVE THIS CELL";
    }
    else
    {
        card.lines[0].text = "KNOB     NO CHANGE";
        card.lines[1].text = " (THIS TRACK ONLY)";
    }
    if (facts.tapUpDown.isNotEmpty())
    {
        card.lines[2].text = "TAP      " + facts.tapUpDown.substring(0, HELP_LINE_CHARS - 9);
        card.lines[3].text = " UP = BACK, DOWN = ON";
    }
    else
        card.lines[2].text = "TAP      NOTHING HERE";
    // (Under Up / Down a bottom pad mutes, with Func too.)
    card.lines[4].text = facts.funcBottomRow == "SOLO" ? "SOLO: LET GO, THEN" : "SOLO: TRACKS SCREEN,";
    card.lines[5].text = " FUNC + BOTTOM PAD";
}

// RECORD: what letting go does now, and the chord with Play.
void ScreenManager::buildRecord(HelpCard& card) const
{
    const bool armed = isRecordArmed(), running = isTransportRunning();
    card.lines[0].text = armed ? "LET GO   RECORD OFF" : "LET GO   RECORD ON";
    if (armed && running)
    {
        card.lines[1].text = " PLAYING GOES ON";
        card.lines[2].text = "+ PLAY   FREE / QUANT";
        card.lines[3].text = " NOW: " + recordModeText();
        return;
    }
    if (!armed)
        card.lines[1].text = running ? " RECORDS FROM NOW" : " THEN PLAY RECORDS";
    card.lines[2].text = running ? "+ PLAY   RECORD ON" : "+ PLAY   RECORD NOW";
    const bool counts = sequencer != nullptr && sequencer->getCountInBars() > 0;
    card.lines[3].text = running ? " PLAYING GOES ON" : counts ? " AFTER THE COUNT-IN" : " FROM THE TOP";
    card.lines[4].text = "AGAIN    FREE / QUANT";
    card.lines[5].text = " NOW: " + recordModeText();
}

// The card: title and "n/N" where the name bar is, six lines of text under it. Pad labels sit over
// their pad's sixteen-pixel column, alternate pads on alternate lines.
void ScreenManager::drawHelpCard(DisplayRenderer& disp, const HelpCard& card) const
{
    const int charW = BitmapFont::TOTAL_CHAR_WIDTH;
    disp.drawString(2, 1, card.title);
    if (card.pages.isNotEmpty())
        disp.drawString(DisplayRenderer::WIDTH - 2 - card.pages.length() * charW, 1, card.pages);
    disp.drawHLine(0, NAME_BAR_HEIGHT, DisplayRenderer::WIDTH);

    // Lines are 8 pixels apart; the lines after a row of pad labels start 2 pixels lower, which
    // sets the rows apart.
    auto lineY = [&card](int line)
    {
        int y = NAME_BAR_HEIGHT + 3 + line * 8;
        for (auto& row : card.padRows)
            if (row.line + 1 < line)
                y += 2;
        return y;
    };

    for (int i = 0; i < HELP_LINES; ++i)
    {
        const auto& line = card.lines[(size_t) i];
        if (line.text.isEmpty())
            continue;
        disp.drawString(1, lineY(i), line.text);
        if (line.inverted)
            disp.invertRegion(0, lineY(i) - 1, DisplayRenderer::WIDTH, 9);
    }

    for (auto& row : card.padRows)
    {
        for (int pad = 0; pad < 8; ++pad)
        {
            const auto& label = row.labels[(size_t) pad];
            if (label.text.isEmpty())
                continue;
            const juce::String text = label.text + (label.marked ? "*" : "");
            const int w = text.length() * charW - 1;
            const int x = juce::jlimit(1, DisplayRenderer::WIDTH - 1 - w, pad * ScreenLayout::COLUMN_W + ScreenLayout::COLUMN_W / 2 - w / 2);
            const int y = lineY(row.line + (pad % 2));
            disp.drawString(x, y, text);
            if (label.inverted)
                disp.invertRegion(x - 1, y - 1, label.text.length() * charW + 1, 9);
        }
    }
}
