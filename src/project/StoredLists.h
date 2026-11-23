#pragma once

#include "StoredListHistory.h"
#include "CymbalPresets.h"
#include "../engine/ModRouter.h"
#include "../engine/UnifiedVoiceEngine.h"
#include "../engine/FMEngine.h"
#include "../engine/MacroOscEngine.h"
#include "../engine/WavetableEngine.h"
#include "../engine/WaveSeqEngine.h"
#include "../engine/SamplerEngine.h"
#include "../engine/DrumEngine.h"
#include "../engine/ResonatorEngine.h"
#include "../engine/VoiceFilterSection.h"
#include "../dsp/LFO.h"
#include "../dsp/filters/FilterSlot.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstring>

// -----------------------------------------------------------------------------
// STORED POSITIONS: the cells of a sound whose value is a place in a list.
//
// A DEST cell, a slot's SRC, an LFO's shape, a filter's TYPE, a drum pad's TYPE: the value a
// project or a preset holds for such a cell is a POSITION, and it means something only as long
// as the list keeps its order. The lists grow, and twice already one grew in its middle. So:
//
//   FORMAT 3 (what this build writes)
//     A cell of a list is written with the entry's NAME beside its number ("n" beside "v"), and
//     the name decides when it is read: listOf(engine type, id) says which list a cell is in
//     TODAY, nameOf() gives the name to write, loaded() the value a stored cell has today.
//     A name the list no longer has becomes OFF where the list has an OFF (every DEST, SRC and
//     VIA), else stays the number the file gave; either way it is said once, where the owner can
//     read it: the app's log (report()).
//     A cell without a name (a file of the hours between the first format-3 build, 3864411,
//     and this one, for the cells that build did not name yet; a value that is no whole number)
//     is its number: no list moved in those hours.
//
//   BEFORE FORMAT 3 (a project of version 1 or 2, a preset file with no version)
//     The file has numbers only, and no stamp that says which build wrote it. old::sound()
//     works out the layout from the ids the sound holds (StoredListHistory.h: the lists as they
//     were, commit by commit, and how a layout is told) and turns every position into today's.
//     For every file a PUBLISHED build wrote - v0.9 to 66a3f06, which is every file the owner
//     has - that changes no router position at all (not one of those lists had changed yet), and
//     it REPAIRS THE DRUM KIT: its TYPE list was re-ordered on 2026-11-21 (U2b), not appended to,
//     and nothing translated the stored numbers, so a kit saved before came back with its pads
//     exchanged (SUB as CSYN, the kick machine as RIM, the hats as KSYN and CYMB ...). old::kit().
//
// WHICH CELLS (listOf). The router's own: mod::NamedList in ModRouter.h, for every engine alike.
// The engines' own: the unified voice's LFO pages (ids 40-63 DEST SHPE TRIG, 130-145 DIV; the
// extra envelopes' DEST 69 + 6 n; TYPE and TYPB of its two filters), the TYPE of each voice
// filter (FM, macro, wavetable with its filter B, wave sequencer, sampler, resonator), the drum
// kit's TYPE per pad and its per-pad LFO and slots (PadModParam, 4000-4255). A pressure range
// (RangeParam) of such a cell is a position of the same list.
// NOT here, and still plain numbers in a file - each a list that is either fixed, or follows the
// state of another cell (so its name could not be read without the rest of the sound), or is
// stored by name already: the effect TYPE of an insert slot (the owner's rule: P01 stores HALL,
// RVB and DLY by number); a filter's MODE (the list is the TYPE's); the oscillator waves; the
// wavetable's TABL; FM's ALGO; the macro's MODL; a kit pad's SND / MODL / KIND (the list is the
// pad TYPE's); the saturation curves; the XIN page's FLT (four, tied to the four sources); the
// samples (the sampler's and the wavetable's by name already; FM's SMPL and the resonator's are
// places in the bank and are NOT yet by name: left, see the "Format V3" inventory).
// "Format V3" walks every page of every engine and fails for a Choice cell that shows one of the
// lists named here without being classified, and for a long list that is in neither group.
//
// Nothing here runs on the audio thread.
// -----------------------------------------------------------------------------
namespace stored
{
    // ---- Today's lists ---------------------------------------------------------------------------
    struct List
    {
        const char* const* labels = nullptr;
        int count = 0;
        int off = -1;                               // where the list says OFF; -1: it has no such entry
        history::Family family = history::NumFamilies;  // its history (StoredListHistory.h); NumFamilies: it only ever grew at its end
        const char* what = "";                      // for the log
        // (the pressure slots: beyond the table itself, the DEST cell's own list, as AT ROUTE shows it)
        const char* const* more = nullptr;
        int moreCount = 0;

        explicit operator bool() const { return labels != nullptr && count > 0; }

        const char* at(int index) const
        {
            if (index >= 0 && index < count)            return labels[index];
            if (index >= count && index < moreCount)    return more[index];
            return nullptr;
        }
        int indexOf(const char* name) const
        {
            if (name == nullptr || name[0] == '\0')
                return -1;
            for (int i = 0; i < count; ++i)
                if (std::strcmp(labels[i], name) == 0)
                    return i;
            for (int i = count; i < moreCount; ++i)
                if (std::strcmp(more[i], name) == 0)
                    return i;
            return -1;
        }
    };

    struct Tables
    {
        const mod::DestTable* dests = nullptr;
        const mod::SourceTable* sources = nullptr;
    };

    // An engine type's destination table and source list: static storage, built at first use.
    // (The macro oscillator has a table a model: they differ only in the names of the four macros,
    // and a name of one model is another macro's in the next - FDBK is the third macro of one and
    // the fourth of another. So a macro has ONE name in a file and in a preset written in code:
    // the first model's, SAWS SPRD HARM MORP, whatever model the sound plays.)
    inline Tables tablesOf(int engineType)
    {
        switch ((EngineType) engineType)
        {
            case EngineType::UnifiedVoice:  return { &UnifiedVoiceEngine::routerDests(),
                                                     &mod::SourceTable::standard(ModParam::MAX_LFOS, ModParam::MAX_ENVS) };
            case EngineType::FM:            return { &FMEngine::lfoDests(), &FMEngine::sources() };
            case EngineType::MacroOsc:      return { &MacroOscEngine::lfoDests(0), &MacroOscEngine::sources() };
            case EngineType::Wavetable:     return { &WavetableEngine::lfoDests(), &WavetableEngine::sources() };
            case EngineType::WaveSeq:       return { &WaveSeqEngine::lfoDests(), &WaveSeqEngine::sources() };
            case EngineType::Sampler:       return { &SamplerEngine::lfoDests(), &SamplerEngine::sources() };
            case EngineType::Drum:          return { &DrumEngine::lfoDests(), &mod::SourceTable::standard(2) };
            case EngineType::Resonator:     return { &ResonatorEngine::lfoDests(), &mod::SourceTable::standard(2) };
            case EngineType::MIDITrack:     return {};
            default:                        return {};
        }
    }

    // The drum kit's types by name, in the order of DrumSynthType (the number a pad's TYPE cell
    // stores: the first DrumVoice::NUM_OFFERED are what the cell offers, the rest are still read).
    static constexpr int NUM_DRUM_TYPES = (int) DrumSynthType::OpenHat + 1;
    inline const char* const* drumTypeLabels()
    {
        static const char* const names[NUM_DRUM_TYPES] = {
            "FMKK", "KICK", "MKIK", "CLAP", "SHKR", "SUB", "KSYN", "CYMB", "RSN", "BANK",
            "TOM", "SNAR", "FMSN", "CSYN", "RIM", "CLAV", "HAT", "OHAT" };
        static_assert((int) DrumSynthType::FMKick == 0 && (int) DrumSynthType::Clap == 3 && (int) DrumSynthType::KickSynth == 6
                      && (int) DrumSynthType::Resonator == 8 && (int) DrumSynthType::OscBank == 9 && (int) DrumSynthType::Tom == 10
                      && (int) DrumSynthType::NoiseSnare == 11 && (int) DrumSynthType::CymbalSynth == 13 && (int) DrumSynthType::Rim == 14
                      && (int) DrumSynthType::ClosedHat == 16 && (int) DrumSynthType::OpenHat == 17,
                      "the names above are DrumSynthType's, in its order: a new type gets its name at the end of both");
        return names;
    }

    inline bool isDrumTypeCell(int id)
    {
        return id >= 0 && id < DrumEngine::NUM_VOICES * DrumEngine::PARAMS_PER_VOICE && id % DrumEngine::PARAMS_PER_VOICE == DrumEngine::TYPE;
    }

    inline List routerList(mod::NamedList::Kind kind, const Tables& t)
    {
        using K = mod::NamedList::Kind;
        if (kind == K::None || t.dests == nullptr || t.sources == nullptr)
            return {};
        const auto l = mod::NamedList::listOf(kind, *t.dests, *t.sources);
        List list;
        list.labels = l.labels;
        list.count = l.count;
        switch (kind)
        {
            case K::CellDest:       list.off = 0; list.family = history::Cell;   list.what = "DEST"; break;
            case K::SlotDest:       list.off = 0; list.family = history::Slot;   list.what = "slot DEST"; break;
            case K::Source:         list.off = 0; list.family = history::Source; list.what = "SRC"; break;
            // (The pressure slots' own list, by value: the table with its hidden entries, then VIB, then
            // the track's effect cells - ModRouter.h, THE PRESSURE SLOTS. Nothing lies beyond it.)
            case K::PressureDest:   list.off = 0; list.what = "pressure DEST"; break;
            case K::Shape:          list.what = "LFO shape"; break;
            case K::Trigger:        list.what = "LFO trigger"; break;
            case K::Division:       list.what = "LFO division"; break;
            case K::None:           break;
            default:                break;
        }
        return list;
    }

    inline List filterTypeList()
    {
        List list;
        list.labels = FilterSlot::modelNames();
        list.count = FilterSlot::NUM_MODELS;
        list.what = "filter TYPE";
        return list;
    }

    // The list a stored cell is a position in, today. Not valid (operator bool): the value is no
    // position (or is one of the lists that stay numbers: the header says which).
    inline List listOf(int engineType, int id)
    {
        using K = mod::NamedList::Kind;
        if (RangeParam::isRangeParam(id))
            id = RangeParam::targetOf(id);
        const auto type = (EngineType) engineType;
        const Tables t = tablesOf(engineType);
        if (t.dests == nullptr)
            return {};                              // (a MIDI track, or no engine type at all)

        if (type == EngineType::UnifiedVoice)
        {
            // Its own LFO pages (MOD, MOD2), built by the screen at ids of its own.
            if (id >= ModParam::LFO_BASE && id < ModParam::LFO_BASE + ModParam::MAX_LFOS * ModParam::LFO_STRIDE)
            {
                List list;
                switch ((id - ModParam::LFO_BASE) % ModParam::LFO_STRIDE)
                {
                    case ModParam::LFO_DEST:
                        list.labels = UnifiedVoice::lfoDestCellLabels();
                        list.count = UnifiedVoice::NUM_LFO_CELL_DESTS;
                        list.off = 0;
                        list.what = "LFO DEST";
                        return list;
                    case ModParam::LFO_SHAPE:   return routerList(K::Shape, t);
                    case ModParam::LFO_TRIG:
                        list.labels = LFO::triggerLabels();         // (its LFO pages offer the five of dsp/LFO.h)
                        list.count = (int) LFO::TriggerMode::COUNT;
                        list.what = "LFO trigger";
                        return list;
                    default:                    return {};
                }
            }
            if (id >= ModParam::LFO_EXT_BASE && id < ModParam::LFO_EXT_END)
                return (id - ModParam::LFO_EXT_BASE) % ModParam::LFO_EXT_STRIDE == ModParam::LFO_DIV ? routerList(K::Division, t) : List();
            if (id >= ModParam::ENV_BASE && id < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
            {
                if ((id - ModParam::ENV_BASE) % ModParam::ENV_STRIDE != ModParam::ENV_TARGET)
                    return {};
                List list;
                list.labels = UnifiedVoice::envDestLabels();
                list.count = UnifiedVoice::NUM_ENV_DESTS;
                list.off = 3;                               // AMP CUT PTCH OFF, then the LFOs' list
                jassert(std::strcmp(list.labels[3], "OFF") == 0);
                list.what = "envelope DEST";
                return list;
            }
            if (id == UnifiedParam::MODEL_A || id == UnifiedParam::MODEL_B)
                return filterTypeList();
            // The router's STRIP and XIN pages at 158-173; 174-189 are the LFOs' WHL / VEL / KTRK / PRES.
            if (id >= mod::Router::LFO_BASE && id < mod::Router::XIN_END)
            {
                const int routerId = UnifiedVoiceEngine::routerIdOf(id);
                return routerId >= 0 ? routerList(mod::NamedList::classify(routerId), t) : List();
            }
            return routerList(mod::NamedList::classify(id), t);
        }

        if (auto list = routerList(mod::NamedList::classify(id), t))
            return list;

        const int filterType = (int) VoiceFilterSection::Type;
        switch (type)
        {
            case EngineType::FM:        if (id == (int) FMEngine::FilterBase + filterType) return filterTypeList(); break;
            case EngineType::MacroOsc:  if (id == (int) MacroOscEngine::FilterBase + filterType) return filterTypeList(); break;
            case EngineType::WaveSeq:   if (id == (int) WaveSeqEngine::FilterBase + filterType) return filterTypeList(); break;
            case EngineType::Resonator: if (id == (int) ResonatorEngine::FilterBase + filterType) return filterTypeList(); break;
            case EngineType::Sampler:   if (id == (int) SamplerEngine::FilterType) return filterTypeList(); break;
            case EngineType::Wavetable:
                if (id == (int) WavetableEngine::FilterBase + filterType || id == (int) WavetableEngine::FilterBase + (int) VoiceFilterSection::TypeB)
                    return filterTypeList();
                break;
            case EngineType::Drum:
                if (isDrumTypeCell(id))
                {
                    List list;
                    list.labels = drumTypeLabels();
                    list.count = NUM_DRUM_TYPES;
                    list.family = history::Type;
                    list.what = "drum TYPE";
                    return list;
                }
                if (PadModParam::contains(id))
                {
                    // A pad's own LFO and two slots: the kit's lists (DrumEngine::padLfoPage / padSlotPage).
                    // They came with format 3: no history.
                    List list;
                    switch (PadModParam::cellOf(id))
                    {
                        case PadModParam::LFO_SHAPE:    return routerList(K::Shape, t);
                        case PadModParam::LFO_TRIGGER:  return routerList(K::Trigger, t);
                        case PadModParam::LFO_DIV:      return routerList(K::Division, t);
                        case PadModParam::LFO_DEST:
                        case PadModParam::SLOT1_DEST:
                        case PadModParam::SLOT2_DEST:
                            // (What a pad's DEST shows: the kit's table at its own places, without what
                            // a pad's own modulation cannot move and without the track's effect cells.)
                            list = routerList(K::CellDest, t);
                            list.labels = DrumEngine::padDestLabels();
                            list.count = DrumEngine::padDestCount();
                            list.what = "pad DEST";
                            break;
                        case PadModParam::SLOT1_SRC:
                        case PadModParam::SLOT1_VIA:
                        case PadModParam::SLOT2_SRC:
                        case PadModParam::SLOT2_VIA:    list = routerList(K::Source, t); list.what = "pad SRC"; break;
                        default:                        return {};
                    }
                    list.family = history::NumFamilies;
                    return list;
                }
                break;
            case EngineType::UnifiedVoice:
            case EngineType::MIDITrack:
            default:
                break;
        }
        return {};
    }

    // The name a stored cell's value has today: what a file is written with. nullptr: the cell is
    // no position, or the value is no entry of its list.
    inline const char* nameOf(int engineType, int id, float value)
    {
        const List list = listOf(engineType, id);
        // (A value that is no whole number is no position: it is written as the number it is, and
        // comes back as that.)
        if (! list || ! std::isfinite(value) || value != std::round(value))
            return nullptr;
        return list.at((int) std::lround(juce::jlimit(-1.0f, 1.0e6f, value)));
    }

    // The value today of a cell by its name alone (-1: no such cell, or no such entry).
    inline int valueOf(int engineType, int id, const char* name)
    {
        const List list = listOf(engineType, id);
        return list ? list.indexOf(name) : -1;
    }

    inline const char* engineLabel(int engineType)
    {
        static const char* const names[ENGINE_TYPE_COUNT] = { "synth", "FM", "macro", "wavetable", "wave sequencer", "sampler",
                                                              "drum kit", "MIDI", "resonator" };
        return engineType >= 0 && engineType < ENGINE_TYPE_COUNT ? names[engineType] : "?";
    }

    // What a load could not place, one line a thing however often it occurs.
    inline void lose(juce::StringArray& lost, int engineType, const List& list, const juce::String& name, const juce::String& became)
    {
        lost.addIfNotAlreadyThere(juce::String(engineLabel(engineType)) + ": " + list.what + " \"" + name + "\" is not in the list any more: " + became);
    }

    // Said once a load, where the owner can read it (the app's log; the tests' output).
    inline void report(const juce::String& file, const juce::StringArray& lost)
    {
        for (auto& line : lost)
            juce::Logger::writeToLog("[Format] " + file + ": " + line);
    }

    // A cell as a format-3 file stores it - its number and, for a position, the entry's name - as
    // the value it has today.
    inline float loaded(int engineType, int id, float number, const juce::var& name, juce::StringArray& lost)
    {
        if (! name.isString())
            return number;
        const List list = listOf(engineType, id);
        if (! list)
            return number;                          // (a cell that is no position any more: its number)
        const juce::String text = name.toString();
        const int at = list.indexOf(text.toRawUTF8());
        if (at >= 0)
            return (float) at;
        if (list.off >= 0)
        {
            lose(lost, engineType, list, text, "OFF");
            return (float) list.off;
        }
        lose(lost, engineType, list, text, "the number the file gave is kept");
        return number;
    }

    // ---- Before format 3 -------------------------------------------------------------------------
    namespace old
    {
        // "8-9,66-69": is one of these ids in the sound?
        inline bool holds(const ParamMap& sound, const char* ids)
        {
            for (const char* p = ids; p != nullptr && *p != '\0';)
            {
                char* end = nullptr;
                const long first = std::strtol(p, &end, 10);
                long last = first;
                if (end == p)
                    break;
                if (*end == '-')
                {
                    const char* q = end + 1;
                    last = std::strtol(q, &end, 10);
                    if (end == q)
                        break;
                }
                const auto it = sound.lower_bound((int) first);
                if (it != sound.end() && it->first <= (int) last)
                    return true;
                p = *end == ',' ? end + 1 : end;
                if (*end != ',' )
                    break;
            }
            return false;
        }

        // The stage a sound was written at, as far as its ids tell (StoredListHistory.h): the newest
        // one whose ids it holds; 0 (the oldest: v0.9, for the engines the public release had)
        // when it holds none.
        inline int stageOf(int engineType, const ParamMap& sound)
        {
            int count = 0;
            const auto* stages = history::stagesOf(engineType, count);
            int stage = 0;
            for (int s = 1; s < count; ++s)
                if (holds(sound, stages[s].evidence))
                    stage = s;
            return stage;
        }
        inline int stageCount(int engineType)
        {
            int count = 0;
            history::stagesOf(engineType, count);
            return count;
        }

        // A list as StoredListHistory.h writes it ("A|B|C"): how many entries, and the n-th.
        inline int entries(const char* list)
        {
            if (list == nullptr || list[0] == '\0')
                return 0;
            int n = 1;
            for (const char* p = list; *p != '\0'; ++p)
                n += *p == '|' ? 1 : 0;
            return n;
        }
        inline juce::String entry(const char* list, int index)
        {
            const char* p = list;
            for (int i = 0; i < index && p != nullptr; ++i)
            {
                p = std::strchr(p, '|');
                p = p != nullptr ? p + 1 : nullptr;
            }
            if (p == nullptr)
                return {};
            const char* end = std::strchr(p, '|');
            return end != nullptr ? juce::String(p, (size_t) (end - p)) : juce::String(p);
        }
        // How many entries a family's list had ("=N": today's first N).
        inline int length(const char* list) { return list == nullptr ? 0 : (list[0] == '=' ? std::atoi(list + 1) : entries(list)); }

        // The list of a family as it was at a stage: the newest one written at or before it.
        // nullptr: the engine had no such list yet.
        inline const char* listAt(int engineType, history::Family family, int stage)
        {
            int count = 0;
            const auto* stages = history::stagesOf(engineType, count);
            for (int s = juce::jmin(stage, count - 1); s >= 0; --s)
                if (stages[s].lists[family] != nullptr)
                    return stages[s].lists[family];
            return nullptr;
        }

        // One position of an old file as today's. `stage`: what the sound's ids told. A number the
        // list of that stage has no entry for was written later, whatever the ids say: the first
        // later list that holds it is taken.
        inline float cell(int engineType, int stage, int id, float number, juce::StringArray& lost)
        {
            const List list = listOf(engineType, id);
            if (! list || list.family == history::NumFamilies || ! std::isfinite(number) || number != std::round(number))
                return number;
            const int index = (int) std::lround(juce::jlimit(0.0f, 1.0e6f, number));
            const int count = stageCount(engineType);
            const char* was = listAt(engineType, list.family, stage);
            for (int s = stage + 1; s < count && (was == nullptr || index >= length(was)); ++s)
                if (const char* later = listAt(engineType, list.family, s); later != nullptr && index < length(later))
                    was = later;
            if (was == nullptr || was[0] == '=' || index >= length(was))
                return number;                      // (today's own entry, or no entry of any list: as it stands)
            const juce::String name = entry(was, index);
            const int at = list.indexOf(name.toRawUTF8());
            if (at >= 0)
                return (float) at;
            if (list.off >= 0)
            {
                lose(lost, engineType, list, name, "OFF");
                return (float) list.off;
            }
            lose(lost, engineType, list, name, "the number the file gave is kept");
            return number;
        }

        // ---- The drum kit of before U2b (871c986) ------------------------------------------------
        // What such a kit stored and what it is today:
        //   TYPE          FMKK KICK MKIK SNAR FMSN CLAP HAT OHAT CYMB TOM RIM SHKR CLAV SUB KSYN (CSYN),
        //                 by its place in THAT list. Since then the enum has another order, and
        //                 SNAR FMSN TOM RIM CLAV are sounds of the resonator drum, HAT OHAT of the
        //                 oscillator bank, FMKK KICK MKIK points of the kick machine. A pad becomes
        //                 what a factory kit's pad of that type became (CymbalPresets::drum: the
        //                 conversions U2b, the kicks and the fold measured and the owner accepted),
        //                 from the pad's own eight cells.
        //   528 + pad*8   the cymbal machine's page (MODL PING COLR NOIS DAMP BLOM VSNS LOFI), stored
        //                 for every pad whatever its type. Those ids are a pad's SECOND page now (the
        //                 resonator drum's sound, the bank's cells, the kick machine's KCK2): read as
        //                 they stand they would give every such pad the sound number 2 and half-open
        //                 cells. They are taken out; a CSYN pad becomes the cymbal its MODL named
        //                 (METL RING on the bank with its PING COLR DAMP BLOM LOFI; RIDE BELL CRSH
        //                 CHNA SPLS GONG TAM as the resonator drum's sounds), and a kick machine's
        //                 second page is put at rest, which is the kick machine as it was.
        //   400 + pad*8   the kick machine's page: the same cells then and now (kept; on a pad that
        //                 was FMKK KICK or MKIK it held values that did nothing and is made the
        //                 kick's own).
        // A pad whose TYPE the file does not hold is left alone.
        inline bool isOldKit(int stage)
        {
            const char* types = listAt((int) EngineType::Drum, history::Type, stage);
            return types != nullptr && types[0] != '=';
        }

        inline DrumSynthType oldDrumType(int stage, int index)
        {
            const char* types = listAt((int) EngineType::Drum, history::Type, stage);
            const juce::String name = entry(types, juce::jlimit(0, juce::jmax(0, entries(types) - 1), index));
            for (int i = 0; i < NUM_DRUM_TYPES; ++i)
                if (name == drumTypeLabels()[i])
                    return (DrumSynthType) i;
            jassertfalse;
            return DrumSynthType::FMKick;
        }

        inline void kit(int stage, ParamMap& sound)
        {
            static constexpr int PAD = DrumEngine::PARAMS_PER_VOICE, PADS = DrumEngine::NUM_VOICES;
            static constexpr int PAGE = CymbalPresets::SOUND_BASE, CELLS = CymbalPresets::PAD_PARAMS;
            enum { Model = 0, Ping, Colour, Noise, Damp, Bloom, Sense, Lofi };       // the cymbal machine's page

            // The cymbal pages, out of the sound and kept for the pads that were cymbals.
            std::array<std::array<float, CELLS>, PADS> cymbal {};
            for (int pad = 0; pad < PADS; ++pad)
            {
                cymbal[(size_t) pad] = { { 2.0f, 0.5f, 0.5f, 0.5f, 0.4f, 0.3f, 0.5f, 0.0f } };      // (its defaults: RIDE)
                for (int n = 0; n < CELLS; ++n)
                    if (auto it = sound.find(PAGE + pad * CELLS + n); it != sound.end())
                    {
                        if (std::isfinite(it->second))
                            cymbal[(size_t) pad][(size_t) n] = it->second;
                        sound.erase(it);
                    }
            }

            for (int pad = 0; pad < PADS; ++pad)
            {
                const auto typeCell = sound.find(pad * PAD + DrumEngine::TYPE);
                if (typeCell == sound.end() || ! std::isfinite(typeCell->second))
                    continue;
                const auto type = oldDrumType(stage, (int) std::lround(juce::jlimit(0.0f, 1000.0f, typeCell->second)));
                typeCell->second = (float) (int) type;          // (at the least: today's number of the same type)

                float c[PAD] {};
                bool whole = true;
                for (int n = 0; n < PAD - 1; ++n)
                {
                    const auto it = sound.find(pad * PAD + n);
                    whole = whole && it != sound.end() && std::isfinite(it->second);
                    if (whole)
                        c[n] = it->second;
                }
                if (! whole)
                    continue;       // (not a whole pad: the engine makes of the type what it makes of one it is given)
                const float tune = c[DrumEngine::TUNE], decay = c[DrumEngine::DECAY], tone = c[DrumEngine::TONE], drive = c[DrumEngine::DRIVE],
                            snap = c[DrumEngine::SNAP], level = c[DrumEngine::LEVEL], pan = c[DrumEngine::PAN];

                if (type == DrumSynthType::CymbalSynth)
                {
                    static const char* const sounds[] = { nullptr, nullptr, "RIDE", "CUP", "CRSH", "CHNA", "SPLS", "GONG", "TAM" };
                    const auto& page = cymbal[(size_t) pad];
                    const int model = juce::jlimit(0, 8, (int) std::lround(page[Model]));
                    if (sounds[model] == nullptr)
                        CymbalPresets::bank(sound, pad, model == 0 ? OscBank::Metal : OscBank::Ring, tune, decay, tone, drive, snap, level, pan,
                                            page[Ping], page[Colour], page[Damp], page[Bloom], 0.0f, page[Lofi]);
                    else
                    {
                        CymbalPresets::pad(sound, pad, tune, decay, tone, drive, snap, level, pan, CymbalPresets::RSN_TYPE);
                        sound[PAGE + pad * CELLS] = (float) resonatorSoundIndex(sounds[model]);
                        for (int n = 1; n < CELLS; ++n)
                            sound[PAGE + pad * CELLS + n] = 0.0f;
                    }
                    continue;
                }
                CymbalPresets::drum(sound, pad, tune, decay, tone, drive, snap, level, pan, type);
                if (type == DrumSynthType::KickSynth)
                    for (int n = 0; n < CELLS; ++n)
                        sound[PAGE + pad * CELLS + n] = 0.0f;       // KCK2 at rest: the kick machine as it was
            }
        }

        // ---- Ids that moved --------------------------------------------------------------------
        // The wave sequencer's BODY page was at 182-189 from 340e9dc to b7e8463 (ten hours of
        // 2026-11-21); the XIN page is there since. A file with those ids and without BODY's own
        // (133-140) is of those hours.
        inline bool waveSeqBodyAtXin(int engineType, const ParamMap& sound)
        {
            return (EngineType) engineType == EngineType::WaveSeq && holds(sound, "182-189") && ! holds(sound, "133-140");
        }
        inline void moveWaveSeqBody(ParamMap& cells)
        {
            ParamMap moved;
            for (auto it = cells.lower_bound(182); it != cells.end() && it->first < 190;)
            {
                moved[it->first - 182 + 133] = it->second;
                it = cells.erase(it);
            }
            for (auto& [id, value] : moved)
                cells[id] = value;
        }

        // THRU (through zero) of the chorus, the ensemble and the vibrato took the stored place
        // of a cell they had before it: the chorus's and the ensemble's FDBK (-0.95..0.95, the
        // second cell of FX2), the vibrato's KEY (the fourth). A sound of before format 3 was
        // written before THRU was (2026-11-22, some hours ahead of format 3): what it holds
        // there is a feedback or a key, and read as THRU a feedback of 0.5 or more, or any key
        // but SELF, would switch THRU on. Such a sound has no THRU: OFF. (The numbers are
        // AudioEngine.h's EffectType and insert ids, which this file cannot include; the
        // "Through Zero" suite holds them to it.)
        static constexpr int INSERT_TYPE_BASE = 232, INSERT_EXTRA_BASE = 312, INSERT_SLOTS = 8, INSERT_EXTRAS = 4;
        static constexpr int FX_CHORUS = 0, FX_ENSEMBLE = 11, FX_VIBRATO = 22;
        inline void thruOff(ParamMap& cells)
        {
            for (int slot = 0; slot < INSERT_SLOTS; ++slot)
            {
                const auto type = cells.find(INSERT_TYPE_BASE + slot);
                if (type == cells.end())
                    continue;
                const int fx = (int) std::lround(type->second) - 1;        // (stored as the type + 1)
                const int cell = (fx == FX_CHORUS || fx == FX_ENSEMBLE) ? 1 : (fx == FX_VIBRATO ? 3 : -1);
                if (cell >= 0)
                    if (auto it = cells.find(INSERT_EXTRA_BASE + slot * INSERT_EXTRAS + cell); it != cells.end())
                        it->second = 0.0f;
            }
        }

        // What old::sound() found, for the same sound's step locks.
        struct Layout
        {
            int stage = 0;
            bool oldKit = false, bodyAtXin = false;
        };

        // A whole sound of before format 3, made today's in place.
        inline Layout sound(int engineType, ParamMap& cells, juce::StringArray& lost)
        {
            Layout layout;
            thruOff(cells);         // (the track's inserts: whatever its engine)
            if (engineType < 0 || engineType >= ENGINE_TYPE_COUNT || stageCount(engineType) == 0)
                return layout;
            layout.stage = stageOf(engineType, cells);
            layout.bodyAtXin = waveSeqBodyAtXin(engineType, cells);
            if (layout.bodyAtXin)
                moveWaveSeqBody(cells);
            if ((EngineType) engineType == EngineType::Drum && isOldKit(layout.stage))
            {
                // (A TYPE past the old list's end was not written with it.)
                bool fits = true;
                for (int pad = 0; pad < DrumEngine::NUM_VOICES; ++pad)
                    if (auto it = cells.find(pad * DrumEngine::PARAMS_PER_VOICE + DrumEngine::TYPE); it != cells.end())
                        fits = fits && it->second < (float) entries(listAt(engineType, history::Type, layout.stage)) - 0.5f;
                layout.oldKit = fits;
                if (layout.oldKit)
                    kit(layout.stage, cells);
            }
            for (auto& [id, value] : cells)
            {
                if (layout.oldKit && isDrumTypeCell(id))
                    continue;                       // (kit() has done them)
                value = cell(engineType, layout.stage, id, value, lost);
            }
            return layout;
        }

        // The locks of one step of a track whose sound was `layout`.
        inline void locks(int engineType, const Layout& layout, ParamMap& cells, juce::StringArray& lost)
        {
            if (cells.empty() || engineType < 0 || engineType >= ENGINE_TYPE_COUNT || stageCount(engineType) == 0)
                return;
            if (layout.bodyAtXin)
                moveWaveSeqBody(cells);
            if (layout.oldKit)      // (the cymbal page's cells are another page now: such a lock has nothing to act on)
                for (auto it = cells.lower_bound(CymbalPresets::SOUND_BASE); it != cells.end() && it->first < DrumEngine::SOUND_BASE + DrumEngine::NUM_VOICES * DrumEngine::SOUND_CELLS;)
                    it = cells.erase(it);
            for (auto& [id, value] : cells)
                value = cell(engineType, layout.stage, id, value, lost);
        }
    }

    // (As one number, for Preset.h, which cannot include this file: bit 0 an old kit, bit 1 the wave
    // sequencer's BODY at the XIN ids, the stage from bit 4.)
    inline int soundOfBefore(int engineType, ParamMap& sound, juce::StringArray& lost)
    {
        const auto layout = old::sound(engineType, sound, lost);
        return (layout.oldKit ? 1 : 0) | (layout.bodyAtXin ? 2 : 0) | (layout.stage << 4);
    }
    inline void locksOfBefore(int engineType, int layout, ParamMap& locks, juce::StringArray& lost)
    {
        old::Layout l;
        l.oldKit = (layout & 1) != 0;
        l.bodyAtXin = (layout & 2) != 0;
        l.stage = layout >> 4;
        old::locks(engineType, l, locks, lost);
    }
}
