#pragma once

#include <map>
#include "../ScreenManager.h"
#include "../ScreenLayout.h"
#include "../NoteUtils.h"
#include "../BitmapFont.h"
#include "../TurnNotice.h"
#include "../GrammarHooks.h"
#include "../../sequencer/SequencerCore.h"
#include "../../engine/AudioEngine.h"
#include "../../engine/UnifiedVoiceEngine.h"
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
//
// MULT, the last category, lists the multi presets (MultiPreset.h) and nothing else, whatever EN
// says. A multi is never auditioned: moving onto one gives the track its own sound back, and the
// name bar says where the multi would land ("3TRK>T7-9": the earliest empty tracks, EmptyTracks)
// or that there is no room ("NEED 3 EMPTY"). LD loads every member - engine, sound, starter pattern
// into the active pattern, preset name, routings turned from member numbers into track numbers -
// as ONE undo step, and selects the track of the member the keys should play.
// SV on a track that listens to another track (its input, a DUCK insert) or that another track
// listens to saves all the connected tracks as one multi; any other track saves a single preset.
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
        if (multiListed())
            return landingText();
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

    // ---- Pads play the selected track: the notes of the scale chosen on SCALE, with velocity and
    // per-pad pressure, through the same callbacks the SCALE pads use. Pads never load or save.
    using NoteCallback = std::function<void(int midiNote, int velocity)>;
    void setNoteCallback(NoteCallback cb) { noteCallback = std::move(cb); }
    void setNoteOffCallback(std::function<void(int midiNote)> cb) { noteOffCallback = std::move(cb); }
    void setAftertouchCallback(std::function<void(int midiNote, int pressure)> cb) { aftertouchCallback = std::move(cb); }
    // The note of the first pad (the C of the track's current octave); middle C without a provider.
    void setBaseNoteProvider(std::function<int()> p) { baseNoteProvider = std::move(p); }
    int getBaseNote() const { return juce::jlimit(0, 127 - 15, baseNoteProvider ? baseNoteProvider() : 60); }

    // Each track's arpeggiator: saved with a preset, taken over by a preset that has one, and put
    // back with the track's own sound.
    void setArpProvider(std::function<Arpeggiator*(int track)> p) { arpForTrack = std::move(p); }
    // The keyboard (split and octave): saved with a preset, set by a preset that has one, and put
    // back with the track's own sound.
    void setKeyboardProvider(std::function<KeyboardSetup()> get, std::function<void(const KeyboardSetup&)> set)
    {
        getKeyboard = std::move(get);
        applyKeyboard = std::move(set);
    }

    // The pads' notes come from the scale chosen on SCALE (its page and root), like the SCALE
    // screen's own pads, when the app provides them: `note(pad)` and the root to mark. Without
    // them: 16 semitones up from the base note.
    void setPadScale(std::function<int(int pad)> note, std::function<int()> root)
    {
        padScaleNote = std::move(note);
        padScaleRoot = std::move(root);
    }
    int getPadNote(int padIndex) const
    {
        if (padScaleNote)
            if (const int n = padScaleNote(padIndex); n >= 0)
                return juce::jlimit(0, 127, n);
        return getBaseNote() + padIndex;
    }

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
        if (!snapshotArp.empty() && snapshotTrack < (int) data.arps.size())
            data.arps[(size_t) snapshotTrack] = snapshotArp;
        if (getKeyboard && keyboardSet)
            data.keyboard = snapshotKeyboard;
        td.presetName = trackPresets.nameFor(snapshotTrack, snapshotType);
        td.archMemory = snapshotArchMem;
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

    // The category row: where each tab stands and what it says. All the categories are on the row
    // at once (ten of them: nine tabs of two letters were all the row held, and MULT, the tenth,
    // was off its right end until CT reached it). A tab says as many letters as tell it from the
    // others: one where no other category begins with it, else two (ALL INIT BASS PAD LEAD KEYS FX
    // PERC KIT MULT read "A I B PA L KE FX PE KI M"; a name of two letters is said whole). The
    // name bar says the chosen category in full. The tabs are spread over the row with what room
    // there is between them, at least two blank columns besides each tab's own margin; if a list
    // ever has more than fits so, the tabs keep to two columns between them and run off the right.
    struct Tab { int x = 0, width = 0; juce::String label; };
    static std::vector<Tab> tabLayout(const juce::StringArray& cats)
    {
        std::vector<Tab> tabs;
        int used = 0;
        for (auto& category : cats)
        {
            const auto name = category.toUpperCase();
            int sameStart = 0;
            for (auto& other : cats)
                if (other.toUpperCase().startsWith(name.substring(0, 1)))
                    ++sameStart;
            Tab tab;
            tab.label = name.substring(0, (name.length() <= 2 || sameStart > 1) ? 2 : 1);
            tab.width = tab.label.length() * BitmapFont::TOTAL_CHAR_WIDTH + 1;     // (a blank column each side of the letters)
            used += tab.width;
            tabs.push_back(tab);
        }
        const int gaps = juce::jmax(1, (int) tabs.size() - 1);
        const float gap = juce::jmax(2.0f, (float) (DisplayRenderer::WIDTH - used) / (float) gaps);
        float x = 0.0f;
        for (auto& tab : tabs)
        {
            tab.x = (int) std::lround(x);
            x += (float) tab.width + gap;
        }
        return tabs;
    }

    void render(DisplayRenderer& display) override
    {
        // Another track was selected (Shift + Up / Down, hold Up / Down + pad): the picture is
        // drawn straight after, so this is where the old track gets its sound back at once.
        followTrack();

        // Category tabs across the top of the content zone: every category at once (tabLayout: laid
        // out when the list of categories is another than the one the tabs were made for), the
        // chosen one inverted.
        constexpr int tabY = 12;
        if (const auto cats = PresetManager::getCategories(); cats != tabCategories)
        {
            tabCategories = cats;
            tabs = tabLayout(cats);
        }
        for (int cat = 0; cat < (int) tabs.size(); ++cat)
        {
            const auto& tab = tabs[(size_t) cat];
            display.drawString(tab.x + 1, tabY, tab.label);
            if (cat == categoryIndex)
                display.invertRegion(tab.x, tabY - 1, tab.width, 9);
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
            if (multiListed())
            {
                // A multi: how many tracks it takes, where a preset shows its engine.
                auto& multi = presetManager.getMulti(filteredIndices[(size_t) listIdx]);
                display.drawString(2, y, juce::String(multi.memberCount()) + (multi.memberCount() < 10 ? "T" : ""));
                display.drawString(18, y, multi.name.substring(0, 15));
                if (multi.isFactory)
                    display.drawChar(119, y, '*');
            }
            else
            {
                auto& preset = presetManager.getPreset(filteredIndices[(size_t) listIdx]);

                auto eng = NoteUtils::engineAbbrev((int)preset.engineType);
                display.drawString(2, y, eng);
                display.drawString(18, y, preset.name.substring(0, 15));

                if (preset.isFactory)
                    display.drawChar(119, y, '*');       // clear of the scroll bar at 126
            }

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
                if (multiListed())
                    loadHighlightedMulti();     // (says in the name bar where it went, or why not)
                else
                    flash = keepSelectedPreset() ? "LOAD OK" : "LOAD FAIL";
                break;
            case 5:   // SV - the track's sound; with the tracks it is connected to, as a multi
                flash = saveCurrentTrackAsPreset() ? "SAVED MULTI" : "SAVED";
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
                if (multiListed()) return landingText();        // (its name is in the list)
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

    // Called after SV with the name the preset was saved under ("User N"). The app asks for a
    // name on the Mac's keyboard and answers with renameUserPreset; the save never waits for it.
    std::function<void(const juce::String& savedName)> onPresetSaved;

    // Gives the user preset `from` the typed name. False (nothing changed) when there is no such
    // user preset, or the name is empty or in use. Call holding ScreenManager::getLock().
    bool renameUserPreset(const juce::String& from, const juce::String& typed)
    {
        if (savedIsMulti(from))
        {
            const auto highlighted = highlightedKey();
            const int multi = presetManager.findUserMulti(from);
            const auto oldKey = multiKey(presetManager.getMulti(multi));
            if (!presetManager.renameMulti(multi, typed))
                return false;
            multisChanged(highlighted == oldKey ? multiKey(presetManager.getMulti(multi)) : highlighted);
            if (onUserPresetsChanged) onUserPresetsChanged();
            return true;
        }
        const int index = presetManager.findUserPreset(from);
        if (index < 0)
            return false;
        const auto oldKey = keyOf(presetManager.getPreset(index));
        const auto highlighted = highlightedKey();
        if (!presetManager.renamePreset(index, typed))
            return false;

        // The highlight and the audition follow the preset under its new name: a rename is not
        // a move of the highlight and must not start or end an audition.
        const auto newKey = keyOf(presetManager.getPreset(index));
        rebuildFilteredList();
        selectByKey(highlighted == oldKey ? newKey : highlighted);
        if (shownKey == oldKey) shownKey = newKey;
        if (heardKey == oldKey) heardKey = newKey;
        if (onUserPresetsChanged) onUserPresetsChanged();
        return true;
    }

    // The preset each track's sound came from (LD, a named save, a loaded project or session).
    TrackPresetNames& getTrackPresets() { return trackPresets; }
    // The track SV last saved (-1 before any save).
    int getSavedTrack() const { return savedTrack; }

    // The name the Mac's box offers for the preset SV has just saved as `savedName`:
    // - the name of the preset this track's sound came from (Return then saves over that preset;
    //   if it has been deleted since, it is made again);
    // - a built-in preset cannot be saved over: its name with the first free number ("Deep Bass 2");
    // - a track whose sound came from no preset, or whose engine is another one since: `savedName`.
    juce::String suggestedName(const juce::String& savedName) const
    {
        if (savedIsMulti(savedName))
            return savedName;
        const int saved = presetManager.findUserPreset(savedName);
        const auto origin = trackPresets.get(savedTrack);
        if (origin.name.isEmpty() || saved < 0 || presetManager.getPreset(saved).engineType != origin.engine)
            return savedName;
        for (int i = 0; i < presetManager.getPresetCount(); ++i)
            if (presetManager.getPreset(i).isFactory && presetManager.getPreset(i).name.equalsIgnoreCase(origin.name))
                return freeVariation(origin.name);
        return origin.name;
    }

    // `name` with the first number from 2 that makes a name no preset has, within the name length.
    juce::String freeVariation(const juce::String& name) const
    {
        for (int n = 2; n < 1000; ++n)
        {
            const auto tail = " " + juce::String(n);
            const auto candidate = name.substring(0, PresetManager::MAX_NAME_LENGTH - tail.length()).trimEnd() + tail;
            if (!presetManager.isNameTaken(candidate))
                return candidate;
        }
        return name;
    }

    // What Return would do with `typed` in the box for the preset saved as `from`.
    enum class NameUse { Empty, BuiltIn, Replaces, New };
    NameUse nameUse(const juce::String& from, const juce::String& typed) const
    {
        const auto name = PresetManager::cleanName(typed);
        if (name.isEmpty())
            return NameUse::Empty;
        if (name.equalsIgnoreCase(from))
            return NameUse::New;
        NameUse use = NameUse::New;
        if (savedIsMulti(from))
        {
            // A multi's name is looked for among the multis (a single preset may have the same one).
            for (int i = 0; i < presetManager.getMultiCount(); ++i)
                if (presetManager.getMulti(i).name.equalsIgnoreCase(name))
                {
                    if (presetManager.getMulti(i).isFactory)
                        return NameUse::BuiltIn;
                    use = NameUse::Replaces;
                }
            return use;
        }
        for (int i = 0; i < presetManager.getPresetCount(); ++i)
            if (presetManager.getPreset(i).name.equalsIgnoreCase(name))
            {
                if (presetManager.getPreset(i).isFactory)
                    return NameUse::BuiltIn;
                use = NameUse::Replaces;
            }
        return use;
    }

    // The box was answered with a name: the preset saved as `from` gets it. A user preset that
    // already has that name is replaced (saving over it); a built-in preset's name is refused.
    // False = nothing changed. Call holding ScreenManager::getLock().
    bool nameSavedPreset(const juce::String& from, const juce::String& typed)
    {
        const auto name = PresetManager::cleanName(typed);
        if (savedIsMulti(from))
            return nameSavedMulti(from, name);
        if (name.isEmpty() || presetManager.findUserPreset(from) < 0)
            return false;
        const bool same = name.equalsIgnoreCase(from);      // (the name it has, in whatever letters: kept)
        if (!same)
        {
            int other = -1;
            for (int i = 0; i < presetManager.getPresetCount(); ++i)
                if (presetManager.getPreset(i).name.equalsIgnoreCase(name))
                {
                    if (presetManager.getPreset(i).isFactory)
                        return false;
                    other = i;
                }
            if (other >= 0)
            {
                // The highlight and the audition, if on the old one, move to its replacement.
                const auto oldKey = keyOf(presetManager.getPreset(other));
                const auto newKey = keyOf(presetManager.getPreset(presetManager.findUserPreset(from)));
                const auto highlighted = highlightedKey();
                presetManager.removePreset(other);
                rebuildFilteredList();
                selectByKey(highlighted == oldKey ? newKey : highlighted);
                if (shownKey == oldKey) shownKey = newKey;
                if (heardKey == oldKey) heardKey = newKey;
            }
            if (!renameUserPreset(from, name))
                return false;
        }
        const auto& named = presetManager.getPreset(presetManager.findUserPreset(same ? from : name));
        trackPresets.set(savedTrack, named.name, named.engineType);
        return true;
    }

    // The box was cancelled: the preset SV saved as `name` is taken away again.
    bool cancelSavedPreset(const juce::String& name)
    {
        if (savedIsMulti(name))
        {
            const auto highlighted = highlightedKey();
            presetManager.removeMulti(presetManager.findUserMulti(name));
            multisChanged(highlighted);
            if (onUserPresetsChanged) onUserPresetsChanged();
            return true;
        }
        const int index = presetManager.findUserPreset(name);
        if (index < 0)
            return false;
        const auto key = keyOf(presetManager.getPreset(index));
        const auto highlighted = highlightedKey();
        presetManager.removePreset(index);
        rebuildFilteredList();
        if (highlighted != key) selectByKey(highlighted);
        if (shownKey == key) shownKey = highlightedKey();
        if (onUserPresetsChanged) onUserPresetsChanged();
        return true;
    }

    void onPadPress(int padIndex, int velocity) override
    {
        if (padIndex < 0 || padIndex >= 16) return;
        const int note = getPadNote(padIndex);
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
        // The root of the scale (every octave of it) is marked; without a scale, the Cs.
        const int root = padScaleRoot ? padScaleRoot() : 0;
        for (int i = 0; i < 16; ++i)
        {
            PadColour c = ((getPadNote(i) - root) % 12 == 0) ? PadColour { 100, 40, 0 } : PadColour { 0, 30, 40 };
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

    static juce::String multiKey(const MultiPreset& m) { return "MULT/" + m.name; }

    // The key of row `i` of the list as it is shown (presets, or multis under MULT).
    juce::String keyAt(int i) const
    {
        if (i < 0 || i >= (int) filteredIndices.size())
            return {};
        const int index = filteredIndices[(size_t) i];
        return multiListed() ? multiKey(presetManager.getMulti(index)) : keyOf(presetManager.getPreset(index));
    }

    juce::String highlightedKey() const { return keyAt(selectedIndex); }

    void selectByKey(const juce::String& key)
    {
        for (int i = 0; i < (int) filteredIndices.size(); ++i)
            if (keyAt(i) == key)
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
        if (multiListed())
        {
            // A multi is not auditioned (it would take over several tracks): moving onto one ends
            // an audition as moving off a preset does.
            restoreSnapshot();
            return;
        }
        if (!audition) return;
        if (key.isEmpty())
        {
            restoreSnapshot();
            return;
        }
        applyPreset(presetManager.getPreset(filteredIndices[(size_t) selectedIndex]), sequencer.getSelectedTrack());
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
        for (int id = EngineParamRange::FOURTH_BASE; id < EngineParamRange::FOURTH_END; ++id) fn(id);
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
        snapshotArp.clear();
        if (auto* arp = arpOf(snapshotTrack))
            snapshotArp = ProjectBridge::captureArp(*arp);
        snapshotArchMem.clear();
        if (snapshotType == EngineType::UnifiedVoice && audioEngine)
            if (auto* uv = dynamic_cast<UnifiedVoiceEngine*>(audioEngine->getEngine(snapshotTrack)))
                snapshotArchMem = uv->captureArchMemory();
        snapshotKeyboard = getKeyboard ? getKeyboard() : KeyboardSetup();
        keyboardSet = false;
        ownEngine = nullptr;
        parkedEngine.reset();               // (a kept preset: the sound it replaced is gone)
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

    Arpeggiator* arpOf(int trackIdx) const { return arpForTrack ? arpForTrack(trackIdx) : nullptr; }

    // The track's arp takes the saved state, unless it is in it already (an arp that is running
    // is not restarted by scrolling between presets that share it).
    void setArp(int trackIdx, const std::vector<float>& state)
    {
        auto* arp = arpOf(trackIdx);
        if (arp == nullptr || state.empty() || ProjectBridge::captureArp(*arp) == state)
            return;
        ProjectBridge::applyArp(state, *arp);
    }

    // ---- The insert effects in a set of track values (ProjectBridge's ids) ----
    static bool isInsertType(int id)
    {
        return id >= ProjectBridge::INSERT_TYPE_BASE && id < ProjectBridge::INSERT_TYPE_BASE + AudioEngine::MAX_INSERTS_PER_TRACK;
    }
    static void eraseInsertValues(ParamMap& values)
    {
        for (auto it = values.begin(); it != values.end();)
        {
            const int id = it->first;
            const bool insert = isInsertType(id) || AudioEngine::isInsertSwitchParam(id) || AudioEngine::isInsertExtraParam(id)
                                || (id >= AudioEngine::EFFECT_PARAM_BASE && id < AudioEngine::EFFECT_PARAM_BASE + AudioEngine::MAX_INSERTS_PER_TRACK * 4);
            it = insert ? values.erase(it) : std::next(it);
        }
    }

    // The track's insert chain becomes the one `values` names (slot by slot until the first empty
    // one). Nothing is rebuilt when the effects are already those, so they keep ringing.
    void setInsertChain(int trackIdx, const ParamMap& values)
    {
        std::vector<EffectType> wanted;
        for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
        {
            auto it = values.find(ProjectBridge::INSERT_TYPE_BASE + slot);
            if (it == values.end() || it->second < 0.5f)
                break;
            wanted.push_back((EffectType) (juce::jlimit(1, (int) EffectType::NumEffectTypes, (int) std::lround(it->second)) - 1));
        }
        std::vector<EffectType> have;
        for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
        {
            EffectType type {};
            if (!audioEngine->getInsertEffectType(trackIdx, slot, type))
                break;
            have.push_back(type);
        }
        if (wanted == have)
            return;
        while (audioEngine->getInsertChain(trackIdx) != nullptr && audioEngine->getInsertChain(trackIdx)->getEffectCount() > 0)
            audioEngine->removeInsertEffect(trackIdx, 0);
        for (auto type : wanted)
            audioEngine->addInsertEffect(trackIdx, type);
    }

    // Mixer, effect blocks, insert parameters: set what differs.
    void applyTrackLevelValues(int trackIdx, const ParamMap& values)
    {
        for (auto& [id, value] : values)
            if (!EngineParamRange::contains(id) && !RangeParam::isRangeParam(id) && !isInsertType(id)
                && audioEngine->getTrackParameter(trackIdx, id) != value)
                audioEngine->setTrackParameter(trackIdx, id, value);
        // An effect's extra cells that the values do not name (a sound saved before the cell existed) are
        // at their starting values, not at what the slot's effect was left on.
        if (auto* chain = audioEngine->getInsertChain(trackIdx))
            for (int slot = 0; slot < chain->getEffectCount(); ++slot)
                if (auto* fx = chain->getEffect(slot))
                    for (int i = 0; i < EffectBase::MAX_EXTRAS; ++i)
                    {
                        const ParamSpec spec = fx->getExtraSpec(i);
                        const int id = AudioEngine::INSERT_EXTRA_BASE + slot * EffectBase::MAX_EXTRAS + i;
                        if (spec.isValid() && values.count(id) == 0 && fx->getExtra(i) != spec.defaultVal)
                            audioEngine->setTrackParameter(trackIdx, id, spec.defaultVal);
                    }
    }

    // Put back the sound the track had before anything was auditioned.
    void restoreSnapshot()
    {
        if (!auditioned)
            return;
        auditioned = false;
        heardKey.clear();
        if (snapshotIsStale())
        {
            parkedEngine.reset();
            return;
        }

        auto& track = sequencer.getPattern().getTrack(snapshotTrack);
        const bool engineChanged = track.engineType != snapshotType;
        if (audioEngine)
        {
            const auto locked = lockedIds(track);
            // The track's own engine was set aside untouched when the first preset was heard:
            // it comes back as it was, with whatever it holds that no parameter reaches.
            if (parkedEngine)
                audioEngine->unparkEngine(snapshotTrack, std::move(parkedEngine));
            else if (audioEngine->getEngineTypeOf(snapshotTrack) != snapshotType)
                audioEngine->setEngineType(snapshotTrack, snapshotType, true);
            parkedEngine.reset();
            track.engineType = snapshotType;
            track.soundParams = snapshotStored;
            // (every engine value, the pressure slots among them: where pad pressure goes is part of the sound)
            driveEngineTo(snapshotTrack, snapshotEngine, locked);
            setInsertChain(snapshotTrack, snapshotSaved);
            applyTrackLevelValues(snapshotTrack, snapshotSaved);
            setArp(snapshotTrack, snapshotArp);
            ownKeyboardFollowsHand();
            setKeyboard(snapshotKeyboard);
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

    // An octave button or the split gesture used while a preset is heard: what the player set by
    // hand is their keyboard from then on, and nothing puts the earlier one back.
    void ownKeyboardFollowsHand()
    {
        if (!getKeyboard || !keyboardSet) return;
        const auto now = getKeyboard();
        if (now.split != lastKeyboard.split)
            snapshotKeyboard.split = now.split;
        if (now.octave != lastKeyboard.octave && now.octave != KeyboardSetup::UNKNOWN && lastKeyboard.octave != KeyboardSetup::UNKNOWN)
            snapshotKeyboard.octave = now.octave;
        lastKeyboard = now;
    }

    void setKeyboard(const KeyboardSetup& k)
    {
        if (!applyKeyboard) return;
        applyKeyboard(k);
        if (getKeyboard)
        {
            lastKeyboard = getKeyboard();
            keyboardSet = true;
        }
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
        if (multiListed())
        {
            // Every multi, whatever the track's engine and whatever EN says.
            filteredIndices.clear();
            for (int i = 0; i < presetManager.getMultiCount(); ++i)
                filteredIndices.push_back(i);
        }
        else if (anyEngine)
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

    // The preset becomes what track `trackIdx` plays: a fresh engine of the preset's type with
    // the preset's values, on top of the track's mixer and effect values (so one preset never
    // leaves anything behind in the next).
    //
    // On the selected track (audition and LD of a single preset) "the track's values" are the
    // remembered sound's, which is not touched: the track's own engine is set aside on the first
    // audition and comes back on leaving. `asMember` (a member of a multi, loaded for good into
    // any track): the track's values are what it holds now, nothing is set aside, and the keyboard
    // is left alone. A member brings its input (TRK FX page IN) with it; a single preset never
    // does, and leaves the track's input as it is.
    void applyPreset(const Preset& preset, int trackIdx, bool asMember = false)
    {
        if (trackIdx < 0 || trackIdx >= sequencer.getPattern().getTrackCount())
            return;
        auto& track = sequencer.getPattern().getTrack(trackIdx);
        const bool engineChanged = (track.engineType != preset.engineType);

        if (audioEngine == nullptr)
        {
            // (no engine: the stored data is all there is)
            ParamMap input;
            if (!asMember)
                for (auto& [id, value] : track.soundParams)
                    if (TrackInParam::contains(id))
                        input[id] = value;
            PresetManager::applyToTrack(preset, track);
            if (!asMember)
            {
                MultiPreset::stripTrackInput(track.soundParams);
                for (auto& [id, value] : input)
                    track.soundParams[id] = value;
            }
            return;
        }

        // What the preset is laid over: the remembered sound (the selected track), or the track as
        // it is (a member of a multi).
        const bool remembered = !asMember && trackIdx == snapshotTrack;
        ParamMap baseStored = snapshotStored, baseSaved = snapshotSaved;
        std::vector<float> baseArp = snapshotArp;
        if (!remembered)
        {
            baseStored = track.soundParams;
            baseSaved = ProjectBridge::captureTrackSound(ProjectBridge::Live { sequencer, *audioEngine }, trackIdx, track.soundParams);
            baseArp.clear();
            if (auto* arp = arpOf(trackIdx))
                baseArp = ProjectBridge::captureArp(*arp);
        }

        // Stored parameters: the base's without its engine values, plus the preset's.
        ParamMap stored = std::move(baseStored);
        for (auto it = stored.begin(); it != stored.end();)
        {
            const bool engineParam = EngineParamRange::contains(it->first);
            const bool engineRange = RangeParam::isRangeParam(it->first)
                                     && EngineParamRange::contains(RangeParam::targetOf(it->first));
            it = (engineParam || engineRange) ? stored.erase(it) : std::next(it);
        }
        ParamMap trackLevel = std::move(baseSaved);
        // The preset's values, its sample found by name (its place in the bank may have moved).
        ParamMap sound = PresetManager::soundOf(preset);
        if (!asMember)
            MultiPreset::stripTrackInput(sound);
        // A preset is the whole of a track's sound: its effects take the place of the track's.
        // One that stores none (the built-in presets, user presets from before 2026-11-14) leaves
        // the track without insert effects (DIRT/TALK/MODFX are now insert types too).
        // The mixer (volume, pan, sends) is the track's and stays unless the preset stores it.
        eraseInsertValues(trackLevel);
        eraseInsertValues(stored);
        if (sound.count(ProjectBridge::INSERT_TYPE_BASE) == 0)
        {
            // Clear any old TrackFx params from the track level.
            for (int id = TrackFxParam::DIRT_BASE; id < TrackFxParam::END; ++id)
            {
                trackLevel[id] = 0.0f;
                stored.erase(id);
            }
        }
        for (auto& [id, value] : sound)
        {
            stored[id] = value;
            trackLevel[id] = value;
        }

        // Always a new engine, exactly as a load: an engine nudged over from another sound keeps
        // what no parameter reaches (the owner heard "the existing patch plus the preset"). The
        // track's own engine is set aside on the first audition and comes back on leaving.
        // (Where pad pressure goes is part of a sound: the preset's pressure slots, or - a preset
        // that stores none - the routes its engine is made with.)
        if (remembered && !auditioned && !parkedEngine)
            parkedEngine = audioEngine->parkEngine(trackIdx, preset.engineType);
        else
            audioEngine->setEngineType(trackIdx, preset.engineType, true);
        track.engineType = preset.engineType;
        track.soundParams = stored;
        ParamMap engineSound;
        for (auto& [id, value] : sound)
            if (EngineParamRange::contains(id))
                engineSound[id] = value;
        audioEngine->applyAllTrackParameters(trackIdx, engineSound);
        setInsertChain(trackIdx, trackLevel);
        applyTrackLevelValues(trackIdx, trackLevel);
        // The preset's arpeggiator, or (a preset without one) the track's own again.
        setArp(trackIdx, preset.arp.empty() ? baseArp : preset.arp);
        if (remembered)
        {
            // The preset's keyboard; what it does not store is the keyboard from before the audition.
            ownKeyboardFollowsHand();
            setKeyboard({ preset.keyboard.split >= 0 ? preset.keyboard.split : snapshotKeyboard.split,
                          preset.keyboard.octave != KeyboardSetup::UNKNOWN ? preset.keyboard.octave : snapshotKeyboard.octave });
            ownEngine = audioEngine->getEngine(trackIdx);
        }

        if (engineChanged && loadCallback && !asMember)
            loadCallback(trackIdx, preset.engineType);
    }

    // LD: the highlighted preset is the track's sound from now on. If it is the one being heard
    // it simply stays; nothing is put back when the screen is left.
    bool keepSelectedPreset()
    {
        const auto key = highlightedKey();
        if (key.isEmpty())
            return false;
        const auto& kept = presetManager.getPreset(filteredIndices[(size_t) selectedIndex]);
        if (!(auditioned && heardKey == key))
            applyPreset(kept, sequencer.getSelectedTrack());
        trackPresets.set(sequencer.getSelectedTrack(), kept.name, kept.engineType);
        takeSnapshot();
        rebuildFilteredList();
        selectByKey(key);
        shownKey = highlightedKey();
        return true;
    }

    // The first "User N" that neither a preset nor a multi has (names are also the file names).
    juce::String freeUserName() const
    {
        int userCount = 0;
        for (int i = 0; i < presetManager.getPresetCount(); ++i)
            if (!presetManager.getPreset(i).isFactory)
                ++userCount;
        auto taken = [this](const juce::String& n)
        {
            for (int i = 0; i < presetManager.getPresetCount(); ++i)
                if (presetManager.getPreset(i).name == n) return true;
            return presetManager.isMultiNameTaken(n);
        };
        int number = userCount + 1;
        while (taken("User " + juce::String(number))) ++number;
        return "User " + juce::String(number);
    }

    // Is track `t` the one whose own sound is remembered while a preset is heard on it?
    bool heardOn(int t) const { return auditioned && t == snapshotTrack && !snapshotIsStale(); }

    // Track `t`'s OWN sound as a preset, complete: every value as the engine, the mixer and the
    // effects hold it (not the stored parameters, which need not have them all), with the insert
    // effects themselves and the arpeggiator. While a preset is being auditioned on the track that
    // is the remembered sound, never the preset being listened to (the owner's saves came back as
    // the factory preset the highlight happened to be on).
    Preset presetOfTrack(int trackIdx, const juce::String& name) const
    {
        auto& track = sequencer.getPattern().getTrack(trackIdx);
        auto preset = PresetManager::fromTrack(track, name, "INIT");
        if (audioEngine == nullptr)
            return preset;
        if (heardOn(trackIdx))
        {
            preset.engineType = snapshotType;
            preset.soundParams = snapshotSaved;
            for (auto& [id, value] : snapshotEngine)
                if (preset.soundParams.count(id) != 0)
                    preset.soundParams[id] = value;
            preset.arp = snapshotArp;
        }
        else
        {
            preset.engineType = audioEngine->getEngineTypeOf(trackIdx);
            preset.soundParams = ProjectBridge::captureTrackSound(ProjectBridge::Live { sequencer, *audioEngine },
                                                                  trackIdx, track.soundParams);
            // A step lock may be holding a parameter away from its own value right now.
            for (int id : lockedIds(track))
                if (auto it = track.soundParams.find(id); it != track.soundParams.end())
                    preset.soundParams[id] = it->second;
            if (auto* arp = arpOf(trackIdx))
                preset.arp = ProjectBridge::captureArp(*arp);
        }
        // "This preset says what the track's effects are" (none, when slot 1 is 0).
        if (preset.soundParams.count(ProjectBridge::INSERT_TYPE_BASE) == 0)
            preset.soundParams[ProjectBridge::INSERT_TYPE_BASE] = 0.0f;
        preset.sampleName = PresetManager::sampleNameOf(preset.engineType, preset.soundParams);
        return preset;
    }

    // The tracks (0-based) that track `t` listens to: its input source and the KEY / SRC of every
    // keyed insert in its chain (DUCK, CMP, TRNS, WAH, every LFO effect, FilterEffect).
    // Read from the track's own sound, as SV would save it.
    std::vector<int> sourcesOfTrack(int t) const
    {
        const auto& track = sequencer.getPattern().getTrack(t);
        ParamMap sound = heardOn(t) ? snapshotSaved
                       : (audioEngine != nullptr ? ProjectBridge::captureTrackSound(ProjectBridge::Live { sequencer, *audioEngine }, t, track.soundParams)
                                                 : track.soundParams);
        // (the input is the engine's to say once it applies it; until then the stored value is it)
        if (audioEngine != nullptr && !heardOn(t))
            if (const float src = audioEngine->getTrackParameter(t, TrackInParam::SRC); src >= 0.5f)
                sound[TrackInParam::SRC] = src;
        std::vector<int> out;
        for (int number : MultiPreset::sourcesOf(sound))
            if (number - 1 != t && number - 1 < sequencer.getPattern().getTrackCount())
                out.push_back(number - 1);
        return out;
    }

public:
    // The tracks SV on `trackIdx` would save together, ascending: the track, every track it listens
    // to or that listens to it, and so on through those (the whole connected set). One track = an
    // ordinary single preset.
    std::vector<int> connectedTracks(int trackIdx) const
    {
        const int count = sequencer.getPattern().getTrackCount();
        if (trackIdx < 0 || trackIdx >= count)
            return {};
        std::vector<std::vector<int>> listensTo((size_t) count);
        for (int t = 0; t < count; ++t)
            listensTo[(size_t) t] = sourcesOfTrack(t);
        std::vector<bool> in((size_t) count, false);
        in[(size_t) trackIdx] = true;
        for (bool grew = true; grew;)
        {
            grew = false;
            for (int t = 0; t < count; ++t)
                for (int src : listensTo[(size_t) t])
                    if (in[(size_t) t] != in[(size_t) src])
                    {
                        in[(size_t) t] = true;
                        in[(size_t) src] = true;
                        grew = true;
                    }
        }
        std::vector<int> out;
        for (int t = 0; t < count; ++t)
            if (in[(size_t) t])
                out.push_back(t);
        // (more than a multi holds: the highest tracks other than the selected one are left out)
        for (int i = (int) out.size() - 1; i >= 0 && (int) out.size() > MultiPreset::MAX_MEMBERS; --i)
            if (out[(size_t) i] != trackIdx)
                out.erase(out.begin() + i);
        return out;
    }

    // The tracks the last SV saved (one for a single preset), for the save box's words.
    const std::vector<int>& getSavedTracks() const { return savedTracks; }

    // Loads a multi: see the class comment. False (and nothing changed, the name bar saying how
    // many empty tracks it needs) when there are fewer empty tracks than members.
    bool loadMulti(const MultiPreset& given)
    {
        const MultiPreset multi = given;        // (the list it came from may be rebuilt meanwhile)
        if (!multi.isValid())
            return false;
        restoreSnapshot();                      // (no audition goes on under a load)
        const auto tracks = emptyTracks.landing(sequencer, audioEngine, &trackPresets, multi.memberCount());
        if (tracks.empty())
        {
            flash = MultiLanding::needText(multi.memberCount());
            TurnNotice::post(flash);
            return false;
        }

        const int playTrack = tracks[(size_t) multi.playIndex()];
        {
            const UndoMark mark(*this, multiUndoPoint(tracks, sequencer.getActivePatternIndex()));
            for (int m = 0; m < multi.memberCount(); ++m)
            {
                const auto& member = multi.members[(size_t) m];
                const int t = tracks[(size_t) m];
                Preset preset = member.preset;
                MultiPreset::membersToTracks(preset.soundParams, tracks);
                applyPreset(preset, t, true);
                // One sound per track, in every pattern (as a project load and an undo leave it).
                for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                    if (p != sequencer.getActivePatternIndex() && t < sequencer.getPattern(p).getTrackCount())
                    {
                        sequencer.getPattern(p).getTrack(t).engineType = sequencer.getPattern().getTrack(t).engineType;
                        sequencer.getPattern(p).getTrack(t).soundParams = sequencer.getPattern().getTrack(t).soundParams;
                    }
                if (member.hasPattern)
                    writeStarterPattern(t, member);
                trackPresets.set(t, preset.name.isNotEmpty() ? preset.name : multi.name, preset.engineType);
            }
            sequencer.setSelectedTrack(playTrack);
            // The keyboard the play member was saved with, where it stores one.
            const auto& keys = multi.members[(size_t) multi.playIndex()].preset.keyboard;
            if (getKeyboard && applyKeyboard && (keys.split >= 0 || keys.octave != KeyboardSetup::UNKNOWN))
            {
                const auto now = getKeyboard();
                applyKeyboard({ keys.split >= 0 ? keys.split : now.split,
                                keys.octave != KeyboardSetup::UNKNOWN ? keys.octave : now.octave });
            }
        }
        afterTracksReplaced();
        flash = "OK " + MultiLanding::text(tracks, barRoom() - 3);
        if (loadCallback)
            loadCallback(playTrack, multi.members[(size_t) multi.playIndex()].preset.engineType);
        return true;
    }

    // Where the multi would land now (0-based tracks, ascending); empty = not enough empty tracks.
    std::vector<int> landingOf(const MultiPreset& multi) const
    {
        return emptyTracks.landing(sequencer, audioEngine, &trackPresets, multi.memberCount());
    }
    EmptyTracks& getEmptyTracks() const { return emptyTracks; }

private:
    bool multiListed() const
    {
        const auto cats = PresetManager::getCategories();
        return categoryIndex >= 0 && categoryIndex < cats.size() && cats[categoryIndex] == MultiPreset::categoryName();
    }

    const MultiPreset* highlightedMulti() const
    {
        if (!multiListed() || selectedIndex < 0 || selectedIndex >= (int) filteredIndices.size())
            return nullptr;
        return &presetManager.getMulti(filteredIndices[(size_t) selectedIndex]);
    }

    // How many characters the name bar has for this screen's text ("PRESET T3" and a space are taken).
    int barRoom() const
    {
        const int total = (DisplayRenderer::WIDTH - 4) / BitmapFont::TOTAL_CHAR_WIDTH;
        return total - (getName() + " T" + juce::String(sequencer.getSelectedTrack() + 1)).length() - 1;
    }

    // With a multi highlighted: where it would land, or that it has no room.
    juce::String landingText() const
    {
        const auto* multi = highlightedMulti();
        if (multi == nullptr)
            return MultiPreset::categoryName();
        const auto tracks = landingOf(*multi);
        return tracks.empty() ? MultiLanding::needText(multi->memberCount()) : MultiLanding::text(tracks, barRoom());
    }

    void loadHighlightedMulti()
    {
        if (const auto* multi = highlightedMulti())
        {
            const auto key = highlightedKey();
            loadMulti(*multi);
            selectByKey(key);
            shownKey = highlightedKey();
        }
        else
            flash = "LOAD FAIL";
    }

    // The list of multis changed (one named, replaced or taken away): the highlight stays on `key`
    // where that is still in the list. Nothing is auditioned under MULT, so nothing else follows.
    void multisChanged(const juce::String& key)
    {
        rebuildFilteredList();
        if (multiListed())
        {
            selectByKey(key);
            shownKey = highlightedKey();
        }
    }

    bool savedIsMulti(const juce::String& name) const { return presetManager.findUserMulti(name) >= 0; }

    // The audio thread is kept out while a track's steps are replaced (no engine: nothing to keep out).
    StructureLockFn structureLock() const { return audioEngine != nullptr ? GrammarHooks::structureLock(*audioEngine) : StructureLockFn(); }

    // The save box's answer for a multi saved as `from` (`name` is cleaned already): it gets the
    // name; a multi of the player's that has it is replaced, a built-in multi's name is refused.
    bool nameSavedMulti(const juce::String& from, const juce::String& name)
    {
        if (name.isEmpty())
            return false;
        if (name.equalsIgnoreCase(from))
            return true;                        // (the name it has: kept)
        for (int i = 0; i < presetManager.getMultiCount(); ++i)
            if (presetManager.getMulti(i).isFactory && presetManager.getMulti(i).name.equalsIgnoreCase(name))
                return false;
        const auto highlighted = highlightedKey();
        const auto oldKey = multiKey(presetManager.getMulti(presetManager.findUserMulti(from)));
        juce::String replacedKey;
        for (int i = presetManager.getMultiCount() - 1; i >= 0; --i)
            if (presetManager.getMulti(i).name.equalsIgnoreCase(name))
            {
                replacedKey = multiKey(presetManager.getMulti(i));
                presetManager.removeMulti(i);
            }
        const int index = presetManager.findUserMulti(from);
        if (!presetManager.renameMulti(index, name))
            return false;
        const auto newKey = multiKey(presetManager.getMulti(index));
        multisChanged(highlighted == oldKey || highlighted == replacedKey ? newKey : highlighted);
        if (onUserPresetsChanged) onUserPresetsChanged();
        return true;
    }

    // A member's starter pattern becomes the track's steps in the ACTIVE pattern (no other pattern
    // is touched). The audio thread is kept out while the steps are replaced.
    void writeStarterPattern(int trackIdx, const MultiPreset::Member& member)
    {
        auto write = [&]
        {
            auto& track = sequencer.getPattern().getTrack(trackIdx);
            track.clearAllSteps();
            track.setLength(member.length);
            for (int i = 0; i < (int) member.steps.size() && i < (int) track.steps.size(); ++i)
                ProjectBridge::applyStep(member.steps[(size_t) i], track.steps[(size_t) i]);
            // (a member of before the DIRT / TALK / MODFX blocks became inserts: its locks follow its sound)
            ProjectBridge::convertStepLocks(member.preset.soundParams, track);
        };
        underStructure(structureLock(), write);
    }

    // ---- One undo step for a whole multi load -----------------------------------------------------
    // Everything a load changes on the tracks it lands on, and who is selected. The undo history
    // swaps numbers (Scope::Custom): the number is a ticket for a state kept here, so that undo
    // puts back engine types, sounds (pressure slots included), steps, names and arpeggiators of every track at once,
    // and redo puts the loaded state back the same way.
    struct TrackState
    {
        EngineType type = EngineType::UnifiedVoice;
        ParamMap sound;
        std::vector<Step> steps;
        int length = 16;
        float speed = 1.0f;
        TrackPresetNames::Origin origin;
        std::vector<float> arp;
    };
    struct MultiState
    {
        std::vector<TrackState> tracks;
        int selected = 0;
        KeyboardSetup keyboard;
    };
    struct MultiStates
    {
        std::map<int, MultiState> kept;
        int next = 1;
    };

    MultiState captureMultiState(const std::vector<int>& tracks, int pattern) const
    {
        MultiState state;
        state.selected = sequencer.getSelectedTrack();
        if (getKeyboard)
            state.keyboard = getKeyboard();
        for (int t : tracks)
        {
            TrackState ts;
            const auto& track = sequencer.getPattern(pattern).getTrack(t);
            ts.type = audioEngine != nullptr ? audioEngine->getEngineTypeOf(t) : track.engineType;
            ts.sound = audioEngine != nullptr
                           ? ProjectBridge::captureTrackSound(ProjectBridge::Live { sequencer, *audioEngine }, t,
                                                              sequencer.getPattern().getTrack(t).soundParams)
                           : sequencer.getPattern().getTrack(t).soundParams;
            ts.steps = track.steps;
            ts.length = track.length;
            ts.speed = track.speedMultiplier;
            ts.origin = trackPresets.get(t);
            if (auto* arp = arpOf(t))
                ts.arp = ProjectBridge::captureArp(*arp);
            state.tracks.push_back(std::move(ts));
        }
        return state;
    }

    void applyMultiState(const MultiState& state, const std::vector<int>& tracks, int pattern)
    {
        for (size_t i = 0; i < tracks.size() && i < state.tracks.size(); ++i)
        {
            const int t = tracks[i];
            const auto& ts = state.tracks[i];
            if (t < 0 || t >= sequencer.getPattern().getTrackCount())
                continue;
            if (audioEngine != nullptr)
            {
                // The same steps as loading a project does for one track (a fresh engine, the insert
                // chain, every value); then what a sound does not hold.
                GrammarHooks::make(sequencer, *audioEngine).applySound(t, ts.type, ts.sound);
                if (auto* arp = arpOf(t); arp != nullptr && !ts.arp.empty() && ProjectBridge::captureArp(*arp) != ts.arp)
                    ProjectBridge::applyArp(ts.arp, *arp);
            }
            else
            {
                sequencer.getPattern().getTrack(t).engineType = ts.type;
                sequencer.getPattern().getTrack(t).soundParams = ts.sound;
            }
            auto put = [&]
            {
                auto& track = sequencer.getPattern(pattern).getTrack(t);
                track.steps = ts.steps;
                track.length = ts.length;
                track.speedMultiplier = ts.speed;
            };
            underStructure(structureLock(), put);
            trackPresets.set(t, ts.origin.name, ts.origin.engine);
        }
        sequencer.setSelectedTrack(state.selected);
        if (getKeyboard && applyKeyboard && getKeyboard() != state.keyboard)
            applyKeyboard(state.keyboard);
    }

    UndoPoint multiUndoPoint(const std::vector<int>& tracks, int pattern)
    {
        auto states = std::make_shared<MultiStates>();
        UndoPoint point;
        point.scope = UndoPoint::Scope::Custom;
        point.verb = "LOAD";
        point.names = { "MULTI " + MultiLanding::text(tracks, 8), "MULTI" };
        point.capture = [this, states, tracks, pattern]
        {
            restoreSnapshot();                  // (a preset being heard is not the track's sound)
            const int ticket = states->next++;
            states->kept[ticket] = captureMultiState(tracks, pattern);
            return std::vector<float> { (float) ticket };
        };
        point.apply = [this, states, tracks, pattern](const std::vector<float>& blob)
        {
            const int ticket = blob.empty() ? 0 : (int) blob[0];
            auto it = states->kept.find(ticket);
            if (it == states->kept.end())
                return;
            applyMultiState(it->second, tracks, pattern);
            states->kept.erase(it);
            afterTracksReplaced();
            if (loadCallback)
                loadCallback(sequencer.getSelectedTrack(), sequencer.getPattern().getTrack(sequencer.getSelectedTrack()).engineType);
        };
        return point;
    }

    // Tracks were rebuilt under the screen (a multi loaded, undone or redone): what is remembered
    // is the selected track as it is now, and nothing is being heard.
    void afterTracksReplaced()
    {
        const auto key = highlightedKey();
        auditioned = false;
        heardKey.clear();
        takeSnapshot();
        rebuildFilteredList();
        selectByKey(key);
        shownKey = highlightedKey();
    }

    // SV. True when a multi was saved (the track is connected to others), false for a single preset.
    bool saveCurrentTrackAsPreset()
    {
        const int trackIdx = sequencer.getSelectedTrack();
        const juce::String name = freeUserName();
        const auto set = connectedTracks(trackIdx);
        const bool asMulti = set.size() > 1;

        if (asMulti)
        {
            // The connected tracks in ascending order (sources stay ahead of their listeners where
            // they were), their routings by member number, the active pattern's steps as their
            // starter patterns; the keys play the track SV was turned on.
            MultiPreset multi;
            multi.name = name;
            for (size_t m = 0; m < set.size(); ++m)
            {
                const int t = set[m];
                MultiPreset::Member member;
                const auto origin = trackPresets.get(t);
                member.preset = presetOfTrack(t, {});
                member.preset.name = origin.name.isNotEmpty() && origin.engine == member.preset.engineType
                                         ? origin.name : "Part " + juce::String((int) m + 1);
                // (the input comes from the stored value until the engine applies it)
                MultiPreset::tracksToMembers(member.preset.soundParams, set);
                const auto& track = sequencer.getPattern().getTrack(t);
                member.hasPattern = true;
                member.length = track.length;
                for (int i = 0; i < track.length && i < (int) track.steps.size(); ++i)
                    member.steps.push_back(ProjectBridge::captureStep(track.steps[(size_t) i]));
                if (t == trackIdx)
                {
                    multi.playMember = (int) m + 1;
                    if (getKeyboard)
                    {
                        ownKeyboardFollowsHand();
                        member.preset.keyboard = heardOn(t) ? snapshotKeyboard : getKeyboard();
                    }
                }
                multi.members.push_back(std::move(member));
            }
            presetManager.addMulti(multi);
        }
        else
        {
            auto preset = presetOfTrack(trackIdx, name);
            // Which track feeds this one is not a single preset's to say.
            MultiPreset::stripTrackInput(preset.soundParams);
            // The keyboard as it is, or (a preset is being heard and may have moved it) as it was.
            if (getKeyboard)
            {
                ownKeyboardFollowsHand();
                preset.keyboard = heardOn(trackIdx) ? snapshotKeyboard : getKeyboard();
            }
            presetManager.addPreset(preset);
        }
        savedTrack = trackIdx;
        savedTracks = asMulti ? set : std::vector<int> { trackIdx };
        const auto key = highlightedKey();
        rebuildFilteredList();
        if (multiListed())
        {
            selectByKey(key);
            shownKey = highlightedKey();
        }
        if (onUserPresetsChanged) onUserPresetsChanged();
        if (onPresetSaved) onPresetSaved(name);
        return asMulti;
    }

    bool deleteSelectedPreset()
    {
        if (filteredIndices.empty() || selectedIndex >= (int) filteredIndices.size())
            return false;
        int index = filteredIndices[(size_t) selectedIndex];
        if (multiListed())
        {
            if (presetManager.getMulti(index).isFactory)
                return false;
            presetManager.removeMulti(index);
            rebuildFilteredList();
            shownKey = highlightedKey();        // (the highlight is on another multi: nothing is heard)
            if (onUserPresetsChanged) onUserPresetsChanged();
            return true;
        }
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
    juce::StringArray tabCategories;    // the categories `tabs` was laid out for
    std::vector<Tab> tabs;
    int selectedIndex = 0;
    int categoryIndex = 0;
    bool anyEngine = false;
    bool audition = true;               // AU: on unless switched off
    bool auditioned = false;            // a preset is being heard for listening only
    TrackPresetNames trackPresets;      // per track: the preset its sound was loaded from or saved as
    int savedTrack = -1;                // the track SV last saved
    std::vector<int> savedTracks;       // ... and every track that save took (several: a multi)
    mutable EmptyTracks emptyTracks;    // which tracks are empty (keeps each engine type's defaults)
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
    std::shared_ptr<EngineBase> parkedEngine;   // the track's own engine while a preset is heard
    std::vector<float> snapshotArp;             // the track's own arpeggiator
    std::vector<float> snapshotArchMem;         // the unified-voice ARCH memory
    KeyboardSetup snapshotKeyboard;             // the keyboard when the sound was remembered
    KeyboardSetup lastKeyboard;                 // the keyboard as an audition last left it
    bool keyboardSet = false;                   // an audition has set the keyboard since the snapshot
    std::function<Arpeggiator*(int)> arpForTrack;
    std::function<KeyboardSetup()> getKeyboard;
    std::function<void(const KeyboardSetup&)> applyKeyboard;
    TurnConfirm confirm;
    std::function<juce::int64()> clock;
    NoteCallback noteCallback;
    std::function<void(int)> noteOffCallback;
    std::function<void(int, int)> aftertouchCallback;
    std::function<int()> baseNoteProvider;
    std::function<int(int)> padScaleNote;
    std::function<int()> padScaleRoot;
    std::array<int, 16> soundingNote = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    juce::String flash;
    PresetLoadCallback loadCallback;
};
