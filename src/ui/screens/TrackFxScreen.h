#pragma once

#include "ParamPageScreen.h"
#include "../../dsp/effects/FilterEffect.h"
#include "../../project/ProjectBridge.h"

// -----------------------------------------------------------------------------
// TRK FX: the five pages every track has, whatever its engine, then two pages for each send bus.
//
//   TRACK   VOL  PAN  SND1 SND2 / BUS1 BUS2 LEN  SPD     mixer and sequencer settings
//           (BUS1 / BUS2: the type of the first effect on that send, a view of its chain)
//   ARPLK   GATE VEL  RTCH                               arpeggiator values a step can lock
//   FX      SLOT TYPE ON   WET  / the selected insert effect's four parameters
//           (an effect with a delay time or LFO rate shows SYNC instead of ON, and with SYNC on
//            its TIME / RATE cell chooses the tempo division)
//   FX2     SLOT TYPE           / the same effect's cells beyond its four
//   IN      SRC  LEVL MODE      / FATK FREL                the track's input: another track's sound
//           (TrackInParam; what it does is the AudioEngine's: readTrackInput / mixTrackInput)
//   S1FX, S1FX2, S2FX, S2FX2                             a send bus's chain: FX and FX2 again, for the
//           effects on send 1 and on send 2 (AudioEngine::sendTrack). The same cells in the same
//           places, laid out by the same function (fillSlotPage / fillSlotExtraPage). A send is
//           everybody's: these pages show the same thing whatever track is selected, their cells
//           take no step lock and no pressure range, and each send keeps its own SLOT. The first
//           eight pages have a page key each; a page beyond them is reached by paging.
//
// (DIRT and TALK are insert types, chosen with TYPE on FX; they had pages of their own here once.)
//
// Turning, locks, pressure ranges, paging and pad colours are ParamCellScreen's, the same as PARAM.
// -----------------------------------------------------------------------------
class TrackFxScreen : public ParamCellScreen
{
public:
    TrackFxScreen(SequencerCore& seq, AudioEngine* audio = nullptr) : ParamCellScreen(&seq, audio) {}

    juce::String getName() const override { return "TRK FX"; }

    int getFxSelectedSlot() const { return fxSelectedSlot; }

    static constexpr int NUM_PAGES = 5;  // TRACK, ARPLK, FX, FX2, IN

    // SRC is this screen's own cell (a virtual id, so no step lock and no pressure range: a source
    // that changed under a note would be a cut); its value is the track's TrackInParam::SRC.
    static constexpr int IN_SRC = 950;

    // The cells of a send's pages are this screen's own (a send belongs to no track: nothing of
    // it is kept in a track's sound, locked on a step or given a pressure range): SEND_CELL_BASE
    // + bus * SEND_CELL_STRIDE + one of SendCell. Their values are the chain's, through the insert
    // ids of AudioEngine::sendTrack(bus).
    static constexpr int SEND_CELL_BASE = 3200, SEND_CELL_STRIDE = 32;
    enum SendCell { SC_SLOT = 0, SC_TYPE, SC_ON, SC_WET, SC_SYNC, SC_DIV, SC_CELLS = 8, SC_EXTRAS = 12 };
    static int sendCellId(int bus, int cell) { return SEND_CELL_BASE + bus * SEND_CELL_STRIDE + cell; }
    static bool isSendCell(int id) { return id >= SEND_CELL_BASE && id < SEND_CELL_BASE + AudioEngine::MAX_SENDS * SEND_CELL_STRIDE; }
    static juce::String sendPageName(int bus, bool extras) { return "S" + juce::String(bus + 1) + (extras ? "FX2" : "FX"); }
    int getSendSelectedSlot(int bus) const { return sendSlotOf(bus); }

    // ---- Copy / clear / paste on a send's page: that send's chain ---------------------------------
    // On the track's pages the three are ScreenManager's (the selected track's effects), and with a
    // step held they are that step's locks on every page. On a send's page the track's effects are
    // not what is shown: the three act on the send's chain (copy: its effects and their values;
    // clear: a send with nothing on it; paste: the copied chain, on this send or another), each
    // clear and paste one undo step (the chain as it was).
    bool getEditScope(EditScope& scope) const override
    {
        if (ParamCellScreen::getEditScope(scope))
            return true;
        const int bus = shownSend();
        if (bus < 0)
            return false;
        const juce::String s = "SND" + juce::String(bus + 1);
        scope.names = { s + " FX", s };
        scope.canPaste = sendClipFilled;
        scope.step = -1;
        const UndoPoint point = sendUndoPoint(bus, 0);
        scope.capture = point.capture;
        scope.apply = point.apply;
        return true;
    }
    bool onCopy() override
    {
        const int bus = isStepHeld() ? -1 : shownSend();
        if (bus < 0)
            return ParamCellScreen::onCopy();
        sendClip.clear();
        ProjectBridge::captureChain(*audioEngine, AudioEngine::sendTrack(bus), sendClip);
        sendClipFilled = true;
        return true;
    }
    bool onClear() override
    {
        const int bus = isStepHeld() ? -1 : shownSend();
        if (bus < 0)
            return ParamCellScreen::onClear();
        ProjectBridge::applySendChain(*audioEngine, bus, {});
        return true;
    }
    bool onPaste() override
    {
        const int bus = isStepHeld() ? -1 : shownSend();
        if (bus < 0)
            return ParamCellScreen::onPaste();
        if (sendClipFilled)
            ProjectBridge::applySendChain(*audioEngine, bus, sendClip);
        return true;
    }
    // A send's cells take no pressure range and are nobody's on "every track": the help cards do
    // not offer either on a send's page.
    void getHelpFacts(HelpFacts& facts) const override
    {
        ParamCellScreen::getHelpFacts(facts);
        if (shownSend() >= 0)
            facts.pressureRange = facts.allTracksTurn = false;
    }
    // The send whose page is shown (-1: a page of the track's).
    int shownSend() const
    {
        if (! ready() || audioEngine == nullptr)
            return -1;
        const juce::String name = getPageName();
        for (int bus = 0; bus < sendCount(); ++bus)
            if (name == sendPageName(bus, false) || name == sendPageName(bus, true))
                return bus;
        return -1;
    }

    // One page key per page, for the first eight.
    int getPageKeyCount() const override { return ready() ? juce::jmin(8, (int) pages.size()) : 0; }
    void onPageKey(int index) override { if (ready() && index >= 0 && index < (int) pages.size()) goToPage(index); }
    int getActivePageKey() const override { return currentPageIndex; }
    juce::String getPageKeyName(int index) const override
    {
        return (index >= 0 && index < getPageKeyCount()) ? pages[(size_t) index].name : juce::String();
    }

protected:
    void buildPages(ParamPages& out) const override
    {
        out.push_back(trackPage());
        out.push_back(arpLockPage());

        ParamPage fx;
        fx.name = "FX";
        fx.section = "FX";
        out.push_back(fx);     // cells are resolved in resolvePage()

        // The same slot's cells beyond its four (an effect that has none leaves the lower row empty).
        ParamPage more;
        more.name = "FX2";
        more.section = "FX";
        out.push_back(more);

        ParamPage in;
        in.name = "IN";
        in.section = "FX";
        out.push_back(in);     // SRC's list ends at the track count: resolvePage()

        // Each send's chain: its FX page and its FX2 page.
        for (int bus = 0; bus < sendCount(); ++bus)
            for (bool extras : { false, true })
            {
                ParamPage send;
                send.name = sendPageName(bus, extras);
                send.section = "SEND";
                out.push_back(send);
            }
    }

    void resolvePage(ParamPage& page) const override
    {
        if (page.name == "FX")
            fillFxPage(page);
        else if (page.name == "FX2")
            fillFxExtraPage(page);
        else if (page.name == "IN")
            fillInPage(page);
        else if (page.section == "SEND")
            for (int bus = 0; bus < sendCount(); ++bus)
            {
                if (sendPageName(bus, false) == juce::String(page.name))
                    fillSlotPage(page, AudioEngine::sendTrack(bus), sendSlotOf(bus), sendSlotIds(bus));
                else if (sendPageName(bus, true) == juce::String(page.name))
                    fillSlotExtraPage(page, AudioEngine::sendTrack(bus), sendSlotOf(bus), sendSlotIds(bus));
            }
    }

    // Every track has these pages. An insert effect's own cells only mean the same thing on a
    // track with the same effect in that slot; the slot selector is this screen's own state.
    bool trackSharesParameter(const ParamSpec& spec, int selected, int other) const override
    {
        if (spec.id == FX_SLOT || spec.id == IN_SRC)     // (one source for every track would include the source itself)
            return false;
        if (isSendCell(spec.id) || isSendTypeParam(spec.id))      // (a send is one thing, not one a track)
            return false;
        const bool slotCell = spec.id == FX_ON || spec.id == FX_WET
                           || (spec.id >= AudioEngine::EFFECT_PARAM_BASE && spec.id < AudioEngine::EFFECT_PARAM_BASE + AudioEngine::MAX_INSERTS_PER_TRACK * 4)
                           || AudioEngine::isInsertSwitchParam(spec.id) || AudioEngine::isInsertExtraParam(spec.id);
        if (!slotCell || audioEngine == nullptr)
            return true;
        EffectType a, b;
        return audioEngine->getInsertEffectType(selected, fxSelectedSlot, a)
            && audioEngine->getInsertEffectType(other, fxSelectedSlot, b) && a == b;
    }

    juce::String cellText(const ParamSpec& spec, float shown) const override
    {
        if (AudioEngine::isArpLockParam(spec.id)
            && (spec.id == AudioEngine::ARP_LOCK_BASE + 2 ? shown < 0.5f : shown < 0.0f))
            return "OFF";
        // A source that is not there (the track itself from a damaged file, a track count that
        // shrank) is no source, and the cell says so.
        if (spec.id == IN_SRC && shown >= 0.5f && ! sourceExists(sequencer->getSelectedTrack(), (int) std::lround(shown)))
            return "--";
        // BUS1 / BUS2 name the send's first effect: a send with nothing on it has none.
        if (isSendTypeParam(spec.id) && audioEngine != nullptr
            && insertCount(AudioEngine::sendTrack(spec.id - AudioEngine::SEND_TYPE_PARAM_BASE)) == 0)
            return "OFF";
        // A cell of an effect that has its own way of showing it (TALK's VOWL): the effect says.
        int cell = -1;
        if (const EffectBase* fx = effectOfCell(spec.id, cell))
            return fx->cellText(cell, shown);
        return {};
    }

    // The effect a cell of a slot's page belongs to (the track's FX / FX2, a send's), and which
    // of its eight cells it is (0..3 the four, 4..7 the extras); null: not such a cell.
    const EffectBase* effectOfCell(int id, int& cell) const
    {
        if (isSendCell(id))
        {
            const int bus = (id - SEND_CELL_BASE) / SEND_CELL_STRIDE, at = (id - SEND_CELL_BASE) % SEND_CELL_STRIDE;
            if (at < SC_CELLS || at >= SC_EXTRAS + EffectBase::MAX_EXTRAS)
                return nullptr;
            cell = at - SC_CELLS;
            return effectAt(AudioEngine::sendTrack(bus), sendSlotOf(bus));
        }
        const int cells = AudioEngine::EFFECT_PARAM_BASE + fxSelectedSlot * 4;
        const int extras = AudioEngine::INSERT_EXTRA_BASE + fxSelectedSlot * EffectBase::MAX_EXTRAS;
        if (id >= cells && id < cells + 4)
            cell = id - cells;
        else if (id >= extras && id < extras + EffectBase::MAX_EXTRAS)
            cell = 4 + id - extras;
        else
            return nullptr;
        return sequencer != nullptr ? effectOf(sequencer->getSelectedTrack()) : nullptr;
    }

    // The filter insert effect gets the picture too: its four cells are FREQ RESO TYPE MODE.
    bool filterPictureSettings(const ParamPage& page, FilterCurve::Settings& out) const override
    {
        if (page.name != "FX" || audioEngine == nullptr)
            return false;
        const int trackIdx = sequencer->getSelectedTrack();
        EffectType type;
        if (!audioEngine->getInsertEffectType(trackIdx, fxSelectedSlot, type) || type != EffectType::FilterEffectType)
            return false;
        const int base = AudioEngine::EFFECT_PARAM_BASE + fxSelectedSlot * 4;
        out = FilterCurve::single(FilterSlot::modelFromIndex((int) std::lround(shownValueOfId(base + FilterEffect::Type))),
                                  (int) std::lround(shownValueOfId(base + FilterEffect::ModeParam)),
                                  shownValueOfId(base + FilterEffect::Frequency),
                                  shownValueOfId(base + FilterEffect::Resonance));
        return true;
    }

    bool readVirtual(const ParamSpec& spec, int trackIdx, float& out) const override
    {
        auto& track = sequencer->getPattern().getTrack(trackIdx);
        switch (spec.id)
        {
            case FX_SLOT:      out = (float) fxSelectedSlot; return true;
            case IN_SRC:       out = storedSource(trackIdx); return true;
            case FX_TYPE:
            {
                EffectType t;
                out = (audioEngine && audioEngine->getInsertEffectType(trackIdx, fxSelectedSlot, t)) ? (float) fxIndexFromType(t) : 0.0f;
                return true;
            }
            case FX_ON:        { auto* fx = effectOf(trackIdx); out = (fx && fx->isEnabled()) ? 1.0f : 0.0f; return true; }
            case FX_WET:       { auto* fx = effectOf(trackIdx); out = fx ? fx->getMix() : 0.0f; return true; }
            case TRACK_LENGTH: out = (float) track.length; return true;
            case TRACK_SPEED:  out = (float) indexFromSpeed(track.speedMultiplier); return true;
            default: break;
        }

        if (isSendTypeParam(spec.id))
        {
            const int bus = spec.id - AudioEngine::SEND_TYPE_PARAM_BASE;
            out = (audioEngine && bus < audioEngine->getSendCount()) ? (float) fxIndexFromType(audioEngine->getSendBusType(bus))
                                                                    : spec.defaultVal;
            return true;
        }

        // A cell of a send's page: the chain's value, whatever track is selected.
        if (isSendCell(spec.id))
        {
            const int bus = (spec.id - SEND_CELL_BASE) / SEND_CELL_STRIDE, cell = (spec.id - SEND_CELL_BASE) % SEND_CELL_STRIDE;
            const int chain = AudioEngine::sendTrack(bus), slot = sendSlotOf(bus);
            EffectType t;
            if (cell == SC_SLOT)      out = (float) slot;
            else if (cell == SC_TYPE) out = (audioEngine && audioEngine->getInsertEffectType(chain, slot, t)) ? (float) fxIndexFromType(t) : 0.0f;
            else                      out = audioEngine ? audioEngine->getTrackParameter(chain, sendInsertId(cell, slot)) : spec.defaultVal;
            return true;
        }
        return false;
    }

    bool writeVirtual(const ParamSpec& spec, int trackIdx, float value) override
    {
        auto& track = sequencer->getPattern().getTrack(trackIdx);
        const int iv = (int) std::lround(value);

        switch (spec.id)
        {
            case FX_SLOT:
                fxSelectedSlot = juce::jlimit(0, juce::jmin(insertCount(trackIdx), AudioEngine::MAX_INSERTS_PER_TRACK - 1), iv);
                return true;
            case FX_TYPE:
            {
                if (!audioEngine) return true;
                // Another type replaces the effect and its values at one click: an undo step (the
                // clicks of one scroll through the list are one step).
                UndoPoint point;
                point.scope = UndoPoint::Scope::Sound;
                point.track = trackIdx;
                point.verb = "SET";
                const juce::String t = "T" + juce::String(trackIdx + 1), fx = "FX" + juce::String(fxSelectedSlot + 1);
                point.names = { t + " " + fx + " TYPE", fx + " TYPE", fx };
                point.coalesceKey = 1000 + trackIdx * AudioEngine::MAX_INSERTS_PER_TRACK + fxSelectedSlot;
                const UndoMark mark(*this, point);
                if (iv <= 0)
                {
                    if (fxSelectedSlot < insertCount(trackIdx))
                        audioEngine->removeInsertEffect(trackIdx, fxSelectedSlot);
                }
                else
                {
                    audioEngine->setInsertEffect(trackIdx, fxSelectedSlot, fxTypeFromIndex(iv));
                }
                // The slot's stored parameter values belonged to the previous effect.
                for (int i = 0; i < 4; ++i)
                {
                    track.soundParams.erase(AudioEngine::EFFECT_PARAM_BASE + fxSelectedSlot * 4 + i);
                    track.soundParams.erase(AudioEngine::INSERT_EXTRA_BASE + fxSelectedSlot * EffectBase::MAX_EXTRAS + i);
                }
                return true;
            }
            case FX_ON:
                if (auto* fx = effectOf(trackIdx)) fx->setEnabled(iv > 0);
                return true;
            case FX_WET:
                if (auto* fx = effectOf(trackIdx)) fx->setMix(value);
                return true;
            case TRACK_LENGTH:
                lengthTurn.turn(track, iv, UiClock::nowMs());
                return true;
            case TRACK_SPEED:
                track.speedMultiplier = speedFromIndex(iv);
                return true;
            case IN_SRC:
            {
                // The track itself is not offered: the turn goes on to the next one, or stays
                // where it was when there is none that way.
                const int count = trackCount(), from = (int) std::lround(storedSource(trackIdx));
                int to = juce::jlimit(0, count, iv);
                if (to == trackIdx + 1)
                    to += to >= from ? 1 : -1;
                if (to < 0 || to > count || to == trackIdx + 1)
                    to = from;
                track.soundParams[TrackInParam::SRC] = (float) to;
                if (audioEngine)
                    audioEngine->setTrackParameter(trackIdx, TrackInParam::SRC, (float) to);
                return true;
            }
            default: break;
        }

        // BUS1 / BUS2: the send's first effect is exchanged for one of another type (as TYPE on the
        // send's own page does for its slot 1); what else is on the send stays.
        if (isSendTypeParam(spec.id))
        {
            const int bus = spec.id - AudioEngine::SEND_TYPE_PARAM_BASE;
            EffectType now;
            const bool has = audioEngine && audioEngine->getInsertEffectType(AudioEngine::sendTrack(bus), 0, now);
            if (audioEngine && bus < audioEngine->getSendCount() && iv >= 1 && iv < NUM_FX_TYPES
                && ! (has && fxIndexFromType(now) == iv))
            {
                const UndoMark mark(*this, sendUndoPoint(bus, 0));
                audioEngine->setInsertEffect(AudioEngine::sendTrack(bus), 0, fxTypeFromIndex(iv));
            }
            return true;
        }

        if (isSendCell(spec.id))
        {
            const int bus = (spec.id - SEND_CELL_BASE) / SEND_CELL_STRIDE, cell = (spec.id - SEND_CELL_BASE) % SEND_CELL_STRIDE;
            const int chain = AudioEngine::sendTrack(bus), slot = sendSlotOf(bus);
            if (audioEngine == nullptr || bus >= audioEngine->getSendCount())
                return true;
            if (cell == SC_SLOT)
                sendSlot[(size_t) bus] = juce::jlimit(0, juce::jmin(insertCount(chain), AudioEngine::MAX_INSERTS_PER_TRACK - 1), iv);
            else if (cell == SC_TYPE)
            {
                // Another type replaces the effect and its values at one click: an undo step.
                const UndoMark mark(*this, sendUndoPoint(bus, slot));
                if (iv <= 0)
                {
                    if (slot < insertCount(chain))
                        audioEngine->removeInsertEffect(chain, slot);
                }
                else
                    audioEngine->setInsertEffect(chain, slot, fxTypeFromIndex(iv));
            }
            else
                audioEngine->setTrackParameter(chain, sendInsertId(cell, slot), value);
            return true;
        }
        return false;
    }

private:
    static bool isSendTypeParam(int id)
    {
        return id >= AudioEngine::SEND_TYPE_PARAM_BASE && id < AudioEngine::SEND_TYPE_PARAM_BASE + AudioEngine::MAX_SENDS;
    }

    // ---- The sends' pages -----------------------------------------------------------------------
    int sendCount() const { return audioEngine != nullptr ? juce::jlimit(0, (int) AudioEngine::MAX_SENDS, audioEngine->getSendCount()) : 0; }

    // The slot a send's pages show: one of those in use, or the first empty one.
    int sendSlotOf(int bus) const
    {
        if (bus < 0 || bus >= AudioEngine::MAX_SENDS)
            return 0;
        return juce::jlimit(0, juce::jmin(insertCount(AudioEngine::sendTrack(bus)), AudioEngine::MAX_INSERTS_PER_TRACK - 1), sendSlot[(size_t) bus]);
    }

    // The insert id (of the send's chain) behind a cell of a send's page.
    static int sendInsertId(int cell, int slot)
    {
        switch (cell)
        {
            case SC_ON:   return AudioEngine::INSERT_ON_BASE + slot;
            case SC_WET:  return AudioEngine::INSERT_WET_BASE + slot;
            case SC_SYNC: return AudioEngine::INSERT_SYNC_BASE + slot;
            case SC_DIV:  return AudioEngine::INSERT_DIV_BASE + slot;
            default:      break;
        }
        if (cell >= SC_EXTRAS)
            return AudioEngine::INSERT_EXTRA_BASE + slot * EffectBase::MAX_EXTRAS + juce::jlimit(0, EffectBase::MAX_EXTRAS - 1, cell - SC_EXTRAS);
        return AudioEngine::EFFECT_PARAM_BASE + slot * 4 + juce::jlimit(0, 3, cell - SC_CELLS);
    }

    // A change of what is on a send is undone as a whole: the bus's chain as it was stored
    // (ProjectBridge: the form a project keeps it in), put back the way a project puts it.
    UndoPoint sendUndoPoint(int bus, int slot) const
    {
        UndoPoint point;
        point.scope = UndoPoint::Scope::Custom;
        point.verb = "SET";
        const juce::String s = "SND" + juce::String(bus + 1), fx = "FX" + juce::String(slot + 1);
        point.names = { s + " " + fx + " TYPE", s + " " + fx, s };
        point.coalesceKey = 5000 + bus * AudioEngine::MAX_INSERTS_PER_TRACK + slot;
        auto* audio = audioEngine;
        point.capture = [audio, bus]
        {
            ParamMap stored;
            ProjectBridge::captureChain(*audio, AudioEngine::sendTrack(bus), stored);
            std::vector<float> flat;
            for (auto& [id, value] : stored)
            {
                flat.push_back((float) id);
                flat.push_back(value);
            }
            return flat;
        };
        point.apply = [audio, bus](const std::vector<float>& flat)
        {
            ParamMap stored;
            for (size_t i = 0; i + 1 < flat.size(); i += 2)
                stored[(int) std::lround(flat[i])] = flat[i + 1];
            if (bus < audio->getSendCount())
                ProjectBridge::applySendChain(*audio, bus, stored);
        };
        return point;
    }

    static ParamPage trackPage()
    {
        const char* const* fxTypes = fxTypeNames();
        static const char* const speeds[] = { "1/4", "1/2", "1x", "2x", "4x" };
        ParamPage p;
        p.name = "TRACK";
        p.section = "TRACK";
        p.params = {{
            { "VOL",  MixParam::VOLUME, 0, 1, MixParam::DEFAULT_VOLUME },
            { "PAN",  MixParam::PAN, -1, 1, 0 },
            { "SND1", MixParam::SEND_BASE, 0, 1, 0 },
            { "SND2", MixParam::SEND_BASE + 1, 0, 1, 0 },
            { "BUS1", AudioEngine::SEND_TYPE_PARAM_BASE, 1, (float) (NUM_FX_TYPES - 1), 7, ParamKind::Choice, "", fxTypes, NUM_FX_TYPES },
            { "BUS2", AudioEngine::SEND_TYPE_PARAM_BASE + 1, 1, (float) (NUM_FX_TYPES - 1), 6, ParamKind::Choice, "", fxTypes, NUM_FX_TYPES },
            { "LEN",  TRACK_LENGTH, 1, 64, 16, ParamKind::Int },
            { "SPD",  TRACK_SPEED, 0, 4, 2, ParamKind::Choice, "", speeds, 5 } }};
        return p;
    }

    // The arpeggiator values a step can lock (hold a step and turn; OFF = follow the arpeggiator's
    // own setting). How a lock combines with the variation macros is the LOCK cell on ARP > SETUP.
    static ParamPage arpLockPage()
    {
        ParamPage p;
        p.name = "ARPLK";
        p.section = "TRACK";
        const int b = AudioEngine::ARP_LOCK_BASE;
        p.params[0] = { "GATE", b + 0, -0.02f, 1, -0.02f, ParamKind::Linear };
        p.params[1] = { "VEL",  b + 1, -0.02f, 1, -0.02f, ParamKind::Linear };
        p.params[2] = { "RTCH", b + 2, 0, 4, 0, ParamKind::Int };
        return p;
    }

    // ---- FX page: slot, type, on, wet | the selected effect's four parameters -----------------

    // Index 0 is OFF; index n is effect type n - 1, in the order of the EffectType enum.
    static EffectType fxTypeFromIndex(int idx)
    {
        return static_cast<EffectType>(juce::jlimit(0, (int) EffectType::NumEffectTypes - 1, idx - 1));
    }

    static const char* const* fxTypeNames()
    {
        static const char* names[1 + (int) EffectType::NumEffectTypes] = {};
        if (names[0] == nullptr)
        {
            names[0] = "OFF";
            for (int i = 0; i < (int) EffectType::NumEffectTypes; ++i)
                names[i + 1] = AudioEngine::effectTypeName(static_cast<EffectType>(i));
        }
        return names;
    }

    static int fxIndexFromType(EffectType t)
    {
        for (int i = 1; i < NUM_FX_TYPES; ++i)
            if (fxTypeFromIndex(i) == t)
                return i;
        return 0;
    }

    // The cells of the effect page depend on what is in the slot and on its SYNC (RATE / TIME
    // show tempo divisions while it is on). Those can change without a knob of this screen being
    // turned (a project or preset with SYNC already on, undo, a step lock): the pages are built
    // again whenever this differs from when they were built.
    int pageStateKey() const override
    {
        if (sequencer == nullptr || audioEngine == nullptr)
            return 0;
        const int trackIdx = sequencer->getSelectedTrack();
        auto* fx = effectOf(trackIdx);
        int key = fxSelectedSlot + 16 * insertCount(trackIdx) + (trackCount() << 21);     // (SRC's list)
        if (fx != nullptr)
        {
            EffectType type {};
            if (audioEngine->getInsertEffectType(trackIdx, fxSelectedSlot, type))
                key += 256 * (1 + (int) type);
            if (fx->isSynced())
                key += 1 << 20;
        }
        // The sends' pages: how many sends, and for each what its shown slot holds (mixed into
        // the key without a sign: the sum may run over).
        unsigned mixed = (unsigned) key;
        for (int bus = 0; bus < sendCount(); ++bus)
        {
            const int chain = AudioEngine::sendTrack(bus), slot = sendSlotOf(bus);
            EffectType type {};
            int part = 1 + slot + 16 * insertCount(chain);
            if (audioEngine->getInsertEffectType(chain, slot, type))
                part += 256 * (1 + (int) type);
            if (auto* sendFx = effectAt(chain, slot); sendFx != nullptr && sendFx->isSynced())
                part += 1 << 16;
            mixed = mixed * 31u + (unsigned) part;
        }
        return (int) (mixed & 0x7fffffffu);
    }

    int insertCount(int trackIdx) const
    {
        auto* chain = audioEngine ? audioEngine->getInsertChain(trackIdx) : nullptr;
        return chain ? chain->getEffectCount() : 0;
    }

    EffectBase* effectAt(int chainTrack, int slot) const
    {
        auto* chain = audioEngine ? audioEngine->getInsertChain(chainTrack) : nullptr;
        return (chain && slot < chain->getEffectCount()) ? chain->getEffect(slot) : nullptr;
    }
    EffectBase* effectOf(int trackIdx) const { return effectAt(trackIdx, fxSelectedSlot); }

    // The ids of a slot's page: the track's own (the screen's SLOT / TYPE / ON / WET, the insert
    // ids for what a step can lock), or a send's (all the screen's own).
    struct SlotIds { int slot, type, on, wet, sync, division, cells, extras; };
    SlotIds trackSlotIds() const
    {
        return { FX_SLOT, FX_TYPE, FX_ON, FX_WET, AudioEngine::INSERT_SYNC_BASE + fxSelectedSlot, AudioEngine::INSERT_DIV_BASE + fxSelectedSlot,
                 AudioEngine::EFFECT_PARAM_BASE + fxSelectedSlot * 4, AudioEngine::INSERT_EXTRA_BASE + fxSelectedSlot * EffectBase::MAX_EXTRAS };
    }
    static SlotIds sendSlotIds(int bus)
    {
        return { sendCellId(bus, SC_SLOT), sendCellId(bus, SC_TYPE), sendCellId(bus, SC_ON), sendCellId(bus, SC_WET),
                 sendCellId(bus, SC_SYNC), sendCellId(bus, SC_DIV), sendCellId(bus, SC_CELLS), sendCellId(bus, SC_EXTRAS) };
    }

    void fillFxPage(ParamPage& page) const { fillSlotPage(page, sequencer->getSelectedTrack(), fxSelectedSlot, trackSlotIds()); }
    void fillFxExtraPage(ParamPage& page) const { fillSlotExtraPage(page, sequencer->getSelectedTrack(), fxSelectedSlot, trackSlotIds()); }

    // The page of one slot of a chain (a track's FX, a send's S1FX): SLOT TYPE ON WET / its four cells.
    void fillSlotPage(ParamPage& page, int chainTrack, int slot, const SlotIds& ids) const
    {
        const char* const* fxTypes = fxTypeNames();
        // Slots are numbered from 1; the first empty slot is always selectable so an effect can be added.
        int maxSlot = juce::jmin(insertCount(chainTrack), AudioEngine::MAX_INSERTS_PER_TRACK - 1);
        page.params[0] = { "SLOT", ids.slot, 0, (float) maxSlot, 0, ParamKind::Int };
        page.params[1] = { "TYPE", ids.type, 0, (float) (NUM_FX_TYPES - 1), 0, ParamKind::Choice, "", fxTypes, NUM_FX_TYPES };

        auto* fx = effectAt(chainTrack, slot);
        if (fx != nullptr)
        {
            // Cell 3: an effect with a delay time or an LFO rate gets SYNC (lock it to the tempo);
            // the others keep the ON switch. WET at 0 takes any effect out of the sound.
            const int syncIndex = fx->getSyncParamIndex();
            if (syncIndex >= 0)
                page.params[2] = { "SYNC", ids.sync, 0, 1, 0, ParamKind::Choice, "", ParamChoices::offOn, 2 };
            else
                page.params[2] = { "ON",  ids.on, 0, 1, 1, ParamKind::Choice, "", ParamChoices::offOn, 2 };
            page.params[3] = { "WET", ids.wet, 0, 1, 1 };
            for (int i = 0; i < 4; ++i)
            {
                ParamSpec spec = fx->getParamSpec(i);
                if (spec.isValid())
                    spec.id = ids.cells + i;
                // With SYNC on, the TIME / RATE cell chooses the division instead (both values are kept).
                if (i == syncIndex && fx->isSynced())
                    spec = { spec.label, ids.division, 0, (float) (FxSync::NUM_DIVISIONS - 1),
                             (float) FxSync::DEFAULT_DIVISION, ParamKind::Choice, "", FxSync::divisionLabels(), FxSync::NUM_DIVISIONS };
                page.params[(size_t) (4 + i)] = spec;
            }
        }
    }

    // FX2: the slot and its type again (so the page says what it belongs to), then the extra cells.
    void fillSlotExtraPage(ParamPage& page, int chainTrack, int slot, const SlotIds& ids) const
    {
        int maxSlot = juce::jmin(insertCount(chainTrack), AudioEngine::MAX_INSERTS_PER_TRACK - 1);
        page.params[0] = { "SLOT", ids.slot, 0, (float) maxSlot, 0, ParamKind::Int };
        page.params[1] = { "TYPE", ids.type, 0, (float) (NUM_FX_TYPES - 1), 0, ParamKind::Choice, "", fxTypeNames(), NUM_FX_TYPES };
        if (auto* fx = effectAt(chainTrack, slot))
            for (int i = 0; i < EffectBase::MAX_EXTRAS; ++i)
            {
                ParamSpec spec = fx->getExtraSpec(i);
                if (spec.isValid())
                    spec.id = ids.extras + i;
                page.params[(size_t) (4 + i)] = spec;
            }
    }

    // ---- IN page: the track's input ------------------------------------------------------------
    int trackCount() const
    {
        return sequencer != nullptr ? juce::jlimit(1, (int) AudioEngine::MAX_TRACKS, sequencer->getPattern().getTrackCount()) : 1;
    }

    float storedSource(int trackIdx) const
    {
        if (audioEngine != nullptr)
            return audioEngine->getTrackParameter(trackIdx, TrackInParam::SRC);
        auto& params = sequencer->getPattern().getTrack(trackIdx).soundParams;
        auto it = params.find(TrackInParam::SRC);
        return it != params.end() ? it->second : 0.0f;
    }

    bool sourceExists(int trackIdx, int source) const { return source >= 1 && source <= trackCount() && source != trackIdx + 1; }

    static const char* const* sourceNames()
    {
        static const char* const names[1 + AudioEngine::MAX_TRACKS] = {
            "OFF", "T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8", "T9", "T10", "T11", "T12", "T13", "T14", "T15", "T16",
            "T17", "T18", "T19", "T20", "T21", "T22", "T23", "T24", "T25", "T26", "T27", "T28", "T29", "T30", "T31", "T32" };
        return names;
    }

    void fillInPage(ParamPage& page) const
    {
        static const char* const modes[] = { "OFF", "MIX", "RING" };
        const int count = trackCount();
        // (a project of one track has nobody to listen to: one value, and the knob says so)
        page.params[0] = { "SRC",  IN_SRC, 0, count > 1 ? (float) count : 0.0f, 0, ParamKind::Choice, "", sourceNames(), 1 + AudioEngine::MAX_TRACKS };
        page.params[1] = { "LEVL", TrackInParam::LEVEL, 0, 1, AudioEngine::TRACK_IN_LEVEL };
        page.params[2] = { "MODE", TrackInParam::MODE, 0, 2, 0, ParamKind::Choice, "", modes, 3 };
        page.params[4] = { "FATK", TrackInParam::FOLLOW_ATK, 1.0f, 200.0f, AudioEngine::TRACK_IN_ATTACK_MS, ParamKind::Log, "ms" };
        page.params[5] = { "FREL", TrackInParam::FOLLOW_REL, 10.0f, 2000.0f, AudioEngine::TRACK_IN_RELEASE_MS, ParamKind::Log, "ms" };
    }

    static float speedFromIndex(int i)
    {
        static const float speeds[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
        return speeds[juce::jlimit(0, 4, i)];
    }

    static int indexFromSpeed(float s)
    {
        int best = 2;
        float bestDiff = 1e9f;
        for (int i = 0; i < 5; ++i)
        {
            float d = std::abs(speedFromIndex(i) - s);
            if (d < bestDiff) { bestDiff = d; best = i; }
        }
        return best;
    }

    int fxSelectedSlot = 0;
    std::array<int, AudioEngine::MAX_SENDS> sendSlot {};     // each send's shown slot
    ParamMap sendClip;                                       // a send's chain, copied on its page (ProjectBridge's stored form)
    bool sendClipFilled = false;
    LengthTurn lengthTurn;       // LEN made longer: the track loops through the new steps
};
