#pragma once

// Format 3 (src/project/StoredLists.h): a cell whose value is a position in a list is stored
// with the entry's name, the name decides on load, and a file of before - numbers only, no stamp
// of the build that wrote it - is translated from the layout its ids tell
// (src/project/StoredListHistory.h).
//
//   which cells      every page of every engine is walked: a Choice cell that shows one of the
//                    named lists must be classified with that list, and a long list must be
//                    either named or in the inventory of lists that stay numbers (with the reason)
//   format 3         every kind of cell there and back; the NAME decides (a file whose numbers are
//                    those of a shorter list - a list that has grown in its middle since - keeps
//                    every route); a name its list no longer has is OFF, said once
//   old files        the lists as they were, checked against witnesses that are not the tables:
//                    v0.9's own lists (copied from that tree's output), the old engines kept under
//                    tests/old, the owner's P01; each stage of each engine read back entry by
//                    entry; the drum kit of before U2b put back on its types
//   factory presets  no preset or multi written in code names what is not there, none has a route
//                    that aims at nothing, and the constants they route by are the entries they say

#include "../src/project/Project.h"
#include "../src/project/ProjectBridge.h"
#include "../src/project/PresetManager.h"
#include "../src/project/presets/ResonatorKits.h"
#include "old/OldFMEngine.h"
#include "old/OldMacroOscEngine.h"
#include "old/OldWaveSeqEngine.h"
#include "old/OldSamplerEngine.h"
#include "old/OldDrumEngine.h"
#include "old/OldResonatorEngine.h"
#include "old/OldWavetableEngine.h"
#include "old/OldUnifiedVoice.h"

class FormatV3Tests : public juce::UnitTest
{
public:
    FormatV3Tests() : juce::UnitTest("Format V3", "Neptune") {}

    // ---- helpers -----------------------------------------------------------------------------
    static juce::String joined(const char* const* labels, int count)
    {
        juce::String s;
        for (int i = 0; i < count; ++i)
            s << (i > 0 ? "|" : "") << (labels[i] != nullptr ? labels[i] : "(null)");
        return s;
    }
    static juce::String joined(const stored::List& list) { return joined(list.labels, list.count); }

    static const ParamSpec* find(const ParamPages& pages, int id)
    {
        for (auto& page : pages)
            if (const auto* spec = page.find(id))
                return spec;
        return nullptr;
    }
    static juce::String shown(const ParamPages& pages, int id)
    {
        const auto* spec = find(pages, id);
        return spec != nullptr && spec->kind == ParamKind::Choice && spec->choices != nullptr ? joined(spec->choices, spec->numChoices) : juce::String();
    }

    // A project file of version `version` with one track: its sound, and one step with these locks.
    static juce::var projectVar(int version, int engineType, const ParamMap& sound, const ParamMap& locks = {})
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("version", version);
        root->setProperty("name", "test");
        root->setProperty("bpm", 120.0);
        root->setProperty("activePattern", 0);
        auto* pattern = new juce::DynamicObject();
        pattern->setProperty("swing", 0.0);
        auto* track = new juce::DynamicObject();
        track->setProperty("engine", engineType);
        track->setProperty("length", 4);
        track->setProperty("speed", 1.0);
        track->setProperty("midiCh", 1);
        juce::Array<juce::var> params;
        for (auto& [id, value] : sound)
        {
            auto* p = new juce::DynamicObject();
            p->setProperty("id", id);
            p->setProperty("v", (double) value);
            params.add(juce::var(p));
        }
        track->setProperty("params", params);
        juce::Array<juce::var> steps;
        if (! locks.empty())
        {
            auto* step = new juce::DynamicObject();
            step->setProperty("trig", true);
            step->setProperty("vel", 1.0);
            step->setProperty("len", 1.0);
            juce::Array<juce::var> lockList;
            for (auto& [id, value] : locks)
            {
                auto* l = new juce::DynamicObject();
                l->setProperty("id", id);
                l->setProperty("v", (double) value);
                lockList.add(juce::var(l));
            }
            step->setProperty("locks", lockList);
            steps.add(juce::var(step));
        }
        track->setProperty("steps", steps);
        juce::Array<juce::var> tracks;
        tracks.add(juce::var(track));
        pattern->setProperty("tracks", tracks);
        juce::Array<juce::var> patterns;
        patterns.add(juce::var(pattern));
        root->setProperty("patterns", patterns);
        return juce::var(root);
    }

    // The sound of that one track as an old file's loader makes it today's.
    static ParamMap oldSound(int engineType, const ParamMap& sound, juce::StringArray* lost = nullptr)
    {
        auto data = ProjectData::fromVar(projectVar(2, engineType, sound));
        if (lost != nullptr)
            *lost = data.lost;
        return data.patterns[0].tracks[0].soundParams;
    }

    static float at(const ParamMap& map, int id, float fallback = -999.0f)
    {
        const auto it = map.find(id);
        return it != map.end() ? it->second : fallback;
    }

    // A list of StoredListHistory.h as its entries ("=N": today's first N).
    static juce::String written(int engineType, stored::history::Family family, int stage)
    {
        const char* list = stored::old::listAt(engineType, family, stage);
        if (list == nullptr)
            return {};
        if (list[0] != '=')
            return list;
        const int id = family == stored::history::Cell ? (engineType == 0 ? 158 : 165)
                     : (family == stored::history::Slot ? 701 : (family == stored::history::Source ? 700 : DrumEngine::TYPE));
        const auto today = stored::listOf(engineType, id);
        return joined(today.labels, juce::jmin(today.count, stored::old::length(list)));
    }
    static int stageOfCommit(int engineType, const char* commit)
    {
        int count = 0;
        const auto* stages = stored::history::stagesOf(engineType, count);
        for (int s = 0; s < count; ++s)
            if (juce::String(stages[s].since) == commit)
                return s;
        return -1;
    }
    // The first id of each range of a stage's evidence.
    static std::vector<int> evidenceIds(const char* evidence)
    {
        std::vector<int> ids;
        for (auto& range : juce::StringArray::fromTokens(juce::String(evidence), ",", ""))
            if (range.isNotEmpty())
                ids.push_back(range.upToFirstOccurrenceOf("-", false, false).getIntValue());
        return ids;
    }

    // An old engine (tests/old: the product's own code of before its port) shows the lists a stage
    // of the tables says: the witness that the tables are what the code was.
    template <typename Engine>
    void witness(const char* name, int engineType, const char* commit, bool hasCells = true)
    {
        Engine old;
        const auto pages = old.getParamPages();
        const int stage = stageOfCommit(engineType, commit);
        expect(stage >= 0, juce::String(name) + ": no stage " + commit);
        if (stage < 0)
            return;
        // (Two of the copies are built on shared pieces that have grown since they were copied, and
        // show what those brought: the track's effect cells at the end of a list from the shared
        // router, BODY's second page from the shared body section. Taken out, the rest is the copy's.)
        const auto asCopied = [](juce::String list)
        {
            const juce::String effects = "|" + joined(mod::TrackDest::labels(), mod::TrackDest::COUNT);
            if (list.endsWith(effects))
                list = list.dropLastCharacters(effects.length());
            return list.replace("|BCNT|BGRN|BRED|BHRD|BRDM|BKDC|BDIR|BWIR", "");
        };
        if (hasCells)
            expectEquals(asCopied(shown(pages, 165)), written(engineType, stored::history::Cell, stage), juce::String(name) + ": the DEST list of " + commit);
        expectEquals(asCopied(shown(pages, 701)), written(engineType, stored::history::Slot, stage), juce::String(name) + ": the slots' DEST list of " + commit);
        expectEquals(shown(pages, 700), written(engineType, stored::history::Source, stage), juce::String(name) + ": the SRC list of " + commit);
    }

    void runTest() override
    {
        using K = mod::NamedList::Kind;
        using F = stored::history::Family;
        const int UV = (int) EngineType::UnifiedVoice, FM = (int) EngineType::FM, MACRO = (int) EngineType::MacroOsc,
                  WT = (int) EngineType::Wavetable, WSEQ = (int) EngineType::WaveSeq, SMP = (int) EngineType::Sampler,
                  DRUM = (int) EngineType::Drum, RSN = (int) EngineType::Resonator;

        // =========================================================================================
        beginTest("The router's own cells");
        {
            expect(mod::NamedList::classify(165) == K::CellDest);                       // LFO1 DEST
            expect(mod::NamedList::classify(173) == K::CellDest);
            expect(mod::NamedList::classify(160) == K::Shape && mod::NamedList::classify(168) == K::Shape);
            expect(mod::NamedList::classify(161) == K::Trigger && mod::NamedList::classify(163) == K::Division);
            expect(mod::NamedList::classify(158) == K::None && mod::NamedList::classify(162) == K::None);
            for (int r = 0; r < mod::Router::NUM_STRIP; ++r)
            {
                expect(mod::NamedList::classify(mod::Router::stripDestId(r)) == K::CellDest);
                expect(mod::NamedList::classify(mod::Router::stripAmountId(r)) == K::None);
            }
            expect(mod::NamedList::classify(mod::Router::STRIP_VIBRATO) == K::None && mod::NamedList::classify(mod::Router::STRIP_VIBRATO_RATE) == K::None);
            expect(mod::NamedList::classify(mod::Router::xinId(mod::Router::XinDest1)) == K::CellDest);
            expect(mod::NamedList::classify(mod::Router::xinId(mod::Router::XinDest2)) == K::CellDest, "the XIN page's second route too");
            expect(mod::NamedList::classify(mod::Router::xinId(mod::Router::XinFilter1)) == K::None);
            for (int s = 0; s < ModSlotParam::MAX_SLOTS; ++s)
            {
                expect(mod::NamedList::classify(ModSlotParam::id(s, ModSlotParam::SRC)) == K::Source);
                expect(mod::NamedList::classify(ModSlotParam::id(s, ModSlotParam::DEST)) == K::SlotDest);
                expect(mod::NamedList::classify(ModSlotParam::id(s, ModSlotParam::AMT)) == K::None);
                expect(mod::NamedList::classify(ModSlotParam::id(s, ModSlotParam::VIA)) == K::Source);
            }
            expect(mod::NamedList::classify(ModSlotParam::COUNT) == K::None && mod::NamedList::classify(LfoShapeParam::BASE) == K::None);
            // The pressure slots, by the constants of whoever owns them (not by 773 ..).
            for (int s = 0; s < PressureSlotParam::MAX_SLOTS; ++s)
                for (int c = 0; c < PressureSlotParam::STRIDE; ++c)
                    expect((mod::NamedList::classify(PressureSlotParam::id(s, c)) == K::PressureDest) == (c == PressureSlotParam::DEST_OFFSET));
            // Every id the router owns is either a position or not: none is left unlooked-at.
            int positions = 0;
            for (int id = 0; id < 1000; ++id)
                if (mod::NamedList::classify(id) != K::None)
                {
                    expect(mod::Router::owns(id), "classified but not the router's: " + juce::String(id));
                    ++positions;
                }
            expectEquals(positions, 2 * 4 + 3 + 2 + 16 * 3 + PressureSlotParam::MAX_SLOTS);
        }

        // =========================================================================================
        beginTest("Every page of every engine: a cell that shows a named list is stored by name");
        {
            // The long lists that stay numbers in a file, by engine and label, and why. A long list
            // that is in neither group fails here: whoever adds one decides which it is.
            struct Stays { int engine; const char* label; const char* why; };
            static const Stays stays[] = {
                { UV, "WAV1", "the ten oscillator kinds: a fixed set (the SRC cells' meaning follows it)" },
                { UV, "WAV2", "as WAV1" }, { UV, "WAV3", "as WAV1" },
                { FM, "ALGO", "the eight algorithms: fixed" },
                { FM, "WAV1", "the shared oscillator's waves (phaseosc): the wavetable set, appended to only" },
                { FM, "WAV2", "as WAV1" }, { FM, "WAV3", "as WAV1" }, { FM, "WAV4", "as WAV1" }, { FM, "WAV5", "as WAV1" }, { FM, "WAV6", "as WAV1" },
                { FM, "SMPL", "LEFT: a place in the sample bank, which moves when files are added (not yet by name)" },
                { FM, "WSEQ", "a count (SCAN, 2 .. 16)" },
                { MACRO, "MODL", "the eight models: stored by index, never re-ordered (OA-LATENT-WIDEN)" },
                { WT, "TABL", "the built-in tables: WavetableFactory, append only (guardrails)" },
                { WT, "LEN", "the scan's own nine lengths: fixed" },
                { WT, "SMP1", "the key frames' samples: stored by name already (TrackData::keySamples)" },
                { WT, "SMP2", "as SMP1" }, { WT, "SMP3", "as SMP1" }, { WT, "SMP4", "as SMP1" }, { WT, "SMP5", "as SMP1" },
                { WT, "SMP6", "as SMP1" }, { WT, "SMP7", "as SMP1" }, { WT, "SMP8", "as SMP1" },
                { SMP, "SMPL", "the sample: stored by name already (TrackData::sampleName, Preset::sampleName)" },
                { SMP, "MODE", "the seven play modes: fixed" },
                { SMP, "ALGO", "the seven stretch algorithms: fixed" },
                { SMP, "LEN", "the stretch's own ten lengths: fixed" },
                { DRUM, "SND", "a pad's resonator sound: the list is the pad TYPE's (the same id is KIND on a kick, MODL on the bank)" },
                { RSN, "CURV", "the saturation curves: sat::Curve, append only (guardrails)" },
                { RSN, "SMPL", "LEFT: a place in the sample bank (not yet by name)" },
            };
            const auto staysNumber = [&](int engine, const juce::String& label)
            {
                for (auto& s : stays)
                    if (s.engine == engine && label == s.label)
                        return true;
                // (the drum kit's sixteen WAVE cells W1 .. W16: the shared oscillator's waves, as FM's)
                return engine == DRUM && label.startsWithChar('W') && label.substring(1).containsOnly("0123456789");
            };

            int classified = 0, kinds = 0;
            for (int e = 0; e < ENGINE_TYPE_COUNT; ++e)
            {
                if (e == (int) EngineType::MIDITrack)
                {
                    expect(! stored::listOf(e, 165), "a MIDI track has no list");
                    continue;
                }
                const auto engine = AudioEngine::createEngine((EngineType) e);
                const auto pages = engine->getParamPages();
                const auto tables = stored::tablesOf(e);
                expect(tables.dests != nullptr && tables.sources != nullptr, juce::String(stored::engineLabel(e)) + " has tables");
                if (tables.dests == nullptr || engine->modRouter() == nullptr)
                    continue;
                expect(tables.dests->visible() == engine->modRouter()->dests().visible() && tables.dests->size() == engine->modRouter()->dests().size(),
                       juce::String(stored::engineLabel(e)) + ": the table the format names by is the engine's router's");
                expect(tables.sources == &engine->modRouter()->sources(), juce::String(stored::engineLabel(e)) + ": the source list is the engine's router's");

                // The lists that are named, by what they hold (a cell is known by showing one).
                juce::StringArray namedLists;
                for (auto kind : { K::CellDest, K::SlotDest, K::Source, K::Shape, K::Trigger, K::Division })
                    namedLists.addIfNotAlreadyThere(joined(stored::routerList(kind, tables)));
                namedLists.addIfNotAlreadyThere(joined(stored::filterTypeList()));
                namedLists.addIfNotAlreadyThere(joined(LFO::triggerLabels(), (int) LFO::TriggerMode::COUNT));
                if (e == DRUM)
                    namedLists.addIfNotAlreadyThere(joined(DrumEngine::typeNames(), DrumEngine::NUM_DRUM_TYPES));

                for (auto& page : pages)
                    for (auto& cell : page.params)
                    {
                        if (! cell.isValid())
                            continue;
                        const auto list = stored::listOf(e, cell.id);
                        const juce::String where = juce::String(stored::engineLabel(e)) + " " + page.name + " " + cell.label + " (id " + juce::String(cell.id) + ")";
                        if (cell.kind != ParamKind::Choice || cell.choices == nullptr)
                        {
                            expect(! list, where + " is no Choice cell and must not be read as a position");
                            continue;
                        }
                        const juce::String shows = joined(cell.choices, cell.numChoices);
                        if (list)
                        {
                            ++classified;
                            // (the kit's TYPE cell offers the first ten of the eighteen that are stored)
                            const juce::String all = joined(list);
                            expect(all == shows || (e == DRUM && stored::isDrumTypeCell(cell.id) && all.startsWith(shows + "|")),
                                   where + " is read by a list that is not the one it shows");
                            // (round trip of every entry it shows: a place kept with an empty label is
                            // not shown - ParamSpec::isOffered - and has no name to come back by)
                            for (int i = 0; i < cell.numChoices; ++i)
                                if (cell.isOffered(i) && stored::valueOf(e, cell.id, stored::nameOf(e, cell.id, (float) i)) != i)
                                {
                                    expect(false, where + ": entry " + juce::String(i) + " does not come back by its name");
                                    break;
                                }
                        }
                        else if (namedLists.contains(shows))
                            expect(false, where + " shows a named list and is stored as a bare number: classify it (StoredLists.h, mod::NamedList)");
                        else if (cell.numChoices > 6)
                            expect(staysNumber(e, cell.label), where + ": a list of " + juce::String(cell.numChoices)
                                   + " that is neither stored by name nor in the inventory of lists that stay numbers");
                    }
                ++kinds;
            }
            expectEquals(kinds, ENGINE_TYPE_COUNT - 1);
            expect(classified > 8 * 20, "the pages were walked (" + juce::String(classified) + " cells stored by name)");

            // The cells that are on no engine page.
            //   the unified voice's LFO and envelope pages (built by the screen)
            for (int l = 0; l < ModParam::MAX_LFOS; ++l)
            {
                expect(stored::listOf(UV, ModParam::lfoParamId(l, ModParam::LFO_DEST)).labels == UnifiedVoice::lfoDestCellLabels());
                expectEquals(stored::listOf(UV, ModParam::lfoParamId(l, ModParam::LFO_DEST)).count, (int) UnifiedVoice::NUM_LFO_CELL_DESTS);
                expect(stored::listOf(UV, ModParam::lfoParamId(l, ModParam::LFO_SHAPE)).labels == LFO::shapeLabels());
                expect(stored::listOf(UV, ModParam::lfoParamId(l, ModParam::LFO_TRIG)).labels == LFO::triggerLabels());
                expect(stored::listOf(UV, ModParam::lfoExtParamId(l, ModParam::LFO_DIV)).labels == LFO::divisionLabels());
                for (int off : { ModParam::LFO_RATE, ModParam::LFO_DEPTH, ModParam::LFO_SYNC })
                    expect(! stored::listOf(UV, ModParam::lfoParamId(l, off)));
                for (int off : { ModParam::LFO_PHASE, ModParam::LFO_DELAY, ModParam::LFO_FADE })
                    expect(! stored::listOf(UV, ModParam::lfoExtParamId(l, off)));
                for (int off : { ModParam::LFO_WHEEL, ModParam::LFO_VEL, ModParam::LFO_KTRK, ModParam::LFO_PRES })
                    expect(! stored::listOf(UV, ModParam::lfoExt2ParamId(l, off)), "174-189 are the LFOs' amounts on this engine, not the router's STRIP / XIN cells");
            }
            for (int env = 0; env < ModParam::MAX_ENVS; ++env)
            {
                const auto list = stored::listOf(UV, ModParam::envParamId(env, ModParam::ENV_TARGET));
                expect(list.labels == UnifiedVoice::envDestLabels() && list.count == UnifiedVoice::NUM_ENV_DESTS);
                expectEquals(juce::String(list.labels[list.off]), juce::String("OFF"));
                expect(! stored::listOf(UV, ModParam::envParamId(env, ModParam::ENV_DEPTH)));
            }
            expect(! stored::listOf(UV, ModParam::LFO_COUNT) && ! stored::listOf(UV, ModParam::ENV_COUNT));
            //   the pressure slots (the AT ROUTE screen), on every engine with a router
            for (int e : { UV, FM, MACRO, WT, WSEQ, SMP, DRUM, RSN })
                for (int s = 0; s < PressureSlotParam::MAX_SLOTS; ++s)
                {
                    const auto list = stored::listOf(e, PressureSlotParam::id(s, PressureSlotParam::DEST_OFFSET));
                    const auto tables = stored::tablesOf(e);
                    expect(list.labels == tables.dests->pressureLabels() && list.count == tables.dests->pressureChoices(), "a pressure DEST is a place in the pressure slots' own list");
                    expect(! stored::listOf(e, PressureSlotParam::id(s, PressureSlotParam::AMT_OFFSET)));
                    // ... its hidden entries by their names, and an effect cell by the name AT ROUTE shows
                    for (int i = 0; i < tables.dests->pressureChoices(); ++i)
                        if (const char* name = stored::nameOf(e, PressureSlotParam::id(s, PressureSlotParam::DEST_OFFSET), (float) i))
                            if (stored::valueOf(e, PressureSlotParam::id(s, PressureSlotParam::DEST_OFFSET), name) != i)
                            {
                                expect(false, juce::String(stored::engineLabel(e)) + ": pressure DEST " + juce::String(i) + " (" + name + ") does not come back");
                                break;
                            }
                }
            //   a pressure range of a position is a position of the same list
            expect(stored::listOf(FM, RangeParam::LO_BASE + 165).labels == stored::listOf(FM, 165).labels);
            expect(stored::listOf(FM, RangeParam::HI_BASE + 701).labels == stored::listOf(FM, 701).labels);
            expect(! stored::listOf(FM, RangeParam::LO_BASE + 159));
            //   the kit's eighteen types, the first ten as the TYPE cell offers them
            for (int i = 0; i < DrumEngine::NUM_DRUM_TYPES; ++i)
                expectEquals(juce::String(stored::drumTypeLabels()[i]), juce::String(DrumEngine::typeNames()[i]));
        }

        beginTest("A name is one entry: no list has two entries of one name");
        {
            for (int e : { UV, FM, MACRO, WT, WSEQ, SMP, DRUM, RSN })
            {
                std::vector<int> ids { e == UV ? 158 : 165, 701, 700, PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET) };
                if (e == UV)
                {
                    ids.push_back(ModParam::lfoParamId(0, ModParam::LFO_DEST));
                    ids.push_back(ModParam::envParamId(0, ModParam::ENV_TARGET));
                }
                if (e == DRUM)
                    ids.push_back(DrumEngine::TYPE);
                for (int id : ids)
                {
                    const auto list = stored::listOf(e, id);
                    expect((bool) list, juce::String(stored::engineLabel(e)) + " id " + juce::String(id));
                    juce::StringArray seen;
                    const int all = juce::jmax(list.count, list.moreCount);
                    for (int i = 0; i < all; ++i)
                    {
                        const juce::String name = list.at(i);
                        expect(! seen.contains(name), juce::String(stored::engineLabel(e)) + " " + list.what + ": \"" + name + "\" twice");
                        seen.add(name);
                    }
                }
            }
            // The macro oscillator's four macros have a name a model, and one model's name is another
            // macro's in the next (which is why a file and a preset use the first model's alone):
            // every model's table has the same entries at the same places but for those four names.
            const auto& va = MacroOscEngine::lfoDests(0);
            bool clash = false;
            for (int model = 1; model < 8; ++model)
            {
                const auto& table = MacroOscEngine::lfoDests(model);
                expectEquals(table.visible(), va.visible());
                for (int d = 0; d < table.visible(); ++d)
                {
                    const bool macro = d >= MacroOscVoice::LfoTimbre && d <= MacroOscVoice::LfoMorph;
                    if (! macro)
                        expectEquals(juce::String(table.labels()[d]), juce::String(va.labels()[d]), "model " + juce::String(model) + " entry " + juce::String(d));
                    else if (const int elsewhere = va.indexOfLabel(table.labels()[d]); elsewhere >= 0 && elsewhere != d)
                        clash = true;
                    for (int other = 1; macro && other < model; ++other)
                        if (const int e2 = MacroOscEngine::lfoDests(other).indexOfLabel(table.labels()[d]); e2 >= 0 && e2 != d)
                            clash = true;
                }
            }
            expect(clash, "(if no model's macro name is another's any more, a file could read any model's names)");
        }

        // =========================================================================================
        beginTest("Format 3: every kind of position there and back");
        {
            struct Case { int engine; int id; const char* name; };
            const Case cases[] = {
                { FM, 165, "CUT" }, { FM, 173, "INDX" }, { FM, 174, "PTCH" }, { FM, 182, "PM1" }, { FM, 186, "2:C3" },
                { FM, 160, "SAW" }, { FM, 161, "XKEY" }, { FM, 163, "1/8T" },
                { FM, 700, "LFO2" }, { FM, 701, "L1RT" }, { FM, 703, "OP3" }, { FM, 705, "AMT2" }, { FM, 709, "3:WT" },
                { FM, 100, "LADR" }, { FM, PressureSlotParam::id(1, PressureSlotParam::DEST_OFFSET), "INDX" },
                { FM, RangeParam::LO_BASE + 165, "PAN" },
                { WT, 100, "DIOD" }, { WT, 116, "COMB" }, { WT, 165, "POS" }, { SMP, 26, "SEM" }, { RSN, 100, "LADR" },
                { MACRO, 165, "BCNT" }, { WSEQ, 701, "ETIM" }, { RSN, 701, "MRPH" },
                { UV, ModParam::lfoParamId(0, ModParam::LFO_DEST), "CUT" }, { UV, ModParam::lfoParamId(3, ModParam::LFO_DEST), "4:C2" },
                { UV, ModParam::lfoParamId(1, ModParam::LFO_SHAPE), "EXPU" }, { UV, ModParam::lfoParamId(1, ModParam::LFO_TRIG), "HOLD" },
                { UV, ModParam::lfoExtParamId(2, ModParam::LFO_DIV), "1/16" }, { UV, ModParam::envParamId(0, ModParam::ENV_TARGET), "PW" },
                { UV, ModParam::envParamId(1, ModParam::ENV_TARGET), "OFF" }, { UV, UnifiedParam::MODEL_A, "LADR" }, { UV, UnifiedParam::MODEL_B, "OFF" },
                { UV, 158, "CUT" }, { UV, 166, "RING" }, { UV, 700, "ENV3" }, { UV, 701, "L4DP" },
                { DRUM, 3 * 8 + DrumEngine::TYPE, "KSYN" }, { DRUM, 11 * 8 + DrumEngine::TYPE, "SNAR" },
                { DRUM, PadModParam::id(5, PadModParam::LFO_DEST), "TONE" }, { DRUM, PadModParam::id(5, PadModParam::LFO_SHAPE), "RND" },
                { DRUM, PadModParam::id(5, PadModParam::LFO_TRIGGER), "XONE" }, { DRUM, PadModParam::id(5, PadModParam::LFO_DIV), "1BAR" },
                { DRUM, PadModParam::id(9, PadModParam::SLOT1_SRC), "VEL" }, { DRUM, PadModParam::id(9, PadModParam::SLOT2_DEST), "MRPH" },
                { DRUM, PadModParam::id(9, PadModParam::SLOT2_VIA), "PRES" },
            };
            for (int e : { UV, FM, MACRO, WT, WSEQ, SMP, DRUM, RSN })
            {
                ProjectData data;
                data.name = "there and back";
                data.patterns[0].tracks.resize(1);
                auto& track = data.patterns[0].tracks[0];
                track.engineType = (EngineType) e;
                TrackData::StepData step;
                step.trig = true;
                int used = 0;
                for (auto& c : cases)
                    if (c.engine == e)
                    {
                        const int value = stored::valueOf(e, c.id, c.name);
                        expect(value > 0 || juce::String(c.name) == "OFF" || juce::String(c.name) == "SEM", juce::String(stored::engineLabel(e)) + " id " + juce::String(c.id) + " has " + c.name);
                        track.soundParams[c.id] = (float) value;
                        step.paramLocks[c.id] = (float) value;
                        ++used;
                    }
                track.soundParams[1] = 0.25f;                // (and a cell that is no position)
                track.steps.push_back(step);

                const juce::String json = data.toJSON();
                auto parsed = juce::JSON::parse(json);
                expectEquals((int) parsed["version"], 3);
                // Every position carries its name, in the sound and in the lock; nothing else does.
                int names = 0;
                auto* trackVar = parsed["patterns"][0]["tracks"][0].getDynamicObject();
                for (auto& p : *trackVar->getProperty("params").getArray())
                    names += p.hasProperty("n") ? 1 : 0;
                expectEquals(names, used, juce::String(stored::engineLabel(e)) + ": names in the sound");
                names = 0;
                for (auto& l : *trackVar->getProperty("steps")[0]["locks"].getArray())
                    names += l.hasProperty("n") ? 1 : 0;
                expectEquals(names, used, juce::String(stored::engineLabel(e)) + ": names in the locks");

                // THE NAME DECIDES. The same file with the numbers a shorter list would have given
                // (as if every list had grown by three entries in its middle since): every route is kept.
                for (auto* list : { trackVar->getProperty("params").getArray(), trackVar->getProperty("steps")[0]["locks"].getArray() })
                    for (auto& p : *list)
                        if (p.hasProperty("n"))
                            p.getDynamicObject()->setProperty("v", juce::jmax(0.0, (double) p["v"] - 3.0));
                for (const auto& loaded : { ProjectData::fromJSON(json), ProjectData::fromVar(parsed) })
                {
                    const auto& t = loaded.patterns[0].tracks[0];
                    expect(loaded.lost.isEmpty(), "nothing lost: " + loaded.lost.joinIntoString("; "));
                    for (auto& c : cases)
                        if (c.engine == e)
                        {
                            expectEquals(juce::String(stored::nameOf(e, c.id, at(t.soundParams, c.id))), juce::String(c.name),
                                         juce::String(stored::engineLabel(e)) + " id " + juce::String(c.id));
                            expectEquals(at(t.steps[0].paramLocks, c.id), at(t.soundParams, c.id), "the lock as the cell");
                        }
                    expectEquals(at(t.soundParams, 1), 0.25f);
                }
            }
        }

        beginTest("Format 3: a name the list no longer has is OFF, said once; a cell without a name is its number");
        {
            ProjectData data;
            data.name = "lost";
            data.patterns[0].tracks.resize(2);
            for (auto& track : data.patterns[0].tracks)
            {
                track.engineType = EngineType::FM;
                track.soundParams[165] = (float) stored::valueOf(FM, 165, "CUT");
                track.soundParams[173] = (float) stored::valueOf(FM, 165, "CUT");
                track.soundParams[701] = (float) stored::valueOf(FM, 701, "AMT3");
                track.soundParams[700] = (float) stored::valueOf(FM, 700, "LFO1");
                track.soundParams[160] = (float) (int) LFO::Shape::Saw;
                track.soundParams[174] = (float) stored::valueOf(FM, 165, "PAN");
            }
            auto parsed = juce::JSON::parse(data.toJSON());
            for (int t = 0; t < 2; ++t)
                for (auto& p : *parsed["patterns"][0]["tracks"][t]["params"].getArray())
                {
                    auto* o = p.getDynamicObject();
                    const int id = (int) p["id"];
                    if (id == 165 || id == 173 || id == 701) o->setProperty("n", "GONE");      // an entry that was taken out of the list
                    if (id == 700)                           o->setProperty("n", "NOSRC");
                    if (id == 160)                           o->setProperty("n", "WOBL");       // a shape that is not there
                    if (id == 174)                           o->removeProperty("n");            // a cell that was written without one
                }
            const auto loaded = ProjectData::fromVar(parsed);
            for (auto& track : loaded.patterns[0].tracks)
            {
                expectEquals(at(track.soundParams, 165), 0.0f, "DEST: OFF");
                expectEquals(at(track.soundParams, 173), 0.0f);
                expectEquals(at(track.soundParams, 701), 0.0f, "a slot's DEST: OFF");
                expectEquals(at(track.soundParams, 700), 0.0f, "SRC: OFF");
                expectEquals(at(track.soundParams, 160), (float) (int) LFO::Shape::Saw, "a list without an OFF keeps the number");
                expectEquals(juce::String(stored::nameOf(FM, 174, at(track.soundParams, 174))), juce::String("PAN"), "no name: the number");
            }
            // Said once a name and list, however many cells and tracks held it.
            expectEquals(loaded.lost.size(), 4, loaded.lost.joinIntoString(" / "));
            expect(loaded.lost.joinIntoString(" ").contains("GONE") && loaded.lost.joinIntoString(" ").contains("WOBL"));

            // ... and it reaches the log, a line a thing.
            struct Catch : juce::Logger
            {
                juce::StringArray lines;
                void logMessage(const juce::String& m) override { lines.add(m); }
            } log;
            auto* before = juce::Logger::getCurrentLogger();
            juce::Logger::setCurrentLogger(&log);
            (void) ProjectData::fromVar(parsed);
            juce::Logger::setCurrentLogger(before);
            int said = 0;
            for (auto& line : log.lines)
                said += line.startsWith("[Format] project lost:") ? 1 : 0;
            expectEquals(said, 4, "the app's log has what was lost, once each");
        }

        beginTest("Format 3: presets, multi presets and their steps, the ARCH memory");
        {
            Preset p;
            p.name = "By Name";
            p.engineType = EngineType::Wavetable;
            p.soundParams[165] = (float) stored::valueOf(WT, 165, "POS");
            p.soundParams[705] = (float) stored::valueOf(WT, 705, "L2DP");
            p.soundParams[116] = (float) stored::valueOf(WT, 116, "LADR");
            auto var = juce::JSON::parse(p.toJSON());
            expectEquals((int) var["version"], 3);
            for (auto& cell : *var["params"].getArray())
            {
                expect(cell.hasProperty("n"));
                cell.getDynamicObject()->setProperty("value", 1.0);     // (wrong numbers: the names decide)
            }
            const auto back = Preset::fromVar(var);
            expectEquals(juce::String(stored::nameOf(WT, 165, at(back.soundParams, 165))), juce::String("POS"));
            expectEquals(juce::String(stored::nameOf(WT, 705, at(back.soundParams, 705))), juce::String("L2DP"));
            expectEquals(juce::String(stored::nameOf(WT, 116, at(back.soundParams, 116))), juce::String("LADR"));
            expectEquals(back.fileVersion, 3);

            // A multi: its members' sounds and the locks of their steps.
            MultiPreset multi;
            multi.name = "Pair";
            MultiPreset::Member member;
            member.preset = p;
            member.hasPattern = true;
            member.length = 1;
            TrackData::StepData step;
            step.trig = true;
            step.paramLocks[173] = (float) stored::valueOf(WT, 173, "SPRD");
            member.steps.push_back(step);
            multi.members.push_back(member);
            auto multiVar = juce::JSON::parse(multi.toJSON());
            auto& lock = (*multiVar["members"][0]["steps"][0]["locks"].getArray()).getReference(0);
            expectEquals(lock["n"].toString(), juce::String("SPRD"));
            lock.getDynamicObject()->setProperty("v", 0.0);
            const auto multiBack = MultiPreset::fromVar(multiVar);
            expectEquals(juce::String(stored::nameOf(WT, 173, at(multiBack.members[0].steps[0].paramLocks, 173))), juce::String("SPRD"),
                         "a multi's step lock is read by its name");
            expectEquals(juce::String(stored::nameOf(WT, 165, at(multiBack.members[0].preset.soundParams, 165))), juce::String("POS"));

            // The ARCH memory of a unified-voice track: its positions by name too.
            UnifiedVoiceEngine engine;
            engine.setParameter(UnifiedParam::ARCH, 1.0f);
            engine.setParameter(ModParam::lfoParamId(0, ModParam::LFO_DEST), (float) (int) UnifiedVoice::LfoDest::PulseWidth);
            engine.setParameter(UnifiedParam::MODEL_A, (float) stored::valueOf(UV, UnifiedParam::MODEL_A, "DIOD"));
            engine.setParameter(UnifiedParam::ARCH, 2.0f);
            ProjectData data;
            data.patterns[0].tracks.resize(1);
            data.patterns[0].tracks[0].engineType = EngineType::UnifiedVoice;
            data.patterns[0].tracks[0].archMemory = engine.captureArchMemory();
            auto projectVar3 = juce::JSON::parse(data.toJSON());
            auto* names = projectVar3["patterns"][0]["tracks"][0]["archNames"].getArray();
            expect(names != nullptr && names->size() > 4, "the memory's positions are named");
            // (numbers of a list that was shorter: every memory cell that has a name moved down one)
            auto* memory = projectVar3["patterns"][0]["tracks"][0]["archMem"].getArray();
            std::vector<float> flat;
            for (auto& v : *memory)
                flat.push_back((float) (double) v);
            int moved = 0;
            TrackData::forEachArchCell(flat, [&](int slot, int id, float& value)
            {
                if (slot == 1 && (id == ModParam::lfoParamId(0, ModParam::LFO_DEST) || id == UnifiedParam::MODEL_A))
                {
                    value -= 1.0f;
                    ++moved;
                }
            });
            expectEquals(moved, 2);
            memory->clear();
            for (float v : flat)
                memory->add((double) v);
            auto restored = ProjectData::fromVar(projectVar3).patterns[0].tracks[0].archMemory;
            int checked = 0;
            TrackData::forEachArchCell(restored, [&](int slot, int id, float& value)
            {
                if (slot != 1)
                    return;
                if (id == ModParam::lfoParamId(0, ModParam::LFO_DEST)) { expectEquals(value, (float) (int) UnifiedVoice::LfoDest::PulseWidth); ++checked; }
                if (id == UnifiedParam::MODEL_A)                        { expectEquals(juce::String(stored::nameOf(UV, id, value)), juce::String("DIOD")); ++checked; }
            });
            expectEquals(checked, 2, "the ARCH slot's DEST and filter type came back by name");
        }

        // =========================================================================================
        beginTest("Old files: v0.9's own lists are the first entries of today's (no published file moves)");
        {
            // Copied from the output of v0.9's own code (c210682, the tree the tag has): what the DEST
            // cell of an LFO and of a STRIP route showed on each engine.
            struct Was { int engine; const char* dest; };
            const Was was[] = {
                { FM, "OFF|PTCH|AMP|PAN|CUT|RESO|INDX|FDBK|DETN|LVL1|LVL2|LVL3|LVL4|LVL5|LVL6|RAT1|RAT2|RAT3|RAT4|RAT5|RAT6|DET1|FDB1|ATK1|DEC1|SUS1|REL1|"
                      "DET2|FDB2|ATK2|DEC2|SUS2|REL2|DET3|FDB3|ATK3|DEC3|SUS3|REL3|DET4|FDB4|ATK4|DEC4|SUS4|REL4|DET5|FDB5|ATK5|DEC5|SUS5|REL5|DET6|FDB6|ATK6|"
                      "DEC6|SUS6|REL6|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT" },
                { MACRO, "OFF|PTCH|AMP|PAN|CUT|RESO|SAWS|SPRD|HARM|MORP|LEVL|ATK|DEC|SUS|REL|ENV|DRIV|FMOR|KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT" },
                { WT, "OFF|PTCH|AMP|PAN|CUT|RESO|POS|SPRD|SUB|MSPD|MDPT|ENVP|VELP|TIME|SPD|GLID|VIBR|ATK|DEC|SUS|REL|VEL|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|"
                      "FREL|EVEL|FVEL|LRAT|LDPT" },
                { WSEQ, "OFF|PTCH|AMP|PAN|CUT|RESO|RATE|XFAD|PRNG|LEVL|ATK|DEC|SUS|REL|VEL|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT|"
                        "P1|P2|P3|P4|P5|P6|P7|P8|P9|P10|P11|P12|P13|P14|P15|P16|L1|L2|L3|L4|L5|L6|L7|L8|L9|L10|L11|L12|L13|L14|L15|L16|"
                        "T1|T2|T3|T4|T5|T6|T7|T8|T9|T10|T11|T12|T13|T14|T15|T16" },
                { SMP, "OFF|PTCH|AMP|PAN|CUT|RESO|STRT|DRIV|GPOS|SIZE|DENS|SPD|END|LPST|LPEN|SCAN|SPRY|PJIT|SPRD|RVRS|SLIC|FADE|SLEN|SSIZ|XFAD|JIT|SSPR|"
                       "BITS|RATE|LEVL|VELF|ENVF|KTRK|FDRV|ATK|DEC|SUS|REL|VEL|RAND|LRAT|LDPT" },
                { DRUM, "OFF|PTCH|DEC|TONE|DRIV|SNAP|AMP|PAN|SWEP|SWPT|SCRV|SHAP|KNCK|KTON|HOLD|ACRV|LRAT|LDPT" },
            };
            for (auto& w : was)
            {
                const auto today = stored::listOf(w.engine, 165);
                const juce::String name = stored::engineLabel(w.engine);
                const int n = stored::old::entries(w.dest);
                expect(joined(today).startsWith(juce::String(w.dest) + "|"), name + ": v0.9's DEST list is the start of today's");
                // ... which is what the table says of it ("=N"),
                expectEquals(juce::String(stored::old::listAt(w.engine, F::Cell, 0)), "=" + juce::String(n), name + ": stage 0");
                // ... so a v0.9 file's DEST of an LFO and of every STRIP route is the number it was, for every entry.
                for (int i = 0; i < n; ++i)
                {
                    const auto sound = oldSound(w.engine, { { 165, (float) i }, { 173, (float) (n - 1 - i) }, { 174, (float) i }, { 176, (float) i }, { 178, (float) i } });
                    if (at(sound, 165) != (float) i || at(sound, 173) != (float) (n - 1 - i) || at(sound, 174) != (float) i || at(sound, 178) != (float) i)
                    {
                        expect(false, name + ": DEST " + juce::String(i) + " of a v0.9 file moved");
                        break;
                    }
                }
                if (w.engine == MACRO)
                    continue;       // (model 0's names)
            }
            // The lists every engine shares, as v0.9 had them: shapes, trigger modes, divisions, filter types.
            expect(joined(LFO::shapeLabels(), (int) LFO::Shape::COUNT).startsWith("SIN|TRI|SQR|SAW|RND|SRND|RAMP|EXPU|EXPD|"));
            expect(joined(mod::Router::triggerLabels(), mod::Router::NUM_TRIGGERS).startsWith("FREE|KEY|ONE|HALF|HOLD|"));
            expect(joined(LFO::divisionLabels(), LFO::NUM_DIVISIONS).startsWith("8BAR|4BAR|2BAR|1BAR|1/2D|1/2|1/4D|1/2T|1/4|1/8D|1/4T|1/8|1/16D|1/8T|1/16|1/16T|1/32|"));
            expectEquals(joined(stored::filterTypeList()), juce::String("SVF|SEM|LADR|DIOD|COMB|OFF"));
            // The unified voice's LFO and envelope destinations as v0.9 had them.
            const juce::String uvLfo = "OFF|PTCH|CUT|RESO|MIX|AMP|PW|XMOD|CUTB|PAN|LVL1|LVL2|LVL3|NOIS|FFM|PIT2|RAT1|RAT2|RAT3|RAT4|DPT1|DPT2|DPT3|DPT4|"
                                       "TUN2|DET2|DRIV|TUN3|NCOL|1A|1B|2A|2B|3A|3B|SPRD|GLID|DRFT|DRAT|ENV|FDRV|MORP|RESB|BAL|DRVB|TRKB|FATK|FDEC|FSUS|FREL|"
                                       "KEY|FVEL|PMOD|ATK|DEC|SUS|REL|VEL|ATK2|DEC2|SUS2|REL2|ATK3|DEC3|SUS3|REL3|ATK4|DEC4|SUS4|REL4|ATK5|DEC5|SUS5|REL5|EDPT";
            expect(joined(UnifiedVoice::lfoDestCellLabels(), UnifiedVoice::NUM_LFO_CELL_DESTS).startsWith(uvLfo + "|"), "the unified voice's LFO DEST list");
            expectEquals(joined(UnifiedVoice::envDestLabels(), UnifiedVoice::NUM_ENV_DESTS),
                         "AMP|CUT|PTCH|OFF|" + uvLfo.fromFirstOccurrenceOf("RESO|", true, false).replace("|AMP|PW|", "|PW|"), "its extra envelopes' DEST list");
        }

        beginTest("Old files: the tables are what the old code showed (the engines kept under tests/old)");
        {
            witness<OldFMEngine>("FM before its port", FM, "ca01eb0");
            witness<OldMacroOscEngine>("macro before U4-MACRO", MACRO, "b7e8463");
            witness<OldWaveSeqEngine>("wave sequencer before its port", WSEQ, "7a406c1");
            witness<OldSamplerEngine>("sampler before its port", SMP, "fbbbe40");
            witness<OldDrumEngine>("drum kit before its port", DRUM, "871c986");
            witness<OldResonatorEngine>("resonator before its port", RSN, "871c986");
            witness<oldwt::WavetableEngine>("wavetable before its port", WT, "7fe94b3");
            witness<olduv::UnifiedVoiceEngine>("unified voice before its port", UV, "fbbbe40", false);

            // Each engine's last stage is today: a prefix of today's lists and as long as they are
            // (a list that has grown since by entries at its END needs no new stage; this fails when
            // one is SHORTER than the table says, which is a list that lost entries).
            for (int e : { UV, FM, MACRO, WT, WSEQ, SMP, DRUM, RSN })
            {
                const int last = stored::old::stageCount(e) - 1;
                expect(last >= 0);
                for (auto family : { F::Cell, F::Slot, F::Source })
                {
                    const char* list = stored::old::listAt(e, family, last);
                    expect(list != nullptr && list[0] == '=', juce::String(stored::engineLabel(e)) + ": the last stage is today's own list");
                    const int id = family == F::Cell ? (e == UV ? 158 : 165) : (family == F::Slot ? 701 : 700);
                    if (list != nullptr)
                        expect(stored::old::length(list) <= stored::listOf(e, id).count,
                               juce::String(stored::engineLabel(e)) + ": a list is shorter than it was when the table was written");
                }
            }
        }

        beginTest("Old files: every stage of every engine, entry by entry");
        {
            // A sound as a save of that stage holds it - the ids that came with the stage and with
            // every stage before it - and one position; it must come back as the entry the list of
            // that stage had at that place.
            for (int e : { UV, FM, MACRO, WT, WSEQ, SMP, DRUM, RSN })
            {
                int count = 0;
                const auto* stages = stored::history::stagesOf(e, count);
                ParamMap ids;
                for (int s = 0; s < count; ++s)
                {
                    for (int id : evidenceIds(stages[s].evidence))
                        ids[id] = 0.0f;
                    // (a stage with no evidence of its own cannot be told from the one before: it is
                    // read as that one, which the next test says for each such pair)
                    if (s > 0 && juce::String(stages[s].evidence).isEmpty())
                        continue;
                    expectEquals(stored::old::stageOf(e, ids), s, juce::String(stored::engineLabel(e)) + ": the ids of " + stages[s].since);
                    for (auto family : { F::Cell, F::Slot, F::Source })
                    {
                        const juce::String list = written(e, family, s);
                        if (list.isEmpty())
                            continue;
                        const int id = family == F::Cell ? (e == UV ? 158 : 165) : (family == F::Slot ? 701 : 700);
                        const auto today = stored::listOf(e, id);
                        const auto names = juce::StringArray::fromTokens(list, "|", "");
                        for (int i = 0; i < names.size(); ++i)
                        {
                            ParamMap sound = ids;
                            // (the wave sequencer's BODY sat at the XIN ids for ten hours: those cells are moved, not read as DESTs)
                            sound[id] = (float) i;
                            ParamMap locks { { id, (float) i } };
                            const auto data = ProjectData::fromVar(projectVar(2, e, sound, locks));
                            const float now = at(data.patterns[0].tracks[0].soundParams, id);
                            const int expected = today.indexOf(names[i].toRawUTF8());
                            const juce::String where = juce::String(stored::engineLabel(e)) + " " + stages[s].since + " " + today.what + " " + juce::String(i) + " (" + names[i] + ")";
                            if (expected >= 0)
                            {
                                if (now != (float) expected || at(data.patterns[0].tracks[0].steps[0].paramLocks, id) != (float) expected)
                                {
                                    expect(false, where + " is read as " + juce::String(now) + ", not as " + juce::String(expected));
                                    break;
                                }
                            }
                            else if (now != 0.0f || data.lost.isEmpty())        // an entry that exists no more: OFF, and said
                            {
                                expect(false, where + " exists no more and must be OFF and said");
                                break;
                            }
                        }
                    }
                }
            }
        }

        beginTest("Old files: by hand - the free slots' later entries, the lists that grew in their middle");
        {
            const auto name = [&](int e, int id, float value) { return juce::String(stored::nameOf(e, id, value)); };
            const int slot2 = ModSlotParam::id(1, ModSlotParam::DEST);

            // FM. The slots came on 11-21 with the table's 69 entries before L1RT; POS1-6 (ca01eb0) and
            // ETIM .. PMAL (its port) pushed L1RT .. AM16 along twice.
            auto fm = oldSound(FM, { { 701, 69.0f }, { slot2, 73.0f }, { 700, 1.0f } });
            expectEquals(name(FM, 701, at(fm, 701)), juce::String("L1RT"));
            expectEquals(name(FM, slot2, at(fm, slot2)), juce::String("AMT1"));
            fm = oldSound(FM, { { 701, 69.0f }, { slot2, 75.0f }, { 66, 0.0f } });          // (WAV / SMPL cells: after ca01eb0)
            expectEquals(name(FM, 701, at(fm, 701)), juce::String("POS1"));
            expectEquals(name(FM, slot2, at(fm, slot2)), juce::String("L1RT"));
            fm = oldSound(FM, { { 701, 75.0f }, { slot2, 84.0f }, { 66, 0.0f }, { 182, 0.0f } });   // (the XIN page: its port)
            expectEquals(name(FM, 701, at(fm, 701)), juce::String("ETIM"));
            expectEquals(name(FM, slot2, at(fm, slot2)), juce::String("L1RT"));
            // A number the detected list has no entry for was written later: FM's list of 11-21 had 89.
            fm = oldSound(FM, { { 701, 93.0f } });
            expectEquals(name(FM, 701, at(fm, 701)), juce::String("AM15"), "93 fits the next list (95 entries), not the first (89)");

            // The macro oscillator. BODY's second page (395b067) went in before the effect cells: a
            // DEST on an effect cell saved in the two and a half hours between E4 and it is 8 further on.
            auto macro = oldSound(MACRO, { { 165, 40.0f }, { 174, 36.0f }, { 125, 0.0f }, { 182, 0.0f } });
            expectEquals(name(MACRO, 165, at(macro, 165)), juce::String("1:C4"), "37 entries then: 40 is the fourth effect cell");
            expectEquals(at(macro, 165), 48.0f);
            expectEquals(name(MACRO, 174, at(macro, 174)), juce::String("BKTK"));
            macro = oldSound(MACRO, { { 165, 40.0f }, { 125, 0.0f }, { 182, 0.0f }, { 141, 0.0f } });      // (with BDY2's ids: today's list)
            expectEquals(at(macro, 165), 40.0f);
            expectEquals(name(MACRO, 165, 40.0f), juce::String("BHRD"));
            // ... and its slots: 27 entries before L1RT when they came, 29 from U4-0 (no id tells), 37 with BODY.
            macro = oldSound(MACRO, { { 701, 27.0f }, { slot2, 31.0f } });
            expectEquals(name(MACRO, 701, at(macro, 701)), juce::String("L1RT"));
            expectEquals(name(MACRO, slot2, at(macro, slot2)), juce::String("AMT1"));
            macro = oldSound(MACRO, { { 701, 37.0f }, { slot2, 29.0f }, { 125, 0.0f }, { 182, 0.0f } });
            expectEquals(name(MACRO, 701, at(macro, 701)), juce::String("L1RT"));
            expectEquals(name(MACRO, slot2, at(macro, slot2)), juce::String("BMIX"));

            // The wave sequencer. BDY2 went in before ETIM FTIM PM as well.
            auto wseq = oldSound(WSEQ, { { 165, 83.0f }, { 173, 85.0f }, { 174, 86.0f }, { 176, 82.0f }, { 133, 0.0f }, { 182, 0.0f } });
            expectEquals(name(WSEQ, 165, at(wseq, 165)), juce::String("ETIM"));
            expectEquals(name(WSEQ, 173, at(wseq, 173)), juce::String("PM"));
            expectEquals(name(WSEQ, 174, at(wseq, 174)), juce::String("1:C1"));
            expectEquals(name(WSEQ, 176, at(wseq, 176)), juce::String("BKTK"));
            wseq = oldSound(WSEQ, { { 165, 83.0f }, { 133, 0.0f }, { 141, 0.0f }, { 182, 0.0f } });
            expectEquals(name(WSEQ, 165, at(wseq, 165)), juce::String("BCNT"), "with BDY2's ids the number is today's");
            // Its BODY page at the XIN ids (340e9dc to b7e8463): moved to BODY's own, not read as routes.
            wseq = oldSound(WSEQ, { { 182, 0.6f }, { 183, 0.25f }, { 186, 0.9f }, { 189, 1.0f }, { 165, 82.0f } });
            expectEquals(at(wseq, 133), 0.6f);
            expectEquals(at(wseq, 134), 0.25f);
            expectEquals(at(wseq, 137), 0.9f);
            expectEquals(at(wseq, 140), 1.0f);
            expect(wseq.find(182) == wseq.end() && wseq.find(186) == wseq.end() && wseq.find(189) == wseq.end(), "nothing of BODY is left on the XIN page");
            expectEquals(name(WSEQ, 165, at(wseq, 165)), juce::String("BKTK"));

            // The unified voice. Its slots' SRC had LFO3 LFO4 at 11 and 12 until its port; its DEST no L1RT.
            auto uv = oldSound(UV, { { 700, 11.0f }, { 703, 12.0f }, { 701, 75.0f }, { 704, 10.0f } });
            expectEquals(name(UV, 700, at(uv, 700)), juce::String("LFO3"));
            expectEquals(name(UV, 703, at(uv, 703)), juce::String("LFO4"));
            expectEquals(name(UV, 701, at(uv, 701)), juce::String("AMT1"));
            expectEquals(name(UV, 704, at(uv, 704)), juce::String("IN"));
            uv = oldSound(UV, { { 700, 11.0f }, { 701, 75.0f }, { 158, 0.0f } });           // (the STRIP page: its port)
            expectEquals(name(UV, 700, at(uv, 700)), juce::String("XIN"));
            expectEquals(name(UV, 701, at(uv, 701)), juce::String("PM"));
            // Its own LFO and envelope cells never moved.
            uv = oldSound(UV, { { ModParam::lfoParamId(0, ModParam::LFO_DEST), 74.0f }, { ModParam::envParamId(0, ModParam::ENV_TARGET), 74.0f },
                                { ModParam::lfoParamId(0, ModParam::LFO_SHAPE), 8.0f }, { UnifiedParam::MODEL_A, 3.0f } });
            expectEquals(at(uv, ModParam::lfoParamId(0, ModParam::LFO_DEST)), 74.0f);
            expectEquals(at(uv, ModParam::envParamId(0, ModParam::ENV_TARGET)), 74.0f);
            expectEquals(at(uv, ModParam::lfoParamId(0, ModParam::LFO_SHAPE)), 8.0f);
            expectEquals(at(uv, UnifiedParam::MODEL_A), 3.0f);

            // The first resonator engine (three hours and a half of 11-21, replaced by U1): what its
            // list still has by name is kept, the rest is OFF and said.
            juce::StringArray lost;
            auto rsn = oldSound(RSN, { { 165, 9.0f }, { 173, 7.0f }, { 174, 4.0f } }, &lost);
            expectEquals(name(RSN, 165, at(rsn, 165)), juce::String("DCAY"));
            expectEquals(at(rsn, 173), 0.0f, "PICK is no destination any more");
            expectEquals(name(RSN, 174, at(rsn, 174)), juce::String("CUT"));
            expectEquals(lost.size(), 1, lost.joinIntoString("; "));

            // WHAT CANNOT BE TOLD. These stages brought no id: a file of one is read as the stage before.
            //   macro b7e8463 (ETIM FTIM before L1RT, five hours of the night of 11-21): a slot DEST of 27 .. 46
            //   drum  871c986 (the resonator drum's 24 cells before L1RT, nine hours): a slot DEST of 18 .. 37
            //   E4 e208eee on the macro and the wave sequencer: told by the number alone (above)
            for (auto [e, commit] : { std::pair<int, const char*> { MACRO, "b7e8463" }, { DRUM, "871c986" } })
            {
                const int s = stageOfCommit(e, commit);
                int count = 0;
                expect(s > 0 && juce::String(stored::history::stagesOf(e, count)[s].evidence).isEmpty(), juce::String(commit) + " has no id of its own");
            }
        }

        beginTest("Old files: a drum kit of before U2b is put back on its types");
        {
            // The owner's P01, track 2, as it is in ~/Music/Neptune/Projects/P01 (saved 2026-11-20 by
            // the build of 11-16): sixteen pads by v0.9's TYPE numbers, each with the cymbal page's
            // defaults at 528 + pad * 8, the kit's strip routes on DEC and DRIV.
            const int types[16] = { 13, 13, 13, 13, 13, 13, 8, 8, 14, 1, 2, 9, 3, 5, 6, 7 };
            ParamMap kit;
            for (int pad = 0; pad < 16; ++pad)
            {
                const float cells[8] = { 100.0f + 10.0f * (float) pad, 0.3f, 0.5f, 0.1f, 0.5f, 0.8f, 0.5f, (float) types[pad] };
                for (int n = 0; n < 8; ++n)
                    kit[pad * 8 + n] = cells[n];
                const float cymbal[8] = { 2.0f, 0.5f, 0.5f, 0.5f, 0.4f, 0.3f, 0.5f, 0.0f };
                for (int n = 0; n < 8; ++n)
                    kit[528 + pad * 8 + n] = cymbal[n];
                for (int n = 0; n < 8; ++n)
                    kit[400 + pad * 8 + n] = DrumVoice::kickDefault(n);
            }
            kit[400 + 8 * 8 + DrumVoice::KickSweep] = 30.0f;        // (the kick machine's own page, as the owner set it)
            kit[174] = 2.0f;  kit[175] = 0.7925f;  kit[176] = 4.0f;  kit[177] = 0.5f;
            kit[163] = 8.0f;  kit[171] = 8.0f;

            ParamMap locks { { 3 * 8 + DrumEngine::TYPE, 5.0f }, { 528 + 4, 0.9f }, { 8 * 8 + DrumEngine::DECAY, 0.2f } };
            const auto data = ProjectData::fromVar(projectVar(2, DRUM, kit, locks));
            const auto& sound = data.patterns[0].tracks[0].soundParams;
            expect(data.lost.isEmpty(), data.lost.joinIntoString("; "));

            using T = DrumSynthType;
            const T expected[16] = { T::SubBass, T::SubBass, T::SubBass, T::SubBass, T::SubBass, T::SubBass, T::Cymbal, T::Cymbal,
                                     T::KickSynth, T::AnalogKick, T::ModCyclesKick, T::Resonator, T::Resonator, T::Clap, T::OscBank, T::OscBank };
            for (int pad = 0; pad < 16; ++pad)
                expectEquals((int) at(sound, pad * 8 + DrumEngine::TYPE), (int) expected[pad], "pad " + juce::String(pad + 1) + " TYPE");
            // The tom and the snare are the resonator drum's sounds of those names, the hats the bank's.
            expectEquals(at(sound, 528 + 11 * 8), (float) resonatorSoundIndex("TOM"));
            expectEquals(at(sound, 528 + 12 * 8), (float) resonatorSoundIndex("SNAR"));
            expectEquals(at(sound, 528 + 14 * 8 + OscBank::ParModel), (float) OscBank::Hat);
            expectEquals(at(sound, 528 + 15 * 8 + OscBank::ParModel), (float) OscBank::OpenHat);
            // The kick machine keeps its own page; its second page is at rest. The two old kicks are its points.
            expectEquals(at(sound, 400 + 8 * 8 + DrumVoice::KickSweep), 30.0f);
            for (int n = 0; n < 8; ++n)
                expectEquals(at(sound, 528 + 8 * 8 + n), 0.0f, "KCK2 cell " + juce::String(n));
            const auto point = DrumVoice::kickPoint(T::AnalogKick, 190.0f, 0.3f, 0.5f, 0.1f, 0.5f, 0.8f);
            expectEquals(at(sound, 400 + 9 * 8 + DrumVoice::KickSweep), point.kick[DrumVoice::KickSweep]);
            expectEquals(at(sound, 528 + 9 * 8 + DrumVoice::KickPath), (float) DrumVoice::PathDrum);
            // Nothing of the cymbal page is left where a pad's second page is now.
            for (int pad : { 0, 5, 6, 7, 13 })
                for (int n = 0; n < 8; ++n)
                    expect(sound.find(528 + pad * 8 + n) == sound.end(), "pad " + juce::String(pad + 1) + " keeps no cymbal-page cell");
            // The kit's routes are what they were (DEC, DRIV), and its divisions.
            expectEquals(juce::String(stored::nameOf(DRUM, 174, at(sound, 174))), juce::String("DEC"));
            expectEquals(juce::String(stored::nameOf(DRUM, 176, at(sound, 176))), juce::String("DRIV"));
            expectEquals(at(sound, 163), 8.0f);
            // The step's locks: a TYPE lock by the old number, a cymbal-page lock gone, a pad cell kept.
            const auto& stepLocks = data.patterns[0].tracks[0].steps[0].paramLocks;
            expectEquals((int) at(stepLocks, 3 * 8 + DrumEngine::TYPE), (int) T::Clap);
            expect(stepLocks.find(528 + 4) == stepLocks.end());
            expectEquals(at(stepLocks, 8 * 8 + DrumEngine::DECAY), 0.2f);

            // In an engine, as a project applies it (the cells in the order of their ids): the pads ARE those drums.
            DrumEngine engine;
            for (auto& [id, value] : sound)
                engine.setParameter(id, value);
            const T voices[16] = { T::SubBass, T::SubBass, T::SubBass, T::SubBass, T::SubBass, T::SubBass, T::Cymbal, T::Cymbal,
                                   T::KickSynth, T::KickSynth, T::KickSynth, T::Resonator, T::Resonator, T::Clap, T::OscBank, T::OscBank };
            for (int pad = 0; pad < 16; ++pad)
                expectEquals((int) engine.getVoice(pad).getType(), (int) voices[pad], "pad " + juce::String(pad + 1) + " plays");
            expectEquals(engine.getParameter(528 + 11 * 8), (float) resonatorSoundIndex("TOM"));
            expectEquals(engine.getParameter(528 + 12 * 8), (float) resonatorSoundIndex("SNAR"));
            // ... and the kick machine's second page as the engine itself leaves one it has just made.
            DrumEngine fresh;
            fresh.setParameter(0 * 8 + DrumEngine::TYPE, (float) (int) T::Clap);
            fresh.setParameter(0 * 8 + DrumEngine::TYPE, (float) (int) T::KickSynth);
            for (int n = 0; n < 8; ++n)
                expectEquals(engine.getParameter(528 + 8 * 8 + n), fresh.getParameter(528 + n), "KCK2 cell " + juce::String(n) + " as a new kick machine has it");

            // What today's build makes of the SAME file without the translation (the defect this ends):
            DrumEngine scrambled;
            for (auto& [id, value] : kit)
                scrambled.setParameter(id, value);
            expect(scrambled.getVoice(0).getType() != T::SubBass && scrambled.getVoice(8).getType() != T::KickSynth,
                   "read by today's numbers the kit comes back with other drums on its pads");

            // A cymbal machine pad (CSYN, v0.9's number 15): the cymbal its MODL named.
            ParamMap cymbals;
            for (int pad = 0; pad < 2; ++pad)
            {
                const float cells[8] = { 400.0f, 1.5f, 0.5f, 0.0f, 0.5f, 0.7f, 0.5f, 15.0f };
                for (int n = 0; n < 8; ++n)
                    cymbals[pad * 8 + n] = cells[n];
            }
            cymbals[528] = 4.0f;                                    // CRSH
            cymbals[528 + 8] = 1.0f;  cymbals[528 + 8 + 1] = 0.8f;  // RING, PING 80
            const auto made = oldSound(DRUM, cymbals);
            expectEquals((int) at(made, DrumEngine::TYPE), (int) T::Resonator);
            expectEquals(at(made, 528), (float) resonatorSoundIndex("CRSH"));
            expectEquals((int) at(made, 8 + DrumEngine::TYPE), (int) T::OscBank);
            expectEquals(at(made, 528 + 8 + OscBank::ParModel), (float) OscBank::Ring);
            expectEquals(at(made, 528 + 8 + OscBank::ParPing), 0.8f);

            // A kit that holds what the kit's port brought (a pad's WAVE, the XIN page) is of today's numbers.
            ParamMap today = kit;
            today[128] = 0.0f;
            today[DrumEngine::TYPE] = (float) (int) T::Resonator;
            today[528] = 7.0f;
            const auto kept = oldSound(DRUM, today);
            expectEquals((int) at(kept, DrumEngine::TYPE), (int) T::Resonator);
            expectEquals(at(kept, 528), 7.0f);
            expectEquals((int) at(kept, 8 * 8 + DrumEngine::TYPE), 14, "its numbers are left as they are");

            // A preset file of the same age (no "version" at all) is read the same way.
            Preset preset;
            preset.name = "old kit";
            preset.engineType = EngineType::Drum;
            preset.soundParams = kit;
            auto presetVar = juce::JSON::parse(preset.toJSON());
            presetVar.getDynamicObject()->removeProperty("version");
            for (auto& cell : *presetVar["params"].getArray())
                cell.getDynamicObject()->removeProperty("n");
            const auto oldPreset = Preset::fromVar(presetVar);
            expectEquals((int) at(oldPreset.soundParams, DrumEngine::TYPE), (int) T::SubBass);
            expectEquals((int) at(oldPreset.soundParams, 12 * 8 + DrumEngine::TYPE), (int) T::Resonator);
            expectEquals(oldPreset.fileVersion, 1);
        }

        beginTest("Old files: P01's stored sounds are read number for number; saved again they are format 3");
        {
            const auto file = juce::File(NEPTUNE_TESTS_DIR).getChildFile("fixtures/p01_keyboard.json");
            expect(file.existsAsFile());
            const auto raw = juce::JSON::parse(file.loadFileAsString());
            expectEquals((int) raw["version"], 2);
            const auto data = ProjectData::fromVar(raw);
            expect(data.lost.isEmpty(), data.lost.joinIntoString("; "));
            int cells = 0;
            for (int p = 0; p < raw["patterns"].size(); ++p)
                for (int t = 0; t < raw["patterns"][p]["tracks"].size(); ++t)
                    if (auto* params = raw["patterns"][p]["tracks"][t]["params"].getArray())
                    {
                        const auto& sound = data.patterns[(size_t) p].tracks[(size_t) t].soundParams;
                        expectEquals((int) sound.size(), params->size());
                        for (auto& cell : *params)
                        {
                            ++cells;
                            if (at(sound, (int) cell["id"]) != (float) (double) cell["v"])
                                expect(false, "P01 pattern " + juce::String(p) + " track " + juce::String(t) + " id " + cell["id"].toString() + " changed");
                        }
                    }
            expect(cells > 300, "the two sounds were compared (" + juce::String(cells) + " cells)");
            // Saved by this build and read again: the same numbers, by name where they are positions.
            const auto again = ProjectData::fromJSON(data.toJSON());
            for (size_t p = 0; p < data.patterns.size(); ++p)
                for (size_t t = 0; t < data.patterns[p].tracks.size(); ++t)
                    expect(again.patterns[p].tracks[t].soundParams == data.patterns[p].tracks[t].soundParams);
            expect(data.toJSON().contains("\"n\""));
        }

        // =========================================================================================
        // NEPTUNE_FORMAT_FILES=<a folder or a file>: what reading the projects and presets there
        // changes, cell by cell (nothing is written: the files are only read). For the owner's own
        // files: NEPTUNE_FORMAT_FILES=~/Music/Neptune NEPTUNE_TEST_ONLY="Format V3" <the test binary>.
        if (const auto where = juce::SystemStats::getEnvironmentVariable("NEPTUNE_FORMAT_FILES", {}); where.isNotEmpty())
        {
            beginTest("What reading the files of " + where + " changes (read only)");
            const juce::File root(where);
            juce::Array<juce::File> files;
            if (root.isDirectory())
                files = root.findChildFiles(juce::File::findFiles, true, "*.json");
            else
                files.add(root);
            files.sort();
            const auto said = [&](const juce::String& file, int e, const ParamMap& raw, const ParamMap& now)
            {
                int count = 0;
                const auto* stages = stored::history::stagesOf(e, count);
                const int stage = count > 0 ? stored::old::stageOf(e, raw) : 0;
                juce::String line = "  " + file + ": " + stored::engineLabel(e) + ", " + juce::String((int) raw.size()) + " cells, read as "
                                    + (count > 0 ? juce::String(stages[stage].since) + (stage == 0 ? " (the public release's layout)" : "") : juce::String("-"));
                int changed = 0;
                juce::String detail;
                for (auto& [id, value] : raw)
                {
                    const auto it = now.find(id);
                    if (it == now.end())                    { ++changed; if (changed <= 40) detail << "\n      id " << id << " (" << value << ") taken out"; }
                    else if (it->second != value)
                    {
                        ++changed;
                        const char* entry = stored::nameOf(e, id, it->second);
                        if (changed <= 40)
                            detail << "\n      id " << id << ": " << value << " -> " << it->second << (entry != nullptr ? juce::String(" (") + entry + ")" : juce::String());
                    }
                }
                for (auto& [id, value] : now)
                    if (raw.find(id) == raw.end())          { ++changed; if (changed <= 40) detail << "\n      id " << id << " = " << value << " added"; }
                logMessage(line + (changed == 0 ? ": nothing changes" : ": " + juce::String(changed) + " cells change" + detail));
            };
            for (auto& file : files)
            {
                const auto var = juce::JSON::parse(file.loadFileAsString());
                auto* obj = var.getDynamicObject();
                if (obj == nullptr)
                    continue;
                const juce::String name = file.getRelativePathFrom(root.isDirectory() ? root : root.getParentDirectory());
                const auto rawOf = [](const juce::var& params, const char* key)
                {
                    ParamMap raw;
                    if (auto* list = params.getArray())
                        for (auto& cell : *list)
                            raw[(int) cell["id"]] = (float) (double) cell[key];
                    return raw;
                };
                if (obj->getProperty("patterns").isArray())
                {
                    const auto data = ProjectData::fromVar(var);
                    logMessage(name + " (project, version " + obj->getProperty("version").toString() + ")");
                    for (int t = 0; t < (int) data.patterns[(size_t) data.activePattern].tracks.size(); ++t)
                    {
                        const auto& track = data.patterns[(size_t) data.activePattern].tracks[(size_t) t];
                        const auto raw = rawOf(var["patterns"][data.activePattern]["tracks"][t]["params"], "v");
                        said("track " + juce::String(t + 1), (int) track.engineType, raw, track.soundParams);
                    }
                    for (auto& line : data.lost)
                        logMessage("  LOST: " + line);
                }
                else if (obj->getProperty("params").isArray() && ! obj->hasProperty("members"))
                {
                    const auto preset = Preset::fromVar(var);
                    logMessage(name + " (preset, " + (obj->hasProperty("version") ? "version " + obj->getProperty("version").toString() : juce::String("no version")) + ")");
                    said(preset.name, (int) preset.engineType, rawOf(obj->getProperty("params"), "value"), preset.soundParams);
                    for (auto& line : preset.lost)
                        logMessage("  LOST: " + line);
                }
            }
            expect(! files.isEmpty(), "no file found");
        }

        // =========================================================================================
        beginTest("Factory presets and multis name nothing that is not there, and no route aims at nothing");
        {
            // (The list is not emptied first: the multis and their routes are made once a process,
            // and a name one of them could not place must still be here, whoever made them.)
            PresetManager manager;
            std::vector<Preset> all;
            for (int i = 0; i < manager.getPresetCount(); ++i)
                all.push_back(manager.getPreset(i));
            for (int m = 0; m < manager.getMultiCount(); ++m)
                for (auto& member : manager.getMulti(m).members)
                    all.push_back(member.preset);
            for (auto& kit : resonatorKits())
                all.push_back(kit);
            expect(all.size() > 150, juce::String((int) all.size()) + " sounds written in code");
            expect(stored::unknownNames().isEmpty(), "a preset written in code names what does not exist: " + stored::unknownNames().joinIntoString("; "));

            int routes = 0;
            for (auto& preset : all)
            {
                const int e = (int) preset.engineType;
                const auto& p = preset.soundParams;
                const auto has = [&](int id) { const auto it = p.find(id); return it != p.end() && it->second != 0.0f; };
                // Every position it holds is an entry of its list.
                for (auto& [id, value] : p)
                    if (const auto list = stored::listOf(e, id))
                        expect(stored::nameOf(e, id, value) != nullptr && value == std::round(value),
                               preset.name + ": id " + juce::String(id) + " holds " + juce::String(value) + ", which is no entry of its list (" + list.what + ")");
                if (stored::tablesOf(e).dests == nullptr)
                    continue;
                // A route with something to say that aims at OFF: a name that was not found, whoever looked.
                for (int s = 0; s < ModSlotParam::MAX_SLOTS; ++s)
                    if (has(ModSlotParam::id(s, ModSlotParam::SRC)) && has(ModSlotParam::id(s, ModSlotParam::AMT)))
                    {
                        expect(has(ModSlotParam::id(s, ModSlotParam::DEST)), preset.name + ": slot " + juce::String(s + 1) + " has a source and an amount and no destination");
                        ++routes;
                    }
                    else if (has(ModSlotParam::id(s, ModSlotParam::DEST)) && has(ModSlotParam::id(s, ModSlotParam::AMT)))
                        expect(false, preset.name + ": slot " + juce::String(s + 1) + " has a destination and an amount and no source");
                if (e == UV)
                    continue;       // (its STRIP / XIN pages are at ids of its own, and its LFOs' DPTH is 0.3 at rest)
                for (int l = 0; l < 2; ++l)
                    if (p.count(mod::Router::lfoId(l, mod::Router::DestCell)) != 0 && has(mod::Router::lfoId(l, mod::Router::Depth)))
                    {
                        expect(has(mod::Router::lfoId(l, mod::Router::DestCell)), preset.name + ": LFO " + juce::String(l + 1) + " is given a DEST that is OFF, and a depth");
                        ++routes;
                    }
                for (int r = 0; r < mod::Router::NUM_XIN; ++r)
                    if (has(mod::Router::xinId(r * 4 + mod::Router::XinAmount1)))
                        expect(has(mod::Router::xinId(r * 4 + mod::Router::XinDest1)), preset.name + ": XIN route " + juce::String(r + 1) + " has an amount and no destination");
            }
            expect(routes > 20, juce::String(routes) + " routes looked at");

            // A name that does not exist is remembered (and is 0), not silently OFF: in ONE list,
            // whichever lookup was used (PresetRoute's, or stored::placeOf under the resonator presets).
            expect(&PresetRoute::unresolved() == &stored::unknownNames(), "one list of names that were not found");
            expectEquals(PresetRoute::cellDest(EngineType::FM, "CUT"), (float) stored::valueOf(FM, 165, "CUT"));
            expectEquals(PresetRoute::source(EngineType::FM, "OP2"), (float) stored::valueOf(FM, 700, "OP2"));
            expectEquals(PresetRoute::slotDest(EngineType::FM, "L2RT"), (float) stored::valueOf(FM, 701, "L2RT"));
            expectEquals(PresetRoute::cellDest(EngineType::MacroOsc, "SAWS"), (float) (int) MacroOscVoice::LfoTimbre, "a macro by its one name");
            // (a list whose first entry is an entry: the place 0 is found, not missed)
            expectEquals(PresetRoute::shape(LFO::shapeLabels()[0]), 0.0f);
            expectEquals(PresetRoute::division(LFO::divisionLabels()[0]), 0.0f);
            expect(stored::unknownNames().isEmpty(), stored::unknownNames().joinIntoString("; "));
            expectEquals(PresetRoute::cellDest(EngineType::FM, "NOSUCH"), 0.0f);        // (a JUCE assertion line is expected here)
            expectEquals(PresetRoute::source(EngineType::FM, "NOSRC"), 0.0f);
            expectEquals(PresetRoute::slotDest(EngineType::FM, "L9RT"), 0.0f);
            expectEquals(stored::placeOf(LFO::divisionLabels(), LFO::NUM_DIVISIONS, "1/7", "tempo division"), 0.0f);
            expectEquals(stored::unknownNames().size(), 4, "each remembered: " + stored::unknownNames().joinIntoString("; "));
            // A DEST, a SRC and a VIA begin with OFF: a route is not written to "OFF" by name.
            expectEquals(PresetRoute::cellDest(EngineType::FM, "OFF"), 0.0f);
            expectEquals(PresetRoute::source(EngineType::FM, mod::SourceTable::standard(2).labels()[0]), 0.0f);
            expectEquals(stored::unknownNames().size(), 6, "OFF by name is a miss: " + stored::unknownNames().joinIntoString("; "));
            stored::unknownNames().clear();
        }

        beginTest("The constants the factory presets route by are the entries they say");
        {
            // A factory preset and an engine's default routes use the voices' own enums for a
            // destination. An enum is a name only while it agrees with its table: these are the ones
            // used (FactoryPresets.h, ArchitecturePreset.h, the engines' strip defaults).
            using U = UnifiedVoice::LfoDest;
            struct Constant { int engine; int id; int value; const char* is; };
            const int uvDest = ModParam::lfoParamId(0, ModParam::LFO_DEST);
            const Constant constants[] = {
                { UV, uvDest, (int) U::Pitch, "PTCH" }, { UV, uvDest, (int) U::Cutoff, "CUT" }, { UV, uvDest, (int) U::Resonance, "RESO" },
                { UV, uvDest, (int) U::OscMix, "MIX" }, { UV, uvDest, (int) U::Amp, "AMP" }, { UV, uvDest, (int) U::PulseWidth, "PW" },
                { UV, uvDest, (int) U::CrossMod, "XMOD" }, { UV, uvDest, (int) U::Osc2Pitch, "PIT2" },
                { UV, 158, (int) UnifiedVoice::RCutoff, "CUT" }, { UV, 158, (int) UnifiedVoice::RPm, "PM" }, { UV, 158, (int) UnifiedVoice::RRing, "RING" },
                { UV, 158, (int) UnifiedVoice::RSync, "SYNC" },
                { MACRO, 165, (int) MacroOscVoice::LfoPitch, "PTCH" }, { MACRO, 165, (int) MacroOscVoice::LfoPan, "PAN" },
                { MACRO, 165, (int) MacroOscVoice::LfoTimbre, "SAWS" }, { MACRO, 165, (int) MacroOscVoice::LfoColor, "SPRD" },
                { MACRO, 165, (int) MacroOscVoice::LfoHarmonics, "HARM" }, { MACRO, 165, (int) MacroOscVoice::LfoMorph, "MORP" },
                { WT, 165, (int) WavetableVoice::LfoPitch, "PTCH" }, { WT, 165, (int) WavetableVoice::LfoAmp, "AMP" },
                { WT, 165, (int) WavetableVoice::LfoPosition, "POS" },
                { SMP, 165, (int) SamplerEngine::LfoCutoff, "CUT" }, { SMP, 165, (int) SamplerEngine::LfoGrainPos, "GPOS" },
                { SMP, 165, (int) SamplerEngine::LfoDrive, "DRIV" },
                { FM, 165, (int) FMVoice::LfoIndex, "INDX" },
                { WSEQ, 165, (int) WaveSeqVoice::LfoRate, "RATE" }, { WSEQ, 165, (int) WaveSeqEngine::dests().envTime, "ETIM" },
                { WSEQ, 165, (int) WaveSeqEngine::dests().filterTime, "FTIM" }, { WSEQ, 165, (int) WaveSeqEngine::dests().phase, "PM" },
                { DRUM, 165, (int) DrumVoice::ModDecay, "DEC" }, { DRUM, 165, (int) DrumVoice::ModDrive, "DRIV" },
            };
            for (auto& c : constants)
                expectEquals(juce::String(stored::nameOf(c.engine, c.id, (float) c.value)), juce::String(c.is),
                             juce::String(stored::engineLabel(c.engine)) + ": a constant no longer names the entry it is used for");
            // The sources a slot stores up to ONE are the router's own numbers (mod::Src), by construction.
            const auto& sources = mod::SourceTable::standard(2);
            const std::pair<int, const char*> src[] = { { mod::Src::Lfo1, "LFO1" }, { mod::Src::Env, "ENV" }, { mod::Src::FilterEnv, "FENV" },
                                                        { mod::Src::Velocity, "VEL" }, { mod::Src::Pressure, "PRES" }, { mod::Src::Strip, "STRP" },
                                                        { mod::Src::Key, "KEY" }, { mod::Src::Random, "RAND" }, { mod::Src::Input, "IN" },
                                                        { mod::Src::Xin, "XIN" }, { mod::Src::Vib, "VIB" }, { mod::Src::One, "ONE" } };
            for (auto& [value, name] : src)
                expectEquals(juce::String(sources.labels()[value]), juce::String(name));
        }
    }
};

static FormatV3Tests formatV3Tests;
