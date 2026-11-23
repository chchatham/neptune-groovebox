#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>
#include "help_card_tests.h"
#include "../src/project/FactoryPresets.h"
#include "../src/project/MultiPreset.h"
#include "../src/project/FactoryMultis.h"
#include "../src/project/PresetManager.h"
#include "../src/project/ProjectManager.h"
#include "../src/project/ProjectBridge.h"
#include "../src/ui/screens/PresetScreen.h"

// Multi presets (MultiPreset.h): several tracks that belong together, loaded into the earliest
// empty tracks with their routings turned from member numbers into track numbers, as one undo step.
// Driven as the app is, on the help-card rig: the app's screens on a real AudioEngine, the
// controller's own messages into ScreenManager. A new rig is a new project (track 2 a drum kit).
namespace multitest
{
    using namespace helptest;
    using Reason = EmptyTracks::Reason;

    struct MRig : Rig
    {
        PresetScreen* screen = nullptr;

        MRig()
        {
            // (as the app's new project: track 2 is the drum kit in every pattern)
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                seq.getPattern(p).getTrack(1).engineType = EngineType::Drum;
            mgr.setActiveScreen(PRESET);
            screen = dynamic_cast<PresetScreen*>(mgr.getActiveScreen());
        }

        TrackPresetNames& names() { return screen->getTrackPresets(); }
        Reason reason(int t) { return screen->getEmptyTracks().reasonFor(seq, &audio, &names(), t); }
        std::vector<int> empty() { return screen->getEmptyTracks().find(seq, &audio, &names()); }

        // The whole live state as a project file would hold it, the tracks' preset names with it.
        juce::String state()
        {
            ProjectBridge::Live live { seq, audio };
            live.presetNames = &names();
            return juce::JSON::toString(ProjectBridge::capture(live, "multi").toVar());
        }

        void undo() { shift(true); func(true); tap(PLAY);   func(false); shift(false); }
        void redo() { shift(true); func(true); tap(RECORD); func(false); shift(false); }
        int undos() const { return mgr.getUndoHistory().undoCount(); }

        // LD / SV / DL: four clicks on the knob, spread over more than 300 ms.
        void confirm(int knob)
        {
            for (int i = 0; i < 4; ++i)
            {
                turn(knob, 1);
                UiClock::advance(120);
            }
        }

        // CT turned to its last category, MULT.
        void toMult()
        {
            for (int i = 0; i < 12; ++i)
                turn(1, 1);
            UiClock::advance(ScreenManager::READOUT_MS + 50);        // (the knob's readout leaves the name bar)
        }
        void select(int t)
        {
            seq.setSelectedTrack(t);
            tick();
        }

        // Trigs on these tracks (in the active pattern): they are no longer empty.
        void occupy(std::initializer_list<int> tracks)
        {
            for (int t : tracks)
                track(t).getStep(2).trig = true;
        }

        // What the name bar says on the right.
        juce::String barText()
        {
            UiClock::advance(ScreenManager::READOUT_MS + 50);
            tick();
            return text();
        }
        bool barFits()
        {
            const auto bar = mgr.composeNameBar();
            const int chars = bar.left.length() + 1 + bar.text.length() + (bar.tags.isNotEmpty() ? bar.tags.length() + 1 : 0);
            return chars * BitmapFont::TOTAL_CHAR_WIDTH <= DisplayRenderer::WIDTH - 4;
        }

        juce::String trigs(int pattern, int t)
        {
            juce::String out;
            auto& tr = seq.getPattern(pattern).getTrack(t);
            for (int i = 0; i < (int) tr.steps.size(); ++i)
                if (tr.steps[(size_t) i].trig)
                    out << i << " ";
            return out.trim();
        }

        float stored(int t, int id)
        {
            auto& params = track(t).soundParams;
            auto it = params.find(id);
            return it != params.end() ? it->second : -999.0f;
        }

        // `seconds` of the whole instrument: its RMS, and whether every sample was a number.
        float render(double seconds, bool* finite = nullptr)
        {
            std::vector<float> l(256), r(256);
            double sum = 0.0;
            int count = 0;
            for (int done = 0; done < (int) (seconds * 44100.0); done += 256)
            {
                audio.renderBlock(l.data(), r.data(), 256);
                for (int i = 0; i < 256; ++i)
                {
                    if (finite != nullptr && !(std::isfinite(l[(size_t) i]) && std::isfinite(r[(size_t) i])))
                        *finite = false;
                    sum += (double) l[(size_t) i] * l[(size_t) i] + (double) r[(size_t) i] * r[(size_t) i];
                    ++count;
                }
            }
            return count > 0 ? (float) std::sqrt(sum / (2.0 * count)) : 0.0f;
        }
    };

    inline const MultiPreset& duckPair(const PresetManager& mgr)
    {
        for (int i = 0; i < mgr.getMultiCount(); ++i)
            if (mgr.getMulti(i).name == "Duck Pair")
                return mgr.getMulti(i);
        return mgr.getMulti(0);
    }

    // A place for this suite's files, inside the build tree.
    inline juce::File scratchFolder()
    {
        return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory()
                   .getChildFile("multi_preset_tests_tmp");
    }
}

class MultiPresetTests : public juce::UnitTest
{
public:
    MultiPresetTests() : UnitTest("Multi Presets") {}

    // Two states of the instrument (project JSON) are the same; otherwise the first line that differs.
    void expectSame(const juce::String& now, const juce::String& wanted, const juce::String& what = {})
    {
        if (now == wanted)
        {
            expect(true);
            return;
        }
        const auto a = juce::StringArray::fromLines(now), b = juce::StringArray::fromLines(wanted);
        int i = 0;
        while (i < a.size() && i < b.size() && a[i] == b[i])
            ++i;
        juce::String around;
        for (int k = juce::jmax(0, i - 6); k < i; ++k)
            around << a[k].trim() << " ";
        expect(false, what + ": line " + juce::String(i) + " is [" + a[i].trim() + "], wanted [" + b[i].trim() + "] after " + around);
    }

    void runTest() override
    {
        using namespace multitest;
        const int DUCK_TYPE = ProjectBridge::INSERT_TYPE_BASE;          // slot 1's effect type
        const int DUCK_SRC = AudioEngine::EFFECT_PARAM_BASE;            // slot 1's first cell
        const float DUCK = (float) ((int) EffectType::SidechainEffect + 1);

        beginTest("The factory list holds single-track presets only");
        {
            for (auto& preset : FactoryPresets::getAll())
            {
                expect((int) preset.engineType >= 0 && (int) preset.engineType < ENGINE_TYPE_COUNT, preset.name);
                expect(preset.category != MultiPreset::categoryName(), preset.name);
            }
            PresetManager mgr;
            for (int i = 0; i < mgr.getPresetCount(); ++i)
                expect(mgr.getPreset(i).name != "Duck Pair");
            expect(mgr.getMultiCount() >= 1);
            expect(PresetManager::getCategories()[PresetManager::getCategories().size() - 1] == juce::String("MULT"),
                   "MULT is the last category");
        }

        beginTest("A multi goes to JSON and back unchanged");
        {
            auto multi = FactoryMultis::duckPair();
            multi.members[0].steps[4].paramLocks[3] = 0.25f;
            multi.members[1].preset.arp = { 1.0f, 2.0f, 3.0f };
            multi.members[1].preset.keyboard.octave = -1;
            multi.members[1].preset.soundParams[TrackInParam::SRC] = 1.0f;
            const auto json = multi.toJSON();
            auto back = MultiPreset::fromJSON(json);
            expect(back.isValid());
            expectEquals(back.toJSON(), json);
            expectEquals(back.name, juce::String("Duck Pair"));
            expectEquals(back.category, juce::String("MULT"));
            expectEquals(back.version, 1);
            expectEquals(back.playMember, 2);
            expectEquals(back.memberCount(), 2);
            if (back.memberCount() == 2)
            {
                expect(back.members[0].preset.engineType == EngineType::Drum);
                expect(back.members[0].hasPattern);
                expectEquals(back.members[0].length, 16);
                expectEquals((int) back.members[0].steps.size(), 16);
                expect(back.members[0].steps[4].trig && back.members[0].steps[4].notes.size() == 1
                       && back.members[0].steps[4].notes[0].pitch == 8);
                expectEquals(back.members[0].steps[4].paramLocks[3], 0.25f);
                expect(!back.members[0].steps[5].trig);
                expectEquals((int) back.members[1].steps[0].notes.size(), 4);
                expectEquals(back.members[1].steps[0].length, 16.0f);
                expectEquals(back.members[1].preset.soundParams[DUCK_SRC], 1.0f);
                expectEquals(back.members[1].preset.soundParams[TrackInParam::SRC], 1.0f);
                expectEquals(back.members[1].preset.keyboard.octave, -1);
                expectEquals((int) back.members[1].preset.arp.size(), 3);
            }

            // A member without a starter pattern stays without one.
            auto bare = multi;
            bare.members[0].hasPattern = false;
            bare.members[0].steps.clear();
            expect(!MultiPreset::fromJSON(bare.toJSON()).members[0].hasPattern);

            // What a file may hold: no members, too many, a play member that is not there, a wrong engine.
            expect(!MultiPreset::fromJSON("{\"name\":\"x\",\"members\":[]}").isValid());
            expect(!MultiPreset::fromJSON("not json").isValid());
            auto odd = multi;
            odd.playMember = 99;
            odd.members[0].preset.engineType = (EngineType) 77;
            for (int i = 0; i < 30; ++i)
                odd.members.push_back(odd.members[1]);
            const auto held = MultiPreset::fromJSON(odd.toJSON());
            expectEquals(held.memberCount(), (int) MultiPreset::MAX_MEMBERS);
            expectEquals(held.playMember, (int) MultiPreset::MAX_MEMBERS);
            expect(held.members[0].preset.engineType == EngineType::UnifiedVoice);
        }

        beginTest("A multi's file is never loaded as a single preset, nor a single preset as a multi");
        {
            // A single preset's JSON is not a multi; a multi's JSON gives no usable single preset list entry.
            Preset single;
            single.name = "Lone";
            single.category = "BASS";
            single.soundParams[10] = 900.0f;
            expect(!MultiPreset::isMultiVar(juce::JSON::parse(single.toJSON())));
            expect(!MultiPreset::fromJSON(single.toJSON()).isValid());
            expect(MultiPreset::isMultiVar(juce::JSON::parse(FactoryMultis::duckPair().toJSON())));

            const auto dir = scratchFolder();
            dir.deleteRecursively();
            {
                PresetManager mgr;
                const int factoryPresets = mgr.getPresetCount(), factoryMultis = mgr.getMultiCount();
                mgr.addPreset(single);
                auto tricky = single;
                tricky.name = "Pair.multi";             // (its file must not be a multi's)
                mgr.addPreset(tricky);
                auto mine = FactoryMultis::duckPair();
                mine.name = "Pair";
                mine.isFactory = false;
                mgr.addMulti(mine);
                mgr.saveUserPresetsToDirectory(dir);

                expect(dir.getChildFile("Lone.json").existsAsFile());
                expect(dir.getChildFile("Pair.multi.json").existsAsFile(), "a user multi is <name>.multi.json");
                expect(!dir.getChildFile("Duck_Pair.multi.json").existsAsFile(), "built-in multis are not written");
                expectEquals(dir.findChildFiles(juce::File::findFiles, false, "*.json").size(), 3);

                PresetManager fresh;
                fresh.loadFromDirectory(dir);
                expectEquals(fresh.getPresetCount(), factoryPresets + 2);
                expectEquals(fresh.getMultiCount(), factoryMultis + 1);
                expect(fresh.findUserPreset("Lone") >= 0 && fresh.findUserPreset("Pair.multi") >= 0);
                expect(fresh.findUserPreset("Pair") < 0, "the multi is not among the presets");
                const int at = fresh.findUserMulti("Pair");
                expect(at >= 0);
                if (at >= 0)
                {
                    expectEquals(fresh.getMulti(at).memberCount(), 2);
                    expect(!fresh.getMulti(at).isFactory);
                    auto expected = mine;
                    expectEquals(fresh.getMulti(at).toJSON(), expected.toJSON());
                }
                expect(fresh.findUserMulti("Lone") < 0);

                // A file that calls itself built-in is the player's all the same: a built-in multi is
                // never written, so the next rewrite of the folder would have deleted it for good.
                auto claims = mine;
                claims.name = "Claims";
                claims.isFactory = true;
                dir.getChildFile("Claims.multi.json").replaceWithText(claims.toJSON());
                PresetManager again;
                again.loadFromDirectory(dir);
                expect(again.findUserMulti("Claims") >= 0, "a multi from a file is a user multi");
            }
            dir.deleteRecursively();
        }

        beginTest("Routing by member number: to track numbers at load and back at save");
        {
            ParamMap sound;
            sound[TrackInParam::SRC] = 2.0f;                            // the input: member 2
            sound[DUCK_TYPE] = (float) ((int) EffectType::DelayEffect + 1);
            sound[DUCK_SRC] = 3.0f;                                     // a delay's first cell: not a track
            sound[DUCK_TYPE + 1] = DUCK;
            sound[DUCK_SRC + 4] = 1.0f;                                 // DUCK in slot 2: member 1
            sound[DUCK_TYPE + 2] = DUCK;
            sound[DUCK_SRC + 8] = 5.0f;                                 // DUCK in slot 3: no such member
            sound[DUCK_TYPE + 4] = DUCK;                                // (slot 4 is empty: the chain ends there)
            sound[DUCK_SRC + 16] = 2.0f;

            auto loaded = sound;
            MultiPreset::membersToTracks(loaded, { 6, 7 });             // members 1, 2 landed on tracks 7, 8
            expectEquals(loaded[TrackInParam::SRC], 8.0f);
            expectEquals(loaded[DUCK_SRC], 3.0f, "another effect's cell is left alone");
            expectEquals(loaded[DUCK_SRC + 4], 7.0f);
            expectEquals(loaded[DUCK_SRC + 8], 0.0f, "a member that is not there: SELF");
            expectEquals(loaded[DUCK_SRC + 16], 2.0f, "past the end of the chain");

            auto nonAdjacent = sound;
            MultiPreset::membersToTracks(nonAdjacent, { 2, 7 });
            expectEquals(nonAdjacent[TrackInParam::SRC], 8.0f);
            expectEquals(nonAdjacent[DUCK_SRC + 4], 3.0f);

            auto saved = loaded;
            saved[DUCK_SRC + 8] = 2.0f;                                 // listens to track 2: outside the set
            MultiPreset::tracksToMembers(saved, { 6, 7 });
            expectEquals(saved[TrackInParam::SRC], 2.0f);
            expectEquals(saved[DUCK_SRC + 4], 1.0f);
            expectEquals(saved[DUCK_SRC + 8], 0.0f, "a track outside the multi: SELF");

            ParamMap outside;
            outside[TrackInParam::SRC] = 4.0f;
            MultiPreset::tracksToMembers(outside, { 6, 7 });
            expectEquals(outside[TrackInParam::SRC], 0.0f, "an input from outside the multi: none");
            ParamMap broken;
            broken[TrackInParam::SRC] = std::numeric_limits<float>::quiet_NaN();
            MultiPreset::membersToTracks(broken, { 0, 1 });
            expectEquals(broken[TrackInParam::SRC], 0.0f);

            expect(MultiPreset::sourcesOf(sound) == std::vector<int> ({ 2, 1, 5 }));

            ParamMap single = sound;
            single[TrackInParam::LEVEL] = 0.5f;
            MultiPreset::stripTrackInput(single);
            for (auto& [id, value] : single)
            {
                juce::ignoreUnused(value);
                expect(!TrackInParam::contains(id));
            }
            expectEquals(single[DUCK_SRC + 4], 1.0f);
        }

        beginTest("An empty track, case by case");
        {
            MRig r;
            for (int t = 0; t < 8; ++t)
                expect(r.reason(t) == Reason::Empty, "a new project: track " + juce::String(t + 1) + " is empty");
            expect(r.audio.getEngineTypeOf(1) == EngineType::Drum, "(track 2 is the drum kit, and empty)");
            expect(r.reason(8) == Reason::NoTrack && r.reason(-1) == Reason::NoTrack);

            // A changed engine type alone leaves the track empty, whatever the type.
            for (int type = 0; type < ENGINE_TYPE_COUNT; ++type)
            {
                r.audio.setEngineType(3, (EngineType) type);
                r.track(3).engineType = (EngineType) type;
                expect(r.reason(3) == Reason::Empty, "engine type " + juce::String(type) + " as it comes is empty");
            }
            r.audio.setEngineType(3, EngineType::UnifiedVoice);
            r.track(3).engineType = EngineType::UnifiedVoice;

            // A trig, in any pattern of the bank.
            r.seq.getPattern(11).getTrack(3).getStep(7).trig = true;
            expect(r.reason(3) == Reason::Trigs, "a trig in another pattern");
            r.seq.getPattern(11).getTrack(3).getStep(7).trig = false;
            r.track(3).getStep(0).trig = true;
            expect(r.reason(3) == Reason::Trigs);
            r.track(3).getStep(0).trig = false;
            expect(r.reason(3) == Reason::Empty);

            // A preset name (for the engine the track has now).
            r.names().set(3, "Deep Bass", EngineType::UnifiedVoice);
            expect(r.reason(3) == Reason::Named);
            r.names().set(3, "FM Bell", EngineType::FM);
            expect(r.reason(3) == Reason::Empty, "a name for an engine the track no longer has is no name");
            r.names().clear(3);

            // The sound: asked of the engine, so a turned knob counts though nothing was stored.
            ParamSpec cell;
            for (auto& page : r.audio.getEngine(3)->getParamPages())
                for (auto& p : page.params)
                    if (!cell.isValid() && p.isValid() && p.kind == ParamKind::Linear && p.maxVal > p.minVal)
                        cell = p;
            const float was = r.audio.getTrackParameter(3, cell.id);
            r.audio.setTrackParameter(3, cell.id, was > cell.minVal ? cell.minVal : cell.maxVal);
            expect(r.track(3).soundParams.empty());
            expect(r.reason(3) == Reason::Sound, "a turned knob");
            r.audio.setTrackParameter(3, cell.id, was);
            expect(r.reason(3) == Reason::Empty, "turned back");

            // Effects: an insert.
            r.audio.addInsertEffect(3, EffectType::DelayEffect);
            expect(r.reason(3) == Reason::Effects, "an insert effect");
            r.audio.removeInsertEffect(3, 0);
            expect(r.reason(3) == Reason::Empty);
            // (TrackFx block params are dead since DIRT/TALK/MODFX became inserts.)

            // An input.
            r.track(3).soundParams[TrackInParam::SRC] = 2.0f;
            expect(r.reason(3) == Reason::Input);
            r.track(3).soundParams.erase(TrackInParam::SRC);

            // The mixer does not count: a preset load leaves it alone.
            r.audio.setTrackParameter(3, MixParam::VOLUME, 0.3f);
            r.audio.setTrackParameter(3, MixParam::PAN, -0.5f);
            expect(r.reason(3) == Reason::Empty, "volume and pan");

            // A project that was saved and loaded holds every value of every track: still empty.
            {
                ProjectBridge::Live live { r.seq, r.audio };
                const auto data = ProjectBridge::capture(live, "p");
                ProjectBridge::apply(ProjectData::fromVar(data.toVar()), live);
                expect(!r.track(4).soundParams.empty(), "(the stored parameters are full after a load)");
                for (int t = 0; t < 8; ++t)
                    expect(r.reason(t) == Reason::Empty, "after save and load: track " + juce::String(t + 1));
            }

            // A cleared sound (Func + Play on PARAM) is an untouched sound again.
            r.select(4);
            r.audio.setTrackParameter(4, cell.id, was > cell.minVal ? cell.minVal : cell.maxVal);
            expect(r.reason(4) == Reason::Sound);
            r.mgr.setActiveScreen(PARAM);
            r.func(true); r.tap(PLAY); r.func(false);
            expect(r.reason(4) == Reason::Empty, "after CLEAR of the sound");
            ProjectBridge::Live live { r.seq, r.audio };
            expect(EmptyTracks::isClearedSound({ { MixParam::VOLUME, 0.5f }, { TrackFxParam::DIRT_BASE, 0.0f } }));
            expect(!EmptyTracks::isClearedSound(ProjectBridge::captureTrackSound(live, 4, r.track(4).soundParams)));
        }

        beginTest("Where a multi lands: the earliest empty tracks, and what the name bar says");
        {
            MRig r;
            const auto& duck = duckPair(r.presets);
            expect(r.screen->landingOf(duck) == std::vector<int> ({ 0, 1 }), "a new project: tracks 1 and 2");

            r.toMult();
            expectEquals(r.screen->getListSize(), r.presets.getMultiCount());
            expect(!r.screen->isAuditioning());
            expectEquals(r.barText(), juce::String("2TRK>T1-2"));
            expect(r.barFits());

            r.occupy({ 0, 1, 2, 3, 4, 5 });
            expect(r.screen->landingOf(duck) == std::vector<int> ({ 6, 7 }), "tracks 1-6 hold trigs: 7 and 8");
            expectEquals(r.barText(), juce::String("2TRK>T7-8"));

            r.track(2).getStep(2).trig = false;
            r.track(6).getStep(2).trig = true;
            expect(r.screen->landingOf(duck) == std::vector<int> ({ 2, 7 }), "not side by side");
            expectEquals(r.barText(), juce::String("T3 8"));
            expect(r.barFits());

            r.track(2).getStep(2).trig = true;
            expect(r.screen->landingOf(duck).empty());
            expectEquals(r.barText(), juce::String("NEED 2"), "(NEED 2 EMPTY, as much of it as the bar holds)");
            expectEquals(r.screen->getSubPage(), juce::String("NEED 2 EMPTY"));
            expect(r.barFits());

            // The words fit whatever the tracks are.
            expectEquals(MultiLanding::text({ 6, 7, 8 }), juce::String("3TRK>T7-9"));
            expectEquals(MultiLanding::text({ 2, 6, 7 }), juce::String("T3 7 8"));
            expectEquals(MultiLanding::text({ 9, 10 }), juce::String("T10-11"));
            expectEquals(MultiLanding::text({ 0, 2, 3, 4, 7 }), juce::String("T1 3 4 5 8"));
            expectEquals(MultiLanding::text({ 0, 2, 3, 4, 7 }, 9), juce::String("T1 3-5 8"));
            expectEquals(MultiLanding::text({ 4 }), juce::String("1TRK>T5"));
            juce::Random random(7);
            for (int i = 0; i < 400; ++i)
            {
                std::vector<int> tracks;
                for (int t = 0; t < 32; ++t)
                    if (random.nextInt(3) == 0 && tracks.size() < 16)
                        tracks.push_back(t);
                if (tracks.empty()) continue;
                for (int room = 4; room <= 10; ++room)
                {
                    const auto words = MultiLanding::text(tracks, room);
                    expect(words.isNotEmpty() && words.length() <= room, words);
                }
            }
        }

        beginTest("LD loads every member: engines, sounds, starter patterns, names, routings; the keys' track selected");
        {
            MRig r;
            r.seq.selectPattern(3, SequencerCore::ChangeMode::Immediate);
            r.select(5);
            r.toMult();
            const int undosBefore = r.undos();
            r.confirm(4);

            expect(r.audio.getEngineTypeOf(0) == EngineType::Drum, "member 1, the kit, on track 1");
            expect(r.track(0).engineType == EngineType::Drum);
            expect(r.audio.getEngineTypeOf(1) == EngineType::UnifiedVoice, "member 2, the pad, on track 2");
            expect(r.track(1).engineType == EngineType::UnifiedVoice);
            expectEquals(r.names().get(0).name, juce::String("Duck Kit"));
            expectEquals(r.names().get(1).name, juce::String("Duck Pad"));
            expect(r.names().get(0).engine == EngineType::Drum);
            expectEquals(r.seq.getSelectedTrack(), 1, "the play member's track");
            expectEquals(r.undos(), undosBefore + 1, "one undo step");
            expect(r.screen->getSubPage().startsWith("OK"), r.screen->getSubPage());

            // The starter patterns: in the active pattern (4) and in no other.
            expectEquals(r.trigs(3, 0), juce::String("0 4 8 12"));
            expectEquals(r.trigs(3, 1), juce::String("0"));
            expectEquals((int) r.track(1).getStep(0).notes.size(), 4);
            expectEquals(r.track(1).getStep(0).length, 16.0f);
            expectEquals(r.track(0).getStep(4).notes[0].pitch, 8);
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                if (p != 3)
                    for (int t = 0; t < 8; ++t)
                        expect(r.trigs(p, t).isEmpty(), "pattern " + juce::String(p + 1) + " is untouched");
            for (int t = 2; t < 8; ++t)
                expect(r.trigs(3, t).isEmpty() && r.reason(t) == Reason::Empty, "the other tracks are untouched");

            // The pad's sound and its DUCK, listening to the track the kit landed on.
            expectEquals(r.audio.getTrackParameter(1, 10), 3000.0f);
            EffectType type {};
            expect(r.audio.getInsertEffectType(1, 0, type) && type == EffectType::SidechainEffect);
            expectEquals(r.audio.getTrackParameter(1, DUCK_SRC), 1.0f);
            expectEquals(r.stored(1, DUCK_SRC), 1.0f);
            expectEquals(r.audio.getTrackParameter(1, DUCK_SRC + 1), -30.0f);

            // Loaded tracks are not empty: the next one goes to 3 and 4.
            expect(r.reason(0) != Reason::Empty && r.reason(1) != Reason::Empty);
            expect(r.screen->landingOf(duckPair(r.presets)) == std::vector<int> ({ 2, 3 }));
            r.confirm(4);
            expect(r.audio.getEngineTypeOf(2) == EngineType::Drum && r.audio.getEngineTypeOf(3) == EngineType::UnifiedVoice);
            expectEquals(r.audio.getTrackParameter(3, DUCK_SRC), 3.0f, "the second pair's pad listens to its own kit");
            expectEquals(r.audio.getTrackParameter(1, DUCK_SRC), 1.0f);
            expectEquals(r.seq.getSelectedTrack(), 3);
        }

        beginTest("Tracks 1-6 in use: the multi lands on 7 and 8, and on a pair that is not side by side");
        {
            MRig r;
            r.occupy({ 0, 1, 2, 3, 4, 5 });
            r.toMult();
            r.confirm(4);
            expect(r.audio.getEngineTypeOf(6) == EngineType::Drum && r.audio.getEngineTypeOf(7) == EngineType::UnifiedVoice);
            expectEquals(r.audio.getTrackParameter(7, DUCK_SRC), 7.0f, "DUCK listens to track 7");
            expectEquals(r.stored(7, DUCK_SRC), 7.0f);
            expectEquals(r.trigs(0, 6), juce::String("0 4 8 12"));
            expectEquals(r.seq.getSelectedTrack(), 7);
            expect(r.audio.getEngineTypeOf(0) == EngineType::UnifiedVoice && r.audio.getEngineTypeOf(1) == EngineType::Drum,
                   "tracks 1 and 2 are as they were");
            expectEquals(r.trigs(0, 0), juce::String("2"));

            MRig s;
            s.occupy({ 0, 1, 3, 4, 5, 6 });
            // A member that hears another through its input as well (stored; the engine applies it later).
            auto multi = duckPair(s.presets);
            multi.members[1].preset.soundParams[TrackInParam::SRC] = 1.0f;
            multi.members[1].preset.soundParams[TrackInParam::LEVEL] = 0.7f;
            multi.members[0].preset.soundParams[TrackInParam::SRC] = 9.0f;      // no such member
            expect(s.screen->loadMulti(multi));
            expect(s.audio.getEngineTypeOf(2) == EngineType::Drum && s.audio.getEngineTypeOf(7) == EngineType::UnifiedVoice);
            expectEquals(s.audio.getTrackParameter(7, DUCK_SRC), 3.0f, "DUCK listens to track 3");
            expectEquals(s.stored(7, TrackInParam::SRC), 3.0f, "the input is track 3");
            expectEquals(s.stored(7, TrackInParam::LEVEL), 0.7f);
            expectEquals(s.stored(2, TrackInParam::SRC), 0.0f, "a source outside the multi: no input");
            expectEquals(s.seq.getSelectedTrack(), 7);
            // The engine has it too, and one undo takes the input away again.
            expectEquals(s.audio.getTrackParameter(7, TrackInParam::SRC), 3.0f, "the engine's input is track 3");
            expectWithinAbsoluteError(s.audio.getTrackParameter(7, TrackInParam::LEVEL), 0.7f, 1.0e-4f);
            expectEquals(s.audio.getTrackParameter(2, TrackInParam::SRC), 0.0f);
            s.undo();
            expectEquals(s.audio.getTrackParameter(7, TrackInParam::SRC), 0.0f, "undo: no input any more");
            expect(s.empty() == std::vector<int> ({ 2, 7 }), "tracks 3 and 8 are empty again after the undo");
        }

        beginTest("Too few empty tracks: nothing changes and the name bar says how many are needed");
        {
            MRig r;
            r.occupy({ 0, 1, 2, 3, 4, 5, 6 });
            r.select(2);
            r.toMult();
            const auto before = r.state();
            const int undosBefore = r.undos();
            const int said = TurnNotice::count();
            r.confirm(4);
            expectSame(r.state(), before);
            expectEquals(r.undos(), undosBefore);
            expectEquals(r.seq.getSelectedTrack(), 2);
            expectEquals(r.screen->getSubPage(), juce::String("NEED 2 EMPTY"));
            expectEquals(TurnNotice::count(), said + 1, "said through the name bar's notice");
            expect(r.names().get(7).name.isEmpty());
            expect(!r.screen->loadMulti(duckPair(r.presets)));
            expectSame(r.state(), before);
        }

        beginTest("One undo takes the whole load back; redo loads it again");
        {
            MRig r;
            r.occupy({ 0, 2, 3, 4, 5 });                // (lands on 2 and 7: the drum kit's track and track 7)
            r.track(1).soundParams[MixParam::VOLUME] = 0.4f;
            r.audio.setTrackParameter(1, MixParam::VOLUME, 0.4f);
            r.select(4);
            r.toMult();
            const auto before = r.state();
            const int undosBefore = r.undos();
            r.confirm(4);
            const auto after = r.state();
            expect(after != before);
            expect(r.audio.getEngineTypeOf(1) == EngineType::Drum && r.audio.getEngineTypeOf(6) == EngineType::UnifiedVoice);
            expectEquals(r.names().get(6).name, juce::String("Duck Pad"));
            expectEquals(r.seq.getSelectedTrack(), 6);
            expectEquals(r.undos(), undosBefore + 1);
            expect(r.mgr.getEditPlan(Edit::Undo).wordings().contains("UNDO LOAD MULTI")
                   || r.mgr.getEditPlan(Edit::Undo).wordings().joinIntoString("|").contains("LOAD"),
                   r.mgr.getEditPlan(Edit::Undo).wordings().joinIntoString("|"));

            r.undo();
            expectSame(r.state(), before, "engines, sounds, steps and names of both tracks are back");
            expect(r.names().get(1).name.isEmpty() && r.names().get(6).name.isEmpty());
            expect(r.trigs(0, 1).isEmpty() && r.trigs(0, 6).isEmpty());
            expect(r.audio.getInsertChain(6) == nullptr || r.audio.getInsertChain(6)->getEffectCount() == 0);
            expectEquals(r.seq.getSelectedTrack(), 4, "the track that was selected");
            expect(r.screen->landingOf(duckPair(r.presets)) == std::vector<int> ({ 1, 6 }), "the tracks are empty again");
            expectEquals(r.undos(), undosBefore);

            r.redo();
            expectSame(r.state(), after, "redo: everything loaded again");
            expectEquals(r.names().get(1).name, juce::String("Duck Kit"));
            expectEquals(r.names().get(6).name, juce::String("Duck Pad"));
            expectEquals(r.audio.getTrackParameter(6, DUCK_SRC), 2.0f);
            expectEquals(r.trigs(0, 1), juce::String("0 4 8 12"));
            expectEquals(r.seq.getSelectedTrack(), 6);

            r.undo();
            expectSame(r.state(), before, "and back once more");
        }

        beginTest("MULT lists the multis only, whatever EN says; a multi is not auditioned; DL deletes a user multi");
        {
            MRig r;
            auto mine = duckPair(r.presets);
            mine.name = "My Pair";
            mine.isFactory = false;
            r.presets.addMulti(mine);

            // An audition is going on when the highlight reaches MULT: the track gets its sound back.
            const auto before = r.state();
            r.turn(0, 1);
            expect(r.screen->isAuditioning());
            r.toMult();
            expect(!r.screen->isAuditioning(), "no audition of a multi");
            expectSame(r.state(), before, "the track has its own sound again");
            expectEquals(r.screen->getListSize(), r.presets.getMultiCount());
            r.turn(2, 1);       // EN: any engine
            expectEquals(r.screen->getListSize(), r.presets.getMultiCount());
            r.turn(2, -1);
            expectEquals(r.screen->getListSize(), r.presets.getMultiCount());
            r.turn(0, 1);
            r.turn(0, -1);
            expect(!r.screen->isAuditioning());
            expectSame(r.state(), before, "scrolling through multis changes nothing");

            // Not listed anywhere else.
            for (int i = 0; i < 12; ++i)
                r.turn(1, -1);                  // ALL
            r.turn(2, 1);                       // any engine
            expectEquals(r.screen->getListSize(), r.presets.getPresetCount(), "ALL holds the single presets only");
            r.turn(2, -1);

            // The category row shows every category at once, MULT, the tenth, too (nine tabs of two
            // letters were all it held, and MULT was off its right end): a tab says one letter where
            // no other category begins with it, else two.
            {
                const auto cats = PresetManager::getCategories();
                const auto tabs = PresetScreen::tabLayout(cats);
                expectEquals((int) tabs.size(), cats.size());
                // (And every built-in preset is in one of them: 36 resonator drum sounds were once in "DRUM",
                // which CT does not stop at, and so were found under ALL only.)
                for (int i = 0; i < r.presets.getPresetCount(); ++i)
                    expect(cats.contains(r.presets.getPreset(i).category) && r.presets.getPreset(i).category != MultiPreset::categoryName(),
                           r.presets.getPreset(i).name + " is in " + r.presets.getPreset(i).category);
                juce::StringArray said;
                for (auto& tab : tabs) said.add(tab.label);
                expectEquals(said.joinIntoString(" "), juce::String("A I B PA L KE FX PE KI M"));
                for (int i = 0; i < said.size(); ++i)
                {
                    expect(cats[i].startsWith(said[i]), said[i]);
                    for (int k = 0; k < i; ++k)
                        expect(said[k] != said[i], "two tabs read " + said[i]);
                }
                expectEquals(tabs.front().x, 0);
                expectEquals(tabs.back().x + tabs.back().width, 128, "the last tab ends at the glass's edge");
                for (size_t i = 1; i < tabs.size(); ++i)
                    expect(tabs[i].x - (tabs[i - 1].x + tabs[i - 1].width) >= 2, "room between the tabs");

                r.turn(1, 1);                   // INIT
                const auto init = r.picture();
                r.toMult();
                const auto mult = r.picture();
                // The chosen tab is the inverted one (its top row is lit from edge to edge), no other.
                auto lit = [](const auto& picture, const PresetScreen::Tab& tab)
                {
                    for (int x = tab.x; x < tab.x + tab.width; ++x)
                        if (!picture[11][(size_t) x]) return false;
                    return true;
                };
                for (size_t i = 0; i < tabs.size(); ++i)
                {
                    expect(lit(init, tabs[i]) == (i == 1), "INIT chosen, tab " + said[(int) i]);
                    expect(lit(mult, tabs[i]) == (i + 1 == tabs.size()), "MULT chosen, tab " + said[(int) i]);
                }
                // Blank columns between one tab and the next, all along the row, and nothing of the
                // old "more that way" dots at the ends.
                for (size_t i = 1; i < tabs.size(); ++i)
                    for (int x = tabs[i - 1].x + tabs[i - 1].width; x < tabs[i].x; ++x)
                        for (int y = 11; y < 20; ++y)
                            expect(!mult[(size_t) y][(size_t) x] && !init[(size_t) y][(size_t) x]);
                // Every tab's letters are on the glass: the row as drawn is the labels at their places.
                DisplayRenderer ref;
                for (auto& tab : tabs) ref.drawString(tab.x + 1, 12, tab.label);
                for (size_t i = 2; i + 1 < tabs.size(); ++i)        // (neither chosen in `init` nor in `mult`)
                    for (int y = 12; y < 19; ++y)
                        for (int x = tabs[i].x; x < tabs[i].x + tabs[i].width; ++x)
                            expect((bool) mult[(size_t) y][(size_t) x] == (bool) ref.getPixel(x, y), "tab " + said[(int) i]);
                if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_MULTI_SHOW", {}).isNotEmpty())
                    for (int y = 0; y < 64; ++y)
                    {
                        juce::String line;
                        for (int x = 0; x < 128; ++x)
                            line << (mult[(size_t) y][(size_t) x] ? "#" : ".");
                        logMessage(line);
                    }
            }

            // DL: the factory multi stays, the user's goes.
            const int count = r.presets.getMultiCount();
            expectEquals(r.screen->getSelectedIndex(), 0);
            r.confirm(6);
            expectEquals(r.presets.getMultiCount(), count, "a built-in multi cannot be deleted");
            expectEquals(r.screen->getSubPage(), juce::String("FACTORY"));
            for (int i = 0; i < count - 1; ++i) r.turn(0, 1);  // scroll to the last (user) multi
            r.confirm(6);
            expectEquals(r.presets.getMultiCount(), count - 1);
            expect(r.presets.findUserMulti("My Pair") < 0);
            expectEquals(r.screen->getListSize(), count - 1);
            expectSame(r.state(), before);
        }

        beginTest("SV on a connected track saves the whole set as a multi; a lone track saves a single preset");
        {
            MRig r;
            r.occupy({ 0, 1, 2, 3, 4, 5 });
            r.toMult();
            r.confirm(4);                       // Duck Pair on tracks 7 and 8; track 8 selected
            const int multis = r.presets.getMultiCount(), presets = r.presets.getPresetCount();
            juce::String savedAs;
            r.screen->onPresetSaved = [&](const juce::String& name) { savedAs = name; };

            r.confirm(5);                       // SV on track 8, which listens to track 7
            expectEquals(r.presets.getMultiCount(), multis + 1);
            expectEquals(r.presets.getPresetCount(), presets, "no single preset was made");
            expectEquals(r.screen->getSubPage(), juce::String("SAVED MULTI"));
            expect(r.screen->getSavedTracks() == std::vector<int> ({ 6, 7 }));
            const int at = r.presets.findUserMulti(savedAs);
            expect(at >= 0, savedAs);
            if (at >= 0)
            {
                const auto saved = r.presets.getMulti(at);
                expectEquals(saved.memberCount(), 2);
                expectEquals(saved.playMember, 2, "the track SV was turned on");
                expect(saved.members[0].preset.engineType == EngineType::Drum);
                expect(saved.members[1].preset.engineType == EngineType::UnifiedVoice);
                expectEquals(saved.members[0].preset.name, juce::String("Duck Kit"));
                auto sound = saved.members[1].preset.soundParams;
                expectEquals(sound[DUCK_TYPE], DUCK);
                expectEquals(sound[DUCK_SRC], 1.0f, "the DUCK's source by member number");
                expectEquals(sound[10], 3000.0f);
                expect(saved.members[0].hasPattern && saved.members[0].length == 16);
                expect(saved.members[0].steps[4].trig && !saved.members[0].steps[5].trig);
                expectEquals((int) saved.members[1].steps[0].notes.size(), 4);

                // It loads again like the built-in one (tracks 7 and 8 emptied first).
                MRig other;
                other.occupy({ 0, 1, 2, 4, 5, 6 });
                expect(other.screen->loadMulti(saved));
                expectEquals(other.audio.getTrackParameter(7, DUCK_SRC), 4.0f);
                expectEquals(other.trigs(0, 3), juce::String("0 4 8 12"));
                expectEquals(other.audio.getTrackParameter(7, 10), 3000.0f);
            }

            // The other end of the link: SV on the kit, which the pad listens to.
            r.select(6);
            r.confirm(5);
            expectEquals(r.presets.getMultiCount(), multis + 2);
            if (const int kit = r.presets.findUserMulti(savedAs); kit >= 0)
            {
                expectEquals(r.presets.getMulti(kit).memberCount(), 2);
                expectEquals(r.presets.getMulti(kit).playMember, 1);
            }

            // The save box: named, a built-in multi's name refused, cancelled.
            expect(r.screen->nameUse(savedAs, "Duck Pair") == PresetScreen::NameUse::BuiltIn);
            expect(r.screen->nameUse(savedAs, "Deep Bass") == PresetScreen::NameUse::New, "a single preset's name is free for a multi");
            expect(!r.screen->nameSavedPreset(savedAs, "duck pair"));
            expect(r.screen->nameSavedPreset(savedAs, "Kit And Pad"));
            expect(r.presets.findUserMulti("Kit And Pad") >= 0 && r.presets.findUserMulti(savedAs) < 0);
            expect(r.screen->cancelSavedPreset("Kit And Pad"));
            expectEquals(r.presets.getMultiCount(), multis + 1);

            // A lone track: a single preset, as ever.
            r.select(0);
            r.confirm(5);
            expectEquals(r.presets.getMultiCount(), multis + 1);
            expectEquals(r.presets.getPresetCount(), presets + 1);
            expectEquals(r.screen->getSubPage(), juce::String("SAVED"));
            expect(r.presets.findUserPreset(savedAs) >= 0);
            expect(r.screen->getSavedTracks() == std::vector<int> ({ 0 }));

            // The closure runs through inputs as well, and through more than one link:
            // track 3 hears track 4 (input), track 5 hears track 3 (input), track 1 is alone.
            r.audio.setTrackParameter(2, TrackInParam::SRC, 4.0f);
            r.audio.setTrackParameter(4, TrackInParam::SRC, 3.0f);
            expect(r.screen->connectedTracks(3) == std::vector<int> ({ 2, 3, 4 }));
            expect(r.screen->connectedTracks(4) == std::vector<int> ({ 2, 3, 4 }));
            expect(r.screen->connectedTracks(0) == std::vector<int> ({ 0 }));
            expect(r.screen->connectedTracks(7) == std::vector<int> ({ 6, 7 }));
            r.select(3);
            r.confirm(5);
            expectEquals(r.presets.getMultiCount(), multis + 2);
            if (const int trio = r.presets.findUserMulti(savedAs); trio >= 0)
            {
                const auto saved = r.presets.getMulti(trio);
                expectEquals(saved.memberCount(), 3);
                expectEquals(saved.playMember, 2);
                auto first = saved.members[0].preset.soundParams, third = saved.members[2].preset.soundParams;
                expectEquals(first[TrackInParam::SRC], 2.0f, "track 3's input, track 4, is member 2");
                expectEquals(third[TrackInParam::SRC], 1.0f, "track 5's input, track 3, is member 1");
                expect(saved.members[1].preset.soundParams.count(TrackInParam::SRC) == 0
                       || saved.members[1].preset.soundParams.at(TrackInParam::SRC) == 0.0f);
            }
        }

        beginTest("A single preset never stores a track's input, and loading one leaves the track's input alone");
        {
            MRig r;
            // Track 1 names itself as its input (nothing to connect to) and has input settings.
            r.track(0).soundParams[TrackInParam::SRC] = 1.0f;
            r.track(0).soundParams[TrackInParam::LEVEL] = 0.4f;
            r.track(0).soundParams[TrackInParam::MODE] = 1.0f;
            juce::String savedAs;
            r.screen->onPresetSaved = [&](const juce::String& name) { savedAs = name; };
            const int multis = r.presets.getMultiCount();
            r.confirm(5);
            expectEquals(r.presets.getMultiCount(), multis);
            const int at = r.presets.findUserPreset(savedAs);
            expect(at >= 0);
            if (at >= 0)
            {
                for (auto& [id, value] : r.presets.getPreset(at).soundParams)
                {
                    juce::ignoreUnused(value);
                    expect(!TrackInParam::contains(id), "id " + juce::String(id) + " in a single preset");
                }
                expect(r.presets.getPreset(at).soundParams.count(MixParam::VOLUME) == 1, "(the rest of the sound is there)");
            }

            // LD of a single preset, one that (wrongly) holds an input as well: track 6 keeps its own.
            Preset odd = r.presets.getPreset(r.presets.getFilteredIndices(EngineType::UnifiedVoice).front());
            odd.name = "Odd Input";
            odd.isFactory = false;
            odd.soundParams[TrackInParam::SRC] = 2.0f;
            odd.soundParams[TrackInParam::LEVEL] = 0.1f;
            r.presets.addPreset(odd);
            r.track(5).soundParams[TrackInParam::SRC] = 3.0f;
            r.track(5).soundParams[TrackInParam::LEVEL] = 0.6f;
            r.select(5);
            r.turn(1, 1);
            r.turn(1, -1);                              // (the list is rebuilt with the new preset)
            for (int i = 0; i < 6; ++i)
                r.turn(0, 60);                          // the last preset of the list: the one just added
            expect(r.screen->isAuditioning());
            expectEquals(r.stored(5, TrackInParam::SRC), 3.0f, "heard: the input is the track's");
            r.confirm(4);
            expectEquals(r.names().get(5).name, juce::String("Odd Input"));
            expectEquals(r.stored(5, TrackInParam::SRC), 3.0f, "kept: the input is the track's");
            expectEquals(r.stored(5, TrackInParam::LEVEL), 0.6f);
        }

        beginTest("Every factory multi loads, and each of its tracks sounds and stays finite");
        {
            for (auto& multi : FactoryMultis::getAll())
            {
                expect(multi.isValid() && multi.isFactory, multi.name);
                expect(multi.name.length() <= PresetManager::MAX_NAME_LENGTH, multi.name);
                expectEquals(multi.category, juce::String("MULT"));
                // Its routings name members that exist.
                for (auto& member : multi.members)
                    for (int source : MultiPreset::sourcesOf(member.preset.soundParams))
                        expect(source >= 1 && source <= multi.memberCount(), multi.name + ": a source that is no member");
                expectEquals(MultiPreset::fromJSON(multi.toJSON()).toJSON(), multi.toJSON(), multi.name);

                for (int m = 0; m < multi.memberCount(); ++m)
                {
                    MRig r;
                    r.audio.prepare(44100.0, 256);
                    expect(r.screen->loadMulti(multi), multi.name);
                    const auto tracks = std::vector<int> ({ 0, 1, 2, 3, 4, 5, 6, 7 });
                    // Every member plays the first trig of its starter pattern (or middle C); only
                    // member m is up in the mixer.
                    for (int k = 0; k < multi.memberCount(); ++k)
                    {
                        r.audio.setTrackParameter(k, MixParam::VOLUME, k == m ? 0.8f : 0.0f);
                        std::vector<int> notes { 60 };
                        for (auto& step : multi.members[(size_t) k].steps)
                            if (step.trig && !step.notes.empty())
                            {
                                notes.clear();
                                for (auto& n : step.notes) notes.push_back(n.pitch);
                                break;
                            }
                        for (int note : notes)
                            r.audio.getEngine(k)->noteOn(note, 0.9f);
                    }
                    bool finite = true;
                    const float level = r.render(0.5, &finite);
                    expect(finite, multi.name + " member " + juce::String(m + 1) + ": not finite");
                    expect(level > 1.0e-4f, multi.name + " member " + juce::String(m + 1) + ": silent (" + juce::String(level, 6) + ")");
                    expect(level < 4.0f);
                    expectEquals(r.audio.getNonFiniteBlocks(), 0);
                }
            }
        }

        beginTest("Duck Pair on tracks 7 and 8: the pad really ducks under the kick");
        {
            MRig r;
            r.audio.prepare(44100.0, 256);
            r.occupy({ 0, 1, 2, 3, 4, 5 });
            r.toMult();
            r.confirm(4);
            expect(r.audio.getEngineTypeOf(6) == EngineType::Drum && r.audio.getEngineTypeOf(7) == EngineType::UnifiedVoice);
            // The kit is down in the mixer (it feeds the DUCK all the same): what is heard is the pad.
            r.audio.setTrackParameter(6, MixParam::VOLUME, 0.0f);
            for (int note : { 48, 55, 60, 64 })
                r.audio.getEngine(7)->noteOn(note, 0.9f);
            r.render(1.0);
            bool finite = true;
            const float open = r.render(0.3, &finite);
            float ducked = 0.0f;
            for (int hit = 0; hit < 2; ++hit)
            {
                r.audio.getEngine(6)->noteOn(8, 1.0f);
                r.render(0.02);
                ducked = r.render(0.1, &finite);
                r.render(0.13);
            }
            r.render(1.2);
            const float back = r.render(0.3, &finite);
            logMessage("pad level: open " + juce::String(open, 5) + ", under the kick " + juce::String(ducked, 5)
                       + ", afterwards " + juce::String(back, 5));
            expect(finite);
            expect(open > 1.0e-3f);
            expect(ducked < open * 0.3f, "the pad goes down under the kick");
            // (The pad is two detuned saws with a per-note random spread: its level over a short window
            // moves by a quarter from run to run, so "back" and "stays up" are asked for at half.)
            expect(back > open * 0.5f, "and comes back");
            expectEquals(r.audio.getNonFiniteBlocks(), 0);

            // The same kick with the DUCK's source pointed away: no ducking (it is the routing that does it).
            r.audio.setTrackParameter(7, DUCK_SRC, 1.0f);
            r.render(0.3);
            r.audio.getEngine(6)->noteOn(8, 1.0f);
            r.render(0.02);
            const float deaf = r.render(0.4);
            expect(deaf > open * 0.5f, "listening to track 1, the pad stays up (" + juce::String(deaf, 5) + ")");
        }

        beginTest("SV of a connected track while a preset is heard on it saves the tracks' own sounds");
        {
            MRig r;
            r.occupy({ 0, 1, 2, 3, 4, 5 });
            r.toMult();
            r.confirm(4);                       // Duck Pair on tracks 7 and 8; track 8 selected
            for (int i = 0; i < 12; ++i)
                r.turn(1, -1);                  // ALL
            r.turn(2, 1);                       // any engine
            r.turn(0, 3);
            expect(r.screen->isAuditioning());
            juce::String savedAs;
            r.screen->onPresetSaved = [&](const juce::String& name) { savedAs = name; };
            const int multis = r.presets.getMultiCount();
            r.confirm(5);
            expectEquals(r.presets.getMultiCount(), multis + 1, "the pad still listens to the kit: a multi");
            const int at = r.presets.findUserMulti(savedAs);
            expect(at >= 0);
            if (at >= 0)
            {
                const auto saved = r.presets.getMulti(at);
                expectEquals(saved.memberCount(), 2);
                auto sound = saved.members[1].preset.soundParams;
                expect(saved.members[1].preset.engineType == EngineType::UnifiedVoice);
                expectEquals(sound[DUCK_TYPE], DUCK, "the pad's own DUCK, not the heard preset's effects");
                expectEquals(sound[DUCK_SRC], 1.0f);
                expectEquals(sound[10], 3000.0f, "the pad's own sound, not the preset being heard");
            }
        }

        beginTest("Undo of a load: after a pattern change, under an audition, with engine types that changed");
        {
            MRig r;
            r.toMult();
            const auto before = r.state();
            r.confirm(4);                       // tracks 1 and 2: both change their engine type
            const auto after = r.state();
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                expect(r.seq.getPattern(p).getTrack(0).engineType == EngineType::Drum
                       && r.seq.getPattern(p).getTrack(1).engineType == EngineType::UnifiedVoice
                       && r.seq.getPattern(p).getTrack(1).soundParams.count(DUCK_SRC) == 1,
                       "one sound per track, in every pattern (" + juce::String(p + 1) + ")");

            // Another pattern is active when the load is undone: the steps go back where they came from.
            r.seq.selectPattern(5, SequencerCore::ChangeMode::Immediate);
            r.tick();
            r.undo();
            expect(r.trigs(0, 0).isEmpty() && r.trigs(0, 1).isEmpty(), "the starter patterns are gone from pattern 1");
            expect(r.audio.getEngineTypeOf(0) == EngineType::UnifiedVoice && r.audio.getEngineTypeOf(1) == EngineType::Drum);
            r.redo();
            expectEquals(r.trigs(0, 0), juce::String("0 4 8 12"));
            for (int t = 0; t < 8; ++t)
                expect(r.trigs(5, t).isEmpty(), "the pattern that is active now is not written to");
            r.seq.selectPattern(0, SequencerCore::ChangeMode::Immediate);
            r.tick();
            expectSame(r.state(), after, "redo: as loaded");

            // A preset is being heard on a track the multi landed on: leaving gives the loaded sound
            // back, and an undo takes the load back, not the heard preset.
            for (int i = 0; i < 12; ++i)
                r.turn(1, -1);
            r.turn(2, 1);
            r.turn(0, 3);
            expect(r.screen->isAuditioning());
            r.mgr.setActiveScreen(TRACKS);
            expectSame(r.state(), after, "leaving PRESET: the loaded sound");
            r.mgr.setActiveScreen(PRESET);
            r.turn(0, 2);
            expect(r.screen->isAuditioning());
            r.undo();
            expect(!r.screen->isAuditioning());
            r.seq.setSelectedTrack(0);
            expectSame(r.state(), before, "undo under an audition");
            r.redo();
            expectSame(r.state(), after, "and redo");
        }

        beginTest("NEW, as the app makes it, leaves eight empty tracks");
        {
            MRig r;
            r.toMult();
            r.confirm(4);
            r.confirm(4);
            r.audio.addInsertEffect(5, EffectType::DelayEffect);
            r.audio.addInsertEffect(6, EffectType::DriveEffect);
            r.audio.setTrackParameter(7, 10, 500.0f);
            r.occupy({ 4 });
            expect(r.empty().empty(), "every track is in use");
            ProjectManager pm;
            pm.onRebuild = [&](const ProjectData& data)
            {
                ProjectBridge::Live live { r.seq, r.audio };
                live.presetNames = &r.names();
                ProjectBridge::apply(data, live);
                r.mgr.projectLoaded();
            };
            pm.newProject();
            r.audio.setEngineType(1, EngineType::Drum);         // (main.cpp: track 2 is the drum kit)
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                r.seq.getPattern(p).getTrack(1).engineType = EngineType::Drum;
            for (int t = 0; t < 8; ++t)
                expect(r.reason(t) == Reason::Empty, "after NEW: track " + juce::String(t + 1));
            expect(r.screen->landingOf(duckPair(r.presets)) == std::vector<int> ({ 0, 1 }));
        }

        beginTest("Duck Pair's starter patterns, played by the sequencer: the pad ducks on every beat");
        {
            MRig r;
            r.audio.prepare(44100.0, 256);
            r.toMult();
            r.confirm(4);
            r.audio.setTrackParameter(0, MixParam::VOLUME, 0.0f);       // (the kit feeds the DUCK all the same)
            r.seq.setTempo(120.0f);
            r.seq.setTrigCallback([&](int t, const Step& step)
            {
                if (auto* engine = r.audio.getEngine(t))
                    for (auto& note : step.notes)
                        engine->noteOn(note.pitch, note.velocity, -1);
            });
            r.seq.setNoteOffCallback([&](int t, int note)
            {
                if (auto* engine = r.audio.getEngine(t))
                    engine->noteOff(note, -1);
            });
            r.seq.start();
            bool finite = true;
            r.render(0.5, &finite);                                     // (the first beat: the pad's attack)
            for (int beat = 1; beat < 4; ++beat)
            {
                r.render(0.03, &finite);
                const float under = r.render(0.08, &finite);
                r.render(0.24, &finite);
                const float open = r.render(0.12, &finite);
                r.render(0.03, &finite);
                logMessage("beat " + juce::String(beat + 1) + ": under the kick " + juce::String(under, 5) + ", open " + juce::String(open, 5));
                expect(open > 1.0e-3f, "the held chord sounds (beat " + juce::String(beat + 1) + ")");
                expect(under < open * 0.5f, "and goes down under the kick (" + juce::String(under, 5) + " / " + juce::String(open, 5) + ")");
            }
            r.seq.stop();
            expect(finite);
            expectEquals(r.audio.getNonFiniteBlocks(), 0);
        }

        // ---- The DIRT / TALK / MODFX blocks of before (ids 240-263): insert effects in front of the chain ----
        const int TYPE = ProjectBridge::INSERT_TYPE_BASE, CELL = AudioEngine::EFFECT_PARAM_BASE, EXTRA = AudioEngine::INSERT_EXTRA_BASE;
        const int WET = AudioEngine::INSERT_WET_BASE;
        const int DIRT = TrackFxParam::DIRT_BASE, TALK = TrackFxParam::TALK_BASE, MODFX = TrackFxParam::MODFX_BASE;
        const auto typeOf = [](EffectType t) { return (float) ((int) t + 1); };
        const auto chainOf = [](AudioEngine& audio, int t)
        {
            juce::StringArray types;
            for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
            {
                EffectType type {};
                if (! audio.getInsertEffectType(t, slot, type))
                    break;
                types.add(juce::String((int) type));
            }
            return types.joinIntoString(" ");
        };
        const auto typesText = [](std::initializer_list<EffectType> types)
        {
            juce::StringArray out;
            for (auto t : types)
                out.add(juce::String((int) t));
            return out.joinIntoString(" ");
        };
        const auto holdsBlockId = [](const ParamMap& sound)
        {
            return sound.lower_bound(TrackFxParam::DIRT_BASE) != sound.lower_bound(TrackFxParam::END);
        };
        const auto lockAt = [](const Step& step, int id)
        {
            auto it = step.paramLocks.find(id);
            return it != step.paramLocks.end() ? it->second : -999.0f;
        };
        // The six blocks' effects, all on: DIRT (DRIV, MIX), TALK (VOWL, MIX), and MODFX's four depths.
        const auto allBlocksOn = [&](ParamMap& sound)
        {
            sound[DIRT + 0] = 0.5f;  sound[DIRT + 5] = 220.0f;  sound[DIRT + 6] = 0.3f;  sound[DIRT + 7] = 1.0f;
            sound[TALK + 0] = 0.3f;  sound[TALK + 2] = 0.5f;    sound[TALK + 3] = 0.7f;  sound[TALK + 4] = 1.0f;  sound[TALK + 7] = 0.3f;
            sound[MODFX + 4] = 2.0f; sound[MODFX + 5] = 0.4f;                           // phaser: rate, depth
            sound[MODFX + 2] = 1.0f; sound[MODFX + 3] = 0.5f;                           // flanger
            sound[MODFX + 0] = 1.0f; sound[MODFX + 1] = 0.5f;                           // chorus
            sound[MODFX + 6] = 1.0f; sound[MODFX + 7] = 0.5f;                           // rotary
        };

        beginTest("A project of before DIRT / TALK / MODFX were inserts, with more blocks on than the chain has slots left: "
                  "the first ones in front, the sound's own inserts all kept, the locks where their cells went");
        {
            MRig r;
            ProjectBridge::Live live { r.seq, r.audio };
            auto data = ProjectBridge::capture(live, "old blocks");
            const EffectType own[5] = { EffectType::DelayEffect, EffectType::EQEffect, EffectType::CompressorEffect,
                                        EffectType::TremoloEffect, EffectType::AutoPanEffect };
            {
                auto& sound = data.patterns[(size_t) data.activePattern].tracks[0].soundParams;
                for (int i = 0; i < 5; ++i)
                {
                    sound[TYPE + i] = typeOf(own[i]);
                    sound[AudioEngine::INSERT_ON_BASE + i] = 1.0f;
                    sound[WET + i] = 1.0f;
                }
                sound[CELL + 1] = 0.8f;             // the delay's feedback
                sound[WET + 4] = 0.25f;             // the last one's WET
                sound[CELL + 6 * 4] = 0.123f;       // (a cell of a slot the sound names no effect for: nothing)
                allBlocksOn(sound);
            }
            // The same locks on a step of the active pattern and of another one.
            for (int p : { data.activePattern, 5 })
            {
                auto& steps = data.patterns[(size_t) p].tracks[0].steps;
                if (steps.empty())
                    steps.resize(16);
                auto& step = steps[2];
                step.trig = true;
                step.paramLocks = { { DIRT + 0, 0.9f }, { DIRT + 5, 440.0f }, { TALK + 3, 0.4f }, { MODFX + 1, 0.3f },
                                    { CELL + 1, 0.6f }, { WET + 4, 0.5f }, { EXTRA + 4 * EffectBase::MAX_EXTRAS, 0.2f },
                                    { UnifiedParam::CUTOFF, 500.0f } };
            }
            const auto before = data.patterns[(size_t) data.activePattern].tracks[0].soundParams;
            const auto prepend = ProjectBridge::trackFxPrepend(before);
            expectEquals((int) prepend.size(), 3, "six blocks on, five inserts stored, eight slots: room for three");

            // (through the file, as a project comes in)
            const auto loaded = ProjectData::fromJSON(data.toJSON());
            ProjectBridge::apply(loaded, live);

            expectEquals(chainOf(r.audio, 0), typesText({ EffectType::DirtInsertEffect, EffectType::TalkInsertEffect, EffectType::PhaserEffect,
                                                          own[0], own[1], own[2], own[3], own[4] }),
                         "DIRT, TALK and the phaser in front; the five inserts of the sound all still there");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, CELL + 0), 0.5f, 1.0e-6f, "DIRT's DRIV");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, EXTRA + 3), 1.0f, 1.0e-6f, "DIRT's MIX, its eighth cell");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, CELL + 4 + 3), 0.7f, 1.0e-6f, "TALK's MIX");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, CELL + 8 + 1), 0.4f, 1.0e-6f, "the phaser's depth");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, WET + 2), 0.5f, 1.0e-6f, "the phaser's mix is its slot's WET");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, CELL + 3 * 4 + 1), 0.8f, 1.0e-6f, "the delay's feedback moved back with it");
            expectWithinAbsoluteError(r.audio.getTrackParameter(0, WET + 7), 0.25f, 1.0e-6f, "the last insert's WET moved back with it");

            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
            {
                const auto& stored = r.seq.getPattern(p).getTrack(0).soundParams;
                expect(! holdsBlockId(stored), "pattern " + juce::String(p + 1) + ": the track's stored sound holds no id of a block any more");
                expect(stored.count(TYPE) == 1 && stored.at(TYPE) == typeOf(EffectType::DirtInsertEffect), "... DIRT is its first slot");
                expectEquals((int) stored.count(TYPE + 7), 1, "... and it names all eight slots");
                expectEquals((int) stored.count(CELL + 6 * 4), 0, "a cell of a slot that named no effect went");
            }
            // What the track stores is what a save now writes for it (the chain as it plays).
            {
                const auto saved = ProjectBridge::captureTrackSound(live, 0, r.track(0).soundParams);
                for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
                    expectEquals(r.stored(0, TYPE + slot), saved.at(TYPE + slot), "slot " + juce::String(slot + 1) + " stored as it plays");
            }

            for (int p : { r.seq.getActivePatternIndex(), 5 })
            {
                const auto& step = r.seq.getPattern(p).getTrack(0).steps[2];
                const juce::String where = "pattern " + juce::String(p + 1) + ": ";
                expectEquals(lockAt(step, CELL + 0), 0.9f, where + "a lock on DIRT's DRIV is on the DIRT insert's first cell");
                expectEquals(lockAt(step, EXTRA + 1), 440.0f, where + "a lock on DIRT's TUNE is on its sixth");
                expectEquals(lockAt(step, CELL + 4 + 3), 0.4f, where + "a lock on TALK's MIX is on the TALK insert's");
                expectEquals(lockAt(step, CELL + 3 * 4 + 1), 0.6f, where + "a lock on the first insert moved back THREE slots (not six)");
                expectEquals(lockAt(step, WET + 7), 0.5f, where + "a lock on the fifth insert's WET is on the eighth slot's (not off the end)");
                expectEquals(lockAt(step, EXTRA + 7 * EffectBase::MAX_EXTRAS), 0.2f, where + "and one on its fifth cell");
                expectEquals(lockAt(step, UnifiedParam::CUTOFF), 500.0f, where + "an engine lock stays");
                expectEquals((int) step.paramLocks.size(), 7, where + "the MODFX lock has no cell to go to; nothing else is left over");
                for (auto& [id, value] : step.paramLocks)
                    expect(! TrackFxParam::isTrackFxParam(id), where + "no lock on a block id");
            }

            // A chain that is full already: nothing is put in front, and nothing moves.
            {
                ParamMap full;
                for (int i = 0; i < AudioEngine::MAX_INSERTS_PER_TRACK; ++i)
                    full[TYPE + i] = typeOf(EffectType::TremoloEffect);
                full[CELL + 1] = 0.5f;
                allBlocksOn(full);
                expect(ProjectBridge::trackFxPrepend(full).empty());
                expect(ProjectBridge::convertTrackFxParams(full) == full, "a full chain is left as it is");
                Track track;
                track.steps[0].paramLocks = { { CELL + 1, 0.7f }, { DIRT + 0, 0.2f } };
                const auto locks = track.steps[0].paramLocks;
                ProjectBridge::convertStepLocks(full, track);
                expect(track.steps[0].paramLocks == locks, "and its locks stay where they are");
            }
            // No block on (the zeros a cleared TRK FX writes): the sound is not touched.
            {
                ParamMap off { { UnifiedParam::CUTOFF, 900.0f }, { TYPE, typeOf(EffectType::DelayEffect) }, { CELL + 1, 0.5f } };
                for (int id = DIRT; id < TrackFxParam::END; ++id)
                    off[id] = 0.0f;
                expect(ProjectBridge::convertTrackFxParams(off) == off);
            }
        }

        beginTest("A preset of before DIRT / TALK / MODFX were inserts keeps them, in front of the inserts it stores, as a project does");
        {
            MRig r;
            Preset old;
            old.name = "Old Dirt";
            old.category = "LEAD";
            old.engineType = EngineType::UnifiedVoice;
            old.soundParams = { { UnifiedParam::CUTOFF, 900.0f }, { TYPE, typeOf(EffectType::DelayEffect) },
                                { AudioEngine::INSERT_ON_BASE, 1.0f }, { WET, 0.4f }, { CELL + 1, 0.8f },
                                { DIRT + 0, 0.6f }, { DIRT + 5, 220.0f }, { DIRT + 7, 1.0f },
                                { TALK + 0, 0.3f }, { TALK + 2, 0.5f }, { TALK + 3, 0.5f }, { TALK + 4, 1.0f } };

            const auto sound = PresetManager::soundOf(old);
            expect(! holdsBlockId(sound), "the sound a track takes holds no id of a block");
            expectEquals(sound.at(TYPE + 0), typeOf(EffectType::DirtInsertEffect));
            expectEquals(sound.at(TYPE + 1), typeOf(EffectType::TalkInsertEffect));
            expectEquals(sound.at(TYPE + 2), typeOf(EffectType::DelayEffect));
            expectEquals(sound.at(CELL + 2 * 4 + 1), 0.8f);
            expectEquals(sound.at(WET + 2), 0.4f);
            expect(sound == ProjectBridge::convertTrackFxParams(old.soundParams), "by the project's own conversion");

            // Loaded for good on a track (a multi of one member: PresetScreen::applyPreset, as LD and
            // an audition go), with a starter pattern whose step locks a DIRT cell and a cell of the delay.
            MultiPreset one;
            one.name = old.name;
            one.members.push_back({ old, true, 16, std::vector<TrackData::StepData>(16) });
            one.members[0].steps[4].trig = true;
            one.members[0].steps[4].paramLocks = { { DIRT + 0, 0.9f }, { CELL + 1, 0.3f } };
            const auto landing = r.screen->landingOf(one);
            expect(landing.size() == 1);
            expect(r.screen->loadMulti(one), "loaded");
            const int t = landing.empty() ? 0 : landing[0];
            expectEquals(chainOf(r.audio, t), typesText({ EffectType::DirtInsertEffect, EffectType::TalkInsertEffect, EffectType::DelayEffect }),
                         "DIRT and TALK are on the track, in front of the preset's delay");
            expectWithinAbsoluteError(r.audio.getTrackParameter(t, CELL + 0), 0.6f, 1.0e-6f, "DIRT's DRIV");
            expectWithinAbsoluteError(r.audio.getTrackParameter(t, CELL + 4 + 3), 0.5f, 1.0e-6f, "TALK's MIX");
            expectWithinAbsoluteError(r.audio.getTrackParameter(t, CELL + 2 * 4 + 1), 0.8f, 1.0e-6f, "the delay's feedback");
            expectWithinAbsoluteError(r.audio.getTrackParameter(t, WET + 2), 0.4f, 1.0e-6f, "the delay's WET");
            expect(! holdsBlockId(r.track(t).soundParams), "the track stores the converted sound");
            expectEquals(r.stored(t, TYPE + 2), typeOf(EffectType::DelayEffect));
            const auto& step = r.track(t).steps[4];
            expectEquals(lockAt(step, CELL + 0), 0.9f, "the starter pattern's lock on DIRT's DRIV follows it");
            expectEquals(lockAt(step, CELL + 2 * 4 + 1), 0.3f, "and its lock on the delay moves back with the delay");
            expectEquals((int) step.paramLocks.size(), 2);

            // No sound written in code has a block on: every factory preset and multi is as it was.
            for (int i = 0; i < r.presets.getPresetCount(); ++i)
                expect(PresetManager::soundOf(r.presets.getPreset(i)).size() == r.presets.getPreset(i).soundParams.size()
                       && ProjectBridge::trackFxPrepend(r.presets.getPreset(i).soundParams).empty(), r.presets.getPreset(i).name);
            for (int m = 0; m < r.presets.getMultiCount(); ++m)
                for (auto& member : r.presets.getMulti(m).members)
                    expect(ProjectBridge::trackFxPrepend(member.preset.soundParams).empty(), r.presets.getMulti(m).name);
        }
    }
};

static MultiPresetTests multiPresetTests;
