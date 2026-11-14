#pragma once

#include "Project.h"
#include "../sequencer/SequencerCore.h"
#include "../engine/AudioEngine.h"
#include "../arpeggiator/Arpeggiator.h"
#include "../controller/ChordPads.h"

// -----------------------------------------------------------------------------
// Moves the whole live state of the instrument into a ProjectData (for saving)
// and back (after loading or "new"): tempo, all 16 patterns with every step and
// lock, each track's engine and every parameter the pages show (engine, mixer,
// effect blocks, insert effects), send buses, master level, song, custom scale
// pages and pad-pressure routing.
//
// The sound of a track (engine, parameters, effects) is one per track, shared by
// all patterns; it is stored on the active pattern's tracks and written to every
// pattern on load.
// -----------------------------------------------------------------------------
namespace ProjectBridge
{
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
        AftertouchRouter* pressure = nullptr;     // legacy single router (optional); the routes are per track in `audio`
        std::function<Arpeggiator*(int track)> arpForTrack;     // one arpeggiator per track
        ChordPads* chords = nullptr;              // the CHORD screen's pads
    };

    // Arpeggiator settings as a flat list; the order is the file format (append only).
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
            a.isEnabled() ? 1.0f : 0.0f,
            (float) (int) a.getStripTarget() };
        // Index 31 on: the tapped rhythm.
        auto rhythm = a.getRhythmState();
        v.insert(v.end(), rhythm.begin(), rhythm.end());
        return v;
    }

    inline void applyArp(const std::vector<float>& v, Arpeggiator& a)
    {
        auto at = [&](size_t i, float fallback) { return i < v.size() ? v[i] : fallback; };
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
        a.setEnabled(at(29, 0) > 0.5f);
        a.setStripTarget((Arpeggiator::StripTarget) juce::jlimit(0, (int) Arpeggiator::StripTarget::NumTargets - 1, (int) at(30, 0)));
        if (v.size() > 32)
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
        s.clear();
        s.trig = d.trig;
        s.notes = d.notes;
        s.velocity = d.velocity;
        s.length = d.length;
        s.microTiming = d.microTiming;
        s.trigCondition.type = d.trigConditionType;
        s.trigCondition.a = d.trigA;
        s.trigCondition.b = d.trigB;
        s.trigCondition.probability = d.trigProbability;
        s.paramLocks = d.paramLocks;
        s.pageChangeLock = d.pageChangeLock;
        s.pressureScale = d.pressureScale;
    }

    // Everything that defines how track `t` sounds, read from the engine / mixer / effects (they are
    // the truth; values the user never touched are included).
    inline ParamMap captureTrackSound(const Live& live, int t, const ParamMap& stored)
    {
        ParamMap out = stored;

        // Stale insert-slot entries are rebuilt below.
        for (auto it = out.begin(); it != out.end();)
            it = ((it->first >= AudioEngine::EFFECT_PARAM_BASE && it->first < TrackFxParam::DIRT_BASE)
                  || AudioEngine::isInsertSwitchParam(it->first)) ? out.erase(it) : std::next(it);

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

        out[MixParam::VOLUME] = live.audio.getTrackParameter(t, MixParam::VOLUME);
        out[MixParam::PAN] = live.audio.getTrackParameter(t, MixParam::PAN);
        for (int s = 0; s < MixParam::MAX_SENDS; ++s)
            out[MixParam::SEND_BASE + s] = live.audio.getTrackParameter(t, MixParam::SEND_BASE + s);

        for (int id = TrackFxParam::DIRT_BASE; id < TrackFxParam::END; ++id)
            out[id] = live.audio.getTrackParameter(t, id);

        for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
        {
            EffectType type;
            if (!live.audio.getInsertEffectType(t, slot, type))
                break;
            out[INSERT_TYPE_BASE + slot] = (float) ((int) type + 1);
            out[AudioEngine::INSERT_ON_BASE + slot] = live.audio.getTrackParameter(t, AudioEngine::INSERT_ON_BASE + slot);
            out[AudioEngine::INSERT_WET_BASE + slot] = live.audio.getTrackParameter(t, AudioEngine::INSERT_WET_BASE + slot);
            out[AudioEngine::INSERT_SYNC_BASE + slot] = live.audio.getTrackParameter(t, AudioEngine::INSERT_SYNC_BASE + slot);
            out[AudioEngine::INSERT_DIV_BASE + slot] = live.audio.getTrackParameter(t, AudioEngine::INSERT_DIV_BASE + slot);
            for (int p = 0; p < 4; ++p)
                out[AudioEngine::EFFECT_PARAM_BASE + slot * 4 + p] =
                    live.audio.getTrackParameter(t, AudioEngine::EFFECT_PARAM_BASE + slot * 4 + p);
        }
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
        }

        // The engine type of a track is the one actually loaded.
        auto& activeTracks = data.patterns[(size_t) data.activePattern].tracks;
        for (int t = 0; t < (int) activeTracks.size(); ++t)
            if (auto* engine = live.audio.getEngine(t))
                activeTracks[(size_t) t].engineType = engine->getType();

        for (int b = 0; b < live.audio.getSendCount(); ++b)
        {
            SendBusData sb;
            sb.effectType = (int) live.audio.getSendBusType(b);
            data.sendBuses.push_back(sb);
        }

        if (live.song != nullptr)
        {
            for (int i = 0; i < live.song->getEntryCount(); ++i)
                data.song.entries.push_back(live.song->getEntry(i));
            data.song.loop = live.song->getLoopMode();
        }

        if (live.chords != nullptr && live.chords->anyStored())
            data.chordPads = live.chords->toFlat();

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
                          r.getSwing(), r.getGlide(), r.getThreshold(), r.isLatch() ? 1.0f : 0.0f,
                          r.getHumanize(), (float) r.getAccent(), r.getProbability(), (float) r.getMaxRepeats() };
        }

        if (live.arpForTrack)
            for (int t = 0; t < live.sequencer.getPattern().getTrackCount(); ++t)
            {
                auto* arp = live.arpForTrack(t);
                data.arps.push_back(arp != nullptr ? captureArp(*arp) : std::vector<float>());
            }

        // Pad-pressure routes, one list per track.
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
            static const float defaults[AudioEngine::NUM_MASTER_PARAMS] = { 0, 0, 1000, 0, 0, 1, 0, -0.3f };
            for (int i = 0; i < AudioEngine::NUM_MASTER_PARAMS; ++i)
                live.audio.setMasterParam(i, i < (int) data.master.size() ? data.master[(size_t) i] : defaults[i]);
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
                    track.engineType = soundTracks[(size_t) t].engineType;
                    track.soundParams = soundTracks[(size_t) t].soundParams;
                }
            }
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

            // A fresh engine of the stored type (this clears the track's stored engine parameters,
            // so they are put back afterwards), then the insert chain, then every parameter.
            if (live.audio.getEngine(t) == nullptr)
                live.audio.addEngine(t, AudioEngine::createEngine(type));
            live.audio.setEngineType(t, type, true);
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                live.sequencer.getPattern(p).getTrack(t).soundParams = params;

            while (live.audio.getInsertChain(t) != nullptr && live.audio.getInsertChain(t)->getEffectCount() > 0)
                live.audio.removeInsertEffect(t, 0);
            for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
            {
                auto it = params.find(INSERT_TYPE_BASE + slot);
                if (it == params.end() || it->second < 0.5f)
                    break;
                int type1 = juce::jlimit(1, (int) EffectType::NumEffectTypes, (int) std::lround(it->second));
                live.audio.addInsertEffect(t, static_cast<EffectType>(type1 - 1));
            }

            // A project that never stored a mixer value gets the defaults, not the previous project's.
            live.audio.setTrackParameter(t, MixParam::VOLUME, MixParam::DEFAULT_VOLUME);
            live.audio.setTrackParameter(t, MixParam::PAN, 0.0f);
            for (int s = 0; s < MixParam::MAX_SENDS; ++s)
                live.audio.setTrackParameter(t, MixParam::SEND_BASE + s, 0.0f);
            const TrackFx defaults;
            for (int id = TrackFxParam::DIRT_BASE; id < TrackFxParam::END; ++id)
                live.audio.setTrackParameter(t, id, defaults.get(id));

            live.audio.applyAllTrackParameters(t, params);
        }

        for (int b = 0; b < (int) data.sendBuses.size() && b < live.audio.getSendCount(); ++b)
        {
            int type = juce::jlimit(0, (int) EffectType::NumEffectTypes - 1, data.sendBuses[(size_t) b].effectType);
            if ((int) live.audio.getSendBusType(b) != type)
                live.audio.setSendBusEffect(b, static_cast<EffectType>(type));
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
            auto at = [&](size_t i, float fallback) { return i < data.roll.size() ? data.roll[i] : fallback; };
            r.allOff();
            r.setEnabled(at(0, 0) > 0.5f);
            r.setMode(at(1, 0) > 0.5f ? PadRatchet::Mode::Free : PadRatchet::Mode::Quantized);
            r.setBaseDivision((PadRatchet::Division) (int) at(2, 3));
            r.setMaxDivision((PadRatchet::Division) (int) at(3, 7));
            r.setBaseRateHz(at(4, 2));
            r.setMaxRateHz(at(5, 40));
            r.setPressureDepth(at(6, 1));
            r.setGate(at(7, 0.5f));
            r.setPressureToVelocity(at(8, 0.5f));
            r.setVelocityDecay(at(9, 0));
            r.setSwing(at(10, 0));
            r.setGlide(at(11, 0));
            r.setThreshold(at(12, 0));
            r.setLatch(at(13, 0) > 0.5f);
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

        // Pad-pressure routes. A project with per-track lists: each track gets its own. An older
        // project with one list: every track whose engine uses a router gets that list (it never
        // reached the drum kits, which keep their default). Neither: each engine's default.
        auto toRouter = [](const AftertouchRouteData& list)
        {
            AftertouchRouter router;
            router.clear();
            for (int i = 0; i < AftertouchRouter::MAX_ROUTES && i < (int) list.routes.size(); ++i)
            {
                const auto& e = list.routes[(size_t) i];
                router.setRoute(i, { static_cast<AftertouchRouter::Destination>(juce::jlimit(0, AftertouchRouter::DEST_COUNT - 1, e.destination)),
                                     e.depth, e.bipolar });
            }
            return router;
        };

        const bool hasGlobal = !data.aftertouchRouting.routes.empty();
        if (live.pressure != nullptr && hasGlobal)
            *live.pressure = toRouter(data.aftertouchRouting);
        live.audio.adoptLegacyRouter();         // what was just loaded is not an edit to copy to every track

        for (int t = 0; t < trackCount; ++t)
        {
            if (t < (int) data.trackAftertouch.size())
                live.audio.setPressureRouter(t, toRouter(data.trackAftertouch[(size_t) t]));
            else if (hasGlobal && live.audio.getEngineTypeOf(t) != EngineType::Drum)
                live.audio.setPressureRouter(t, toRouter(data.aftertouchRouting));
            else
                live.audio.resetPressureRouter(t);
        }
    }
}
