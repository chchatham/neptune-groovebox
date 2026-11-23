#pragma once

#include "ParamSpec.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// -----------------------------------------------------------------------------
// Mod slots: open modulation on the sound engines.
//
// A slot is SRC (what moves), DEST (what is moved), AMT (-1..1, how far) and VIA (a second
// source that scales the amount). Ids: ModSlotParam (700 + slot * 4 + n), the least number of
// slots shown ModSlotParam::COUNT (772), the custom LFO shape LfoShapeParam (764-771).
//
//   SLT1   SRC DEST AMT VIA | SRC DEST AMT VIA     slots 1 and 2; SLT2 = slots 3 and 4, ...
//   LSHP   L1 .. L8                                the eight levels of the USR / USRS LFO shapes
//
// The pages follow use: every page that holds a slot with a cell off its default is shown, and one
// more after the last of them (pagesShown), so there is always a free pair and never a page to
// count. Every slot therefore has its cells on the glass: nothing acts unseen. Id 772 is the least
// number of slots to show (default 2 = one page); it has no cell (every page is eight working
// cells and none has room): a preset or a project may set it to keep more empty pages open.
//
// Sources (ModSrc; stored by index, the list only grows at the end):
//   OFF
//   LFO1 LFO2   the LFO's shape with its fade-in, WITHOUT its DPTH (the slot's AMT is the depth),
//               whether or not the LFO has a destination of its own
//   ENV FENV    the amp envelope and the filter's, 0..1
//   VEL         the note's velocity, 0..1
//   PRES        this voice's own pad pressure, 0..1
//   STRP        the mod strip, 0..1
//   KEY         the note round middle C: (note - 60) / 60, -1..1
//   RAND        one value a note, -1..1
//   IN          the envelope follower of the track's input, 0..1 (the same for every voice)
//   LFO3 LFO4   the unified voice only
// VIA uses the same list: OFF = the amount as it is; otherwise the amount is multiplied by the VIA
// source taken as 0..1 (a source that swings both ways is moved up: (v + 1) / 2).
//
// What a slot gives its destination is  source x AMT x via  (-1..1): one more source beside the
// LFOs and the strip's routes, summed with them and read by the voice through the same calls, so
// it moves the destination as far as an LFO at that depth would. "The AMT of slot k" as a
// destination adds to that slot's AMT (the sum held to -1..1), one sample late; a slot aimed at
// its own AMT does nothing.
//
// Nothing is heard as a jump: a slot's output reaches every destination but pitch on a lead that
// moves no faster than the whole swing in 4 ms (an envelope's attack through a slot is therefore
// never shorter than 2 ms), the sources that arrive once a block (PRES, STRP, IN) are moved to
// over 5 ms, and a slot taken off its destination under a sounding note lets go of it first.
// A note-on takes everything as it stands.
//
// This file: the sources, one voice's slots (VoiceModSlots: what each gives right now) and the
// engine's stored values and pages (EngineModSlots). What a destination index means belongs to
// the engine: EngineLfoSection for the six engines that share it, UnifiedVoiceEngine for its own.
//
// Nothing here allocates or locks on the audio thread (the label lists are made when pages are).
// -----------------------------------------------------------------------------

namespace ModSrc
{
    enum
    {
        Off = 0, Lfo1, Lfo2, Env, FilterEnv, Velocity, Pressure, Strip, Key, Random, Input,
        Lfo3, Lfo4,
        COUNT
    };
    static constexpr int BASIC_COUNT = Lfo3;        // what an engine with two LFOs offers

    inline const char* const* labels()
    {
        static const char* const names[COUNT] = {
            "OFF", "LFO1", "LFO2", "ENV", "FENV", "VEL", "PRES", "STRP", "KEY", "RAND", "IN", "LFO3", "LFO4" };
        return names;
    }

    // 0..3 for the LFO sources, -1 for the others.
    static constexpr int lfoIndex(int source)
    {
        return source == Lfo1 ? 0 : (source == Lfo2 ? 1 : (source == Lfo3 ? 2 : (source == Lfo4 ? 3 : -1)));
    }
    static constexpr int ofLfo(int lfo) { return lfo == 0 ? Lfo1 : (lfo == 1 ? Lfo2 : (lfo == 2 ? Lfo3 : Lfo4)); }
}

// What a slot's DEST is, as the engine resolved it.
struct ModSlotTarget
{
    enum Kind
    {
        None = 0,
        Fixed,          // index = a destination of the engine's LFO list that its voice reads by index
        Keyed,          // index = the key of a page cell; lo / hi / span / log / base as for an LFO
        LfoRate,        // index = the LFO (-1: every LFO), +-2 octaves
        LfoDepth,       // index = the LFO (-1: every LFO), x 0..2
        SlotAmount      // index = the slot whose AMT is moved
    };
    int kind = None;
    int index = 0;
    float lo = 0.0f, hi = 1.0f, span = 1.0f, base = 0.0f;
    bool log = false;

    bool sameAs(const ModSlotTarget& o) const
    {
        return kind == o.kind && index == o.index && lo == o.lo && hi == o.hi && span == o.span
            && base == o.base && log == o.log;
    }
};

// One voice's slots: the sources as the voice has them and what each slot gives right now.
class VoiceModSlots
{
public:
    static constexpr int MAX = ModSlotParam::MAX_SLOTS;

    struct Settings
    {
        int src = ModSrc::Off;
        int via = ModSrc::Off;
        float amount = 0.0f;            // -1..1
        bool modulated = false;         // another slot moves this one's amount
        ModSlotTarget target;

        bool on() const
        {
            return src != ModSrc::Off && target.kind != ModSlotTarget::None && (amount != 0.0f || modulated);
        }
        bool sameAs(const Settings& o) const
        {
            return src == o.src && via == o.via && amount == o.amount && modulated == o.modulated && target.sameAs(o.target);
        }
    };

    void setSampleRate(double sr)
    {
        const double rate = sr > 0.0 ? sr : 44100.0;
        step = (float) (2.0 / (0.004 * rate));
        blockStep = (float) (1.0 / (0.005 * rate));
    }

    // A slot's settings. One that is taken off its destination (or switched off) under a sounding
    // voice lets go first: its output glides to zero, and only then do the new settings apply.
    void apply(int i, const Settings& s, bool sounding)
    {
        if (i < 0 || i >= MAX)
            return;
        const auto n = (size_t) i;
        const bool stays = s.on() && s.target.kind == set[n].target.kind && s.target.index == set[n].target.index;
        if (isOn[n] && ! stays && sounding && heard[n] != 0.0f)
        {
            pending[n] = s;
            closing[n] = true;
            anyClosing = true;
            return;
        }
        closing[n] = false;
        applyNow(i, s);
    }

    bool isClosing() const { return anyClosing; }

    // The slots that have let go take their new settings; now: all that were letting go.
    void finishClosing(bool now)
    {
        bool left = false;
        for (int i = 0; i < MAX; ++i)
        {
            const auto n = (size_t) i;
            if (! closing[n])
                continue;
            if (now || heard[n] == 0.0f)
            {
                closing[n] = false;
                heard[n] = 0.0f;
                applyNow(i, pending[n]);
            }
            else
                left = true;
        }
        anyClosing = left;
    }

    // ---- The sources ---------------------------------------------------------------------------
    // A source is fed only while a slot uses it (uses()); the voice need not ask for the cheap ones.
    bool uses(int source) const { return (usedMask & (1u << source)) != 0; }
    bool usesEnvelopes() const { return (usedMask & ((1u << ModSrc::Env) | (1u << ModSrc::FilterEnv))) != 0; }

    // The note that is about to start (before noteOn).
    void setNote(int note, float velocity)
    {
        value[ModSrc::Velocity] = std::clamp(velocity, 0.0f, 1.0f);
        value[ModSrc::Key] = std::clamp((float) (note - 60) / 60.0f, -1.0f, 1.0f);
        pendingNote = note;
    }

    // Note-on: a new random value (every voice draws its own series, begun from its own seed, and
    // the note stirs it).
    void noteOn()
    {
        randomState = randomState * 1664525u + 1013904223u + (uint32_t) pendingNote * 2654435761u;
        value[ModSrc::Random] = (float) ((randomState >> 8) & 0xffffu) / 32767.5f - 1.0f;
    }

    void setLfo(int lfo, float shapeOutput, bool unipolarShape)
    {
        const int s = ModSrc::ofLfo(lfo);
        value[(size_t) s] = shapeOutput;
        lfoUnipolar[(size_t) std::clamp(lfo, 0, 3)] = unipolarShape;
    }
    void setEnvelopes(float amp, float filter)
    {
        value[ModSrc::Env] = amp;
        value[ModSrc::FilterEnv] = filter;
    }
    // Once a block: this voice's pad pressure, the mod strip and the input's follower (each 0..1).
    void setBlockSources(float pressure, float strip, float input)
    {
        blockTarget[0] = std::clamp(pressure, 0.0f, 1.0f);
        blockTarget[1] = std::clamp(strip, 0.0f, 1.0f);
        blockTarget[2] = std::isfinite(input) ? std::clamp(input, 0.0f, 1.0f) : 0.0f;
    }

    // ---- One sample ----------------------------------------------------------------------------
    // fresh: the first sample of a note (everything is taken as it stands).
    void run(bool fresh)
    {
        // (A slot that has come to zero was heard at zero for one sample before it goes: whoever
        // moves a cell by it has put the cell back exactly.)
        if (anyClosing)
            finishClosing(false);
        static constexpr int blockSource[3] = { ModSrc::Pressure, ModSrc::Strip, ModSrc::Input };
        for (int b = 0; b < 3; ++b)
        {
            float& v = value[(size_t) blockSource[b]];
            const float d = blockTarget[(size_t) b] - v;
            if (d == 0.0f)
                continue;
            v = (fresh || std::abs(d) <= blockStep) ? blockTarget[(size_t) b] : v + (d > 0.0f ? blockStep : -blockStep);
        }

        for (int a = 0; a < numActive; ++a)
        {
            const auto n = (size_t) active[(size_t) a];
            const Settings& s = set[n];
            float amount = s.amount;
            if (s.modulated)
                amount = std::clamp(amount + amountMod[n], -1.0f, 1.0f);
            float v = 0.0f;
            if (! closing[n])
            {
                v = value[(size_t) s.src] * amount;
                if (s.via != ModSrc::Off)
                    v *= unipolar(s.via);
            }
            absAmount[n] = std::abs(amount);
            out[n] = v;
            const float d = v - heard[n];
            heard[n] = (fresh || std::abs(d) <= step) ? v : heard[n] + (d > 0.0f ? step : -step);
        }

        if (anyAmount)
        {
            amountMod.fill(0.0f);
            for (int a = 0; a < numActive; ++a)
            {
                const auto n = (size_t) active[(size_t) a];
                if (set[n].target.kind == ModSlotTarget::SlotAmount)
                    amountMod[(size_t) std::clamp(set[n].target.index, 0, MAX - 1)] += heard[n];
            }
        }
    }

    // ---- What the slots give -------------------------------------------------------------------
    bool live() const { return numActive > 0; }
    bool keyed() const { return anyKeyed; }
    bool crossing() const { return anyCross; }
    // True once after the set of slots in use changed (the host's own flags follow).
    bool takeChanged() { const bool c = changed; changed = false; return c; }

    int count() const { return numActive; }
    int slotAt(int a) const { return active[(size_t) a]; }
    const ModSlotTarget& target(int slot) const { return set[(size_t) slot].target; }
    float heardOf(int slot) const { return heard[(size_t) slot]; }      // on its lead: levels, cutoff, cells
    float outputOf(int slot) const { return out[(size_t) slot]; }       // as it is: pitch, LFO rate and depth
    float absAmountOf(int slot) const { return absAmount[(size_t) slot]; }
    // The key slot `slot` is moving right now (-1: none).
    int keyOf(int slot) const
    {
        const auto n = (size_t) std::clamp(slot, 0, MAX - 1);
        return isOn[n] && set[n].target.kind == ModSlotTarget::Keyed ? set[n].target.index : -1;
    }

    // Sum of the slots on fixed destination d (each -1..1).
    float fixedAmount(int d) const
    {
        float a = 0.0f;
        for (int i = 0; i < numActive; ++i)
        {
            const auto n = (size_t) active[(size_t) i];
            if (set[n].target.kind == ModSlotTarget::Fixed && set[n].target.index == d)
                a += heard[n];
        }
        return a;
    }
    // The same for a pitch destination, with the amount squared (as an LFO's depth is on pitch).
    float fixedPitch(int d) const
    {
        float a = 0.0f;
        for (int i = 0; i < numActive; ++i)
        {
            const auto n = (size_t) active[(size_t) i];
            if (set[n].target.kind == ModSlotTarget::Fixed && set[n].target.index == d)
                a += out[n] * absAmount[n];
        }
        return a;
    }
    bool fixedRouted(int d) const
    {
        for (int i = 0; i < numActive; ++i)
        {
            const auto n = (size_t) active[(size_t) i];
            if (set[n].target.kind == ModSlotTarget::Fixed && set[n].target.index == d)
                return true;
        }
        return false;
    }
    bool hasKey(int k) const
    {
        for (int i = 0; i < numActive; ++i)
        {
            const auto n = (size_t) active[(size_t) i];
            if (set[n].target.kind == ModSlotTarget::Keyed && set[n].target.index == k)
                return true;
        }
        return false;
    }
    // Parameter k under the slots routed to it.
    float keyedValue(int k, float v) const
    {
        for (int i = 0; i < numActive; ++i)
        {
            const auto n = (size_t) active[(size_t) i];
            const ModSlotTarget& t = set[n].target;
            if (t.kind != ModSlotTarget::Keyed || t.index != k)
                continue;
            const float o = heard[n] * t.span;
            v = std::clamp(t.log ? v * std::exp2(o) : v + o, t.lo, t.hi);
        }
        return v;
    }
    // The summed offset (in spans: -1..1 a slot) of the slots on key k, for a host that moves the
    // cell itself.
    float keyedAmount(int k) const
    {
        float a = 0.0f;
        for (int i = 0; i < numActive; ++i)
        {
            const auto n = (size_t) active[(size_t) i];
            if (set[n].target.kind == ModSlotTarget::Keyed && set[n].target.index == k)
                a += heard[n];
        }
        return a;
    }

    void reset()
    {
        finishClosing(true);
        out.fill(0.0f);
        heard.fill(0.0f);
        amountMod.fill(0.0f);
    }

private:
    float unipolar(int source) const
    {
        const float v = value[(size_t) source];
        const int lfo = ModSrc::lfoIndex(source);
        const bool bipolar = lfo >= 0 ? ! lfoUnipolar[(size_t) lfo] : (source == ModSrc::Key || source == ModSrc::Random);
        return bipolar ? 0.5f * (v + 1.0f) : v;
    }

    void applyNow(int i, const Settings& s)
    {
        const auto n = (size_t) i;
        set[n] = s;
        set[n].src = std::clamp(s.src, 0, (int) ModSrc::COUNT - 1);
        set[n].via = std::clamp(s.via, 0, (int) ModSrc::COUNT - 1);
        isOn[n] = set[n].on();
        if (! isOn[n])
        {
            out[n] = 0.0f;
            heard[n] = 0.0f;
            absAmount[n] = 0.0f;
        }
        numActive = 0;
        usedMask = 0;
        anyKeyed = anyCross = anyAmount = false;
        for (int k = 0; k < MAX; ++k)
        {
            const auto m = (size_t) k;
            if (! isOn[m])
                continue;
            active[(size_t) numActive++] = k;
            usedMask |= (1u << set[m].src) | (1u << set[m].via);
            const int kind = set[m].target.kind;
            anyKeyed = anyKeyed || kind == ModSlotTarget::Keyed;
            anyCross = anyCross || kind == ModSlotTarget::LfoRate || kind == ModSlotTarget::LfoDepth;
            anyAmount = anyAmount || kind == ModSlotTarget::SlotAmount;
        }
        usedMask &= ~1u;        // OFF is not a source
        if (! anyAmount)
            amountMod.fill(0.0f);
        changed = true;
    }

    std::array<Settings, MAX> set {}, pending {};
    std::array<bool, MAX> isOn {}, closing {};
    std::array<float, MAX> out {}, heard {}, absAmount {}, amountMod {};
    std::array<int, MAX> active {};
    int numActive = 0;
    uint32_t usedMask = 0;
    bool anyKeyed = false, anyCross = false, anyAmount = false, anyClosing = false, changed = false;

    std::array<float, ModSrc::COUNT> value {};
    std::array<bool, 4> lfoUnipolar {};
    std::array<float, 3> blockTarget {};
    float step = 0.01f, blockStep = 0.005f;
    static uint32_t nextSeed()
    {
        static std::atomic<uint32_t> made { 0 };
        return 0x9e3779b9u * (made.fetch_add(1, std::memory_order_relaxed) + 1u);
    }
    uint32_t randomState = nextSeed();
    int pendingNote = 60;
};

// The engine's stored values of the slots, their count and the custom LFO shape, and the pages.
class EngineModSlots
{
public:
    static constexpr int MAX = ModSlotParam::MAX_SLOTS;
    static constexpr int PER_PAGE = 2;
    static constexpr int DEFAULT_COUNT = 2;
    static constexpr int POINTS = LfoShapeParam::POINTS;

    struct Slot
    {
        int src = ModSrc::Off, dest = 0, via = ModSrc::Off;
        float amount = 0.0f;
    };

    enum Change { Nothing = 0, SlotChanged, CountChanged, ShapeChanged };
    // The pages' section (a page key and an Up / Down stop of their own, so the MOD key still
    // lands on the LFO pages).
    static constexpr const char* SECTION = "SLOT";
    static constexpr int MAX_PAGES = MAX / PER_PAGE;

    // How many slot pages are shown, from the engine's values (get(id) = the value of parameter
    // id): the screens ask this too, to know when the page list has changed.
    template <typename Get>
    static int pagesShown(const Get& get)
    {
        int last = -1;
        for (int s = MAX - 1; s >= 0 && last < 0; --s)
            for (int c = 0; c < ModSlotParam::STRIDE; ++c)
                if (get(ModSlotParam::id(s, c)) != 0.0f)
                    last = s;
        const int inUse = last < 0 ? 0 : last / PER_PAGE + 1;
        const int asked = ((int) std::lround(get(ModSlotParam::COUNT)) + PER_PAGE - 1) / PER_PAGE;
        return std::clamp(std::max(inUse + 1, asked), 1, MAX_PAGES);
    }

    EngineModSlots()
    {
        for (int i = 0; i < POINTS; ++i)
            levels[(size_t) i] = defaultLevel(i);
    }

    // A rising staircase (LFO::defaultUserLevel is the same).
    static float defaultLevel(int point) { return -1.0f + 2.0f * (float) point / (float) (POINTS - 1); }

    static constexpr bool ownsSlot(int id) { return id >= ModSlotParam::BASE && id < ModSlotParam::END; }
    static constexpr bool ownsShape(int id) { return id >= LfoShapeParam::BASE && id < LfoShapeParam::END; }
    static constexpr bool owns(int id) { return ownsSlot(id) || ownsShape(id) || id == ModSlotParam::COUNT; }
    static constexpr int slotOf(int id) { return (id - ModSlotParam::BASE) / ModSlotParam::STRIDE; }
    static constexpr int cellOf(int id) { return (id - ModSlotParam::BASE) % ModSlotParam::STRIDE; }

    const Slot& slot(int i) const { return slots[(size_t) std::clamp(i, 0, MAX - 1)]; }
    int count() const { return shown; }
    // Every slot acts (each one that holds anything is on a page).
    int acting() const { return MAX; }
    int numPages() const { return pagesShown([this](int id) { return get(id); }); }
    const float* shape() const { return levels.data(); }

    float get(int id) const
    {
        if (id == ModSlotParam::COUNT)
            return (float) shown;
        if (ownsShape(id))
            return levels[(size_t) (id - LfoShapeParam::BASE)];
        if (! ownsSlot(id))
            return 0.0f;
        const Slot& s = slots[(size_t) slotOf(id)];
        switch (cellOf(id))
        {
            case ModSlotParam::SRC:   return (float) s.src;
            case ModSlotParam::DEST:  return (float) s.dest;
            case ModSlotParam::AMT:   return s.amount;
            default:                  return (float) s.via;
        }
    }

    // Stores the (range-checked) value; says what kind of thing changed.
    int set(int id, float value, int numDests, int numSources)
    {
        if (! std::isfinite(value))
            value = 0.0f;
        value = std::clamp(value, -1.0e6f, 1.0e6f);     // (an index is rounded: keep it a number that can be)
        if (id == ModSlotParam::COUNT)
        {
            const int n = std::clamp((int) std::lround(value), 0, MAX);
            if (n == shown)
                return Nothing;
            shown = n;
            return CountChanged;
        }
        if (ownsShape(id))
        {
            levels[(size_t) (id - LfoShapeParam::BASE)] = std::clamp(value, -1.0f, 1.0f);
            return ShapeChanged;
        }
        if (! ownsSlot(id))
            return Nothing;
        Slot& s = slots[(size_t) slotOf(id)];
        switch (cellOf(id))
        {
            case ModSlotParam::SRC:   s.src = std::clamp((int) std::lround(value), 0, std::max(1, numSources) - 1); break;
            case ModSlotParam::DEST:  s.dest = std::clamp((int) std::lround(value), 0, std::max(1, numDests) - 1); break;
            case ModSlotParam::AMT:   s.amount = std::clamp(value, -1.0f, 1.0f); break;
            default:                  s.via = std::clamp((int) std::lround(value), 0, std::max(1, numSources) - 1); break;
        }
        return SlotChanged;
    }

    // ---- Pages ---------------------------------------------------------------------------------
    // SLT1 ... (two slots each, as many as pagesShown says), then LSHP.
    std::vector<ParamPage> makePages(const char* const* destLabels, int numDests, int numSources) const
    {
        auto L = ParamKind::Linear; auto C = ParamKind::Choice;
        static const char* const pageNames[MAX_PAGES] = { "SLT1", "SLT2", "SLT3", "SLT4", "SLT5", "SLT6", "SLT7", "SLT8" };
        static const char* const levelNames[POINTS] = { "L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8" };
        std::vector<ParamPage> out;

        for (int p = 0; p < numPages(); ++p)
        {
            ParamPage page;
            page.name = pageNames[p];
            page.section = SECTION;
            for (int h = 0; h < PER_PAGE; ++h)
            {
                const int s = p * PER_PAGE + h;
                const auto c = (size_t) (h * ModSlotParam::STRIDE);
                page.params[c + 0] = { "SRC", ModSlotParam::id(s, ModSlotParam::SRC), 0, (float) (numSources - 1), 0, C, "",
                                       ModSrc::labels(), numSources };
                page.params[c + 1] = { "DEST", ModSlotParam::id(s, ModSlotParam::DEST), 0, (float) (numDests - 1), 0, C, "",
                                       destLabels, numDests };
                page.params[c + 2] = { "AMT", ModSlotParam::id(s, ModSlotParam::AMT), -1, 1, 0, L };
                page.params[c + 3] = { "VIA", ModSlotParam::id(s, ModSlotParam::VIA), 0, (float) (numSources - 1), 0, C, "",
                                       ModSrc::labels(), numSources };
            }
            out.push_back(page);
        }

        ParamPage shapePage;
        shapePage.name = "LSHP";
        shapePage.section = SECTION;
        for (int i = 0; i < POINTS; ++i)
            shapePage.params[(size_t) i] = { levelNames[i], LfoShapeParam::BASE + i, -1, 1, defaultLevel(i), L };
        out.push_back(shapePage);
        return out;
    }

    // Puts the pages in front of the page called `before` (at the end when there is none).
    void insertPages(ParamPages& pages, const char* before, const char* const* destLabels, int numDests, int numSources) const
    {
        auto at = pages.end();
        if (before != nullptr)
            for (auto it = pages.begin(); it != pages.end(); ++it)
                if (it->name == before)
                {
                    at = it;
                    break;
                }
        const auto made = makePages(destLabels, numDests, numSources);
        pages.insert(at, made.begin(), made.end());
    }

    static juce::String name(int id)
    {
        if (id == ModSlotParam::COUNT)
            return "Mod Slots";
        if (ownsShape(id))
            return "LFO Shape " + juce::String(id - LfoShapeParam::BASE + 1);
        if (! ownsSlot(id))
            return {};
        static const char* const cells[ModSlotParam::STRIDE] = { " Source", " Dest", " Amount", " Via" };
        return "Slot " + juce::String(slotOf(id) + 1) + cells[cellOf(id)];
    }

    // ---- The destination names of a slot's DEST cell -------------------------------------------
    // The engine's own list (the LFOs' indices), then what only a slot can reach: `extra` labels
    // (an LFO's rate and depth, where the engine's list does not have them) and the amount of
    // each slot. The lists are kept for as long as the app runs (a ParamSpec points into them);
    // a list is known by the engine's label array. Not for the audio thread.
    static constexpr int MAX_DESTS = 96 + 8 + MAX;

    struct DestNames
    {
        std::array<const char*, MAX_DESTS> text {};
        std::array<int, MAX_DESTS> starts {};
        int count = 0, numStarts = 0;
        int engineCount = 0;        // the engine's own destinations
        int firstAmount = 0;        // index of "the AMT of slot 1"
    };

    static const DestNames& destNames(const char* const* engineLabels, int engineCount,
                                      const char* const* extra, int numExtra)
    {
        static std::mutex lock;
        static std::vector<std::unique_ptr<DestNames>> made;
        static std::vector<const char* const*> keys;
        const std::lock_guard<std::mutex> guard(lock);
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == engineLabels && made[i]->engineCount == engineCount)
                return *made[i];

        static const char* const amounts[MAX] = { "AMT1", "AMT2", "AMT3", "AMT4", "AMT5", "AMT6", "AMT7", "AMT8",
                                                  "AMT9", "AM10", "AM11", "AM12", "AM13", "AM14", "AM15", "AM16" };
        auto n = std::make_unique<DestNames>();
        engineCount = std::clamp(engineCount, 1, 96);
        numExtra = std::clamp(numExtra, 0, 8);
        // The engine's own groups (Func + turn), where it registered them.
        if (const auto* groups = ChoiceGroups::find(engineLabels))
            for (int g = 0; g < groups->count; ++g)
                n->starts[(size_t) n->numStarts++] = groups->starts[g];
        else
        {
            n->starts[(size_t) n->numStarts++] = 0;
            if (engineCount > 1)
                n->starts[(size_t) n->numStarts++] = 1;
        }
        for (int i = 0; i < engineCount; ++i)
            n->text[(size_t) n->count++] = engineLabels[i];
        n->engineCount = engineCount;
        if (numExtra > 0)
            n->starts[(size_t) n->numStarts++] = n->count;
        for (int i = 0; i < numExtra; ++i)
            n->text[(size_t) n->count++] = extra[i];
        n->firstAmount = n->count;
        n->starts[(size_t) n->numStarts++] = n->count;
        for (int i = 0; i < MAX; ++i)
            n->text[(size_t) n->count++] = amounts[i];
        ChoiceGroups::add(n->text.data(), n->starts.data(), n->numStarts);
        keys.push_back(engineLabels);
        made.push_back(std::move(n));
        return *made.back();
    }

private:
    std::array<Slot, MAX> slots {};
    std::array<float, POINTS> levels {};
    int shown = DEFAULT_COUNT;
};
