#pragma once

#include "mod_wheel_tests.h"
#include "lfo_destinations_tests.h"
#include "robustness_tests.h"
#include "../src/engine/AudioEngine.h"
#include "../src/project/ProjectBridge.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <set>
#include <thread>
#include <vector>

// -----------------------------------------------------------------------------------------------
// E4: the cells of the effects on a track are destinations of the track's modulation router
// (mod::TrackDest in ModRouter.h; AudioEngine::moveTrackEffects hands the offsets over).
//
// Everything here goes through a whole AudioEngine (renderBlock), an engine on track 1 and its
// effects, as the app plays it:
//   1. every engine: an LFO on an insert cell and a free slot from the envelope on another sound,
//      to the bit, as the two knobs moved to the same values each block; a slot at amount 0, a
//      slot taken away again and a slot aimed at an empty insert slot change nothing, to the bit;
//   2. WET, a cell of FX2, MODFX's cells the same way; a list, a count and an effect that takes no
//      modulation are left alone;
//   3. let go exactly once: the slot aimed elsewhere or emptied, the effect exchanged, the effect
//      before it taken out, the engine exchanged, a project applied (and the stored DEST brought
//      back by a project);
//   4. no voice sounding: a free LFO goes on moving the cell, before the first note and after the
//      last, without a step where the lead passes;
//   5. who leads: the note played last; when it ends, the one still sounding; a kit's last hit;
//   6. the strip's routes, VIA, a slot's amount moved by another slot, the sum held to -1 .. 1;
//   7. no click while an LFO sweeps a cell under a held sound (the click audit's measure);
//   8. the lists: nothing stored before E4 moved, the names, the groups, what the display shows;
//   9. random routes while effects and engines come and go, also from another thread.
// NEPTUNE_E4_BENCH=1: the cost table (meant for the Release binary).
// -----------------------------------------------------------------------------------------------
class EffectDestinationsTests : public juce::UnitTest
{
public:
    EffectDestinationsTests() : UnitTest("Effect Destinations") {}

    using R = mod::Router;
    static constexpr int CELL = AudioEngine::EFFECT_PARAM_BASE;
    static constexpr int EXTRA = AudioEngine::INSERT_EXTRA_BASE;
    static constexpr int WET = AudioEngine::INSERT_WET_BASE;
    static constexpr int M = AudioEngine::MASTER_TRACK;
    static constexpr int BLOCK = 128;
    static constexpr double SR = 48000.0;
    enum { SIN = 0, TRI = 1, SQR = 2 };
    enum { FREE = 0, KEY = 1 };

    static int insertDest(int slot, int cell) { return mod::TrackDest::ofInsert(slot, cell); }
    static int modFxDest(int cell) { return mod::TrackDest::ofModFx(cell); }

    struct Stereo
    {
        std::vector<float> l, r;
        bool operator== (const Stereo& o) const { return l == o.l && r == o.r && ! l.empty(); }
    };
    static float maxDiff(const Stereo& a, const Stereo& b)
    {
        float worst = a.l.size() == b.l.size() ? 0.0f : 1.0e9f;
        for (size_t i = 0; i < a.l.size() && i < b.l.size(); ++i)
            worst = std::max({ worst, std::abs(a.l[i] - b.l[i]), std::abs(a.r[i] - b.r[i]) });
        return worst;
    }
    static float peak(const Stereo& a)
    {
        float p = 0.0f;
        for (size_t i = 0; i < a.l.size(); ++i)
            p = std::max({ p, std::abs(a.l[i]), std::abs(a.r[i]) });
        return p;
    }
    static bool finite(const Stereo& a)
    {
        for (size_t i = 0; i < a.l.size(); ++i)
            if (! std::isfinite(a.l[i]) || ! std::isfinite(a.r[i]) || std::abs(a.l[i]) > 16.0f || std::abs(a.r[i]) > 16.0f)
                return false;
        return true;
    }
    // The largest step from one sample to the next from `from` on, and the peak (the click audit's measure).
    struct Steps { float step = 0.0f, peak = 0.0f; };
    static Steps steps(const Stereo& a, int from)
    {
        Steps s;
        for (auto* ch : { &a.l, &a.r })
            for (size_t i = (size_t) std::max(1, from); i < ch->size(); ++i)
            {
                s.step = std::max(s.step, std::abs((*ch)[i] - (*ch)[i - 1]));
                s.peak = std::max(s.peak, std::abs((*ch)[i]));
            }
        return s;
    }

    static const std::vector<EngineType>& soundEngines()
    {
        static const std::vector<EngineType> types = { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                                                       EngineType::WaveSeq, EngineType::Sampler, EngineType::Drum, EngineType::Resonator };
        return types;
    }
    static juce::String nameOf(EngineType t) { return LfoDestinationsTests::engineName(t); }

    // An effect that only writes down what modulateCell is given.
    struct Spy : EffectBase
    {
        struct Call { int cell; float offset; };
        std::vector<Call> calls;
        std::array<float, EffectBase::WET_CELL + 1> now {};
        Spy() { calls.reserve(1 << 16); }
        void reset() override {}
        void process(float&, float&) override {}
        void processBlock(float*, float*, int) override {}
        void modulateCell(int cell, float offset) override
        {
            calls.push_back({ cell, offset });
            if (cell >= 0 && cell <= EffectBase::WET_CELL)
                now[(size_t) cell] = offset;
        }
        bool takesModulation(int) const override { return true; }
        int count(int cell, size_t from = 0) const
        {
            int n = 0;
            for (size_t i = from; i < calls.size(); ++i)
                n += calls[i].cell == cell ? 1 : 0;
            return n;
        }
        int zeros(int cell, size_t from = 0) const
        {
            int n = 0;
            for (size_t i = from; i < calls.size(); ++i)
                n += calls[i].cell == cell && calls[i].offset == 0.0f ? 1 : 0;
            return n;
        }
        bool moved(int cell, size_t from = 0) const
        {
            for (size_t i = from; i < calls.size(); ++i)
                if (calls[i].cell == cell && calls[i].offset != 0.0f)
                    return true;
            return false;
        }
    };

    // An engine on track 1 of a whole AudioEngine, nothing after the sum.
    struct Rig
    {
        AudioEngine audio;
        std::shared_ptr<EngineBase> engine;
        EngineType type;
        int block;
        std::vector<float> l, r;

        explicit Rig(EngineType t, int blockSize = BLOCK, double sr = SR) : type(t), block(blockSize), l((size_t) blockSize), r((size_t) blockSize)
        {
            engine = AudioEngine::createEngine(t);
            ModWheelTests::makeRepeatable(*engine);
            audio.addEngine(0, engine);
            while (audio.getInsertChain(M)->getEffectCount() > 0)
                audio.removeInsertEffect(M, 0);
            audio.setMasterLevel(1.0f);
            audio.setTrackParameter(0, MixParam::VOLUME, 0.8f);
            audio.prepare(sr, blockSize);
        }

        EngineBase& e() { return *audio.getEngine(0); }
        const R& router() { return *e().modRouter(); }
        bool kit() { return e().getType() == EngineType::Drum; }
        bool unified() { return e().getType() == EngineType::UnifiedVoice; }
        void set(int id, float v) { audio.setTrackParameter(0, id, v); }
        EffectChain& chain() { return *audio.getInsertChain(0); }      // (once the track has had an effect)
        int effects() { auto* c = audio.getInsertChain(0); return c != nullptr ? c->getEffectCount() : 0; }
        CellEffect* cells(int slot) { return effects() > slot ? dynamic_cast<CellEffect*>(chain().getEffect(slot)) : nullptr; }

        // The cell of a page of the engine, by the page's and the cell's name (-1: none).
        int cellId(const char* page, const char* label, int which = 0)
        {
            for (auto& p : e().getParamPages())
                if (p.name == page)
                    for (auto& spec : p.params)
                        if (spec.isValid() && juce::String(spec.label) == label && which-- == 0)
                            return spec.id;
            return -1;
        }

        // LFO n on a cell of the track's effects (d < 0: on nothing).
        void lfo(int n, int d, float depth, float hz, int shape = SIN, int trig = FREE)
        {
            if (unified())
            {
                if (e().getParameter(ModParam::LFO_COUNT) < (float) (n + 1))
                    set(ModParam::LFO_COUNT, (float) (n + 1));
                set(ModParam::lfoParamId(n, ModParam::LFO_RATE), hz);
                set(ModParam::lfoParamId(n, ModParam::LFO_SHAPE), (float) shape);
                set(ModParam::lfoParamId(n, ModParam::LFO_TRIG), (float) trig);
                set(ModParam::lfoParamId(n, ModParam::LFO_SYNC), 0.0f);
                set(ModParam::lfoParamId(n, ModParam::LFO_DEPTH), depth);
                set(ModParam::lfoParamId(n, ModParam::LFO_DEST), d < 0 ? 0.0f : (float) ((int) UnifiedVoice::LfoDest::Count + d));
                return;
            }
            set(R::lfoId(n, R::Rate), hz);
            set(R::lfoId(n, R::Shape), (float) shape);
            set(R::lfoId(n, R::Trigger), (float) trig);
            set(R::lfoId(n, R::Sync), 0.0f);
            set(R::lfoId(n, R::Depth), depth);
            set(R::lfoId(n, R::DestCell), d < 0 ? 0.0f : (float) router().trackCellIndex(d));
        }
        // Free slot k: a source of mod::Src on a cell of the track's effects (d < 0: on nothing).
        void slot(int k, int source, int d, float amount, int via = mod::Src::Off)
        {
            set(ModSlotParam::id(k, ModSlotParam::SRC), (float) router().sourceIndex(source));
            set(ModSlotParam::id(k, ModSlotParam::DEST), d < 0 ? 0.0f : (float) router().trackSlotIndex(d));
            set(ModSlotParam::id(k, ModSlotParam::AMT), amount);
            set(ModSlotParam::id(k, ModSlotParam::VIA), (float) router().sourceIndex(via));
        }
        // Free slot k on another free slot's amount.
        void slotOnAmount(int k, int source, int other, float amount)
        {
            set(ModSlotParam::id(k, ModSlotParam::SRC), (float) router().sourceIndex(source));
            set(ModSlotParam::id(k, ModSlotParam::DEST), (float) (router().dests().visible() + 2 * router().numLfos() + other));
            set(ModSlotParam::id(k, ModSlotParam::AMT), amount);
        }

        // A spy in a slot (the slots before it must be taken).
        Spy* spy(int slot)
        {
            auto made = std::make_unique<Spy>();
            Spy* s = made.get();
            const AudioEngine::StructureLock lock(audio.structure());
            if (slot < chain().getEffectCount())
                chain().replaceEffect(slot, std::move(made));
            else
                chain().addEffect(std::move(made));
            return s;
        }

        // What is played: a held chord, or a hit of the kit every eighth of a second.
        void play(int n)
        {
            if (kit())
            {
                if (n % 47 == 0)
                    e().noteOn((n / 47) % 4, 0.9f, -1);
            }
            else if (n == 0)
                for (int note : { 48, 55 })
                    e().noteOn(note, 0.8f, -1);
        }
        void stop()
        {
            if (kit())
                for (int pad = 0; pad < 4; ++pad)       // (the open hat sounds while it is held)
                    e().noteOff(pad, -1);
            else
                for (int note : { 48, 55 })
                    e().noteOff(note, -1);
        }

        Stereo render(int blocks, const std::function<void (int)>& before = {}, const std::function<void (int)>& after = {})
        {
            Stereo out;
            for (int n = 0; n < blocks; ++n)
            {
                if (before) before(n);
                audio.renderBlock(l.data(), r.data(), block);
                out.l.insert(out.l.end(), l.begin(), l.end());
                out.r.insert(out.r.end(), r.begin(), r.end());
                if (after) after(n);
            }
            return out;
        }
        float offset(int d) { return router().trackOffset(d); }
    };

    static float range(const std::vector<float>& v)
    {
        if (v.empty()) return 0.0f;
        const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
        return *hi - *lo;
    }

    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_E4_BENCH", {}).isNotEmpty())
        {
            bench();
            return;
        }
        // NEPTUNE_E4_ONLY=<numbers>: only those parts (1 lists and names, 2 equivalence, 3 other cells,
        // 4 release, 5 silence, 6 who leads, 7 routes, 8 clicks, 9 the storm).
        const auto only = juce::SystemStats::getEnvironmentVariable("NEPTUNE_E4_ONLY", {});
        auto wanted = [&only](char part) { return only.isEmpty() || only.containsChar(part); };
        if (wanted('1')) { lists(); display(); }
        if (wanted('2')) equivalence();
        if (wanted('3')) otherCells();
        if (wanted('4')) release();
        if (wanted('5')) silence();
        if (wanted('6')) leader();
        if (wanted('7')) routes();
        if (wanted('8')) clicks();
        if (wanted('9')) storm();
    }

    // ---- 8 ---------------------------------------------------------------------------------------
    void lists()
    {
        beginTest("The lists: every DEST cell keeps what it showed at the places it showed it, and the track's effect cells follow; a slot a group");
        {
            // The visible entries of each engine's table as they stood before E4 (the LFO pages' list
            // was exactly these; a free slot's went on with LnRT / LnDP and AMT1 .. AM16).
            struct Pin { EngineType type; int visible; int lfos; };
            const Pin pins[] = { { EngineType::UnifiedVoice, 80, 4 }, { EngineType::FM, 84, 2 }, { EngineType::MacroOsc, 45, 2 },  // +8 BDY2
                                 { EngineType::Wavetable, 42, 2 }, { EngineType::WaveSeq, 94, 2 }, { EngineType::Sampler, 43, 2 },  // +8 BDY2
                                 { EngineType::Drum, 46, 2 }, { EngineType::Resonator, 57, 2 } };
            for (auto& pin : pins)
            {
                Rig g(pin.type);
                const auto& t = g.router().dests();
                const juce::String name = nameOf(pin.type);
                expectEquals(t.visible(), pin.visible, name + ": the table's visible entries");
                expectEquals(t.cellChoices(), pin.visible + mod::TrackDest::COUNT, name + ": an LFO's / STRIP's / XIN's DEST");
                expectEquals(t.slotChoices(), pin.visible + 2 * pin.lfos + 16 + mod::TrackDest::COUNT, name + ": a free slot's DEST");
                expectEquals(t.trackCell(), pin.visible, name);
                expectEquals(t.trackSlotChoice(), pin.visible + 2 * pin.lfos + 16, name);
                for (int i = 0; i < pin.visible; ++i)
                {
                    expect(t.cellLabels()[i] == t.labels()[i], name + ": DEST " + juce::String(i) + " is the table's entry");
                    expect(t.slotLabels()[i] == t.labels()[i], name + ": a slot's DEST " + juce::String(i) + " is the table's entry");
                }
                expectEquals(juce::String(t.slotLabels()[pin.visible]), juce::String("L1RT"), name);
                expectEquals(juce::String(t.slotLabels()[t.trackSlotChoice() - 1]), juce::String("AM16"), name);
                std::set<juce::String> seen;
                for (int i = 0; i < t.slotChoices(); ++i)
                    expect(seen.insert(t.slotLabels()[i]).second, name + ": " + t.slotLabels()[i] + " twice in a slot's DEST");
                for (int d = 0; d < mod::TrackDest::COUNT; ++d)
                {
                    expect(t.cellLabels()[pin.visible + d] == mod::TrackDest::labels()[d], name + ": DEST, the track's " + juce::String(d));
                    expect(t.slotLabels()[t.trackSlotChoice() + d] == mod::TrackDest::labels()[d], name + ": a slot's DEST, the track's " + juce::String(d));
                }
                // The cells on the pages show these lists, and hold a value to them.
                int cells = 0;
                for (auto& page : g.e().getParamPages())
                    for (auto& spec : page.params)
                    {
                        if (spec.choices == t.cellLabels())
                        {
                            ++cells;
                            expectEquals(spec.numChoices, t.cellChoices(), name + " " + page.name + " " + spec.label);
                            expectEquals(spec.maxVal, (float) (t.cellChoices() - 1), name + " " + page.name + " " + spec.label);
                            g.set(spec.id, 9999.0f);
                            expectEquals(g.e().getParameter(spec.id), spec.maxVal, name + " " + page.name + " " + spec.label + ": held to the list");
                            g.set(spec.id, (float) g.router().trackCellIndex(insertDest(2, 3)));
                            expectEquals(g.e().getParameter(spec.id), (float) (pin.visible + 2 * 9 + 3), name + " " + page.name + ": 3:C4 reads back");
                            g.set(spec.id, spec.defaultVal);
                        }
                        if (spec.choices == t.slotLabels())
                        {
                            ++cells;
                            expectEquals(spec.numChoices, t.slotChoices(), name + " " + page.name + " " + spec.label);
                            expectEquals(spec.maxVal, (float) (t.slotChoices() - 1), name + " " + page.name + " " + spec.label);
                        }
                    }
                // (XIN's two, the shown slots' DEST, STRIP's three, and the two LFO pages' where the engine has the router's.)
                expect(cells >= 2 + 2 + 3 + (pin.type == EngineType::UnifiedVoice ? 0 : 2), name + ": " + juce::String(cells) + " DEST cells show the lists");
                for (const char* const* list : { t.cellLabels(), t.slotLabels() })
                {
                    const auto* groups = ChoiceGroups::find(list);
                    expect(groups != nullptr, name + ": Func + turn knows the list");
                    if (groups == nullptr) continue;
                    const int first = list == t.cellLabels() ? t.trackCell() : t.trackSlotChoice();
                    for (int k = 0; k < mod::TrackDest::GROUPS; ++k)
                        expectEquals(groups->starts[groups->count - mod::TrackDest::GROUPS + k], first + mod::TrackDest::groupStart(k),
                                     name + ": a group begins at each slot and at MODFX");
                }
            }
            // The synth's own LFO pages: the voice's list as it was, then the same eighty.
            const char* const* own = UnifiedVoice::lfoDestLabels();
            const char* const* cell = UnifiedVoice::lfoDestCellLabels();
            for (int i = 0; i < (int) UnifiedVoice::LfoDest::Count; ++i)
                expect(cell[i] == own[i], "synth LFO DEST " + juce::String(i));
            for (int d = 0; d < mod::TrackDest::COUNT; ++d)
                expect(cell[(int) UnifiedVoice::LfoDest::Count + d] == mod::TrackDest::labels()[d], "synth LFO DEST, the track's " + juce::String(d));
            expectEquals(UnifiedVoice::NUM_LFO_CELL_DESTS, 75 + mod::TrackDest::COUNT);
            expect(ChoiceGroups::find(cell) != nullptr, "the synth's LFO DEST has groups");
            // (A table registers three lists now: the room for them must not run out.)
            logMessage("grouped lists in use: " + juce::String(ChoiceGroups::used().load()) + " of " + juce::String((int) ChoiceGroups::MAX_LISTS));
            expect(ChoiceGroups::used().load() < ChoiceGroups::MAX_LISTS - 16, "room for grouped lists");
        }

        beginTest("The names: eighty, unique, four characters, in the glossary, none an engine's own; MODFX's are its cells'");
        {
            expectEquals((int) mod::TrackDest::COUNT, 80);
            const auto glossary = LfoDestinationsTests::glossaryKeys();
            std::set<juce::String> names;
            for (int d = 0; d < mod::TrackDest::COUNT; ++d)
            {
                const juce::String label(mod::TrackDest::labels()[d]);
                expect(names.insert(label).second, label + " twice");
                expect(label.isNotEmpty() && label.length() <= 4, label + " does not fit a cell");
                expect(glossary.count(label) > 0 || glossary.count(LfoDestinationsTests::numbered(label)) > 0,
                       label + " is not in tools/manual/glossary_data.py");
                if (mod::TrackDest::isInsert(d))
                {
                    expectEquals(label.substring(0, 1).getIntValue(), mod::TrackDest::slot(d) + 1, label);
                    expectEquals(label.substring(2), mod::TrackDest::cell(d) == mod::TrackDest::WET_CELL ? juce::String("WT")
                                                                                                         : "C" + juce::String(mod::TrackDest::cell(d) + 1));
                }
                else
                    expectEquals(label, juce::String(ModFxBlock::spec(d - mod::TrackDest::MODFX).label), "MODFX's cell under its own name");
            }
            // (the macro oscillator names its macros per model)
            for (int model = 0; model < 8; ++model)
            {
                MacroOscEngine e;
                e.setParameter(MacroOscEngine::ModelParam, (float) model);
                for (auto& page : e.getParamPages())
                    for (auto& spec : page.params)
                        if (spec.id == R::lfoId(0, R::DestCell))
                        {
                            std::set<juce::String> seen;
                            for (int i = 0; i < spec.numChoices; ++i)
                                expect(seen.insert(spec.choices[i]).second, "macro model " + juce::String(model) + ": " + spec.choices[i] + " twice");
                            expectEquals(mod::TrackDest::firstIn(spec.choices, spec.numChoices), spec.numChoices - (int) mod::TrackDest::COUNT);
                        }
            }
            expectEquals(mod::TrackDest::firstIn(UnifiedVoice::lfoDestLabels(), (int) UnifiedVoice::LfoDest::Count), (int) UnifiedVoice::LfoDest::Count,
                         "a list without them has none");
        }
    }

    void display()
    {
        beginTest("What the display shows: the slot's number and the cell's own name for the effect in the slot; n:-- where nothing moves");
        {
            Rig g(EngineType::FM);
            g.audio.addInsertEffect(0, EffectType::DelayEffect);
            g.audio.addInsertEffect(0, EffectType::DriveEffect);
            g.audio.addInsertEffect(0, EffectType::VocoderEffect);
            const char* const delay[9] = { "1TIME", "1FDBK", "1:MIX", "1:--", "1:--", "1DRIV", "1:--", "1:AGE", "1:WET" };
            for (int c = 0; c < 9; ++c)
                expectEquals(g.audio.trackDestText(0, insertDest(0, c)), juce::String(delay[c]), "DLY cell " + juce::String(c));
            for (int c = 0; c < 9; ++c)
                expectEquals(g.audio.trackDestText(0, insertDest(1, c)), juce::String("2:--"), "DRV takes no modulation");
            const char* const vocoder[9] = { "3:--", "3:--", "3NOIS", "3:--", "3:REL", "3RING", "3BODY", "3:--", "3:--" };
            for (int c = 0; c < 9; ++c)
                expectEquals(g.audio.trackDestText(0, insertDest(2, c)), juce::String(vocoder[c]), "VOC cell " + juce::String(c));
            expectEquals(g.audio.trackDestText(0, insertDest(3, 0)), juce::String("4:--"), "an empty slot");
            expectEquals(g.audio.trackDestText(5, insertDest(0, 0)), juce::String("1:--"), "a track without effects");
            expectEquals(g.audio.trackDestText(0, modFxDest(2)), juce::String(), "MODFX's cells are named in the list");
            expect(g.audio.trackDestText(0, 5).length() <= 5 && g.audio.trackDestText(0, 8).length() <= 5, "five characters fit a cell's value");

            // Which destination a DEST cell's value stands for.
            for (auto type : soundEngines())
            {
                Rig h(type);
                int found = 0;
                for (auto& page : h.e().getParamPages())
                    for (auto& spec : page.params)
                    {
                        const bool cell = spec.choices == h.router().dests().cellLabels(), slot = spec.choices == h.router().dests().slotLabels();
                        if (! cell && ! slot)
                        {
                            if (spec.kind == ParamKind::Choice && spec.isValid())
                                expectEquals(h.audio.trackDestOfCell(0, spec, spec.maxVal), -1, nameOf(type) + " " + page.name + " " + spec.label + " is no DEST");
                            continue;
                        }
                        ++found;
                        const int first = cell ? h.router().trackCellIndex(0) : h.router().trackSlotIndex(0);
                        expectEquals(h.audio.trackDestOfCell(0, spec, (float) (first - 1)), -1, nameOf(type) + ": the entry before them");
                        expectEquals(h.audio.trackDestOfCell(0, spec, 0.0f), -1, nameOf(type) + ": OFF");
                        for (int d : { 0, 8, 9, 71, 72, 79 })
                            expectEquals(h.audio.trackDestOfCell(0, spec, (float) (first + d)), d, nameOf(type) + " " + page.name + " " + spec.label);
                    }
                expect(found >= 7, nameOf(type) + ": DEST cells found");
            }
            Rig u(EngineType::UnifiedVoice);
            const ParamSpec lfoDest { "DEST", ModParam::lfoParamId(0, ModParam::LFO_DEST), 0, (float) (UnifiedVoice::NUM_LFO_CELL_DESTS - 1), 0,
                                      ParamKind::Choice, "", UnifiedVoice::lfoDestCellLabels(), UnifiedVoice::NUM_LFO_CELL_DESTS };
            expectEquals(u.audio.trackDestOfCell(0, lfoDest, (float) ((int) UnifiedVoice::LfoDest::Count + 17)), 17, "the synth's LFO pages");
            expectEquals(u.audio.trackDestOfCell(0, lfoDest, (float) ((int) UnifiedVoice::LfoDest::Count - 1)), -1);
            u.set(lfoDest.id, 9999.0f);
            expectEquals(u.e().getParameter(lfoDest.id), lfoDest.maxVal, "the synth's LFO DEST is held to its list");
        }

        beginTest("PARAM: a DEST cell aimed at an effect's cell reads with the cell's name, and follows the effect in the slot");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::FM));
            seq.getPattern().getTrack(0).engineType = EngineType::FM;
            const auto& router = *audio.getEngine(0)->modRouter();
            ParamPageScreen param(seq, &audio);
            expect(param.selectPageByName("LFO1"), "the LFO1 page");
            audio.setTrackParameter(0, R::lfoId(0, R::DestCell), 1.0f);
            expectEquals(param.getCellReadout(7), juce::String("DEST PTCH"), "an entry of the engine reads as it did");
            audio.setTrackParameter(0, R::lfoId(0, R::DestCell), (float) router.trackCellIndex(insertDest(0, 0)));
            expectEquals(param.getCellReadout(7), juce::String("DEST 1:--"), "no effect in the slot");
            audio.addInsertEffect(0, EffectType::DelayEffect);
            expectEquals(param.getCellReadout(7), juce::String("DEST 1TIME"), "DLY in slot 1");
            audio.setInsertEffect(0, 0, EffectType::PhaserEffect);
            expectEquals(param.getCellReadout(7), juce::String("DEST 1RATE"), "PHS in slot 1: the page was not rebuilt, the cell reads the slot");
            audio.setTrackParameter(0, R::lfoId(0, R::DestCell), (float) router.trackCellIndex(insertDest(0, EffectBase::WET_CELL)));
            expectEquals(param.getCellReadout(7), juce::String("DEST 1:WET"));
            audio.setTrackParameter(0, R::lfoId(0, R::DestCell), (float) router.trackCellIndex(modFxDest(ModFxBlock::RotarySpeed)));
            expectEquals(param.getCellReadout(7), juce::String("DEST RO.S"), "MODFX's cells by their own names");
            expect(param.selectPageByName("SLT1"), "the SLT1 page");
            audio.setTrackParameter(0, ModSlotParam::id(0, ModSlotParam::DEST), (float) router.trackSlotIndex(insertDest(0, 1)));
            expectEquals(param.getCellReadout(1), juce::String("DEST 1DPTH"), "a slot's DEST");
            audio.setTrackParameter(0, ModSlotParam::id(0, ModSlotParam::DEST), (float) (router.trackSlotIndex(0) - 1));
            expectEquals(param.getCellReadout(1), juce::String("DEST AM16"));
            expect(param.selectPageByName("STRIP"), "the STRIP page");
            audio.setTrackParameter(0, R::stripDestId(1), (float) router.trackCellIndex(insertDest(1, 3)));
            expectEquals(param.getCellReadout(2), juce::String("DST2 2:--"), "the strip's routes too");
        }
    }

    // ---- 1 ---------------------------------------------------------------------------------------
    void equivalence()
    {
        const int dT = insertDest(0, 0), dF = insertDest(0, 1);
        const float time = 0.12f, feedback = 0.5f;
        for (auto type : soundEngines())
        {
            beginTest(nameOf(type) + ": an LFO on 1:C1 (DLY TIME) and a slot from ENV on 1:C2 (FDBK) are the two knobs moved the same way, to the bit; "
                      "amount 0, a slot taken away, a slot on an empty insert change nothing, to the bit");
            const int blocks = 320;
            auto build = [&](Rig& g)
            {
                g.audio.addInsertEffect(0, EffectType::DelayEffect);
                g.set(CELL, time);
                g.set(CELL + 1, feedback);
            };

            Rig a(type);
            build(a);
            a.lfo(0, dT, 0.25f, 3.0f);
            a.slot(2, mod::Src::Env, dF, -0.6f);
            const ParamSpec specT = a.cells(0)->cellSpec(0), specF = a.cells(0)->cellSpec(1);
            expectEquals(juce::String(specT.label), juce::String("TIME"));
            expectEquals(juce::String(specF.label), juce::String("FDBK"));
            std::vector<float> oT, oF;
            bool followed = true;
            const Stereo outA = a.render(blocks, [&](int n) { a.play(n); }, [&](int)
            {
                oT.push_back(a.offset(dT));
                oF.push_back(a.offset(dF));
                auto* fx = a.cells(0);
                followed = followed && fx->getModulation(0) == oT.back() && fx->getModulation(1) == oF.back()
                           && fx->getMovedCell(0) == CellEffect::moved(specT, time, oT.back())
                           && fx->getMovedCell(1) == CellEffect::moved(specF, feedback, oF.back())
                           && fx->getParameter(0) == time && fx->getParameter(1) == feedback;
            });
            expect(followed, "each block the cells are worth knob + offset, and read as they were set");
            expect(range(oT) > 0.4f, "the LFO swings the offset of TIME: " + juce::String(range(oT), 3));
            expect(*std::min_element(oF.begin(), oF.end()) < -0.2f && *std::max_element(oF.begin(), oF.end()) <= 0.0f,
                   "the envelope pulls FDBK down: " + juce::String(*std::min_element(oF.begin(), oF.end()), 3));
            for (int c = 2; c <= EffectBase::WET_CELL; ++c)
                expectEquals(a.cells(0)->getModulation(c), 0.0f, "no other cell is touched");

            Rig b(type);
            build(b);
            const Stereo outB = b.render(blocks, [&](int n)
            {
                b.play(n);
                b.set(CELL, CellEffect::moved(specT, time, oT[(size_t) n]));
                b.set(CELL + 1, CellEffect::moved(specF, feedback, oF[(size_t) n]));
            });
            expect(peak(outA) > 0.02f && finite(outA), "it sounds: peak " + juce::String(peak(outA), 3));
            expect(outA == outB, "the knobs moved to the same values each block: largest difference " + juce::String(maxDiff(outA, outB), 8));

            Rig c(type);
            build(c);
            const Stereo plain = c.render(blocks, [&](int n) { c.play(n); });
            expect(maxDiff(outA, plain) > 1.0e-3f, "the modulation is heard: " + juce::String(maxDiff(outA, plain), 5));

            Rig d(type);
            build(d);
            d.lfo(0, dT, 0.0f, 3.0f);
            d.slot(2, mod::Src::Env, dF, 0.0f);
            expect(d.render(blocks, [&](int n) { d.play(n); }) == plain, "amount 0: the plain sound, to the bit");

            Rig f(type);
            build(f);
            f.lfo(0, insertDest(7, 0), 0.25f, 3.0f);
            f.slot(2, mod::Src::Env, insertDest(5, 1), -0.6f);
            f.slot(3, mod::Src::Lfo1, modFxDest(1), 0.0f);
            expect(f.render(blocks, [&](int n) { f.play(n); }) == plain, "slots on empty inserts: the voices and the effect are untouched, to the bit");

            // Slots that moved the cells in silence and were taken away: once the effect has come
            // back to where it is set, the sound is the plain one.
            auto late = [&](bool slots)
            {
                Rig g(type);
                build(g);
                if (slots)
                {
                    g.lfo(0, dT, 0.25f, 3.0f);
                    g.slot(2, mod::Src::One, dF, -0.6f);
                }
                g.render(60);
                if (slots)
                {
                    expect(g.cells(0)->getModulation(0) != 0.0f && g.cells(0)->getModulation(1) == -0.6f, "moved in silence");
                    g.lfo(0, -1, 0.25f, 3.0f);
                    g.slot(2, mod::Src::One, -1, -0.6f);
                }
                // (Four seconds: the delay glides back to its time, and what was written into its
                // line meanwhile would come back late for as long as it is fed back.)
                g.render(1500);
                if (slots)
                    for (int cell = 0; cell <= EffectBase::WET_CELL; ++cell)
                        expectEquals(g.cells(0)->getModulation(cell), 0.0f, "let go");
                return g.render(blocks, [&](int n) { g.play(n); });
            };
            {
                const Stereo was = late(true), never = late(false);
                expect(was == never, "slots that moved the cells and were taken away: the plain sound again, to the bit (largest difference "
                                     + juce::String(maxDiff(was, never), 8) + ", peak " + juce::String(peak(never), 3) + ")");
            }
            Rig e(type);
            build(e);
            e.lfo(0, dT, 0.25f, 3.0f);
            e.slot(2, mod::Src::Env, dF, -0.6f);
            e.lfo(0, -1, 0.25f, 3.0f);
            e.slot(2, mod::Src::Env, -1, -0.6f);
            expect(e.render(blocks, [&](int n) { e.play(n); }) == plain, "slots set and taken away before a block ran: the plain sound, to the bit");
        }
    }

    // ---- 2 ---------------------------------------------------------------------------------------
    // One destination against its knob: the offsets a first render gives, then the knob set to
    // knob + offset before each block of a second one.
    struct Against
    {
        EffectType effect;
        int dest;               // the track destination
        int knob;               // the id of the cell's knob
        float base;
        const char* what;
    };
    void against(EngineType type, const Against& k, const std::function<void (Rig&)>& more = {})
    {
        const int blocks = 300;
        auto build = [&](Rig& g)
        {
            if (mod::TrackDest::isInsert(k.dest))
                g.audio.addInsertEffect(0, k.effect);
            g.set(k.knob, k.base);
            if (more) more(g);
        };
        auto specOf = [&](Rig& g) -> ParamSpec
        {
            if (mod::TrackDest::isModFx(k.dest))
                return ModFxBlock::spec(k.dest - mod::TrackDest::MODFX);
            return g.cells(0)->cellSpec(mod::TrackDest::cell(k.dest));
        };
        Rig a(type);
        build(a);
        a.lfo(1, k.dest, 0.35f, 4.0f, TRI);
        const ParamSpec spec = specOf(a);
        std::vector<float> offsets;
        const Stereo outA = a.render(blocks, [&](int n) { a.play(n); }, [&](int) { offsets.push_back(a.offset(k.dest)); });
        Rig b(type);
        build(b);
        const Stereo outB = b.render(blocks, [&](int n)
        {
            b.play(n);
            b.set(k.knob, CellEffect::moved(spec, k.base, offsets[(size_t) n]));
        });
        Rig c(type);
        build(c);
        const Stereo plain = c.render(blocks, [&](int n) { c.play(n); });
        const juce::String what = nameOf(type) + " " + k.what;
        expect(range(offsets) > 0.5f, what + ": the offset swings " + juce::String(range(offsets), 3));
        expect(peak(outA) > 0.02f && finite(outA), what + " sounds");
        expect(outA == outB, what + ": as the knob moved, to the bit (largest difference " + juce::String(maxDiff(outA, outB), 8) + ")");
        expect(maxDiff(outA, plain) > 1.0e-3f, what + " is heard: " + juce::String(maxDiff(outA, plain), 5));
    }

    void otherCells()
    {
        beginTest("WET, a cell of FX2 and MODFX's cells against their knobs, to the bit");
        {
            const int mf = TrackFxParam::MODFX_BASE;
            for (auto type : { EngineType::FM, EngineType::Drum })
            {
                against(type, { EffectType::PhaserEffect, insertDest(0, EffectBase::WET_CELL), WET, 0.5f, "1:WT on PHS (the chain mixes)" });
                against(type, { EffectType::DelayEffect, insertDest(0, EffectBase::WET_CELL), WET, 0.5f, "1:WT on DLY (it mixes itself)" });
                against(type, { EffectType::DelayEffect, insertDest(0, 7), EXTRA + 3, 0.5f, "1:C8 (DLY AGE)" },
                        [](Rig& g) { g.set(CELL + 3, 2.0f); });      // (MODE TAPE: AGE is heard)
                against(type, { EffectType::DelayEffect, insertDest(0, 5), EXTRA + 1, 0.4f, "1:C6 (DLY DRIV)" });
                against(type, { EffectType::PhaserEffect, insertDest(0, 0), CELL, 1.0f, "1:C1 (PHS RATE, a rate moves by ratio)" });
                // (MODFX mod destinations CH.D, PH.R, RO.S are dead since MODFX became inserts.
                //  The dest slots 72-79 are reserved but no longer receive modulation.)
            }
        }

        beginTest("A slot on a second insert, and two inserts moved at once, reach the right effect and the right cell");
        {
            Rig g(EngineType::FM);
            for (auto t : { EffectType::DelayEffect, EffectType::PhaserEffect, EffectType::TapeDelayEffect })
                g.audio.addInsertEffect(0, t);
            g.slot(0, mod::Src::One, insertDest(1, 2), 0.3f);
            g.slot(1, mod::Src::One, insertDest(2, 1), -0.2f);
            g.slot(2, mod::Src::One, insertDest(1, EffectBase::WET_CELL), -0.4f);
            g.slot(3, mod::Src::One, modFxDest(ModFxBlock::FlangerDepth), 0.25f);
            g.render(20, [&](int n) { g.play(n); });
            for (int slot = 0; slot < 3; ++slot)
                for (int cell = 0; cell <= EffectBase::WET_CELL; ++cell)
                {
                    const float want = slot == 1 && cell == 2 ? 0.3f : (slot == 2 && cell == 1 ? -0.2f : (slot == 1 && cell == EffectBase::WET_CELL ? -0.4f : 0.0f));
                    expectWithinAbsoluteError(g.cells(slot)->getModulation(cell), want, 1.0e-6f, "slot " + juce::String(slot + 1) + " cell " + juce::String(cell));
                }
            // (MODFX modulation test removed: MODFX is no longer a fixed block.)
            // Another track's effects are not this router's.
            g.audio.addEngine(1, AudioEngine::createEngine(EngineType::FM));
            g.audio.addInsertEffect(1, EffectType::DelayEffect);
            g.audio.addInsertEffect(1, EffectType::PhaserEffect);
            g.render(10);
            for (int slot = 0; slot < 2; ++slot)
                for (int cell = 0; cell <= EffectBase::WET_CELL; ++cell)
                    expectEquals(dynamic_cast<CellEffect*>(g.audio.getInsertChain(1)->getEffect(slot))->getModulation(cell), 0.0f, "track 2 is its own");
            // Nor the mix's or a send's.
            g.audio.addInsertEffect(M, EffectType::PhaserEffect);
            g.render(10);
            for (int cell = 0; cell <= EffectBase::WET_CELL; ++cell)
                expectEquals(dynamic_cast<CellEffect*>(g.audio.getInsertChain(M)->getEffect(0))->getModulation(cell), 0.0f, "the master chain has no router");
        }

        beginTest("A list, a count and an effect that takes no modulation are left as they are: the sound is the plain one, to the bit");
        {
            struct Inert { EffectType effect; int cell; const char* what; };
            const Inert inert[] = { { EffectType::DelayEffect, 3, "DLY MODE (a list)" }, { EffectType::DelayEffect, 6, "DLY SAT (a list)" },
                                    { EffectType::ChorusEffect, 3, "CHR VOIC (a count)" },
                                    { EffectType::DriveEffect, 0, "DRV (takes none)" }, { EffectType::AmpEffect, 0, "AMP (takes none)" } };
            for (auto& k : inert)
            {
                Stereo out[2];
                for (int with = 0; with < 2; ++with)
                {
                    Rig g(EngineType::Wavetable);
                    g.audio.addInsertEffect(0, k.effect);
                    if (with)
                        g.lfo(0, insertDest(0, k.cell), 1.0f, 5.0f, SQR);
                    out[with] = g.render(200, [&](int n) { g.play(n); });
                    if (with)
                        expect(std::abs(g.offset(insertDest(0, k.cell))) > 0.5f, juce::String(k.what) + ": the router does give an offset");
                }
                expect(out[0] == out[1] && peak(out[0]) > 0.02f, juce::String(k.what) + " under a square LFO at full depth");
            }
        }
    }

    // ---- 3 ---------------------------------------------------------------------------------------
    void release()
    {
        const int cell0 = insertDest(0, 0);
        for (auto type : { EngineType::FM, EngineType::UnifiedVoice, EngineType::Drum })
        {
            const juce::String name = nameOf(type);
            beginTest(name + ": a cell is let go exactly once when its slot is aimed elsewhere, emptied or set to nothing");
            {
                Rig g(type);
                g.audio.addInsertEffect(0, EffectType::DelayEffect);
                Spy* spy = g.spy(0);
                g.render(5);
                expectEquals((int) spy->calls.size(), 0, "no slot on an effect: the effects are not told anything");
                g.lfo(0, cell0, 0.5f, 6.0f);
                g.render(1);
                // (An effect that is met for the first time is let go of whole, once: nine cells.)
                expectEquals((int) spy->calls.size(), EffectBase::WET_CELL + 2, "met: let go of whole, then the offset");
                size_t mark = spy->calls.size();
                g.render(30, [&](int n) { g.play(n); });
                expect(spy->count(0, mark) == 30 && spy->moved(0, mark), "an offset each block: " + juce::String(spy->count(0, mark)));
                expectEquals((int) (spy->calls.size() - mark), 30, "and nothing else");

                mark = spy->calls.size();
                g.lfo(0, insertDest(0, 1), 0.5f, 6.0f);                 // aimed at the next cell
                g.render(30);
                expectEquals(spy->zeros(0, mark), 1, "1:C1 let go once");
                expectEquals(spy->count(0, mark), 1, "and not touched again");
                expectEquals(spy->now[0], 0.0f);
                expect(spy->count(1, mark) == 30 && spy->moved(1, mark), "1:C2 is moved now");

                mark = spy->calls.size();
                g.lfo(0, insertDest(0, 1), 0.0f, 6.0f);                 // depth 0
                g.render(30);
                expectEquals(spy->count(1, mark), 1, "depth 0: one call");
                expectEquals(spy->now[1], 0.0f, "... which lets go");
                expectEquals((int) (spy->calls.size() - mark), 1);

                g.lfo(0, insertDest(0, 1), 0.5f, 6.0f);
                g.slot(4, mod::Src::One, insertDest(0, EffectBase::WET_CELL), -0.5f);
                g.render(30);
                expect(spy->now[1] != 0.0f && spy->now[(size_t) EffectBase::WET_CELL] == -0.5f, "back, and WET with it");
                mark = spy->calls.size();
                g.lfo(0, -1, 0.5f, 6.0f);                               // DEST OFF
                g.slot(4, mod::Src::Off, insertDest(0, EffectBase::WET_CELL), -0.5f);       // no source
                g.render(30);
                expectEquals((int) (spy->calls.size() - mark), 2, "each let go once, then silence");
                expectEquals(spy->zeros(1, mark), 1);
                expectEquals(spy->zeros(EffectBase::WET_CELL, mark), 1, "WET is let go too");

                // (MODFX modulation test removed: MODFX is no longer a fixed block.)
            }

            beginTest(name + ": the effect exchanged, the effect before it taken out, the engine exchanged");
            {
                // A real effect: its cell comes back to where the knob stands.
                Rig g(type);
                g.audio.addInsertEffect(0, EffectType::DelayEffect);
                g.audio.addInsertEffect(0, EffectType::PhaserEffect);
                g.set(CELL + 4, 1.5f);                                  // PHS RATE
                g.lfo(0, insertDest(1, 0), 0.4f, 5.0f);
                g.slot(5, mod::Src::One, insertDest(1, EffectBase::WET_CELL), -0.3f);
                g.render(30, [&](int n) { g.play(n); });
                auto* phaser = g.cells(1);
                expect(phaser->getModulation(0) != 0.0f && phaser->getMovedCell(0) != 1.5f, "the phaser's RATE is moved in slot 2");
                expectEquals(phaser->getModulation(EffectBase::WET_CELL), -0.3f);
                g.audio.removeInsertEffect(0, 0);                       // the delay goes: the phaser is in slot 1 now
                expect(g.cells(0) == phaser && g.chain().getEffectCount() == 1);
                g.render(30);
                for (int cell = 0; cell <= EffectBase::WET_CELL; ++cell)
                    expectEquals(phaser->getModulation(cell), 0.0f, "moved up: nothing aims at it there, cell " + juce::String(cell));
                expectEquals(phaser->getMovedCell(0), 1.5f, "RATE is where it is set");
                expectEquals(phaser->wetWanted(), phaser->getMix(), "and its mix");

                g.audio.addInsertEffect(0, EffectType::TapeDelayEffect);    // slot 2 again: what aims there moves the newcomer
                g.render(30);
                expect(g.cells(1)->getModulation(0) != 0.0f && g.cells(1)->getModulation(EffectBase::WET_CELL) == -0.3f, "a new effect in the slot is moved");
                expectEquals(phaser->getModulation(0), 0.0f);
                g.audio.setInsertEffect(0, 1, EffectType::ChorusEffect);    // exchanged under the slots
                g.render(30);
                expect(g.cells(1)->getModulation(0) != 0.0f, "the exchanged one too");
                expect(finite(g.render(30)));

                // Counted: who is told what, and how often.
                Rig h(type);
                h.audio.addInsertEffect(0, EffectType::DelayEffect);
                h.audio.addInsertEffect(0, EffectType::DelayEffect);
                Spy* first = h.spy(0);
                Spy* second = h.spy(1);
                h.lfo(0, insertDest(1, 0), 0.5f, 6.0f);
                h.render(20, [&](int n) { h.play(n); });
                expect(second->moved(0) && second->count(0) == 21, "slot 2's spy is moved (met once, then an offset a block)");
                expectEquals(first->count(0), 1, "slot 1's was met once (let go of whole when the first slot came into use)");
                size_t mark = second->calls.size();
                h.audio.removeInsertEffect(0, 0);
                h.render(20);
                expectEquals((int) (second->calls.size() - mark), EffectBase::WET_CELL + 1, "moved up: let go of whole, once");
                for (float v : second->now)
                    expectEquals(v, 0.0f);
                mark = second->calls.size();
                Spy* third = h.spy(1);                                  // a new one where the LFO aims
                h.render(20);
                expect(third->moved(0) && third->count(0) == 21, "the newcomer is moved");
                expectEquals((int) (second->calls.size() - mark), 0, "the one that moved up is left alone");

                // The engine exchanged: the new one's slots aim at nothing.
                mark = third->calls.size();
                h.audio.setEngineType(0, type == EngineType::FM ? EngineType::Wavetable : EngineType::FM);
                h.render(20);
                expectEquals((int) (third->calls.size() - mark), 1, "engine exchanged: let go once");
                expectEquals(third->now[0], 0.0f);
                h.lfo(0, insertDest(1, 0), 0.5f, 6.0f);                 // (the rig's lfo() asks the engine that is there now)
                h.render(20);
                expect(third->moved(0, mark + 1), "the new engine's LFO moves it again");
                mark = third->calls.size();
                h.audio.setEngineType(0, EngineType::MIDITrack);        // an engine without a router
                h.render(20);
                expectEquals((int) (third->calls.size() - mark), 1, "no router: let go once");
                expectEquals(third->now[0], 0.0f);
                expectEquals(h.audio.getNonFiniteBlocks(), 0);
            }
        }

        beginTest("A project applied: what was moved is let go; a project that holds the routes brings them back at the same cells");
        {
            using Whole = RobustnessTests::Rig;
            std::vector<float> l(64), r(64);
            auto run = [&](Whole& w, int blocks)
            {
                bool ok = true;
                for (int n = 0; n < blocks; ++n)
                {
                    w.audio.renderBlock(l.data(), r.data(), 64);
                    ok = ok && robust::allFinite(l.data(), 64) && robust::allFinite(r.data(), 64);
                }
                return ok;
            };
            Whole empty;
            const auto none = ProjectBridge::capture(empty.live(), "NONE").toJSON();

            Whole w;
            // Tracks 1 .. 7 (track 1 is the synth, whose LFO pages are the screen's: a free slot there).
            for (int t = 0; t < 7; ++t)
            {
                w.audio.addInsertEffect(t, EffectType::DelayEffect);
                w.audio.addInsertEffect(t, EffectType::PhaserEffect);
                auto& params = w.seq.getPattern().getTrack(t).soundParams;
                params[ProjectBridge::INSERT_TYPE_BASE] = (float) ((int) EffectType::DelayEffect + 1);
                params[ProjectBridge::INSERT_TYPE_BASE + 1] = (float) ((int) EffectType::PhaserEffect + 1);
                const auto& router = *w.audio.getEngine(t)->modRouter();
                auto put = [&](int id, float v) { params[id] = v; w.audio.setTrackParameter(t, id, v); };
                put(ModSlotParam::id(1, ModSlotParam::SRC), (float) router.sourceIndex(mod::Src::One));
                put(ModSlotParam::id(1, ModSlotParam::DEST), (float) router.trackSlotIndex(insertDest(1, 1)));
                put(ModSlotParam::id(1, ModSlotParam::AMT), 0.3f);
                if (t > 0)
                {
                    put(R::lfoId(1, R::DestCell), (float) router.trackCellIndex(insertDest(0, 1)));
                    put(R::lfoId(1, R::Depth), 0.4f);
                    put(R::lfoId(1, R::Rate), 7.0f);
                }
                w.seq.getPattern().getTrack(t).engineType = w.audio.getEngineTypeOf(t);
            }
            expect(run(w, 40));
            auto moved = [&](Whole& rig, const juce::String& when)
            {
                for (int t = 0; t < 7; ++t)
                {
                    auto* delay = dynamic_cast<CellEffect*>(rig.audio.getInsertChain(t)->getEffect(0));
                    auto* phaser = dynamic_cast<CellEffect*>(rig.audio.getInsertChain(t)->getEffect(1));
                    expect(delay != nullptr && phaser != nullptr, when + ": track " + juce::String(t + 1) + " has its two effects");
                    if (delay == nullptr || phaser == nullptr) continue;
                    expectWithinAbsoluteError(phaser->getModulation(1), 0.3f, 1.0e-6f, when + ": track " + juce::String(t + 1) + " 2:C2 from the slot");
                    if (t > 0)
                        expect(delay->getModulation(1) != 0.0f, when + ": track " + juce::String(t + 1) + " 1:C2 from LFO 2");
                    expectEquals(delay->getModulation(0), 0.0f, when + ": and no other cell");
                }
            };
            moved(w, "set");
            const auto full = ProjectBridge::capture(w.live(), "FULL").toJSON();

            // The effects that stay through a project without the routes are let go.
            std::vector<CellEffect*> before;
            for (int t = 0; t < 7; ++t)
                before.push_back(dynamic_cast<CellEffect*>(w.audio.getInsertChain(t)->getEffect(1)));
            ProjectBridge::apply(ProjectData::fromJSON(none), w.live());
            expect(run(w, 40));
            for (int t = 0; t < 7; ++t)
            {
                auto* chain = w.audio.getInsertChain(t);
                for (int slot = 0; chain != nullptr && slot < chain->getEffectCount(); ++slot)
                    if (auto* fx = dynamic_cast<CellEffect*>(chain->getEffect(slot)))
                        for (int cell = 0; cell <= EffectBase::WET_CELL; ++cell)
                            expectEquals(fx->getModulation(cell), 0.0f, "a project without routes: nothing is moved");
                // (MODFX modulation check removed: MODFX is no longer a fixed block.)
            }
            // And back: the stored DEST values mean the same cells.
            ProjectBridge::apply(ProjectData::fromJSON(full), w.live());
            expect(run(w, 40));
            moved(w, "loaded");
            Whole second;
            ProjectBridge::apply(ProjectData::fromJSON(full), second.live());
            expect(run(second, 40));
            moved(second, "loaded into a new instrument");
            ProjectBridge::apply(ProjectData::fromJSON(none), second.live());
            expect(run(second, 40));
            expectEquals(w.audio.getNonFiniteBlocks() + second.audio.getNonFiniteBlocks(), 0);
        }
    }

    // ---- 4 ---------------------------------------------------------------------------------------
    void silence()
    {
        const int dT = insertDest(0, 0);
        for (auto type : soundEngines())
        {
            beginTest(nameOf(type) + ": with no voice sounding a free LFO on DLY TIME goes on moving the cell, before the first note and after the last, and steps nowhere");
            Rig g(type);
            g.audio.addInsertEffect(0, EffectType::DelayEffect);
            g.set(WET, 0.0f);                               // (the dry sound alone: what is heard is the voices)
            g.lfo(0, dT, 0.5f, 2.0f, SIN, FREE);
            std::vector<float> offsets, values;
            auto note = [&](int) { offsets.push_back(g.offset(dT)); values.push_back(g.cells(0)->getMovedCell(0)); };
            // 0.5 s of silence, a note of 0.3 s (a few hits of the kit), then 3.5 s for it to end and more silence.
            g.render(190, {}, note);
            const float before = range(offsets);
            const float valuesBefore = *std::max_element(values.begin(), values.end()) / *std::min_element(values.begin(), values.end());
            g.render(110, [&](int n) { g.play(n); }, note);
            g.stop();
            g.render(1300, {}, note);
            const std::vector<float> tail(offsets.end() - 190, offsets.end());
            expect(before > 0.9f, "before the first note the offset swings " + juce::String(before, 3) + " (the whole of +-0.5 in half a second)");
            expect(valuesBefore > 4.0f, "and TIME with it: by a factor of " + juce::String(valuesBefore, 2));
            expect(range(tail) > 0.9f, "after the last note: " + juce::String(range(tail), 3));
            const Stereo quiet = g.render(20);
            expect(peak(quiet) < 1.0e-3f, "(nothing sounds by then: " + juce::String(peak(quiet), 5) + ")");
            // A free LFO runs in step in every voice: no step where the lead passes to the note's
            // voice and none where the note ends (a sine of 2 Hz at +-0.5 moves 0.017 a block at most).
            float worst = 0.0f;
            for (size_t i = 1; i < offsets.size(); ++i)
                worst = std::max(worst, std::abs(offsets[i] - offsets[i - 1]));
            expect(worst < 0.03f, "the largest step of the offset from one block to the next: " + juce::String(worst, 4));
            expect(*std::max_element(offsets.begin(), offsets.end()) > 0.45f && *std::min_element(offsets.begin(), offsets.end()) < -0.45f,
                   "both ends are reached");
        }
    }

    // ---- 5 ---------------------------------------------------------------------------------------
    void leader()
    {
        const int d = insertDest(0, 0);
        beginTest("The rule, on a router and three voices: the newest note that sounds leads; when it ends, the newest still sounding; with none, the last leader goes on; before any note, the first voice looked at");
        {
            static const mod::DestTable table = []
            {
                mod::DestTable t;
                t.group();
                t.input("PTCH", 1.0f, 1.0f, 0.0f, 64.0f, mod::Pitch);
                t.input("AMP", 1.0f, 1.0f, 0.0f, 4.0f, mod::PerSample);
                t.finish();
                return t;
            }();
            R r(table, 2);
            r.prepare(SR);
            std::array<mod::Voice, 3> v;
            for (auto& voice : v)
                voice.prepare(SR);
            const int dest = 37;
            r.set(ModSlotParam::id(0, ModSlotParam::SRC), (float) r.sourceIndex(mod::Src::Velocity));
            r.set(ModSlotParam::id(0, ModSlotParam::DEST), (float) r.trackSlotIndex(dest));
            r.set(ModSlotParam::id(0, ModSlotParam::AMT), 1.0f);
            // Sub-blocks in which the named voices sound; `backwards`: the engine looks at them in the other order.
            auto run = [&](std::initializer_list<int> sounding, bool backwards = false)
            {
                for (int chunk = 0; chunk < 3; ++chunk)
                {
                    r.beginBlock(nullptr, nullptr, 0, 0.0f, 0.0f, SR);
                    const int n = r.beginChunk(0, 16);
                    for (int k = 0; k < 3; ++k)
                    {
                        const int i = backwards ? 2 - k : k;
                        if (std::find(sounding.begin(), sounding.end(), i) != sounding.end())
                            expect(! v[(size_t) i].tick(r, n, 1.0f, 1.0f, 0.0f), "a slot on an effect cell moves nothing of the voice: the plain path");
                        else
                            expect(! v[(size_t) i].idle(r, n), "... and leaves it nothing to give back");
                    }
                }
                return r.trackOffset(dest);
            };
            expectEquals(run({}), 0.0f, "no note yet: a voice leads, with no velocity");
            expect(r.trackDests().test(dest), "the cell is in the set");
            for (int other = 0; other < mod::TrackDest::COUNT; ++other)
                expect(r.trackDests().test(other) == (other == dest), "and no other");
            v[1].noteOn(r, 60, 0.4f);
            expectEquals(run({ 1 }), 0.4f, "the first note");
            v[2].noteOn(r, 64, 0.9f);
            expectEquals(run({ 1, 2 }), 0.9f, "the second, newer");
            expectEquals(run({ 1, 2 }, true), 0.9f, "whatever the order the voices are looked at in");
            expectEquals(run({ 1 }), 0.4f, "the second has ended: the first, still sounding");
            expectEquals(run({}), 0.4f, "none sounds: the last leader goes on");
            v[0].noteOn(r, 48, 0.2f);
            expectEquals(run({ 0 }), 0.2f, "a new note on a lower voice");
            v[1].noteOn(r, 60, 0.7f);
            expectEquals(run({ 0, 1 }), 0.7f, "a voice struck again is the newest");
            expectEquals(run({ 0, 1 }, true), 0.7f);
            expectEquals(run({ 0 }), 0.2f);
            expectEquals(run({ 2 }), 0.9f, "(a voice that sounds without a new note still has its own)");
            expectEquals(run({}), 0.9f);
            // Aimed elsewhere: the set follows, and the new cell starts from nothing.
            r.set(ModSlotParam::id(0, ModSlotParam::DEST), (float) r.trackSlotIndex(3));
            r.set(ModSlotParam::id(0, ModSlotParam::AMT), 0.5f);
            run({});
            expect(r.trackDests().test(3) && ! r.trackDests().test(dest), "the set follows the cells");
            expectEquals(r.trackOffset(3), 0.45f);
            r.set(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
            run({});
            expect(! r.trackDests().any(), "amount 0: no cell");
            // The same cell from an LFO page's DEST and from the STRIP page.
            r.set(R::lfoId(0, R::DestCell), (float) r.trackCellIndex(79));
            r.set(R::lfoId(0, R::Depth), 1.0f);
            r.set(R::stripDestId(1), (float) r.trackCellIndex(0));
            r.set(R::stripAmountId(1), 1.0f);
            run({ 0 });
            expect(r.trackDests().test(79) && r.trackDests().test(0), "an LFO's DEST and a strip route");
            expect(! r.routed(1) && ! r.routed(2), "they are no destination of the voices");
            r.set(R::lfoId(0, R::DestCell), (float) (r.trackCellIndex(79) + 1));
            expectEquals(r.get(R::lfoId(0, R::DestCell)), (float) r.trackCellIndex(79), "held to the list");

            // A voice that takes the lead at rest reads the strip as it stands, not as it last read it.
            struct Three
            {
                R r { table, 2 };
                std::array<mod::Voice, 3> v;
                float strip = 0.0f, least = 1.0e9f, most = -1.0e9f;
                Three() { r.prepare(SR); for (auto& voice : v) voice.prepare(SR); }
                float run(std::initializer_list<int> sounding, int chunks, int dest)
                {
                    least = 1.0e9f; most = -1.0e9f;
                    for (int chunk = 0; chunk < chunks; ++chunk)
                    {
                        r.beginBlock(nullptr, nullptr, 0, 0.0f, strip, SR);
                        const int n = r.beginChunk(0, 16);
                        for (int i = 0; i < 3; ++i)
                        {
                            if (std::find(sounding.begin(), sounding.end(), i) != sounding.end())
                                v[(size_t) i].tick(r, n, 1.0f, 1.0f, 0.0f);
                            else if (v[(size_t) i].idle(r, n))
                                v[(size_t) i].restore(r, [](int, float) {});
                        }
                        least = std::min(least, r.trackOffset(dest));
                        most = std::max(most, r.trackOffset(dest));
                    }
                    return r.trackOffset(dest);
                }
            };
            {
                // The STRIP page's route (it counts only while the strip is up).
                Three t;
                t.r.set(R::stripDestId(0), (float) t.r.trackCellIndex(5));
                t.r.set(R::stripAmountId(0), 1.0f);
                t.strip = 1.0f;
                t.v[0].noteOn(t.r, 60, 0.8f);
                expectEquals(t.run({ 0 }, 40, 5), 1.0f, "strip up, voice 1 sounds");
                expectEquals(t.run({}, 5, 5), 1.0f, "it ends and goes on");
                t.v[1].noteOn(t.r, 62, 0.8f);
                expectEquals(t.run({ 1 }, 5, 5), 1.0f, "voice 2 takes over");
                expectEquals(t.least, 1.0f, "without a dip");
                t.strip = 0.0f;
                expectEquals(t.run({ 1 }, 40, 5), 0.0f, "strip down");
                // (Voice 2's route is closed now; voice 1, at rest, last read the strip when it was up.)
                t.run({ 1 }, 40, 5);
                expectEquals(t.most, 0.0f, "and nothing comes back from a voice that read the strip long ago");
                t.run({}, 40, 5);
                expectEquals(t.most, 0.0f);
            }
            {
                // Every voice sounds, the strip is down, and a slot that held the cell goes: the
                // strip's route on the same cell is worth nothing, and so is the cell.
                Three t;
                t.r.set(R::stripDestId(0), (float) t.r.trackCellIndex(5));
                t.r.set(R::stripAmountId(0), 1.0f);
                t.r.set(ModSlotParam::id(0, ModSlotParam::SRC), (float) t.r.sourceIndex(mod::Src::One));
                t.r.set(ModSlotParam::id(0, ModSlotParam::DEST), (float) t.r.trackSlotIndex(5));
                t.r.set(ModSlotParam::id(0, ModSlotParam::AMT), 0.7f);
                for (int i = 0; i < 3; ++i)
                    t.v[(size_t) i].noteOn(t.r, 60 + i, 0.8f);
                expectEquals(t.run({ 0, 1, 2 }, 10, 5), 0.7f, "a slot and a strip route on one cell, strip down");
                t.r.set(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
                expectEquals(t.run({ 0, 1, 2 }, 10, 5), 0.0f, "the slot gone: the strip's route alone, strip down");
                expect(t.r.trackDests().test(5), "(the cell is still aimed at)");
            }
            {
                // An engine's own source that is samples (an operator's output): the leader at rest
                // makes none, and what another voice last handed over is not its.
                static const mod::SourceTable sources = []
                {
                    mod::SourceTable list(2, 0);
                    list.publish("OWN", mod::AudioRate);
                    return list;
                }();
                R r(table, 2, 0, &sources);
                r.prepare(SR);
                std::array<mod::Voice, 2> v;
                for (auto& voice : v)
                    voice.prepare(SR);
                r.set(ModSlotParam::id(0, ModSlotParam::SRC), (float) r.sourceIndex(mod::Src::Published1));
                r.set(ModSlotParam::id(0, ModSlotParam::DEST), (float) r.trackSlotIndex(4));
                r.set(ModSlotParam::id(0, ModSlotParam::AMT), 1.0f);
                std::array<float, R::MAX_CHUNK> made;
                made.fill(0.75f);
                auto run = [&](bool sounding)
                {
                    for (int chunk = 0; chunk < 6; ++chunk)
                    {
                        r.beginBlock(nullptr, nullptr, 0, 0.0f, 0.0f, SR);
                        const int n = r.beginChunk(0, 16);
                        if (sounding)
                        {
                            expect(r.wants(0), "the source is asked for");
                            v[0].tick(r, n, 1.0f, 1.0f, 0.0f);
                            v[0].publishAudio(0, made.data(), n);
                        }
                        else
                            v[0].idle(r, n);
                        v[1].idle(r, n);
                    }
                    return r.trackOffset(4);
                };
                v[0].noteOn(r, 60, 0.8f);
                expectEquals(run(true), 0.75f, "the voice's own samples reach the cell (as they stand at a sub-block's end)");
                expectEquals(run(false), 0.0f, "at rest it makes none");
            }
            {
                // A leader that also had a destination of its own moved: when it comes to rest it
                // lets that go and goes on leading without a dip.
                Three t;
                t.r.set(ModSlotParam::id(0, ModSlotParam::SRC), (float) t.r.sourceIndex(mod::Src::Strip));
                t.r.set(ModSlotParam::id(0, ModSlotParam::DEST), (float) t.r.trackSlotIndex(9));
                t.r.set(ModSlotParam::id(0, ModSlotParam::AMT), 1.0f);
                t.r.set(ModSlotParam::id(1, ModSlotParam::SRC), (float) t.r.sourceIndex(mod::Src::One));
                t.r.set(ModSlotParam::id(1, ModSlotParam::DEST), 2.0f);     // AMP, the voice's
                t.r.set(ModSlotParam::id(1, ModSlotParam::AMT), 0.5f);
                t.strip = 0.8f;
                t.v[2].noteOn(t.r, 60, 0.8f);
                expectEquals(t.run({ 2 }, 40, 9), 0.8f, "strip at 0.8, voice 3 sounds");
                t.run({}, 40, 9);
                expectEquals(t.least, 0.8f, "at rest: no dip");
                expectEquals(t.most, 0.8f, "and no bump");
            }
        }

        for (auto type : soundEngines())
        {
            if (type == EngineType::Drum)
                continue;
            beginTest(nameOf(type) + ": the note played last leads; with none sounding the last one goes on; a new note takes over");
            Rig g(type);
            g.audio.addInsertEffect(0, EffectType::DelayEffect);
            g.set(WET, 0.0f);
            Spy* spy = g.spy(0);
            g.slot(0, mod::Src::Velocity, d, 1.0f);
            g.e().noteOn(48, 0.3f, -1);
            g.render(30);
            expectWithinAbsoluteError(spy->now[0], 0.3f, 1.0e-4f, "the first note's velocity");
            g.e().noteOn(60, 0.9f, -1);
            g.render(30);
            expectWithinAbsoluteError(spy->now[0], 0.9f, 1.0e-4f, "the second note's");
            g.e().noteOff(60, -1);
            g.e().noteOff(48, -1);
            g.render(1900);                                 // (five seconds: any release has ended)
            const float atRest = spy->now[0];
            expect(std::abs(atRest - 0.9f) < 1.0e-4f || std::abs(atRest - 0.3f) < 1.0e-4f,
                   "nothing sounds: the voice that led last goes on (" + juce::String(atRest, 3) + ")");
            g.e().noteOn(72, 0.6f, -1);
            g.render(30);
            expectWithinAbsoluteError(spy->now[0], 0.6f, 1.0e-4f, "and a new note takes over");
        }

        beginTest("drums: the drum struck last leads");
        {
            Rig g(EngineType::Drum);
            g.audio.addInsertEffect(0, EffectType::DelayEffect);
            Spy* spy = g.spy(0);
            g.slot(0, mod::Src::Velocity, d, 1.0f);
            const std::pair<int, float> hits[] = { { 0, 0.4f }, { 2, 0.9f }, { 0, 0.5f }, { 5, 0.2f }, { 2, 0.7f } };
            for (auto& hit : hits)
            {
                g.e().noteOn(hit.first, hit.second, -1);
                g.render(12);
                expectWithinAbsoluteError(spy->now[0], hit.second, 1.0e-4f, "pad " + juce::String(hit.first + 1));
            }
            g.render(1900);
            expectWithinAbsoluteError(spy->now[0], 0.7f, 1.0e-4f, "the kit has rung out: the last drum goes on");
        }
    }

    // ---- 6 ---------------------------------------------------------------------------------------
    void routes()
    {
        const int d = insertDest(0, 0);
        for (auto type : soundEngines())
        {
            beginTest(nameOf(type) + ": the strip's routes, VIA, a slot's amount moved by another, the sum held to -1 .. 1, the XIN page");
            Rig g(type);
            g.audio.addInsertEffect(0, EffectType::DelayEffect);
            Spy* spy = g.spy(0);
            const int stripDest = g.cellId("STRIP", "DST1"), stripAmount = g.cellId("STRIP", "AMT1");
            expect(stripDest >= 0 && stripAmount >= 0, "the STRIP page");
            // (The strip's default routes and vibrato stay as the engine has them: they are the voices'.)
            g.set(stripDest, (float) g.router().trackCellIndex(d));
            g.set(stripAmount, 0.5f);
            g.render(60, [&](int n) { g.play(n); });
            expectEquals(spy->now[0], 0.0f, "strip down: nothing");
            g.e().setModWheel(1.0f);
            g.render(150);
            expectWithinAbsoluteError(spy->now[0], 0.5f, 2.0e-3f, "strip up: its amount");
            g.e().setModWheel(0.0f);
            g.render(150);
            expectEquals(spy->now[0], 0.0f, "strip down again: back to nothing");
            g.stop();
            g.render(1500);                                 // nothing sounds: the strip still plays the effect
            g.e().setModWheel(1.0f);
            g.render(150);
            expectWithinAbsoluteError(spy->now[0], 0.5f, 2.0e-3f, "strip up in silence");
            g.e().setModWheel(0.0f);
            g.render(150);
            expectEquals(spy->now[0], 0.0f, "and down");
            g.set(stripDest, 0.0f);
            g.set(stripAmount, 0.0f);

            g.slot(0, mod::Src::One, d, 0.8f, mod::Src::Strip);
            g.render(30);
            expectEquals(spy->now[0], 0.0f, "VIA the strip, strip down");
            g.e().setModWheel(1.0f);
            g.render(150);
            expectWithinAbsoluteError(spy->now[0], 0.8f, 2.0e-3f, "VIA the strip, strip up");
            g.e().setModWheel(0.0f);
            g.slot(0, mod::Src::Off, -1, 0.0f);
            g.render(150);

            g.slot(0, mod::Src::One, d, 0.2f);
            g.slotOnAmount(1, mod::Src::One, 0, 0.5f);
            g.render(30);
            expectWithinAbsoluteError(spy->now[0], 0.7f, 1.0e-5f, "AMT1 moved by slot 2");
            g.slot(1, mod::Src::One, d, 0.9f);
            g.slot(2, mod::Src::One, d, 0.9f);
            g.render(30);
            expectEquals(spy->now[0], 1.0f, "three slots on one cell: summed, held to 1");
            g.slot(0, mod::Src::One, d, -1.0f);
            g.slot(1, mod::Src::One, d, -1.0f);
            g.slot(2, mod::Src::One, d, -1.0f);
            g.render(30);
            expectEquals(spy->now[0], -1.0f, "... and to -1");
            for (int k = 0; k < 3; ++k)
                g.slot(k, mod::Src::Off, -1, 0.0f);
            g.render(30);
            expectEquals(spy->now[0], 0.0f);

            // The XIN page offers them (its source is the input's samples, read as they stand at a
            // sub-block's end: with no input, nothing).
            const int xinDest = g.cellId("XIN", "DST1"), xinAmount = g.cellId("XIN", "AMT1");
            expect(xinDest >= 0 && xinAmount >= 0, "the XIN page");
            g.set(xinDest, (float) g.router().trackCellIndex(insertDest(0, 2)));
            g.set(xinAmount, 1.0f);
            const size_t mark = spy->calls.size();
            g.render(30);
            expect(spy->count(2, mark) == 30 && ! spy->moved(2, mark), "XIN on a cell with no input: aimed, and nothing");
            expectEquals(g.audio.getNonFiniteBlocks(), 0);
        }

        beginTest("synth: an LFO with a WHL amount (the voices' own path) still plays an effect cell; LFOS below its number takes it away");
        {
            Rig g(EngineType::UnifiedVoice);
            g.audio.addInsertEffect(0, EffectType::DelayEffect);
            Spy* spy = g.spy(0);
            g.lfo(1, insertDest(0, 3), 0.5f, 6.0f);
            g.set(ModParam::lfoExt2ParamId(1, ModParam::LFO_WHEEL), 0.7f);
            g.render(1);
            size_t mark = spy->calls.size();
            g.render(30, [&](int n) { g.play(n); });
            expect(spy->moved(3, mark) && spy->count(3, mark) == 30, "moved");
            mark = spy->calls.size();
            g.set(ModParam::LFO_COUNT, 1.0f);
            g.render(30);
            expectEquals((int) (spy->calls.size() - mark), 1, "LFO 2 is not running: let go once");
            expectEquals(spy->now[3], 0.0f);
        }
    }

    // ---- 7 ---------------------------------------------------------------------------------------
    void clicks()
    {
        beginTest("No click: an LFO sweeping a cell under a held sound steps no more than the sound at rest (the click audit's measure)");
        {
            struct Case { EffectType effect; int dest; int knob; float base; const char* what; std::function<void (Rig&)> more; float depth = 0.3f; };
            const int mf = TrackFxParam::MODFX_BASE;
            const std::vector<Case> cases = {
                { EffectType::DelayEffect, insertDest(0, 0), CELL, 0.12f, "DLY TIME", {}, 0.05f },      // (a time moves by ratio: this is a third up and down)
                { EffectType::DelayEffect, insertDest(0, 1), CELL + 1, 0.5f, "DLY FDBK", {} },
                { EffectType::DelayEffect, insertDest(0, EffectBase::WET_CELL), WET, 0.5f, "DLY WET", {} },
                { EffectType::PhaserEffect, insertDest(0, 0), CELL, 1.0f, "PHS RATE", {} },
                { EffectType::PhaserEffect, insertDest(0, 1), CELL + 1, 0.5f, "PHS DPTH", {} },
                { EffectType::PhaserEffect, insertDest(0, EffectBase::WET_CELL), WET, 0.5f, "PHS WET", {} },
                { EffectType::FilterEffectType, insertDest(0, 0), CELL, 1200.0f, "FLT FREQ", {} },
                { EffectType::TapeDelayEffect, insertDest(0, 0), CELL, 0.2f, "TAPE TIME", {}, 0.05f },
                { EffectType::ChorusEffect, modFxDest(ModFxBlock::ChorusDepth), mf + ModFxBlock::ChorusDepth, 0.5f, "MODFX CH.D", {} },
                { EffectType::ChorusEffect, modFxDest(ModFxBlock::PhaserRate), mf + ModFxBlock::PhaserRate, 0.6f, "MODFX PH.R",
                  [mf](Rig& g) { g.set(mf + ModFxBlock::PhaserDepth, 0.7f); } },
                { EffectType::ChorusEffect, modFxDest(ModFxBlock::RotaryDepth), mf + ModFxBlock::RotaryDepth, 0.5f, "MODFX RO.D", {} } };
            const int blocks = 600, settle = 4800;
            for (auto type : { EngineType::FM, EngineType::Wavetable })
                for (auto& k : cases)
                {
                    auto build = [&](Rig& g)
                    {
                        if (mod::TrackDest::isInsert(k.dest))
                            g.audio.addInsertEffect(0, k.effect);
                        g.set(k.knob, k.base);
                        if (k.more) k.more(g);
                    };
                    // At rest: the knob held at both ends of the sweep and where it is set.
                    float still = 0.0f, top = 0.0f;
                    for (float offset : { -k.depth, 0.0f, k.depth })
                    {
                        Rig g(type);
                        build(g);
                        auto* fx = mod::TrackDest::isInsert(k.dest) ? g.cells(0) : nullptr;
                        const ParamSpec spec = mod::TrackDest::isModFx(k.dest) ? ModFxBlock::spec(k.dest - mod::TrackDest::MODFX)
                                             : fx != nullptr && fx->moves(mod::TrackDest::cell(k.dest)) ? fx->cellSpec(mod::TrackDest::cell(k.dest)) : ParamSpec();
                        expect(spec.isValid(), juce::String(k.what) + " takes modulation");
                        if (! spec.isValid())
                            continue;
                        g.set(k.knob, CellEffect::moved(spec, k.base, offset));
                        const Steps st = steps(g.render(blocks, [&](int n) { g.play(n); }), settle);
                        still = std::max(still, st.step);
                        top = std::max(top, st.peak);
                    }
                    for (int shape : { TRI, SQR })
                    {
                        Rig g(type);
                        build(g);
                        g.lfo(0, k.dest, k.depth, 5.0f, shape);
                        const Stereo out = g.render(blocks, [&](int n) { g.play(n); });
                        const Steps st = steps(out, settle);
                        expect(finite(out));
                        expect(std::abs(g.offset(k.dest)) > 0.0f || shape == TRI, "(moved)");
                        expect(st.step <= 1.5f * still + 0.03f * top,
                               nameOf(type) + " " + k.what + (shape == TRI ? " swept by a triangle" : " thrown by a square") + ": largest step "
                               + juce::String(st.step, 4) + " against " + juce::String(still, 4) + " at rest, peak " + juce::String(top, 3));
                    }
                }
        }

        beginTest("No click when the lead passes from one note to the next (a source that differs per note: the offset steps, the effect glides)");
        {
            for (auto type : { EngineType::FM, EngineType::Wavetable })
            {
                float still = 0.0f, top = 0.0f;
                Stereo out[2];
                for (int with = 0; with < 2; ++with)
                {
                    Rig g(type);
                    g.audio.addInsertEffect(0, EffectType::PhaserEffect);
                    g.audio.addInsertEffect(0, EffectType::DelayEffect);
                    g.set(CELL + 4, 0.1f);
                    if (with)
                    {
                        g.slot(0, mod::Src::Velocity, insertDest(0, 1), 0.6f);
                        g.slot(1, mod::Src::Velocity, insertDest(1, 1), 0.4f);
                        g.slot(2, mod::Src::Velocity, insertDest(1, EffectBase::WET_CELL), -0.8f);
                    }
                    // A held low note, and quiet high notes of changing velocity over it (so quiet
                    // that their own onsets are not what is measured).
                    out[with] = g.render(700, [&](int n)
                    {
                        if (n == 0) g.e().noteOn(40, 1.0f, -1);
                        if (n >= 100 && n % 60 == 0) g.e().noteOn(96, n % 120 == 0 ? 0.02f : 0.01f, -1);
                        if (n >= 100 && n % 60 == 30) g.e().noteOff(96, -1);
                    });
                    const Steps st = steps(out[with], 9600);
                    if (! with) { still = st.step; top = st.peak; }
                    else
                        expect(st.step <= 1.5f * still + 0.03f * top, nameOf(type) + ": largest step " + juce::String(st.step, 4) + " against "
                                                                          + juce::String(still, 4) + " without the slots, peak " + juce::String(top, 3));
                }
                expect(maxDiff(out[0], out[1]) > 1.0e-3f, "(the slots are heard)");
            }
        }
    }

    // ---- 9 ---------------------------------------------------------------------------------------
    void storm()
    {
        beginTest("Random routes on every slot while effects and engines come and go: numbers, nothing left moved when the routes are gone");
        {
            juce::Random rng(41);
            const bool deep = juce::SystemStats::getEnvironmentVariable("NEPTUNE_ROBUST_DEEP", {}).isNotEmpty();
            for (auto type : soundEngines())
            {
                Rig g(type, 64);
                bool numbers = true;
                auto anyEffect = [&] { return (EffectType) rng.nextInt((int) EffectType::NumEffectTypes); };
                auto randomRoutes = [&]
                {
                    const auto& t = g.router().dests();
                    for (auto& page : g.e().getParamPages())
                        for (auto& spec : page.params)
                        {
                            if (spec.choices == t.cellLabels())
                                g.set(spec.id, (float) (rng.nextInt(4) == 0 ? rng.nextInt(t.cellChoices()) : t.trackCell() + rng.nextInt(mod::TrackDest::COUNT)));
                            else if (spec.isValid() && (juce::String(spec.label).startsWith("AMT") || juce::String(spec.label) == "DPTH"))
                                g.set(spec.id, rng.nextFloat() * 2.0f - 1.0f);
                        }
                    for (int k = 0; k < R::NUM_FREE; ++k)
                    {
                        g.set(ModSlotParam::id(k, ModSlotParam::SRC), (float) rng.nextInt(g.router().numSources()));
                        g.set(ModSlotParam::id(k, ModSlotParam::DEST), (float) (rng.nextInt(4) == 0 ? rng.nextInt(t.slotChoices())
                                                                                                   : t.trackSlotChoice() + rng.nextInt(mod::TrackDest::COUNT)));
                        g.set(ModSlotParam::id(k, ModSlotParam::AMT), rng.nextFloat() * 2.0f - 1.0f);
                        g.set(ModSlotParam::id(k, ModSlotParam::VIA), rng.nextInt(3) == 0 ? (float) rng.nextInt(g.router().numSources()) : 0.0f);
                    }
                    if (g.unified())
                        for (int l = 0; l < 4; ++l)
                            g.lfo(l, rng.nextInt(mod::TrackDest::COUNT), rng.nextFloat() * 2.0f - 1.0f, 0.1f + 30.0f * rng.nextFloat(), rng.nextInt(6), rng.nextInt(2));
                    g.e().setModWheel(rng.nextFloat());
                    for (int cell = 0; cell < 8; ++cell)
                        g.set(TrackFxParam::MODFX_BASE + cell, rng.nextFloat());
                };
                for (int round = 0; round < (deep ? 400 : 15); ++round)
                {
                    switch (rng.nextInt(6))
                    {
                        case 0:  g.audio.addInsertEffect(0, anyEffect()); break;
                        case 1:  if (g.effects() > 0) g.audio.removeInsertEffect(0, rng.nextInt(g.effects())); break;
                        case 2:  g.audio.setInsertEffect(0, rng.nextInt(8), anyEffect()); break;
                        case 3:  g.audio.setEngineType(0, soundEngines()[(size_t) rng.nextInt((int) soundEngines().size())], rng.nextBool()); break;
                        default: break;
                    }
                    randomRoutes();
                    g.e().noteOn(g.kit() ? rng.nextInt(16) : 30 + rng.nextInt(60), 0.2f + 0.8f * rng.nextFloat(), -1);
                    const Stereo out = g.render(1 + rng.nextInt(12));
                    numbers = numbers && finite(out);
                    if (rng.nextInt(3) == 0)
                        g.e().allNotesOff();
                }
                expect(numbers, nameOf(type) + ": numbers throughout");
                expectEquals(g.audio.getNonFiniteBlocks(), 0, nameOf(type));
                // The routes gone: nothing stays moved.
                g.audio.setEngineType(0, type, true);
                g.render(4);
                for (int slot = 0; slot < g.effects(); ++slot)
                    if (auto* fx = g.cells(slot))
                        for (int cell = 0; cell <= EffectBase::WET_CELL; ++cell)
                            expectEquals(fx->getModulation(cell), 0.0f, nameOf(type) + ": slot " + juce::String(slot + 1) + " cell " + juce::String(cell));
                // (MODFX modulation check removed: MODFX is no longer a fixed block.)
            }
        }

        beginTest("The same while another thread exchanges the effects (the audio thread only tries the structure lock)");
        {
            Rig g(EngineType::FM, 64);
            for (auto t : { EffectType::DelayEffect, EffectType::PhaserEffect, EffectType::ChorusEffect })
                g.audio.addInsertEffect(0, t);
            for (int k = 0; k < 8; ++k)
                g.slot(k, k % 2 == 0 ? (int) mod::Src::Lfo1 : (int) mod::Src::One, insertDest(k % 4, k % 9), k % 2 == 0 ? 0.5f : -0.3f);
            g.lfo(0, insertDest(0, 0), 0.5f, 8.0f);
            g.lfo(1, modFxDest(1), 0.5f, 3.0f);
            g.e().noteOn(48, 0.9f, -1);
            std::atomic<bool> done { false };
            std::thread worker([&]
            {
                juce::Random rng(7);
                for (int i = 0; i < 120; ++i)
                {
                    const auto type = (EffectType) rng.nextInt((int) EffectType::NumEffectTypes);
                    switch (i % 4)
                    {
                        case 0:  g.audio.setInsertEffect(0, rng.nextInt(4), type); break;
                        case 1:  g.audio.removeInsertEffect(0, rng.nextInt(3)); break;
                        case 2:  g.audio.addInsertEffect(0, type); break;
                        default: g.audio.setInsertEffect(0, 0, type); break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                done.store(true);
            });
            bool numbers = true;
            int blocks = 0;
            while (! done.load() || blocks < 50)
            {
                g.audio.renderBlock(g.l.data(), g.r.data(), 64);
                for (int i = 0; i < 64; ++i)
                    numbers = numbers && std::abs(g.l[(size_t) i]) <= 16.0f && std::abs(g.r[(size_t) i]) <= 16.0f;
                ++blocks;
            }
            worker.join();
            expect(numbers, "numbers throughout");
            expectEquals(g.audio.getNonFiniteBlocks(), 0);
        }
    }

    // ---- the cost --------------------------------------------------------------------------------
    // Microseconds a 128-sample block of one track (eight held notes, a delay and a phaser on it):
    // no slot on an effect cell; one and sixteen slots aimed at an insert slot with nothing in it
    // (what the router and the hand-over cost); one and sixteen on the cells of the two effects
    // (with what the effects pay to follow a cell that moves). NEPTUNE_E4_BENCH=1 on the Release
    // binary; the "no slot" column is what a tree without E4 is measured against (the same rig
    // compiled into it).
    void bench()
    {
        beginTest("cost");
        std::cout << "E4 cost: microseconds a 128-sample block at 48 kHz, eight held notes, DLY and PHS on the track; best of 9 runs of 3000 blocks\n";
        static const char* const rows[5] = { "no slot", "one, on nothing", "sixteen, on nothing", "one", "sixteen" };
        for (auto type : soundEngines())
        {
            double best[5] = { 1.0e9, 1.0e9, 1.0e9, 1.0e9, 1.0e9 };
            for (int run = 0; run < 9; ++run)
                for (int row = 0; row < 5; ++row)
                {
                    Rig g(type);
                    g.audio.addInsertEffect(0, EffectType::DelayEffect);
                    g.audio.addInsertEffect(0, EffectType::PhaserEffect);
                    const int count = row == 0 ? 0 : (row % 2 == 1 ? 1 : 16);
                    for (int k = 0; k < count; ++k)
                        g.slot(k, k % 2 == 0 ? (int) mod::Src::Lfo1 : (int) mod::Src::Env, insertDest((row < 3 ? 6 : 0) + k % 2, k % 3), 0.3f);
                    for (int n = 0; n < 8; ++n)
                        g.e().noteOn(g.kit() ? n : 40 + 3 * n, 0.8f, -1);
                    for (int n = 0; n < 200; ++n)
                        g.audio.renderBlock(g.l.data(), g.r.data(), BLOCK);
                    const auto t0 = std::chrono::steady_clock::now();
                    for (int n = 0; n < 3000; ++n)
                    {
                        if (g.kit() && n % 40 == 0)
                            for (int p = 0; p < 8; ++p)
                                g.e().noteOn(p, 0.8f, -1);
                        g.audio.renderBlock(g.l.data(), g.r.data(), BLOCK);
                    }
                    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 3000.0;
                    best[row] = std::min(best[row], us);
                }
            std::cout << "  " << nameOf(type).paddedRight(' ', 10);
            for (int row = 0; row < 5; ++row)
                std::cout << " | " << rows[row] << " " << juce::String(best[row], 2);
            std::cout << std::endl;
        }
        expect(true);
    }
};

static EffectDestinationsTests effectDestinationsTests;
