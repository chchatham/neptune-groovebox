#pragma once

// The wavetable engine as it was before phase U4-WT moved it onto the modulation router
// (mod::Router) and dsp/PhaseOsc.h, with the two sections it used (EngineModSection.h,
// EngineLfoSection.h) as they were at commit c21530a: copies, in a namespace of their own, so
// that "U4 Wavetable" can render old against new. Nothing in the product includes this file.

#include "../../src/engine/EngineBase.h"
#include "../../src/engine/VoiceAllocator.h"
#include "../../src/engine/VoiceFilterSection.h"
#include "../../src/engine/VoiceSpread.h"
#include "../../src/engine/ParamSpec.h"
#include "../../src/engine/SampleBank.h"
#include "../../src/dsp/LFO.h"
#include "../../src/dsp/Envelope.h"
#include "../../src/dsp/EnvelopeChain.h"
#include "../../src/dsp/DSPUtils.h"
#include "../../src/dsp/Wavetable.h"
#include "../../src/dsp/filters/SVFilter.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace oldwt
{

// (spread::scaleTimes as it was: the product keeps it only until every engine is ported.)
inline Envelope::Params oldScaleTimes(Envelope::Params p, float scale)
{
    p.attack *= scale;
    p.decay *= scale;
    p.release *= scale;
    return p;
}


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



// -----------------------------------------------------------------------------
// The LFO section shared by the FM, macro-oscillator, wavetable, wave-sequencing,
// sampler and drum engines: two LFOs in every voice and the two pages that set
// them (the same values go to every voice).
//
//   LFO1   RATE DPTH SHPE TRIG | SYNC DIV  FADE DEST     ids 158-165
//   LFO2   the same for LFO 2                            ids 166-173
//
// RATE (Hz) is used while SYNC is off, DIV (a tempo division) while it is on.
// DEST picks one destination from the engine's own list (index 0 = OFF); DPTH
// is signed. An LFO with DEST = OFF or DPTH = 0 is not run at all, so a sound
// that does not use the section is rendered exactly as before.
//
// Amounts. A voice reads amount(dest): the sum of the outputs (-1..1, already
// scaled by depth and fade-in) of the LFOs routed to that destination, and
// scales it itself: cutoff x 4 octaves, resonance x 0.5, level as a gain of
// 1 + amount (0..2), pan as +-1, normalised engine parameters as +-1 around
// the knob (clamped to the knob's range).
// Pitch uses pitchSemitones(dest) instead: +-12 semitones at full depth with
// the DEPTH SQUARED, so the lower half of the knob is vibrato territory
// (depth 10 % = 12 cents, 20 % = half a semitone, 50 % = 3 semitones).
//
// FREE (and HOLD) behave like one LFO that never stops: every voice's LFO is
// advanced by exactly the same number of samples whether the voice sounds or
// not (tick() while it sounds, endBlock() for the rest of each block), and a
// change of trigger mode starts all of them again from the same state.
//
//
// Destinations. An engine's list (LfoDestList) has three kinds of entry, in this order:
//   fixed      the engine's original destinations, at their original indices (saved sounds keep
//              their meaning). The voice reads amount(index) / pitchSemitones(index) as above.
//   parameter  a cell of the engine's own pages, taken from its ParamSpec (label, range, kind) and
//              known by a key (the parameter id). The LFO moves the cell's value in the cell's own
//              scale: a linear cell by +-(its whole range) at full depth, a logarithmic one by
//              +-(half its range in octaves), clamped to the range. The voice asks
//              value(key, setValue) wherever it reads that parameter (or the engine pushes
//              value(key) into the voice's own setter each sample, forEachKeyed), so one rule
//              serves every cell and no destination needs code of its own.
//   LRAT LDPT  the other LFO's rate (+-2 octaves) and depth (x 0..2), one sample late.
// The list is built once per engine from its pages; DEST resolves the entry into the voices'
// LFOs when it is set, so the audio thread compares two integers per read and looks nothing up.
//
//
// The mod strip. The same section holds the STRIP page (ids 174-181):
//
//   STRIP  DST1 AMT1 DST2 AMT2 | DST3 AMT3 WVIB VRAT
//
// Three routes from the strip (0..1, one value for the whole track) to a destination of the
// same list, each with a signed amount. A route is one more source beside the two LFOs: its
// output is strip x AMT, and the voice reads it through the very same calls (amount,
// pitchSemitones, value, forEachKeyed), so whatever an LFO can reach the strip can reach, in the
// same scale: AMT +100 with the strip fully up = an LFO at full depth at its peak. Pitch is
// strip x AMT squared x 12 semitones. LRAT and LDPT move BOTH LFOs from the strip.
// WVIB is a vibrato the strip brings in (semitones at the top of the strip, a sine at VRAT Hz,
// the same for every voice), added to the pitch destination.
// The engine gives the section the smoothed strip once per block (stripBlock). With the strip
// at rest nothing is live: no route is read and the voices render exactly as without the page.
// An engine's default routes (what its strip did before the page existed) are part of its
// destination list (LfoDestList::stripRoute / stripVibrato).
//
//
// Mod slots (EngineModSection.h, ids 700-772). The section also holds the engine's slots, their
// pages (SLT1 ..., LSHP, placed in front of STRIP: insertModPages) and the custom LFO shape.
// A voice's slots live in its VoiceLfos and are one more kind of source: whatever a slot is aimed
// at is read through the same calls as the LFOs and the strip. A slot's DEST list is the engine's
// own list at the LFOs' indices, then L1RT L1DP L2RT L2DP (one LFO's rate / depth; LRAT / LDPT
// from a slot, as from the strip, move both), then AMT1 .. AM16 (another slot's amount).
// The voice gives its slots what they need to know: setNote before noteOn, setEnvelopes each
// sample while wantsEnvelopes(), and the engine setBlockSources once a block while slotsOn().
// With no slot in use none of it runs and the voices render exactly as without the pages.
//
// Nothing here allocates or locks.
// -----------------------------------------------------------------------------

// An engine's LFO destinations: labels for the DEST cell and what each entry moves.
class LfoDestList
{
public:
    static constexpr int MAX = 96;

    enum Cross { None = 0, OtherRate, OtherDepth };

    struct Entry
    {
        const char* label = "OFF";
        int key = -1;                   // >= 0: a parameter destination (the engine's own key, usually the id)
        float lo = 0.0f, hi = 1.0f;     // the parameter's range
        bool log = false;
        int cross = None;
        float stripScale = 1.0f;        // the strip's amount on this destination is multiplied by this
    };

    static constexpr int STRIP_ROUTES = 3;
    struct StripDefault { int dest = 0; float amount = 0.0f; };

    LfoDestList() = default;

    // fixedLabels: the engine's original destinations (index 0 = "OFF"), static storage.
    LfoDestList(const char* const* fixedLabels, int numFixed)
    {
        for (int i = 0; i < numFixed && count < MAX; ++i)
        {
            items[(size_t) count].label = fixedLabels[i];
            names[(size_t) count++] = fixedLabels[i];
        }
        fixed = count;
        starts[(size_t) numStarts++] = 0;       // OFF
        if (count > 1)
            starts[(size_t) numStarts++] = 1;   // the original destinations
    }

    // The entries added from here on are a new group (Func + turn on DEST stops at its start).
    LfoDestList& group()
    {
        if (numStarts < MAX && (numStarts == 0 || starts[(size_t) (numStarts - 1)] != count))
            starts[(size_t) numStarts++] = count;
        return *this;
    }

    // A page cell as a destination. label: another name where the cell's own is taken; key: the
    // voice's name for the parameter where that is not the id.
    LfoDestList& add(const ParamSpec& spec, const char* label = nullptr, int key = -1)
    {
        if (! spec.isValid() || spec.kind == ParamKind::Choice || count >= MAX || spec.maxVal <= spec.minVal)
            return *this;
        Entry e;
        e.label = label != nullptr ? label : spec.label;
        e.key = key >= 0 ? key : spec.id;
        e.lo = spec.minVal;
        e.hi = spec.maxVal;
        e.log = spec.kind == ParamKind::Log && spec.minVal > 0.0f;
        jassert(indexOfLabel(e.label) < 0 && indexOfKey(e.key) < 0);
        items[(size_t) count] = e;
        names[(size_t) count++] = e.label;
        return *this;
    }

    // The cell with this id on one of the pages.
    LfoDestList& add(const ParamPages& pages, int id, const char* label = nullptr, int key = -1)
    {
        for (auto& page : pages)
            if (const auto* spec = page.find(id))
                return add(*spec, label, key);
        jassertfalse;
        return *this;
    }

    // The other LFO's rate and depth.
    LfoDestList& addCross()
    {
        group();
        static const char* const labels[2] = { "LRAT", "LDPT" };
        for (int i = 0; i < 2 && count < MAX; ++i)
        {
            Entry e;
            e.label = labels[i];
            e.cross = i == 0 ? OtherRate : OtherDepth;
            items[(size_t) count] = e;
            names[(size_t) count++] = e.label;
        }
        return *this;
    }

    // ---- What the engine's mod strip does until the STRIP page is edited ---------------------
    LfoDestList& stripRoute(int route, int destIndex, float amount)
    {
        if (route >= 0 && route < STRIP_ROUTES && destIndex >= 0 && destIndex < count)
            stripDefaults[(size_t) route] = { destIndex, std::clamp(amount, -1.0f, 1.0f) };
        else
            jassertfalse;
        return *this;
    }
    LfoDestList& stripRoute(int route, const char* label, float amount)
    {
        return stripRoute(route, indexOfLabel(label), amount);
    }
    LfoDestList& stripVibrato(float semitones) { stripVibratoDefault = semitones; return *this; }
    // From the strip, AMT on this destination counts `scale` times (FM: INDX +100 triples the index).
    LfoDestList& stripScale(int destIndex, float scale)
    {
        if (destIndex >= 0 && destIndex < count)
            items[(size_t) destIndex].stripScale = scale;
        return *this;
    }
    const StripDefault& stripDefault(int route) const { return stripDefaults[(size_t) std::clamp(route, 0, STRIP_ROUTES - 1)]; }
    float stripVibratoDefaultValue() const { return stripVibratoDefault; }

    // Call once the list is complete and where it will stay (static storage): Func + turn on a
    // DEST cell with these labels then moves by group.
    const LfoDestList& registerGroups() const
    {
        ChoiceGroups::add(names.data(), starts.data(), numStarts);
        return *this;
    }

    int size() const { return count; }
    int numFixed() const { return fixed; }
    const Entry& operator[](int i) const { return items[(size_t) std::clamp(i, 0, std::max(0, count - 1))]; }
    const char* const* labels() const { return names.data(); }
    int numGroups() const { return numStarts; }
    int groupStart(int g) const { return starts[(size_t) std::clamp(g, 0, std::max(0, numStarts - 1))]; }

    int indexOfKey(int key) const
    {
        for (int i = 0; i < count; ++i)
            if (items[(size_t) i].key == key && key >= 0)
                return i;
        return -1;
    }
    int indexOfLabel(const char* label) const
    {
        for (int i = 0; i < count; ++i)
            if (std::strcmp(items[(size_t) i].label, label) == 0)
                return i;
        return -1;
    }

private:
    std::array<Entry, MAX> items {};
    std::array<const char*, MAX> names {};
    std::array<int, MAX> starts {};
    int count = 0, fixed = 0, numStarts = 0;
    std::array<StripDefault, STRIP_ROUTES> stripDefaults {};
    float stripVibratoDefault = 0.0f;
};

// One voice's two LFOs.
class VoiceLfos
{
public:
    static constexpr int NUM_LFOS = 2;
    static constexpr int NUM_STRIP = LfoDestList::STRIP_ROUTES;
    static constexpr int NUM_SLOTS = VoiceModSlots::MAX;
    static constexpr int NUM_SOURCES = NUM_LFOS + NUM_STRIP + NUM_SLOTS;    // what keyAt() counts: the LFOs, the strip routes, the mod slots
    static constexpr float PITCH_SEMITONES = 12.0f;
    static constexpr float CUTOFF_OCTAVES = 4.0f;
    static constexpr float RESONANCE_RANGE = 0.5f;

    struct Settings
    {
        float rate = 2.0f;                      // Hz
        float depth = 0.0f;                     // -1..1
        int shape = 0;                          // LFO::Shape
        int trigger = 0;                        // LFO::TriggerMode
        bool sync = false;
        int division = LFO::DEFAULT_DIVISION;
        float fade = 0.0f;                      // seconds
        int dest = 0;                           // engine's list, 0 = off
        float tempo = 120.0f;
        // What `dest` is in the engine's list (EngineLfoSection fills these in when DEST is set).
        int key = -1;                           // parameter destination: its key
        float lo = 0.0f, hi = 1.0f;             // ... its range
        bool log = false;
        float base = 0.0f;                      // ... its set value (for engines that push, forEachKeyed)
        int cross = 0;                          // LfoDestList::Cross
    };

    static constexpr float CROSS_RATE_OCTAVES = 2.0f;

    void setSampleRate(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        for (auto& l : lfos)
            l.setSampleRate(sampleRate);
        gainSlew.setSampleRate(sampleRate);
        panSlew.setSampleRate(sampleRate);
        mods.setSampleRate(sampleRate);
        glideStep = (float) (2.0 / (GLIDE_SECONDS * sampleRate));
        stripStep = (float) (1.0 / (STRIP_GLIDE_SECONDS * sampleRate));
    }

    // restart: begin again from a new LFO (used when the trigger mode changes, so that the voices
    // of an engine are in step with each other afterwards).
    //
    // An LFO that is taken off its destination (DEST changed, or switched off by DEST or DPTH)
    // under a sounding voice lets go of it first: its output glides to zero over a few
    // milliseconds, and only then do the new settings apply (a snap back would be a click).
    void apply(int i, const Settings& s, bool restart)
    {
        if (i < 0 || i >= NUM_LFOS)
            return;
        const bool stays = s.dest == dest[(size_t) i] && s.dest != 0 && s.depth != 0.0f;
        if (on[(size_t) i] && ! stays && sounding() && glide[(size_t) i] != 0.0f)
        {
            pending[(size_t) i] = s;
            pendingRestart[(size_t) i] = restart || (closing[(size_t) i] && pendingRestart[(size_t) i]);
            closing[(size_t) i] = true;
            anyClosing = true;
            return;
        }
        closing[(size_t) i] = false;
        applyNow(i, s, restart);
    }

    // True while an LFO or a strip route is still letting go of a destination (see apply).
    bool isClosing() const { return anyClosing; }

private:
    void applyNow(int i, const Settings& s, bool restart)
    {
        auto& l = lfos[(size_t) i];
        if (restart)
        {
            l = LFO();
            l.setSampleRate(sampleRate);
            l.setUserShape(userShape);
            out[(size_t) i] = 0.0f;
        }
        l.setRate(s.rate);
        l.setDepth(s.depth);
        l.setShape((LFO::Shape) s.shape);
        l.setTriggerMode((LFO::TriggerMode) s.trigger);
        l.setTempo(s.tempo);
        l.setSyncDivision(s.division);
        l.setTempoSync(s.sync);
        l.setFadeTime(s.fade);

        dest[(size_t) i] = s.dest;
        absDepth[(size_t) i] = std::abs(s.depth);
        on[(size_t) i] = s.dest != 0 && s.depth != 0.0f;
        if (! on[(size_t) i])
        {
            out[(size_t) i] = 0.0f;
            glide[(size_t) i] = 0.0f;
        }
        edged[(size_t) i] = hasEdge((LFO::Shape) s.shape);
        catchUp[(size_t) i] = true;         // whatever changed, the output may have jumped
        anyActive = on[0] || on[1] || stripLive || slotsLive;

        key[(size_t) i] = on[(size_t) i] ? s.key : -1;
        lo[(size_t) i] = s.lo;
        hi[(size_t) i] = s.hi;
        isLog[(size_t) i] = s.log && s.lo > 0.0f;
        // Full depth: a linear cell's whole range, half a logarithmic one's (in octaves).
        span[(size_t) i] = isLog[(size_t) i] ? 0.5f * std::log2(s.hi / s.lo) : s.hi - s.lo;
        baseValue[(size_t) i] = s.base;
        cross[(size_t) i] = on[(size_t) i] ? s.cross : 0;
        anyKeyed = key[0] >= 0 || key[1] >= 0 || stripKeyed || mods.keyed();
        updateCross();
    }

    // The shapes that jump: at the turn of the cycle, or (the two decays) when it starts again.
    static bool hasEdge(LFO::Shape s)
    {
        return s != LFO::Shape::Sine && s != LFO::Shape::Triangle && s != LFO::Shape::SmoothRandom;
    }

    // True when this voice was heard in this block or the last one.
    bool sounding() const { return ticks > 0 || wasSounding; }

public:
    // ---- The mod strip's routes --------------------------------------------------------------
    struct StripRoute
    {
        int dest = 0;                           // engine's list, 0 = off
        float amount = 0.0f;                    // -1..1
        float scale = 1.0f;                     // LfoDestList::Entry::stripScale
        int key = -1;                           // as Settings
        float lo = 0.0f, hi = 1.0f;
        bool log = false;
        float base = 0.0f;
        int cross = 0;
    };

    // A route's settings (EngineLfoSection, when a STRIP cell is set).
    // A route taken off its destination under a sounding voice lets go of it first, like an LFO.
    void applyStrip(int r, const StripRoute& s)
    {
        if (r < 0 || r >= NUM_STRIP)
            return;
        const auto i = (size_t) r;
        const bool stays = s.dest == sDest[i] && s.dest != 0 && s.amount != 0.0f;
        if (sOn[i] && ! stays && sounding() && sGlide[i] != 0.0f)
        {
            sPending[i] = s;
            sClosing[i] = true;
            anyClosing = true;
            sOut[i] = 0.0f;
            return;
        }
        sClosing[i] = false;
        applyStripNow(r, s);
    }

private:
    void applyStripNow(int r, const StripRoute& s)
    {
        const auto i = (size_t) r;
        sDest[i] = s.dest;
        sAmount[i] = s.amount * s.scale;
        sAbs[i] = std::abs(s.amount);
        sUsed[i] = s.dest != 0 && s.amount != 0.0f;
        sSetKey[i] = sUsed[i] ? s.key : -1;
        sSetCross[i] = sUsed[i] ? s.cross : 0;
        sLo[i] = s.lo;
        sHi[i] = s.hi;
        sLog[i] = s.log && s.lo > 0.0f;
        sSpan[i] = sLog[i] ? 0.5f * std::log2(s.hi / s.lo) : s.hi - s.lo;
        sBase[i] = s.base;
        refreshStrip();
    }

public:
    // The strip's position (0..1, smoothed) and the vibrato it brings (semitones, this block),
    // which is added to pitch destination `pitchDest`.
    void setStrip(float position, float vibratoSemitones, int pitchDest)
    {
        stripPosition = position;
        stripVibrato = vibratoSemitones;
        stripPitchDest = pitchDest;
        refreshStrip();
    }

    // True while the strip is up and has a route (or vibrato) to apply.
    bool stripOn() const { return stripLive; }

    // ---- The mod slots -----------------------------------------------------------------------
    // A slot's settings (EngineLfoSection, when one of its cells is set).
    void applySlot(int slot, const VoiceModSlots::Settings& s)
    {
        mods.apply(slot, s, sounding());
        anyClosing = anyClosing || mods.isClosing();
        if (mods.takeChanged())
            refreshSlots();
    }
    // The note that is about to start: call before noteOn / noteOnAndSample.
    void setNote(int note, float velocity) { mods.setNote(note, velocity); }
    // True while a slot reads the envelopes: the voice then gives them each sample, before tick().
    bool wantsEnvelopes() const { return slotsLive && mods.usesEnvelopes(); }
    void setEnvelopes(float amp, float filter) { mods.setEnvelopes(amp, filter); }
    // Once a block (while the engine has a slot in use): this voice's pad pressure, the strip and
    // the track input's follower, each 0..1.
    void setBlockSources(float pressure, float strip, float input) { mods.setBlockSources(pressure, strip, input); }
    // The track's custom LFO shape (LfoShapeParam): eight levels that stay the engine's.
    void setUserShape(const float* levels)
    {
        userShape = levels;
        for (auto& l : lfos)
            l.setUserShape(levels);
    }
    const VoiceModSlots& getSlots() const { return mods; }

    // ---- Parameter destinations ------------------------------------------------------------
    // True when an LFO in use is routed to a parameter destination.
    bool keyed() const { return anyKeyed; }
    // True when an LFO in use is routed to parameter k.
    bool has(int k) const
    {
        return anyKeyed && (key[0] == k || key[1] == k
                            || (stripKeyed && (sKey[0] == k || sKey[1] == k || sKey[2] == k))
                            || (mods.keyed() && mods.hasKey(k)));
    }

    // The parameter source i is moving (-1: none): 0 and 1 are the LFOs, then the strip routes,
    // then the mod slots.
    int keyAt(int i) const
    {
        i = std::clamp(i, 0, NUM_SOURCES - 1);
        if (i >= NUM_LFOS + NUM_STRIP)
            return mods.keyOf(i - NUM_LFOS - NUM_STRIP);
        return i < NUM_LFOS ? key[(size_t) i] : sKey[(size_t) (i - NUM_LFOS)];
    }

    // Parameter k under the LFOs routed to it; `set` (its set value) when none is.
    float value(int k, float set) const
    {
        if (! anyKeyed)
            return set;
        if (key[0] == k) set = moved(0, set);
        if (key[1] == k) set = moved(1, set);
        if (stripKeyed)
            for (int r = 0; r < NUM_STRIP; ++r)
                if (sKey[(size_t) r] == k)
                    set = movedByStrip(r, set);
        if (mods.keyed())
            set = mods.keyedValue(k, set);
        return set;
    }

    // For engines whose voices have a setter per parameter: fn(key, value) for every parameter an
    // LFO is routed to, from the set value EngineLfoSection keeps up to date.
    template <typename Fn>
    void forEachKeyed(Fn&& fn) const
    {
        if (key[0] >= 0)
            fn(key[0], value(key[0], baseValue[0]));
        if (key[1] >= 0 && key[1] != key[0])
            fn(key[1], value(key[1], baseValue[1]));
        if (stripKeyed)
            for (int r = 0; r < NUM_STRIP; ++r)
            {
                const int k = sKey[(size_t) r];
                if (k < 0 || k == key[0] || k == key[1] || (r > 0 && k == sKey[0]) || (r > 1 && k == sKey[1]))
                    continue;       // already given, with every source on it
                fn(k, value(k, sBase[(size_t) r]));
            }
        if (mods.keyed())
            for (int a = 0; a < mods.count(); ++a)
            {
                const int slot = mods.slotAt(a);
                const int k = mods.keyOf(slot);
                if (k < 0 || k == key[0] || k == key[1] || (stripKeyed && (k == sKey[0] || k == sKey[1] || k == sKey[2])))
                    continue;       // already given, with every source on it
                bool earlier = false;
                for (int b = 0; b < a && ! earlier; ++b)
                    earlier = mods.keyOf(mods.slotAt(b)) == k;
                if (! earlier)
                    fn(k, value(k, mods.target(slot).base));
            }
    }

    // True when at least one LFO has a destination and a depth.
    bool anyOn() const { return anyActive; }

    // Note-on: see LFO::trigger for what each trigger mode does with it.
    void noteOn()
    {
        // A new note takes everything as it stands: nothing left to let go of, nothing to glide from.
        // (Only while the LFOs run: one that is switched on later, or the strip coming up, glides
        // in from nothing under the note.)
        if (anyClosing)
            finishClosing(true);
        if (landing)
        {
            sGlide.fill(0.0f);
            refreshStrip();
        }
        fresh = anyActive;
        for (auto& l : lfos)
            l.trigger();
        mods.noteOn();
    }

    // Note-on for a voice that needs the LFO values at the moment of the hit (a drum's decay, a
    // sample's start point): triggers and works out the first sample's values now; the first
    // tick() afterwards reuses them, so the LFOs do not run ahead.
    void noteOnAndSample()
    {
        noteOn();
        if (anyActive)
        {
            run();
            sampledAhead = true;
        }
    }

    // One sample.
    void tick()
    {
        ++ticks;
        if (sampledAhead)
            sampledAhead = false;
        else
            run();
    }

    // End of a block of numSamples: moves the LFOs on by the samples this voice did not process.
    void endBlock(int numSamples)
    {
        int idle = numSamples - ticks;
        wasSounding = ticks > 0;
        ticks = 0;
        if (anyClosing && ! wasSounding)
            finishClosing(true);        // nobody is listening: let go at once
        if (landing && ! wasSounding)
        {
            sGlide.fill(0.0f);
            refreshStrip();
        }
        if (idle <= 0 || ! anyActive)
            return;
        for (int i = 0; i < NUM_LFOS; ++i)
        {
            if (! on[(size_t) i] && ! slotLfo[(size_t) i])
                continue;
            // At most one cycle per call, so the random shapes draw as many values as a running LFO.
            auto& l = lfos[(size_t) i];
            const float hz = l.getEffectiveRate();
            const int chunk = hz > 0.0f ? std::max(1, (int) (sampleRate / (double) hz)) : idle;
            for (int left = idle; left > 0; left -= chunk)
                l.advanceIdle(std::min(left, chunk));
        }
    }

    bool routed(int d) const
    {
        return (on[0] && dest[0] == d) || (on[1] && dest[1] == d)
            || (stripLive && ((sOn[0] && sDest[0] == d) || (sOn[1] && sDest[1] == d) || (sOn[2] && sDest[2] == d)))
            || (slotsLive && mods.fixedRouted(d));
    }

    // Sum of the LFOs (and strip routes) routed to destination d, -1..1 each (0 if none is).
    float amount(int d) const
    {
        const float a = (dest[0] == d ? glide[0] : 0.0f) + (dest[1] == d ? glide[1] : 0.0f);
        if (! stripLive)
            return slotsLive ? a + mods.fixedAmount(d) : a;
        const float b = a + (sDest[0] == d ? sGlide[0] : 0.0f) + (sDest[1] == d ? sGlide[1] : 0.0f) + (sDest[2] == d ? sGlide[2] : 0.0f);
        return slotsLive ? b + mods.fixedAmount(d) : b;
    }

    // A level destination as the gain the voice applies, 0..2, and a pan destination's amount:
    // once per sample, after tick(). Neither ever steps (dsp::GainSlew), whatever the shape.
    float gain(int d) { return gainSlew.process(std::clamp(1.0f + amount(d), 0.0f, 2.0f)); }
    float panAmount(int d) { return panSlew.process(amount(d)); }
    // A voice that starts from silence, or whose LFOs were off, has nothing to step from.
    void snapSlews() { gainSlew.snap(); panSlew.snap(); }

    // The same for a pitch destination, in semitones (depth squared, see the top of the file).
    float pitchSemitones(int d) const
    {
        const float a = ((dest[0] == d ? out[0] * absDepth[0] : 0.0f)
                       + (dest[1] == d ? out[1] * absDepth[1] : 0.0f)) * PITCH_SEMITONES;
        if (! stripLive)
            return slotsLive ? a + mods.fixedPitch(d) * PITCH_SEMITONES : a;
        const float b = a + ((sDest[0] == d ? sOut[0] * sAbs[0] : 0.0f) + (sDest[1] == d ? sOut[1] * sAbs[1] : 0.0f)
                    + (sDest[2] == d ? sOut[2] * sAbs[2] : 0.0f)) * PITCH_SEMITONES
                 + (d == stripPitchDest ? stripVibrato : 0.0f);
        return slotsLive ? b + mods.fixedPitch(d) * PITCH_SEMITONES : b;
    }

    LFO& getLfo(int i) { return lfos[(size_t) std::clamp(i, 0, NUM_LFOS - 1)]; }
    float getOutput(int i) const { return out[(size_t) std::clamp(i, 0, NUM_LFOS - 1)]; }

private:
    // What every destination but pitch reads is the output on a lead: `glide` follows `out`
    // exactly while it moves smoothly, and at no more than the whole swing (2) in GLIDE_SECONDS
    // where it jumps: at the edge of a square, ramp, random or decay shape, when a setting was
    // changed, while one LFO moves the other's depth, and while it lets go of a destination. The
    // level or timbre still changes abruptly to the ear; only the one-sample jump (a click on
    // whatever is sounding) is gone. Pitch reads `out` itself: a pitch step is no click (the phase
    // goes on from where it is). A note-on takes everything as it stands (fresh).
    void run()
    {
        if (anyClosing)
            finishClosing(false);
        for (int i = 0; i < NUM_LFOS; ++i)
        {
            if (! on[(size_t) i])
            {
                if (slotLfo[(size_t) i])
                    lfos[(size_t) i].process();     // a slot's source: its shape is read below
                continue;
            }
            const float raw = lfos[(size_t) i].process();
            out[(size_t) i] = closing[(size_t) i] ? 0.0f : raw;
            const float d = out[(size_t) i] - glide[(size_t) i];
            const bool held = edged[(size_t) i] || catchUp[(size_t) i] || anyCross || closing[(size_t) i];
            if (fresh || ! held || std::abs(d) <= glideStep)
            {
                glide[(size_t) i] = out[(size_t) i];
                catchUp[(size_t) i] = false;
            }
            else
                glide[(size_t) i] += d > 0.0f ? glideStep : -glideStep;
        }
        if (stripLive || anyClosing)
            for (int r = 0; r < NUM_STRIP; ++r)
            {
                // The strip arrives once per block: its routes move on at no more than the whole
                // strip in STRIP_GLIDE_SECONDS.
                const float d = sOut[(size_t) r] - sGlide[(size_t) r];
                const float step = stripStep * std::max(1.0f, std::abs(sAmount[(size_t) r]));
                if (fresh || std::abs(d) <= step)
                    sGlide[(size_t) r] = sOut[(size_t) r];
                else
                    sGlide[(size_t) r] += d > 0.0f ? step : -step;
            }
        if (slotsLive)
        {
            for (int i = 0; i < NUM_LFOS; ++i)
                if (slotLfo[(size_t) i])
                    mods.setLfo(i, lfos[(size_t) i].getShapeOutput(), LFO::isUnipolar(lfos[(size_t) i].getShape()));
            mods.run(fresh);
            if (mods.takeChanged())
                refreshSlots();     // a slot has let go
        }
        fresh = false;
        if (landing && sGlide[0] == 0.0f && sGlide[1] == 0.0f && sGlide[2] == 0.0f)
            refreshStrip();     // the strip has been let go of: at rest
        if (anyCross)
        {
            // An LFO moving the other one (and the strip moving both): heard from the next sample on.
            float rate[NUM_LFOS] = { 0.0f, 0.0f }, depth[NUM_LFOS] = { 0.0f, 0.0f };
            bool hasRate[NUM_LFOS] = { false, false }, hasDepth[NUM_LFOS] = { false, false };
            for (int i = 0; i < NUM_LFOS; ++i)
            {
                if (cross[(size_t) i] == 1)       { rate[1 - i] += out[(size_t) i] * CROSS_RATE_OCTAVES; hasRate[1 - i] = true; }
                else if (cross[(size_t) i] == 2)  { depth[1 - i] += out[(size_t) i]; hasDepth[1 - i] = true; }
            }
            for (int r = 0; r < NUM_STRIP; ++r)
            {
                if (sCross[(size_t) r] == 1)
                    for (int i = 0; i < NUM_LFOS; ++i) { rate[i] += sOut[(size_t) r] * CROSS_RATE_OCTAVES; hasRate[i] = true; }
                else if (sCross[(size_t) r] == 2)
                    for (int i = 0; i < NUM_LFOS; ++i) { depth[i] += sOut[(size_t) r]; hasDepth[i] = true; }
            }
            if (mods.crossing())
                for (int a = 0; a < mods.count(); ++a)
                {
                    const int slot = mods.slotAt(a);
                    const ModSlotTarget& t = mods.target(slot);
                    if (t.kind != ModSlotTarget::LfoRate && t.kind != ModSlotTarget::LfoDepth)
                        continue;
                    for (int i = 0; i < NUM_LFOS; ++i)
                    {
                        if (t.index >= 0 && t.index != i)
                            continue;
                        if (t.kind == ModSlotTarget::LfoRate)   { rate[i] += mods.outputOf(slot) * CROSS_RATE_OCTAVES; hasRate[i] = true; }
                        else                                    { depth[i] += mods.outputOf(slot); hasDepth[i] = true; }
                    }
                }
            for (int i = 0; i < NUM_LFOS; ++i)
            {
                if (hasRate[i])  lfos[(size_t) i].setRateMod(rate[i]);
                if (hasDepth[i]) lfos[(size_t) i].setDepthMod(depth[i]);
            }
        }
    }

    // Which sources move an LFO changed: start from unmoved LFOs (run() sets what still applies).
    void updateCross()
    {
        const bool crossed = cross[0] != 0 || cross[1] != 0 || sCross[0] != 0 || sCross[1] != 0 || sCross[2] != 0
                             || mods.crossing();
        if (anyCross || crossed)
            for (auto& lfo : lfos)
            {
                lfo.setRateMod(0.0f);
                lfo.setDepthMod(0.0f);
            }
        anyCross = crossed;
    }

    // The slots in use changed: the flags that say whether anything is live, keyed or moving an LFO.
    void refreshSlots()
    {
        slotsLive = mods.live();
        for (int i = 0; i < NUM_LFOS; ++i)
            slotLfo[(size_t) i] = slotsLive && mods.uses(ModSrc::ofLfo(i));
        anyActive = on[0] || on[1] || stripLive || slotsLive;
        anyKeyed = key[0] >= 0 || key[1] >= 0 || stripKeyed || mods.keyed();
        updateCross();
    }

    // The strip's routes as they stand with the strip where it is.
    void refreshStrip()
    {
        bool live = false, keyedNow = false, crossChanged = false;
        landing = false;
        for (int r = 0; r < NUM_STRIP; ++r)
        {
            const auto i = (size_t) r;
            // (A strip that is brought down at once - the drum kit does not smooth it - is still
            // let go of over a few milliseconds by a voice that sounds: landing.)
            const bool down = stripPosition <= 0.0f && sGlide[i] != 0.0f && sounding();
            landing = landing || (sUsed[i] && down);
            sOn[i] = sUsed[i] && (stripPosition > 0.0f || down);
            sOut[i] = sOn[i] && ! sClosing[i] ? stripPosition * sAmount[i] : 0.0f;
            if (! sOn[i])
                sGlide[i] = 0.0f;       // at rest
            sKey[i] = sOn[i] ? sSetKey[i] : -1;
            const int c = sOn[i] ? sSetCross[i] : 0;
            crossChanged = crossChanged || c != sCross[i];
            sCross[i] = c;
            live = live || sOn[i];
            keyedNow = keyedNow || sKey[i] >= 0;
        }
        stripLive = live || stripVibrato != 0.0f;
        stripKeyed = keyedNow;
        anyActive = on[0] || on[1] || stripLive || slotsLive;
        anyKeyed = key[0] >= 0 || key[1] >= 0 || stripKeyed || mods.keyed();
        if (crossChanged)
            updateCross();
    }

    // The LFOs and routes that have let go (their output has come to zero) take their new
    // settings; now: all of them, whether they have or not.
    void finishClosing(bool now)
    {
        bool left = false;
        for (int i = 0; i < NUM_LFOS; ++i)
            if (closing[(size_t) i])
            {
                if (now || glide[(size_t) i] == 0.0f)
                {
                    closing[(size_t) i] = false;
                    glide[(size_t) i] = 0.0f;
                    applyNow(i, pending[(size_t) i], pendingRestart[(size_t) i]);
                }
                else
                    left = true;
            }
        for (int r = 0; r < NUM_STRIP; ++r)
            if (sClosing[(size_t) r])
            {
                if (now || sGlide[(size_t) r] == 0.0f)
                {
                    sClosing[(size_t) r] = false;
                    sGlide[(size_t) r] = 0.0f;
                    applyStripNow(r, sPending[(size_t) r]);
                }
                else
                    left = true;
            }
        if (mods.isClosing())
        {
            mods.finishClosing(now);
            if (mods.takeChanged())
                refreshSlots();
            left = left || mods.isClosing();
        }
        anyClosing = left;
    }

    float movedByStrip(int r, float v) const
    {
        const float o = sGlide[(size_t) r] * sSpan[(size_t) r];
        v = sLog[(size_t) r] ? v * std::exp2(o) : v + o;
        return std::clamp(v, sLo[(size_t) r], sHi[(size_t) r]);
    }

    float moved(int i, float v) const
    {
        const float o = glide[(size_t) i] * span[(size_t) i];
        v = isLog[(size_t) i] ? v * std::exp2(o) : v + o;
        return std::clamp(v, lo[(size_t) i], hi[(size_t) i]);
    }

    std::array<LFO, NUM_LFOS> lfos;
    VoiceModSlots mods;
    std::array<bool, NUM_LFOS> slotLfo {};      // a slot reads this LFO: it runs whether or not it has a destination
    bool slotsLive = false;
    const float* userShape = nullptr;
    std::array<float, NUM_LFOS> out {};
    std::array<float, NUM_LFOS> absDepth {};
    std::array<int, NUM_LFOS> dest {};
    std::array<bool, NUM_LFOS> on {};
    std::array<int, NUM_LFOS> key { { -1, -1 } };
    std::array<int, NUM_LFOS> cross {};
    std::array<float, NUM_LFOS> lo {}, hi {}, span {}, baseValue {};
    std::array<bool, NUM_LFOS> isLog {};
    bool anyKeyed = false, anyCross = false;
    // The strip's routes: as set (sUsed, sSetKey, sSetCross) and as they are with the strip where it is.
    std::array<int, NUM_STRIP> sDest {}, sSetCross {}, sCross {};
    std::array<int, NUM_STRIP> sSetKey { { -1, -1, -1 } }, sKey { { -1, -1, -1 } };
    std::array<float, NUM_STRIP> sAmount {}, sAbs {}, sOut {}, sLo {}, sHi {}, sSpan {}, sBase {};
    std::array<bool, NUM_STRIP> sUsed {}, sOn {}, sLog {};
    float stripPosition = 0.0f, stripVibrato = 0.0f;
    int stripPitchDest = -1;
    bool stripLive = false, stripKeyed = false;
    bool anyActive = false;
    bool sampledAhead = false;
    int ticks = 0;
    double sampleRate = 44100.0;
    dsp::GainSlew gainSlew, panSlew;

    // The outputs on a lead (see run) and the settings waiting for an LFO or route to let go.
    static constexpr double GLIDE_SECONDS = 0.004, STRIP_GLIDE_SECONDS = 0.005;
    float glideStep = 0.01f, stripStep = 0.005f;
    std::array<float, NUM_LFOS> glide {};
    std::array<float, NUM_STRIP> sGlide {};
    std::array<bool, NUM_LFOS> edged {}, catchUp {}, closing {}, pendingRestart {};
    std::array<bool, NUM_STRIP> sClosing {};
    std::array<Settings, NUM_LFOS> pending {};
    std::array<StripRoute, NUM_STRIP> sPending {};
    bool fresh = true, anyClosing = false, wasSounding = false, landing = false;
};

// The engine side: parameter ids, page descriptions, true values, and applying a change to every
// voice's LFOs and strip routes.
class EngineLfoSection
{
public:
    static constexpr int BASE = 158;                // LFO 1 = 158-165, LFO 2 = 166-173
    static constexpr int PER_LFO = 8;
    static constexpr int NUM_LFOS = VoiceLfos::NUM_LFOS;
    static constexpr int LFO_COUNT = PER_LFO * NUM_LFOS;
    static constexpr int MAX_DESTS = LfoDestList::MAX;

    // The STRIP page: 174 + route * 2 = DST, + 1 = AMT (three routes), 180 WVIB, 181 VRAT.
    // 182-189 are free on the engines that use this section.
    static constexpr int STRIP_BASE = BASE + LFO_COUNT;     // 174
    static constexpr int NUM_STRIP = VoiceLfos::NUM_STRIP;
    static constexpr int STRIP_VIBRATO = STRIP_BASE + NUM_STRIP * 2;        // 180
    static constexpr int STRIP_VIBRATO_RATE = STRIP_VIBRATO + 1;            // 181
    static constexpr int STRIP_COUNT = NUM_STRIP * 2 + 2;
    static constexpr int COUNT = LFO_COUNT + STRIP_COUNT;   // everything this section owns: 158-181

    static constexpr float MAX_STRIP_VIBRATO = 1.0f;        // semitones
    static constexpr float MIN_VIBRATO_RATE = 0.5f, MAX_VIBRATO_RATE = 12.0f, DEFAULT_VIBRATO_RATE = 5.5f;

    static constexpr int stripDestId(int route) { return STRIP_BASE + route * 2; }
    static constexpr int stripAmountId(int route) { return STRIP_BASE + route * 2 + 1; }
    static constexpr bool ownsLfo(int paramId) { return paramId >= BASE && paramId < BASE + LFO_COUNT; }
    static constexpr bool ownsStrip(int paramId) { return paramId >= STRIP_BASE && paramId < STRIP_BASE + STRIP_COUNT; }

    static constexpr float MIN_RATE = 0.02f;
    static constexpr float MAX_RATE = 40.0f;
    static constexpr float MAX_FADE = 5.0f;

    enum Offset
    {
        Rate = 0,       // Hz, used while Sync is off
        Depth,          // -1..1
        Shape,          // LFO::Shape index
        Trigger,        // LFO::TriggerMode index
        Sync,           // 0 / 1
        Division,       // LFO division index, used while Sync is on
        Fade,           // seconds of fade-in after each note-on
        Dest            // index into the engine's destination list, 0 = off
    };

    // destLabels: the engine's destination names (index 0 = "OFF"), static storage: a list of
    // fixed destinations only.
    EngineLfoSection(const char* const* destLabels, int numDests)
        : labels(destLabels), destCount(std::clamp(numDests, 1, MAX_DESTS))
    {
    }

    // The engine's whole list (static storage).
    explicit EngineLfoSection(const LfoDestList& dests)
        : labels(dests.labels()), destCount(std::clamp(dests.size(), 1, MAX_DESTS)), list(&dests)
    {
        // The engine's own strip routes (a parameter destination gets its set value when the
        // engine seeds its cells, or with stripBaseChanged).
        for (int r = 0; r < NUM_STRIP; ++r)
        {
            routes[(size_t) r].amount = dests.stripDefault(r).amount;
            resolveStrip(r, dests.stripDefault(r).dest, [](int) { return 0.0f; });
        }
        vibratoDepth = std::clamp(dests.stripVibratoDefaultValue(), 0.0f, MAX_STRIP_VIBRATO);
        pitchDest = dests.indexOfLabel("PTCH");
    }

    const LfoDestList* dests() const { return list; }
    // The parameter key LFO `lfo` is moving right now (-1: none, or it is off).
    int keyOf(int lfo) const
    {
        const auto& s = settings(lfo);
        return s.dest != 0 && s.depth != 0.0f ? s.key : -1;
    }

    static constexpr int id(int lfo, Offset o) { return BASE + lfo * PER_LFO + (int) o; }
    static constexpr bool ownsMod(int paramId) { return EngineModSlots::owns(paramId); }
    // The LFO and STRIP cells (158-181) and the mod slots, their count and the custom shape (700-772).
    static constexpr bool owns(int paramId) { return (paramId >= BASE && paramId < BASE + COUNT) || ownsMod(paramId); }

    int numDests() const { return destCount; }
    const VoiceLfos::Settings& settings(int lfo) const { return lfoSettings[(size_t) std::clamp(lfo, 0, NUM_LFOS - 1)]; }
    int dest(int lfo) const { return settings(lfo).dest; }

    // The true current value.
    float get(int paramId) const
    {
        if (ownsMod(paramId))
            return modSlots.get(paramId);
        if (ownsStrip(paramId))
        {
            if (paramId == STRIP_VIBRATO)       return vibratoDepth;
            if (paramId == STRIP_VIBRATO_RATE)  return vibratoRate;
            const auto& r = routes[(size_t) ((paramId - STRIP_BASE) / 2)];
            return (paramId - STRIP_BASE) % 2 == 0 ? (float) r.dest : r.amount;
        }
        if (! ownsLfo(paramId))
            return 0.0f;
        const auto& s = lfoSettings[(size_t) ((paramId - BASE) / PER_LFO)];
        switch ((paramId - BASE) % PER_LFO)
        {
            case Rate:      return s.rate;
            case Depth:     return s.depth;
            case Shape:     return (float) s.shape;
            case Trigger:   return (float) s.trigger;
            case Sync:      return s.sync ? 1.0f : 0.0f;
            case Division:  return (float) s.division;
            case Fade:      return s.fade;
            case Dest:      return (float) s.dest;
            default:        return 0.0f;
        }
    }

    // Stores the (range-checked) value and applies it. forEachVoice(fn) must call fn(VoiceLfos&)
    // for every voice of the engine.
    // Returns the key of a parameter this LFO was moving and no longer is (-1 if none): an engine
    // that pushes values into its voices (VoiceLfos::forEachKeyed) puts that parameter's set
    // value back. baseOf(key) gives the set value of a parameter (only needed by such engines).
    template <typename ForEachVoice>
    int set(int paramId, float value, const ForEachVoice& forEachVoice)
    {
        return set(paramId, value, forEachVoice, [](int) { return 0.0f; });
    }

    template <typename ForEachVoice, typename BaseOf>
    int set(int paramId, float value, const ForEachVoice& forEachVoice, const BaseOf& baseOf)
    {
        if (ownsMod(paramId))
            return setMod(paramId, value, forEachVoice, baseOf);
        if (ownsStrip(paramId))
            return setStrip(paramId, value, forEachVoice, baseOf);
        if (! ownsLfo(paramId))
            return -1;
        const int lfo = (paramId - BASE) / PER_LFO;
        auto& s = lfoSettings[(size_t) lfo];
        bool restart = false;
        const int keyBefore = keyOf(lfo);

        switch ((paramId - BASE) % PER_LFO)
        {
            case Rate:      s.rate = std::clamp(value, MIN_RATE, MAX_RATE); break;
            case Depth:     s.depth = std::clamp(value, -1.0f, 1.0f); break;
            case Shape:     s.shape = std::clamp((int) std::lround(value), 0, (int) LFO::Shape::COUNT - 1); break;
            case Trigger:
            {
                const int t = std::clamp((int) std::lround(value), 0, (int) LFO::TriggerMode::COUNT - 1);
                restart = t != s.trigger;
                s.trigger = t;
                break;
            }
            case Sync:      s.sync = value > 0.5f; break;
            case Division:  s.division = std::clamp((int) std::lround(value), 0, LFO::NUM_DIVISIONS - 1); break;
            case Fade:      s.fade = std::clamp(value, 0.0f, MAX_FADE); break;
            case Dest:
                s.dest = std::clamp((int) std::lround(value), 0, destCount - 1);
                s.key = -1;
                s.cross = 0;
                if (list != nullptr)
                {
                    const auto& e = (*list)[s.dest];
                    s.key = e.key;
                    s.lo = e.lo;
                    s.hi = e.hi;
                    s.log = e.log;
                    s.cross = e.cross;
                    if (s.key >= 0)
                        s.base = baseOf(s.key);
                }
                break;
            default:        return -1;
        }

        const VoiceLfos::Settings copy = s;
        bool closing = false;
        forEachVoice([lfo, &copy, restart, &closing](VoiceLfos& v) { v.apply(lfo, copy, restart); closing = closing || v.isClosing(); });
        letGo = letGo || closing;

        const int keyNow = keyOf(lfo);
        if (keyBefore >= 0 && keyBefore != keyNow)
            noteLetGo(keyBefore);
        return keyBefore >= 0 && keyBefore != keyNow && keyBefore != keyOf(1 - lfo) ? keyBefore : -1;
    }

    // ---- The mod strip ---------------------------------------------------------------------
    const VoiceLfos::StripRoute& stripRoute(int route) const { return routes[(size_t) std::clamp(route, 0, NUM_STRIP - 1)]; }
    // The strip as the voices have it now (0 = at rest).
    float stripPosition() const { return stripNow; }
    // True while the strip is up and has something to do: the voices' LFO path must run.
    bool stripOn() const
    {
        if (stripNow <= 0.0f)
            return false;
        if (vibratoDepth > 0.0f)
            return true;
        for (auto& r : routes)
            if (r.dest != 0 && r.amount != 0.0f)
                return true;
        return false;
    }
    // What the strip adds to fixed destination `dest` right now (the sum of its routes there, as
    // the voices read it with amount()).
    float stripAmount(int dest) const
    {
        float a = 0.0f;
        if (stripNow > 0.0f)
            for (auto& r : routes)
                if (r.dest == dest && dest != 0)
                    a += stripNow * r.amount * r.scale;
        return a;
    }

    // Once per block, before the voices run: `position` is the strip, 0..1, already smoothed.
    // released(key) is called for each parameter destination a route lets go of when the strip
    // comes to rest (an engine that pushes values into its voices puts the set value back).
    template <typename ForEachVoice, typename Released>
    void stripBlock(float position, int numSamples, double sampleRate, const ForEachVoice& forEachVoice, const Released& released)
    {
        if (position <= 0.0f && stripNow <= 0.0f)
            return;                                 // at rest, and the voices know it
        float vibrato = 0.0f;
        if (position > 0.0f && vibratoDepth > 0.0f)
        {
            vibratoPhase += (double) vibratoRate * numSamples / std::max(sampleRate, 1.0);
            vibratoPhase -= std::floor(vibratoPhase);
            vibrato = position * vibratoDepth * std::sin((float) vibratoPhase * 6.2831853f);
        }
        const bool landed = position <= 0.0f;
        stripNow = std::max(0.0f, position);
        const float now = stripNow;
        const int pitch = pitchDest;
        forEachVoice([now, vibrato, pitch](VoiceLfos& v) { v.setStrip(now, vibrato, pitch); });
        if (landed)
            for (auto& r : routes)
                if (r.key >= 0 && r.dest != 0 && r.amount != 0.0f)
                    released(r.key);
    }

    template <typename ForEachVoice>
    void stripBlock(float position, int numSamples, double sampleRate, const ForEachVoice& forEachVoice)
    {
        stripBlock(position, numSamples, sampleRate, forEachVoice, [](int) {});
    }

    // The set value of parameter `key` changed (engines that push only).
    template <typename ForEachVoice>
    void baseChanged(int key, float value, const ForEachVoice& forEachVoice)
    {
        for (int r = 0; r < NUM_STRIP; ++r)
        {
            auto& route = routes[(size_t) r];
            if (route.key != key || key < 0 || route.base == value)
                continue;
            route.base = value;
            const VoiceLfos::StripRoute copy = route;
            forEachVoice([r, &copy](VoiceLfos& v) { v.applyStrip(r, copy); });
        }
        for (int lfo = 0; lfo < NUM_LFOS; ++lfo)
        {
            auto& s = lfoSettings[(size_t) lfo];
            if (s.key != key || key < 0 || s.base == value)
                continue;
            s.base = value;
            const VoiceLfos::Settings copy = s;
            forEachVoice([lfo, &copy](VoiceLfos& v) { v.apply(lfo, copy, false); });
        }
        if (anySlotOn && key >= 0)
            for (int slot = 0; slot < EngineModSlots::MAX; ++slot)
            {
                auto& r = slotSettings[(size_t) slot];
                if (r.target.kind != ModSlotTarget::Keyed || r.target.index != key || r.target.base == value)
                    continue;
                r.target.base = value;
                const VoiceModSlots::Settings copy = r;
                forEachVoice([slot, &copy](VoiceLfos& v) { v.applySlot(slot, copy); });
            }
    }

    template <typename ForEachVoice>
    void setTempo(float bpm, const ForEachVoice& forEachVoice)
    {
        for (auto& s : lfoSettings)
            s.tempo = bpm;
        applyAll(forEachVoice);
    }

    // Applies every stored value (when the engine's voices exist; also after they are rebuilt).
    template <typename ForEachVoice>
    void applyAll(const ForEachVoice& forEachVoice) const
    {
        for (int lfo = 0; lfo < NUM_LFOS; ++lfo)
        {
            const VoiceLfos::Settings copy = lfoSettings[(size_t) lfo];
            forEachVoice([lfo, &copy](VoiceLfos& v) { v.apply(lfo, copy, false); });
        }
        for (int r = 0; r < NUM_STRIP; ++r)
        {
            const VoiceLfos::StripRoute copy = routes[(size_t) r];
            forEachVoice([r, &copy](VoiceLfos& v) { v.applyStrip(r, copy); });
        }
        const float now = stripNow;
        const int pitch = pitchDest;
        forEachVoice([now, pitch](VoiceLfos& v) { v.setStrip(now, 0.0f, pitch); });
        const float* shape = modSlots.shape();
        forEachVoice([shape](VoiceLfos& v) { v.setUserShape(shape); });
        for (int slot = 0; slot < EngineModSlots::MAX; ++slot)
        {
            const VoiceModSlots::Settings copy = slotSettings[(size_t) slot];
            forEachVoice([slot, &copy](VoiceLfos& v) { v.applySlot(slot, copy); });
        }
    }

    // End of a block: keeps the LFOs of voices that did not sound (or stopped part-way) in time.
    template <typename ForEachVoice>
    void endBlock(int numSamples, const ForEachVoice& forEachVoice) const
    {
        bool closing = false;
        forEachVoice([numSamples, &closing](VoiceLfos& v) { v.endBlock(numSamples); closing = closing || v.isClosing(); });
        letGo = closing;
    }

    // For engines that push values into their voices: once every voice has let go of the cells an
    // LFO or route was taken off, restore(key) is called for each (a voice that stopped sounding
    // part-way was left holding a value on the way back). Call after endBlock.
    template <typename Restore>
    void afterLetGo(const Restore& restore)
    {
        if (letGo || numLetGo == 0)
            return;
        const int n = numLetGo;
        numLetGo = 0;
        for (int i = 0; i < n; ++i)
            restore(letGoKeys[(size_t) i]);
    }

    // True while an LFO has a destination and a depth, and for the few milliseconds a voice needs
    // to let go of one that has just been switched off (VoiceLfos::apply).
    bool anyOn() const
    {
        if (letGo || anySlotOn)
            return true;
        for (auto& s : lfoSettings)
            if (s.dest != 0 && s.depth != 0.0f)
                return true;
        return false;
    }

    // The LFO1 and LFO2 pages. Pass other labels to name the destinations differently (same count).
    std::array<ParamPage, NUM_LFOS> makePages(const char* const* destLabels = nullptr) const
    {
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto C = ParamKind::Choice;
        static const char* const names[NUM_LFOS] = { "LFO1", "LFO2" };
        const char* const* dests = destLabels != nullptr ? destLabels : labels;
        std::array<ParamPage, NUM_LFOS> out;
        for (int i = 0; i < NUM_LFOS; ++i)
        {
            const int b = BASE + i * PER_LFO;
            out[(size_t) i].name = names[i];
            out[(size_t) i].section = "MOD";
            out[(size_t) i].params = {{
                { "RATE", b + Rate, MIN_RATE, MAX_RATE, 2.0f, G, "Hz" },
                { "DPTH", b + Depth, -1, 1, 0, L },
                { "SHPE", b + Shape, 0, (float) ((int) LFO::Shape::COUNT - 1), 0, C, "", LFO::shapeLabels(), (int) LFO::Shape::COUNT },
                { "TRIG", b + Trigger, 0, (float) ((int) LFO::TriggerMode::COUNT - 1), 0, C, "", LFO::triggerLabels(), (int) LFO::TriggerMode::COUNT },
                { "SYNC", b + Sync, 0, 1, 0, C, "", ParamChoices::offOn, 2 },
                { "DIV",  b + Division, 0, (float) (LFO::NUM_DIVISIONS - 1), (float) LFO::DEFAULT_DIVISION, C, "", LFO::divisionLabels(), LFO::NUM_DIVISIONS },
                { "FADE", b + Fade, 0, MAX_FADE, 0, L, "s" },
                { "DEST", b + Dest, 0, (float) (destCount - 1), 0, C, "", dests, destCount } }};
        }
        return out;
    }

    // The STRIP page (the same destination names as the LFO pages; defaults from the engine's list).
    ParamPage makeStripPage(const char* const* destLabels = nullptr) const
    {
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto C = ParamKind::Choice;
        static const char* const destNames[NUM_STRIP] = { "DST1", "DST2", "DST3" };
        static const char* const amountNames[NUM_STRIP] = { "AMT1", "AMT2", "AMT3" };
        const char* const* dests = destLabels != nullptr ? destLabels : labels;
        ParamPage page;
        page.name = "STRIP";
        page.section = "MOD";
        for (int r = 0; r < NUM_STRIP; ++r)
        {
            const LfoDestList::StripDefault d = list != nullptr ? list->stripDefault(r) : LfoDestList::StripDefault();
            page.params[(size_t) (r * 2)] = { destNames[r], stripDestId(r), 0, (float) (destCount - 1), (float) d.dest, C, "", dests, destCount };
            page.params[(size_t) (r * 2 + 1)] = { amountNames[r], stripAmountId(r), -1, 1, d.amount, L };
        }
        page.params[6] = { "WVIB", STRIP_VIBRATO, 0, MAX_STRIP_VIBRATO, list != nullptr ? list->stripVibratoDefaultValue() : 0.0f, L, "st", nullptr, 0, 0.01f };
        page.params[7] = { "VRAT", STRIP_VIBRATO_RATE, MIN_VIBRATO_RATE, MAX_VIBRATO_RATE, DEFAULT_VIBRATO_RATE, G, "Hz" };
        return page;
    }

    // STRIP, then LFO1 and LFO2 (which stay an engine's last two pages).
    void appendPages(ParamPages& pages, const char* const* destLabels = nullptr) const
    {
        pages.push_back(makeStripPage(destLabels));
        for (auto& p : makePages(destLabels))
            pages.push_back(p);
    }

    // ---- The mod slots -----------------------------------------------------------------------
    // True while a slot is in use: the engine then gives every voice its block sources
    // (VoiceLfos::setBlockSources) before the voices run.
    bool slotsOn() const { return anySlotOn; }
    const EngineModSlots& slots() const { return modSlots; }
    int numSlotDests() const { return destCount + NUM_SLOT_EXTRA + EngineModSlots::MAX; }
    // The names of a slot's DEST cell for the engine's list with these labels (the same count).
    const EngineModSlots::DestNames& slotDestNames(const char* const* destLabels = nullptr) const
    {
        static const char* const extra[NUM_SLOT_EXTRA] = { "L1RT", "L1DP", "L2RT", "L2DP" };
        return EngineModSlots::destNames(destLabels != nullptr ? destLabels : labels, destCount, extra, NUM_SLOT_EXTRA);
    }

    // SLT1 ... and LSHP, in front of the STRIP page (an engine's last pages stay STRIP, LFO1,
    // LFO2). Call with the pages the engine shows, after appendPages.
    void insertModPages(ParamPages& pages, const char* const* destLabels = nullptr) const
    {
        const auto& names = slotDestNames(destLabels);
        modSlots.insertPages(pages, "STRIP", names.text.data(), names.count, ModSrc::BASIC_COUNT);
    }

    // The "every voice's LFOs" argument for set / applyAll / endBlock, for a container of voices
    // that each have getLfos().
    template <typename Voices>
    static auto each(Voices& voices)
    {
        return [&voices](auto&& fn) { for (auto& v : voices) fn(v.getLfos()); };
    }

    static const char* name(int paramId)
    {
        static const char* const names[COUNT] = {
            "LFO1 Rate", "LFO1 Depth", "LFO1 Shape", "LFO1 Trigger", "LFO1 Sync", "LFO1 Division", "LFO1 Fade", "LFO1 Dest",
            "LFO2 Rate", "LFO2 Depth", "LFO2 Shape", "LFO2 Trigger", "LFO2 Sync", "LFO2 Division", "LFO2 Fade", "LFO2 Dest",
            "Strip 1 Dest", "Strip 1 Amount", "Strip 2 Dest", "Strip 2 Amount", "Strip 3 Dest", "Strip 3 Amount",
            "Strip Vibrato", "Strip Vibrato Rate" };
        if (ownsMod(paramId))
        {
            // (Kept for as long as the app runs: the callers want a C string.)
            static const std::array<juce::String, EngineParamRange::FOURTH_END - EngineParamRange::FOURTH_BASE> modNames = []
            {
                std::array<juce::String, EngineParamRange::FOURTH_END - EngineParamRange::FOURTH_BASE> n;
                for (int i = 0; i < (int) n.size(); ++i)
                    n[(size_t) i] = EngineModSlots::name(EngineParamRange::FOURTH_BASE + i);
                return n;
            }();
            return modNames[(size_t) (paramId - EngineParamRange::FOURTH_BASE)].toRawUTF8();
        }
        return owns(paramId) ? names[paramId - BASE] : "";
    }

private:
    template <typename BaseOf>
    void resolveStrip(int r, int dest, const BaseOf& baseOf)
    {
        auto& route = routes[(size_t) r];
        route.dest = std::clamp(dest, 0, destCount - 1);
        route.key = -1;
        route.cross = 0;
        route.scale = 1.0f;
        if (list != nullptr)
        {
            const auto& e = (*list)[route.dest];
            route.key = e.key;
            route.lo = e.lo;
            route.hi = e.hi;
            route.log = e.log;
            route.cross = e.cross;
            route.scale = e.stripScale;
            if (route.key >= 0)
                route.base = baseOf(route.key);
        }
    }

    // A STRIP cell. Returns the key of a parameter the route was moving and no longer is (-1 if none).
    template <typename ForEachVoice, typename BaseOf>
    int setStrip(int paramId, float value, const ForEachVoice& forEachVoice, const BaseOf& baseOf)
    {
        if (paramId == STRIP_VIBRATO)
        {
            vibratoDepth = std::clamp(value, 0.0f, MAX_STRIP_VIBRATO);
            if (vibratoDepth <= 0.0f)
            {
                // No vibrato left to bring in: the voices drop what the last block gave them.
                const float now = stripNow;
                const int pitch = pitchDest;
                forEachVoice([now, pitch](VoiceLfos& v) { v.setStrip(now, 0.0f, pitch); });
            }
            return -1;
        }
        if (paramId == STRIP_VIBRATO_RATE)
        {
            vibratoRate = std::clamp(value, MIN_VIBRATO_RATE, MAX_VIBRATO_RATE);
            return -1;
        }
        const int r = (paramId - STRIP_BASE) / 2;
        auto& route = routes[(size_t) r];
        const int keyBefore = route.dest != 0 && route.amount != 0.0f ? route.key : -1;
        if ((paramId - STRIP_BASE) % 2 == 0)
            resolveStrip(r, (int) std::lround(value), baseOf);
        else
            route.amount = std::clamp(value, -1.0f, 1.0f);
        const VoiceLfos::StripRoute copy = route;
        bool closing = false;
        forEachVoice([r, &copy, &closing](VoiceLfos& v) { v.applyStrip(r, copy); closing = closing || v.isClosing(); });
        letGo = letGo || closing;
        const int keyNow = route.dest != 0 && route.amount != 0.0f ? route.key : -1;
        if (keyBefore >= 0 && keyBefore != keyNow)
            noteLetGo(keyBefore);
        return keyBefore >= 0 && keyBefore != keyNow ? keyBefore : -1;
    }

    // ---- The mod slots -----------------------------------------------------------------------
    static constexpr int NUM_SLOT_EXTRA = 4;        // L1RT L1DP L2RT L2DP

    // What a slot's DEST index is (see the top of the file).
    ModSlotTarget slotTarget(int slot, int dest) const
    {
        ModSlotTarget t;
        if (dest <= 0)
            return t;
        if (dest < destCount)
        {
            t.kind = ModSlotTarget::Fixed;
            t.index = dest;
            if (list != nullptr)
            {
                const auto& e = (*list)[dest];
                if (e.cross != LfoDestList::None)
                {
                    t.kind = e.cross == LfoDestList::OtherRate ? ModSlotTarget::LfoRate : ModSlotTarget::LfoDepth;
                    t.index = -1;       // both LFOs, as from the strip
                }
                else if (e.key >= 0)
                {
                    t.kind = ModSlotTarget::Keyed;
                    t.index = e.key;
                    t.lo = e.lo;
                    t.hi = e.hi;
                    t.log = e.log && e.lo > 0.0f;
                    t.span = t.log ? 0.5f * std::log2(e.hi / e.lo) : e.hi - e.lo;
                }
            }
            return t;
        }
        const int extra = dest - destCount;
        if (extra < NUM_SLOT_EXTRA)
        {
            t.kind = extra % 2 == 0 ? ModSlotTarget::LfoRate : ModSlotTarget::LfoDepth;
            t.index = extra / 2;
            return t;
        }
        const int other = extra - NUM_SLOT_EXTRA;
        if (other < modSlots.acting() && other != slot)
        {
            t.kind = ModSlotTarget::SlotAmount;
            t.index = other;
        }
        return t;
    }

    // Slot `slot` as its voices should have it: nothing when it is beyond the slots in use.
    template <typename BaseOf>
    VoiceModSlots::Settings resolveSlot(int slot, const BaseOf& baseOf) const
    {
        VoiceModSlots::Settings r;
        if (slot >= modSlots.acting())
            return r;
        const auto& m = modSlots.slot(slot);
        r.src = m.src;
        r.via = m.via;
        r.amount = m.amount;
        r.target = slotTarget(slot, m.dest);
        if (r.target.kind == ModSlotTarget::Keyed)
            r.target.base = baseOf(r.target.index);
        const int amountDest = destCount + NUM_SLOT_EXTRA + slot;
        for (int other = 0; other < modSlots.acting(); ++other)
            if (other != slot && modSlots.slot(other).src != ModSrc::Off && modSlots.slot(other).dest == amountDest)
                r.modulated = true;
        return r;
    }

    static int slotKey(const VoiceModSlots::Settings& r)
    {
        return r.on() && r.target.kind == ModSlotTarget::Keyed ? r.target.index : -1;
    }

    // True while an LFO, a strip route or a slot is set to move parameter `key`.
    bool keyHeld(int key) const
    {
        for (int lfo = 0; lfo < NUM_LFOS; ++lfo)
            if (keyOf(lfo) == key)
                return true;
        for (auto& r : routes)
            if (r.key == key && r.dest != 0 && r.amount != 0.0f)
                return true;
        for (auto& r : slotSettings)
            if (slotKey(r) == key)
                return true;
        return false;
    }

    // A slot cell, the least number of slots shown or a level of the custom shape. Returns the key of a parameter a
    // slot was moving and no longer is (-1 if none), as set() does for an LFO.
    template <typename ForEachVoice, typename BaseOf>
    int setMod(int paramId, float value, const ForEachVoice& forEachVoice, const BaseOf& baseOf)
    {
        const int change = modSlots.set(paramId, value, numSlotDests(), ModSrc::BASIC_COUNT);
        if (change != EngineModSlots::SlotChanged)
            return -1;      // (the LFOs read the shape's levels where they are; 772 only shows pages)

        // Every slot whose settings are no longer what its voices have (one cell can change two
        // slots: the one it belongs to and the one whose amount that slot moves).
        int released = -1;
        bool closing = false, on = false;
        for (int slot = 0; slot < EngineModSlots::MAX; ++slot)
        {
            const VoiceModSlots::Settings now = resolveSlot(slot, baseOf);
            auto& held = slotSettings[(size_t) slot];
            on = on || now.on();
            if (now.sameAs(held))
                continue;
            const int keyBefore = slotKey(held);
            held = now;
            forEachVoice([slot, &now, &closing](VoiceLfos& v) { v.applySlot(slot, now); closing = closing || v.isClosing(); });
            if (keyBefore >= 0 && keyBefore != slotKey(now))
            {
                noteLetGo(keyBefore);
                if (released < 0)
                    released = keyBefore;
            }
        }
        anySlotOn = on;
        letGo = letGo || closing;
        return released >= 0 && ! keyHeld(released) ? released : -1;
    }

    EngineModSlots modSlots;
    std::array<VoiceModSlots::Settings, EngineModSlots::MAX> slotSettings {};
    bool anySlotOn = false;

    std::array<VoiceLfos::StripRoute, NUM_STRIP> routes {};
    float vibratoDepth = 0.0f, vibratoRate = DEFAULT_VIBRATO_RATE;
    double vibratoPhase = 0.0;
    float stripNow = 0.0f;
    int pitchDest = -1;
    const char* const* labels;
    int destCount;
    const LfoDestList* list = nullptr;
    std::array<VoiceLfos::Settings, NUM_LFOS> lfoSettings {};
    mutable bool letGo = false;     // a voice is still letting go of a destination
    std::array<int, 32> letGoKeys {};    // ... and the cells to put back when all have (afterLetGo)
    int numLetGo = 0;

    void noteLetGo(int key)
    {
        for (int i = 0; i < numLetGo; ++i)
            if (letGoKeys[(size_t) i] == key)
                return;
        if (numLetGo < (int) letGoKeys.size())
            letGoKeys[(size_t) numLetGo++] = key;
    }
};



// ============================================================================
// WavetableVoice — a single voice that reads from a wavetable.
//
// Supports optional EnvelopeChain for multi-stage envelope architectures.
// When envChainEnabled is true, the chain output replaces the simple ADSR.
// Each voice ends in the shared filter section (VoiceFilter: two filters, routing, filter FM
// from the table's own sound) and takes its note's share of the per-note spread (VoiceSpread).
// ============================================================================

class WavetableVoice
{
public:
    // LFO destinations (EngineLfoSection), in the order of the DEST cell.
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // +-12 semitones, depth squared
        LfoAmp,         // gain 0..2
        LfoPan,         // +-1
        LfoCutoff,      // +-4 octaves
        LfoResonance,   // +-0.5
        LfoPosition,    // scan position, +-1 (the whole table) around POS
        LfoSpread,      // +-1 around SPRD (0..1)
        LfoSub,         // +-1 around SUB (0..1)
        NumLfoDests
    };

    static const char* const* lfoDestLabels()
    {
        static const char* const names[NumLfoDests] = {
            "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "POS", "SPRD", "SUB" };
        return names;
    }

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        ampEnv.setSampleRate(sr);
        envChain.setSampleRate(sr);
        filter.setSampleRate(sr);
        lfos.setSampleRate(sr);
        declick.setSampleRate(sr);
        gainDeclick.setSampleRate(sr);
        positionStep = (float) (1.0 / (0.003 * sr));    // the whole table in 3 ms (a STEP of a sixteenth: 0.2 ms)
        levelStep = (float) (1.0 / (0.004 * sr));       // a level's whole range in 4 ms
    }

    void setWavetable(const Wavetable* wt) { wavetable = wt; }

    void setNote(int note, float velocity)
    {
        currentNote = note;
        currentVelocity = velocity;
        targetFrequency = dsp::midiNoteToHz(note);
        // Glide from the previous pitch only if this voice is still sounding.
        if (glideCoeff >= 1.0f || !active)
            baseFrequency = targetFrequency;
    }

    // `own`: what this note takes of the per-note spread (VoiceSpread, the ANLG page): with every
    // amount at 0 it is nothing, and the note is sample for sample what it was without the page.
    void noteOn(const VoiceSpread::Note& own = {})
    {
        const bool wasActive = active;
        if (active)
        {
            declick.arm();      // struck again while sounding: the phase restart must not jump,
            gainDeclick.arm();  // nor the step to the new velocity
        }
        else
            lfos.snapSlews();
        // An LFO on a page cell is read before the first sample (the engine sets the cell then).
        lfos.setNote(currentNote, currentVelocity);
        if (lfos.keyed()) lfos.noteOnAndSample(); else lfos.noteOn();
        // EVAR: this note's envelopes (amp, the chain's stages, the filter's) are all longer or
        // shorter by the same factor.
        if (own.timeScale != envScale)
        {
            envScale = own.timeScale;
            applyAmpParams();
            filter.setEnvTimeScale(envScale);
        }
        filter.noteOn(currentNote, currentVelocity, active, own.cutoffOctaves);
        if (envChainEnabled && envChain.getStageCount() > 0)
            envChain.noteOn();
        else
            ampEnv.noteOn();
        active = true;
        struck = true;          // the new note takes every glided value as it stands
        spreadSemis = own.semitones;
        spreadPosition = own.wave * SPREAD_POSITION;
        // (A voice struck again while it sounds is carried to its new place: the two declicks
        // above work on the voice's one signal, before the engine places it.)
        spreadPan = own.pan;
        if (! wasActive)
            spreadPanNow = spreadPan;
        spreadGain = own.gain;
        phase = (double) own.phase(0);
        phase2 = dsp::wrapPhase(0.25 + (double) own.phase(1));
        subPhase = (double) own.phase(2);
        // Every note-on starts this voice's own scan run again (like its envelope and its phase).
        scanPhase = 0.0;
        scanHeldStep = -1;
    }

    void noteOff()
    {
        if (envChainEnabled && envChain.getStageCount() > 0)
            envChain.noteOff();
        else
            ampEnv.noteOff();
        filter.noteOff();
    }

    bool isActive() const { return active; }

    void setPosition(float pos) { position = juce::jlimit(0.0f, 1.0f, pos); }
    void setInterpMode(WavetableInterpMode m) { interpMode = m; }
    void setDetune(float cents) { detuneCents = cents; }
    void setSubLevel(float lvl) { subLevel = juce::jlimit(0.0f, 1.0f, lvl); }
    void setMorphSpeed(float speed) { morphSpeed = speed; }
    void setOctave(int oct) { octaveShift = juce::jlimit(-4, 4, oct); }
    void setSpread(float s) { spread = juce::jlimit(0.0f, 1.0f, s); }          // second, detuned copy
    void setPitchOffset(float semitones) { pitchOffset = semitones; }           // tune, bend, vibrato
    void setPositionMod(float m) { positionMod = m; }                           // pressure
    void setGainMod(float g) { gainMod = g; }
    void setLevel(float l) { level = l; }                                       // the engine's LEVL
    void setMorphDepth(float d) { morphDepth = juce::jlimit(0.0f, 1.0f, d); }
    void setEnvToPosition(float a) { envToPos = juce::jlimit(-1.0f, 1.0f, a); }
    void setVelToPosition(float a) { velToPos = juce::jlimit(0.0f, 1.0f, a); }
    void setVelocitySensitivity(float s) { velSens = juce::jlimit(0.0f, 1.0f, s); }
    void setGlideTime(float seconds)
    {
        glideCoeff = seconds > 0.0005f ? 1.0f - std::exp(-3.0f / (seconds * (float) sampleRate)) : 1.0f;
    }

    // --- The track's input at audio rate (the XIN page) ---
    // mode 0 = off, 1 = PM (the input moves the read phase), 2 = POS (the input moves the table
    // position around where it stands). `depth` is XDPT with the strip's share, 0..1; the amp
    // envelope (XENV) and the velocity (XVEL) take their part of it away. Set once a block; what
    // is heard moves to it over a few milliseconds, so a mode or a depth is never jumped to.
    enum XinMode { XinOff = 0, XinPhase, XinPosition, NumXinModes };
    static constexpr float XIN_PM_CYCLES = 2.0f;        // the phase swing at full depth, in cycles each way (4 pi radians)
    void setXin(int mode, float depth, float envAmount, float velAmount)
    {
        xinMode = juce::jlimit(0, (int) NumXinModes - 1, mode);
        xinDepth = std::isfinite(depth) ? juce::jlimit(0.0f, 1.0f, depth) : 0.0f;
        xinEnv = std::isfinite(envAmount) ? juce::jlimit(0.0f, 1.0f, envAmount) : 0.0f;
        xinVel = std::isfinite(velAmount) ? juce::jlimit(0.0f, 1.0f, velAmount) : 0.0f;
    }
    // True while the input reaches this voice, or is still being faded out of it. A voice that
    // has ended holds on to nothing (its next note takes the amounts as they stand then).
    bool xinRunning() const { return xinMode != XinOff || (active && (xinPhaseNow != 0.0f || xinPositionNow != 0.0f)); }
    // The position the table was read at in the last sample (with the input's movement).
    float getReadPosition() const { return readPosition; }

    // A position beyond an end of the table comes back in from that end (mirrored): bounded, and
    // without the corner a hard stop would cut into the wave.
    static float foldPosition(float p)
    {
        if (! std::isfinite(p))
            return 0.0f;
        p = std::abs(p);
        p -= 2.0f * std::floor(p * 0.5f);
        return p > 1.0f ? 2.0f - p : p;
    }

    // --- Timed scan ---
    void setScanMode(WavetableScanMode m) { scanMode = m; }
    void setScanPassSeconds(double seconds) { scanInc = 1.0 / (juce::jmax(0.001, seconds) * sampleRate); }
    void setScanSteps(int n) { scanSteps = juce::jlimit(2, 256, n); }
    void setScanReverse(bool r) { scanReverse = r; }
    void setScanSeed(uint32_t seed) { scanRng = seed != 0 ? seed : 0x9E3779B9u; }
    double getScanPhase() const { return scanPhase; }                   // passes since the note-on
    float getEffectivePosition() const { return effectivePosition; }    // as of the last sample

    // --- EnvelopeChain support ---
    void setEnvelopeChainEnabled(bool enabled) { envChainEnabled = enabled; }
    bool isEnvelopeChainEnabled() const { return envChainEnabled; }
    EnvelopeChain& getEnvelopeChain() { return envChain; }

    // One sample. `x` is the track's input (one channel, -1..1) for the XIN modes; it is not read
    // while XIN is off.
    float process(float x = 0.0f)
    {
        if (!active || wavetable == nullptr || wavetable->numFrames == 0)
            return 0.0f;

        // LFOs: spread and sub level are moved for this sample only (the set values stay).
        const bool lfoOn = lfos.anyOn();
        const float setSpreadValue = spread, setSubValue = subLevel;
        if (lfoOn)
        {
            if (lfos.wantsEnvelopes())
                lfos.setEnvelopes(ampEnv.getCurrentLevel(), filter.getEnvelope().getCurrentLevel());
            lfos.tick();
            lfoApplied = true;
            lfoPosition = lfos.amount(LfoPosition);
            lfoSemis = lfos.pitchSemitones(LfoPitch);
            spread = juce::jlimit(0.0f, 1.0f, spread + lfos.amount(LfoSpread));
            subLevel = juce::jlimit(0.0f, 1.0f, subLevel + lfos.amount(LfoSub));
            filter.setLfoOctaves(lfos.amount(LfoCutoff) * VoiceLfos::CUTOFF_OCTAVES);
            filter.setResonanceMod(lfos.amount(LfoResonance) * VoiceLfos::RESONANCE_RANGE);
            lfoGain = lfos.gain(LfoAmp);
            lfoPan = juce::jlimit(-1.0f, 1.0f, lfos.panAmount(LfoPan));
        }
        else if (lfoApplied)
        {
            lfoPosition = 0.0f;
            lfoSemis = 0.0f;
            filter.setLfoOctaves(0.0f);
            filter.setResonanceMod(0.0f);
            lfoGain = 1.0f;
            lfoPan = 0.0f;
            lfos.snapSlews();
            lfoApplied = false;
        }

        // The timed scan sets the base position (the knob's value when it is off).
        const bool scanning = scanMode != WavetableScanMode::Off;
        const float basePosition = scanning ? advanceScan() : position;

        // Auto-morph position
        if (morphSpeed > 0.0f)
        {
            morphPhase += morphSpeed / (float) sampleRate;
            if (morphPhase >= 1.0f)
                morphPhase = (float) dsp::wrapPhase((double) morphPhase);
            // Sinusoidal LFO modulation of position
            effectivePosition = basePosition + 0.5f * morphDepth * std::sin(morphPhase * 2.0f * juce::MathConstants<float>::pi);
            if (! scanning)
                effectivePosition = effectivePosition - std::floor(effectivePosition); // wrap 0-1
        }
        else
        {
            effectivePosition = basePosition;
        }
        // Envelope, velocity and pressure move the scan position.
        float posOffset = positionMod + envToPos * lastEnv + velToPos * currentVelocity + lfoPosition + spreadPosition;
        if (posOffset != 0.0f || scanning)
            effectivePosition = juce::jlimit(0.0f, 1.0f, effectivePosition + posOffset);

        // Nothing that is set or stepped under a sounding note is jumped to (a jump of the frame
        // read, or of a level, is a click): the position, the sub level, the spread coming in or
        // going out, velocity's share, pressure's and the LEVL cell move on at a limited rate: the
        // whole table in three milliseconds, a level's whole range in four. A change that is slower than that - any knob sweep,
        // any smooth LFO, a running scan - passes exactly. STEP and RAND still step to the ear.
        // The first sample of a note takes everything as it stands.
        const float velGain = (1.0f - velSens + velSens * currentVelocity) * spreadGain;
        if (spreadPanNow != spreadPan)
            spreadPanNow = dsp::towards(spreadPanNow, spreadPan, levelStep);
        const bool first = struck;
        if (struck)
        {
            struck = false;
            positionNow = effectivePosition;
            subNow = subLevel;
            spreadMix = spread > 0.0f ? 1.0f : 0.0f;
            velGainNow = velGain;
            gainModNow = gainMod;
            levelNow = level;
        }
        else
        {
            positionNow = dsp::towards(positionNow, effectivePosition, positionStep);
            subNow = dsp::towards(subNow, subLevel, levelStep);
            spreadMix = dsp::towards(spreadMix, spread > 0.0f ? 1.0f : 0.0f, levelStep);
            velGainNow = dsp::towards(velGainNow, velGain, levelStep);
            gainModNow = dsp::towards(gainModNow, gainMod, 2.0f * levelStep);
            levelNow = dsp::towards(levelNow, level, levelStep);
        }
        effectivePosition = positionNow;
        readPosition = positionNow;

        // The track's input at audio rate. PM: it is added to the read phase (depth squared, up
        // to two cycles each way). POS: it moves the position around where it stands, after the
        // glide above (which would hold an audio-rate movement back), mirrored at the ends.
        double phaseOffset = 0.0;
        if (xinRunning())
        {
            const float depth = xinDepth * (1.0f - xinEnv + xinEnv * lastEnv) * (1.0f - xinVel + xinVel * currentVelocity);
            const float phaseTarget = xinMode == XinPhase ? depth : 0.0f;
            const float positionTarget = xinMode == XinPosition ? depth : 0.0f;
            if (first)
            {
                xinPhaseNow = phaseTarget;
                xinPositionNow = positionTarget;
            }
            else
            {
                xinPhaseNow = dsp::towards(xinPhaseNow, phaseTarget, levelStep);
                xinPositionNow = dsp::towards(xinPositionNow, positionTarget, levelStep);
            }
            if (! std::isfinite(x))
                x = 0.0f;
            if (xinPhaseNow != 0.0f)
                phaseOffset = (double) (xinPhaseNow * xinPhaseNow * XIN_PM_CYCLES * x);
            if (xinPositionNow != 0.0f)
                readPosition = foldPosition(positionNow + xinPositionNow * x);
        }

        if (baseFrequency != targetFrequency)
        {
            baseFrequency += (targetFrequency - baseFrequency) * (double) glideCoeff;
            if (std::abs(targetFrequency - baseFrequency) < 0.01)
                baseFrequency = targetFrequency;
        }

        // Compute frequency with detune, octave and pitch offset
        double freq = baseFrequency * std::exp2((double) octaveShift + detuneCents / 1200.0 + (pitchOffset + lfoSemis + spreadSemis) / 12.0);
        double phaseInc = freq / sampleRate;

        // The band-limited copy for this pitch
        const int mipLevel = WavetableRead::mipLevel(*wavetable, phaseInc);

        // Read from wavetable with position-based frame interpolation
        float sample = readWavetable(mipLevel, phaseOffset != 0.0 ? dsp::wrapPhase(phase + phaseOffset) : phase);

        // Spread: a second copy up to 30 cents away, mixed equally (faded in and out where the
        // spread starts or ends under a sounding note).
        if (spread > 0.0f || spreadMix > 0.0f)
        {
            const float both = (sample + readWavetable(mipLevel, phaseOffset != 0.0 ? dsp::wrapPhase(phase2 + phaseOffset) : phase2)) * 0.7071f;
            sample = spreadMix >= 1.0f ? both : sample + spreadMix * (both - sample);
            phase2 = dsp::wrapPhase(phase2 + phaseInc * std::exp2((double) spread * 30.0 / 1200.0));
        }

        // Sub oscillator: a sine one octave below
        float sub = 0.0f;
        if (subNow > 0.0f)
            sub = subNow * std::sin((float) subPhase * juce::MathConstants<float>::twoPi);
        subPhase = dsp::wrapPhase(subPhase + phaseInc * 0.5);

        // Advance phase
        phase = dsp::wrapPhase(phase + phaseInc);       // (never a loop: a huge or infinite step must not hang)

        // Envelope: use chain if enabled, otherwise simple ADSR
        float envVal;
        if (envChainEnabled && envChain.getStageCount() > 0)
        {
            envVal = envChain.process();
            if (!envChain.isActive())
                active = false;
        }
        else
        {
            envVal = ampEnv.process();
            if (!ampEnv.isActive())
                active = false;
        }

        lastEnv = envVal;
        if (lfoOn)
        {
            spread = setSpreadValue;
            subLevel = setSubValue;
        }
        // (FFM: the table's own sound moves the cutoff; the sine below it does not.)
        return gainDeclick.process(filter.process(declick.process(sample + sub), sample) * envVal * velGainNow * gainModNow * lfoGain * levelNow);
    }

    void reset()
    {
        declick.reset();
        gainDeclick.reset();
        lastEnv = 0.0f;         // ENVP must not start a note from the note before's envelope
        ampEnv.reset();
        envChain.reset();
        filter.reset();
        active = false;
        phase = 0.0;
        morphPhase = 0.0f;
        scanPhase = 0.0;
        scanHeldStep = -1;
    }

    Envelope& getAmpEnvelope() { return ampEnv; }
    // The amp envelope as set. A note's EVAR scales its times (and the chain's stages, which are
    // copies of it): what is set is kept apart, so that the scale never adds up.
    const Envelope::Params& getAmpParams() const { return ampBase; }
    void setAmpParams(const Envelope::Params& p)
    {
        ampBase = p;
        applyAmpParams();
    }
    // One value of it under a running note (an LFO on ATK ...): 0 attack, 1 decay, 2 sustain, 3 release.
    void setAmpStage(int stage, float value)
    {
        auto set = [stage, value, scale = envScale](Envelope& e)
        {
            if (stage == 0)      e.setAttack(value * scale);
            else if (stage == 1) e.setDecay(value * scale);
            else if (stage == 2) e.setSustain(value);
            else                 e.setRelease(value * scale);
        };
        (stage == 0 ? ampBase.attack : stage == 1 ? ampBase.decay : stage == 2 ? ampBase.sustain : ampBase.release) = value;
        set(ampEnv);
        for (int s = 0; s < envChain.getStageCount(); ++s)
            set(envChain.getEnvelopeMut(s));
    }
    VoiceFilter& getFilter() { return filter; }
    VoiceLfos& getLfos() { return lfos; }
    spread::Generator& getSpreadGenerator() { return spreadGenerator; }
    // -1..1: LFOs routed to the pan (as of the last processed sample) and the note's own place
    // (PVAR); the engine places the voice.
    float getPan() const { return spreadPanNow != 0.0f ? juce::jlimit(-1.0f, 1.0f, lfoPan + spreadPanNow) : lfoPan; }
    // WVAR at its top moves a note's table position by this much of the table either way.
    static constexpr float SPREAD_POSITION = 0.5f;

private:
    void applyAmpParams()
    {
        const Envelope::Params p = oldScaleTimes(ampBase, envScale);
        ampEnv.setParams(p);
        for (int s = 0; s < envChain.getStageCount(); ++s)
            envChain.getEnvelopeMut(s).setParams(p);
    }

    // One sample of the scan run: where the run is between the POS knob (0) and the end of the
    // table (1), then the base position. The first sample of a note is exactly at the start.
    float advanceScan()
    {
        const double u = scanPhase;
        scanPhase += scanInc;
        float t = 0.0f;
        switch (scanMode)
        {
            case WavetableScanMode::Run:
                t = u >= 1.0 ? 1.0f : (float) u;
                break;
            case WavetableScanMode::Loop:
                t = (float) (u - std::floor(u));
                break;
            case WavetableScanMode::Ping:
            {
                const double v = u - 2.0 * std::floor(u * 0.5);
                t = (float) (v <= 1.0 ? v : 2.0 - v);
                break;
            }
            case WavetableScanMode::Step:
            {
                const int k = u >= 1.0 ? scanSteps - 1 : juce::jmin(scanSteps - 1, (int) (u * scanSteps));
                t = (float) k / (float) (scanSteps - 1);
                break;
            }
            case WavetableScanMode::Rand:
            {
                const juce::int64 k = (juce::int64) (u * scanSteps);
                if (k != scanHeldStep)
                {
                    scanHeldStep = k;
                    // xorshift32 (Marsaglia, "Xorshift RNGs", J. Statistical Software 8(14), 2003)
                    scanRng ^= scanRng << 13;
                    scanRng ^= scanRng >> 17;
                    scanRng ^= scanRng << 5;
                    scanHeld = (float) (scanRng >> 8) * (1.0f / 16777215.0f);
                }
                t = scanHeld;
                break;
            }
            case WavetableScanMode::Off:
            case WavetableScanMode::Count:
                break;
        }
        if (scanReverse)
            t = 1.0f - t;
        float pos = position + t * (1.0f - position);
        if (scanMode == WavetableScanMode::Rand && wavetable->numFrames > 1)
        {
            const float last = (float) (wavetable->numFrames - 1);      // whole frames only
            pos = std::round(pos * last) / last;
        }
        return pos;
    }

    // The shared read-out (WavetableRead), at this voice's position and INTP.
    float readWavetable(int mipLevel, double phase) const
    {
        return wavetable != nullptr ? WavetableRead::sample(*wavetable, mipLevel, readPosition, phase, interpMode) : 0.0f;
    }

    double sampleRate = 44100.0;
    double baseFrequency = 440.0;
    double phase = 0.0;
    int currentNote = 60;
    float currentVelocity = 1.0f;
    bool active = false;

    const Wavetable* wavetable = nullptr;
    float position = 0.0f;          // 0.0 - 1.0 scan through frames
    float effectivePosition = 0.0f; // after morph modulation
    float readPosition = 0.0f;      // where the table is read: effectivePosition, moved by the input in XIN POS
    // The track's input at audio rate: what is set, and the two amounts as they are heard
    int xinMode = XinOff;
    float xinDepth = 0.0f, xinEnv = 0.0f, xinVel = 0.0f;
    float xinPhaseNow = 0.0f, xinPositionNow = 0.0f;
    WavetableInterpMode interpMode = WavetableInterpMode::Linear;
    float detuneCents = 0.0f;
    float subLevel = 0.0f;
    float morphSpeed = 0.0f;        // Hz, 0 = manual position only
    float morphPhase = 0.0f;
    int octaveShift = 0;
    float spread = 0.0f;
    float pitchOffset = 0.0f;
    float positionMod = 0.0f;
    float gainMod = 1.0f;
    float level = 1.0f;
    // The values above as they are heard: moved to, never jumped to, under a sounding note.
    bool struck = true;
    float positionNow = 0.0f, subNow = 0.0f, spreadMix = 0.0f, velGainNow = 1.0f, gainModNow = 1.0f, levelNow = 1.0f;
    float positionStep = 0.007f, levelStep = 0.005f;
    float morphDepth = 1.0f;
    float envToPos = 0.0f;
    float velToPos = 0.0f;
    float velSens = 1.0f;
    float lastEnv = 0.0f;
    float glideCoeff = 1.0f;
    double targetFrequency = 440.0;
    double phase2 = 0.25;
    double subPhase = 0.0;

    // Timed scan
    WavetableScanMode scanMode = WavetableScanMode::Off;
    double scanPhase = 0.0;         // passes since the note-on
    double scanInc = 0.0;           // passes per sample
    int scanSteps = 16;
    bool scanReverse = false;
    juce::int64 scanHeldStep = -1;
    float scanHeld = 0.0f;
    uint32_t scanRng = 0x9E3779B9u;

    Envelope ampEnv;
    Envelope::Params ampBase;   // the amp envelope as set (ampEnv holds it with the note's EVAR)
    // The per-note spread (VoiceSpread): this voice's generator, and what its present note took
    spread::Generator spreadGenerator;
    float envScale = 1.0f, spreadSemis = 0.0f, spreadPosition = 0.0f, spreadPan = 0.0f, spreadPanNow = 0.0f, spreadGain = 1.0f;
    dsp::Declick declick;       // on the oscillators, before the filter (whose state smears a jump)
    dsp::Declick gainDeclick;   // on the output

    // Filter (after the oscillators and sub, before the amp envelope)
    VoiceFilter filter;

    // LFOs
    VoiceLfos lfos;
    bool lfoApplied = false;
    float lfoPosition = 0.0f;
    float lfoSemis = 0.0f;
    float lfoGain = 1.0f;
    float lfoPan = 0.0f;

    // EnvelopeChain support — replaces ampEnv when enabled
    EnvelopeChain envChain;
    bool envChainEnabled = false;
};

// ============================================================================
// WavetableEngine — EngineBase implementation wrapping VoiceAllocator and
// WavetableVoice, with built-in factory tables and parameter interface.
// ============================================================================

class WavetableEngine : public EngineBase
{
public:
    enum Param
    {
        Position       = 0,
        InterpMode     = 1,
        Detune         = 2,
        UnisonSpread   = 3,
        SubLevel       = 4,
        MorphSpeed     = 5,
        Level          = 6,
        Octave         = 7,
        TableSelect    = 8,
        AmpAttack      = 9,
        AmpDecay       = 10,
        AmpSustain     = 11,
        AmpRelease     = 12,
        EnvChainMode   = 13,   // 0=off, 1=series, 2=parallel
        EnvChainStages = 14,   // number of envelope stages in the chain
        EnvChainCombine = 15,  // 0=sum, 1=max (for parallel mode)
        TuneParam = 16,        // semitones
        BendRangeParam = 17,   // semitones
        MorphDepth = 18,       // depth of the position motion
        EnvToPosition = 19,    // amp envelope -> scan position, -1..1
        VelToPosition = 20,    // velocity -> scan position
        VelSens = 21,          // velocity -> level
        Glide = 22,            // seconds
        Vibrato = 23,          // constant vibrato depth (the mod strip adds to it)
        UserSample = 24,       // which SampleBank entry the USER table is built from
        // The SCAN page: a timed run of the position through the table (see pages()).
        ScanMode = 25,         // WavetableScanMode index
        ScanTime = 26,         // seconds for one pass (SYNC = OFF)
        ScanSync = 27,         // 0 = TIME, 1 = LEN at the tempo, 2 = NAT (the USER sample's own length)
        ScanLength = 28,       // index into scanLengthBeats (SYNC = ON)
        ScanSteps = 29,        // index into scanStepCounts (STEP and RAND)
        ScanDirection = 30,    // 0 = forward, 1 = reverse
        ScanSpeed = 31,        // 0.25 .. 4: the pass is this many times faster (1 = 100 %)
        UserFrames = 32,       // how the USER table is cut: index into userFrameCounts, +3 = period-aligned
        // The KEYS table (WavetableKeys): pages KEYS, K SMP, K POS and K PLC. Rebuild parameters all.
        KeyAlgo = 33,          // wavemorph::Algo index: how the frames between two keys are computed
        KeyFrames = 34,        // index into WavetableKeys::frameCount
        KeyBias = 35,          // -1 .. 1
        KeyWrap = 36,          // 0 / 1
        KeyAlign = 37,         // 0 / 1
        KeyNorm = 38,          // WavetableKeys::Norm
        KeyTilt = 39,          // dB per octave
        KeyHarmonics = 40,     // highest harmonic of a key, 1 .. 127
        KeySampleBase = 41,    // 41-48: key n's sample, 0 = OFF, else SampleBank index + 1
        KeyPositionBase = 49,  // 49-56: where in its sample key n is taken, 0 .. 1
        KeyPlaceBase = 57,     // 57-64: where along the table key n sits, 0 .. 1
        // The XIN page: the track's input (TRK FX page IN) as an audio-rate modulator (see pages()).
        XInMode = 65,          // WavetableVoice::XinMode index: OFF / PM / POS (stored: append only)
        XInDepth = 66,         // 0 .. 1
        XInEnv = 67,           // how much of the depth follows the amp envelope, 0 .. 1
        XInVel = 68,           // how much of the depth follows the velocity, 0 .. 1
        XInStrip = 69,         // what the mod strip adds to the depth, 0 .. 1
        XInRectify = 70,       // 0 = the input as it is, 1 = its absolute value
        XInLowPass = 71,       // Hz; 20 kHz = open
        XInHighPass = 72,      // Hz; 20 Hz = open
        FilterBase = 100,      // 100-115: the per-voice filter (VoiceFilterSection offsets)
        FilterDualBase = 116,  // 116-124: its second filter, routing, balance and FM (the section's dual block)
        SpreadBase = 125       // 125-132: the per-note spread (VoiceSpread offsets; the ANLG page)
    };

    explicit WavetableEngine(int maxVoices = 8)
        : allocator(maxVoices)
    {
        voices.resize((size_t) maxVoices);

        // The built-in tables are shared by every engine (built once, read only); the engine owns
        // the two made from samples. USER (a recording made in the app, or a file, cut into frames)
        // and KEYS (key frames) are copies of the sine table until something is chosen.
        const auto& builtIn = WavetableFactory::builtInTables();
        for (int i = 0; i < WavetableFactory::NUM_FIRST_TABLES; ++i)
            tables.push_back(builtIn[(size_t) i]);
        userTable = *builtIn[3];
        userTable.name = "USER";
        keyTable = *builtIn[3];
        keyTable.name = "KEYS";
        tables.push_back(&userTable);
        tables.push_back(&keyTable);
        for (size_t i = (size_t) WavetableFactory::NUM_FIRST_TABLES; i < builtIn.size(); ++i)
            tables.push_back(builtIn[i]);
        jassert((int) tables.size() == NUM_TABLES);

        // Point all voices at the first table
        for (auto& v : voices)
        {
            v.setWavetable(tables[0]);
            v.setLevel(masterLevel);
        }
        setScanSeed(0x4E455054u);
        seedSpread();

        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            voices[(size_t) voiceIdx].setNote(v.note, v.velocity);
            auto& voice = voices[(size_t) voiceIdx];
            voice.noteOn(spreadSection.next(voice.getSpreadGenerator()));
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            voices[(size_t) voiceIdx].noteOff();
        });

        // A new note takes a silent voice while there is one: release tails ring out. Not with
        // GLID on: a glide starts from the pitch of a voice that still sounds, so it takes that one.
        allocator.setVoiceSoundingQuery([this](int voiceIdx)
        {
            return voices[(size_t) voiceIdx].isActive() && getParameter(Glide) <= 0.0f;
        });

        // Seed every parameter so getParameter is always the true value.
        for (auto& page : pages())
            for (auto& p : page.params)
                if (p.isValid())
                    setParameter(p.id, p.defaultVal);
        filterSection.applyAll(VoiceFilterSection::each(voices));
        lfoSection.applyAll(EngineLfoSection::each(voices));
    }

    // ---- The SCAN page ---------------------------------------------------------------------
    // MODE  OFF / RUN / LOOP / PING / STEP / RAND. Each voice starts its own run at its note-on
    //       (every note-on restarts it, exactly as it restarts the voice's envelope) and travels
    //       from the POS knob's value to the end of the table: POS is the start offset.
    //         RUN   once through, then holds the last frame
    //         LOOP  jumps back to POS and goes again
    //         PING  to the end and back
    //         STEP  once through in STEP equal plateaus (first = POS, last = the end), then holds
    //         RAND  a random whole frame between POS and the end, a new one STEP times per pass
    // TIME  seconds for one pass (SYNC = OFF).
    // SYNC  OFF: TIME.  ON: LEN at the tempo, followed live.  NAT: the natural length — one pass
    //       over the whole USER table lasts exactly as long as the sample it was built from, so RUN
    //       replays the recording at its original speed at any pitch. With POS above 0 the pass is
    //       shorter in proportion (the rest of the recording, still at its own speed). On any
    //       other table NAT has no source and uses TIME.
    // LEN   1/16 note ... 16 bars of 4 beats.
    // STEP  4 / 8 / 16 / 32 / 64 steps per pass (STEP and RAND).
    // DIR   FWD: POS -> end.  REV: end -> POS.
    // SPD   x0.25 ... x4 (25 % - 400 %): the pass is that much faster, in every SYNC setting
    //       (NAT x 0.5 = half speed at the original pitch of the key).
    // FRMS  how the USER table is cut from its sample: 64 / 128 / 256 equal pieces, or A64 / A128 /
    //       A256 = one phase-aligned period per frame (WavetableFactory::createFromSamplesEx).
    //       The table is rebuilt when this changes. 64 is the default: it is what every USER table
    //       made so far uses, so saved sounds do not change.
    //
    // How the run adds up with everything else that moves the position, per sample:
    //     base     = POS knob                       (MODE = OFF)
    //              = POS + run * (1 - POS)          (run = 0..1 from the mode and DIR)
    //     position = clamp(base + MSPD/MDPT motion + pressure + ENVP + VELP + LFOs to POS, 0, 1)
    // The run sets the base and the others add to it; the sum stops at the two ends of the table.
    // (With MODE = OFF the MSPD motion wraps around the table as it always has; under a run it is
    // clamped like the rest, so a held last frame does not jump to the first.)
    // ---- The table list ---------------------------------------------------------------------
    // TABL is an index and is stored in projects: 0-8 the first built-in tables, 9 USER, 10 KEYS,
    // 11 on the newer built-in tables in the order of wavetables::definitions(). New ones go at
    // the end.
    static constexpr int USER_TABLE = 9;
    static constexpr int KEY_TABLE = 10;
    static constexpr int FIRST_MORE_TABLE = 11;
    static constexpr int NUM_MORE_TABLES = 14;
    static constexpr int NUM_TABLES = FIRST_MORE_TABLE + NUM_MORE_TABLES;
    static const char* const* tableLabels()
    {
        static const std::array<const char*, NUM_TABLES> names = []
        {
            std::array<const char*, NUM_TABLES> n {};
            const char* const* builtIn = WavetableFactory::builtInLabels();
            jassert(WavetableFactory::builtInCount() == NUM_TABLES - 2);
            for (int i = 0; i < USER_TABLE; ++i)
                n[(size_t) i] = builtIn[i];
            n[(size_t) USER_TABLE] = "USER";
            n[(size_t) KEY_TABLE] = "KEYS";
            for (int i = USER_TABLE; i < WavetableFactory::builtInCount() && i + 2 < NUM_TABLES; ++i)
                n[(size_t) (i + 2)] = builtIn[i];
            return n;
        }();
        return names.data();
    }

    // ---- The key-frame pages ------------------------------------------------------------------
    // KEYS   ALGO  how the frames between two keys are made: LIN crossfade, STEP none (the nearer
    //              key), CLSC crossfade stored as 128 samples of 8 bits, PHAS level and phase of
    //              each harmonic separately, TRNS the energy glides along the harmonics (optimal
    //              transport), VOC envelope first, fine structure after (vocoder cross-synthesis
    //              half way). See wavemorph::morph().
    //        KFRM  frames in the table: 8 .. 256.
    //        BIAS  the frames between lean towards the earlier (-) or the later (+) key.
    //        WRAP  ON: after the last key the table returns to the first (for LOOP scans).
    //        ALGN  ON: every key is turned so that its fundamental starts like a sine.
    //        NORM  OFF as computed / PEAK every frame to full scale / RMS every frame equally loud.
    //        TILT  dB per octave on every key.  HARM  the highest harmonic of a key.
    // K SMP  SMP1-8  the sample key n is taken from; OFF = the key is not used.
    // K POS  POS1-8  where in that sample.
    // K PLC  PLC1-8  where along the table. Only the order and the spacing count: the earliest key
    //                in use is the first frame, the latest the last.
    static const char* const* keySampleNames()
    {
        static std::array<const char*, SampleBank::MAX_ENTRIES + 1> names {};
        auto& bank = SampleBank::get();
        names[0] = "OFF";
        for (int i = 0; i < bank.size() && i < SampleBank::MAX_ENTRIES; ++i)
            names[(size_t) i + 1] = bank.names()[i];
        return names.data();
    }
    static bool isKeyParam(ParamId id) { return id >= KeyAlgo && id < KeyPlaceBase + WavetableKeys::MAX_KEYS; }

    static constexpr int NUM_SCAN_LENGTHS = 9;
    static constexpr int NUM_SCAN_STEPS = 5;
    static constexpr int NUM_USER_FRAME_OPTIONS = 6;
    static float scanLengthBeats(int index)
    {
        static const float beats[NUM_SCAN_LENGTHS] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f };
        return beats[(size_t) juce::jlimit(0, NUM_SCAN_LENGTHS - 1, index)];
    }
    static int scanStepCount(int index)
    {
        static const int counts[NUM_SCAN_STEPS] = { 4, 8, 16, 32, 64 };
        return counts[(size_t) juce::jlimit(0, NUM_SCAN_STEPS - 1, index)];
    }
    static int userFrameCount(int option)
    {
        static const int counts[3] = { 64, 128, 256 };
        return counts[(size_t) (juce::jlimit(0, NUM_USER_FRAME_OPTIONS - 1, option) % 3)];
    }
    static bool userFramesAligned(int option) { return option >= 3; }

    // ---- The XIN page: the track's input as an audio-rate modulator ---------------------------
    // The input is the track's own (TRK FX page IN: SRC another track, LEVL; MODE OFF leaves it to
    // the engine). Both channels are added to one; every voice reads it sample by sample.
    // XIN   OFF: the input is not used, and the engine makes exactly the samples it made before
    //            the page existed (so does PM or POS with nothing connected).
    //       PM:  the input is added to the phase the table is read at (phase modulation): the
    //            other track's sound becomes sidebands around every harmonic of the note.
    //       POS: the input moves the table position around the place POS, the scan and the rest
    //            have brought it to: the wave changes shape at audio rate. A movement past an end
    //            of the table comes back in from that end.
    // XDPT  how far: PM up to two cycles each way (the depth is squared, so the low half of the
    //       cell is fine); POS up to the whole table each way.
    // XENV  how much of the depth follows the note's amp envelope (0 = none of it, 100 = all).
    // XVEL  the same for the note's velocity.
    // XWHL  what the mod strip adds to XDPT.
    // XRCT  0 = the input as it is; 100 = its absolute value (it only pushes one way: in POS the
    //       position only rises from where it stands; in PM the modulator sounds an octave up).
    // XLP   low-pass on the input before it modulates (20 kHz = open): a bright input makes a
    //       harsh PM. Low settings leave only the input's slow movement.
    // XHP   high-pass on the input (20 Hz = open): only its upper part modulates.
    // XDPT is not an LFO destination (the stored destination list is unchanged).
    static constexpr float XIN_LP_MIN = 20.0f, XIN_LP_OPEN = 20000.0f;
    static constexpr float XIN_HP_OPEN = 20.0f, XIN_HP_MAX = 5000.0f;

    // WAVE (table, scan position, motion), SCAN (the timed run), OSC (pitch, thickness), ANLG (the
    // per-note spread: VoiceSpread.h) and AMP (envelope, chain, level); the key-frame pages; FILTR,
    // FLT B and F ENV (the shared filter section with its dual block: VoiceFilterSection.h); XIN.
    // Still to come for this engine (phase U4): its oscillator reads on dsp/PhaseOsc.h (the include
    // cycle that stood in the way is gone: the table classes are in dsp/Wavetable.h) and its
    // modulation on the router (ModRouter.h), where the filter's KTRK / EVEL / VEL / WHL become
    // default slots (VoiceFilterSection's Routed layout) and XIN two slots.
    static const ParamPages& pages()
    {
        static const char* const scanModes[] = { "OFF", "RUN", "LOOP", "PING", "STEP", "RAND" };
        static const char* const scanSync[] = { "OFF", "ON", "NAT" };
        static const char* const scanLengths[] = { "1/16", "1/8", "1/4", "1/2", "1BAR", "2BAR", "4BAR", "8BAR", "16BR" };
        static const char* const scanSteps[] = { "4", "8", "16", "32", "64" };
        static const char* const scanDirs[] = { "FWD", "REV" };
        static const char* const userFrames[] = { "64", "128", "256", "A64", "A128", "A256" };
        static const char* const interp[] = { "LIN", "SPEC", "STEP", "VINT", "CLSC" };
        static const char* const keyAlgos[] = { "LIN", "STEP", "CLSC", "PHAS", "TRNS", "VOC" };
        static const char* const keyFrames[] = { "8", "16", "32", "64", "128", "256" };
        static const char* const offOn[] = { "OFF", "ON" };
        static const char* const keyNorms[] = { "OFF", "PEAK", "RMS" };
        static const char* const smpLabels[] = { "SMP1", "SMP2", "SMP3", "SMP4", "SMP5", "SMP6", "SMP7", "SMP8" };
        static const char* const posLabels[] = { "POS1", "POS2", "POS3", "POS4", "POS5", "POS6", "POS7", "POS8" };
        static const char* const plcLabels[] = { "PLC1", "PLC2", "PLC3", "PLC4", "PLC5", "PLC6", "PLC7", "PLC8" };
        static const char* const chain[] = { "OFF", "SER", "PAR" };
        static const char* const combine[] = { "SUM", "MAX" };
        static const char* const xinModes[] = { "OFF", "PM", "POS" };
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;
        static const ParamPages list = [&]
        {
          ParamPages l = {
            { "WAVE", "SRC", {{
                { "TABL", TableSelect, 0, NUM_TABLES - 1, 0, C, "", tableLabels(), NUM_TABLES },
                { "POS",  Position, 0, 1, 0, L },
                { "MSPD", MorphSpeed, 0, 10, 0, L, "Hz" },
                { "MDPT", MorphDepth, 0, 1, 1, L },
                { "ENVP", EnvToPosition, -1, 1, 0, L },
                { "VELP", VelToPosition, 0, 1, 0, L },
                { "INTP", InterpMode, 0, (float) ((int) WavetableInterpMode::Count - 1), 0, C, "", interp, (int) WavetableInterpMode::Count },
                { "LEVL", Level, 0, 1, 0.8f, L } }} },
            { "SCAN", "SRC", {{
                { "MODE", ScanMode, 0, (float) ((int) WavetableScanMode::Count - 1), 0, C, "", scanModes, (int) WavetableScanMode::Count },
                { "TIME", ScanTime, 0.05f, 60, 2, G, "s" },
                { "SYNC", ScanSync, 0, 2, 0, C, "", scanSync, 3 },
                { "LEN",  ScanLength, 0, NUM_SCAN_LENGTHS - 1, 4, C, "", scanLengths, NUM_SCAN_LENGTHS },
                { "STEP", ScanSteps, 0, NUM_SCAN_STEPS - 1, 2, C, "", scanSteps, NUM_SCAN_STEPS },
                { "DIR",  ScanDirection, 0, 1, 0, C, "", scanDirs, 2 },
                { "SPD",  ScanSpeed, 0.25f, 4, 1, G, "x" },
                { "FRMS", UserFrames, 0, NUM_USER_FRAME_OPTIONS - 1, 0, C, "", userFrames, NUM_USER_FRAME_OPTIONS } }} },
            { "OSC", "SRC", {{
                { "OCT",  Octave, -4, 4, 0, I },
                { "TUNE", TuneParam, -12, 12, 0, I, "st" },
                { "FINE", Detune, -100, 100, 0, L, "ct" },
                { "BEND", BendRangeParam, 0, 24, 2, I, "st" },
                { "GLID", Glide, 0, 2, 0, L, "s" },
                { "VIBR", Vibrato, 0, 1, 0, L },
                { "SPRD", UnisonSpread, 0, 1, 0, L },
                { "SUB",  SubLevel, 0, 1, 0, L } }} },
            VoiceSpread::makePage(SpreadBase),
            { "AMP", "AMP", {{
                { "ATK",  AmpAttack, 0.001f, 10, 0.01f, G, "s" },
                { "DEC",  AmpDecay, 0.001f, 10, 0.3f, G, "s" },
                { "SUS",  AmpSustain, 0, 1, 0.7f, L },
                { "REL",  AmpRelease, 0.001f, 10, 0.3f, G, "s" },
                { "VEL",  VelSens, 0, 1, 0.7f, L },
                { "CHAN", EnvChainMode, 0, 2, 0, C, "", chain, 3 },
                { "STGS", EnvChainStages, 1, 8, 2, I },
                { "COMB", EnvChainCombine, 0, 1, 0, C, "", combine, 2 } }} },
            { "KEYS", "KEYS", {{
                { "ALGO", KeyAlgo, 0, (float) ((int) wavemorph::Algo::Count - 1), 0, C, "", keyAlgos, (int) wavemorph::Algo::Count },
                { "KFRM", KeyFrames, 0, WavetableKeys::NUM_FRAME_OPTIONS - 1, 3, C, "", keyFrames, WavetableKeys::NUM_FRAME_OPTIONS },
                { "BIAS", KeyBias, -1, 1, 0, L },
                { "WRAP", KeyWrap, 0, 1, 0, C, "", offOn, 2 },
                { "ALGN", KeyAlign, 0, 1, 1, C, "", offOn, 2 },
                { "NORM", KeyNorm, 0, WavetableKeys::NumNorms - 1, WavetableKeys::NormRms, C, "", keyNorms, WavetableKeys::NumNorms },
                { "TILT", KeyTilt, -6, 6, 0, L },
                { "HARM", KeyHarmonics, 1, wavemorph::H, wavemorph::H, I } }} },
          };
          {
              ParamPage smp { "K SMP", "KEYS", {} }, pos { "K POS", "KEYS", {} }, plc { "K PLC", "KEYS", {} };
              const int bank = SampleBank::get().size();
              for (int k = 0; k < WavetableKeys::MAX_KEYS; ++k)
              {
                  smp.params[(size_t) k] = { smpLabels[k], KeySampleBase + k, 0, (float) bank, 0, C, "", keySampleNames(), bank + 1 };
                  pos.params[(size_t) k] = { posLabels[k], KeyPositionBase + k, 0, 1, defaultKeyPosition(k), L };
                  plc.params[(size_t) k] = { plcLabels[k], KeyPlaceBase + k, 0, 1, defaultKeyPlace(k), L };
              }
              l.push_back(smp);
              l.push_back(pos);
              l.push_back(plc);
          }
          VoiceFilterSection::appendPages(l, FilterBase, FilterDualBase);
          l.push_back({ "XIN", "XIN", {{
                { "XIN",  XInMode, 0, WavetableVoice::NumXinModes - 1, 0, C, "", xinModes, WavetableVoice::NumXinModes },
                { "XDPT", XInDepth, 0, 1, 0.5f, L },
                { "XENV", XInEnv, 0, 1, 0, L },
                { "XVEL", XInVel, 0, 1, 0, L },
                { "XWHL", XInStrip, 0, 1, 0, L },
                { "XRCT", XInRectify, 0, 1, 0, L },
                { "XLP",  XInLowPass, XIN_LP_MIN, XIN_LP_OPEN, XIN_LP_OPEN, G, "Hz" },
                { "XHP",  XInHighPass, XIN_HP_OPEN, XIN_HP_MAX, XIN_HP_OPEN, G, "Hz" } }} });
          EngineLfoSection(lfoDests(&l)).appendPages(l);
          return l;
        }();
        return list;
    }

    // The LFO destinations: the original eight, then the other continuous cells of the pages and
    // the other LFO. TIME, SPD (the scan) and VIBR are read once per block, the rest per sample.
    // Not offered: TABL, INTP, the scan's MODE / SYNC / LEN / STEP / DIR / FRMS, CHAN, STGS, COMB
    // (choices and counts), OCT, TUNE, FINE and BEND (PTCH is the pitch), LEVL (the level of the
    // mixed voices; AMP is each voice's), and what the filter leaves out.
    static const LfoDestList& lfoDests(const ParamPages* enginePages = nullptr)
    {
        if (enginePages == nullptr)
            pages();                    // the list is made on the way
        static const LfoDestList list = makeLfoDests(*enginePages);
        static const bool registered = (list.registerGroups(), true);
        juce::ignoreUnused(registered);
        return list;
    }

    static LfoDestList makeLfoDests(const ParamPages& p)
    {
        LfoDestList l(WavetableVoice::lfoDestLabels(), WavetableVoice::NumLfoDests);
        l.group();
        for (int id : { (int) MorphSpeed, (int) MorphDepth, (int) EnvToPosition, (int) VelToPosition })
            l.add(p, id);
        l.group();
        l.add(p, ScanTime);
        l.add(p, ScanSpeed);
        l.group();
        l.add(p, Glide);
        l.add(p, Vibrato);
        l.group();
        for (int id : { (int) AmpAttack, (int) AmpDecay, (int) AmpSustain, (int) AmpRelease, (int) VelSens })
            l.add(p, id);
        VoiceFilterSection::addLfoDests(l, p, FilterBase);
        l.addCross();
        // (After everything that was there before: a stored destination keeps its number.)
        // (VoiceFilterSection::addDualDests, written out: its overloads take the product's list types.)
        l.group();
        for (auto o : { VoiceFilterSection::Balance, VoiceFilterSection::FilterFM, VoiceFilterSection::OffsetB,
                        VoiceFilterSection::ResonanceB, VoiceFilterSection::DriveB, VoiceFilterSection::TrackB })
        {
            const int cell = FilterDualBase + (int) o - (int) VoiceFilterSection::Count;
            bool shown = false;
            for (auto& page : p)
                shown = shown || page.find(cell) != nullptr;
            if (shown)
                l.add(p, cell);
        }
        // The mod strip until the STRIP page says otherwise: 60 % of the table further on, and a
        // quarter-tone of vibrato.
        l.stripRoute(0, WavetableVoice::LfoPosition, 0.6f);
        l.stripVibrato(0.25f);
        return l;
    }

    // A key's first place in its sample and along the table: eight keys spread over both, so
    // that choosing one sample for several keys already gives a table that moves.
    static float defaultKeyPosition(int key) { return ((float) key + 0.5f) / (float) WavetableKeys::MAX_KEYS; }
    static float defaultKeyPlace(int key) { return (float) key / (float) (WavetableKeys::MAX_KEYS - 1); }

    bool isRebuildParam(ParamId id) const override { return id == UserSample || id == UserFrames || isKeyParam(id); }

    ParamPages getParamPages() const override
    {
        ParamPages out = pages();
        filterSection.updateModeCell(out);
        lfoSection.insertModPages(out);
        // The sample bank grows while the app runs (recordings).
        const int bank = SampleBank::get().size();
        for (auto& page : out)
            for (auto& p : page.params)
                if (p.id >= KeySampleBase && p.id < KeySampleBase + WavetableKeys::MAX_KEYS)
                {
                    p.choices = keySampleNames();
                    p.numChoices = bank + 1;
                    p.maxVal = (float) bank;
                }
        return out;
    }

    void pitchBend(float amount) override { bend = juce::jlimit(-1.0f, 1.0f, amount); }
    void modWheel(float value) override { wheel = juce::jlimit(0.0f, 1.0f, value); }
    void setTempo(float bpm) override
    {
        if (bpm > 0.0f)
            tempo = bpm;        // a synced scan follows it from the next block on
        lfoSection.setTempo(bpm, EngineLfoSection::each(voices));
    }

    EngineType getType() const override { return EngineType::Wavetable; }
    juce::String getName() const override { return "Wavetable"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr;
        for (auto& v : voices)
            v.setSampleRate(sr);
        applyParameter(Glide, getParameter(Glide));
        for (auto* f : { &xinLowPass, &xinHighPass })
        {
            f->setSampleRate(sr);
            f->setResonance(0.707f);
            f->reset();
        }
        xinLowPass.setMode(SVFilter::Mode::LowPass);
        xinHighPass.setMode(SVFilter::Mode::HighPass);
        xinLowPass.setCutoff(xinLowPassHz);
        xinHighPass.setCutoff(xinHighPassHz);
        xinStep = (float) (1.0 / (0.004 * std::max(sr, 1.0)));
        xinHeld = 0.0f;
        xinGain = 0.0f;
        xinTail = 0.0f;
        xinWasOn = false;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        int numSamples = buffer.getNumSamples();

        if (userReady.load(std::memory_order_relaxed) || keyReady.load(std::memory_order_relaxed))
        {
            const juce::SpinLock::ScopedTryLockType lock(userLock);
            if (lock.isLocked())
            {
                if (userReady.load(std::memory_order_relaxed))
                {
                    std::swap(userTable, userPending);      // the voices' pointers stay valid
                    userSeconds = userPendingSeconds;
                    userReady.store(false, std::memory_order_relaxed);
                }
                if (keyReady.load(std::memory_order_relaxed))
                {
                    std::swap(keyTable, keyPending);
                    keyReady.store(false, std::memory_order_relaxed);
                }
            }
        }

        // The timed scan: one pass length for the block (so SYNC follows the tempo live).
        if (scanMode != WavetableScanMode::Off)
        {
            const double seconds = getScanPassSeconds();
            for (auto& voice : voices)
                voice.setScanPassSeconds(voice.getLfos().keyed() ? getScanPassSeconds(&voice.getLfos()) : seconds);
        }

        // Once per block: tuning, bend, vibrato and each voice's pressure (scan position, volume).
        // The mod strip: the routes of the STRIP page (by default the table position and
        // vibrato), and the cutoff by the filter's WHL amount.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, sampleRate);
        lfoSection.stripBlock(wheelNow, numSamples, sampleRate, EngineLfoSection::each(voices),
                              [this](int key) { setParameter(key, getParameter(key)); });
        vibratoPhase += 5.5 * numSamples / sampleRate;
        vibratoPhase -= std::floor(vibratoPhase);
        const float vib = std::sin((float) vibratoPhase * juce::MathConstants<float>::twoPi);
        const bool slotsOn = lfoSection.slotsOn();
        for (int v = 0; v < (int) voices.size(); ++v)
        {
            float pressure = v < allocator.getVoiceCount() ? allocator.getAftertouch(v) : 0.0f;
            if (slotsOn)        // what the mod slots read once a block
                voices[(size_t) v].getLfos().setBlockSources(pressure, wheelNow, audioInput.envelope);
            AftertouchRouter::ModValues m;
            if (pressure > 0.0f)
                m = (aftertouchRouter ? *aftertouchRouter : allocator.getAftertouchRouter()).computeModulation(pressure);
            auto& voice = voices[(size_t) v];
            voice.setPositionMod(m.wavePosMod + m.timbreMod);
            voice.setGainMod(juce::jlimit(0.0f, 2.0f, 1.0f + m.volumeMod));
            voice.setPitchOffset(tune + bend * bendRange
                                 + vib * (m.vibratoMod + voice.getLfos().value(Vibrato, vibrato) * 0.5f));
            voice.getFilter().setExternalOctaves(filterSection.externalOctaves(wheelNow, m.cutoffMod));
        }

        // The track's input at audio rate (XIN): one channel, kept inside +-1 whatever arrives,
        // through XHP, XLP and XRCT. With XIN off or nothing connected the voices are told
        // "off", and once the last of them has let the input go nothing here runs. While they
        // let go (XIN turned off under a note) they still get the input, as long as it is there.
        const bool xinConnected = audioInput.connected();
        const bool xinWanted = xinMode != WavetableVoice::XinOff && xinConnected;
        bool xinOn = xinWanted;
        {
            const float depth = juce::jlimit(0.0f, 1.0f, xinDepth + xinStrip * wheelNow);
            for (auto& voice : voices)
            {
                voice.setXin(xinWanted ? xinMode : (int) WavetableVoice::XinOff, depth, xinEnv, xinVel);
                xinOn = xinOn || voice.xinRunning();
            }
        }
        const float* xinL = xinOn && xinConnected ? audioInput.left : nullptr;
        const float* xinR = xinL != nullptr && audioInput.right != nullptr ? audioInput.right : xinL;
        const int xinFrames = xinL != nullptr ? audioInput.frames : 0;
        if (xinOn && ! xinWasOn)
        {
            xinLowPass.reset();
            xinHighPass.reset();
            xinHeld = 0.0f;
            xinTail = 0.0f;
            xinGain = xinWanted ? 1.0f : 0.0f;
        }
        xinWasOn = xinOn;
        const bool xinLowPassOn = xinLowPassHz < XIN_LP_OPEN, xinHighPassOn = xinHighPassHz > XIN_HP_OPEN;

        for (int i = 0; i < numSamples; ++i)
        {
            float x = 0.0f;
            if (xinOn)
            {
                // Where the input ends (taken away, or shorter than the block) what it last gave
                // is let go of in four milliseconds (the tail), and where it comes back it is let
                // in as fast: never dropped, never jumped to.
                const bool have = i < xinFrames;
                if (! have && xinGain != 0.0f)
                {
                    xinTail = juce::jlimit(-2.0f, 2.0f, xinTail + xinHeld * xinGain);
                    xinGain = 0.0f;
                }
                if (xinTail != 0.0f)
                    xinTail = dsp::towards(xinTail, 0.0f, xinStep);
                if (have)
                {
                    x = 0.5f * (xinL[i] + xinR[i]);
                    x = std::isfinite(x) ? juce::jlimit(-1.0f, 1.0f, x) : 0.0f;
                    if (xinHighPassOn) x = xinHighPass.process(x);
                    if (xinLowPassOn)  x = xinLowPass.process(x);
                    if (xinRectify > 0.0f)
                        x += xinRectify * (std::abs(x) - x);
                    if (! std::isfinite(x))
                    {
                        x = 0.0f;
                        xinLowPass.reset();
                        xinHighPass.reset();
                    }
                    x = juce::jlimit(-2.0f, 2.0f, x);
                    xinHeld = x;
                    if (xinGain != 1.0f)
                    {
                        xinGain = dsp::towards(xinGain, 1.0f, xinStep);
                        x *= xinGain;
                    }
                }
                if (xinTail != 0.0f)
                    x += xinTail;
            }

            // An LFO routed to PAN moves its own voice between the channels (balance law: the
            // centre is unchanged, so a sound without that routing is exactly as before).
            float sampleL = 0.0f, sampleR = 0.0f;
            for (auto& voice : voices)
            {
                if (! voice.isActive())
                    continue;
                // LFOs on page cells: this voice's own value of each (as of its last sample).
                if (voice.getLfos().keyed())
                    voice.getLfos().forEachKeyed([&voice](int key, float value) { applyVoiceParam(voice, key, value); });
                const float s = voice.process(x);
                const float pan = voice.getPan();
                sampleL += pan > 0.0f ? s * (1.0f - pan) : s;
                sampleR += pan < 0.0f ? s * (1.0f + pan) : s;
            }

            sampleL *= 0.7f;        // LEVL is applied by each voice (so that a note takes it whole)
            sampleR *= 0.7f;

            if (right)
            {
                left[i] += sampleL;
                right[i] += sampleR;
            }
            else
            {
                left[i] += 0.5f * (sampleL + sampleR);
            }
        }

        lfoSection.endBlock(numSamples, EngineLfoSection::each(voices));
        lfoSection.afterLetGo([this](int key) { setParameter(key, getParameter(key)); });
    }

    void releaseResources() override
    {
        for (auto& v : voices)
            v.reset();
    }

    void noteOn(int note, float velocity, int /*voiceIndex*/) override
    {
        allocator.noteOn(note, velocity);
    }

    void noteOff(int note, int /*voiceIndex*/) override
    {
        allocator.noteOff(note);
    }

    void allNotesOff() override
    {
        soundCut();
        allocator.allNotesOff();
        for (auto& v : voices)
            v.reset();
    }

    void aftertouch(int note, float pressure) override
    {
        allocator.setAftertouch(note, pressure);
    }

    // ---- Per-stage envelope curves (StageCurveParam): the amp envelope is envelope slot 0 -------
    int getStageCurveParam(ParamId cellId) const override
    {
        if (cellId == AmpAttack)  return StageCurveParam::id(0, StageCurveParam::Attack);
        if (cellId == AmpDecay)   return StageCurveParam::id(0, StageCurveParam::Decay);
        if (cellId == AmpRelease) return StageCurveParam::id(0, StageCurveParam::Release);
        return filterSection.stageCurveParamFor(cellId);
    }

private:
    StageCurves stageCurves;

    void setStageCurve(ParamId id, float value)
    {
        stageCurves.set(id, value);
        if (StageCurveParam::envelopeOf(id) != 0)
            return;
        for (auto& v : voices)
        {
            auto p = v.getAmpParams();
            stageCurves.applyTo(p, 0);
            v.setAmpParams(p);      // (and the chain's stages, which are copies of this envelope)
        }
    }

public:
    void setParameter(ParamId id, float value) override
    {
        if (filterSection.owns(id))
        {
            if (filterSection.swaps(id, value))
                soundSwapped();
            filterSection.set(id, value, VoiceFilterSection::each(voices));
            lfoSection.baseChanged(id, filterSection.get(id), EngineLfoSection::each(voices));
            return;
        }
        if (spreadSection.owns(id))
        {
            spreadSection.set(id, value);
            if (spreadSection.reseeds(id))
                seedSpread();
            return;
        }
        if (lfoSection.owns(id))
        {
            // A cell an LFO has let go of goes back to its set value on every voice.
            const int released = lfoSection.set(id, value, EngineLfoSection::each(voices),
                                                [this](int key) { return getParameter(key); });
            if (released >= 0)
                setParameter(released, getParameter(released));
            return;
        }
        if (StageCurveParam::owns(id))
        {
            setStageCurve(id, value);
            return;
        }
        if ((id == TableSelect || id == InterpMode || id == EnvChainMode || id == EnvChainStages || id == EnvChainCombine)
            && params[id] != value)
            soundSwapped();
        params[id] = value;
        applyParameter(id, value);
        lfoSection.baseChanged(id, value, EngineLfoSection::each(voices));
    }

    float getParameter(ParamId id) const override
    {
        if (filterSection.owns(id))
            return filterSection.get(id);
        if (spreadSection.owns(id))
            return spreadSection.get(id);
        if (lfoSection.owns(id))
            return lfoSection.get(id);
        if (StageCurveParam::owns(id))
            return stageCurves.get(id);
        auto it = params.find(id);
        return it != params.end() ? it->second : 0.0f;
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (filterSection.owns(id))
            return VoiceFilterSection::name(filterSection.offsetOf(id));
        if (spreadSection.owns(id))
            return VoiceSpread::name(id - SpreadBase);
        if (EngineLfoSection::owns(id))
            return EngineLfoSection::name(id);
        switch (id)
        {
            case Position:       return "Position";
            case InterpMode:     return "Interp";
            case Detune:         return "Detune";
            case UnisonSpread:   return "Spread";
            case SubLevel:       return "Sub";
            case MorphSpeed:     return "Morph";
            case Level:          return "Level";
            case Octave:         return "Octave";
            case TableSelect:    return "Table";
            case AmpAttack:      return "Attack";
            case AmpDecay:       return "Decay";
            case AmpSustain:     return "Sustain";
            case AmpRelease:     return "Release";
            case EnvChainMode:   return "Env Chain";
            case EnvChainStages: return "Chain Stages";
            case EnvChainCombine: return "Chain Combine";
            case ScanMode:       return "Scan Mode";
            case ScanTime:       return "Scan Time";
            case ScanSync:       return "Scan Sync";
            case ScanLength:     return "Scan Length";
            case ScanSteps:      return "Scan Steps";
            case ScanDirection:  return "Scan Direction";
            case ScanSpeed:      return "Scan Speed";
            case UserFrames:     return "User Frames";
            case KeyAlgo:        return "Key Algorithm";
            case KeyFrames:      return "Key Table Frames";
            case KeyBias:        return "Key Bias";
            case KeyWrap:        return "Key Wrap";
            case KeyAlign:       return "Key Align";
            case KeyNorm:        return "Key Level";
            case KeyTilt:        return "Key Tilt";
            case KeyHarmonics:   return "Key Harmonics";
            case XInMode:        return "Input Mod";
            case XInDepth:       return "Input Depth";
            case XInEnv:         return "Input Depth Envelope";
            case XInVel:         return "Input Depth Velocity";
            case XInStrip:       return "Input Depth Strip";
            case XInRectify:     return "Input Rectify";
            case XInLowPass:     return "Input Low-pass";
            case XInHighPass:    return "Input High-pass";
            default:             break;
        }
        if (id >= KeySampleBase && id < KeySampleBase + WavetableKeys::MAX_KEYS)
            return "Key " + juce::String(id - KeySampleBase + 1) + " Sample";
        if (id >= KeyPositionBase && id < KeyPositionBase + WavetableKeys::MAX_KEYS)
            return "Key " + juce::String(id - KeyPositionBase + 1) + " Position";
        if (id >= KeyPlaceBase && id < KeyPlaceBase + WavetableKeys::MAX_KEYS)
            return "Key " + juce::String(id - KeyPlaceBase + 1) + " Place";
        return "Param " + juce::String(id);
    }

    int getParameterCount() const override { return 24; }

    // Access for tests and advanced use
    VoiceAllocator& getAllocator() { return allocator; }
    WavetableVoice& getVoice(int index) { return voices[(size_t) index]; }
    int getTableCount() const { return (int) tables.size(); }
    const Wavetable& getTable(int index) const { return *tables[(size_t) index]; }

    // The USER table: the sample is cut into frames (FRMS: 64 equal pieces unless set otherwise),
    // so POS scans through the recording. Built on the calling (message) thread; the audio thread
    // swaps it in, with the sample's duration (the natural scan length), at the start of its next
    // block and does nothing else: no allocation, and only a try-lock.
    void buildUserTable(int bankIndex)
    {
        const auto* e = SampleBank::get().entry(bankIndex);
        if (e == nullptr || e->left.size() < 64)
            return;
        std::vector<float> mono(e->left);
        if (e->right.size() == mono.size())
            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] = 0.5f * (mono[i] + e->right[i]);
        buildUserTableFrom(mono.data(), (int) mono.size(), e->sampleRate);
    }

    // The same from any mono buffer (the bank is not involved).
    void buildUserTableFrom(const float* mono, int numSamples, double sourceRate)
    {
        if (mono == nullptr || numSamples < 64)
            return;
        if (sourceRate <= 0.0)
            sourceRate = 44100.0;
        Wavetable wt = userFrameOption == 0
            ? WavetableFactory::createFromSamples(mono, numSamples, 64)
            : WavetableFactory::createFromSamplesEx(mono, numSamples, sourceRate, userFrameCount(userFrameOption),
                                                    userFramesAligned(userFrameOption));
        wt.name = "USER";
        const juce::SpinLock::ScopedLockType lock(userLock);
        userPending = std::move(wt);
        userPendingSeconds = (double) numSamples / sourceRate;
        userReady.store(true);
    }
    bool userTablePending() const { return userReady.load(); }

    // ---- The KEYS table ---------------------------------------------------------------------
    // Built from the key cells whenever one of them changes while the table is in use, or when
    // the table is chosen after they changed; on the calling (message) thread, and handed to the
    // audio thread exactly as the USER table is. A key's cycle is taken from its sample once and
    // kept until its sample or its position changes, so turning any other cell only recomputes
    // the frames.
    bool keyTablePending() const { return keyReady.load(); }
    int getKeyCount() const
    {
        int n = 0;
        for (int k = 0; k < WavetableKeys::MAX_KEYS; ++k)
            if (SampleBank::get().entry(keySample[(size_t) k] - 1) != nullptr)
                ++n;
        return n;
    }
    // Whether key n's cycle was taken as one period of a pitched sound (as of the last build).
    bool isKeyPitched(int key) const { return key >= 0 && key < WavetableKeys::MAX_KEYS && keyCache[(size_t) key].pitched; }
    const WavetableKeys::Settings& getKeySettings() const { return keySettings; }

    void buildKeyTable()
    {
        std::vector<WavetableKeys::Key> keys;
        for (int k = 0; k < WavetableKeys::MAX_KEYS; ++k)
        {
            auto& cache = keyCache[(size_t) k];
            const int bankIndex = keySample[(size_t) k] - 1;
            const auto* e = SampleBank::get().entry(bankIndex);
            if (e == nullptr)
            {
                cache.sample = -1;
                cache.pitched = false;
                continue;
            }
            if (cache.sample != bankIndex || cache.position != keyPosition[(size_t) k])
            {
                cache.sample = bankIndex;
                cache.position = keyPosition[(size_t) k];
                cache.valid = WavetableKeys::extract(e->left.data(), e->right.size() == e->left.size() ? e->right.data() : nullptr,
                                                     (int) e->left.size(), e->sampleRate, cache.position,
                                                     cache.spectrum, &cache.pitched);
            }
            if (cache.valid)
                keys.push_back({ cache.spectrum, keyPlace[(size_t) k] });
        }
        setKeyTable(WavetableKeys::build(std::move(keys), keySettings));
    }

    // The same from waves given directly (tests; the bank is not involved).
    void buildKeyTableFrom(std::vector<WavetableKeys::Key> keys)
    {
        setKeyTable(WavetableKeys::build(std::move(keys), keySettings));
    }

    // Duration of the sample behind the USER table that is playing (0 until one has been built).
    double getUserSampleSeconds() const { return userSeconds; }

    // Seconds one scan pass takes right now (TIME, LEN at the tempo or the natural length, over SPD).
    // With a voice's LFOs: for that voice, whose LFOs may be moving TIME or SPD.
    double getScanPassSeconds(const VoiceLfos* lfos = nullptr) const
    {
        double seconds = lfos != nullptr ? lfos->value(ScanTime, scanTime) : scanTime;
        if (scanSync == 2 && currentTableIndex == USER_TABLE && userSeconds > 0.0)
            seconds = userSeconds * (double) juce::jmax(0.0f, 1.0f - scanStart);
        else if (scanSync == 1)
            seconds = (double) scanLengthBeats(scanLength) * 60.0 / (double) tempo;
        return seconds / (double) (lfos != nullptr ? lfos->value(ScanSpeed, scanSpeed) : scanSpeed);
    }

    // The per-note spread: every voice's generator starts its sequence again, from the SEED cell
    // and the voice's number.
    void seedSpread()
    {
        for (size_t v = 0; v < voices.size(); ++v)
            voices[v].getSpreadGenerator().seed(spreadSection.seed(), (int) v);
    }

    // RAND draws from one generator per voice; the same seed gives the same jumps.
    void setScanSeed(uint32_t seed)
    {
        for (size_t v = 0; v < voices.size(); ++v)
            voices[v].setScanSeed(seed + (uint32_t) v * 0x9E3779B9u);
    }

    void selectTable(int index)
    {
        if (index >= 0 && index < (int) tables.size())
        {
            currentTableIndex = index;
            for (auto& v : voices)
                v.setWavetable(tables[(size_t) index]);
            if (index == KEY_TABLE && keyDirty)
                buildKeyTable();
        }
    }

private:
    void setKeyTable(Wavetable wt)
    {
        keyDirty = false;
        const juce::SpinLock::ScopedLockType lock(userLock);
        keyPending = std::move(wt);
        keyReady.store(true);
    }

    // A key cell changed: the table is rebuilt now if it is the one playing, otherwise when it
    // is next chosen (a project that sets thirty key cells on a track playing SAW builds nothing).
    void keysChanged()
    {
        keyDirty = true;
        if (currentTableIndex == KEY_TABLE)
            buildKeyTable();
    }

    template <typename T>
    void setKeyValue(T& member, T value)
    {
        if (member == value)
            return;
        member = value;
        keysChanged();
    }

    void applyKeyParameter(ParamId id, float value)
    {
        const int whole = (int) std::lround(value);
        if (id >= KeySampleBase && id < KeySampleBase + WavetableKeys::MAX_KEYS)
            setKeyValue(keySample[(size_t) (id - KeySampleBase)], juce::jlimit(0, SampleBank::MAX_ENTRIES, whole));
        else if (id >= KeyPositionBase && id < KeyPositionBase + WavetableKeys::MAX_KEYS)
            setKeyValue(keyPosition[(size_t) (id - KeyPositionBase)], juce::jlimit(0.0f, 1.0f, value));
        else if (id >= KeyPlaceBase && id < KeyPlaceBase + WavetableKeys::MAX_KEYS)
            setKeyValue(keyPlace[(size_t) (id - KeyPlaceBase)], juce::jlimit(0.0f, 1.0f, value));
        else switch (id)
        {
            case KeyAlgo:      setKeyValue(keySettings.algo, static_cast<wavemorph::Algo>(juce::jlimit(0, (int) wavemorph::Algo::Count - 1, whole))); break;
            case KeyFrames:    setKeyValue(keySettings.frames, WavetableKeys::frameCount(whole)); break;
            case KeyBias:      setKeyValue(keySettings.bias, juce::jlimit(-1.0f, 1.0f, value)); break;
            case KeyWrap:      setKeyValue(keySettings.wrap, value > 0.5f); break;
            case KeyAlign:     setKeyValue(keySettings.align, value > 0.5f); break;
            case KeyNorm:      setKeyValue(keySettings.norm, juce::jlimit(0, WavetableKeys::NumNorms - 1, whole)); break;
            case KeyTilt:      setKeyValue(keySettings.tilt, juce::jlimit(-6.0f, 6.0f, value)); break;
            case KeyHarmonics: setKeyValue(keySettings.harmonics, juce::jlimit(1, wavemorph::H, whole)); break;
            default: break;
        }
    }

    // One voice's own value of a page cell: what an LFO routed to that cell sets, sample by
    // sample (TIME, SPD and VIBR are read per block instead, where the engine works them out).
    // Envelope times act on the stage that is running (the chain's stages are copies).
    static void applyVoiceParam(WavetableVoice& voice, ParamId id, float value)
    {
        if (id >= FilterBase)      // (the dual block follows the single filter's ids: one offset)
        {
            VoiceFilterSection::applyTo(voice.getFilter(), id - FilterBase, value);
            return;
        }
        switch (id)
        {
            case MorphSpeed:    voice.setMorphSpeed(value); return;
            case MorphDepth:    voice.setMorphDepth(value); return;
            case EnvToPosition: voice.setEnvToPosition(value); return;
            case VelToPosition: voice.setVelToPosition(value); return;
            case Glide:         voice.setGlideTime(value); return;
            case VelSens:       voice.setVelocitySensitivity(value); return;
            case AmpAttack: case AmpDecay: case AmpSustain: case AmpRelease: break;
            default: return;
        }
        voice.setAmpStage(id - AmpAttack, value);
    }

    static float xinCell(float value) { return std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : 0.0f; }

    void applyParameter(ParamId id, float value)
    {
        if (isKeyParam(id))
        {
            applyKeyParameter(id, value);
            return;
        }
        switch (id)
        {
            case Position:
                scanStart = juce::jlimit(0.0f, 1.0f, value);
                for (auto& v : voices)
                    v.setPosition(value);
                break;

            case ScanMode:
                scanMode = static_cast<WavetableScanMode>(juce::jlimit(0, (int) WavetableScanMode::Count - 1, (int) std::lround(value)));
                for (auto& v : voices)
                    v.setScanMode(scanMode);
                break;
            case ScanTime:       scanTime = juce::jlimit(0.05f, 60.0f, value); break;
            case ScanSync:       scanSync = juce::jlimit(0, 2, (int) std::lround(value)); break;
            case ScanLength:     scanLength = juce::jlimit(0, NUM_SCAN_LENGTHS - 1, (int) std::lround(value)); break;
            case ScanSpeed:      scanSpeed = juce::jlimit(0.25f, 4.0f, value); break;
            case ScanSteps:
                for (auto& v : voices)
                    v.setScanSteps(scanStepCount((int) std::lround(value)));
                break;
            case ScanDirection:
                for (auto& v : voices)
                    v.setScanReverse(value > 0.5f);
                break;
            case UserFrames:
            {
                const int option = juce::jlimit(0, NUM_USER_FRAME_OPTIONS - 1, (int) std::lround(value));
                if (option != userFrameOption)
                {
                    userFrameOption = option;
                    auto source = params.find(UserSample);       // rebuild the table that is in use
                    if (source != params.end())
                        buildUserTable((int) std::lround(source->second));
                }
                break;
            }

            case InterpMode:
            {
                int mode = juce::jlimit(0, (int) WavetableInterpMode::Count - 1, (int) value);
                for (auto& v : voices)
                    v.setInterpMode(static_cast<WavetableInterpMode>(mode));
                break;
            }

            case Detune:
                for (auto& v : voices)
                    v.setDetune(value);
                break;

            case UnisonSpread:
                unisonSpread = value;
                for (auto& v : voices)
                    v.setSpread(value);
                break;

            case TuneParam:      tune = value; break;
            case BendRangeParam: bendRange = value; break;
            case Vibrato:        vibrato = value; break;
            case MorphDepth:     for (auto& v : voices) v.setMorphDepth(value); break;
            case EnvToPosition:  for (auto& v : voices) v.setEnvToPosition(value); break;
            case VelToPosition:  for (auto& v : voices) v.setVelToPosition(value); break;
            case VelSens:        for (auto& v : voices) v.setVelocitySensitivity(value); break;
            case Glide:          for (auto& v : voices) v.setGlideTime(value); break;

            // (Whatever is given - a value from a file - each is held to its cell: off, or open, for not-a-number.)
            case XInMode:        xinMode = std::isfinite(value) ? juce::jlimit(0, (int) WavetableVoice::NumXinModes - 1, (int) std::lround(juce::jlimit(-1.0f, 8.0f, value))) : 0; break;
            case XInDepth:       xinDepth = xinCell(value); break;
            case XInEnv:         xinEnv = xinCell(value); break;
            case XInVel:         xinVel = xinCell(value); break;
            case XInStrip:       xinStrip = xinCell(value); break;
            case XInRectify:     xinRectify = xinCell(value); break;
            case XInLowPass:
                xinLowPassHz = std::isfinite(value) ? juce::jlimit(XIN_LP_MIN, XIN_LP_OPEN, value) : XIN_LP_OPEN;
                xinLowPass.setCutoff(xinLowPassHz);
                break;
            case XInHighPass:
                xinHighPassHz = std::isfinite(value) ? juce::jlimit(XIN_HP_OPEN, XIN_HP_MAX, value) : XIN_HP_OPEN;
                xinHighPass.setCutoff(xinHighPassHz);
                break;

            case SubLevel:
                for (auto& v : voices)
                    v.setSubLevel(value);
                break;

            case MorphSpeed:
                for (auto& v : voices)
                    v.setMorphSpeed(value);
                break;

            case Level:
                masterLevel = juce::jlimit(0.0f, 1.0f, value);
                for (auto& v : voices)
                    v.setLevel(masterLevel);
                break;

            case Octave:
                for (auto& v : voices)
                    v.setOctave((int) value);
                break;

            case TableSelect:
                selectTable((int) value);
                break;
            case UserSample:
                buildUserTable((int) std::lround(value));
                break;

            case AmpAttack:
            case AmpDecay:
            case AmpSustain:
            case AmpRelease:
            {
                for (auto& v : voices)
                {
                    auto p = v.getAmpParams();
                    if (id == AmpAttack) p.attack = value;
                    else if (id == AmpDecay) p.decay = value;
                    else if (id == AmpSustain) p.sustain = value;
                    else if (id == AmpRelease) p.release = value;
                    v.setAmpParams(p);      // (and the chain's stages, which are copies of this envelope)
                }
                break;
            }

            case EnvChainMode:
            {
                // 0 = off (use simple ADSR), 1 = series, 2 = parallel
                int modeVal = juce::jlimit(0, 2, (int) value);
                for (auto& v : voices)
                {
                    if (modeVal == 0)
                    {
                        v.setEnvelopeChainEnabled(false);
                    }
                    else
                    {
                        v.setEnvelopeChainEnabled(true);
                        auto chainMode = (modeVal == 1)
                            ? EnvelopeChain::ChainMode::Series
                            : EnvelopeChain::ChainMode::Parallel;
                        v.getEnvelopeChain().setMode(chainMode);
                    }
                }
                break;
            }

            case EnvChainStages:
            {
                // Rebuild the chain with N identical envelope stages
                // using the current ADSR parameters
                int stages = juce::jlimit(1, EnvelopeChain::MaxEnvelopes, (int) value);
                for (auto& v : voices)
                {
                    auto& chain = v.getEnvelopeChain();
                    chain.reset();
                    // Reconstruct the chain (reset clears active state, not stages;
                    // we rebuild by creating a new chain)
                    EnvelopeChain newChain;
                    newChain.setSampleRate(sampleRate);
                    newChain.setMode(chain.getMode());
                    newChain.setCombineMode(chain.getCombineMode());
                    auto p = v.getAmpEnvelope().getParams();
                    for (int s = 0; s < stages; ++s)
                        newChain.addEnvelope(p);
                    v.getEnvelopeChain() = newChain;
                }
                break;
            }

            case EnvChainCombine:
            {
                // 0 = sum, 1 = max
                auto combineMode = ((int) value == 0)
                    ? EnvelopeChain::CombineMode::Sum
                    : EnvelopeChain::CombineMode::Max;
                for (auto& v : voices)
                    v.getEnvelopeChain().setCombineMode(combineMode);
                break;
            }

            default:
                break;
        }
    }

    double sampleRate = 44100.0;
    float masterLevel = 0.8f;
    float unisonSpread = 0.0f;
    float tune = 0.0f, bend = 0.0f, bendRange = 2.0f, wheel = 0.0f, vibrato = 0.0f;
    float wheelNow = 0.0f;                              // the strip, smoothed per block
    double vibratoPhase = 0.0;
    int currentTableIndex = 0;

    // Timed scan (SCAN page)
    WavetableScanMode scanMode = WavetableScanMode::Off;
    float scanTime = 2.0f, scanSpeed = 1.0f, scanStart = 0.0f, tempo = 120.0f;
    int scanSync = 0, scanLength = 4, userFrameOption = 0;
    double userSeconds = 0.0, userPendingSeconds = 0.0;

    // The track's input at audio rate (XIN page)
    int xinMode = WavetableVoice::XinOff;
    float xinDepth = 0.5f, xinEnv = 0.0f, xinVel = 0.0f, xinStrip = 0.0f, xinRectify = 0.0f;
    float xinLowPassHz = XIN_LP_OPEN, xinHighPassHz = XIN_HP_OPEN;
    SVFilter xinLowPass, xinHighPass;           // on the input, before the voices (the project's SVF)
    float xinHeld = 0.0f, xinGain = 0.0f;       // the last input sample, and how much of the input is let in
    float xinTail = 0.0f, xinStep = 0.02f;      // what is left of an input that ended, and how fast both move
    bool xinWasOn = false;

    VoiceFilterSection filterSection { FilterBase, FilterDualBase };
    VoiceSpread spreadSection { SpreadBase };
    EngineLfoSection lfoSection { lfoDests() };

    VoiceAllocator allocator;
    std::vector<WavetableVoice> voices;
    std::vector<const Wavetable*> tables;       // TABL order; the built-in ones are shared
    Wavetable userTable, keyTable;              // the two this engine owns (their addresses never change)
    ParamMap params;
    Wavetable userPending, keyPending;
    juce::SpinLock userLock;
    std::atomic<bool> userReady { false }, keyReady { false };

    // Key frames
    struct KeyCache
    {
        int sample = -1;
        float position = -1.0f;
        bool valid = false, pitched = false;
        wavemorph::Spectrum spectrum;
    };
    std::array<int, WavetableKeys::MAX_KEYS> keySample {};
    std::array<float, WavetableKeys::MAX_KEYS> keyPosition {}, keyPlace {};
    std::array<KeyCache, WavetableKeys::MAX_KEYS> keyCache;
    WavetableKeys::Settings keySettings;
    bool keyDirty = true;
};

} // namespace oldwt
