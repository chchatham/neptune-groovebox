#pragma once

#include "ParamPageScreen.h"
#include "../../dsp/effects/FilterEffect.h"

// -----------------------------------------------------------------------------
// TRK FX: the six pages every track has, whatever its engine.
//
//   TRACK   VOL  PAN  SND1 SND2 / BUS1 BUS2 LEN  SPD     mixer and sequencer settings
//   ARPLK   GATE VEL  RTCH                               arpeggiator values a step can lock
//   DIRT, TALK, MODFX                                    the three effect blocks (TrackFx.h)
//   FX      SLOT TYPE ON   WET  / the selected insert effect's four parameters
//           (an effect with a delay time or LFO rate shows SYNC instead of ON, and with SYNC on
//            its TIME / RATE cell chooses the tempo division)
//
// Turning, locks, pressure ranges, paging and pad colours are ParamCellScreen's, the same as PARAM.
// -----------------------------------------------------------------------------
class TrackFxScreen : public ParamCellScreen
{
public:
    TrackFxScreen(SequencerCore& seq, AudioEngine* audio = nullptr) : ParamCellScreen(&seq, audio) {}

    juce::String getName() const override { return "TRK FX"; }

    int getFxSelectedSlot() const { return fxSelectedSlot; }

    static constexpr int NUM_PAGES = 6;

    // One page key per page (six pages, eight keys).
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

        // The effect blocks every track has, then the insert slots.
        out.push_back(DirtBlock::page());
        out.push_back(TalkBlock::page());
        out.push_back(ModFxBlock::page());

        ParamPage fx;
        fx.name = "FX";
        fx.section = "FX";
        out.push_back(fx);     // cells are resolved in resolvePage()
    }

    void resolvePage(ParamPage& page) const override
    {
        if (page.name == "FX")
            fillFxPage(page);
    }

    // Every track has these pages. An insert effect's own cells only mean the same thing on a
    // track with the same effect in that slot; the slot selector is this screen's own state.
    bool trackSharesParameter(const ParamSpec& spec, int selected, int other) const override
    {
        if (spec.id == FX_SLOT)
            return false;
        const bool slotCell = spec.id == FX_ON || spec.id == FX_WET
                           || (spec.id >= AudioEngine::EFFECT_PARAM_BASE && spec.id < AudioEngine::EFFECT_PARAM_BASE + AudioEngine::MAX_INSERTS_PER_TRACK * 4)
                           || AudioEngine::isInsertSwitchParam(spec.id);
        if (!slotCell || audioEngine == nullptr)
            return true;
        EffectType a, b;
        return audioEngine->getInsertEffectType(selected, fxSelectedSlot, a)
            && audioEngine->getInsertEffectType(other, fxSelectedSlot, b) && a == b;
    }

    juce::String cellText(const ParamSpec& spec, float shown) const override
    {
        if (spec.id == TrackFxParam::TALK_BASE + TalkBlock::Vowel)     // "A 0" .. "I 50" .. "U 100"
            return juce::String(TalkBlock::vowelName(shown)) + " " + juce::String((int) std::lround(shown * 100.0f));
        if (AudioEngine::isArpLockParam(spec.id)
            && (spec.id == AudioEngine::ARP_LOCK_BASE + 2 ? shown < 0.5f : shown < 0.0f))
            return "OFF";
        return {};
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
                    track.soundParams.erase(AudioEngine::EFFECT_PARAM_BASE + fxSelectedSlot * 4 + i);
                return true;
            }
            case FX_ON:
                if (auto* fx = effectOf(trackIdx)) fx->setEnabled(iv > 0);
                return true;
            case FX_WET:
                if (auto* fx = effectOf(trackIdx)) fx->setMix(value);
                return true;
            case TRACK_LENGTH:
                track.setLength(iv);
                return true;
            case TRACK_SPEED:
                track.speedMultiplier = speedFromIndex(iv);
                return true;
            default: break;
        }

        if (isSendTypeParam(spec.id))
        {
            const int bus = spec.id - AudioEngine::SEND_TYPE_PARAM_BASE;
            track.soundParams[spec.id] = (float) iv;
            if (audioEngine && bus < audioEngine->getSendCount() && iv >= 1 && iv < NUM_FX_TYPES
                && fxIndexFromType(audioEngine->getSendBusType(bus)) != iv)
                audioEngine->setSendBusEffect(bus, fxTypeFromIndex(iv));
            return true;
        }
        return false;
    }

private:
    static bool isSendTypeParam(int id)
    {
        return id >= AudioEngine::SEND_TYPE_PARAM_BASE && id < AudioEngine::SEND_TYPE_PARAM_BASE + AudioEngine::MAX_SENDS;
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
        int key = fxSelectedSlot + 16 * insertCount(trackIdx);
        if (fx != nullptr)
        {
            EffectType type {};
            if (audioEngine->getInsertEffectType(trackIdx, fxSelectedSlot, type))
                key += 256 * (1 + (int) type);
            if (fx->isSynced())
                key += 1 << 20;
        }
        return key;
    }

    int insertCount(int trackIdx) const
    {
        auto* chain = audioEngine ? audioEngine->getInsertChain(trackIdx) : nullptr;
        return chain ? chain->getEffectCount() : 0;
    }

    EffectBase* effectOf(int trackIdx) const
    {
        auto* chain = audioEngine ? audioEngine->getInsertChain(trackIdx) : nullptr;
        return (chain && fxSelectedSlot < chain->getEffectCount()) ? chain->getEffect(fxSelectedSlot) : nullptr;
    }

    void fillFxPage(ParamPage& page) const
    {
        const int trackIdx = sequencer->getSelectedTrack();
        const char* const* fxTypes = fxTypeNames();
        // Slots are numbered from 1; the first empty slot is always selectable so an effect can be added.
        int maxSlot = juce::jmin(insertCount(trackIdx), AudioEngine::MAX_INSERTS_PER_TRACK - 1);
        page.params[0] = { "SLOT", FX_SLOT, 0, (float) maxSlot, 0, ParamKind::Int };
        page.params[1] = { "TYPE", FX_TYPE, 0, (float) (NUM_FX_TYPES - 1), 0, ParamKind::Choice, "", fxTypes, NUM_FX_TYPES };

        auto* fx = effectOf(trackIdx);
        if (fx != nullptr)
        {
            // Cell 3: an effect with a delay time or an LFO rate gets SYNC (lock it to the tempo);
            // the others keep the ON switch. WET at 0 takes any effect out of the sound.
            const int syncIndex = fx->getSyncParamIndex();
            if (syncIndex >= 0)
                page.params[2] = { "SYNC", AudioEngine::INSERT_SYNC_BASE + fxSelectedSlot, 0, 1, 0, ParamKind::Choice, "", ParamChoices::offOn, 2 };
            else
                page.params[2] = { "ON",  FX_ON, 0, 1, 1, ParamKind::Choice, "", ParamChoices::offOn, 2 };
            page.params[3] = { "WET", FX_WET, 0, 1, 1 };
            for (int i = 0; i < 4; ++i)
            {
                ParamSpec spec = fx->getParamSpec(i);
                if (spec.isValid())
                    spec.id = AudioEngine::EFFECT_PARAM_BASE + fxSelectedSlot * 4 + i;
                // With SYNC on, the TIME / RATE cell chooses the division instead (both values are kept).
                if (i == syncIndex && fx->isSynced())
                    spec = { spec.label, AudioEngine::INSERT_DIV_BASE + fxSelectedSlot, 0, (float) (FxSync::NUM_DIVISIONS - 1),
                             (float) FxSync::DEFAULT_DIVISION, ParamKind::Choice, "", FxSync::divisionLabels(), FxSync::NUM_DIVISIONS };
                page.params[(size_t) (4 + i)] = spec;
            }
        }
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
};
