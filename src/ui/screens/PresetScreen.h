#pragma once

#include "../ScreenManager.h"
#include "../ScreenLayout.h"
#include "../NoteUtils.h"
#include "../../sequencer/SequencerCore.h"
#include "../../engine/AudioEngine.h"
#include "../../project/PresetManager.h"
#include "../../project/ProjectBridge.h"
#include <set>

// -----------------------------------------------------------------------------
// PRESET: the preset browser. Bar: PS CT EN AU LD SV DL VL.
//
// Audition is the normal state (AU is its off switch and starts ON): as soon as the highlighted
// preset changes - knob 1, Up / Down, a category or engine-filter change, a deleted preset - that
// preset takes over the selected track's sound, for listening only. The sound the track had when
// the screen was entered (or when the track was selected, or after LD) is remembered and comes
// back when the screen is left by any route, when another track is selected, or when AU is
// switched off. LD (four clicks) keeps the highlighted preset: nothing is put back after that.
// Entering the screen changes nothing until the highlight moves.
//
// A preset of the track's own engine is applied to the running engine (no new engine, held notes
// keep sounding); only a preset of another engine type builds a new engine.
// A project saved while a preset is auditioned stores the remembered sound (keepOriginalInSave).
// -----------------------------------------------------------------------------
class PresetScreen : public Screen
{
public:
    PresetScreen(SequencerCore& seq, AudioEngine* audio, PresetManager& mgr)
        : sequencer(seq), audioEngine(audio), presetManager(mgr)
    {
        rebuildFilteredList();
    }

    juce::String getName() const override { return "PRESET"; }

    juce::String getSubPage() const override
    {
        if (flash.isNotEmpty())
            return flash;
        auto cats = PresetManager::getCategories();
        juce::String sub = (categoryIndex >= 0 && categoryIndex < cats.size()) ? cats[categoryIndex] : juce::String("ALL");
        // ANY: presets of any engine. AUD: a preset is being heard for listening only (the track's
        // own sound returns unless LD keeps it). AU-: audition is switched off.
        if (anyEngine) sub += " ANY";
        if (!audition)       sub += " AU-";
        else if (auditioned) sub += " AUD";
        return sub;
    }

    // Entering the screen remembers the track's sound and changes nothing: the highlighted preset
    // takes over only once the highlight moves.
    void onActivate() override
    {
        takeSnapshot();
        rebuildFilteredList();
        shownKey = highlightedKey();
    }

    // Leaving by any route gives the track its own sound back, unless LD kept a preset.
    void onDeactivate() override
    {
        restoreSnapshot();
        releaseAllPads();
        confirm.cancel();
        flash.clear();
    }

    // The clock used by the LD / SV / DL confirmation (tests inject their own).
    void setClock(std::function<juce::int64()> c) { clock = std::move(c); }

    // ---- Pads play the selected track: 16 semitones up from the base note, with velocity and
    // per-pad pressure, through the same callbacks the SCALE pads use. Pads never load or save.
    using NoteCallback = std::function<void(int midiNote, int velocity)>;
    void setNoteCallback(NoteCallback cb) { noteCallback = std::move(cb); }
    void setNoteOffCallback(std::function<void(int midiNote)> cb) { noteOffCallback = std::move(cb); }
    void setAftertouchCallback(std::function<void(int midiNote, int pressure)> cb) { aftertouchCallback = std::move(cb); }
    // The note of the first pad (the C of the track's current octave); middle C without a provider.
    void setBaseNoteProvider(std::function<int()> p) { baseNoteProvider = std::move(p); }
    int getBaseNote() const { return juce::jlimit(0, 127 - 15, baseNoteProvider ? baseNoteProvider() : 60); }

    bool isAuditionOn() const { return audition; }
    bool isAuditioning() const { return auditioned; }
    int getSelectedIndex() const { return selectedIndex; }
    int getListSize() const { return (int) filteredIndices.size(); }

    // A project captured while a preset is auditioned must hold the track's own sound, not the
    // one being listened to: this puts the remembered sound into the captured data. Call it on
    // the result of ProjectBridge::capture, holding ScreenManager::getLock().
    void keepOriginalInSave(ProjectData& data) const
    {
        if (!auditioned || snapshotIsStale())
            return;
        if (data.activePattern < 0 || data.activePattern >= (int) data.patterns.size())
            return;
        auto& tracks = data.patterns[(size_t) data.activePattern].tracks;
        if (snapshotTrack < 0 || snapshotTrack >= (int) tracks.size())
            return;

        auto& td = tracks[(size_t) snapshotTrack];
        td.engineType = snapshotType;
        td.soundParams = snapshotSaved;
        td.sampleName.clear();
        if (snapshotType == EngineType::Sampler || snapshotType == EngineType::Wavetable)
        {
            auto it = td.soundParams.find(ProjectBridge::SAMPLE_INDEX_PARAM);
            if (it != td.soundParams.end())
                if (const auto* e = SampleBank::get().entry((int) std::lround(it->second)))
                    td.sampleName = juce::String(e->name);
        }

        if (snapshotTrack < (int) data.trackAftertouch.size())
        {
            AftertouchRouteData routes;
            for (int i = 0; i < AftertouchRouter::MAX_ROUTES; ++i)
            {
                auto r = snapshotRoutes.effectiveRoute(i);
                routes.routes.push_back({ (int) r.dest, r.depth, r.bipolar });
            }
            data.trackAftertouch[(size_t) snapshotTrack] = routes;
        }
    }

    void render(DisplayRenderer& display) override
    {
        // Another track was selected (Shift + Up / Down, hold Up / Down + pad): the picture is
        // drawn straight after, so this is where the old track gets its sound back at once.
        followTrack();

        auto cats = PresetManager::getCategories();
        int catCount = (int) cats.size();

        // Category tabs across the top of content zone
        int tabY = 12;
        int tabW = (catCount > 0) ? juce::jmin(128 / catCount, 24) : 16;
        for (int i = 0; i < catCount && i * tabW < 128; ++i)
        {
            int tx = i * tabW;
            // Nine categories share 128 px: two letters fit a 14 px tab without running into the next.
            juce::String label = cats[i].substring(0, tabW >= 19 ? 3 : 2).toUpperCase();
            int textW = label.length() * 6;
            int textX = tx + (tabW - textW) / 2;
            display.drawString(textX, tabY, label);
            if (i == categoryIndex)
                display.invertRegion(tx, tabY - 1, tabW, 9);
        }
        display.drawHLine(0, tabY + 9, 128);

        // Preset list fills remaining space (y=22 to y=63)
        if (filteredIndices.empty())
        {
            display.drawString(4, 36, "NO PRESETS");
            return;
        }

        constexpr int lineHeight = 9;
        constexpr int startY = 23;
        constexpr int maxVisible = 3;      // the knob names take the bottom line

        int scrollOffset = 0;
        if (selectedIndex >= maxVisible)
            scrollOffset = selectedIndex - maxVisible + 1;

        for (int i = 0; i < maxVisible; ++i)
        {
            int listIdx = scrollOffset + i;
            if (listIdx >= (int) filteredIndices.size())
                break;

            int y = startY + i * lineHeight + (i * 1);
            auto& preset = presetManager.getPreset(filteredIndices[(size_t) listIdx]);

            auto eng = NoteUtils::engineAbbrev((int)preset.engineType);
            display.drawString(2, y, eng);
            display.drawString(18, y, preset.name.substring(0, 15));

            if (preset.isFactory)
                display.drawChar(119, y, '*');       // clear of the scroll bar at 126

            if (listIdx == selectedIndex)
                display.invertRegion(0, y - 1, 128, lineHeight);
        }

        ScreenLayout::drawScrollBar(display, 126, startY, 29, (int) filteredIndices.size(), maxVisible, scrollOffset);
    }

    void onEncoderChange(int encoderIndex, int value) override
    {
        const int clicks = (value > 64) ? value - 128 : value;
        if (clicks == 0) return;
        flash.clear();
        followTrack();

        // LD, SV and DL act after four clicks clockwise on the same knob, spread over at least
        // 300 ms, so a brushed knob does nothing. Any other knob cancels.
        const bool isAction = encoderIndex >= 4 && encoderIndex <= 6;
        if (!isAction)
            confirm.cancel();
        else if (!confirm.step(encoderIndex, clicks, now()))
        {
            if (confirm.steps > 0)
                flash = juce::String(actionName(encoderIndex)) + " " + confirm.progress();
            return;
        }

        switch (encoderIndex)
        {
            case 0:   // PS - scroll the list
            {
                const int count = (int) filteredIndices.size();
                if (count == 0) return;
                const ParamSpec spec { "PS", 0, 0, (float) (count - 1), 0, ParamKind::Int };
                scrollTo((int) spec.turned((float) selectedIndex, clicks, turnStep()));
                break;
            }
            case 1:   // CT - category
            {
                auto cats = PresetManager::getCategories();
                const ParamSpec spec { "CT", 0, 0, (float) juce::jmax(0, cats.size() - 1), 0, ParamKind::Int };
                categoryIndex = (int) spec.turned((float) categoryIndex, clicks, turnStep());
                rebuildFilteredList();
                syncAudition();
                break;
            }
            case 2:   // EN - this track's engine only, or presets of any engine
                anyEngine = clicks > 0;
                rebuildFilteredList();
                syncAudition();
                break;
            case 3:   // AU - the off switch for audition (on unless switched off)
            {
                const bool on = clicks > 0;
                if (on == audition) break;
                audition = on;
                if (!audition)
                    restoreSnapshot();
                else
                {
                    shownKey.clear();       // the highlighted preset is heard at once
                    syncAudition();
                }
                break;
            }
            case 4:   // LD - the highlighted preset becomes the track's sound
                flash = keepSelectedPreset() ? "LOAD OK" : "LOAD FAIL";
                break;
            case 5:   // SV
                saveCurrentTrackAsPreset();
                flash = "SAVED";
                break;
            case 6:   // DL - user presets only
                flash = deleteSelectedPreset() ? "DELETED" : "FACTORY";
                syncAudition();             // the highlight is on another preset now
                break;
            case 7:   // VL - the track's volume, to compare presets at matched level
                if (audioEngine)
                {
                    int t = sequencer.getSelectedTrack();
                    float v = volumeSpec().turned(audioEngine->getTrackParameter(t, MixParam::VOLUME), clicks, turnStep());
                    audioEngine->setTrackParameter(t, MixParam::VOLUME, v);
                    sequencer.getPattern().getTrack(t).soundParams[MixParam::VOLUME] = v;
                    snapshotStored[MixParam::VOLUME] = v;        // the level set here stays
                    snapshotSaved[MixParam::VOLUME] = v;
                }
                break;
            default: break;
        }
    }

    // Up / Down scroll the list (and audition), like knob 1.
    void onPadPagePrev() override { flash.clear(); confirm.cancel(); followTrack(); scrollTo(selectedIndex - 1); }
    void onPadPageNext() override { flash.clear(); confirm.cancel(); followTrack(); scrollTo(selectedIndex + 1); }
    void getHelpFacts(HelpFacts& facts) const override { facts.tapUpDown = "PRESET LIST"; }

    bool usesEncoderBar() const override { return true; }

    juce::String getEncoderName(int encoderIndex) const override
    {
        static const char* const names[] = { "PS", "CT", "EN", "AU", "LD", "SV", "DL", "VL" };
        return (encoderIndex >= 0 && encoderIndex < 8) ? names[encoderIndex] : "";
    }

    juce::String getEncoderReadout(int encoderIndex) const override
    {
        switch (encoderIndex)
        {
            case 0:
                if (filteredIndices.empty() || selectedIndex >= (int) filteredIndices.size()) return "PRESET --";
                return "PRESET " + presetManager.getPreset(filteredIndices[(size_t) selectedIndex]).name;
            case 1:
            {
                auto cats = PresetManager::getCategories();
                return "CATEGORY " + ((categoryIndex >= 0 && categoryIndex < cats.size()) ? cats[categoryIndex] : juce::String("ALL"));
            }
            case 2: return anyEngine ? "ENGINE ANY" : "ENGINE THIS TRACK";
            case 3: return audition ? "AUDITION ON" : "AUDITION OFF";
            case 4: case 5: case 6:
                return juce::String(actionName(encoderIndex)) + (confirm.knob == encoderIndex ? " " + confirm.progress() : juce::String());
            case 7:
                return "VOLUME " + (audioEngine ? volumeSpec().format(audioEngine->getTrackParameter(sequencer.getSelectedTrack(), MixParam::VOLUME))
                                                : juce::String("--"));
            default: return {};
        }
    }

    // Called after a user preset is saved or deleted (the app writes them to disk).
    std::function<void()> onUserPresetsChanged;

    void onPadPress(int padIndex, int velocity) override
    {
        if (padIndex < 0 || padIndex >= 16) return;
        const int note = getBaseNote() + padIndex;
        soundingNote[(size_t) padIndex] = note;
        if (noteCallback) noteCallback(note, velocity);
    }

    void onPadAftertouch(int padIndex, int pressure) override
    {
        if (padIndex < 0 || padIndex >= 16) return;
        const int note = soundingNote[(size_t) padIndex];
        if (aftertouchCallback && note >= 0) aftertouchCallback(note, pressure);
    }

    void onPadRelease(int padIndex) override
    {
        if (padIndex < 0 || padIndex >= 16) return;
        const int note = soundingNote[(size_t) padIndex];
        soundingNote[(size_t) padIndex] = -1;
        if (noteOffCallback && note >= 0) noteOffCallback(note);
    }

    // All 16 pads play: the Cs marked, a held pad bright.
    bool getPadColours(std::array<PadColour, 16>& out) const override
    {
        const int base = getBaseNote();
        for (int i = 0; i < 16; ++i)
        {
            PadColour c = ((base + i) % 12 == 0) ? PadColour { 100, 40, 0 } : PadColour { 0, 30, 40 };
            if (soundingNote[(size_t) i] >= 0) c = { 127, 127, 127 };
            out[(size_t) i] = c;
        }
        return true;
    }

    using PresetLoadCallback = std::function<void(int trackIndex, EngineType newType)>;
    void setPresetLoadCallback(PresetLoadCallback cb) { loadCallback = std::move(cb); }

private:
    juce::int64 now() const { return clock ? clock() : UiClock::nowMs(); }
    static ParamSpec volumeSpec() { return { "VOL", 0, 0, 1, MixParam::DEFAULT_VOLUME, ParamKind::Linear }; }
    static const char* actionName(int knob) { return knob == 4 ? "LOAD" : (knob == 5 ? "SAVE" : "DELETE"); }

    void scrollTo(int index)
    {
        const int count = (int) filteredIndices.size();
        if (count == 0) return;
        selectedIndex = juce::jlimit(0, count - 1, index);
        syncAudition();
    }

    void releaseAllPads()
    {
        for (int i = 0; i < 16; ++i)
            onPadRelease(i);
    }

    // ---- Which preset is highlighted (by name, so it survives the list being rebuilt) ----
    static juce::String keyOf(const Preset& p)
    {
        return juce::String((int) p.engineType) + "/" + p.category + "/" + p.name;
    }

    juce::String highlightedKey() const
    {
        if (filteredIndices.empty() || selectedIndex < 0 || selectedIndex >= (int) filteredIndices.size())
            return {};
        return keyOf(presetManager.getPreset(filteredIndices[(size_t) selectedIndex]));
    }

    void selectByKey(const juce::String& key)
    {
        for (int i = 0; i < (int) filteredIndices.size(); ++i)
            if (keyOf(presetManager.getPreset(filteredIndices[(size_t) i])) == key)
            {
                selectedIndex = i;
                return;
            }
    }

    // The highlight is on another preset than before (scroll, category, engine filter, delete):
    // that preset takes over the track's sound. An empty list gives the track's own sound back.
    void syncAudition()
    {
        const auto key = highlightedKey();
        if (key == shownKey) return;
        shownKey = key;
        if (!audition) return;
        if (key.isEmpty())
        {
            restoreSnapshot();
            return;
        }
        applyPreset(presetManager.getPreset(filteredIndices[(size_t) selectedIndex]));
        auditioned = true;
        heardKey = key;
    }

    // ---- The remembered sound ----
    template <typename Fn>
    static void forEachEngineId(Fn&& fn)
    {
        for (int id = 0; id < EngineParamRange::FIRST_END; ++id) fn(id);
        for (int id = EngineParamRange::EXT_BASE; id < EngineParamRange::EXT_END; ++id) fn(id);
        for (int id = EngineParamRange::THIRD_BASE; id < EngineParamRange::THIRD_END; ++id) fn(id);
    }

    static ParamMap readEngine(const EngineBase& engine)
    {
        ParamMap values;
        forEachEngineId([&](int id) { values[id] = engine.getParameter(id); });
        return values;
    }

    // Parameters some step of the track locks: while the pattern plays, the engine may hold a
    // lock's value at this moment, and the mixer keeps the value to go back to.
    static std::set<int> lockedIds(const Track& track)
    {
        std::set<int> ids;
        for (auto& step : track.steps)
            for (auto& lock : step.paramLocks)
                ids.insert(lock.first);
        return ids;
    }

    // Someone else rebuilt the track since the sound was remembered (a project was loaded): what
    // is remembered belongs to a sound that no longer exists and must not be put back.
    bool snapshotIsStale() const
    {
        if (snapshotTrack < 0 || snapshotTrack >= sequencer.getPattern().getTrackCount())
            return true;
        return audioEngine != nullptr && audioEngine->getEngine(snapshotTrack) != ownEngine;
    }

    // The selected track changed while the screen is open (or its sound was replaced from
    // outside): the old track gets its sound back and the new one is remembered.
    void followTrack()
    {
        if (sequencer.getSelectedTrack() == snapshotTrack && !snapshotIsStale()) return;
        restoreSnapshot();
        takeSnapshot();
        rebuildFilteredList();
        shownKey = highlightedKey();
    }

    void takeSnapshot()
    {
        snapshotTrack = sequencer.getSelectedTrack();
        auto& track = sequencer.getPattern().getTrack(snapshotTrack);
        snapshotType = track.engineType;
        snapshotStored = track.soundParams;
        snapshotSaved = track.soundParams;
        snapshotEngine.clear();
        ownEngine = nullptr;
        if (audioEngine)
        {
            snapshotRoutes = audioEngine->getPressureRouter(snapshotTrack);
            // What a project save would store for this track (engine, mixer, effects)...
            snapshotSaved = ProjectBridge::captureTrackSound(ProjectBridge::Live { sequencer, *audioEngine },
                                                             snapshotTrack, track.soundParams);
            ownEngine = audioEngine->getEngine(snapshotTrack);
            if (ownEngine != nullptr)
            {
                // ...and every value of the engine, also those on no page.
                snapshotType = ownEngine->getType();
                snapshotEngine = readEngine(*ownEngine);
            }
            // A step lock may be holding a parameter away from its own value right now: the
            // stored value is the sound.
            for (int id : lockedIds(track))
            {
                auto it = track.soundParams.find(id);
                if (it == track.soundParams.end()) continue;
                snapshotSaved[id] = it->second;
                if (snapshotEngine.count(id) != 0) snapshotEngine[id] = it->second;
            }
        }
        auditioned = false;
        heardKey.clear();
    }

    // Move the running engine to `target` (every engine id), setting only what differs: no new
    // engine, sounding notes carry on. The engine's master parameter goes first (it loads values
    // into others); a further pass picks up anything a parameter reset on being set.
    void driveEngineTo(int trackIdx, const ParamMap& target, const std::set<int>& always)
    {
        auto* live = audioEngine->getEngine(trackIdx);
        if (live == nullptr) return;
        const int master = live->getMasterParamId();
        for (int pass = 0; pass < 3; ++pass)
        {
            bool changed = false;
            auto put = [&](int id, float value)
            {
                // A locked parameter is always set, so that the value it returns to is the new one.
                if (live->getParameter(id) == value && !(pass == 0 && always.count(id) != 0)) return;
                audioEngine->setTrackParameter(trackIdx, id, value);
                changed = true;
            };
            if (auto it = target.find(master); master >= 0 && it != target.end())
                put(master, it->second);
            for (auto& [id, value] : target)
                if (id != master)
                    put(id, value);
            if (!changed) break;
        }
    }

    // Mixer, effect blocks, insert parameters: set what differs.
    void applyTrackLevelValues(int trackIdx, const ParamMap& values)
    {
        for (auto& [id, value] : values)
            if (!EngineParamRange::contains(id) && !RangeParam::isRangeParam(id)
                && audioEngine->getTrackParameter(trackIdx, id) != value)
                audioEngine->setTrackParameter(trackIdx, id, value);
    }

    // Put back the sound the track had before anything was auditioned.
    void restoreSnapshot()
    {
        if (!auditioned)
            return;
        auditioned = false;
        heardKey.clear();
        if (snapshotIsStale())
            return;

        auto& track = sequencer.getPattern().getTrack(snapshotTrack);
        const bool engineChanged = track.engineType != snapshotType;
        if (audioEngine)
        {
            const auto locked = lockedIds(track);
            // Only a sound of another engine type needs a new engine.
            if (audioEngine->getEngineTypeOf(snapshotTrack) != snapshotType)
                audioEngine->setEngineType(snapshotTrack, snapshotType, true);
            track.engineType = snapshotType;
            track.soundParams = snapshotStored;
            driveEngineTo(snapshotTrack, snapshotEngine, locked);
            applyTrackLevelValues(snapshotTrack, snapshotSaved);
            audioEngine->setPressureRouter(snapshotTrack, snapshotRoutes);
            ownEngine = audioEngine->getEngine(snapshotTrack);
        }
        else
        {
            track.engineType = snapshotType;
            track.soundParams = snapshotStored;
        }
        if (engineChanged && loadCallback)
            loadCallback(snapshotTrack, snapshotType);
        rebuildFilteredList();
    }

    // While a preset of another engine is auditioned, "this track's engine" is still the engine
    // of the track's own sound.
    EngineType listEngineType() const
    {
        if (auditioned && !snapshotIsStale())
            return snapshotType;
        return sequencer.getPattern().getTrack(sequencer.getSelectedTrack()).engineType;
    }

    void rebuildFilteredList()
    {
        auto engineType = listEngineType();
        auto cats = PresetManager::getCategories();
        juce::String cat = (categoryIndex > 0 && categoryIndex < cats.size())
                               ? cats[categoryIndex] : "";
        if (anyEngine)
        {
            filteredIndices.clear();
            for (int i = 0; i < presetManager.getPresetCount(); ++i)
                if (cat.isEmpty() || cat == "ALL" || presetManager.getPreset(i).category == cat)
                    filteredIndices.push_back(i);
        }
        else
            filteredIndices = presetManager.getFilteredIndices(engineType, cat);
        selectedIndex = juce::jlimit(0, juce::jmax(0, (int) filteredIndices.size() - 1), selectedIndex);
    }

    // The preset becomes what the selected track plays: a fresh engine of the preset's type with
    // the preset's values, on top of the remembered sound's mixer and effect values (so one
    // preset never leaves anything behind in the next). The remembered sound is not touched.
    void applyPreset(const Preset& preset)
    {
        const int trackIdx = sequencer.getSelectedTrack();
        auto& track = sequencer.getPattern().getTrack(trackIdx);
        const bool engineChanged = (track.engineType != preset.engineType);

        if (audioEngine == nullptr)
        {
            PresetManager::applyToTrack(preset, track);
            return;
        }

        // Stored parameters: the remembered sound's without its engine values, plus the preset's.
        ParamMap stored = snapshotStored;
        for (auto it = stored.begin(); it != stored.end();)
        {
            const bool engineParam = EngineParamRange::contains(it->first);
            const bool engineRange = RangeParam::isRangeParam(it->first)
                                     && EngineParamRange::contains(RangeParam::targetOf(it->first));
            it = (engineParam || engineRange) ? stored.erase(it) : std::next(it);
        }
        ParamMap trackLevel = snapshotSaved;
        for (auto& [id, value] : preset.soundParams)
        {
            stored[id] = value;
            trackLevel[id] = value;
        }

        // What a fresh engine holds after the preset: worked out on an engine that is not playing.
        auto reference = AudioEngine::createEngine(preset.engineType);
        const int master = reference->getMasterParamId();
        if (auto it = preset.soundParams.find(master); master >= 0 && it != preset.soundParams.end())
            reference->setParameter(master, it->second);
        for (auto& [id, value] : preset.soundParams)
            if (id != master && EngineParamRange::contains(id))
                reference->setParameter(id, value);

        // A preset of the engine already running is applied to it; another type needs a new engine
        // (built under AudioEngine::structure() by setEngineType; its notes end with the old one).
        const bool newEngine = audioEngine->getEngine(trackIdx) == nullptr
                               || audioEngine->getEngineTypeOf(trackIdx) != preset.engineType;
        const auto locked = lockedIds(track);
        if (newEngine)
            audioEngine->setEngineType(trackIdx, preset.engineType, true);
        track.engineType = preset.engineType;
        track.soundParams = stored;
        driveEngineTo(trackIdx, readEngine(*reference), locked);
        applyTrackLevelValues(trackIdx, trackLevel);
        // Back on the remembered sound's engine type: its pressure routes, not the type's defaults.
        if (newEngine && preset.engineType == snapshotType)
            audioEngine->setPressureRouter(trackIdx, snapshotRoutes);
        ownEngine = audioEngine->getEngine(trackIdx);

        if (engineChanged && loadCallback)
            loadCallback(trackIdx, preset.engineType);
    }

    // LD: the highlighted preset is the track's sound from now on. If it is the one being heard
    // it simply stays; nothing is put back when the screen is left.
    bool keepSelectedPreset()
    {
        const auto key = highlightedKey();
        if (key.isEmpty())
            return false;
        if (!(auditioned && heardKey == key))
            applyPreset(presetManager.getPreset(filteredIndices[(size_t) selectedIndex]));
        takeSnapshot();
        rebuildFilteredList();
        selectByKey(key);
        shownKey = highlightedKey();
        return true;
    }

    void saveCurrentTrackAsPreset()
    {
        int trackIdx = sequencer.getSelectedTrack();
        auto& track = sequencer.getPattern().getTrack(trackIdx);

        int userCount = 0;
        for (int i = 0; i < presetManager.getPresetCount(); ++i)
            if (!presetManager.getPreset(i).isFactory)
                ++userCount;

        // "User N" with the first N not already taken (names are also the file names on disk).
        auto taken = [this](const juce::String& n)
        {
            for (int i = 0; i < presetManager.getPresetCount(); ++i)
                if (presetManager.getPreset(i).name == n) return true;
            return false;
        };
        int number = userCount + 1;
        while (taken("User " + juce::String(number))) ++number;
        juce::String name = "User " + juce::String(number);
        auto preset = PresetManager::fromTrack(track, name, "INIT");
        presetManager.addPreset(preset);
        rebuildFilteredList();
        if (onUserPresetsChanged) onUserPresetsChanged();
    }

    bool deleteSelectedPreset()
    {
        if (filteredIndices.empty() || selectedIndex >= (int) filteredIndices.size())
            return false;
        int index = filteredIndices[(size_t) selectedIndex];
        if (presetManager.getPreset(index).isFactory)
            return false;
        presetManager.removePreset(index);
        rebuildFilteredList();
        if (onUserPresetsChanged) onUserPresetsChanged();
        return true;
    }

    SequencerCore& sequencer;
    AudioEngine* audioEngine;
    PresetManager& presetManager;
    std::vector<int> filteredIndices;
    int selectedIndex = 0;
    int categoryIndex = 0;
    bool anyEngine = false;
    bool audition = true;               // AU: on unless switched off
    bool auditioned = false;            // a preset is being heard for listening only
    juce::String shownKey;              // the preset the highlight was on when last looked at
    juce::String heardKey;              // the preset being heard
    // The remembered sound of `snapshotTrack`.
    int snapshotTrack = -1;
    EngineType snapshotType = EngineType::UnifiedVoice;
    ParamMap snapshotStored;            // Track::soundParams as it was
    ParamMap snapshotSaved;             // what a project save stores for the track
    ParamMap snapshotEngine;            // every engine value
    AftertouchRouter snapshotRoutes;
    const EngineBase* ownEngine = nullptr;      // the track's engine as this screen last left it
    TurnConfirm confirm;
    std::function<juce::int64()> clock;
    NoteCallback noteCallback;
    std::function<void(int)> noteOffCallback;
    std::function<void(int, int)> aftertouchCallback;
    std::function<int()> baseNoteProvider;
    std::array<int, 16> soundingNote = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    juce::String flash;
    PresetLoadCallback loadCallback;
};
