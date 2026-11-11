#pragma once

#include "ParamSpec.h"
#include "../dsp/LFO.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

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
    static constexpr int NUM_SOURCES = NUM_LFOS + NUM_STRIP;    // what keyAt() counts: the LFOs, then the strip routes
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
    }

    // restart: begin again from a new LFO (used when the trigger mode changes, so that the voices
    // of an engine are in step with each other afterwards).
    void apply(int i, const Settings& s, bool restart)
    {
        if (i < 0 || i >= NUM_LFOS)
            return;
        auto& l = lfos[(size_t) i];
        if (restart)
        {
            l = LFO();
            l.setSampleRate(sampleRate);
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
            out[(size_t) i] = 0.0f;
        anyActive = on[0] || on[1] || stripLive;

        key[(size_t) i] = on[(size_t) i] ? s.key : -1;
        lo[(size_t) i] = s.lo;
        hi[(size_t) i] = s.hi;
        isLog[(size_t) i] = s.log && s.lo > 0.0f;
        // Full depth: a linear cell's whole range, half a logarithmic one's (in octaves).
        span[(size_t) i] = isLog[(size_t) i] ? 0.5f * std::log2(s.hi / s.lo) : s.hi - s.lo;
        baseValue[(size_t) i] = s.base;
        cross[(size_t) i] = on[(size_t) i] ? s.cross : 0;
        anyKeyed = key[0] >= 0 || key[1] >= 0 || stripKeyed;
        updateCross();
    }

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
    void applyStrip(int r, const StripRoute& s)
    {
        if (r < 0 || r >= NUM_STRIP)
            return;
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

    // ---- Parameter destinations ------------------------------------------------------------
    // True when an LFO in use is routed to a parameter destination.
    bool keyed() const { return anyKeyed; }
    // True when an LFO in use is routed to parameter k.
    bool has(int k) const
    {
        return anyKeyed && (key[0] == k || key[1] == k
                            || (stripKeyed && (sKey[0] == k || sKey[1] == k || sKey[2] == k)));
    }

    // The parameter source i is moving (-1: none): 0 and 1 are the LFOs, then the strip routes.
    int keyAt(int i) const
    {
        i = std::clamp(i, 0, NUM_SOURCES - 1);
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
    }

    // True when at least one LFO has a destination and a depth.
    bool anyOn() const { return anyActive; }

    // Note-on: see LFO::trigger for what each trigger mode does with it.
    void noteOn()
    {
        for (auto& l : lfos)
            l.trigger();
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
        ticks = 0;
        if (idle <= 0 || ! anyActive)
            return;
        for (int i = 0; i < NUM_LFOS; ++i)
        {
            if (! on[(size_t) i])
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
            || (stripLive && ((sOn[0] && sDest[0] == d) || (sOn[1] && sDest[1] == d) || (sOn[2] && sDest[2] == d)));
    }

    // Sum of the LFOs (and strip routes) routed to destination d, -1..1 each (0 if none is).
    float amount(int d) const
    {
        const float a = (dest[0] == d ? out[0] : 0.0f) + (dest[1] == d ? out[1] : 0.0f);
        if (! stripLive)
            return a;
        return a + (sDest[0] == d ? sOut[0] : 0.0f) + (sDest[1] == d ? sOut[1] : 0.0f) + (sDest[2] == d ? sOut[2] : 0.0f);
    }

    // The same for a pitch destination, in semitones (depth squared, see the top of the file).
    float pitchSemitones(int d) const
    {
        const float a = ((dest[0] == d ? out[0] * absDepth[0] : 0.0f)
                       + (dest[1] == d ? out[1] * absDepth[1] : 0.0f)) * PITCH_SEMITONES;
        if (! stripLive)
            return a;
        return a + ((sDest[0] == d ? sOut[0] * sAbs[0] : 0.0f) + (sDest[1] == d ? sOut[1] * sAbs[1] : 0.0f)
                    + (sDest[2] == d ? sOut[2] * sAbs[2] : 0.0f)) * PITCH_SEMITONES
                 + (d == stripPitchDest ? stripVibrato : 0.0f);
    }

    LFO& getLfo(int i) { return lfos[(size_t) std::clamp(i, 0, NUM_LFOS - 1)]; }
    float getOutput(int i) const { return out[(size_t) std::clamp(i, 0, NUM_LFOS - 1)]; }

private:
    void run()
    {
        if (on[0]) out[0] = lfos[0].process();
        if (on[1]) out[1] = lfos[1].process();
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
        const bool crossed = cross[0] != 0 || cross[1] != 0 || sCross[0] != 0 || sCross[1] != 0 || sCross[2] != 0;
        if (anyCross || crossed)
            for (auto& lfo : lfos)
            {
                lfo.setRateMod(0.0f);
                lfo.setDepthMod(0.0f);
            }
        anyCross = crossed;
    }

    // The strip's routes as they stand with the strip where it is.
    void refreshStrip()
    {
        bool live = false, keyedNow = false, crossChanged = false;
        for (int r = 0; r < NUM_STRIP; ++r)
        {
            const auto i = (size_t) r;
            sOn[i] = sUsed[i] && stripPosition > 0.0f;
            sOut[i] = sOn[i] ? stripPosition * sAmount[i] : 0.0f;
            sKey[i] = sOn[i] ? sSetKey[i] : -1;
            const int c = sOn[i] ? sSetCross[i] : 0;
            crossChanged = crossChanged || c != sCross[i];
            sCross[i] = c;
            live = live || sOn[i];
            keyedNow = keyedNow || sKey[i] >= 0;
        }
        stripLive = live || stripVibrato != 0.0f;
        stripKeyed = keyedNow;
        anyActive = on[0] || on[1] || stripLive;
        anyKeyed = key[0] >= 0 || key[1] >= 0 || stripKeyed;
        if (crossChanged)
            updateCross();
    }

    float movedByStrip(int r, float v) const
    {
        const float o = sOut[(size_t) r] * sSpan[(size_t) r];
        v = sLog[(size_t) r] ? v * std::exp2(o) : v + o;
        return std::clamp(v, sLo[(size_t) r], sHi[(size_t) r]);
    }

    float moved(int i, float v) const
    {
        const float o = out[(size_t) i] * span[(size_t) i];
        v = isLog[(size_t) i] ? v * std::exp2(o) : v + o;
        return std::clamp(v, lo[(size_t) i], hi[(size_t) i]);
    }

    std::array<LFO, NUM_LFOS> lfos;
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
    static constexpr bool owns(int paramId) { return paramId >= BASE && paramId < BASE + COUNT; }

    int numDests() const { return destCount; }
    const VoiceLfos::Settings& settings(int lfo) const { return lfoSettings[(size_t) std::clamp(lfo, 0, NUM_LFOS - 1)]; }
    int dest(int lfo) const { return settings(lfo).dest; }

    // The true current value.
    float get(int paramId) const
    {
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
        forEachVoice([lfo, &copy, restart](VoiceLfos& v) { v.apply(lfo, copy, restart); });

        const int keyNow = keyOf(lfo);
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
    }

    // End of a block: keeps the LFOs of voices that did not sound (or stopped part-way) in time.
    template <typename ForEachVoice>
    void endBlock(int numSamples, const ForEachVoice& forEachVoice) const
    {
        if (! anyOn())
            return;
        forEachVoice([numSamples](VoiceLfos& v) { v.endBlock(numSamples); });
    }

    bool anyOn() const
    {
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
        forEachVoice([r, &copy](VoiceLfos& v) { v.applyStrip(r, copy); });
        const int keyNow = route.dest != 0 && route.amount != 0.0f ? route.key : -1;
        return keyBefore >= 0 && keyBefore != keyNow ? keyBefore : -1;
    }

    std::array<VoiceLfos::StripRoute, NUM_STRIP> routes {};
    float vibratoDepth = 0.0f, vibratoRate = DEFAULT_VIBRATO_RATE;
    double vibratoPhase = 0.0;
    float stripNow = 0.0f;
    int pitchDest = -1;
    const char* const* labels;
    int destCount;
    const LfoDestList* list = nullptr;
    std::array<VoiceLfos::Settings, NUM_LFOS> lfoSettings {};
};
