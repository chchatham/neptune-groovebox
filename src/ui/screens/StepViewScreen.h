#pragma once

#include "../TurnNotice.h"
#include "../ScreenManager.h"
#include "../NoteUtils.h"
#include "../../sequencer/SequencerCore.h"
#include "../../controller/PadManager.h"
#include "../PadLEDFeedback.h"
#include "../GridScreen.h"
#include "ParamPageScreen.h"
#include "../../engine/DrumLane.h"
#include <functional>

class StepViewScreen : public Screen
{
public:
    StepViewScreen(SequencerCore& seq, PadManager& pads, PadLEDFeedback& leds)
        : sequencer(seq), padManager(pads), ledFeedback(leds)
    {
    }

    // With this set, a held step shows its detail pages: TRIG (the step's own settings) and the
    // track's parameter pages (locks for that step), each cell with the value the step will play.
    // They open when the press has lasted longer than TAP_MS (so a tap never flashes them), or at
    // once on a turn or a bank button, and close when the last held step is let go.
    void setLockPages(ParamPageScreen* pages) { lockPages = pages; }

    // A step whose notes carry recorded pad pressure has one more page, after the lock pages
    // (bank-up from TRIG): PRES. PRES = how much of the recorded pressure plays (0 = none, the
    // recording stays); >LCK (four clicks) turns it into parameter locks through the track's
    // pressure routes; CLR (four clicks) removes it. The bottom half shows the recorded curves.
    // The engine is needed for >LCK; `showStatus` says what >LCK and CLR did.
    void setAudioEngine(AudioEngine* audio) { audioEngine = audio; }
    std::function<void(const juce::String&)> showStatus;
    bool isPressurePage() const { return isDetailActive() && detailKind() == Detail::Pressure; }

    // The clock used for the tap / hold rule (tests inject their own).
    void setClock(std::function<juce::int64()> c) { clock = std::move(c); }
    static constexpr juce::int64 TAP_MS = 350;

    // Time-driven: the 30 Hz refresh draws the screen again and so finds the hold long enough.
    // Once open the pages stay open until no step is held.
    bool isDetailActive() const
    {
        if (getHeldStepIndex() < 0)
            return false;
        if (!detailActive && heldLongEnough())
            detailActive = true;
        return detailActive;
    }
    int getDetailPage() const { return currentDetailPage(); }
    bool isStepHeld() const override { return getHeldStepIndex() >= 0; }

    juce::String getName() const override
    {
        if (isDetailActive())
        {
            switch (detailKind())
            {
                case Detail::Trig:      return "TRIG";
                case Detail::Pressure:  return "PRES";
                case Detail::Lock:      break;
            }
            return (syncLockPage(), lockPages->getPageName());
        }
        return "STEP";
    }

    bool usesEncoderBar() const override { return !isDetailActive(); }
    EncoderLayout getEncoderLayout() const override
    {
        return isDetailActive() ? EncoderLayout::Grid : EncoderLayout::Bar;
    }

    // "n/N" is the encoder page and nothing else: the detail pages (TRIG + lock pages) while a
    // step is held; the screen's own knobs are a single page. The step page (which 16 steps the
    // pads show) is in the sub-page text.
    int getPageCount() const override { return isDetailActive() ? detailPageCount() : 1; }
    int getPageIndex() const override { return isDetailActive() ? currentDetailPage() : 0; }
    int getStepPageCount() const
    {
        auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
        return juce::jmax(1, (track.length + 15) / 16);
    }

    juce::String getSubPage() const override
    {
        if (notice.showing())
            return notice.shown();
        if (isDetailActive())
            // The step number first: when room is short the name bar keeps the leading words.
            return "S" + juce::String(getHeldStepIndex() + 1) + (detailKind() == Detail::Lock ? " LOCK" : "");
        return "S " + juce::String(currentPage * 16 + 1) + "-" + juce::String(currentPage * 16 + 16);
    }

    void render(DisplayRenderer& display) override
    {
        if (isDetailActive())
        {
            renderDetail(display);
            return;
        }

        int trackIdx = sequencer.getSelectedTrack();
        auto& track = sequencer.getPattern().getTrack(trackIdx);
        int pageOffset = currentPage * 16;
        int playingStep = sequencer.isPlaying() ? sequencer.getCurrentStep(trackIdx) : -1;
        const int lane = drumLane(trackIdx);

        // Two rows of 8 steps, fitting within content zone (y=11..52)
        // Row 0: y=11, Row 1: y=32. Each cell: 14w x 19h (with 1px gap each side)
        static constexpr int ROW_Y[2] = { 11, 32 };
        static constexpr int CELL_W = 14;
        static constexpr int CELL_H = 19;

        for (int row = 0; row < 2; ++row)
        {
            int y = ROW_Y[row];

            for (int col = 0; col < 8; ++col)
            {
                int stepIdx = pageOffset + row * 8 + col;
                int x = col * 16 + 1;

                if (stepIdx >= track.length)
                {
                    display.drawRect(x, y, CELL_W, CELL_H, false);
                    continue;
                }

                auto& step = track.getStep(stepIdx);
                bool isPlayhead = (stepIdx == playingStep);
                bool isHeld = heldStep[row * 8 + col] == stepIdx;
                // A drum track shows one drum (the pad selected on DRUMS): a cell is filled where
                // that drum hits; a step where only other drums hit keeps its number, underlined.
                const int laneNote = lane >= 0 ? DrumLane::find(step, lane) : -1;
                const bool filled = lane >= 0 ? (step.trig && laneNote >= 0) : step.trig;
                const size_t shownNote = laneNote >= 0 ? (size_t) laneNote : 0;

                // Always draw cell outline first
                display.drawRect(x, y, CELL_W, CELL_H, false);

                if (filled)
                {
                    // Triggered: fill the cell body (inside outline)
                    display.drawRect(x + 1, y + 1, CELL_W - 2, CELL_H - 2, true);

                    // Note name (inverted text since cell is filled); on a drum track the number
                    // of drums on the step when there is more than one.
                    if (lane >= 0)
                    {
                        if (step.notes.size() > 1)
                            display.drawStringInverted(x + 1, y + 2, juce::String((int) step.notes.size()));
                    }
                    else if (!step.notes.empty())
                    {
                        juce::String noteName = NoteUtils::noteToString(step.notes[0].pitch);
                        display.drawStringInverted(x + 1, y + 2, noteName.substring(0, 3));
                    }

                    // Velocity bar at bottom of cell (1px tall, inside outline)
                    if (!step.notes.empty())
                    {
                        int velW = (int)(step.notes[shownNote].velocity * (float)(CELL_W - 4));
                        if (velW > 0)
                        {
                            // Clear a strip for the velocity bar (inverted region)
                            for (int px = x + 2; px < x + 2 + (CELL_W - 4); ++px)
                                display.setPixel(px, y + CELL_H - 3, false);
                            // Draw the velocity bar pixels back on
                            for (int px = x + 2; px < x + 2 + velW; ++px)
                                display.setPixel(px, y + CELL_H - 3, true);
                        }
                    }

                    // Lock indicator dot (bottom-right, inverted)
                    if (!step.paramLocks.empty())
                        display.setPixel(x + CELL_W - 3, y + 2, false);

                    // Trig condition indicator (bottom-left, inverted)
                    if (step.trigCondition.type != TrigCondition::Always)
                        display.setPixel(x + 2, y + CELL_H - 5, false);

                    // Recorded pad pressure on the step's notes (bottom-right, inverted)
                    if (step.hasPressure())
                        display.setPixel(x + CELL_W - 3, y + CELL_H - 5, false);
                }
                else
                {
                    // Empty step: show step number (dim, centered)
                    int stepNum = stepIdx + 1;
                    if (stepNum < 10)
                        display.drawChar(x + 4, y + 6, '0' + (char)stepNum);
                    else
                    {
                        display.drawChar(x + 1, y + 6, '0' + (char)(stepNum / 10));
                        display.drawChar(x + 7, y + 6, '0' + (char)(stepNum % 10));
                    }
                    if (lane >= 0 && step.trig)         // other drums hit here
                        for (int px = x + 2; px < x + CELL_W - 2; ++px)
                            display.setPixel(px, y + CELL_H - 3, true);
                }

                // Playhead and held use invertRegion for consistent selection style
                if (isHeld)
                    display.invertRegion(x, y, CELL_W, CELL_H);
                else if (isPlayhead)
                    display.invertRegion(x, y, CELL_W, CELL_H);
            }
        }
    }

    // A press adds a trig to an empty step. An existing trig is removed only by a release within
    // TAP_MS with nothing edited meanwhile: a held step is never removed.
    void onPadPress(int padIndex, int velocity) override
    {
        juce::ignoreUnused(velocity);
        if (padIndex < 0 || padIndex >= 16)
            return;

        int trackIdx = sequencer.getSelectedTrack();
        auto& track = sequencer.getPattern().getTrack(trackIdx);
        int stepIdx = currentPage * 16 + padIndex;
        if (stepIdx >= track.length)
            return;

        // The pages open on the track's first sound page (MAIN where the engine has one) and
        // stay on the page last shown from one hold to the next, until the track changes or
        // STEP is opened again.
        if (trackIdx != detailTrack)
        {
            detailTrack = trackIdx;
            detailPage = 1;
            pressurePageWanted = false;
        }
        confirm.cancel();

        heldStep[padIndex] = stepIdx;
        editedWhileHeld[padIndex] = false;
        pressMs[padIndex] = now();

        auto& step = track.getStep(stepIdx);
        // A drum track: the pads are the selected drum's hits. The press puts that drum on the
        // step beside whatever else hits there.
        if (const int lane = drumLane(trackIdx); lane >= 0)
        {
            addedOnPress[padIndex] = DrumLane::add(step, lane);
            syncHeldStep();
            return;
        }
        addedOnPress[padIndex] = !step.trig;
        if (!step.trig)
        {
            step.trig = true;
            // On a drum track the new step plays the pad selected on DRUMS; elsewhere middle C.
            if (step.notes.empty())
                step.notes.push_back({ lockPages != nullptr ? lockPages->newStepNote(trackIdx) : 60, 1.0f });
        }
        syncHeldStep();
    }

    void onPadRelease(int padIndex) override
    {
        if (padIndex >= 0 && padIndex < 16 && heldStep[padIndex] >= 0)
        {
            auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
            const int stepIdx = heldStep[padIndex];
            // (Asked before the time is read: pages that are showing mean it was a hold.)
            const bool shown = isDetailActive();
            const bool tap = !shown && now() - pressMs[padIndex] <= TAP_MS;
            if (tap && !addedOnPress[padIndex] && !editedWhileHeld[padIndex] && stepIdx < track.length)
            {
                // A drum track: only the selected drum leaves the step; the others play on.
                if (const int lane = drumLane(sequencer.getSelectedTrack()); lane >= 0)
                    DrumLane::remove(track.getStep(stepIdx), lane);
                else
                    track.getStep(stepIdx).trig = false;
            }
            heldStep[padIndex] = -1;
            editedWhileHeld[padIndex] = false;
        }
        if (getHeldStepIndex() < 0)
            detailActive = false;
        syncHeldStep();
    }

    // No step held: the track's sequencer settings. A step held: that step's detail pages - TRIG
    // (the step itself) and the track's parameter pages (locks). Shift = fine, Func = coarse.
    void onEncoderChange(int encoderIndex, int value) override
    {
        const int clicks = (value > 64) ? value - 128 : value;
        int held = getHeldStepIndex();
        if (held < 0)
        {
            editTrackSetting(encoderIndex, clicks);
            return;
        }

        auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
        if (held >= track.length || encoderIndex < 0 || encoderIndex >= 8)
            return;

        detailActive = true;
        const auto kind = detailKind();
        if (kind != Detail::Pressure || (encoderIndex != LOCK_CELL && encoderIndex != CLEAR_CELL))
            confirm.cancel();
        if (kind == Detail::Pressure)
        {
            auto& step = track.getStep(held);
            if (encoderIndex == LOCK_CELL || encoderIndex == CLEAR_CELL)
            {
                // Four clicks clockwise: turn the recorded pressure into locks, or remove it.
                if (confirm.step(encoderIndex, clicks, now()))
                {
                    // Both are undo steps: baking writes locks on every step the note spans (the
                    // track's steps are kept), clearing changes this step only.
                    const UndoMark mark(*this, pressureUndoPoint(held, encoderIndex == LOCK_CELL));
                    if (encoderIndex == LOCK_CELL) pressureToLocks(track, held);
                    else                           { step.clearPressureLanes(); say("PRESSURE CLEARED"); }
                    if (!step.hasPressure())
                        showDetailPage(0);      // the page has gone with the pressure: back to TRIG
                }
            }
            else
            {
                auto cells = pressureCells(step);
                auto& c = cells[(size_t) encoderIndex];
                if (c.used())
                    c.set(c.spec.turned(c.get(), clicks, turnStep()));
            }
        }
        else if (kind == Detail::Trig)
        {
            auto cells = trigCells(track.getStep(held));
            auto& c = cells[(size_t) encoderIndex];
            if (encoderIndex == 7 && clicks != 0 && !anyHeldHasLocks())
                notice.say("NO LOCKS");         // LCKS counts the step's locks and clears them: there are none
            else if (encoderIndex == 7 && heldCount() > 1)
            {
                if (clicks < 0) clearLocksOfAllHeld();
            }
            else if (c.used())
            {
                c.set(c.spec.turned(c.get(), clicks, turnStep()));
                copyTrigCellToOthers(track, encoderIndex, c);
            }
        }
        else
        {
            lockPages->setModifiers(shiftDown(), funcDown());
            lockPages->setExternalHeldStep(held);
            lockPages->setCurrentPageIndex(currentDetailPage() - 1);
            lockPages->onEncoderChange(encoderIndex, value);
        }
        markEdited();
    }

    TurnNotice notice;

    // Hold a step + hold > + turn: that knob's lock is removed from the held step (on TRIG the
    // cell returns to its default).
    void onEncoderLockClear(int encoderIndex) override
    {
        int held = getHeldStepIndex();
        auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
        if (held < 0 || held >= track.length || encoderIndex < 0 || encoderIndex >= 8)
            return;
        auto& st = track.getStep(held);
        detailActive = true;
        confirm.cancel();
        if (detailKind() == Detail::Pressure)
        {
            if (encoderIndex == 0) st.pressureScale = 1.0f;             // PRES: as recorded
        }
        else if (detailKind() == Detail::Trig)
        {
            auto cells = trigCells(st);
            auto& c = cells[(size_t) encoderIndex];
            if (encoderIndex == 7 && heldCount() > 1)
                clearLocksOfAllHeld();
            else if (encoderIndex == 7)                                 // LCKS: all of them (one undo step)
            {
                const UndoMark mark(*this, locksUndoPoint(held));
                st.paramLocks.clear();
            }
            else if (c.used())
            {
                c.set(c.spec.defaultVal);
                copyTrigCellToOthers(track, encoderIndex, c);
            }
        }
        else
        {
            lockPages->setExternalHeldStep(held);
            lockPages->setCurrentPageIndex(currentDetailPage() - 1);
            // As on PARAM: the cell's lock, or with Shift on an envelope stage its curve's lock.
            lockPages->setModifiers(shiftDown(), funcDown());
            lockPages->onEncoderLockClear(encoderIndex);
        }
        markEdited();
    }

    // Func + Record / Play / > with a step held: copy, clear, paste that whole step (notes,
    // velocity, length, condition, locks). Without a held step the app applies its default (the
    // track's steps). Taking a clear or paste back is ScreenManager's undo history, not this screen's.
    bool onCopy() override
    {
        Step* st = heldStepPtr();
        if (st == nullptr) return false;
        clipboard = *st;
        hasClipboard = true;
        markEdited();
        return true;
    }

    bool onClear() override { return replaceHeld(nullptr); }

    // What the three act on right now, for ScreenManager (who acts) and the help card: the held
    // step (the one the pages show), or nothing of this screen's own.
    bool getEditScope(EditScope& scope) const override
    {
        if (const_cast<StepViewScreen*>(this)->heldStepPtr() == nullptr) return false;
        scope.names = { "STEP " + juce::String(getHeldStepIndex() + 1) };
        scope.canPaste = hasClipboard;
        scope.step = getHeldStepIndex();
        return true;
    }

    // Help cards: on a held step's lock pages Shift + turn bends an envelope stage, as on PARAM.
    // A tap of Up / Down moves the step page; steps are held here.
    void getHelpFacts(HelpFacts& facts) const override
    {
        facts.tapUpDown = "STEP PAGE";
        facts.stepLocks = true;
        if (!isDetailActive() || detailKind() != Detail::Lock || lockPages == nullptr) return;
        syncLockPage();
        HelpFacts locks;
        lockPages->getHelpFacts(locks);
        facts.stageCurves = locks.stageCurves;
    }

    bool onPaste() override
    {
        if (heldStepPtr() == nullptr) return false;
        if (!hasClipboard) { markEdited(); return true; }
        return replaceHeld(&clipboard);
    }

    bool wantsKeyboardEvents() const override
    {
        return getHeldStepIndex() >= 0;
    }

    void onKeyboardNoteOn(int note, int velocity) override
    {
        int held = getHeldStepIndex();
        if (held < 0)
            return;

        int trackIdx = sequencer.getSelectedTrack();
        auto& track = sequencer.getPattern().getTrack(trackIdx);
        if (held >= track.length)
            return;

        // A drum track: the key's drum joins the step (taken off again by a tap in its own lane).
        const bool drums = drumLane(trackIdx) >= 0;
        auto setNote = [&](Step& st)
        {
            if (drums)
            {
                DrumLane::add(st, DrumEngine::padForNote(note), velocity / 127.0f);
                return;
            }
            st.notes.clear();
            st.notes.push_back({note, velocity / 127.0f});
            if (!st.trig)
                st.trig = true;
        };
        setNote(track.getStep(held));
        forEachOtherHeld(track, [&](int other) { setNote(track.getStep(other)); });   // every held step takes the key

        markEdited();
    }

    // Bank buttons = encoder page: the detail pages while a step is held. The screen's own knobs
    // are one page, so without a held step they do nothing.
    void onArrowLeft() override
    {
        if (getHeldStepIndex() < 0) return;
        showDetailPage((currentDetailPage() + detailPageCount() - 1) % detailPageCount());      // goes round
        detailActive = true;
        markEdited();
    }

    void onArrowRight() override
    {
        if (getHeldStepIndex() < 0) return;
        showDetailPage((currentDetailPage() + 1) % detailPageCount());
        detailActive = true;
        markEdited();
    }

    // Up / Down = which 16 steps the pads show.
    void onPadPagePrev() override { if (currentPage > 0) --currentPage; }
    void onPadPageNext() override { if (currentPage < getStepPageCount() - 1) ++currentPage; }
    void showStep(int stepIndex) override { currentPage = juce::jlimit(0, getStepPageCount() - 1, stepIndex / 16); }

    void onActivate() override
    {
        padManager.setMode(PadManager::Mode::Step);
        detailPage = 1;
        pressurePageWanted = false;
        confirm.cancel();
    }

    void onDeactivate() override
    {
        for (auto& h : heldStep) h = -1;
        detailActive = false;
        if (lockPages) lockPages->setExternalHeldStep(-1);
    }

    juce::String getEncoderName(int encoderIndex) const override
    {
        if (encoderIndex < 0 || encoderIndex >= 8) return {};
        if (isDetailActive())
        {
            if (detailKind() == Detail::Lock)
                return (syncLockPage(), lockPages->getEncoderName(encoderIndex));
            auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
            auto cells = const_cast<StepViewScreen*>(this)->detailCells(track.getStep(getHeldStepIndex()));
            return cells[(size_t) encoderIndex].used()
                ? juce::String(cells[(size_t) encoderIndex].spec.label) : juce::String("---");
        }

        // The track's sequencer settings: length, speed, playback quantize ... tempo.
        switch (encoderIndex)
        {
            case 0: return "LN";
            case 1: return "SP";
            case 2: return "QN";
            case 7: return juce::String((int) sequencer.getTempo());
            default: return "--";
        }
    }

    // Full name and value of knob i, for the name bar.
    juce::String getEncoderReadout(int encoderIndex) const override
    {
        if (encoderIndex < 0 || encoderIndex >= 8) return {};
        if (notice.showing())
            return notice.shown();
        auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
        if (isDetailActive())
        {
            if (detailKind() == Detail::Lock)
                return (syncLockPage(), lockPages->getEncoderReadout(encoderIndex));
            auto cells = const_cast<StepViewScreen*>(this)->detailCells(track.getStep(getHeldStepIndex()));
            auto& c = cells[(size_t) encoderIndex];
            if (!c.used()) return {};
            const float v = c.get();
            return juce::String(c.spec.label) + " " + (c.text ? c.text(v) : c.spec.format(v));
        }
        switch (encoderIndex)
        {
            case 0: return "LENGTH " + juce::String(track.length);
            case 1: return "SPEED " + juce::String(speedSpec().format((float) speedIndex(track.speedMultiplier)));
            case 2: return "QUANTIZE " + juce::String((int) std::lround(sequencer.getQuantizeAmount() * 100.0f)) + "%";
            case 7: return "BPM " + juce::String(sequencer.getTempo(), 1);
            default: return {};
        }
    }

    // The step page (which 16 steps the pads show).
    int getCurrentPage() const { return currentPage; }
    void setCurrentPage(int page) { currentPage = page; }

    // The step being held for editing, or -1. With several held it is the one on the lowest pad
    // (top row left to right, then the bottom row): the pages show that step, and what a knob or a
    // key gives it is given to every other held step too (the same value, not the same change).
    // PRES and Func + Record / Play / > act on that step alone.
    int getHeldStepIndex() const
    {
        for (int i = 0; i < 16; ++i)
            if (heldStep[i] >= 0)
                return heldStep[i];
        return -1;
    }

private:
    juce::int64 now() const { return clock ? clock() : UiClock::nowMs(); }
    int lockPageCount() const { return lockPages != nullptr ? lockPages->getPageCount() : 0; }
    // A step has been down for longer than a tap.
    bool heldLongEnough() const
    {
        const auto t = now();
        for (int i = 0; i < 16; ++i)
            if (heldStep[i] >= 0 && t - pressMs[i] > TAP_MS)
                return true;
        return false;
    }
    // The lock pages always know the held step (a drum track's page list depends on it).
    void syncHeldStep() const
    {
        if (lockPages == nullptr) return;
        const int lead = getHeldStepIndex();
        lockPages->setExternalHeldStep(lead);
        int others[16];
        int count = 0;
        for (int i = 0; i < 16; ++i)
            if (heldStep[i] >= 0 && heldStep[i] != lead)
                others[count++] = heldStep[i];
        lockPages->setExternalAlsoHeld(others, count);
    }
    // The drum shown on a drum track (the pad selected on DRUMS), -1 on any other track.
    int drumLane(int trackIdx) const { return lockPages != nullptr ? lockPages->drumLaneOf(trackIdx) : -1; }
    // The note of a step that NOTE and VEL show: on a drum track the selected drum's, else the first.
    size_t shownNoteOf(const Step& step) const
    {
        const int lane = drumLane(sequencer.getSelectedTrack());
        const int i = lane >= 0 ? DrumLane::find(step, lane) : -1;
        return i >= 0 ? (size_t) i : 0;
    }
    int heldCount() const
    {
        int count = 0;
        for (int i = 0; i < 16; ++i) count += heldStep[i] >= 0 ? 1 : 0;
        return count;
    }
    // The held steps other than the one the pages show.
    template <typename Fn>
    void forEachOtherHeld(Track& track, Fn&& fn) const
    {
        const int lead = getHeldStepIndex();
        for (int i = 0; i < 16; ++i)
            if (heldStep[i] >= 0 && heldStep[i] != lead && heldStep[i] < track.length)
                fn(heldStep[i]);
    }
    bool anyHeldHasLocks() const
    {
        auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
        for (int i = 0; i < 16; ++i)
            if (heldStep[i] >= 0 && heldStep[i] < track.length && !track.getStep(heldStep[i]).paramLocks.empty())
                return true;
        return false;
    }
    // A TRIG cell just set on the shown step: the other held steps get the same value, where they
    // have that cell (PROB / A / B exist only under their own COND).
    void copyTrigCellToOthers(Track& track, int encoderIndex, GridScreen::Cell& shown)
    {
        const float value = shown.get();
        const juce::String label(shown.spec.label);
        forEachOtherHeld(track, [&](int other)
        {
            auto cells = trigCells(track.getStep(other));
            auto& c = cells[(size_t) encoderIndex];
            if (c.used() && label == juce::String(c.spec.label))
                c.set(c.spec.clamp(value));
        });
    }
    // LCKS with several steps held: every held step's locks go, as one undo step.
    void clearLocksOfAllHeld()
    {
        auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
        UndoPoint point;
        point.scope = UndoPoint::Scope::TrackSteps;
        point.track = sequencer.getSelectedTrack();
        point.step = getHeldStepIndex();
        point.verb = "CLEAR";
        const juce::String steps = juce::String(heldCount()) + " STEPS";
        point.names = { steps + " LOCKS", steps + " LCKS", "LOCKS" };
        const UndoMark mark(*this, point);
        for (int i = 0; i < 16; ++i)
            if (heldStep[i] >= 0 && heldStep[i] < track.length)
                track.getStep(heldStep[i]).paramLocks.clear();
    }
    // The detail pages: TRIG, the track's parameter pages, and PRES when the held step has recorded
    // pressure. PRES is remembered as a wish: a step without pressure shows TRIG in its place.
    enum class Detail { Trig, Lock, Pressure };
    bool heldHasPressure() const
    {
        const int held = getHeldStepIndex();
        auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
        return held >= 0 && held < track.length && track.getStep(held).hasPressure();
    }
    int detailPageCount() const { return lockPageCount() + 1 + (heldHasPressure() ? 1 : 0); }
    int currentDetailPage() const
    {
        if (pressurePageWanted)
            return heldHasPressure() ? lockPageCount() + 1 : 0;
        return juce::jlimit(0, lockPageCount(), detailPage);
    }
    Detail detailKind() const
    {
        const int page = currentDetailPage();
        return page == 0 ? Detail::Trig : (page > lockPageCount() ? Detail::Pressure : Detail::Lock);
    }
    void showDetailPage(int page)
    {
        pressurePageWanted = page > lockPageCount();
        if (!pressurePageWanted)
            detailPage = page;
        confirm.cancel();
    }
    std::array<GridScreen::Cell, 8> detailCells(Step& step)
    {
        return detailKind() == Detail::Pressure ? pressureCells(step) : trigCells(step);
    }
    void say(const juce::String& text) { if (showStatus) showStatus(text); }

    // Points the lock pages at the held step and the detail page being shown.
    void syncLockPage() const
    {
        if (lockPages == nullptr || detailKind() != Detail::Lock) return;
        lockPages->setExternalHeldStep(getHeldStepIndex());
        lockPages->setCurrentPageIndex(currentDetailPage() - 1);
    }

    Step* heldStepPtr()
    {
        int held = getHeldStepIndex();
        auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
        return (held >= 0 && held < track.length) ? &track.getStep(held) : nullptr;
    }

    // Clear (source null) or paste the held step.
    bool replaceHeld(const Step* source)
    {
        Step* st = heldStepPtr();
        if (st == nullptr) return false;
        if (source != nullptr) { *st = *source; st->trig = true; }
        else                   st->clear();
        markEdited();
        return true;
    }

    // The LCKS cell clears every lock of the step at one click: an undo step, named as the same
    // clear is on PARAM.
    UndoPoint locksUndoPoint(int stepIndex) const
    {
        UndoPoint point;
        point.scope = UndoPoint::Scope::Step;
        point.track = sequencer.getSelectedTrack();
        point.step = stepIndex;
        point.verb = "CLEAR";
        const juce::String step = "S" + juce::String(stepIndex + 1);
        point.names = { step + " LOCKS", step + " LCKS", step };
        return point;
    }

    UndoPoint pressureUndoPoint(int stepIndex, bool toLocks) const
    {
        UndoPoint point;
        point.scope = toLocks ? UndoPoint::Scope::TrackSteps : UndoPoint::Scope::Step;
        point.track = sequencer.getSelectedTrack();
        point.step = stepIndex;
        const juce::String step = "S" + juce::String(stepIndex + 1);
        point.verb = toLocks ? "LOCK" : "CLEAR";
        point.names = { step + " PRESSURE", step + " PRES", step };
        return point;
    }

    static ParamSpec speedSpec()
    {
        static const char* const names[] = { "1/4", "1/2", "1x", "2x", "4x" };
        return { "SPD", 0, 0, 4, 2, ParamKind::Choice, "", names, 5 };
    }
    static int speedIndex(float multiplier)
    {
        static const float speeds[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
        int idx = 2;
        for (int i = 0; i < 5; ++i)
            if (std::abs(speeds[i] - multiplier) < 0.01f) idx = i;
        return idx;
    }

    // Encoders with no step held: track length, speed, playback quantize ... tempo.
    void editTrackSetting(int encoderIndex, int clicks)
    {
        auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
        switch (encoderIndex)
        {
            case 0:
            {
                const ParamSpec spec { "LEN", 0, 1, 64, 16, ParamKind::Int };
                track.setLength((int) spec.turned((float) track.length, clicks, turnStep()));
                currentPage = juce::jmin(currentPage, (track.length - 1) / 16);
                break;
            }
            case 1:
            {
                static const float speeds[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
                const int idx = (int) speedSpec().turned((float) speedIndex(track.speedMultiplier), clicks, turnStep());
                track.speedMultiplier = speeds[juce::jlimit(0, 4, idx)];
                break;
            }
            case 2:
            {
                const ParamSpec spec { "QNT", 0, 0, 1, 0, ParamKind::Linear };
                sequencer.setQuantizeAmount(spec.turned(sequencer.getQuantizeAmount(), clicks, turnStep()));
                break;
            }
            case 7:
            {
                const ParamSpec spec { "BPM", 0, 20, 300, 120, ParamKind::Linear, "bpm" };
                sequencer.setTempo(spec.turned(sequencer.getTempo(), clicks, turnStep()));
                break;
            }
            default: break;
        }
    }

    // TRIG page: NOTE VEL LEN MICR / COND PROB-or-A B LCKS for one step.
    std::array<GridScreen::Cell, 8> trigCells(Step& step)
    {
        static const char* const conds[] = { "ALL", "PROB", "A:B", "FILL", "!FIL", "1ST", "!1ST", "PRE", "!PRE" };
        auto L = ParamKind::Linear; auto I = ParamKind::Int; auto C = ParamKind::Choice;
        std::array<GridScreen::Cell, 8> c;

        c[0] = { { "NOTE", 0, 0, 127, 60, I, "note" },
                 [this, &step] { return step.notes.empty() ? 60.0f : (float) step.notes[shownNoteOf(step)].pitch; },
                 [this, &step](float v)
                 {
                     if (step.notes.empty()) step.notes.push_back({ 60, 1.0f });
                     if (drumLane(sequencer.getSelectedTrack()) >= 0)   // drums: this drum only becomes another
                     {
                         step.notes[shownNoteOf(step)].pitch = juce::jlimit(0, 127, (int) v);
                         return;
                     }
                     int shift = (int) v - step.notes[0].pitch;      // a chord moves together
                     for (auto& n : step.notes) n.pitch = juce::jlimit(0, 127, n.pitch + shift);
                 },
                 [](float v) { return NoteUtils::noteToString((int) v); } };
        c[1] = { { "VEL", 0, 0, 1, 1, L },
                 [this, &step] { return step.notes.empty() ? 1.0f : step.notes[shownNoteOf(step)].velocity; },
                 [this, &step](float v)
                 {
                     if (drumLane(sequencer.getSelectedTrack()) >= 0 && !step.notes.empty())
                         step.notes[shownNoteOf(step)].velocity = v;    // drums: each has its own velocity
                     else
                         for (auto& n : step.notes) n.velocity = v;
                 },
                 [](float v) { return juce::String((int) std::lround(v * 127.0f)); } };
        c[2] = { { "LEN", 0, 0.25f, 16, 1, L, "x", nullptr, 0, 0.25f },
                 [&step] { return step.length; },
                 [&step](float v) { step.length = v; }, nullptr };
        c[3] = { { "MICR", 0, -0.5f, 0.5f, 0, L },
                 [&step] { return step.microTiming; },
                 [&step](float v) { step.microTiming = v; }, nullptr };
        c[4] = { { "COND", 0, 0, 8, 0, C, "", conds, 9 },
                 [&step] { return (float) (int) step.trigCondition.type; },
                 [&step](float v)
                 {
                     auto t = (TrigCondition) (int) v;
                     if (t != step.trigCondition.type)
                     {
                         step.trigCondition.type = t;
                         if (t == TrigCondition::Probability) step.trigCondition.probability = 0.5f;
                         if (t == TrigCondition::A_B) { step.trigCondition.a = 1; step.trigCondition.b = 2; }
                     }
                 }, nullptr };
        if (step.trigCondition.type == TrigCondition::Probability)
        {
            c[5] = { { "PROB", 0, 0, 1, 0.5f, L },
                     [&step] { return step.trigCondition.probability; },
                     [&step](float v) { step.trigCondition.probability = v; }, nullptr };
        }
        else if (step.trigCondition.type == TrigCondition::A_B)
        {
            c[5] = { { "A", 0, 1, 16, 1, I },
                     [&step] { return (float) step.trigCondition.a; },
                     [&step](float v) { step.trigCondition.a = juce::jlimit(1, step.trigCondition.b, (int) v); }, nullptr };
            c[6] = { { "B", 0, 2, 16, 2, I },
                     [&step] { return (float) step.trigCondition.b; },
                     [&step](float v)
                     {
                         step.trigCondition.b = (int) v;
                         step.trigCondition.a = juce::jlimit(1, step.trigCondition.b, step.trigCondition.a);
                     }, nullptr };
        }
        // Number of parameter locks on the step; turning it down clears them.
        c[7] = { { "LCKS", 0, 0, 16, 0, I },
                 [&step] { return (float) juce::jmin(16, (int) step.paramLocks.size()); },
                 [this, &step](float v)
                 {
                     if ((int) v >= (int) step.paramLocks.size()) return;
                     const UndoMark mark(*this, locksUndoPoint(getHeldStepIndex()));
                     step.paramLocks.clear();
                 }, nullptr };
        return c;
    }

    // PRES page: PRES >LCK CLR, and the recorded curves in the bottom half.
    static constexpr int LOCK_CELL = 1, CLEAR_CELL = 2;
    std::array<GridScreen::Cell, 8> pressureCells(Step& step)
    {
        auto L = ParamKind::Linear;
        std::array<GridScreen::Cell, 8> c;
        c[0] = { { "PRES", 0, 0, 2, 1, L, "", nullptr, 0, 0.01f },
                 [&step] { return step.pressureScale; },
                 [&step](float v) { step.pressureScale = v; },
                 [](float v) { return juce::String((int) std::lround(v * 100.0f)) + "%"; } };
        auto confirmCell = [this](const char* label, int index)
        {
            return GridScreen::Cell { { label, 0, 0, (float) TurnConfirm::STEPS, 0, ParamKind::Linear },
                                      [this, index] { return confirm.knob == index ? (float) confirm.steps : 0.0f; },
                                      [](float) {},
                                      [this, index](float)
                                      {
                                          return confirm.knob == index && confirm.steps > 0 ? confirm.progress()
                                                                                             : juce::String("TURN>");
                                      } };
        };
        c[LOCK_CELL] = confirmCell(">LCK", LOCK_CELL);
        c[CLEAR_CELL] = confirmCell("CLR", CLEAR_CELL);
        return c;
    }

    // >LCK: the held step's recorded pressure becomes parameter locks (AudioEngine::bakePressureToLocks).
    void pressureToLocks(Track& track, int held)
    {
        if (audioEngine == nullptr) { say("NO ROUTE"); return; }
        const auto done = audioEngine->bakePressureToLocks(sequencer.getSelectedTrack(), track, held);
        if (done.locked == 0)                say("NO ROUTE");       // nothing to lock: the pressure stays
        else if (done.locked < done.routes)  say(juce::String(done.locked) + " OF " + juce::String(done.routes) + " ROUTES");
        else                                 say("LOCKED " + juce::String(done.steps) + (done.steps == 1 ? " STEP" : " STEPS"));
    }

    // Every note's recorded pressure over the step's length, as it will play (PRES applied).
    void drawPressureCurves(DisplayRenderer& display, const Step& step)
    {
        static constexpr int X0 = 2, W = 124, BASE = ParamGrid::ROW2_Y + 20, H = 18;
        float span = step.length;
        for (auto& n : step.notes)
            span = juce::jmax(span, n.lane.lengthInSteps());
        for (int x = 0; x < W; x += 2)
            display.setPixel(X0 + x, BASE, true);                   // zero line
        for (auto& n : step.notes)
        {
            if (n.lane.empty()) continue;
            int last = -1;
            for (int x = 0; x < W; ++x)
            {
                const float t = span * (float) x / (float) (W - 1);
                if (t > step.length) break;                         // the note has ended
                const float v = juce::jlimit(0.0f, 1.0f, n.lane.valueAt(t) * step.pressureScale);
                const int y = BASE - (int) std::lround(v * (float) H);
                const int from = last < 0 ? y : juce::jmin(last, y), to = last < 0 ? y : juce::jmax(last, y);
                for (int py = from; py <= to; ++py)
                    display.setPixel(X0 + x, py, true);
                last = y;
            }
        }
    }

    void renderDetail(DisplayRenderer& display)
    {
        int held = getHeldStepIndex();
        auto& track = sequencer.getPattern().getTrack(sequencer.getSelectedTrack());
        const auto kind = detailKind();
        if (kind == Detail::Lock)
        {
            lockPages->setExternalHeldStep(held);
            lockPages->setCurrentPageIndex(currentDetailPage() - 1);
            lockPages->render(display);
            return;
        }

        auto& step = track.getStep(held);
        auto cells = detailCells(step);
        for (int i = 0; i < (kind == Detail::Pressure ? 4 : 8); ++i)
        {
            auto& c = cells[(size_t) i];
            if (!c.used()) { ParamGrid::drawEmptyCell(display, i); continue; }
            float v = c.get();
            ParamGrid::drawCell(display, i, c.spec, v, c.spec.label, c.text ? c.text(v) : juce::String());
        }
        if (kind == Detail::Pressure)
            drawPressureCurves(display, step);
        // TRIG is the first mark, the track's parameter pages follow, then PRES when the step has it.
        const int locks = lockPageCount();
        ParamGrid::drawPageStrip(display, detailPageCount(), currentDetailPage(),
                                 [locks](int i) { return i == 1 || i == locks + 1; });
    }

    void markEdited()
    {
        for (int i = 0; i < 16; ++i)
            if (heldStep[i] >= 0)
                editedWhileHeld[i] = true;
    }

    SequencerCore& sequencer;
    PadManager& padManager;
    [[maybe_unused]] PadLEDFeedback& ledFeedback;
    int currentPage = 0;         // step page: which 16 steps the pads show
    ParamPageScreen* lockPages = nullptr;
    AudioEngine* audioEngine = nullptr;
    bool pressurePageWanted = false;        // PRES was the page last shown
    TurnConfirm confirm;                    // >LCK and CLR on PRES
    mutable bool detailActive = false;      // set by a turn, a bank button or a long enough hold
    int detailPage = 1;          // 0 = TRIG, n = parameter page n-1; remembered between holds.
                                 // Starts on the first parameter page (MAIN): hold a step = its locks.
    int detailTrack = -1;        // the track detailPage belongs to
    std::function<juce::int64()> clock;
    int heldStep[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    bool editedWhileHeld[16] = {};
    bool addedOnPress[16] = {};
    juce::int64 pressMs[16] = {};

    Step clipboard;
    bool hasClipboard = false;
};
