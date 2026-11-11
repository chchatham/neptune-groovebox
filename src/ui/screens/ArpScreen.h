#pragma once

#include "../TurnNotice.h"
#include "../GridScreen.h"
#include "../../arpeggiator/Arpeggiator.h"
#include "../../sequencer/PadRatchet.h"
#include "../../arpeggiator/RhythmCapture.h"
#include "../../dsp/DSPUtils.h"

// -----------------------------------------------------------------------------
// ARP: pages in the standard grid, one page key (Func + top-row pad) per page.
//   ARP    MODE RATE OCT  GATE / RPT  SWNG GLID LTCH
//   PLAY   STRM TIME DIR  VEL  / RTCH RDEC HUMN FREZ
//   VARY   SPCE DICE DENS SCAT / RCHN GVAR STRP VAR
//   MUTA   MUTA INTV MODE SPCE / EVOL ANC  >PAT ---
//   ACFG   DEST DPTH REC  LOCK / TAP  RHYT ---  ---
//   ROLL, ROL2   the pad ratchet (added by setPadRatchet)
// Every cell writes straight to the arpeggiator the audio engine runs.
//
// SPCE is the same value on two pages (VARY, MUTA: Spice is also the mutation
// amount).
//
// STRP  which macro the mod strip drives while this arpeggiator is on.
// VAR   the stored variation, 1-99; turning it is the dice re-roll.
// EVOL  rounds of mutation since the ancestor. Clockwise mutates now,
//       anticlockwise returns to the ancestor.
// ANC   what the ancestor is: ORIG (the held notes) or KEPT (a committed
//       mutation). Clockwise commits the current mutation, anticlockwise
//       goes back to the held notes.
// >PAT  writes what the arpeggiator plays into the track's pattern: four
//       clicks clockwise (it replaces steps).
// TAP   ON: every pad press is a tap of the rhythm (the pads light while it is
//       armed); OFF ends the capture and makes it the strum rhythm (RHYT
//       switches it on and off).
//
// While this screen is shown, pad pressure drives the arpeggiator's pressure
// destination (ACFG: DEST, DPTH).
// -----------------------------------------------------------------------------
class ArpScreen : public GridScreen
{
public:
    ArpScreen(Arpeggiator& fallbackArp)
        : arpeggiator(fallbackArp)
    {
        auto L = ParamKind::Linear; auto I = ParamKind::Int; auto C = ParamKind::Choice;
        static const char* const strumDirs[] = { "UP", "DOWN", "ALT" };
        static const char* const glideDivs[] = { "OFF", "1/32", "1/16", "1/8", "1/4", "1/2", "1/1" };
        static const char* const mutateModes[] = { "ANC", "CUM" };
        static const char* const ancestors[] = { "ORIG", "KEPT" };
        static const char* const pressureTargets[] = { "RATE", "OCT", "GATE" };
        static const char* const recordModes[] = { "NOTE", "CHORD" };
        static const char* const stripTargets[] = { "OFF", "SPCE", "DICE", "DENS", "SCAT", "RCHN", "GVAR", "GATE" };
        static const char* const interactions[] = { "ADD", "MACRO", "LOCK", "MULT" };
        const float modeMax = (float) ((int) Arpeggiator::Mode::NumModes - 1);
        const float rateMax = (float) ((int) Arpeggiator::Rate::NumRates - 1);

        // Humanize amount; 0 = strictly on the grid.
        auto humanizeCell = [this, L]
        {
            return cell({ "HUMN", 0, 0, 1, 0, L },
                        [this] { return arp().isHumanized() ? arp().getHumanizeAmount() : 0.0f; },
                        [this](float v)
                        {
                            arp().setHumanize(v > 0.005f);
                            if (v > 0.005f) arp().setHumanizeAmount(v);
                        });
        };
        // Spice: chance of inserted notes and the mutation amount. Shown on VARY and MUTA.
        auto spiceCell = [this, L]
        {
            return cell({ "SPCE", 0, 0, 1, 0, L },
                        [this] { return arp().getSpice(); },
                        [this](float v) { arp().setSpice(v); });
        };

        Page main;
        main.name = "ARP";
        main.cells = {{
            cell({ "MODE", 0, 0, modeMax, 0, I },
                 [this] { return (float) (int) arp().getMode(); },
                 [this](float v) { arp().setMode((Arpeggiator::Mode) (int) v); },
                 [this](float) { return Arpeggiator::getModeLabel(arp().getMode()); }),
            cell({ "RATE", 0, 0, rateMax, 0, I },
                 // The knob goes from slowest (1/1) to fastest (1/64T).
                 [this] { return (float) Arpeggiator::speedPosition(arp().getRate()); },
                 [this](float v) { arp().setRate(Arpeggiator::rateBySpeed((int) v)); },
                 [this](float) { return Arpeggiator::getRateName(arp().getRate()); }),
            cell({ "OCT", 0, 1, 8, 1, I },
                 [this] { return (float) arp().getOctaveRange(); },
                 [this](float v) { arp().setOctaveRange((int) v); }),
            cell({ "GATE", 0, 0, 1, 0.5f, L },
                 [this] { return arp().getGate(); },
                 [this](float v) { arp().setGate(v); }),
            cell({ "RPT", 0, 1, 8, 1, I },
                 [this] { return (float) arp().getNoteRepeats(); },
                 [this](float v) { arp().setNoteRepeats((int) v); }),
            cell({ "SWNG", 0, 0, 1, 0, L },
                 [this] { return arp().getSwing(); },
                 [this](float v) { arp().setSwing(v); }),
            // Snap glide: how long a rate change takes, in note divisions.
            cell({ "GLID", 0, 0, (float) (Arpeggiator::NUM_GLIDE_DIVISIONS - 1), 0, C, "", glideDivs,
                   Arpeggiator::NUM_GLIDE_DIVISIONS },
                 [this] { return (float) Arpeggiator::getGlideDivisionIndex(arp().getGlideTime()); },
                 [this](float v) { arp().setGlideTime(Arpeggiator::getGlideTimeForIndex((int) std::lround(v))); }),
            cell(onOff("LTCH"),
                 [this] { return arp().getLatch() ? 1.0f : 0.0f; },
                 [this](float v) { arp().setLatch(v > 0.5f); }) }};
        gridPages.push_back(main);

        Page play;
        play.name = "PLAY";
        play.cells = {{
            cell(onOff("STRM"),
                 [this] { return arp().isStrumEnabled() ? 1.0f : 0.0f; },
                 [this](float v) { arp().setStrumEnabled(v > 0.5f); }),
            cell({ "TIME", 0, 0, 100, 20, L, "ms" },
                 [this] { return arp().getStrumTime(); },
                 [this](float v) { arp().setStrumTime(v); }),
            cell({ "DIR", 0, 0, 2, 0, C, "", strumDirs, 3 },
                 [this] { return (float) arp().getStrumDirection(); },
                 [this](float v) { arp().setStrumDirection((int) v); }),
            cell({ "VEL", 0, 0, 1, 0, L },
                 [this] { return arp().getStrumVelCurve(); },
                 [this](float v) { arp().setStrumVelCurve(v); }),
            // Ratchet count 1 = no ratchet.
            cell({ "RTCH", 0, 1, 4, 1, I },
                 [this] { return arp().isRatchetEnabled() ? (float) arp().getRatchetCount() : 1.0f; },
                 [this](float v)
                 {
                     arp().setRatchetEnabled(v > 1.5f);
                     arp().setRatchetCount(juce::jmax(1, (int) v));
                 }),
            cell({ "RDEC", 0, 0, 1, 0, L },
                 [this] { return arp().getRatchetDecay(); },
                 [this](float v) { arp().setRatchetDecay(v); }),
            humanizeCell(),
            cell(onOff("FREZ"),
                 [this] { return arp().isFrozen() ? 1.0f : 0.0f; },
                 [this](float v) { arp().setFrozen(v > 0.5f); }) }};
        gridPages.push_back(play);

        Page vary;
        vary.name = "VARY";
        vary.cells = {{
            spiceCell(),
            cell({ "DICE", 0, 0, 1, 0, L },
                 [this] { return arp().getDice(); },
                 [this](float v) { arp().setDice(v); }),
            // Density: 50 = every step; lower drops steps, higher adds ghost notes.
            cell({ "DENS", 0, 0, 1, 0.5f, L },
                 [this] { return arp().getDensity(); },
                 [this](float v) { arp().setDensity(v); }),
            cell({ "SCAT", 0, 0, 1, 0, L },
                 [this] { return arp().getOctaveScatter(); },
                 [this](float v) { arp().setOctaveScatter(v); }),
            cell({ "RCHN", 0, 0, 1, 0, L },
                 [this] { return arp().getRatchetChance(); },
                 [this](float v) { arp().setRatchetChance(v); }),
            cell({ "GVAR", 0, 0, 1, 0, L },
                 [this] { return arp().getGateSpread(); },
                 [this](float v) { arp().setGateSpread(v); }),
            // Which macro the mod strip drives while this arpeggiator is on.
            cell({ "STRP", 0, 0, (float) ((int) Arpeggiator::StripTarget::NumTargets - 1), 0, C, "", stripTargets,
                   (int) Arpeggiator::StripTarget::NumTargets },
                 [this] { return (float) (int) arp().getStripTarget(); },
                 [this](float v) { arp().setStripTarget((Arpeggiator::StripTarget) (int) std::lround(v)); }),
            cell({ "VAR", 0, 1, (float) Arpeggiator::MAX_VARIATION, 1, I },
                 [this] { return (float) arp().getVariation(); },
                 [this](float v) { arp().setVariation((int) std::lround(v)); }) }};
        gridPages.push_back(vary);

        Page muta;
        muta.name = "MUTA";
        muta.cells = {{
            cell(onOff("MUTA"),
                 [this] { return arp().isMutateEnabled() ? 1.0f : 0.0f; },
                 [this](float v) { arp().setMutateEnabled(v > 0.5f); }),
            // Bars between mutations.
            cell({ "INTV", 0, 1, 16, 4, I },
                 [this] { return (float) arp().getMutate().getInterval(); },
                 [this](float v) { arp().getMutate().setInterval((int) std::lround(v)); }),
            cell({ "MODE", 0, 0, 1, 0, C, "", mutateModes, 2 },
                 [this] { return arp().getMutate().getMode() == MutateSystem::Mode::Cumulative ? 1.0f : 0.0f; },
                 [this](float v)
                 {
                     arp().getMutate().setMode(v > 0.5f ? MutateSystem::Mode::Cumulative
                                                              : MutateSystem::Mode::Ancestor);
                 }),
            spiceCell(),
            cell({ "EVOL", 0, 0, 99, 0, I },
                 [this] { return (float) juce::jmin(99, arp().getMutationGeneration()); },
                 [this](float v)
                 {
                     const int now = juce::jmin(99, arp().getMutationGeneration());
                     const int wanted = (int) std::lround(v);
                     if (wanted < now)
                     {
                         // Every round of mutation goes at one click: an undo step.
                         const UndoMark mark(*this, mutationUndoPoint());
                         arp().returnToAncestor();
                     }
                     else if (wanted > now)  arp().mutateNow();
                 }),
            cell({ "ANC", 0, 0, 1, 0, C, "", ancestors, 2 },
                 [this] { return arp().isMutationCommitted() ? 1.0f : 0.0f; },
                 [this](float v)
                 {
                     if (v > 0.5f)
                     {
                         // Nothing to keep while the held notes themselves are playing.
                         if (arp().getMutationGeneration() > 0)
                             arp().commitToAncestor();
                         else if (! arp().isMutationCommitted())
                             notice.say("NOTHING KEPT");
                     }
                     else if (arp().isMutationCommitted())
                     {
                         arp().restoreOriginal();
                     }
                     else
                     {
                         notice.say("NOTHING KEPT");
                     }
                 }),
            // Write what the arpeggiator is playing (the frozen loop if frozen) into the track's
            // pattern as steps: four clicks clockwise (see onEncoderChange).
            cell({ ">PAT", 0, 0, (float) TurnConfirm::STEPS, 0, L },
                 [this] { return confirm.knob == EXPORT_CELL ? (float) confirm.steps : 0.0f; },
                 [](float) {},
                 [this](float)
                 {
                     if (exportResult > 0) return juce::String("OK");
                     if (exportResult < 0) return juce::String("FAIL");
                     return confirm.knob == EXPORT_CELL && confirm.steps > 0 ? confirm.progress() : juce::String("TURN>");
                 }),
            Cell() }};
        gridPages.push_back(muta);

        Page setup;
        setup.name = "ACFG";
        setup.cells = {{
            cell({ "DEST", 0, 0, 2, 2, C, "", pressureTargets, 3 },
                 [this] { return (float) (int) arp().getPressureTarget(); },
                 [this](float v) { arp().setPressureTarget((Arpeggiator::PressureTarget) (int) std::lround(v)); }),
            cell({ "DPTH", 0, 0, 1, 0, L },
                 [this] { return arp().getPressureAmount(); },
                 [this](float v) { arp().setPressureAmount(v); }),
            cell({ "REC", 0, 0, 1, 0, C, "", recordModes, 2 },
                 [this] { return (float) (int) arp().getRecordMode(); },
                 [this](float v) { arp().setRecordMode((Arpeggiator::RecordMode) (int) std::lround(v)); }),
            // How a step's parameter lock and the variation macros combine.
            cell({ "LOCK", 0, 0, 3, 0, C, "", interactions, 4 },
                 [this] { return (float) (int) arp().getMacroInteraction(); },
                 [this](float v) { arp().setMacroInteraction((Arpeggiator::MacroInteraction) (int) std::lround(v)); }),
            cell(onOff("TAP"),
                 [this] { return rhythmCapture.isCapturing() ? 1.0f : 0.0f; },
                 [this](float v) { setRhythmCapturing(v > 0.5f); },
                 [this](float)
                 {
                     return rhythmCapture.isCapturing() ? "ON " + juce::String(rhythmCapture.getEventCount())
                                                        : juce::String("OFF");
                 }),
            cell(onOff("RHYT"),
                 [this] { return arp().isRhythmEnabled() ? 1.0f : 0.0f; },
                 [this](float v)
                 {
                     arp().setRhythmEnabled(v > 0.5f);
                     if (arp().getRhythmLength() < 2)
                         notice.say("NOTHING TAPPED");      // TAP records the rhythm this switches on
                 },
                 [this](float)
                 {
                     if (arp().getRhythmLength() < 2) return juce::String("NONE");
                     return juce::String(arp().isRhythmEnabled() ? "ON" : "OFF");
                 }),
            Cell(), Cell() }};
        gridPages.push_back(setup);
    }

    juce::String getName() const override { return gridPages[(size_t) gridPageIndex].name; }

    // On/off, plus what is active that is not on the current page.
    juce::String getSubPage() const override
    {
        if (notice.showing())
            return notice.shown();
        juce::String s = arp().isEnabled() ? "ON" : "OFF";
        if (arp().getLatch()) s += " LATCH";
        if (arp().isFrozen()) s += " FRZ";
        return s;      // the page position is drawn by the name bar
    }

    // >PAT is confirmed by four clicks; every other cell is a plain grid cell.
    void onEncoderChange(int encoderIndex, int value) override
    {
        const bool exportCell = encoderIndex == EXPORT_CELL && gridPages[(size_t) gridPageIndex].name == "MUTA";
        if (!exportCell)
        {
            confirm.cancel();
            exportResult = 0;
            GridScreen::onEncoderChange(encoderIndex, value);
            return;
        }
        exportResult = 0;
        if (confirm.step(EXPORT_CELL, value > 64 ? value - 128 : value, clock ? clock() : -1))
        {
            // It replaces the selected track's steps: an undo step.
            UndoPoint point;
            point.scope = UndoPoint::Scope::TrackSteps;
            point.verb = "WRITE";
            point.names = { "ARP > " + undoTrackName(), "ARP" };
            const UndoMark mark(*this, point);
            exportResult = (exportToPattern && exportToPattern()) ? 1 : -1;
        }
    }
    void setClock(std::function<juce::int64()> c) { clock = std::move(c); }

    TurnNotice notice;

    // Page keys: one per page.
    int getPageKeyCount() const override { return juce::jmin(8, (int) gridPages.size()); }
    void onPageKey(int index) override
    {
        if (index >= 0 && index < getPageKeyCount()) { confirm.cancel(); setPageIndex(index); }
    }
    int getActivePageKey() const override { return gridPageIndex < 8 ? gridPageIndex : -1; }
    juce::String getPageKeyName(int index) const override
    {
        return (index >= 0 && index < getPageKeyCount()) ? gridPages[(size_t) index].name : juce::String();
    }

    // Pads are dark, except while TAP is armed: then they are what is tapped.
    bool getPadColours(std::array<PadColour, 16>& out) const override
    {
        const PadColour c = rhythmCapture.isCapturing() ? PadColour { 127, 50, 0 } : PadColour {};
        out.fill(c);
        return true;
    }

    // Pads: taps of a rhythm capture while TAP is on.
    void onPadPress(int padIndex, int velocity) override
    {
        juce::ignoreUnused(padIndex);
        if (rhythmCapture.isCapturing())
            tapRhythm((float) velocity / 127.0f, juce::Time::getMillisecondCounterHiRes());
    }

    // Pad pressure goes to the arpeggiator's pressure target; the hardest-pressed pad counts.
    void onPadAftertouch(int padIndex, int pressure) override
    {
        if (padIndex < 0 || padIndex >= 16) return;
        padPressure[(size_t) padIndex] = juce::jlimit(0, 127, pressure);
        sendPressure();
    }

    void onPadRelease(int padIndex) override
    {
        if (padIndex < 0 || padIndex >= 16) return;
        padPressure[(size_t) padIndex] = 0;
        sendPressure();
    }

    void onDeactivate() override
    {
        padPressure.fill(0);
        sendPressure();
        confirm.cancel();
        exportResult = 0;
        if (rhythmCapture.isCapturing())
            setRhythmCapturing(false);
    }

    // Starts a rhythm capture, or ends it and hands the rhythm to the arp().
    void setRhythmCapturing(bool capture)
    {
        if (capture == rhythmCapture.isCapturing())
            return;

        if (capture)
        {
            rhythmCapture.startCapture(1000.0);     // the capture's time unit is the millisecond
            lastTapMs = -1.0;
            return;
        }

        rhythmCapture.stopCapture();
        if (arp().setRhythmTemplate(rhythmCapture))
            arp().setRhythmEnabled(true);
    }

    // One tap at a time in milliseconds (onPadPress passes the clock).
    void tapRhythm(float velocity, double timeMs)
    {
        if (!rhythmCapture.isCapturing())
            return;
        if (lastTapMs >= 0.0)
            rhythmCapture.advanceSamples(juce::jmax(0.0, timeMs - lastTapMs));
        lastTapMs = timeMs;
        rhythmCapture.tapEvent(velocity);
    }

    Arpeggiator& getArpeggiator() { return arp(); }

    // What the >PAT cell does (set by the app): returns false if there was nothing to write.
    std::function<bool()> exportToPattern;

    // Adds the ROLL page: hold-to-ratchet for the pads that play notes (drum pads, scale pads).
    //   ROLL MODE BASE MAX / DPTH GATE P>V DCAY
    // With ROLL on, a held pad retriggers; pressure moves the rate from BASE to MAX (note divisions
    // in QUANT mode, Hz in FREE mode). DPTH is how much pressure matters, P>V how much each hit's
    // level follows the pressure, DCAY fades a roll out.
    void setPadRatchet(PadRatchet* r)
    {
        ratchet = r;
        if (ratchet == nullptr) return;
        Page roll;
        roll.name = "ROLL";
        gridPages.push_back(roll);
        rollPageIndex = (int) gridPages.size() - 1;
        buildRollCells();

        // ROL2: the feel of a roll. SWNG and GLID act in QUANT mode. THRS is the pressure a held pad
        // needs before it repeats; LTCH keeps a roll going after release until the pad is pressed
        // again; ACNT accents every Nth hit; PROB thins repeats out; RPTS ends a roll after N hits.
        auto L = ParamKind::Linear; auto I = ParamKind::Int;
        Page feel;
        feel.name = "ROL2";
        feel.cells = {{
            cell({ "SWNG", 0, 0, 1, 0, L },
                 [this] { return ratchet->getSwing(); },
                 [this](float v) { ratchet->setSwing(v); }),
            cell({ "GLID", 0, 0, PadRatchet::kMaxGlideSeconds, 0, L, "s" },
                 [this] { return ratchet->getGlide(); },
                 [this](float v) { ratchet->setGlide(v); }),
            cell({ "THRS", 0, 0, 1, 0, L },
                 [this] { return ratchet->getThreshold(); },
                 [this](float v) { ratchet->setThreshold(v); }),
            cell(onOff("LTCH"),
                 [this] { return ratchet->isLatch() ? 1.0f : 0.0f; },
                 [this](float v) { ratchet->setLatch(v > 0.5f); }),
            cell({ "HUMN", 0, 0, 1, 0, L },
                 [this] { return ratchet->getHumanize(); },
                 [this](float v) { ratchet->setHumanize(v); }),
            cell({ "ACNT", 0, 1, (float) PadRatchet::kMaxAccent, 1, I },
                 [this] { return (float) juce::jmax(1, ratchet->getAccent()); },
                 [this](float v) { ratchet->setAccent((int) std::lround(v)); },
                 [this](float v) { return v < 1.5f ? juce::String("OFF") : juce::String((int) std::lround(v)); }),
            cell({ "PROB", 0, 0, 1, 1, L },
                 [this] { return ratchet->getProbability(); },
                 [this](float v) { ratchet->setProbability(v); }),
            cell({ "RPTS", 0, 0, 16, 0, I },
                 [this] { return (float) ratchet->getMaxRepeats(); },
                 [this](float v) { ratchet->setMaxRepeats((int) std::lround(v)); },
                 [this](float v) { return v < 0.5f ? juce::String("INF") : juce::String((int) std::lround(v)); }) }};
        gridPages.push_back(feel);
    }

    // One arpeggiator per track: the screen edits whichever this returns (the selected track's).
    // Without a provider it edits the one it was constructed with.
    void setArpProvider(std::function<Arpeggiator*()> p) { provider = std::move(p); }

    // What a cell shows as its value (the text the grid draws), for any page.
    juce::String getCellText(int pageIndex, int cellIndex) const
    {
        if (pageIndex < 0 || pageIndex >= (int) gridPages.size() || cellIndex < 0 || cellIndex >= 8)
            return {};
        auto& c = gridPages[(size_t) pageIndex].cells[(size_t) cellIndex];
        if (!c.used())
            return {};
        const float v = c.get();
        return c.text ? c.text(v) : c.spec.format(v);
    }

    juce::String getCellLabel(int pageIndex, int cellIndex) const
    {
        if (pageIndex < 0 || pageIndex >= (int) gridPages.size() || cellIndex < 0 || cellIndex >= 8)
            return {};
        auto& c = gridPages[(size_t) pageIndex].cells[(size_t) cellIndex];
        return c.used() ? juce::String(c.spec.label) : juce::String();
    }

private:
    // EVOL turned back drops the mutation: the arpeggiator's variation and its ancestor are kept
    // as numbers so that the undo history can put them back.
    UndoPoint mutationUndoPoint()
    {
        UndoPoint point;
        point.scope = UndoPoint::Scope::Custom;
        point.verb = "EVOL";
        point.names = { undoTrackName() + " TO 0", "TO 0" };
        Arpeggiator* a = &arp();
        point.capture = [a]
        {
            const auto state = a->captureMutation();
            std::vector<float> v { (float) state.generation, state.committed ? 1.0f : 0.0f };
            for (auto* list : { state.ancestor, state.current })
                for (int i = 0; i < MutateSystem::MAX_NOTE_SLOTS; ++i)
                {
                    v.push_back((float) list[i].pitchOffset);
                    v.push_back(list[i].velocityScale);
                    v.push_back(list[i].muted ? 1.0f : 0.0f);
                }
            return v;
        };
        point.apply = [a](const std::vector<float>& v)
        {
            if (v.size() < 2 + 2 * 3 * (size_t) MutateSystem::MAX_NOTE_SLOTS) return;
            MutateSystem::NoteState state;
            state.generation = (int) v[0];
            state.committed = v[1] > 0.5f;
            size_t at = 2;
            for (auto* list : { state.ancestor, state.current })
                for (int i = 0; i < MutateSystem::MAX_NOTE_SLOTS; ++i)
                {
                    list[i].pitchOffset = (int) v[at++];
                    list[i].velocityScale = v[at++];
                    list[i].muted = v[at++] > 0.5f;
                }
            a->restoreMutation(state);
        };
        return point;
    }

    void sendPressure()
    {
        int highest = 0;
        for (int p : padPressure)
            highest = juce::jmax(highest, p);
        arp().applyPressure((float) highest / 127.0f);
    }

    Arpeggiator& arpeggiator;
    PadRatchet* ratchet = nullptr;
    static constexpr int EXPORT_CELL = 6;       // >PAT on MUTA
    TurnConfirm confirm;
    std::function<juce::int64()> clock;
    int exportResult = 0;
    int rollPageIndex = -1;

    // BASE and MAX are note divisions or frequencies depending on MODE, so the cells are rebuilt
    // when it changes.
    void buildRollCells()
    {
        if (ratchet == nullptr || rollPageIndex < 0) return;
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto C = ParamKind::Choice;
        auto& cells = gridPages[(size_t) rollPageIndex].cells;
        const bool quant = ratchet->getMode() == PadRatchet::Mode::Quantized;
        const float lastDiv = (float) (PadRatchet::getNumDivisions() - 1);

        cells[0] = cell(onOff("ROLL"),
                        [this] { return ratchet->isEnabled() ? 1.0f : 0.0f; },
                        [this](float v) { ratchet->setEnabled(v > 0.5f); });
        cells[1] = cell({ "MODE", 0, 0, 1, 0, C, "", PadRatchet::getModeNames(), 2 },
                        [this] { return ratchet->getMode() == PadRatchet::Mode::Free ? 1.0f : 0.0f; },
                        [this](float v)
                        {
                            ratchet->setMode(v > 0.5f ? PadRatchet::Mode::Free : PadRatchet::Mode::Quantized);
                            buildRollCells();
                        });
        if (quant)
        {
            cells[2] = cell({ "BASE", 0, 0, lastDiv, 3, C, "", PadRatchet::getDivisionNames(), PadRatchet::getNumDivisions() },
                            [this] { return (float) (int) ratchet->getBaseDivision(); },
                            [this](float v) { ratchet->setBaseDivision((PadRatchet::Division) (int) v); });
            cells[3] = cell({ "MAX", 0, 0, lastDiv, 7, C, "", PadRatchet::getDivisionNames(), PadRatchet::getNumDivisions() },
                            [this] { return (float) (int) ratchet->getMaxDivision(); },
                            [this](float v) { ratchet->setMaxDivision((PadRatchet::Division) (int) v); });
        }
        else
        {
            cells[2] = cell({ "BASE", 0, PadRatchet::kMinRateHz, PadRatchet::kMaxRateHz, 2, G, "Hz" },
                            [this] { return ratchet->getBaseRateHz(); },
                            [this](float v) { ratchet->setBaseRateHz(v); });
            cells[3] = cell({ "MAX", 0, PadRatchet::kMinRateHz, PadRatchet::kMaxRateHz, 40, G, "Hz" },
                            [this] { return ratchet->getMaxRateHz(); },
                            [this](float v) { ratchet->setMaxRateHz(v); });
        }
        cells[4] = cell({ "DPTH", 0, 0, 1, 1, L },
                        [this] { return ratchet->getPressureDepth(); },
                        [this](float v) { ratchet->setPressureDepth(v); });
        cells[5] = cell({ "GATE", 0, PadRatchet::kMinGate, PadRatchet::kMaxGate, 0.5f, L },
                        [this] { return ratchet->getGate(); },
                        [this](float v) { ratchet->setGate(v); });
        cells[6] = cell({ "P>V", 0, 0, 1, 0.5f, L },
                        [this] { return ratchet->getPressureToVelocity(); },
                        [this](float v) { ratchet->setPressureToVelocity(v); });
        cells[7] = cell({ "DCAY", 0, 0, 1, 0, L },
                        [this] { return ratchet->getVelocityDecay(); },
                        [this](float v) { ratchet->setVelocityDecay(v); });
    }
    std::function<Arpeggiator*()> provider;

    Arpeggiator& arp() const
    {
        if (provider)
            if (auto* a = provider())
                return *a;
        return arpeggiator;
    }
    RhythmCapture rhythmCapture;
    double lastTapMs = -1.0;
    std::array<int, 16> padPressure {};
};
