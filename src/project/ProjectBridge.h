#pragma once

#include "Project.h"
#include "TrackPresetNames.h"
#include "../sequencer/SequencerCore.h"
#include "../engine/AudioEngine.h"
#include "../engine/UnifiedVoiceEngine.h"
#include "../arpeggiator/Arpeggiator.h"
#include "../controller/ChordPads.h"

// -----------------------------------------------------------------------------
// Moves the whole live state of the instrument into a ProjectData (for saving)
// and back (after loading or "new"): tempo, all 16 patterns with every step and
// lock, each track's engine and every parameter the pages show (engine, mixer,
// effect blocks, insert effects) with its pad-pressure slots (AT ROUTE), send
// buses, master level, song and custom scale pages.
//
// The sound of a track (engine, parameters, effects) is one per track, shared by
// all patterns; it is stored on the active pattern's tracks and written to every
// pattern on load.
// -----------------------------------------------------------------------------
namespace ProjectBridge
{
    // (Format 3 stores a cell that is a position in a list by the entry's name: StoredLists.h,
    // which Project.h brings. It reads each engine's tables itself, at first use: there is nothing
    // to register, and a file read before any project was captured or applied - the session, at
    // start - is read with every list in place.)

    // Insert-slot effect types are stored with the track's parameters: id = INSERT_TYPE_BASE + slot,
    // value = effect type + 1 (0 = empty). The ids sit between the insert parameters (200-231)
    // and the effect blocks (240+).
    static constexpr int INSERT_TYPE_BASE = 232;
    // SamplerEngine::SampleSelect and WavetableEngine::UserSample are both id 24.
    static constexpr int SAMPLE_INDEX_PARAM = 24;

    struct Live
    {
        SequencerCore& sequencer;
        AudioEngine& audio;
        SongMode* song = nullptr;
        CustomScaleManager* scales = nullptr;
        AftertouchRouter* pressure = nullptr;     // legacy single router (optional): only what an old project's one list is read into
        std::function<Arpeggiator*(int track)> arpForTrack;     // one arpeggiator per track
        ChordPads* chords = nullptr;              // the CHORD screen's pads
        std::function<KeyboardSetup()> getKeyboard;               // the keys' split and octave
        std::function<void(const KeyboardSetup&)> setKeyboard;
        TrackPresetNames* presetNames = nullptr;  // the preset each track's sound came from
    };

    // Arpeggiator settings as a flat list; the order is the file format. Nothing can be appended
    // (the tapped rhythm is the tail), so index 29 holds three switches: 1 = on, 2 = latch,
    // 4 = mutate running. Files written before latch and mutate were stored hold 0 or 1 there.
    inline std::vector<float> captureArp(const Arpeggiator& a)
    {
        std::vector<float> v {
            (float) (int) a.getMode(), (float) (int) a.getRate(), (float) a.getOctaveRange(), a.getGate(), a.getSwing(),
            (float) a.getNoteRepeats(), a.getGlideTime(),
            a.isStrumEnabled() ? 1.0f : 0.0f, a.getStrumTime(), (float) a.getStrumDirection(), a.getStrumVelCurve(),
            a.isRatchetEnabled() ? 1.0f : 0.0f, (float) a.getRatchetCount(), a.getRatchetDecay(),
            a.isHumanized() ? 1.0f : 0.0f, a.getHumanizeAmount(),
            a.getSpice(), a.getDice(), a.getDensity(), a.getOctaveScatter(), a.getRatchetChance(), a.getGateSpread(),
            (float) a.getVariation(),
            (float) (int) a.getPressureTarget(), a.getPressureAmount(),
            (float) (int) a.getRecordMode(), (float) (int) a.getMacroInteraction(),
            (float) a.getMutate().getInterval(), (float) (int) a.getMutate().getMode(),
            (float) ((a.isEnabled() ? 1 : 0) | (a.getLatch() ? 2 : 0) | (a.isMutateEnabled() ? 4 : 0)),
            (float) (int) a.getStripTarget() };
        // Index 31 on: the tapped rhythm.
        auto rhythm = a.getRhythmState();
        v.insert(v.end(), rhythm.begin(), rhythm.end());
        return v;
    }

    // A number from a file where a setting is expected: what is not a number (or infinite) counts as
    // missing, and nothing is so large that a whole-number cast of it is undefined. (jlimit hands a
    // NaN straight through: an arpeggiator with a NaN gate or velocity played NaN notes.)
    inline float storedValue(const std::vector<float>& v, size_t i, float fallback)
    {
        return i < v.size() && std::isfinite(v[i]) ? juce::jlimit(-1.0e9f, 1.0e9f, v[i]) : fallback;
    }

    inline void applyArp(const std::vector<float>& v, Arpeggiator& a)
    {
        auto at = [&](size_t i, float fallback) { return storedValue(v, i, fallback); };
        if (v.empty()) return;
        a.setMode((Arpeggiator::Mode) juce::jlimit(0, (int) Arpeggiator::Mode::NumModes - 1, (int) at(0, 0)));
        a.setRate((Arpeggiator::Rate) juce::jlimit(0, (int) Arpeggiator::Rate::NumRates - 1, (int) at(1, 4)));
        a.setOctaveRange((int) at(2, 1));
        a.setGate(at(3, 0.5f));
        a.setSwing(at(4, 0));
        a.setNoteRepeats((int) at(5, 1));
        a.setGlideTime(at(6, 0));
        a.setStrumEnabled(at(7, 0) > 0.5f);
        a.setStrumTime(at(8, 20));
        a.setStrumDirection((int) at(9, 0));
        a.setStrumVelCurve(at(10, 0));
        a.setRatchetEnabled(at(11, 0) > 0.5f);
        a.setRatchetCount((int) at(12, 1));
        a.setRatchetDecay(at(13, 0));
        a.setHumanize(at(14, 0) > 0.5f);
        a.setHumanizeAmount(at(15, 0.5f));
        a.setSpice(at(16, 0));
        a.setDice(at(17, 0));
        a.setDensity(at(18, 0.5f));
        a.setOctaveScatter(at(19, 0));
        a.setRatchetChance(at(20, 0));
        a.setGateSpread(at(21, 0));
        a.setVariation((int) at(22, 1));
        a.setPressureTarget((Arpeggiator::PressureTarget) juce::jlimit(0, 2, (int) at(23, 2)));
        a.setPressureAmount(at(24, 0));
        a.setRecordMode((Arpeggiator::RecordMode) juce::jlimit(0, 1, (int) at(25, 0)));
        a.setMacroInteraction((Arpeggiator::MacroInteraction) juce::jlimit(0, 3, (int) at(26, 0)));
        a.getMutate().setInterval((int) at(27, 4));
        a.getMutate().setMode((MutateSystem::Mode) juce::jlimit(0, 1, (int) at(28, 0)));
        const int switches = juce::jlimit(0, 7, (int) std::lround(at(29, 0)));
        a.setLatch((switches & 2) != 0);
        a.setMutateEnabled((switches & 4) != 0);
        a.setEnabled((switches & 1) != 0);
        a.setStripTarget((Arpeggiator::StripTarget) juce::jlimit(0, (int) Arpeggiator::StripTarget::NumTargets - 1, (int) at(30, 0)));
        if (v.size() > 32 && std::all_of(v.begin() + 31, v.end(), [](float x) { return std::isfinite(x); }))
            a.setRhythmState(v.data() + 31, (int) v.size() - 31);
        else
            a.clearRhythmTemplate();
    }

    inline TrackData::StepData captureStep(const Step& s)
    {
        TrackData::StepData d;
        d.trig = s.trig;
        d.notes = s.notes;
        d.velocity = s.velocity;
        d.length = s.length;
        d.microTiming = s.microTiming;
        d.trigConditionType = s.trigCondition.type;
        d.trigA = s.trigCondition.a;
        d.trigB = s.trigCondition.b;
        d.trigProbability = s.trigCondition.probability;
        d.paramLocks = s.paramLocks;
        d.pageChangeLock = s.pageChangeLock;
        d.pressureScale = s.pressureScale;      // (the lanes themselves are in the notes)
        return d;
    }

    inline void applyStep(const TrackData::StepData& d, Step& s)
    {
        // A file may hold anything: every number is brought inside what a step can hold here, where
        // it comes in (a velocity that was not a number played notes that were not; a length that
        // was not one never let its note go).
        auto within = [](float v, float lo, float hi, float fallback) { return std::isfinite(v) ? juce::jlimit(lo, hi, v) : fallback; };
        s.clear();
        s.trig = d.trig;
        s.notes = d.notes;
        for (auto& n : s.notes)
        {
            n.pitch = juce::jlimit(0, 127, n.pitch);
            n.velocity = within(n.velocity, 0.0f, 1.0f, 1.0f);
        }
        s.velocity = within(d.velocity, 0.0f, 1.0f, 1.0f);
        s.length = within(d.length, 0.0f, 1024.0f, 1.0f);
        s.microTiming = within(d.microTiming, -0.5f, 0.5f, 0.0f);
        const int type = (int) d.trigConditionType;
        s.trigCondition.type = type >= 0 && type <= (int) TrigCondition::NotPrevTrig ? d.trigConditionType : TrigCondition::Always;
        s.trigCondition.a = juce::jlimit(0, 1024, d.trigA);
        s.trigCondition.b = juce::jlimit(0, 1024, d.trigB);
        s.trigCondition.probability = within(d.trigProbability, 0.0f, 1.0f, 1.0f);
        s.paramLocks = d.paramLocks;
        s.pageChangeLock = d.pageChangeLock;
        s.pressureScale = within(d.pressureScale, 0.0f, 2.0f, 1.0f);
    }

    // ---- The DIRT / TALK / MODFX blocks of before (ids 240-263, TrackFxParam) ---------------------
    // Every track had the three blocks, in front of its insert slots. They are insert effects now
    // (DIRT, TALK; PHS, FLG, CHR, ROT for what MODFX held), so a stored sound that has a block
    // switched on gets that effect IN FRONT of its chain, and the inserts it stores move back.
    // trackFxPrepend() is the ONE place that says which blocks were on and what each becomes: the
    // sound (convertTrackFxParams) and the locks of its steps (convertStepLocks) both go by it.

    // An id of an insert slot: which slot, and the same cell of another slot.
    struct InsertId
    {
        int slot = -1;              // -1: not an insert id
        int base = 0, perSlot = 1, cell = 0;
        int at(int otherSlot) const
        {
            return otherSlot >= 0 && otherSlot < AudioEngine::MAX_INSERTS_PER_TRACK ? base + otherSlot * perSlot + cell : -1;
        }
    };
    inline InsertId insertId(int id)
    {
        const int slots = AudioEngine::MAX_INSERTS_PER_TRACK;
        auto in = [id](int base, int perSlot) { return InsertId { (id - base) / perSlot, base, perSlot, (id - base) % perSlot }; };
        if (id >= AudioEngine::EFFECT_PARAM_BASE && id < AudioEngine::EFFECT_PARAM_BASE + slots * 4)
            return in(AudioEngine::EFFECT_PARAM_BASE, 4);
        if (id >= INSERT_TYPE_BASE && id < INSERT_TYPE_BASE + slots)
            return in(INSERT_TYPE_BASE, 1);
        for (int base : { AudioEngine::INSERT_ON_BASE, AudioEngine::INSERT_WET_BASE, AudioEngine::INSERT_SYNC_BASE, AudioEngine::INSERT_DIV_BASE })
            if (id >= base && id < base + slots)
                return in(base, 1);
        if (AudioEngine::isInsertExtraParam(id))
            return in(AudioEngine::INSERT_EXTRA_BASE, EffectBase::MAX_EXTRAS);
        return {};
    }

    // How many insert slots a stored sound names (up to the first slot without a type).
    inline int storedInsertCount(const ParamMap& params)
    {
        int count = 0;
        for (; count < AudioEngine::MAX_INSERTS_PER_TRACK; ++count)
        {
            auto it = params.find(INSERT_TYPE_BASE + count);
            if (it == params.end() || ! (it->second >= 0.5f))
                break;
        }
        return count;
    }

    struct OldBlockSlot
    {
        static constexpr int CELLS = 4 + EffectBase::MAX_EXTRAS;
        EffectType type;
        float cells[CELLS];         // the effect's four cells, then its cells beyond four
        float wet = -1.0f;          // the slot's WET; below 0: as a new slot has it (1)
        int block = -1;             // the block whose cells are this effect's, one for one (TrackFxParam::
                                    // DIRT_BASE / TALK_BASE: a step's lock on one follows it); -1: none
        // Cell `cell` of this effect in slot `slot`, as an insert id.
        static int cellId(int slot, int cell)
        {
            return cell < 4 ? AudioEngine::EFFECT_PARAM_BASE + slot * 4 + cell
                            : AudioEngine::INSERT_EXTRA_BASE + slot * EffectBase::MAX_EXTRAS + (cell - 4);
        }
    };
    static_assert(OldBlockSlot::CELLS == 8, "a block had eight cells: its effect's four and four beyond");

    // What goes in front of the chain, in order: DIRT, TALK, then MODFX's phaser, flanger, chorus,
    // rotary - each only if it was on, and no more of them than the chain has slots left for.
    // (What does not fit is not loaded: the sound's own inserts are never pushed off the end.)
    inline std::vector<OldBlockSlot> trackFxPrepend(const ParamMap& params)
    {
        auto val = [&](int id) -> float
        {
            auto it = params.find(id);
            return it != params.end() ? it->second : 0.0f;
        };
        const int dirt = TrackFxParam::DIRT_BASE, talk = TrackFxParam::TALK_BASE, modfx = TrackFxParam::MODFX_BASE;
        std::vector<OldBlockSlot> prepend;

        // DIRT: on if any of Drive, Bits, Rate, Feedback > 0 and Mix > 0.
        if (val(dirt + 7) > 0.0f && (val(dirt + 0) > 0.0f || val(dirt + 2) > 0.0f || val(dirt + 3) > 0.0f || val(dirt + 4) > 0.0f))
            prepend.push_back({ EffectType::DirtInsertEffect,
                                { val(dirt + 0), val(dirt + 1), val(dirt + 2), val(dirt + 3), val(dirt + 4), val(dirt + 5), val(dirt + 6), val(dirt + 7) },
                                -1.0f, dirt });
        // TALK: on if Mix > 0.
        if (val(talk + 3) > 0.0f)
            prepend.push_back({ EffectType::TalkInsertEffect,
                                { val(talk + 0), val(talk + 1), val(talk + 2), val(talk + 3), val(talk + 4), val(talk + 5), val(talk + 6), val(talk + 7) },
                                -1.0f, talk });

        // MODFX: each sub-effect that was on becomes an insert of its own (its cells map to no
        // cell of theirs one for one: a step's lock on a MODFX cell is not carried over).
        auto wetFor = [](float depth) { return 0.5f * juce::jmin(1.0f, depth * 4.0f); };
        const float phaserDepth = val(modfx + 5), flangerDepth = val(modfx + 3), chorusDepth = val(modfx + 1), rotaryDepth = val(modfx + 7);
        // Phaser: Rate -> cell 0, Depth -> cell 1, Feedback 0.5, Stages 6; its mix is the slot's WET.
        if (phaserDepth > 0.0f)
            prepend.push_back({ EffectType::PhaserEffect, { val(modfx + 4), phaserDepth, 0.5f, 6.0f, 0, 0, 0, 0 }, wetFor(phaserDepth) });
        // Flanger: Rate -> cell 0, Depth -> cell 1, Feedback 0.6, Mix -> cell 3.
        if (flangerDepth > 0.0f)
            prepend.push_back({ EffectType::FlangerEffect, { val(modfx + 2), flangerDepth, 0.6f, wetFor(flangerDepth), 0, 0, 0, 0 } });
        // Chorus: Rate -> cell 0, Depth -> cell 1, Mix -> cell 2, Voices 3.
        if (chorusDepth > 0.0f)
            prepend.push_back({ EffectType::ChorusEffect, { val(modfx + 0), chorusDepth, wetFor(chorusDepth), 3.0f, 0, 0, 0, 0 } });
        // Rotary: Speed -> cell 0; its depth is the slot's WET (the old block used setMix).
        if (rotaryDepth > 0.0f)
            prepend.push_back({ EffectType::RotaryEffect, { val(modfx + 6), 0, 0, 0, 0, 0, 0, 0 }, rotaryDepth });

        const int room = AudioEngine::MAX_INSERTS_PER_TRACK - storedInsertCount(params);
        if ((int) prepend.size() > room)
            prepend.resize((size_t) juce::jmax(0, room));
        return prepend;
    }

    // A stored sound with its old blocks as insert effects in front of its chain: the block ids
    // are gone from it, the blocks' effects are its first slots, and the inserts it named follow.
    // A sound with no block on (or no slot free for one) is returned as it is.
    inline ParamMap convertTrackFxParams(const ParamMap& params)
    {
        const auto prepend = trackFxPrepend(params);
        if (prepend.empty())
            return params;
        const int shift = (int) prepend.size();
        const int named = storedInsertCount(params);

        ParamMap out;
        for (auto& [id, v] : params)
        {
            if (TrackFxParam::isTrackFxParam(id))
                continue;
            const auto insert = insertId(id);
            if (insert.slot < 0)
                out[id] = v;
            else if (const int moved = insert.at(insert.slot + shift); insert.slot < named && moved >= 0)
                out[moved] = v;                 // (a slot the sound names no effect for holds nothing)
        }
        for (int i = 0; i < shift; ++i)
        {
            const auto& slot = prepend[(size_t) i];
            out[INSERT_TYPE_BASE + i] = (float) ((int) slot.type + 1);
            for (int c = 0; c < OldBlockSlot::CELLS; ++c)
                out[OldBlockSlot::cellId(i, c)] = slot.cells[c];
            if (slot.wet >= 0.0f)
                out[AudioEngine::INSERT_WET_BASE + i] = slot.wet;
        }
        return out;
    }

    // The locks of a track's steps, for a sound whose blocks convertTrackFxParams turned into
    // inserts (`params`: the sound as it was stored, BEFORE that): a lock on a DIRT or TALK cell
    // goes to the same cell of the insert the block became, a lock on an insert slot moves back
    // with its slot (off the end of the chain: gone), a lock on a block that became no insert
    // (MODFX; a block that was off, or had no slot left) is dropped.
    inline void convertStepLocks(const ParamMap& params, Track& track)
    {
        const auto prepend = trackFxPrepend(params);
        if (prepend.empty())
            return;
        const int shift = (int) prepend.size();
        auto slotOfBlock = [&](int block)
        {
            for (int i = 0; i < shift; ++i)
                if (prepend[(size_t) i].block == block)
                    return i;
            return -1;
        };

        for (auto& step : track.steps)
        {
            ParamMap newLocks;
            for (auto& [id, v] : step.paramLocks)
            {
                if (TrackFxParam::isTrackFxParam(id))
                {
                    const int block = id < TrackFxParam::TALK_BASE ? TrackFxParam::DIRT_BASE
                                    : id < TrackFxParam::MODFX_BASE ? TrackFxParam::TALK_BASE : TrackFxParam::MODFX_BASE;
                    if (const int slot = slotOfBlock(block); slot >= 0)
                        newLocks[OldBlockSlot::cellId(slot, id - block)] = v;
                }
                else if (const auto insert = insertId(id); insert.slot >= 0)
                {
                    if (const int moved = insert.at(insert.slot + shift); moved >= 0)
                        newLocks[moved] = v;
                }
                else
                    newLocks[id] = v;
            }
            step.paramLocks = std::move(newLocks);
        }
    }

    // The insert slots of a chain as stored values: a track's, or with AudioEngine::sendTrack(bus) a
    // send's (the same ids for both). Type + 1 a slot, its switches, its four cells, and the cells
    // beyond four that the effect has.
    inline void captureChain(const AudioEngine& audio, int t, ParamMap& out)
    {
        for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
        {
            EffectType type;
            if (!audio.getInsertEffectType(t, slot, type))
                break;
            out[INSERT_TYPE_BASE + slot] = (float) ((int) type + 1);
            out[AudioEngine::INSERT_ON_BASE + slot] = audio.getTrackParameter(t, AudioEngine::INSERT_ON_BASE + slot);
            out[AudioEngine::INSERT_WET_BASE + slot] = audio.getTrackParameter(t, AudioEngine::INSERT_WET_BASE + slot);
            out[AudioEngine::INSERT_SYNC_BASE + slot] = audio.getTrackParameter(t, AudioEngine::INSERT_SYNC_BASE + slot);
            out[AudioEngine::INSERT_DIV_BASE + slot] = audio.getTrackParameter(t, AudioEngine::INSERT_DIV_BASE + slot);
            for (int p = 0; p < 4; ++p)
                out[AudioEngine::EFFECT_PARAM_BASE + slot * 4 + p] =
                    audio.getTrackParameter(t, AudioEngine::EFFECT_PARAM_BASE + slot * 4 + p);
            // (Only the extra cells the effect has: a file from before them reads the same as one without.)
            if (auto* chain = audio.getInsertChain(t))
                if (auto* fx = chain->getEffect(slot))
                    for (int p = 0; p < EffectBase::MAX_EXTRAS; ++p)
                        if (fx->getExtraSpec(p).isValid())
                            out[AudioEngine::INSERT_EXTRA_BASE + slot * EffectBase::MAX_EXTRAS + p] = fx->getExtra(p);
        }
    }

    // A chain emptied and given the effects a stored map names (INSERT_TYPE_BASE + slot, up to the
    // first slot without one); their cells are set afterwards by whoever applies the map.
    inline void buildChain(AudioEngine& audio, int t, const ParamMap& stored)
    {
        while (audio.getInsertChain(t) != nullptr && audio.getInsertChain(t)->getEffectCount() > 0)
            audio.removeInsertEffect(t, 0);
        for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
        {
            auto it = stored.find(INSERT_TYPE_BASE + slot);
            if (it == stored.end() || ! (it->second >= 0.5f))
                break;
            int type1 = juce::jlimit(1, (int) EffectType::NumEffectTypes, (int) std::lround(juce::jmin(it->second, 1.0e6f)));
            audio.addInsertEffect(t, static_cast<EffectType>(type1 - 1));
        }
    }

    // A send's stored chain put on the bus: the effects, then every value of theirs.
    inline void applySendChain(AudioEngine& audio, int bus, const ParamMap& stored)
    {
        const int t = AudioEngine::sendTrack(bus);
        buildChain(audio, t, stored);
        for (auto& [id, value] : stored)
            if (AudioEngine::isInsertParam(id))
                audio.setTrackParameter(t, id, value);
    }

    // Everything that defines how track `t` sounds, read from the engine / mixer / effects (they are
    // the truth; values the user never touched are included).
    inline ParamMap captureTrackSound(const Live& live, int t, const ParamMap& stored)
    {
        ParamMap out = stored;

        // Stale insert-slot entries are rebuilt below; the ids of the DIRT / TALK / MODFX blocks of
        // before hold nothing any more (those effects are inserts of the chain).
        for (auto it = out.begin(); it != out.end();)
            it = ((it->first >= AudioEngine::EFFECT_PARAM_BASE && it->first < TrackFxParam::END)
                  || AudioEngine::isInsertSwitchParam(it->first) || AudioEngine::isInsertExtraParam(it->first)) ? out.erase(it) : std::next(it);

        if (auto* engine = live.audio.getEngine(t))
            for (auto& page : engine->getParamPages())
                for (auto& p : page.params)
                    if (p.isValid())
                        out[p.id] = engine->getParameter(p.id);

        if (auto* engine = live.audio.getEngine(t); engine != nullptr && engine->supportsModulation())
        {
            for (int id = ModParam::LFO_BASE; id <= ModParam::ENV_COUNT; ++id)
                out[id] = engine->getParameter(id);
            for (int id = ModParam::LFO_EXT_BASE; id < ModParam::LFO_EXT_END; ++id)
                out[id] = engine->getParameter(id);
            for (int id = ModParam::LFO_EXT2_BASE; id < ModParam::LFO_EXT2_END; ++id)
                out[id] = engine->getParameter(id);
        }

        // Where pad pressure goes (AT ROUTE): the router's pressure slots, cells on no page.
        if (auto* engine = live.audio.getEngine(t); engine != nullptr && engine->modRouter() != nullptr)
            for (int id = PressureSlotParam::BASE; id < PressureSlotParam::END; ++id)
                out[id] = engine->getParameter(id);

        out[MixParam::VOLUME] = live.audio.getTrackParameter(t, MixParam::VOLUME);
        out[MixParam::PAN] = live.audio.getTrackParameter(t, MixParam::PAN);
        for (int s = 0; s < MixParam::MAX_SENDS; ++s)
            out[MixParam::SEND_BASE + s] = live.audio.getTrackParameter(t, MixParam::SEND_BASE + s);

        // The track's input (TRK FX page IN).
        for (int id : AudioEngine::TRACK_IN_IDS)
            out[id] = live.audio.getTrackParameter(t, id);

        captureChain(live.audio, t, out);
        return out;
    }

    inline ProjectData capture(const Live& live, const juce::String& name)
    {
        ProjectData data;
        data.name = name;
        data.bpm = live.sequencer.getTempo();
        data.activePattern = live.sequencer.getActivePatternIndex();
        data.masterLevel = live.audio.getMasterLevel();
        for (int i = 0; i < AudioEngine::NUM_MASTER_PARAMS; ++i)
            data.master.push_back(live.audio.getMasterParam(i));

        for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
        {
            const auto& pat = live.sequencer.getPattern(p);
            auto& pd = data.patterns[(size_t) p];
            pd.swing = pat.swing;
            pd.tracks.resize((size_t) pat.getTrackCount());
            for (int t = 0; t < pat.getTrackCount(); ++t)
            {
                const auto& track = pat.getTrack(t);
                auto& td = pd.tracks[(size_t) t];
                td.engineType = track.engineType;
                td.length = track.length;
                td.speedMultiplier = track.speedMultiplier;
                td.midiChannel = track.midiChannel;
                td.muted = track.muted;
                td.soloed = track.soloed;
                td.soundParams = (p == data.activePattern) ? captureTrackSound(live, t, track.soundParams)
                                                           : ParamMap();
                // Samples are saved by name as well (sampler: its sample; wavetable: its USER source).
                if (p == data.activePattern
                    && (track.engineType == EngineType::Sampler || track.engineType == EngineType::Wavetable))
                {
                    auto it = td.soundParams.find(SAMPLE_INDEX_PARAM);
                    if (it != td.soundParams.end())
                        if (const auto* e = SampleBank::get().entry((int) std::lround(it->second)))
                            td.sampleName = juce::String(e->name);
                }
                // ... and so are the samples of a wavetable track's key frames (0 = OFF, else index + 1).
                if (p == data.activePattern && track.engineType == EngineType::Wavetable)
                {
                    bool any = false;
                    juce::StringArray names;
                    for (int k = 0; k < WavetableKeys::MAX_KEYS; ++k)
                    {
                        auto it = td.soundParams.find(WavetableEngine::KeySampleBase + k);
                        const auto* e = it != td.soundParams.end()
                                            ? SampleBank::get().entry((int) std::lround(it->second) - 1) : nullptr;
                        names.add(e != nullptr ? juce::String(e->name) : juce::String());
                        any = any || e != nullptr;
                    }
                    if (any)
                        td.keySamples = names;
                }
                td.steps.clear();
                for (int s = 0; s < track.length && s < (int) track.steps.size(); ++s)
                    td.steps.push_back(captureStep(track.steps[(size_t) s]));
            }
            if (! pat.perfMoves.empty())
                pd.perfMoves = pat.perfMoves.toFlat();
        }

        // The engine type of a track is the one actually loaded.
        auto& activeTracks = data.patterns[(size_t) data.activePattern].tracks;
        for (int t = 0; t < (int) activeTracks.size(); ++t)
            if (auto* engine = live.audio.getEngine(t))
                activeTracks[(size_t) t].engineType = engine->getType();
        // The preset each sound came from (not that of an engine the track no longer has).
        if (live.presetNames != nullptr)
            for (int t = 0; t < (int) activeTracks.size(); ++t)
                activeTracks[(size_t) t].presetName = live.presetNames->nameFor(t, activeTracks[(size_t) t].engineType);

        // ARCH memory for unified-voice tracks.
        for (int t = 0; t < (int) activeTracks.size(); ++t)
            if (activeTracks[(size_t) t].engineType == EngineType::UnifiedVoice)
                if (auto* uv = dynamic_cast<UnifiedVoiceEngine*>(live.audio.getEngine(t)))
                    activeTracks[(size_t) t].archMemory = uv->captureArchMemory();

        for (int b = 0; b < live.audio.getSendCount(); ++b)
        {
            SendBusData sb;
            sb.effectType = (int) live.audio.getSendBusType(b);      // (for a build that reads one type a bus)
            sb.sendLevel = live.audio.getSendBus(b).getSendLevel();
            sb.hasChain = true;
            captureChain(live.audio, AudioEngine::sendTrack(b), sb.chain);
            data.sendBuses.push_back(sb);
        }

        if (live.song != nullptr)
        {
            for (int i = 0; i < live.song->getEntryCount(); ++i)
                data.song.entries.push_back(live.song->getEntry(i));
            data.song.loop = live.song->getLoopMode();
        }

        if (live.chords != nullptr && live.chords->hasContent())
            data.chordPads = live.chords->toFlat();
        data.perfPads = live.audio.getPerfFx().toFlat();

        if (live.scales != nullptr)
        {
            for (int i = 0; i < live.scales->getPageCount(); ++i)
            {
                const auto& page = live.scales->getPage(i);
                ScalePage sp;
                sp.name = page.getName();
                sp.rootNote = page.rootNote;
                sp.isFactory = page.isFactory;
                for (int n = 0; n < 16; ++n)
                    sp.padNotes[(size_t) n] = page.notes[n];
                data.customScalePages.push_back(sp);
            }
        }

        {
            auto& r = live.audio.getPadRatchet();
            data.roll = { r.isEnabled() ? 1.0f : 0.0f, r.getMode() == PadRatchet::Mode::Free ? 1.0f : 0.0f,
                          (float) (int) r.getBaseDivision(), (float) (int) r.getMaxDivision(),
                          r.getBaseRateHz(), r.getMaxRateHz(), r.getPressureDepth(), r.getGate(),
                          r.getPressureToVelocity(), r.getVelocityDecay(),
                          r.getSwing(), r.getGlide(), r.getThreshold(), 0.0f /* (was LTCH: not stored) */,
                          r.getHumanize(), (float) r.getAccent(), r.getProbability(), (float) r.getMaxRepeats() };
        }

        if (live.arpForTrack)
            for (int t = 0; t < live.sequencer.getPattern().getTrackCount(); ++t)
            {
                auto* arp = live.arpForTrack(t);
                data.arps.push_back(arp != nullptr ? captureArp(*arp) : std::vector<float>());
            }

        if (live.getKeyboard)
            data.keyboard = live.getKeyboard();

        // Pad-pressure routes, one list per track. (The truth is in each track's parameters: its
        // pressure slots, ids 773-784. This list is the same in the words of a build from before
        // the slots had cells; a slot aimed at something those words cannot name is OFF in it.)
        for (int t = 0; t < live.sequencer.getPattern().getTrackCount() && t < AudioEngine::MAX_TRACKS; ++t)
        {
            AftertouchRouteData routes;
            const auto& router = live.audio.getPressureRouter(t);
            for (int i = 0; i < AftertouchRouter::MAX_ROUTES; ++i)
            {
                auto r = router.effectiveRoute(i);
                routes.routes.push_back({ (int) r.dest, r.depth, r.bipolar });
            }
            data.trackAftertouch.push_back(routes);
        }
        if (live.pressure != nullptr)
        {
            for (int i = 0; i < live.pressure->getRouteCount(); ++i)
            {
                auto r = live.pressure->getRoute(i);
                data.aftertouchRouting.routes.push_back({ (int) r.dest, r.depth, r.bipolar });
            }
        }
        return data;
    }

    // Load a project's state into the running instrument (message thread; playback is stopped).
    inline void apply(const ProjectData& data, const Live& live)
    {
        // The whole instrument is rebuilt: the audio thread stays out until it is done.
        const AudioEngine::StructureLock structureLock(live.audio.structure());
        live.sequencer.stop();
        live.sequencer.setTempo(juce::jlimit(20.0f, 300.0f, data.bpm > 0.0f ? data.bpm : 120.0f));
        live.audio.setMasterLevel(data.masterLevel);
        {
            // Stored master settings, or the defaults for a project without them.
            for (int i = 0; i < AudioEngine::NUM_MASTER_PARAMS; ++i)
                live.audio.setMasterParam(i, storedValue(data.master, (size_t) i, AudioEngine::MASTER_DEFAULTS[i]));
        }
        live.audio.setSequencer(&live.sequencer);

        const int active = juce::jlimit(0, SequencerCore::MAX_PATTERNS - 1, data.activePattern);
        const auto& soundTracks = data.patterns[(size_t) active].tracks;

        // The track count is the same in every pattern: that of the pattern carrying the sounds,
        // or the usual eight for an empty project.
        const int trackCount = juce::jlimit(1, AudioEngine::MAX_TRACKS, soundTracks.empty() ? 8 : (int) soundTracks.size());

        for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
        {
            const auto& pd = data.patterns[(size_t) p];
            auto& pat = live.sequencer.getPattern(p);
            pat.swing = pd.swing;
            pat.tracks.assign((size_t) trackCount, Track());
            for (int t = 0; t < trackCount; ++t)
            {
                auto& track = pat.getTrack(t);
                if (t < (int) pd.tracks.size())
                {
                    const auto& td = pd.tracks[(size_t) t];
                    track.setLength(juce::jmax(1, td.length));
                    track.speedMultiplier = td.speedMultiplier > 0.0f ? td.speedMultiplier : 1.0f;
                    track.midiChannel = td.midiChannel;
                    track.muted = td.muted;
                    track.soloed = td.soloed;
                    for (int s = 0; s < (int) td.steps.size() && s < (int) track.steps.size(); ++s)
                        applyStep(td.steps[(size_t) s], track.steps[(size_t) s]);
                }
                // One sound per track for all patterns.
                if (t < (int) soundTracks.size())
                {
                    // (a number that is no engine, from a damaged file, is the engine a new track has)
                    const int stored = (int) soundTracks[(size_t) t].engineType;
                    track.engineType = stored >= 0 && stored < ENGINE_TYPE_COUNT ? soundTracks[(size_t) t].engineType
                                                                                          : EngineType::UnifiedVoice;
                    track.soundParams = soundTracks[(size_t) t].soundParams;
                }
            }
            // Recorded PERF pad moves for this pattern (empty for older projects).
            if (! pd.perfMoves.empty())
                pat.perfMoves.fromFlat(pd.perfMoves);
            else
                pat.perfMoves.clear();
        }

        live.sequencer.selectPattern(active, SequencerCore::ChangeMode::Immediate);
        live.sequencer.setSelectedTrack(juce::jmin(live.sequencer.getSelectedTrack(), trackCount - 1));

        for (int t = 0; t < trackCount; ++t)
        {
            ParamMap params = live.sequencer.getPattern(active).getTrack(t).soundParams;
            const EngineType type = live.sequencer.getPattern(active).getTrack(t).engineType;

            // The sample by name: the stored index may point elsewhere since files were added.
            if (t < (int) soundTracks.size() && soundTracks[(size_t) t].sampleName.isNotEmpty()
                && (type == EngineType::Sampler || type == EngineType::Wavetable))
            {
                const int index = SampleBank::get().indexOf(soundTracks[(size_t) t].sampleName);
                if (index >= 0)
                    params[SAMPLE_INDEX_PARAM] = (float) index;
            }
            // The key frames' samples by name, in the same way.
            if (t < (int) soundTracks.size() && type == EngineType::Wavetable)
            {
                const auto& names = soundTracks[(size_t) t].keySamples;
                for (int k = 0; k < WavetableKeys::MAX_KEYS && k < names.size(); ++k)
                {
                    const int index = names[k].isNotEmpty() ? SampleBank::get().indexOf(names[k]) : -1;
                    if (index >= 0)
                        params[WavetableEngine::KeySampleBase + k] = (float) (index + 1);
                }
            }

            // The DIRT / TALK / MODFX blocks of a project of before them are inserts in front of its
            // chain: the sound, and the locks of its steps in every pattern. `sound` is what the
            // track holds from here on (for a project without such a block: `params` itself).
            const ParamMap sound = convertTrackFxParams(params);
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                convertStepLocks(params, live.sequencer.getPattern(p).getTrack(t));

            // A fresh engine of the stored type (this clears the track's stored engine parameters,
            // so they are put back afterwards), then the insert chain, then every parameter.
            if (live.audio.getEngine(t) == nullptr)
                live.audio.addEngine(t, AudioEngine::createEngine(type));
            live.audio.setEngineType(t, type, true);
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                live.sequencer.getPattern(p).getTrack(t).soundParams = sound;

            buildChain(live.audio, t, sound);

            // A project that never stored a mixer value gets the defaults, not the previous project's.
            live.audio.setTrackParameter(t, MixParam::VOLUME, MixParam::DEFAULT_VOLUME);
            live.audio.setTrackParameter(t, MixParam::PAN, 0.0f);
            for (int s = 0; s < MixParam::MAX_SENDS; ++s)
                live.audio.setTrackParameter(t, MixParam::SEND_BASE + s, 0.0f);

            // (and no input: a project saved before tracks had one)
            for (int id : AudioEngine::TRACK_IN_IDS)
                live.audio.setTrackParameter(t, id, AudioEngine::trackInDefault(id));

            live.audio.applyAllTrackParameters(t, sound);

            // ARCH memory for unified-voice tracks (absent in older projects: nothing to restore).
            if (t < (int) soundTracks.size() && !soundTracks[(size_t) t].archMemory.empty())
                if (auto* uv = dynamic_cast<UnifiedVoiceEngine*>(live.audio.getEngine(t)))
                    uv->applyArchMemory(soundTracks[(size_t) t].archMemory);
        }

        // The sends: as a new project has them (a project without stored sends), then what the
        // file holds. A file with the buses' chains: each chain as it was saved. A file from
        // before them names one type a bus: the type a new project has there means that bus as it
        // is in a new project (the reverb at DCAY 0.6 / DAMP 0.4, the delay at 0.375 s / 0.35: all
        // such a project ever played), any other type that effect as it is made, its mix at 1.
        live.audio.resetSendBuses();
        for (int b = 0; b < (int) data.sendBuses.size() && b < AudioEngine::MAX_SENDS; ++b)
        {
            const auto& stored = data.sendBuses[(size_t) b];
            if (stored.hasChain)
            {
                while (live.audio.getSendCount() <= b)
                    if (live.audio.addSendBus(AudioEngine::defaultSendType(b)) < 0)
                        break;
                if (b < live.audio.getSendCount())
                    applySendChain(live.audio, b, stored.chain);
            }
            else if (b < live.audio.getSendCount())
            {
                int type = juce::jlimit(0, (int) EffectType::NumEffectTypes - 1, stored.effectType);
                if (type != (int) AudioEngine::defaultSendType(b))
                    live.audio.setInsertEffect(AudioEngine::sendTrack(b), 0, static_cast<EffectType>(type));
            }
        }

        if (live.song != nullptr)
        {
            live.song->stop();
            while (live.song->getEntryCount() > 0)
                live.song->removeEntry(live.song->getEntryCount() - 1);
            for (auto& e : data.song.entries)
            {
                live.song->addEntry(e.patternIndex, e.repeatCount);
                auto entry = live.song->getEntry(live.song->getEntryCount() - 1);
                entry.transposeSemitones = e.transposeSemitones;
                live.song->setEntry(live.song->getEntryCount() - 1, entry);
            }
            live.song->setLoopMode(data.song.loop);
        }

        if (live.chords != nullptr)
            live.chords->fromFlat(data.chordPads);      // (a project without chords: every pad empty)
        // PERF's pads; a project from before they existed gets the starting layout.
        if (! live.audio.getPerfFx().fromFlat(data.perfPads))
            live.audio.loadFactoryPerfPads();

        if (live.scales != nullptr)
        {
            for (int i = 0; i < (int) data.customScalePages.size() && i < CustomScaleManager::MAX_PAGES; ++i)
            {
                while (live.scales->getPageCount() <= i)
                    if (live.scales->addPage("User") < 0)
                        break;
                if (i >= live.scales->getPageCount())
                    break;
                const auto& sp = data.customScalePages[(size_t) i];
                live.scales->setPageName(i, sp.name);
                live.scales->setRootNote(i, sp.rootNote);
                for (int n = 0; n < 16; ++n)
                    live.scales->setNoteForPad(i, n, sp.padNotes[(size_t) n]);
            }
        }

        {
            // Stored roll settings, or the defaults for a project without them.
            auto& r = live.audio.getPadRatchet();
            auto at = [&](size_t i, float fallback) { return storedValue(data.roll, i, fallback); };
            r.allOff();
            r.setEnabled(at(0, 0) > 0.5f);
            r.setMode(at(1, 0) > 0.5f ? PadRatchet::Mode::Free : PadRatchet::Mode::Quantized);
            r.setBaseDivision((PadRatchet::Division) (int) at(2, 3));
            r.setMaxDivision((PadRatchet::Division) (int) at(3, (float) (int) PadRatchet::Div1_128));
            r.setBaseRateHz(at(4, 2));
            r.setMaxRateHz(at(5, 40));
            r.setPressureDepth(at(6, 1));
            r.setGate(at(7, 0.5f));
            r.setPressureToVelocity(at(8, 0.5f));
            r.setVelocityDecay(at(9, 0));
            r.setSwing(at(10, 0));
            r.setGlide(at(11, 0));
            r.setThreshold(at(12, 0));
            // LTCH is never brought back: a roll latch left on would make every pad roll on and on
            // after a start or a load, with nothing on the display to say why. It starts off.
            r.setLatch(false);
            r.setHumanize(at(14, 0));
            r.setAccent((int) at(15, 0));
            r.setProbability(at(16, 1));
            r.setMaxRepeats((int) at(17, 0));
        }

        if (live.arpForTrack)
            for (int t = 0; t < trackCount; ++t)
                if (auto* arp = live.arpForTrack(t))
                {
                    arp->setEnabled(false);      // releases anything it was holding
                    if (t < (int) data.arps.size())
                        applyArp(data.arps[(size_t) t], *arp);
                }

        if (live.setKeyboard)
            live.setKeyboard(data.keyboard);

        // The preset each track's sound came from; none in a project stored without them.
        if (live.presetNames != nullptr)
            for (int t = 0; t < juce::jmax(trackCount, (int) AudioEngine::MAX_TRACKS); ++t)
                live.presetNames->set(t, t < trackCount && t < (int) soundTracks.size() ? soundTracks[(size_t) t].presetName : juce::String(),
                                      t < trackCount ? live.sequencer.getPattern(active).getTrack(t).engineType : EngineType::UnifiedVoice);

        // Pad-pressure routes. A project that stores a track's pressure slots with its parameters
        // (ids 773-784): they are set already, and nothing here touches them. Before the slots had
        // cells - a project with per-track lists: each track gets its own. An older project with
        // one list: every track whose engine uses a router gets that list (it never reached the
        // drum kits, which keep their default). Neither: each engine's default.
        auto toRouter = [](const AftertouchRouteData& list)
        {
            AftertouchRouter router;
            router.clear();
            for (int i = 0; i < AftertouchRouter::MAX_ROUTES && i < (int) list.routes.size(); ++i)
            {
                const auto& e = list.routes[(size_t) i];
                router.setRoute(i, { static_cast<AftertouchRouter::Destination>(juce::jlimit(0, AftertouchRouter::DEST_COUNT - 1, e.destination)),
                                     std::isfinite(e.depth) ? juce::jlimit(-1.0f, 1.0f, e.depth) : 0.0f,     // (DPTH is -1..1)
                                     e.bipolar });
            }
            return router;
        };

        const bool hasGlobal = !data.aftertouchRouting.routes.empty();
        if (live.pressure != nullptr && hasGlobal)
            *live.pressure = toRouter(data.aftertouchRouting);

        for (int t = 0; t < trackCount; ++t)
        {
            const auto& stored = live.sequencer.getPattern(active).getTrack(t).soundParams;
            if (stored.lower_bound(PressureSlotParam::BASE) != stored.lower_bound(PressureSlotParam::END))
                continue;
            if (t < (int) data.trackAftertouch.size())
                live.audio.setPressureRouter(t, toRouter(data.trackAftertouch[(size_t) t]));
            else if (hasGlobal && live.audio.getEngineTypeOf(t) != EngineType::Drum)
                live.audio.setPressureRouter(t, toRouter(data.aftertouchRouting));
            else
                live.audio.resetPressureRouter(t);
        }
    }
}
