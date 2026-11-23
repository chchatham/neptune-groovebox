#pragma once

#include "../sequencer/Pattern.h"
#include "../sequencer/SongMode.h"
#include "../controller/CustomScaleManager.h"
#include "../engine/AftertouchRouter.h"
#include "../engine/ModRouter.h"
#include "Preset.h"
#include "StoredLists.h"
#include <juce_core/juce_core.h>
#include <array>
#include <vector>

// ---------------------------------------------------------------------------
// Project — full project state with JSON serialization and autosave.
//
// File format: a directory containing:
//   project.json   — all state except audio samples
//   samples/       — audio sample files referenced by sampler tracks
//
// JSON uses juce::var / juce::JSON for zero-dependency serialization.
// ---------------------------------------------------------------------------

// A list of numbers in a file (an ARCH memory, recorded PERF moves, the chord pads, the PERF pads,
// the roll, an arpeggiator, the master section, a chord set): a JSON array of them, and back. What
// is no array reads as an empty list.
inline juce::var floatsToVar(const std::vector<float>& values)
{
    juce::Array<juce::var> array;
    for (float v : values)
        array.add((double) v);
    return array;
}

inline std::vector<float> floatsFromVar(const juce::var& stored)
{
    std::vector<float> values;
    if (auto* array = stored.getArray())
    {
        values.reserve((size_t) array->size());
        for (auto& v : *array)
            values.push_back((float) (double) v);
    }
    return values;
}

// ---------------------------------------------------------------------------
// ArpData — serializable per-track arpeggiator settings.
// ---------------------------------------------------------------------------

struct ArpData
{
    int mode = 0;            // Arpeggiator::Mode cast to int
    int rate = 4;            // Arpeggiator::Rate cast to int (default Eighth)
    int octaveRange = 1;
    float gate = 0.5f;
    float swing = 0.0f;
    bool latch = false;
    bool enabled = false;

    // Strum
    bool strumEnabled = false;
    float strumTime = 20.0f;
    int strumDirection = 0;
    float strumVelCurve = 0.0f;

    // Ratchet
    bool ratchetEnabled = false;
    int ratchetCount = 1;
    float ratchetDecay = 0.0f;

    // Extended
    int noteRepeats = 1;
    float glideTime = 0.0f;
    bool humanizeEnabled = false;
    float humanizeAmount = 0.5f;

    // Pressure routing
    int pressureTarget = 2;    // PressureTarget::GateLength
    float pressureAmount = 0.0f;

    // Macro interaction
    int macroInteraction = 0;  // MacroInteraction::Additive

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("mode", mode);
        obj->setProperty("rate", rate);
        obj->setProperty("octRange", octaveRange);
        obj->setProperty("gate", (double) gate);
        obj->setProperty("swing", (double) swing);
        obj->setProperty("latch", latch);
        obj->setProperty("enabled", enabled);
        obj->setProperty("strumOn", strumEnabled);
        obj->setProperty("strumT", (double) strumTime);
        obj->setProperty("strumDir", strumDirection);
        obj->setProperty("strumVel", (double) strumVelCurve);
        obj->setProperty("ratchOn", ratchetEnabled);
        obj->setProperty("ratchCnt", ratchetCount);
        obj->setProperty("ratchDec", (double) ratchetDecay);
        obj->setProperty("noteRep", noteRepeats);
        obj->setProperty("glide", (double) glideTime);
        obj->setProperty("humanize", humanizeEnabled);
        obj->setProperty("humAmt", (double) humanizeAmount);
        obj->setProperty("pressTgt", pressureTarget);
        obj->setProperty("pressAmt", (double) pressureAmount);
        obj->setProperty("macroInt", macroInteraction);
        return juce::var(obj);
    }

    static ArpData fromVar(const juce::var& v)
    {
        ArpData a;
        if (auto* obj = v.getDynamicObject())
        {
            a.mode = (int) obj->getProperty("mode");
            a.rate = (int) obj->getProperty("rate");
            a.octaveRange = juce::jlimit(1, 8, (int) obj->getProperty("octRange"));
            a.gate = (float)(double) obj->getProperty("gate");
            a.swing = (float)(double) obj->getProperty("swing");
            a.latch = (bool) obj->getProperty("latch");
            a.enabled = (bool) obj->getProperty("enabled");
            a.strumEnabled = (bool) obj->getProperty("strumOn");
            a.strumTime = (float)(double) obj->getProperty("strumT");
            a.strumDirection = (int) obj->getProperty("strumDir");
            a.strumVelCurve = (float)(double) obj->getProperty("strumVel");
            a.ratchetEnabled = (bool) obj->getProperty("ratchOn");
            a.ratchetCount = juce::jlimit(1, 4, (int) obj->getProperty("ratchCnt"));
            a.ratchetDecay = (float)(double) obj->getProperty("ratchDec");
            a.noteRepeats = juce::jlimit(1, 8, (int) obj->getProperty("noteRep"));
            a.glideTime = (float)(double) obj->getProperty("glide");
            a.humanizeEnabled = (bool) obj->getProperty("humanize");
            a.humanizeAmount = (float)(double) obj->getProperty("humAmt");
            a.pressureTarget = (int) obj->getProperty("pressTgt");
            a.pressureAmount = (float)(double) obj->getProperty("pressAmt");
            a.macroInteraction = (int) obj->getProperty("macroInt");
        }
        return a;
    }
};

// ---------------------------------------------------------------------------
// AftertouchRouteData — serializable aftertouch routing configuration.
// ---------------------------------------------------------------------------

struct AftertouchRouteData
{
    struct RouteEntry
    {
        int destination = 0;   // AftertouchRouter::Destination cast to int
        float depth = 1.0f;
        bool bipolar = false;
    };

    std::vector<RouteEntry> routes;

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        juce::Array<juce::var> arr;
        for (auto& r : routes)
        {
            auto* ro = new juce::DynamicObject();
            ro->setProperty("dest", r.destination);
            ro->setProperty("depth", (double) r.depth);
            ro->setProperty("bipolar", r.bipolar);
            arr.add(juce::var(ro));
        }
        obj->setProperty("routes", arr);
        return juce::var(obj);
    }

    static AftertouchRouteData fromVar(const juce::var& v)
    {
        AftertouchRouteData data;
        if (auto* obj = v.getDynamicObject())
        {
            if (auto* arr = obj->getProperty("routes").getArray())
            {
                for (auto& rv : *arr)
                {
                    if (auto* ro = rv.getDynamicObject())
                    {
                        RouteEntry r;
                        r.destination = (int) ro->getProperty("dest");
                        r.depth = (float)(double) ro->getProperty("depth");
                        r.bipolar = (bool) ro->getProperty("bipolar");
                        data.routes.push_back(r);
                    }
                }
            }
        }
        return data;
    }
};

// ---------------------------------------------------------------------------
// SendBusData — serializable send bus configuration.
// ---------------------------------------------------------------------------

// The bus's chain is stored as a track's insert slots are, with the same ids (ProjectBridge:
// 232 + slot the type + 1, 200 + slot * 4 + n the cells, 280.. ON / WET / SYNC / division,
// 312 + slot * 4 + n the cells beyond four). A file from before that ("chain" absent) names one
// effect type a bus and nothing else; `effectType` is still written (the first slot's type) for
// a build that reads only that.
struct SendBusData
{
    int effectType = 6;       // EffectType cast to int (default ReverbEffect)
    float sendLevel = 1.0f;
    bool hasChain = false;    // `chain` was stored (it may be empty: a send with nothing on it)
    ParamMap chain;

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("type", effectType);
        obj->setProperty("level", (double) sendLevel);
        if (hasChain)
        {
            juce::Array<juce::var> cells;
            for (auto& [id, val] : chain)
            {
                auto* p = new juce::DynamicObject();
                p->setProperty("id", id);
                p->setProperty("v", (double) val);
                cells.add(juce::var(p));
            }
            obj->setProperty("chain", cells);
        }
        return juce::var(obj);
    }

    static SendBusData fromVar(const juce::var& v)
    {
        SendBusData d;
        if (auto* obj = v.getDynamicObject())
        {
            d.effectType = (int) obj->getProperty("type");
            d.sendLevel = (float)(double) obj->getProperty("level");
            if (auto* cells = obj->getProperty("chain").getArray())
            {
                d.hasChain = true;
                for (auto& cell : *cells)
                    if (auto* c = cell.getDynamicObject())
                        d.chain[(int) c->getProperty("id")] = (float)(double) c->getProperty("v");
            }
        }
        return d;
    }
};

struct TrackData
{
    EngineType engineType = EngineType::UnifiedVoice;
    ParamMap soundParams;
    int length = 16;
    float speedMultiplier = 1.0f;
    int midiChannel = 1;
    bool muted = false;
    bool soloed = false;
    // The name of the sample a sampler track plays (or a wavetable track's USER table is built from).
    // The parameter itself is an index into the sample bank, which shifts when files are added; the
    // name is what a project is loaded by. Empty in older projects (the index is then used as it is).
    juce::String sampleName;
    // The same for the samples of a wavetable track's key frames (SMP1-8): eight names, empty for a
    // key that is OFF. Not present in older projects (there are no key frames in them).
    juce::StringArray keySamples;
    // The name of the preset the track's sound came from (kept with LD, or the name it was saved
    // under): what the save box offers. Empty = none, and in older projects.
    juce::String presetName;

    // Per-track arpeggiator settings
    ArpData arpSettings;

    // Architecture memory for unified-voice tracks: each ARCH slot's tweaked params.
    // Empty for non-unified-voice tracks and projects saved before this field existed.
    std::vector<float> archMemory;

    // Steps serialized inline
    struct StepData
    {
        bool trig = false;
        std::vector<Note> notes;
        float velocity = 1.0f;
        float length = 1.0f;
        float microTiming = 0.0f;
        TrigCondition trigConditionType = TrigCondition::Always;
        int trigA = 1, trigB = 2;
        float trigProbability = 1.0f;
        ParamMap paramLocks;
        int pageChangeLock = -1;

        // Recorded pad pressure lives in the notes (Note::lane); this is the step's PRES amount.
        float pressureScale = 1.0f;

        // A lane in a project file: two hex digits per point ("lane"), and its spacing ("laneShift",
        // left out when 0). A note without pressure has neither.
        static juce::String laneToHex(const PressureLane& lane)
        {
            static const char digits[] = "0123456789abcdef";
            juce::String hex;
            hex.preallocateBytes(lane.points.size() * 2 + 1);
            for (auto p : lane.points)
                hex << digits[p >> 4] << digits[p & 15];
            return hex;
        }

        static void laneFromHex(const juce::String& hex, PressureLane& lane)
        {
            lane.points.clear();
            const int count = juce::jmin(hex.length() / 2, PressureLane::MAX_POINTS);
            lane.points.reserve((size_t) count);
            for (int i = 0; i < count; ++i)
            {
                const int hi = juce::CharacterFunctions::getHexDigitValue((juce::juce_wchar) hex[i * 2]);
                const int lo = juce::CharacterFunctions::getHexDigitValue((juce::juce_wchar) hex[i * 2 + 1]);
                if (hi < 0 || lo < 0) { lane.points.clear(); return; }
                lane.points.push_back((uint8_t) (hi * 16 + lo));
            }
        }

        juce::var toVar(int engineTypeInt = -1) const
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty("trig", trig);
            obj->setProperty("vel", (double) velocity);
            obj->setProperty("len", (double) length);
            obj->setProperty("micro", (double) microTiming);
            obj->setProperty("trigType", static_cast<int>(trigConditionType));
            obj->setProperty("trigA", trigA);
            obj->setProperty("trigB", trigB);
            obj->setProperty("trigProb", (double) trigProbability);
            obj->setProperty("pageLock", pageChangeLock);

            if (!notes.empty())
            {
                juce::Array<juce::var> notesArr;
                for (auto& n : notes)
                {
                    auto* no = new juce::DynamicObject();
                    no->setProperty("pitch", n.pitch);
                    no->setProperty("vel", (double) n.velocity);
                    if (!n.lane.empty())
                    {
                        no->setProperty("lane", laneToHex(n.lane));
                        if (n.lane.shift != 0)
                            no->setProperty("laneShift", (int) n.lane.shift);
                    }
                    notesArr.add(juce::var(no));
                }
                obj->setProperty("notes", notesArr);
            }

            if (!paramLocks.empty())
            {
                juce::Array<juce::var> locksArr;
                for (auto& [id, val] : paramLocks)
                {
                    auto* lo = new juce::DynamicObject();
                    lo->setProperty("id", id);
                    lo->setProperty("v", (double) val);
                    // (a lock on a cell that is a position in a list: by the entry's name too)
                    if (const char* entry = engineTypeInt >= 0 ? stored::nameOf(engineTypeInt, id, val) : nullptr)
                        lo->setProperty("n", juce::String(entry));
                    locksArr.add(juce::var(lo));
                }
                obj->setProperty("locks", locksArr);
            }

            if (pressureScale != 1.0f)
                obj->setProperty("pres", (double) pressureScale);

            return juce::var(obj);
        }

        // version: the file's (3: a lock's name decides; before: the numbers are read as they are
        // here, and whoever knows the track's sound translates them: stored::old::locks).
        // lost: names their lists no longer have (nullptr: not collected).
        static StepData fromVar(const juce::var& v, int version = 2, int engineTypeInt = -1, juce::StringArray* lost = nullptr)
        {
            StepData s;
            if (auto* obj = v.getDynamicObject())
            {
                s.trig = (bool) obj->getProperty("trig");
                s.velocity = (float)(double) obj->getProperty("vel");
                s.length = (float)(double) obj->getProperty("len");
                s.microTiming = (float)(double) obj->getProperty("micro");
                s.trigConditionType = static_cast<TrigCondition>((int) obj->getProperty("trigType"));
                s.trigA = (int) obj->getProperty("trigA");
                s.trigB = (int) obj->getProperty("trigB");
                s.trigProbability = (float)(double) obj->getProperty("trigProb");
                s.pageChangeLock = (int) obj->getProperty("pageLock");

                if (auto* notesArr = obj->getProperty("notes").getArray())
                {
                    for (auto& nv : *notesArr)
                    {
                        if (auto* no = nv.getDynamicObject())
                        {
                            Note n;
                            n.pitch = (int) no->getProperty("pitch");
                            n.velocity = (float)(double) no->getProperty("vel");
                            if (no->hasProperty("lane"))
                            {
                                laneFromHex(no->getProperty("lane").toString(), n.lane);
                                n.lane.shift = (uint8_t) juce::jlimit(0, PressureLane::MAX_SHIFT, (int) no->getProperty("laneShift"));
                            }
                            s.notes.push_back(n);
                        }
                    }
                }

                if (auto* locksArr = obj->getProperty("locks").getArray())
                {
                    for (auto& lv : *locksArr)
                    {
                        if (auto* lo = lv.getDynamicObject())
                        {
                            int id = (int) lo->getProperty("id");
                            float val = (float)(double) lo->getProperty("v");
                            if (version >= 3 && engineTypeInt >= 0)
                            {
                                juce::StringArray unused;
                                val = stored::loaded(engineTypeInt, id, val, lo->getProperty("n"), lost != nullptr ? *lost : unused);
                            }
                            s.paramLocks[id] = val;
                        }
                    }
                }

                // (An older file's per-step "pressure" array was never played or recorded: ignored.)
                if (obj->hasProperty("pres"))
                    s.pressureScale = juce::jlimit(0.0f, 2.0f, (float)(double) obj->getProperty("pres"));
            }
            return s;
        }
    };

    std::vector<StepData> steps;

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("engine", static_cast<int>(engineType));
        obj->setProperty("length", length);
        obj->setProperty("speed", (double) speedMultiplier);
        obj->setProperty("midiCh", midiChannel);
        obj->setProperty("muted", muted);
        obj->setProperty("soloed", soloed);
        if (sampleName.isNotEmpty())
            obj->setProperty("sample", sampleName);
        if (! keySamples.isEmpty())
        {
            juce::Array<juce::var> names;
            for (auto& n : keySamples)
                names.add(n);
            obj->setProperty("keySamples", names);
        }
        if (presetName.isNotEmpty())
            obj->setProperty("preset", presetName);

        // Sound params (format 3: named-list cells carry an "n" field with the entry's label)
        juce::Array<juce::var> paramsArr;
        for (auto& [id, val] : soundParams)
        {
            auto* p = new juce::DynamicObject();
            p->setProperty("id", id);
            p->setProperty("v", (double) val);
            if (const char* entry = stored::nameOf((int) engineType, id, val))
                p->setProperty("n", juce::String(entry));
            paramsArr.add(juce::var(p));
        }
        obj->setProperty("params", paramsArr);

        // Steps (only serialize active region; format 3: step locks carry names too)
        juce::Array<juce::var> stepsArr;
        for (auto& s : steps)
            stepsArr.add(s.toVar((int) engineType));
        obj->setProperty("steps", stepsArr);

        // Arp settings
        obj->setProperty("arp", arpSettings.toVar());

        // Architecture memory (unified-voice tracks only; absent in older projects)
        if (!archMemory.empty())
        {
            obj->setProperty("archMem", floatsToVar(archMemory));

            // The positions among them, by name (a slot's LFO DEST, shape, filter type ...).
            juce::Array<juce::var> names;
            auto memory = archMemory;
            forEachArchCell(memory, [&](int slot, int id, float& value)
            {
                if (const char* entry = stored::nameOf((int) engineType, id, value))
                {
                    auto* n = new juce::DynamicObject();
                    n->setProperty("slot", slot);
                    n->setProperty("id", id);
                    n->setProperty("n", juce::String(entry));
                    names.add(juce::var(n));
                }
            });
            if (! names.isEmpty())
                obj->setProperty("archNames", names);
        }

        return juce::var(obj);
    }

    // The cells of an ARCH memory (UnifiedVoiceEngine::captureArchMemory: version, slots, then a
    // slot: visited, count, count x (id, value)), each handed over to be read or changed.
    template <typename Fn>
    static void forEachArchCell(std::vector<float>& memory, Fn&& fn)
    {
        if (memory.size() < 2 || (int) std::lround(memory[0]) != 1)
            return;
        const int slots = juce::jlimit(0, 64, (int) std::lround(juce::jlimit(0.0f, 64.0f, memory[1])));
        size_t at = 2;
        for (int s = 0; s < slots && at + 1 < memory.size(); ++s)
        {
            ++at;                                                           // visited
            const float stored = at < memory.size() ? memory[at++] : 0.0f;
            const int count = std::isfinite(stored) ? juce::jlimit(0, 2048, (int) std::lround(juce::jlimit(0.0f, 2048.0f, stored))) : 0;
            for (int i = 0; i < count && at + 1 < memory.size(); ++i, at += 2)
                if (std::isfinite(memory[at]))
                    fn(s, (int) std::lround(juce::jlimit(-1.0f, 1.0e6f, memory[at])), memory[at + 1]);
        }
    }

    // lost: names their lists no longer have.
    static TrackData fromVar(const juce::var& v, int version = 2, juce::StringArray* lost = nullptr)
    {
        juce::StringArray unused;
        juce::StringArray& notes = lost != nullptr ? *lost : unused;
        TrackData t;
        if (auto* obj = v.getDynamicObject())
        {
            t.engineType = static_cast<EngineType>((int) obj->getProperty("engine"));
            t.length = (int) obj->getProperty("length");
            t.speedMultiplier = (float)(double) obj->getProperty("speed");
            t.midiChannel = (int) obj->getProperty("midiCh");
            t.muted = (bool) obj->getProperty("muted");
            t.soloed = (bool) obj->getProperty("soloed");
            t.sampleName = obj->getProperty("sample").toString();
            {
                // (a name as a preset can have it: printable, 15 characters; see PresetManager::cleanName)
                juce::String name;
                for (auto c : obj->getProperty("preset").toString())
                    if (c >= 32 && c <= 126)
                        name += juce::String::charToString(c);
                t.presetName = name.trim().substring(0, 15).trim();
            }
            if (auto* names = obj->getProperty("keySamples").getArray())
                for (auto& n : *names)
                    t.keySamples.add(n.toString());

            const int engineTypeInt = (int) t.engineType;

            if (auto* paramsArr = obj->getProperty("params").getArray())
            {
                for (auto& pv : *paramsArr)
                {
                    if (auto* po = pv.getDynamicObject())
                    {
                        int id = (int) po->getProperty("id");
                        float val = (float)(double) po->getProperty("v");
                        // Format 3: a position is the entry its name says (StoredLists.h). A file of
                        // before is translated as a whole, once every track is read (ProjectData).
                        if (version >= 3)
                            val = stored::loaded(engineTypeInt, id, val, po->getProperty("n"), notes);
                        t.soundParams[id] = val;
                    }
                }
            }

            if (auto* stepsArr = obj->getProperty("steps").getArray())
            {
                for (auto& sv : *stepsArr)
                    t.steps.push_back(StepData::fromVar(sv, version, engineTypeInt, &notes));
            }

            // Arp settings (graceful default if missing from older files)
            auto arpVar = obj->getProperty("arp");
            if (arpVar.getDynamicObject() != nullptr)
                t.arpSettings = ArpData::fromVar(arpVar);

            // Architecture memory (absent in older projects)
            if (obj->getProperty("archMem").isArray())
            {
                t.archMemory = floatsFromVar(obj->getProperty("archMem"));
                if (auto* names = obj->getProperty("archNames").getArray())
                    for (auto& nv : *names)
                        if (auto* n = nv.getDynamicObject())
                        {
                            const int slot = (int) n->getProperty("slot"), id = (int) n->getProperty("id");
                            forEachArchCell(t.archMemory, [&](int s, int cell, float& value)
                            {
                                if (s == slot && cell == id)
                                    value = stored::loaded(engineTypeInt, id, value, n->getProperty("n"), notes);
                            });
                        }
            }
        }
        return t;
    }
};

struct PatternData
{
    std::vector<TrackData> tracks;
    float swing = 0.0f;
    std::vector<float> perfMoves;       // PerfFx::MoveLane::toFlat(); empty = none recorded

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("swing", (double) swing);

        juce::Array<juce::var> tracksArr;
        for (auto& t : tracks)
            tracksArr.add(t.toVar());
        obj->setProperty("tracks", tracksArr);

        if (! perfMoves.empty())
            obj->setProperty("perfMoves", floatsToVar(perfMoves));

        return juce::var(obj);
    }

    static PatternData fromVar(const juce::var& v, int version = 2, juce::StringArray* lost = nullptr)
    {
        PatternData p;
        if (auto* obj = v.getDynamicObject())
        {
            p.swing = (float)(double) obj->getProperty("swing");

            if (auto* tracksArr = obj->getProperty("tracks").getArray())
            {
                for (auto& tv : *tracksArr)
                    p.tracks.push_back(TrackData::fromVar(tv, version, lost));
            }
            p.perfMoves = floatsFromVar(obj->getProperty("perfMoves"));
        }
        return p;
    }
};

struct SongData
{
    std::vector<SongEntry> entries;
    bool loop = true;

    juce::var toVar() const
    {
        juce::Array<juce::var> arr;
        for (auto& e : entries)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty("pattern", e.patternIndex);
            obj->setProperty("repeats", e.repeatCount);
            obj->setProperty("transpose", e.transposeSemitones);
            arr.add(juce::var(obj));
        }

        auto* root = new juce::DynamicObject();
        root->setProperty("entries", arr);
        root->setProperty("loop", loop);
        return juce::var(root);
    }

    static SongData fromVar(const juce::var& v)
    {
        SongData s;
        if (auto* obj = v.getDynamicObject())
        {
            s.loop = (bool) obj->getProperty("loop");

            if (auto* arr = obj->getProperty("entries").getArray())
            {
                for (auto& item : *arr)
                {
                    if (auto* eo = item.getDynamicObject())
                    {
                        SongEntry e;
                        e.patternIndex = (int) eo->getProperty("pattern");
                        e.repeatCount = juce::jlimit(1, 999, (int) eo->getProperty("repeats"));
                        e.transposeSemitones = juce::jlimit(-12, 12, (int) eo->getProperty("transpose"));
                        s.entries.push_back(e);
                    }
                }
            }
        }
        return s;
    }
};

struct ProjectData
{
    juce::String name = "Untitled";
    float bpm = 120.0f;
    int activePattern = 0;
    std::array<PatternData, 16> patterns;
    SongData song;
    juce::String sampleFolder;     // relative path to samples directory

    // Clock settings
    int clockMode = 0;             // 0=Internal, 1=External, 2=Auto
    bool linkEnabled = false;

    // Custom scale pages
    std::vector<ScalePage> customScalePages;
    std::vector<float> chordPads;           // ChordPads::toFlat(); empty = no chord stored
    std::vector<float> perfPads;            // PerfFxRack::toFlat(); empty = the starting layout

    // Aftertouch routing. Per track (index = track); `aftertouchRouting` is the single list of
    // older projects, applied to every track when a project has no per-track lists.
    AftertouchRouteData aftertouchRouting;
    std::vector<AftertouchRouteData> trackAftertouch;

    // Send bus configuration
    std::vector<SendBusData> sendBuses;

    // Master level
    float masterLevel = 0.8f;

    // Pad ratchet (ROLL page) settings, in the order written by ProjectBridge (empty = defaults).
    std::vector<float> roll;

    // Arpeggiator settings per track, each in the order written by ProjectBridge (empty = defaults).
    std::vector<std::vector<float>> arps;

    // The keyboard: its split and its octave. Not stored (an older project) = left as it is.
    KeyboardSetup keyboard;

    // Master section as on the MASTR page: EQ low / mid gain, mid frequency, high gain,
    // compressor threshold / ratio / make-up, limiter ceiling. Empty = defaults.
    std::vector<float> master;

    // The file's version. 3: a cell (a sound's, a step lock's, an ARCH memory's) that is a position
    // in a list carries the entry's name ("n") beside its number, and the name decides on load, so
    // a list that grows in its middle moves no saved route (StoredLists.h says which cells).
    // 1 and 2 hold numbers only: fromVar translates them from the layout each track's ids tell
    // (stored::old; StoredListHistory.h), which for a file of a published build changes no router
    // position and puts a drum kit's pads back on the types they were saved with.
    static constexpr int FORMAT_VERSION = 3;

    // Not stored - what reading the file found: names it holds that their lists no longer have
    // (each once; fromVar has written them to the app's log).
    juce::StringArray lost;

    juce::var toVar() const
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("version", FORMAT_VERSION);
        root->setProperty("name", name);
        root->setProperty("bpm", (double) bpm);
        root->setProperty("activePattern", activePattern);
        root->setProperty("clockMode", clockMode);
        root->setProperty("linkEnabled", linkEnabled);
        root->setProperty("sampleFolder", sampleFolder);
        root->setProperty("masterLevel", (double) masterLevel);
        if (!roll.empty())
            root->setProperty("roll", floatsToVar(roll));
        if (!arps.empty())
        {
            juce::Array<juce::var> allArps;
            for (auto& one : arps)
                allArps.add(floatsToVar(one));
            root->setProperty("arps", allArps);
        }
        if (keyboard.split >= 0)
            root->setProperty("keyboardSplit", keyboard.split);
        if (keyboard.octave != KeyboardSetup::UNKNOWN)
            root->setProperty("keyboardOctave", keyboard.octave);
        if (!master.empty())
            root->setProperty("master", floatsToVar(master));

        // Patterns
        juce::Array<juce::var> patternsArr;
        for (auto& p : patterns)
            patternsArr.add(p.toVar());
        root->setProperty("patterns", patternsArr);

        // Song
        root->setProperty("song", song.toVar());

        if (!chordPads.empty())
            root->setProperty("chordPads", floatsToVar(chordPads));
        if (!perfPads.empty())
            root->setProperty("perfPads", floatsToVar(perfPads));

        // Custom scale pages
        if (!customScalePages.empty())
        {
            juce::Array<juce::var> scaleArr;
            for (auto& sp : customScalePages)
                scaleArr.add(sp.toVar());
            root->setProperty("scalePages", scaleArr);
        }

        // Aftertouch routing
        if (!aftertouchRouting.routes.empty())
            root->setProperty("aftertouch", aftertouchRouting.toVar());
        if (!trackAftertouch.empty())
        {
            juce::Array<juce::var> arr;
            for (auto& t : trackAftertouch)
                arr.add(t.toVar());
            root->setProperty("trackAftertouch", arr);
        }

        // Send buses
        if (!sendBuses.empty())
        {
            juce::Array<juce::var> busArr;
            for (auto& sb : sendBuses)
                busArr.add(sb.toVar());
            root->setProperty("sendBuses", busArr);
        }

        return juce::var(root);
    }

    static ProjectData fromVar(const juce::var& v)
    {
        ProjectData proj;
        if (auto* obj = v.getDynamicObject())
        {
            const int version = juce::jmax(1, (int) obj->getProperty("version"));
            proj.name = obj->getProperty("name").toString();
            proj.bpm = (float)(double) obj->getProperty("bpm");
            proj.activePattern = juce::jlimit(0, 15, (int) obj->getProperty("activePattern"));
            proj.clockMode = juce::jlimit(0, 3, (int) obj->getProperty("clockMode"));
            proj.linkEnabled = (bool) obj->getProperty("linkEnabled");
            proj.sampleFolder = obj->getProperty("sampleFolder").toString();

            // Master level (default 0.8 for older files without this field)
            auto mlVar = obj->getProperty("masterLevel");
            if (!mlVar.isVoid())
                proj.masterLevel = (float)(double) mlVar;
            proj.roll = floatsFromVar(obj->getProperty("roll"));
            if (auto* allArps = obj->getProperty("arps").getArray())
                for (auto& one : *allArps)
                    proj.arps.push_back(floatsFromVar(one));
            if (auto splitVar = obj->getProperty("keyboardSplit"); !splitVar.isVoid())
                proj.keyboard.split = juce::jlimit(0, 3, (int) splitVar);
            if (auto octaveVar = obj->getProperty("keyboardOctave"); !octaveVar.isVoid())
                proj.keyboard.octave = juce::jlimit(-8, 8, (int) octaveVar);
            proj.master = floatsFromVar(obj->getProperty("master"));

            if (auto* pArr = obj->getProperty("patterns").getArray())
            {
                int count = juce::jmin((int) pArr->size(), 16);
                for (int i = 0; i < count; ++i)
                    proj.patterns[(size_t) i] = PatternData::fromVar((*pArr)[i], version, &proj.lost);
            }
            if (version < FORMAT_VERSION)
                translateOld(proj);
            stored::report("project " + proj.name, proj.lost);

            proj.song = SongData::fromVar(obj->getProperty("song"));

            proj.chordPads = floatsFromVar(obj->getProperty("chordPads"));
            proj.perfPads = floatsFromVar(obj->getProperty("perfPads"));

            // Custom scale pages
            if (auto* scaleArr = obj->getProperty("scalePages").getArray())
            {
                for (auto& sv : *scaleArr)
                    proj.customScalePages.push_back(ScalePage::fromVar(sv));
            }

            // Aftertouch routing
            auto atVar = obj->getProperty("aftertouch");
            if (atVar.getDynamicObject() != nullptr)
                proj.aftertouchRouting = AftertouchRouteData::fromVar(atVar);
            if (auto* trackAt = obj->getProperty("trackAftertouch").getArray())
                for (auto& tv : *trackAt)
                    proj.trackAftertouch.push_back(AftertouchRouteData::fromVar(tv));

            // Send buses
            if (auto* busArr = obj->getProperty("sendBuses").getArray())
            {
                for (auto& bv : *busArr)
                    proj.sendBuses.push_back(SendBusData::fromVar(bv));
            }
        }
        return proj;
    }

    // A project of before format 3: every position made today's. A track's sound is on the active
    // pattern (one sound a track for all patterns); what layout it was written in is told from
    // that sound, and the track's step locks in every pattern are read by the same layout.
    static void translateOld(ProjectData& proj)
    {
        auto& sounds = proj.patterns[(size_t) proj.activePattern].tracks;
        for (size_t t = 0; t < sounds.size(); ++t)
        {
            const int type = (int) sounds[t].engineType;
            const auto layout = stored::old::sound(type, sounds[t].soundParams, proj.lost);
            for (size_t p = 0; p < proj.patterns.size(); ++p)
            {
                if (t >= proj.patterns[p].tracks.size())
                    continue;
                auto& track = proj.patterns[p].tracks[t];
                // (a sound kept on another pattern as well, by a build that did: by its own ids)
                if ((int) p != proj.activePattern && ! track.soundParams.empty())
                    stored::old::sound((int) track.engineType, track.soundParams, proj.lost);
                for (auto& step : track.steps)
                    stored::old::locks(type, layout, step.paramLocks, proj.lost);
            }
        }
    }

    juce::String toJSON() const
    {
        return juce::JSON::toString(toVar());
    }

    static ProjectData fromJSON(const juce::String& json)
    {
        auto parsed = juce::JSON::parse(json);
        return fromVar(parsed);
    }
};

// ---------------------------------------------------------------------------
// Project — manages save/load/autosave and recent projects list.
// ---------------------------------------------------------------------------

class Project
{
public:
    static constexpr int MAX_RECENT = 8;
    static constexpr int DEFAULT_AUTOSAVE_MS = 60000; // 1 minute

    Project() = default;

    // --- Current project data -----------------------------------------------

    ProjectData& getData() { return data; }
    const ProjectData& getData() const { return data; }

    // --- File operations ----------------------------------------------------

    bool save(const juce::File& projectDir)
    {
        if (!projectDir.isDirectory())
            projectDir.createDirectory();

        auto jsonFile = projectDir.getChildFile("project.json");
        auto json = data.toJSON();

        if (!jsonFile.replaceWithText(json))
            return false;

        // Ensure samples subdirectory exists
        auto samplesDir = projectDir.getChildFile("samples");
        if (!samplesDir.isDirectory())
            samplesDir.createDirectory();

        data.sampleFolder = "samples";
        currentFile = projectDir;
        dirty = false;

        addToRecent(projectDir.getFullPathName());
        return true;
    }

    bool load(const juce::File& projectDir)
    {
        auto jsonFile = projectDir.getChildFile("project.json");
        if (!jsonFile.existsAsFile())
            return false;

        auto json = jsonFile.loadFileAsString();
        if (json.isEmpty())
            return false;

        data = ProjectData::fromJSON(json);
        currentFile = projectDir;
        dirty = false;

        addToRecent(projectDir.getFullPathName());
        return true;
    }

    bool saveAs(const juce::File& newProjectDir)
    {
        return save(newProjectDir);
    }

    // --- New project --------------------------------------------------------

    void newProject(const juce::String& projectName = "Untitled")
    {
        data = ProjectData();
        data.name = projectName;
        currentFile = juce::File();
        dirty = false;
    }

    // --- Dirty flag (unsaved changes) ---------------------------------------

    bool isDirty() const { return dirty; }
    void markDirty() { dirty = true; }
    void clearDirty() { dirty = false; }

    // --- Current file -------------------------------------------------------

    bool hasFile() const { return currentFile.isDirectory(); }
    juce::File getCurrentFile() const { return currentFile; }
    void setCurrentFile(const juce::File& f) { currentFile = f; }      // (a Save As taken back)
    void setRecentProjects(std::vector<juce::String> paths) { recentPaths = std::move(paths); }
    juce::String getProjectName() const { return data.name; }

    // --- Autosave -----------------------------------------------------------

    void setAutosaveInterval(int ms) { autosaveIntervalMs = juce::jlimit(5000, 600000, ms); }
    int getAutosaveInterval() const { return autosaveIntervalMs; }

    bool shouldAutosave() const
    {
        if (!dirty || !hasFile())
            return false;

        auto now = juce::Time::getMillisecondCounterHiRes();
        return (now - lastSaveTime) >= autosaveIntervalMs;
    }

    void performAutosave()
    {
        if (hasFile() && dirty)
        {
            save(currentFile);
            lastSaveTime = juce::Time::getMillisecondCounterHiRes();
        }
    }

    // --- Recent projects ----------------------------------------------------

    const std::vector<juce::String>& getRecentProjects() const { return recentPaths; }

    void addToRecent(const juce::String& path)
    {
        // Remove if already present
        recentPaths.erase(
            std::remove(recentPaths.begin(), recentPaths.end(), path),
            recentPaths.end());

        // Add to front
        recentPaths.insert(recentPaths.begin(), path);

        // Trim to max
        while ((int) recentPaths.size() > MAX_RECENT)
            recentPaths.pop_back();
    }

    void clearRecent() { recentPaths.clear(); }

    // --- Recent list persistence (stored in app support dir) ----------------

    void saveRecentList(const juce::File& appDataDir) const
    {
        auto recentFile = appDataDir.getChildFile("recent_projects.json");

        juce::Array<juce::var> arr;
        for (auto& p : recentPaths)
            arr.add(juce::var(p));

        auto json = juce::JSON::toString(juce::var(arr));
        recentFile.replaceWithText(json);
    }

    void loadRecentList(const juce::File& appDataDir)
    {
        auto recentFile = appDataDir.getChildFile("recent_projects.json");
        if (!recentFile.existsAsFile())
            return;

        auto parsed = juce::JSON::parse(recentFile.loadFileAsString());
        if (auto* arr = parsed.getArray())
        {
            recentPaths.clear();
            for (auto& item : *arr)
            {
                auto path = item.toString();
                if (path.isNotEmpty())
                    recentPaths.push_back(path);
            }
        }
    }

private:
    ProjectData data;
    juce::File currentFile;
    bool dirty = false;
    int autosaveIntervalMs = DEFAULT_AUTOSAVE_MS;
    double lastSaveTime = 0.0;
    std::vector<juce::String> recentPaths;
};
