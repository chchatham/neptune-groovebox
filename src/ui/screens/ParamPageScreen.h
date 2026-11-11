#pragma once

#include "../TurnNotice.h"
#include "../ScreenManager.h"
#include "../NoteUtils.h"
#include "../ParamGrid.h"
#include "../FilterCurve.h"
#include "../EnvelopeCurve.h"
#include "../UiClock.h"
#include "../../sequencer/SequencerCore.h"
#include "../../engine/AudioEngine.h"
#include "../../dsp/DSPUtils.h"
#include <algorithm>
#include <cmath>

// -----------------------------------------------------------------------------
// ParamCellScreen: everything the 8-cell parameter screens share. PARAM (the
// engine's own pages), TRK FX (the six pages every track has) and FLTR (the
// filter picture) derive from it and only say which pages they show and how
// their few screen-owned cells are read and written, so the three behave
// identically (docs/launchkey-mk4-control-map.md, "CONTROL GRAMMAR"):
//
//   turn                 edit the cell; Shift = fine, Func = coarse / snap (ParamSpec::turned)
//   Shift + turn on ATK, DEC or REL of an envelope (times in seconds)
//                        that stage's curve instead (StageCurveParam; -100 log .. +100 exp), shown
//                        in the cell, the name bar ("ATK CRV +40") and the envelope picture
//   hold a pad + turn    parameter lock on that step (same three step sizes)
//   bank buttons         encoder page -1 / +1, going round at the ends (also while a step is
//                        held: the lock pages)
//   Up / Down            which 16 steps the pads address
//   Func + bank          previous / next section
//   Func + top-row pad   page key: one per section, the same key again steps through its pages
//   Func + Down / Up     low / high pressure-range end of the knob turned last on this page, at
//                        the knob's current value; the same again with the value unchanged
//                        removes that end
//   bottom-row pad N     when cell N has a range: pressure moves the parameter (see PressureHold)
//   > + turn             remove that cell's lock from the held step
//   Track + turn         the same click on every track that has the parameter
//   copy / clear / paste act on the held step's locks (false without a held step)
//
// Values go through AudioEngine::setTrackParameter and Track::soundParams, so they are saved
// with the sound. The pad colours always say what a pad will do.
// -----------------------------------------------------------------------------
class ParamCellScreen : public Screen
{
public:
    // Ids handled by a screen rather than by AudioEngine::setTrackParameter.
    enum VirtualParam
    {
        FX_SLOT = 900, FX_TYPE, FX_ON, FX_WET,
        MOD_SOURCE, MOD_COUNT,          // which LFO the MOD / MOD2 pages show, how many LFOs run
        TRACK_LENGTH, TRACK_SPEED,
        MOD_ENV, MOD_ENV_COUNT          // which extra envelope MENV shows, how many run
    };

    static constexpr int NUM_FX_TYPES = 1 + (int) EffectType::NumEffectTypes;   // OFF + every effect type
    static constexpr juce::uint32 FILTER_PICTURE_MS = 1000;
    static constexpr int STEPS_PER_PAD_PAGE = 16;

    // Sub-page: the page name; with a step held its number ("S23 FILTR"); on a track longer than
    // 16 steps the first step the pads address ("FILTR 17+"). Just after a range end is set or
    // removed, that instead.
    juce::String getSubPage() const override
    {
        if (!ready())
            return {};
        if (rangeReadout.isNotEmpty() && UiClock::counter() - rangeReadoutMs < 1500)
            return rangeReadout;
        if (notice.showing())
            return notice.shown();
        if (isCurveReadoutActive())
            return curveReadout;

        // A held step comes first: when room is short the name bar keeps the leading words, and
        // the step is the one sign on the display that the cells now show (and lock) that step.
        juce::String s = getPageName();
        const int held = getHeldStepIndex();
        if (held >= 0)
            s = "S" + juce::String(held + 1) + " " + s;
        else if (selectedTrack().length > STEPS_PER_PAD_PAGE)
            s << " " << (getStepPage() * STEPS_PER_PAD_PAGE + 1) << "+";
        return s;
    }

    bool usesEncoderBar() const override { return false; }
    EncoderLayout getEncoderLayout() const override { return EncoderLayout::Grid; }
    int getPageIndex() const override { ensurePages(); return currentPageIndex; }
    int getPageCount() const override { ensurePages(); return (int) pages.size(); }

    void render(DisplayRenderer& display) override
    {
        if (!ready())
            return;

        const ParamPage page = resolvedPage();
        const int trackIdx = sequencer->getSelectedTrack();
        auto& track = selectedTrack();

        // Lock markers follow the held step, or the playing step when nothing is held.
        const Step* markStep = nullptr;
        const int heldStep = getHeldStepIndex();
        if (heldStep >= 0 && heldStep < track.length)
            markStep = &track.getStep(heldStep);
        else if (sequencer->isPlaying())
            markStep = &track.getStep(sequencer->getCurrentStep(trackIdx));

        for (int i = 0; i < 8; ++i)
        {
            const auto& spec = page.params[(size_t) i];
            if (!spec.isValid())
            {
                ParamGrid::drawEmptyCell(display, i);
                continue;
            }

            // Just after Shift + turn on an envelope stage: the cell shows that stage's curve.
            const int curveId = stageCurveIdFor(spec, trackIdx);
            if (i == curveReadoutCell && curveId >= 0 && isCurveReadoutActive())
            {
                const ParamSpec curve = StageCurveParam::spec(curveId);
                ParamGrid::drawCell(display, i, curve, displayValue(curve), curve.label);
                if (markStep != nullptr && markStep->paramLocks.count(curveId) > 0)
                    ParamGrid::drawCellMark(display, i);
                continue;
            }

            const float shown = displayValue(spec);
            ParamGrid::drawCell(display, i, spec, shown, labelFor(spec), cellText(spec, shown));

            if (canRange(spec))
            {
                auto lo = track.soundParams.find(RangeParam::LO_BASE + spec.id);
                auto hi = track.soundParams.find(RangeParam::HI_BASE + spec.id);
                ParamGrid::drawCellRange(display, i,
                                         lo != track.soundParams.end(), lo != track.soundParams.end() ? spec.toNorm(lo->second) : 0.0f,
                                         hi != track.soundParams.end(), hi != track.soundParams.end() ? spec.toNorm(hi->second) : 0.0f);
            }

            // (A stage whose curve is locked on the step carries the mark too.)
            if (markStep != nullptr && (markStep->paramLocks.find(spec.id) != markStep->paramLocks.end()
                                        || (curveId >= 0 && markStep->paramLocks.count(curveId) > 0)))
                ParamGrid::drawCellMark(display, i);
        }

        drawPageStrip(display);
        drawFilterPicture(display, page);
        drawEnvelopePicture(display, page);
    }

    // ---- Pads --------------------------------------------------------------------------------
    // A bottom-row pad whose cell has a pressure range plays that range. Every other pad holds its
    // step (of the 16 the pads address) for parameter locks. Holding never toggles the step's trig.

    void onPadPress(int padIndex, int velocity) override
    {
        juce::ignoreUnused(velocity);
        if (!ready() || padIndex < 0 || padIndex >= 16)
            return;

        if (isRangePad(padIndex))
        {
            beginPressure(padIndex - 8);
            return;
        }
        const int step = getStepPage() * STEPS_PER_PAD_PAGE + padIndex;
        if (step < selectedTrack().length)
            heldPadStep[padIndex] = step;
    }

    void onPadAftertouch(int padIndex, int pressure) override
    {
        if (padIndex >= 8 && padIndex < 16)
            applyPressure(padIndex - 8, (float) pressure / 127.0f);
    }

    void onPadRelease(int padIndex) override
    {
        if (padIndex < 0 || padIndex >= 16)
            return;
        if (padIndex >= 8 && pressureHolds[(size_t) (padIndex - 8)].active)
        {
            endPressure(padIndex - 8);
            return;
        }
        heldPadStep[padIndex] = -1;
    }

    void onDeactivate() override
    {
        for (auto& h : heldPadStep) h = -1;
        for (int i = 0; i < 8; ++i) endPressure(i);
        lastTurnedCell = -1;
        filterTurnCell = -1;
        envelopeTurnCell = -1;
        curveReadoutCell = -1;
    }

    // True when the bottom-row pad plays a pressure range instead of holding a step.
    bool isRangePad(int padIndex) const { return padIndex >= 8 && padIndex < 16 && cellHasRange(padIndex - 8); }

    static constexpr PadColour PAD_OFF        { 0, 0, 0 };          // beyond the track's length
    static constexpr PadColour PAD_EMPTY      { 5, 5, 8 };          // a step without a trig
    static constexpr PadColour PAD_TRIG       { 0, 60, 0 };         // a trig
    static constexpr PadColour PAD_LOCKED     { 127, 60, 0 };       // a trig with parameter locks
    static constexpr PadColour PAD_HELD       { 127, 127, 127 };    // the step being held
    static constexpr PadColour PAD_RANGE      { 90, 0, 127 };       // plays a pressure range
    static constexpr PadColour PAD_RANGE_DOWN { 127, 90, 127 };     // ... and is pressed

    bool getPadColours(std::array<PadColour, 16>& out) const override
    {
        if (!ready())
        {
            out.fill(PAD_OFF);
            return true;
        }
        auto& track = selectedTrack();
        const int held = getHeldStepIndex();
        for (int pad = 0; pad < 16; ++pad)
        {
            if (isRangePad(pad))
            {
                out[(size_t) pad] = pressureHolds[(size_t) (pad - 8)].active ? PAD_RANGE_DOWN : PAD_RANGE;
                continue;
            }
            const int step = getStepPage() * STEPS_PER_PAD_PAGE + pad;
            if (step >= track.length)       { out[(size_t) pad] = PAD_OFF; continue; }
            const Step& s = track.getStep(step);
            if (step == held)               out[(size_t) pad] = PAD_HELD;
            else if (!s.paramLocks.empty()) out[(size_t) pad] = PAD_LOCKED;
            else if (s.trig)                out[(size_t) pad] = PAD_TRIG;
            else                            out[(size_t) pad] = PAD_EMPTY;
        }
        return true;
    }

    // ---- Pressure ranges -------------------------------------------------------------------------

    // True when cell `cell` of the current page has at least one range end.
    bool cellHasRange(int cell) const
    {
        if (!ready() || cell < 0 || cell >= 8) return false;
        const ParamPage page = resolvedPage();
        const auto& spec = page.params[(size_t) cell];
        if (!canRange(spec)) return false;
        auto& params = selectedTrack().soundParams;
        return params.count(RangeParam::LO_BASE + spec.id) > 0 || params.count(RangeParam::HI_BASE + spec.id) > 0;
    }

    bool pageHasRanges() const
    {
        for (int i = 0; i < 8; ++i)
            if (cellHasRange(i)) return true;
        return false;
    }

    // Func + Down / Func + Up.
    void onFuncDown() override { toggleRangeEnd(false); }
    void onFuncUp() override   { toggleRangeEnd(true); }

    // The cell whose knob was turned last on the current page (-1: none yet).
    int getLastTurnedCell() const { return lastTurnedCell; }

    // ---- The held step -----------------------------------------------------------------------

    // Another screen (the step view) can name the step being edited instead of a held pad here.
    void setExternalHeldStep(int stepIndex) { externalHeldStep = stepIndex; }
    int getExternalHeldStep() const { return externalHeldStep; }

    int getHeldStepIndex() const
    {
        if (externalHeldStep >= 0)
            return externalHeldStep;
        for (int i = 0; i < 16; ++i)
            if (heldPadStep[i] >= 0)
                return heldPadStep[i];
        return -1;
    }

    bool isStepHeld() const override
    {
        if (!ready()) return false;
        const int held = getHeldStepIndex();
        return held >= 0 && held < selectedTrack().length;
    }

    // ---- Turning -----------------------------------------------------------------------------

    void onEncoderChange(int encoderIndex, int value) override
    {
        if (!ready() || encoderIndex < 0 || encoderIndex >= 8)
            return;

        const ParamPage page = resolvedPage();
        const auto& cell = page.params[(size_t) encoderIndex];
        if (!cell.isValid())
            return;

        const int clicks = dsp::encoderDelta(value);
        if (clicks == 0)
            return;

        noteTurn(page, encoderIndex);

        const int trackIdx = sequencer->getSelectedTrack();
        auto& track = selectedTrack();

        // Shift + an envelope stage's knob: that stage's curve, a parameter like any other from
        // here on (stored with the sound, locked on a held step).
        const int curveId = getCurveTurnId(encoderIndex);
        const ParamSpec spec = curveId >= 0 ? StageCurveParam::spec(curveId) : cell;
        const float next = curveId >= 0 ? spec.turned(displayValue(spec), clicks, TurnStep::Plain)
                                        : turned(spec, displayValue(spec), clicks);
        if (curveId >= 0)
            noteCurveTurn(encoderIndex, spec, next);

        // A cell with one value has nowhere to go: say why instead of doing nothing.
        if (curveId < 0 && spec.maxVal <= spec.minVal)
        {
            notice.say(oneValueReason(spec));
            return;
        }
        // The filter's MORP acts in one place, the SEM's MRPH mode: anywhere else it would move
        // on the display and change nothing in the sound.
        if (curveId < 0 && isIdleFilterMorph(page, spec))
        {
            notice.say("SEM MRPH ONLY");
            return;
        }

        if (isStepHeld() && canLock(spec.id))
        {
            // The value the lock returns to is kept with the sound, so removing the lock can put it back.
            if (track.soundParams.find(spec.id) == track.soundParams.end())
                track.soundParams[spec.id] = spec.clamp(baseValue(spec, trackIdx));

            auto& step = track.getStep(getHeldStepIndex());
            step.paramLocks[spec.id] = next;
            if (!step.trig)
            {
                step.trig = true;
                if (step.notes.empty())
                    step.notes.push_back({60, 1.0f});
            }
            return;   // heard when the step plays; the track's own value is untouched
        }

        setValue(spec, next, trackIdx);
    }

    // Why a cell has only one value. MODE cells take their list from the filter type, and OFF
    // has none; SLOT offers the slots in use and the first empty one.
    static juce::String oneValueReason(const ParamSpec& spec)
    {
        const juce::String label(spec.label);
        if (label == "MODB") return "FLT B IS OFF";
        if (label == "MODE") return "FILTER IS OFF";
        if (label == "SLOT") return "SLOT IS EMPTY";
        return "ONE VALUE";
    }

    // MORP on a filter page while no MODE on the engine's pages reads MRPH (the SEM's morphing
    // mode; the synth has two filters, MODE and MODB, and its MORP belongs to both).
    bool isIdleFilterMorph(const ParamPage& page, const ParamSpec& spec) const
    {
        if (juce::String(spec.label) != "MORP" || page.name != "FILTR")
            return false;
        ParamPages all;
        buildPages(all);
        for (auto& p : all)
            for (auto& cell : p.params)
                if (cell.isValid() && cell.kind == ParamKind::Choice && cell.choices != nullptr
                    && (juce::String(cell.label) == "MODE" || juce::String(cell.label) == "MODB")
                    && (p.name == "FILTR" || p.name == "FLT B"))
                {
                    const int index = juce::jlimit(0, juce::jmax(0, cell.numChoices - 1), (int) std::lround(displayValue(cell)));
                    if (juce::String(cell.choices[index]) == "MRPH")
                        return false;
                }
        return true;
    }

    TurnNotice notice;

    // Track + turn: the same click on the same parameter of every track that has it, each from its
    // own value. With a step held it is an ordinary lock on this track.
    void onEncoderChangeAllTracks(int encoderIndex, int value) override
    {
        if (!ready() || encoderIndex < 0 || encoderIndex >= 8)
            return;
        if (isStepHeld())
        {
            onEncoderChange(encoderIndex, value);
            return;
        }

        const ParamPage page = resolvedPage();
        const ParamSpec cell = page.params[(size_t) encoderIndex];
        const int clicks = dsp::encoderDelta(value);
        if (!cell.isValid() || clicks == 0)
            return;

        noteTurn(page, encoderIndex);

        // With Shift on an envelope stage: that stage's curve on every such track.
        const int curveId = getCurveTurnId(encoderIndex);
        const ParamSpec spec = curveId >= 0 ? StageCurveParam::spec(curveId) : cell;

        const int selected = sequencer->getSelectedTrack();
        const int count = juce::jmin(sequencer->getPattern().getTrackCount(), (int) AudioEngine::MAX_TRACKS);
        for (int t = 0; t < count; ++t)
        {
            if (t != selected && !trackSharesParameter(cell, selected, t))
                continue;
            const float current = spec.clamp(baseValue(spec, t));
            const float next = curveId >= 0 ? spec.turned(current, clicks, TurnStep::Plain) : turned(spec, current, clicks);
            setValue(spec, next, t);
            if (curveId >= 0 && t == selected)
                noteCurveTurn(encoderIndex, spec, next);
        }
    }

    // The stage-curve parameter a turn of this knob edits right now: Shift held (and not Func) on
    // the ATK, DEC or REL cell of an envelope that has curves. -1 = an ordinary turn.
    int getCurveTurnId(int encoderIndex) const
    {
        if (!ready() || encoderIndex < 0 || encoderIndex >= 8 || !shiftDown() || funcDown())
            return -1;
        const ParamPage page = resolvedPage();
        return stageCurveIdFor(page.params[(size_t) encoderIndex], sequencer->getSelectedTrack());
    }

    // The stage-curve parameter behind a cell of track `trackIdx` (EngineBase::getStageCurveParam);
    // -1 when the cell is not an envelope stage in seconds.
    int stageCurveIdFor(const ParamSpec& spec, int trackIdx) const
    {
        if (!spec.isValid() || !EngineParamRange::contains(spec.id) || !spec.unitIs("s"))
            return -1;
        if (auto* e = engineOf(trackIdx))
            return e->getStageCurveParam(spec.id);
        return audioEngine == nullptr ? UnifiedVoiceEngine::stageCurveParamFor(spec.id) : -1;   // previews, tests
    }

    // For about a second after such a turn the cell and the name bar show the curve ("ATK CRV +40").
    bool isCurveReadoutActive() const
    {
        return curveReadoutCell >= 0 && UiClock::counter() - curveReadoutMs < FILTER_PICTURE_MS;
    }
    juce::String getCurveReadout() const { return isCurveReadoutActive() ? curveReadout : juce::String(); }

    // > + turn with a step held: that cell's lock is removed and the sound goes back to its own value.
    void onEncoderLockClear(int encoderIndex) override
    {
        if (!ready() || encoderIndex < 0 || encoderIndex >= 8 || !isStepHeld())
            return;
        const ParamPage page = resolvedPage();
        const auto& spec = page.params[(size_t) encoderIndex];
        if (!spec.isValid())
            return;
        // With Shift on an envelope stage it is the lock of that stage's curve.
        const int curveId = getCurveTurnId(encoderIndex);
        const int id = curveId >= 0 ? curveId : spec.id;
        auto& track = selectedTrack();
        if (track.getStep(getHeldStepIndex()).paramLocks.erase(id) > 0)
            restoreBase(id);
    }

    juce::String getEncoderName(int encoderIndex) const override
    {
        if (!ready() || encoderIndex < 0 || encoderIndex >= 8)
            return {};
        const ParamPage page = resolvedPage();
        const auto& spec = page.params[(size_t) encoderIndex];
        return spec.isValid() ? labelFor(spec) : juce::String("---");
    }

    // "CUT 1.2k": a cell's label and value (a held step's lock when it has one). The grid screens
    // show the value in the cell itself, so their getEncoderReadout stays empty and the name bar
    // keeps the page name; bar screens built on this class (FLTR) return this from it.
    juce::String getCellReadout(int encoderIndex) const
    {
        if (!ready() || encoderIndex < 0 || encoderIndex >= 8)
            return {};
        const ParamPage page = resolvedPage();
        const auto& spec = page.params[(size_t) encoderIndex];
        if (!spec.isValid())
            return {};
        if (encoderIndex == curveReadoutCell && isCurveReadoutActive())
            return curveReadout;
        const float shown = displayValue(spec);
        const juce::String text = cellText(spec, shown);
        return labelFor(spec) + " " + (text.isNotEmpty() ? text : spec.format(shown));
    }

    // ---- Copy / clear / paste: the held step's locks ------------------------------------------
    // Without a held step they return false and the app applies its own default (the sound).
    // Taking a clear or paste back is ScreenManager's undo history, not this screen's.

    bool onCopy() override
    {
        if (!isStepHeld()) return false;
        clipboard() = heldStepRef().paramLocks;
        clipboardFilled() = true;
        return true;
    }

    bool onClear() override
    {
        if (!isStepHeld()) return false;
        setHeldLocks(heldStepRef(), {});
        return true;
    }

    bool onPaste() override
    {
        if (!isStepHeld()) return false;
        if (clipboardFilled())
            setHeldLocks(heldStepRef(), clipboard());
        return true;
    }

    // What the three act on right now, for ScreenManager (who acts) and the help card: with a step
    // held, that step's locks; else nothing of this screen's own.
    bool getEditScope(EditScope& scope) const override
    {
        if (!isStepHeld()) return false;
        const juce::String step = "S" + juce::String(getHeldStepIndex() + 1);
        scope.names = { step + " LOCKS", step + " LCKS", step };
        scope.canPaste = clipboardFilled();
        scope.step = getHeldStepIndex();
        return true;
    }

    // Help cards: Shift + turn bends a stage where this page has an envelope stage in seconds;
    // Func + Up / Down set a pressure-range end; Func + bank jumps a section; a tap of Up / Down
    // moves the step page; Up / Down held + turn reaches every track (with a step held it is a
    // lock on this one); a step can be held here.
    void getHelpFacts(HelpFacts& facts) const override
    {
        facts.pressureRange = true;
        facts.bankJump = BankJump::Section;
        facts.tapUpDown = "STEP PAGE";
        facts.stepLocks = true;
        facts.allTracksTurn = !isStepHeld();
        if (!ready()) return;
        const ParamPage page = resolvedPage();
        for (auto& spec : page.params)
            if (stageCurveIdFor(spec, sequencer->getSelectedTrack()) >= 0)
                facts.stageCurves = true;
    }

    // ---- Paging ------------------------------------------------------------------------------

    // Bank buttons: one encoder page back / forward, going round at the ends. Nothing else.
    void onArrowLeft() override
    {
        if (!ready()) return;
        const int n = (int) pages.size();
        goToPage((currentPageIndex + n - 1) % n);
    }

    void onArrowRight() override
    {
        if (!ready()) return;
        goToPage((currentPageIndex + 1) % (int) pages.size());
    }

    // Func + bank: first page of the previous / next section.
    void onSectionPrev() override
    {
        if (!ready()) return;
        const int first = firstPageOfSection(currentPageIndex);
        goToPage((first == currentPageIndex && first > 0) ? firstPageOfSection(first - 1) : first);
    }

    void onSectionNext() override
    {
        if (!ready()) return;
        for (int i = currentPageIndex + 1; i < (int) pages.size(); ++i)
        {
            if (pages[(size_t) i].section != pages[(size_t) currentPageIndex].section)
            {
                goToPage(i);
                return;
            }
        }
    }

    // Up / Down: which 16 steps the pads address.
    void onPadPagePrev() override { setStepPage(getStepPage() - 1); }
    void onPadPageNext() override { setStepPage(getStepPage() + 1); }

    int getStepPageCount() const
    {
        if (!ready()) return 1;
        return juce::jmax(1, (selectedTrack().length + STEPS_PER_PAD_PAGE - 1) / STEPS_PER_PAD_PAGE);
    }
    int getStepPage() const { return juce::jlimit(0, getStepPageCount() - 1, stepPage); }
    void setStepPage(int page) { stepPage = juce::jlimit(0, getStepPageCount() - 1, page); }

    // Page keys: one per section, in the order the sections first appear (at most eight).
    int getPageKeyCount() const override
    {
        if (!ready()) return 0;
        return juce::jmin(8, sectionNames().size());
    }

    void onPageKey(int index) override
    {
        if (!ready()) return;
        const auto sections = sectionNames();
        if (index < 0 || index >= juce::jmin(8, sections.size()))
            return;
        const juce::String wanted = sections[index];
        const bool same = pages[(size_t) currentPageIndex].section == wanted;
        const int n = (int) pages.size();
        // The same key again: the section's next page, wrapping to its first.
        for (int k = same ? 1 : 0; k <= n; ++k)
        {
            const int i = same ? (currentPageIndex + k) % n : k;
            if (i < n && pages[(size_t) i].section == wanted)
            {
                goToPage(i);
                return;
            }
        }
    }

    int getActivePageKey() const override
    {
        if (!ready()) return -1;
        const int i = sectionNames().indexOf(pages[(size_t) currentPageIndex].section);
        return i < 8 ? i : -1;
    }

    juce::String getPageKeyName(int index) const override
    {
        if (!ready()) return {};
        const auto sections = sectionNames();
        return (index >= 0 && index < juce::jmin(8, sections.size())) ? sections[index] : juce::String();
    }

    int getCurrentPageIndex() const { ensurePages(); return currentPageIndex; }
    void setCurrentPageIndex(int index)
    {
        ensurePages();
        currentPageIndex = juce::jlimit(0, juce::jmax(0, (int) pages.size() - 1), index);
    }
    bool selectPageByName(const juce::String& name)
    {
        ensurePages();
        for (int i = 0; i < (int) pages.size(); ++i)
            if (pages[(size_t) i].name == name) { goToPage(i); return true; }
        return false;
    }
    juce::String getPageName() const
    {
        return ready() ? pages[(size_t) currentPageIndex].name : juce::String();
    }
    juce::String getPageSection() const
    {
        return ready() ? pages[(size_t) currentPageIndex].section : juce::String();
    }
    // The page as currently shown (dynamic cells resolved).
    ParamPage getCurrentPage() const { return ready() ? resolvedPage() : ParamPage{}; }

    // ---- The filter picture ------------------------------------------------------------------
    // While a cell of a filter page was turned within the last second, the response curve covers
    // the half of the grid that does not hold that knob.
    bool isFilterPictureActive() const
    {
        return filterTurnCell >= 0 && UiClock::counter() - filterTurnMs < FILTER_PICTURE_MS;
    }
    void expireFilterPicture() { filterTurnCell = -1; envelopeTurnCell = -1; curveReadoutCell = -1; }    // for tests: the second is over

    // ---- The envelope picture ----------------------------------------------------------------
    // The same for envelopes: while ATK, DEC, SUS, REL or CURV of a page that has an envelope was
    // turned within the last second, the envelope's shape covers the other half of the grid.
    // (On a page that is both, such as F ENV, the envelope knobs show the envelope and the other
    // knobs the filter.)
    bool isEnvelopePictureActive() const
    {
        return envelopeTurnCell >= 0 && UiClock::counter() - envelopeTurnMs < FILTER_PICTURE_MS;
    }
    // The envelope of the current page as the picture shows it; false = the page has none.
    bool getEnvelopeSettings(EnvelopeCurve::Settings& out) const
    {
        if (!ready()) return false;
        const auto binding = EnvelopeCurve::bind(resolvedPage());
        if (!binding.valid) return false;
        out = readEnvelope(binding);
        return true;
    }

    // The rows the picture covers: over the bottom row of cells when a top-row knob was turned,
    // and the other way round.
    static constexpr int PICTURE_TOP_Y = 11, PICTURE_TOP_H = 21;        // covers cells 1-4
    static constexpr int PICTURE_BOTTOM_Y = 33, PICTURE_BOTTOM_H = 23;  // covers cells 5-8

protected:
    ParamCellScreen(SequencerCore* seq, AudioEngine* audio) : sequencer(seq), audioEngine(audio) {}

    void rebind(SequencerCore* seq, AudioEngine* audio)
    {
        sequencer = seq;
        audioEngine = audio;
        pagesTrack = -2;
    }

    // ---- What a derived screen supplies ------------------------------------------------------

    // The pages for the selected track.
    virtual void buildPages(ParamPages& out) const = 0;
    // Fill cells that depend on the screen's state (the selected effect slot, LFO, ...).
    virtual void resolvePage(ParamPage& page) const { juce::ignoreUnused(page); }
    // Cells kept by the screen or outside the track's parameters. Return false for ordinary ones.
    virtual bool readVirtual(const ParamSpec& spec, int trackIdx, float& out) const { juce::ignoreUnused(spec, trackIdx, out); return false; }
    virtual bool writeVirtual(const ParamSpec& spec, int trackIdx, float value) { juce::ignoreUnused(spec, trackIdx, value); return false; }
    // Track + turn: does track `other` have this parameter of track `selected`?
    virtual bool trackSharesParameter(const ParamSpec& spec, int selected, int other) const = 0;
    // Anything else the page list depends on (e.g. the selected drum pad).
    virtual int pageStateKey() const { return 0; }
    virtual void trackOrEngineChanged() {}
    // Text shown instead of the formatted value (empty = the usual format).
    virtual juce::String cellText(const ParamSpec& spec, float shown) const { juce::ignoreUnused(spec, shown); return {}; }
    // The filter the page's picture shows; false = this page has none.
    virtual bool filterPictureSettings(const ParamPage& page, FilterCurve::Settings& out) const
    {
        if ((page.section != "FLTR" && page.section != "MAIN") || !filterBinding.valid)
            return false;
        out = FilterCurve::read(filterBinding, [this](int id) { return shownValueOfId(id); });
        return true;
    }

    // ---- Shared machinery ----------------------------------------------------------------------

    bool ready() const
    {
        if (sequencer == nullptr) return false;
        ensurePages();
        return !pages.empty();
    }

    EngineBase* engine() const { return engineOf(sequencer->getSelectedTrack()); }
    EngineBase* engineOf(int trackIdx) const { return audioEngine ? audioEngine->getEngine(trackIdx) : nullptr; }
    Track& selectedTrack() const { return sequencer->getPattern().getTrack(sequencer->getSelectedTrack()); }

    EngineType engineTypeOf(int trackIdx) const
    {
        if (auto* e = engineOf(trackIdx))
            return e->getType();
        return sequencer->getPattern().getTrack(trackIdx).engineType;
    }

    // Rebuild when the selected track, its engine or the derived screen's page state changed.
    void ensurePages() const
    {
        if (sequencer == nullptr) return;
        auto* self = const_cast<ParamCellScreen*>(this);
        const int trackIdx = sequencer->getSelectedTrack();
        auto* e = engine();
        const int key = pageStateKey();
        if (trackIdx != pagesTrack || e != pagesEngine)
        {
            const bool first = pagesTrack == -2;
            self->rebuildPages(true);
            if (!first)
            {
                for (int i = 0; i < 8; ++i) self->endPressure(i);
                self->trackOrEngineChanged();
            }
        }
        else if (key != pagesKey)
        {
            self->rebuildPages(true);
        }
    }

    // byName: stay on the page with the same name when it still exists (track changes); otherwise
    // keep the index (an edit that renamed a page).
    void rebuildPages(bool byName)
    {
        const juce::String previousName = (currentPageIndex < (int) pages.size()) ? pages[(size_t) currentPageIndex].name
                                                                                   : juce::String();
        pagesTrack = sequencer->getSelectedTrack();
        pagesEngine = engine();
        pagesKey = pageStateKey();

        pages.clear();
        buildPages(pages);

        ParamPages enginePages;
        if (pagesEngine != nullptr) enginePages = pagesEngine->getParamPages();
        else                        enginePages = UnifiedVoiceEngine::basePages();
        filterBinding = FilterCurve::bind(enginePages);

        if (byName)
        {
            int found = -1;
            for (int i = 0; i < (int) pages.size(); ++i)
                if (pages[(size_t) i].name == previousName)
                    found = i;
            if (found != currentPageIndex)
            {
                lastTurnedCell = -1;
                filterTurnCell = -1;
                envelopeTurnCell = -1;
                curveReadoutCell = -1;
            }
            currentPageIndex = juce::jmax(0, found);
        }
        currentPageIndex = juce::jlimit(0, juce::jmax(0, (int) pages.size() - 1), currentPageIndex);
    }

    ParamPage resolvedPage() const
    {
        ParamPage page = pages[(size_t) currentPageIndex];
        resolvePage(page);
        return page;
    }

    int firstPageOfSection(int index) const
    {
        index = juce::jlimit(0, juce::jmax(0, (int) pages.size() - 1), index);
        while (index > 0 && pages[(size_t) (index - 1)].section == pages[(size_t) index].section)
            --index;
        return index;
    }

    juce::StringArray sectionNames() const
    {
        juce::StringArray names;
        for (auto& p : pages)
            names.addIfNotAlreadyThere(p.section);
        return names;
    }

    // Pads playing a range and the "knob turned last" do not carry over to another page. A held
    // step does (the bank buttons page its locks).
    void goToPage(int index)
    {
        index = juce::jlimit(0, juce::jmax(0, (int) pages.size() - 1), index);
        if (index == currentPageIndex)
            return;
        for (int i = 0; i < 8; ++i) endPressure(i);
        lastTurnedCell = -1;
        filterTurnCell = -1;
        envelopeTurnCell = -1;
        curveReadoutCell = -1;
        currentPageIndex = index;
    }

    // One click rule for every cell (the control grammar's step sizes).
    float turned(const ParamSpec& spec, float current, int clicks) const
    {
        return spec.turned(current, clicks, turnStep());
    }

    void noteTurn(const ParamPage& page, int cell)
    {
        lastTurnedCell = cell;
        curveReadoutCell = -1;      // a Shift + turn on a stage sets it again (noteCurveTurn)
        // An envelope knob shows the envelope; any other knob of a filter page shows the filter.
        if (EnvelopeCurve::bind(page).isCell(page.params[(size_t) cell].id))
        {
            envelopeTurnCell = cell;
            envelopeTurnMs = UiClock::counter();
            filterTurnCell = -1;
            return;
        }
        envelopeTurnCell = -1;
        FilterCurve::Settings unused;
        if (filterPictureSettings(page, unused))
        {
            filterTurnCell = cell;
            filterTurnMs = UiClock::counter();
        }
    }

    // ---- Values ------------------------------------------------------------------------------

    static bool isLockable(int id)
    {
        if (id >= FX_SLOT) return false;
        if (id >= AudioEngine::SEND_TYPE_PARAM_BASE && id < EngineParamRange::EXT_BASE) return false;
        return true;
    }

    // Parameters that can have a pressure range: the same ones that can be locked.
    static bool isRangeable(const ParamSpec& spec) { return spec.isValid() && isLockable(spec.id); }

    // What this track's cell can actually take: the id rule above, minus the engine's rebuild
    // parameters (sample select, slicing, wavetable frames).
    bool canLock(int id) const
    {
        if (!isLockable(id)) return false;
        if (audioEngine != nullptr && sequencer != nullptr && id < AudioEngine::SEND_TYPE_PARAM_BASE)
            if (auto* e = audioEngine->getEngine(sequencer->getSelectedTrack()))
                return !e->isRebuildParam(id);
        return true;
    }
    bool canRange(const ParamSpec& spec) const { return spec.isValid() && canLock(spec.id); }

    // A track's own (unlocked) value.
    float baseValue(const ParamSpec& spec, int trackIdx) const
    {
        float v = 0.0f;
        if (readVirtual(spec, trackIdx, v))
            return v;

        // The engine / mixer is the truth when there is one; stored values otherwise.
        auto& params = sequencer->getPattern().getTrack(trackIdx).soundParams;
        auto it = params.find(spec.id);
        if (engineOf(trackIdx) != nullptr)
        {
            // While a lock is overriding the parameter, show the stored base value instead.
            if (it != params.end() && sequencer->isPlaying())
                return it->second;
            return audioEngine->getTrackParameter(trackIdx, spec.id);
        }
        return it != params.end() ? it->second : spec.defaultVal;
    }

    // What the cell shows: the held step's lock if it has one, else the track value.
    float displayValue(const ParamSpec& spec) const
    {
        if (isStepHeld())
        {
            auto& locks = selectedTrack().getStep(getHeldStepIndex()).paramLocks;
            auto it = locks.find(spec.id);
            if (it != locks.end())
                return spec.clamp(it->second);
        }
        return spec.clamp(baseValue(spec, sequencer->getSelectedTrack()));
    }

    // The same for a plain parameter id of the selected track (used by the filter picture, which
    // therefore shows a held step's locked filter).
    float shownValueOfId(int id) const
    {
        const int trackIdx = sequencer->getSelectedTrack();
        auto& track = selectedTrack();
        if (isStepHeld())
        {
            auto& locks = track.getStep(getHeldStepIndex()).paramLocks;
            auto it = locks.find(id);
            if (it != locks.end())
                return it->second;
        }
        if (engineOf(trackIdx) != nullptr)
            return audioEngine->getTrackParameter(trackIdx, id);
        auto it = track.soundParams.find(id);
        if (it != track.soundParams.end())
            return it->second;
        for (auto* spec : { &filterBinding.cut, &filterBinding.reso, &filterBinding.type, &filterBinding.mode, &filterBinding.morph,
                            &filterBinding.typeB, &filterBinding.modeB, &filterBinding.offsetB, &filterBinding.resoB,
                            &filterBinding.rout, &filterBinding.bal })
            if (spec->id == id)
                return spec->defaultVal;
        return 0.0f;
    }

    void setValue(const ParamSpec& spec, float value, int trackIdx)
    {
        if (writeVirtual(spec, trackIdx, value))
            return;

        sequencer->getPattern().getTrack(trackIdx).soundParams[spec.id] = value;
        if (audioEngine)
        {
            audioEngine->setTrackParameter(trackIdx, spec.id, value);

            // A switch such as the architecture preset or the filter type changes other parameters
            // and cells too: read them back so stored values match what is sounding.
            if (spec.kind == ParamKind::Choice && EngineParamRange::contains(spec.id))
                syncStoredParams(trackIdx);
        }
    }

    void syncStoredParams(int trackIdx)
    {
        auto* e = engineOf(trackIdx);
        if (e == nullptr) return;
        auto& track = sequencer->getPattern().getTrack(trackIdx);
        for (auto& page : e->getParamPages())
            for (auto& p : page.params)
                if (p.isValid())
                    track.soundParams[p.id] = e->getParameter(p.id);

        // Page names and cells can depend on the engine's state (the unified voice's third
        // oscillator, the modes a filter type has).
        if (trackIdx == sequencer->getSelectedTrack())
            rebuildPages(false);
    }

    // Put a parameter of the selected track back to its stored value (after a lock is removed).
    void restoreBase(int id)
    {
        auto& params = selectedTrack().soundParams;
        auto it = params.find(id);
        if (it != params.end() && audioEngine != nullptr)
            audioEngine->setTrackParameter(sequencer->getSelectedTrack(), id, it->second);
    }

    // ---- Range ends ----------------------------------------------------------------------------

    // Give the knob turned last on this page a low or high end at its current value; the same again
    // with the value unchanged removes it.
    void toggleRangeEnd(bool high)
    {
        if (!ready() || lastTurnedCell < 0)
            return;

        const ParamPage page = resolvedPage();
        const auto& spec = page.params[(size_t) lastTurnedCell];
        if (!canRange(spec))
            return;

        auto& params = selectedTrack().soundParams;
        const int id = (high ? RangeParam::HI_BASE : RangeParam::LO_BASE) + spec.id;
        const float value = spec.clamp(knobValue(spec));
        const juce::String what = labelFor(spec) + (high ? " HI " : " LO ");

        auto existing = params.find(id);
        if (existing != params.end()
            && std::abs(spec.toNorm(existing->second) - spec.toNorm(value)) < 1.0e-4f)
        {
            params.erase(existing);
            rangeReadout = what + "OFF";
        }
        else
        {
            params[id] = value;
            rangeReadout = what + spec.format(value);
        }
        rangeReadoutMs = UiClock::counter();

        // A pad that is playing this cell would otherwise keep ends that no longer exist.
        endPressure(lastTurnedCell);
    }

    // The knob's own value: not a held step's lock, and not where pressure is holding it right now.
    float knobValue(const ParamSpec& spec) const
    {
        for (auto& hold : pressureHolds)
            if (hold.active && hold.spec.id == spec.id && hold.track == sequencer->getSelectedTrack())
                return hold.base;
        return baseValue(spec, sequencer->getSelectedTrack());
    }

    // ---- Pads playing the ranges ---------------------------------------------------------------
    // Pressure moves the parameter away from the knob's value and release puts it back:
    //   one end set    from the knob's value (no pressure) to that end (full pressure)
    //   both ends set  from the low end (no pressure) to the high end (full pressure)
    // Interpolated in the parameter's own scale, so a frequency sweeps evenly by pitch.

    struct PressureHold
    {
        bool active = false;
        int track = 0;
        ParamSpec spec;
        float base = 0.0f;       // the knob's value, restored on release
        float from = 0.0f, to = 0.0f;
    };

    void beginPressure(int cell)
    {
        if (!cellHasRange(cell) || audioEngine == nullptr)
            return;
        const ParamPage page = resolvedPage();
        auto& hold = pressureHolds[(size_t) cell];
        hold.spec = page.params[(size_t) cell];
        hold.track = sequencer->getSelectedTrack();
        auto& params = selectedTrack().soundParams;

        // The knob's own value: what is stored, or what the engine has.
        auto stored = params.find(hold.spec.id);
        hold.base = stored != params.end() ? stored->second : audioEngine->getTrackParameter(hold.track, hold.spec.id);

        auto lo = params.find(RangeParam::LO_BASE + hold.spec.id);
        auto hi = params.find(RangeParam::HI_BASE + hold.spec.id);
        const bool hasLo = lo != params.end(), hasHi = hi != params.end();
        hold.from = (hasLo && hasHi) ? lo->second : hold.base;
        hold.to = hasHi ? hi->second : lo->second;
        hold.active = true;
        applyPressure(cell, 0.0f);
    }

    void applyPressure(int cell, float pressure)
    {
        auto& hold = pressureHolds[(size_t) cell];
        if (!hold.active || audioEngine == nullptr)
            return;
        const float a = hold.spec.toNorm(hold.from), b = hold.spec.toNorm(hold.to);
        audioEngine->modulateTrackParameter(hold.track, hold.spec.id,
                                            hold.spec.fromNorm(a + juce::jlimit(0.0f, 1.0f, pressure) * (b - a)));
    }

    void endPressure(int cell)
    {
        auto& hold = pressureHolds[(size_t) cell];
        if (!hold.active)
            return;
        hold.active = false;
        if (audioEngine != nullptr)
            audioEngine->setTrackParameter(hold.track, hold.spec.id, hold.base);
    }

    // ---- Held-step clipboard -------------------------------------------------------------------

    // One clipboard for all the cell screens: locks copied on PARAM can be pasted on TRK FX.
    static ParamMap& clipboard() { static ParamMap c; return c; }
    static bool& clipboardFilled() { static bool f = false; return f; }

    Step& heldStepRef() const { return selectedTrack().getStep(getHeldStepIndex()); }

    void setHeldLocks(Step& step, const ParamMap& locks)
    {
        const ParamMap old = step.paramLocks;
        auto& params = selectedTrack().soundParams;
        // As when a lock is made by turning: what it returns to is kept with the sound.
        for (auto& [id, value] : locks)
        {
            juce::ignoreUnused(value);
            if (params.find(id) == params.end() && audioEngine != nullptr && old.find(id) == old.end())
                params[id] = audioEngine->getTrackParameter(sequencer->getSelectedTrack(), id);
        }
        step.paramLocks = locks;
        for (auto& [id, value] : old)
        {
            juce::ignoreUnused(value);
            if (locks.find(id) == locks.end())
                restoreBase(id);
        }
        if (!locks.empty() && !step.trig)
        {
            step.trig = true;
            if (step.notes.empty())
                step.notes.push_back({60, 1.0f});
        }
    }

    // ---- Drawing -----------------------------------------------------------------------------

    juce::String labelFor(const ParamSpec& spec) const
    {
        // Send cells name what the bus does: "S1:RVB" is too long, so "RVB1".
        if (audioEngine && spec.id >= MixParam::SEND_BASE && spec.id < MixParam::SEND_BASE + MixParam::MAX_SENDS)
        {
            int bus = spec.id - MixParam::SEND_BASE;
            if (bus < audioEngine->getSendCount())
                return juce::String(AudioEngine::effectTypeName(audioEngine->getSendBusType(bus))) + juce::String(bus + 1);
        }
        return juce::String(spec.label).substring(0, 5);
    }

    void drawPageStrip(DisplayRenderer& display) const
    {
        ParamGrid::drawPageStrip(display, (int) pages.size(), currentPageIndex, [this](int i)
        {
            return pages[(size_t) i].section != pages[(size_t) (i - 1)].section;
        });
    }

    // The curve of the page's filter, 128 columns wide.
    const std::vector<float>* filterPictureHeights(const ParamPage& page, FilterCurve::Settings& settings) const
    {
        if (!filterPictureSettings(page, settings))
            return nullptr;
        return &filterCache.get(settings, DisplayRenderer::WIDTH);
    }

    void drawFilterPicture(DisplayRenderer& display, const ParamPage& page) const
    {
        if (!isFilterPictureActive())
            return;
        FilterCurve::Settings settings;
        auto* heights = filterPictureHeights(page, settings);
        if (heights == nullptr)
            return;
        const bool turnedTopRow = filterTurnCell < 4;
        FilterCurve::draw(display, 0, turnedTopRow ? PICTURE_BOTTOM_Y : PICTURE_TOP_Y, DisplayRenderer::WIDTH,
                          turnedTopRow ? PICTURE_BOTTOM_H : PICTURE_TOP_H, *heights, settings.cutoffHz[0]);
    }

    // The page's envelope as it sounds: the cells, and each stage's own curve (a held step's
    // locks included).
    EnvelopeCurve::Settings readEnvelope(const EnvelopeCurve::Binding& binding) const
    {
        const int trackIdx = sequencer->getSelectedTrack();
        return EnvelopeCurve::read(binding,
                                   [this](const ParamSpec& spec) { return displayValue(spec); },
                                   [this, trackIdx](const ParamSpec& spec)
                                   {
                                       const int curveId = stageCurveIdFor(spec, trackIdx);
                                       return curveId >= 0 ? displayValue(StageCurveParam::spec(curveId)) : 0.0f;
                                   });
    }

    // Shift + turn changed a stage's curve: say so in the cell and the name bar for a second.
    void noteCurveTurn(int cell, const ParamSpec& curve, float value)
    {
        curveReadoutCell = cell;
        curveReadoutMs = UiClock::counter();
        curveReadout = juce::String(StageCurveParam::stageName(curve.id)) + " " + curve.format(value);
    }

    void drawEnvelopePicture(DisplayRenderer& display, const ParamPage& page) const
    {
        if (!isEnvelopePictureActive())
            return;
        const auto binding = EnvelopeCurve::bind(page);
        if (!binding.valid)
            return;
        const auto settings = readEnvelope(binding);
        const bool turnedTopRow = envelopeTurnCell < 4;
        EnvelopeCurve::draw(display, 0, turnedTopRow ? PICTURE_BOTTOM_Y : PICTURE_TOP_Y, DisplayRenderer::WIDTH,
                            turnedTopRow ? PICTURE_BOTTOM_H : PICTURE_TOP_H, EnvelopeCurve::shape(settings));
    }

    SequencerCore* sequencer = nullptr;
    AudioEngine* audioEngine = nullptr;
    ParamPages pages;
    int currentPageIndex = 0;
    int pagesTrack = -2;                 // -2: never built
    EngineBase* pagesEngine = nullptr;
    int pagesKey = 0;

    int heldPadStep[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    int externalHeldStep = -1;
    int stepPage = 0;

    int lastTurnedCell = -1;             // on the current page
    juce::String rangeReadout;
    juce::uint32 rangeReadoutMs = 0;
    std::array<PressureHold, 8> pressureHolds;

    FilterCurve::Binding filterBinding;
    mutable FilterCurve::Cache filterCache;
    int filterTurnCell = -1;
    juce::uint32 filterTurnMs = 0;
    int envelopeTurnCell = -1;
    juce::uint32 envelopeTurnMs = 0;
    int curveReadoutCell = -1;           // the cell whose stage curve was just changed
    juce::uint32 curveReadoutMs = 0;
    juce::String curveReadout;
};

// -----------------------------------------------------------------------------
// PARAM: the pages of the selected track's engine, and nothing else.
//
// The pages come from EngineBase::getParamPages; engines with modulation add MOD
// and MOD2 (the selected LFO) and MENV (the extra envelopes). A drum track shows
// one "DR n" page - the pad selected on DRUMS (DrumEngine::getSelectedVoice) -
// followed by whatever the drum engine declares after its sixteen voice pages.
// What every track has (mixer, arp locks, effects) is on TRK FX.
// -----------------------------------------------------------------------------
class ParamPageScreen : public ParamCellScreen
{
public:
    ParamPageScreen(SequencerCore& seq, AudioEngine* audio = nullptr) : ParamCellScreen(&seq, audio) {}

    juce::String getName() const override { return "PARAM"; }

    int getModSelectedSource() const { return modSelectedSource; }   // LFO index, 0-3
    int getModSelectedEnv() const { return modSelectedEnv; }         // extra envelope index, 0-3

    // The note a step tapped in on STEP starts with: on a drum track the pad selected on DRUMS
    // (the one this screen's DR page shows), on any other track middle C.
    int newStepNote(int trackIdx) const
    {
        if (auto* drums = dynamic_cast<DrumEngine*>(engineOf(trackIdx)))
            return drums->getSelectedVoice();
        return 60;
    }

protected:
    void buildPages(ParamPages& out) const override
    {
        auto* e = engine();
        if (e == nullptr)
            out = UnifiedVoiceEngine::basePages();   // no audio engine attached (tests, previews)
        else if (auto* drums = dynamic_cast<DrumEngine*>(e))
        {
            // One page for the selected pad instead of sixteen; a kick machine (TYPE KSYN) adds
            // its own page. Then the kit's LFO pages.
            const auto all = drums->getParamPages();
            const int pad = shownDrumPad(drums);
            ParamPage voice = DrumEngine::voicePage(pad);
            voice.section = "DRUM";
            out.push_back(voice);
            if (drums->isKickMachine(pad))
            {
                ParamPage kick = DrumEngine::kickPage(pad);
                kick.section = "DRUM";
                out.push_back(kick);
            }
            for (size_t i = (size_t) (2 * DrumEngine::NUM_VOICES); i < all.size(); ++i)
                out.push_back(all[i]);
        }
        else
            out = e->getParamPages();

        prependMainPage(out);

        if (e == nullptr || e->supportsModulation())
        {
            // Cells are resolved in resolvePage(): two pages for the selected LFO, one for the
            // selected extra envelope.
            for (const char* name : { "MOD", "MOD2", "MENV" })
            {
                ParamPage mod;
                mod.name = name;
                mod.section = "MOD";
                out.push_back(mod);
            }
        }
    }

    // The landing page of every engine with a filter: the sound's first handles, always in the
    // same places. Knobs 1-2 are cutoff and resonance, 3 the filter envelope amount (the filter
    // type where there is none), 4 the filter type (its mode), and the bottom row is the
    // envelope that shapes the level: attack, decay, sustain, release. The cells are the engine's
    // own (they are also on its FILTR and AMP pages), so values, locks and ranges are shared.
    static void prependMainPage(ParamPages& pages)
    {
        const auto filter = FilterCurve::bind(pages);
        if (!filter.valid || !filter.reso.isValid())
            return;

        auto findIn = [&pages](const char* section, std::initializer_list<const char*> labels)
        {
            for (auto& page : pages)
                if (page.section == section)
                    for (const char* label : labels)
                        for (auto& p : page.params)
                            if (p.isValid() && std::strcmp(p.label, label) == 0)
                                return p;
            return ParamSpec {};
        };
        // The level envelope: the AMP section's, else the first envelope outside the filter pages
        // (FM: operator 1).
        EnvelopeCurve::Binding env;
        for (auto& page : pages)
            if (!env.valid && page.section == "AMP")
                env = EnvelopeCurve::bind(page);
        for (auto& page : pages)
            if (!env.valid && page.section != "FLTR")
                env = EnvelopeCurve::bind(page);

        const ParamSpec amount = findIn("FLTR", { "ENV", "ENVF" });
        ParamPage main;
        main.name = "MAIN";
        main.section = "MAIN";
        main.params[0] = filter.cut;
        main.params[1] = filter.reso;
        main.params[2] = amount.isValid() ? amount : filter.type;
        main.params[3] = amount.isValid() ? filter.type : filter.mode;
        if (env.valid)
        {
            main.params[4] = env.atk;
            main.params[5] = env.dec;
            main.params[6] = env.sus;
            main.params[7] = env.rel;
        }
        pages.insert(pages.begin(), main);
    }

    int pageStateKey() const override
    {
        if (auto* drums = dynamic_cast<DrumEngine*>(engine()))
            return shownDrumPad(drums) + (drums->isKickMachine(shownDrumPad(drums)) ? 100 : 0);
        return -1;
    }

    // The drum pad whose page is shown: the pad selected on DRUMS, or, while STEP holds a step
    // that has a note, the pad that step plays (so its locks are that drum's knobs).
    int shownDrumPad(const DrumEngine* drums) const
    {
        const int held = getExternalHeldStep();
        auto& track = sequencer->getPattern().getTrack(sequencer->getSelectedTrack());
        if (held >= 0 && held < track.length)
        {
            const Step& step = track.getStep(held);
            const int pad = step.notes.empty() ? -1 : DrumEngine::padForNote(step.notes[0].pitch);
            if (pad >= 0 && pad < DrumEngine::NUM_VOICES)
                return pad;
        }
        return drums->getSelectedVoice();
    }

    void trackOrEngineChanged() override
    {
        modSelectedSource = 0;
        modSelectedEnv = 0;
    }

    void resolvePage(ParamPage& page) const override
    {
        if (page.section != "MOD") return;
        if (page.name == "MOD")       fillLfoPage(page);
        else if (page.name == "MOD2") fillLfoStartPage(page);
        else if (page.name == "MENV") fillEnvPage(page);
    }

    // Engine parameters exist on every track with the same kind of engine. Which LFO / envelope
    // the MOD pages show is this screen's own state.
    bool trackSharesParameter(const ParamSpec& spec, int selected, int other) const override
    {
        if (spec.id == MOD_SOURCE || spec.id == MOD_ENV)
            return false;
        return engineTypeOf(other) == engineTypeOf(selected);
    }

    bool readVirtual(const ParamSpec& spec, int trackIdx, float& out) const override
    {
        switch (spec.id)
        {
            case MOD_SOURCE:    out = (float) modSelectedSource; return true;
            case MOD_ENV:       out = (float) modSelectedEnv; return true;
            case MOD_COUNT:     out = (float) getModLfoCount(trackIdx); return true;
            case MOD_ENV_COUNT: out = (float) getModEnvCount(trackIdx); return true;
            default:            return false;
        }
    }

    bool writeVirtual(const ParamSpec& spec, int trackIdx, float value) override
    {
        const int iv = (int) std::lround(value);
        switch (spec.id)
        {
            case MOD_SOURCE:
                modSelectedSource = juce::jlimit(0, ModParam::MAX_LFOS - 1, iv);
                return true;
            case MOD_ENV:
                modSelectedEnv = juce::jlimit(0, ModParam::MAX_ENVS - 1, iv);
                return true;
            case MOD_COUNT:
            case MOD_ENV_COUNT:
            {
                const int id = spec.id == MOD_COUNT ? ModParam::LFO_COUNT : ModParam::ENV_COUNT;
                sequencer->getPattern().getTrack(trackIdx).soundParams[id] = (float) iv;
                if (audioEngine) audioEngine->setTrackParameter(trackIdx, id, (float) iv);
                return true;
            }
            default:
                return false;
        }
    }

private:
    // ---- MOD pages ---------------------------------------------------------------------------
    //   MOD   LFO  LFOS RATE DPTH / SHPE TRIG SYNC DEST    the selected LFO: what it does
    //   MOD2  LFO  PHAS DLY  FADE / WHL  VEL  KTRK PRES    the selected LFO: how it starts on a note,
    //                                                      and what playing does to it (wheel,
    //                                                      velocity, pressure -> depth; key -> rate)
    //   MENV  ENV  ENVS ATK  DEC  / SUS  REL  DPTH DEST    the selected extra envelope
    // The first cell of each page chooses which LFO / envelope the other cells belong to.

    float trackParam(int trackIdx, int id, float fallback) const
    {
        if (auto* e = engineOf(trackIdx))
            return e->getParameter(id);
        auto& params = sequencer->getPattern().getTrack(trackIdx).soundParams;
        auto it = params.find(id);
        return it != params.end() ? it->second : fallback;
    }
    float trackParam(int id, float fallback) const { return trackParam(sequencer->getSelectedTrack(), id, fallback); }

    int getModLfoCount(int trackIdx) const
    {
        return juce::jlimit(1, ModParam::MAX_LFOS, (int) trackParam(trackIdx, ModParam::LFO_COUNT, 1.0f));
    }

    int getModEnvCount(int trackIdx) const
    {
        return juce::jlimit(0, ModParam::MAX_ENVS, (int) trackParam(trackIdx, ModParam::ENV_COUNT, 0.0f));
    }

    static ParamSpec lfoSelectCell()
    {
        static const char* const lfos[] = { "LFO1", "LFO2", "LFO3", "LFO4" };
        return { "LFO", MOD_SOURCE, 0, ModParam::MAX_LFOS - 1, 0, ParamKind::Choice, "", lfos, ModParam::MAX_LFOS };
    }

    void fillLfoPage(ParamPage& page) const
    {
        const int l = modSelectedSource;
        const int numShapes = (int) LFO::Shape::COUNT;
        const int numTrigs = (int) LFO::TriggerMode::COUNT;
        const int numDests = (int) UnifiedVoice::LfoDest::Count;
        const bool synced = trackParam(ModParam::lfoParamId(l, ModParam::LFO_SYNC), 0.0f) > 0.5f;

        page.params[0] = lfoSelectCell();
        page.params[1] = { "LFOS", MOD_COUNT, 1, ModParam::MAX_LFOS, 1, ParamKind::Int };
        // RATE is a tempo division while SYNC is on and Hz otherwise; each keeps its own value.
        if (synced)
            page.params[2] = { "RATE", ModParam::lfoExtParamId(l, ModParam::LFO_DIV), 0, (float) (LFO::NUM_DIVISIONS - 1),
                               (float) LFO::DEFAULT_DIVISION, ParamKind::Choice, "", LFO::divisionLabels(), LFO::NUM_DIVISIONS };
        else
            page.params[2] = { "RATE", ModParam::lfoParamId(l, ModParam::LFO_RATE), 0.02f, 40.0f, 2.0f, ParamKind::Log, "Hz" };
        page.params[3] = { "DPTH", ModParam::lfoParamId(l, ModParam::LFO_DEPTH), -1, 1, 0.3f };
        page.params[4] = { "SHPE", ModParam::lfoParamId(l, ModParam::LFO_SHAPE), 0, (float) (numShapes - 1), 0,
                           ParamKind::Choice, "", LFO::shapeLabels(), numShapes };
        page.params[5] = { "TRIG", ModParam::lfoParamId(l, ModParam::LFO_TRIG), 0, (float) (numTrigs - 1), 0,
                           ParamKind::Choice, "", LFO::triggerLabels(), numTrigs };
        page.params[6] = { "SYNC", ModParam::lfoParamId(l, ModParam::LFO_SYNC), 0, 1, 0, ParamKind::Choice, "", ParamChoices::offOn, 2 };
        page.params[7] = { "DEST", ModParam::lfoParamId(l, ModParam::LFO_DEST), 0, (float) (numDests - 1), 0,
                           ParamKind::Choice, "", UnifiedVoice::lfoDestLabels(), numDests };
    }

    void fillLfoStartPage(ParamPage& page) const
    {
        const int l = modSelectedSource;
        page.params[0] = lfoSelectCell();
        page.params[1] = { "PHAS", ModParam::lfoExtParamId(l, ModParam::LFO_PHASE), 0, 1, 0 };
        page.params[2] = { "DLY",  ModParam::lfoExtParamId(l, ModParam::LFO_DELAY), 0, 5, 0, ParamKind::Linear, "s", nullptr, 0, 0.01f };
        page.params[3] = { "FADE", ModParam::lfoExtParamId(l, ModParam::LFO_FADE), 0, 5, 0, ParamKind::Linear, "s", nullptr, 0, 0.01f };
        page.params[4] = { "WHL",  ModParam::lfoExt2ParamId(l, ModParam::LFO_WHEEL), -1, 1, 0 };
        page.params[5] = { "VEL",  ModParam::lfoExt2ParamId(l, ModParam::LFO_VEL), -1, 1, 0 };
        page.params[6] = { "KTRK", ModParam::lfoExt2ParamId(l, ModParam::LFO_KTRK), -1, 1, 0 };
        page.params[7] = { "PRES", ModParam::lfoExt2ParamId(l, ModParam::LFO_PRES), -1, 1, 0 };
    }

    // The extra envelopes are numbered from 2 (the amp envelope is number 1).
    void fillEnvPage(ParamPage& page) const
    {
        static const char* const envs[] = { "ENV2", "ENV3", "ENV4", "ENV5" };
        const int e = modSelectedEnv;

        page.params[0] = { "ENV",  MOD_ENV, 0, ModParam::MAX_ENVS - 1, 0, ParamKind::Choice, "", envs, ModParam::MAX_ENVS };
        page.params[1] = { "ENVS", MOD_ENV_COUNT, 0, ModParam::MAX_ENVS, 0, ParamKind::Int };
        page.params[2] = { "ATK",  ModParam::envParamId(e, ModParam::ENV_ATK), 0.001f, 10, 0.01f, ParamKind::Log, "s" };
        page.params[3] = { "DEC",  ModParam::envParamId(e, ModParam::ENV_DEC), 0.001f, 10, 0.1f, ParamKind::Log, "s" };
        page.params[4] = { "SUS",  ModParam::envParamId(e, ModParam::ENV_SUS), 0, 1, 0.7f };
        page.params[5] = { "REL",  ModParam::envParamId(e, ModParam::ENV_REL), 0.001f, 10, 0.3f, ParamKind::Log, "s" };
        page.params[6] = { "DPTH", ModParam::envParamId(e, ModParam::ENV_DEPTH), 0, 1, 1.0f };
        page.params[7] = { "DEST", ModParam::envParamId(e, ModParam::ENV_TARGET), 0, (float) (UnifiedVoice::NUM_ENV_DESTS - 1), 0, ParamKind::Choice, "", UnifiedVoice::envDestLabels(), UnifiedVoice::NUM_ENV_DESTS };
    }

    int modSelectedSource = 0;      // LFO shown on MOD / MOD2
    int modSelectedEnv = 0;         // extra envelope shown on MENV
};
