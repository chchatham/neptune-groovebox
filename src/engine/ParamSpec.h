#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

// -----------------------------------------------------------------------------
// Parameter descriptions for the 8-encoder pages.
//
// Every engine describes its own pages (EngineBase::getParamPages): eight
// parameters per page, in encoder order (cells 1-4 on the top row of the OLED,
// 5-8 on the bottom row). The parameter page screen draws, edits and locks
// parameters purely from these descriptions, so what is on the glass is always
// what the encoder under it changes.
// -----------------------------------------------------------------------------

enum class ParamKind
{
    Linear,   // continuous, 1 % of the range per encoder step
    Log,      // continuous, exponential mapping (frequencies, times); minVal must be > 0
    Int,      // whole numbers, one per encoder step (semitones, counts)
    Choice    // named options, one per encoder step
};

// Long choice lists come in groups (an engine's LFO destinations: the original ones, then one
// group per page). A list registered here is stepped group by group with Func + turn instead of
// four entries at a time (ParamSpec::turned). The list is known by its label array, so a cell
// needs nothing more than the `choices` pointer it already has.
// add() is for the threads that build engines (never the audio thread); find() only reads.
namespace ChoiceGroups
{
    struct Entry
    {
        const char* const* choices = nullptr;
        const int* starts = nullptr;        // ascending indices at which a group begins (the first is 0)
        int count = 0;
    };
    static constexpr int MAX_LISTS = 32;

    inline std::array<Entry, MAX_LISTS>& table() { static std::array<Entry, MAX_LISTS> t {}; return t; }
    inline std::atomic<int>& used() { static std::atomic<int> n { 0 }; return n; }

    inline const Entry* find(const char* const* choices)
    {
        const int n = used().load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (table()[(size_t) i].choices == choices)
                return &table()[(size_t) i];
        return nullptr;
    }

    inline void add(const char* const* choices, const int* starts, int count)
    {
        static std::mutex lock;
        const std::lock_guard<std::mutex> guard(lock);
        const int n = used().load(std::memory_order_acquire);
        if (choices == nullptr || starts == nullptr || count <= 0 || n >= MAX_LISTS || find(choices) != nullptr)
            return;
        table()[(size_t) n] = { choices, starts, count };
        used().store(n + 1, std::memory_order_release);
    }
}

// How a knob click is applied (the control grammar: plain, Shift = fine, Func = coarse / snap).
// Same order as Screen::TurnMode; Screen::turnStep() converts.
enum class TurnStep { Plain, Fine, Coarse };

struct ParamSpec
{
    const char* label = "---";          // up to 4 characters fit a cell
    int id = -1;                        // < 0: empty cell
    float minVal = 0.0f;
    float maxVal = 1.0f;
    float defaultVal = 0.0f;
    ParamKind kind = ParamKind::Linear;
    const char* unit = "";              // "Hz", "s", "st", "ct", "x" or "" (shown as 0-100 / -100..+100)
    const char* const* choices = nullptr;
    int numChoices = 0;
    float stepSize = 0.0f;              // > 0: the value moves on this grid (Shift = 1/50 of it)

    bool isValid() const { return id >= 0; }
    bool isBipolar() const { return kind != ParamKind::Choice && minVal < 0.0f && maxVal > 0.0f; }
    bool isDiscrete() const { return kind == ParamKind::Int || kind == ParamKind::Choice; }

    float clamp(float v) const { return juce::jlimit(minVal, maxVal, v); }

    float toNorm(float v) const
    {
        v = clamp(v);
        if (maxVal <= minVal)
            return 0.0f;
        if (kind == ParamKind::Log && minVal > 0.0f)
            return std::log(v / minVal) / std::log(maxVal / minVal);
        return (v - minVal) / (maxVal - minVal);
    }

    float fromNorm(float n) const
    {
        n = juce::jlimit(0.0f, 1.0f, n);
        if (kind == ParamKind::Log && minVal > 0.0f)
            return minVal * std::pow(maxVal / minVal, n);
        float v = minVal + n * (maxVal - minVal);
        return isDiscrete() ? std::round(v) : v;
    }

    // New value after `steps` encoder steps (positive = clockwise), plain step size. Same as
    // turned(current, steps, TurnStep::Plain) except that a "bpm" value is not special-cased.
    float stepped(float current, int steps) const
    {
        if (steps == 0)
            return clamp(current);
        if (isDiscrete())
            return clamp(std::round(current) + (float) steps);
        if (stepSize > 0.0f)
            return clamp(std::round(current / stepSize + (float) steps) * stepSize);
        return clamp(fromNorm(toNorm(current) + (float) steps * 0.01f));
    }

    // =========================================================================================
    // THE stepping rule of the control grammar (docs/launchkey-mk4-control-map.md, "Step sizes"). Every
    // screen that turns a knob into a value calls this: `clicks` is the signed click count
    // (positive = clockwise), `mode` comes from the modifiers (Screen::turnStep()).
    //
    //   Plain   1 % of a continuous range (or one `stepSize`), 1 of a discrete one.
    //   Fine    a tenth of that. A discrete value cannot move by less than 1, so it moves by 1.
    //   Coarse  snaps, by what the parameter is:
    //             unit "bpm"                 +-10 (plain 1, fine 0.1), whatever the kind
    //             Choice, division list      to the next straight value (labels like "1/8T" and
    //                                        "1/8D" are skipped; a list counts as a division list
    //                                        when at least one label has a '/' and ends in T or D)
    //             Choice, a grouped list     to the start of the next / previous group (ChoiceGroups:
    //                                        the LFO destination lists), and the two ends
    //             Choice, anything else      by 4 entries
    //             unit "st" or "note"        by 12 (an octave; no move if it would leave the range,
    //                                        so the pitch class is kept)
    //             Int, anything else         to the next multiple of 4, and the two ends
    //             unit "Hz"                  by an octave (x2, /2), clamped to the range
    //             bipolar continuous         min, half-way, centre (0), half-way, max
    //             unipolar continuous        0 / 25 / 50 / 75 / 100 % of the range, and the default
    //
    // The result is always inside [minVal, maxVal].
    // =========================================================================================
    float turned(float current, int clicks, TurnStep mode) const
    {
        current = clamp(current);
        if (clicks == 0)
            return current;
        const float n = (float) clicks;

        if (unitIs("bpm"))
        {
            if (mode == TurnStep::Fine)   return clamp(std::round(current * 10.0f + n) / 10.0f);
            if (mode == TurnStep::Coarse) return clamp(current + n * 10.0f);
            return clamp(std::round(current) + n);
        }

        const bool octaves = unitIs("st") || unitIs("note");

        if (isDiscrete())
        {
            const float v = std::round(current);
            if (mode != TurnStep::Coarse)
                return clamp(v + n);

            if (kind == ParamKind::Choice)
            {
                if (const auto* groups = ChoiceGroups::find(choices))
                    return nextGroup((int) v, clicks, *groups);
                return isDivisionList() ? nextStraight((int) v, clicks) : clamp(v + n * 4.0f);
            }

            float out = v;
            for (int i = 0; i < std::abs(clicks); ++i)
            {
                if (octaves)
                {
                    const float next = out + (clicks > 0 ? 12.0f : -12.0f);
                    if (next < minVal || next > maxVal) break;
                    out = next;
                }
                else if (clicks > 0)
                    out = juce::jmin(maxVal, std::floor(out / 4.0f) * 4.0f + 4.0f);
                else
                    out = juce::jmax(minVal, std::ceil(out / 4.0f) * 4.0f - 4.0f);
            }
            return clamp(out);
        }

        if (mode == TurnStep::Plain)
            return stepped(current, clicks);

        if (mode == TurnStep::Fine)
        {
            if (stepSize > 0.0f)
                return clamp(current + n * stepSize * 0.1f);
            return clamp(fromNorm(toNorm(current) + n * 0.001f));
        }

        // Coarse, continuous.
        if (octaves)
        {
            float out = current;
            for (int i = 0; i < std::abs(clicks); ++i)
            {
                const float next = out + (clicks > 0 ? 12.0f : -12.0f);
                if (next < minVal || next > maxVal) break;
                out = next;
            }
            return out;
        }
        if (unitIs("Hz"))
            return clamp(current * std::pow(2.0f, n));

        float points[6];
        int count = 0;
        if (isBipolar())
        {
            points[count++] = minVal;
            points[count++] = minVal * 0.5f;
            points[count++] = 0.0f;
            points[count++] = maxVal * 0.5f;
            points[count++] = maxVal;
        }
        else
        {
            for (int q = 0; q <= 4; ++q)
                points[count++] = (q == 0) ? minVal : (q == 4 ? maxVal : fromNorm((float) q * 0.25f));
            points[count++] = clamp(defaultVal);
        }

        float out = current;
        for (int i = 0; i < std::abs(clicks); ++i)
        {
            const float at = toNorm(out);
            bool found = false;
            float best = out, bestNorm = 0.0f;
            for (int p = 0; p < count; ++p)
            {
                const float pn = toNorm(points[p]);
                const bool beyond = clicks > 0 ? pn > at + 1.0e-4f : pn < at - 1.0e-4f;
                if (beyond && (!found || (clicks > 0 ? pn < bestNorm : pn > bestNorm)))
                {
                    found = true;
                    best = points[p];
                    bestNorm = pn;
                }
            }
            if (!found) break;
            out = best;
        }
        return clamp(out);
    }

    bool unitIs(const char* name) const { return unit != nullptr && std::strcmp(unit, name) == 0; }

    // A triplet or dotted entry of a division list ("1/8T", "1/4D").
    static bool isOddDivision(const char* label)
    {
        if (label == nullptr) return false;
        const size_t len = std::strlen(label);
        return len > 2 && std::strchr(label, '/') != nullptr && (label[len - 1] == 'T' || label[len - 1] == 'D');
    }

    bool isDivisionList() const
    {
        if (kind != ParamKind::Choice || choices == nullptr) return false;
        for (int i = 0; i < numChoices; ++i)
            if (isOddDivision(choices[i]))
                return true;
        return false;
    }

    // The entry `clicks` straight values away from `index` in a division list.
    float nextStraight(int index, int clicks) const
    {
        const int last = juce::jmin(numChoices - 1, (int) std::lround(maxVal));
        const int first = juce::jmax(0, (int) std::lround(minVal));
        int at = juce::jlimit(first, juce::jmax(first, last), index);
        for (int i = 0; i < std::abs(clicks); ++i)
        {
            int next = at;
            do { next += clicks > 0 ? 1 : -1; }
            while (next >= first && next <= last && isOddDivision(choices[next]));
            if (next < first || next > last) break;
            at = next;
        }
        return (float) at;
    }

    // The start of the group `clicks` groups away from `index` in a grouped list; past the last
    // group it is the last entry, before the first it is the first.
    float nextGroup(int index, int clicks, const ChoiceGroups::Entry& groups) const
    {
        const int last = (int) std::lround(maxVal), first = (int) std::lround(minVal);
        int at = juce::jlimit(first, juce::jmax(first, last), index);
        for (int i = 0; i < std::abs(clicks); ++i)
        {
            int next = clicks > 0 ? last : first;
            if (clicks > 0)
            {
                for (int g = 0; g < groups.count; ++g)
                    if (groups.starts[g] > at) { next = juce::jmin(last, groups.starts[g]); break; }
            }
            else
            {
                for (int g = groups.count - 1; g >= 0; --g)
                    if (groups.starts[g] < at) { next = juce::jmax(first, groups.starts[g]); break; }
            }
            if (next == at) break;
            at = next;
        }
        return (float) at;
    }

    // At most five characters, so it fits a 32-pixel cell.
    juce::String format(float v) const
    {
        v = clamp(v);

        if (kind == ParamKind::Choice)
        {
            int i = juce::jlimit(0, juce::jmax(0, numChoices - 1), (int) std::lround(v));
            return (choices != nullptr && numChoices > 0) ? juce::String(choices[i]) : juce::String(i);
        }

        const juce::String u(unit);

        if (u == "Hz")
        {
            if (v >= 10000.0f) return juce::String((int) std::lround(v / 1000.0f)) + "k";
            if (v >= 1000.0f)  return juce::String(v / 1000.0f, 1) + "k";
            if (v >= 100.0f)   return juce::String((int) std::lround(v));
            return juce::String(v, 1);
        }

        if (u == "s")
        {
            if (v < 0.9995f) return juce::String((int) std::lround(v * 1000.0f)) + "m";
            if (v < 10.0f)   return juce::String(v, 2);
            return juce::String(v, 1);
        }

        if (kind == ParamKind::Int || u == "st" || u == "ct")
        {
            int i = (int) std::lround(v);
            if (kind != ParamKind::Int && std::abs(v - (float) i) > 0.005f)
                return juce::String(v, 2);
            return (isBipolar() && i > 0 ? "+" : "") + juce::String(i);
        }

        if (u == "x")
            return juce::String(v, v < 10.0f ? 2 : 1);

        if (u == "bpm")     // whole unless a fine turn left a decimal
            return std::abs(v - std::round(v)) < 0.05f ? juce::String((int) std::lround(v)) : juce::String(v, 1);

        if (u == "cc")   // 0..1 shown as a MIDI controller value
            return juce::String((int) std::lround(v * 127.0f));

        if (u == "dB" || u == "ms")
        {
            if (std::abs(v) >= 100.0f) return juce::String((int) std::lround(v));
            return (isBipolar() && v > 0.0f ? "+" : "") + juce::String(v, 1);
        }

        // Unit-less: show as a percentage of full scale.
        float span = juce::jmax(std::abs(minVal), std::abs(maxVal));
        int pct = (span > 0.0f) ? (int) std::lround(v / span * 100.0f) : 0;
        return (isBipolar() && pct > 0 ? "+" : "") + juce::String(pct);
    }
};

struct ParamPage
{
    juce::String name;                  // shown in the name bar, up to 5 characters
    juce::String section;               // pages sharing a section are neighbours; Up / Down jump between sections
    std::array<ParamSpec, 8> params {};

    const ParamSpec* find(int id) const
    {
        for (auto& p : params)
            if (p.id == id)
                return &p;
        return nullptr;
    }
};

using ParamPages = std::vector<ParamPage>;

namespace ParamChoices
{
    static const char* const offOn[] = { "OFF", "ON" };
}

// Pressure ranges: a parameter can have a low end and / or a high end, between which a pad's
// pressure sweeps it. They are stored with the track's parameters under these ids (LO_BASE + the
// parameter's id, HI_BASE + the parameter's id) so they are saved with the sound; they are never
// sent to an engine.
namespace RangeParam
{
    static constexpr int LO_BASE = 1000;
    static constexpr int HI_BASE = 2000;
    static constexpr int END = 3000;

    inline bool isRangeParam(int id) { return id >= LO_BASE && id < END; }
    inline int targetOf(int id) { return id >= HI_BASE ? id - HI_BASE : id - LO_BASE; }
}

// Track-level mixer parameters. They live in Track::soundParams like engine parameters but are
// applied by the AudioEngine's mixer, never forwarded to an engine (engines use low ids freely:
// the drum engine alone uses 0-127).
// Which ids in a track's parameter map belong to its engine: 0-149, and a second block 158-189
// (150-157 are the send-bus type selectors). Everything from 190 up is the mixer, effects, etc.
namespace EngineParamRange
{
    static constexpr int FIRST_END = 150;
    static constexpr int EXT_BASE = 158;
    static constexpr int EXT_END = 190;
    // Third block, 400-527: the drum kit's kick-machine page (16 pads x 8), see DrumEngine.
    static constexpr int THIRD_BASE = 400;
    static constexpr int THIRD_END = 528;
    static constexpr bool contains(int id)
    {
        return (id >= 0 && id < FIRST_END) || (id >= EXT_BASE && id < EXT_END) || (id >= THIRD_BASE && id < THIRD_END);
    }
}

// Per-stage envelope curves: attack, decay and release of an envelope each have their own curve
// amount (-1 = log .. 0 = straight .. +1 = exp, see Envelope::Params). They are stored and locked
// like any parameter but are on no page: Shift + turning a stage's own knob (ATK, DEC, REL)
// edits them (EngineBase::getStageCurveParam says which id belongs to a cell).
//
// Ids: the start of the third engine block, 400 + envelope * 3 + stage (attack, decay, release),
// where "envelope" is a slot the engine gives each of its envelopes:
//   unified voice                         0 amp, 1 filter envelope, 2-5 extra envelopes 1-4 (MENV)
//   FM                                    0-5 operators 1-6, 6 the voice filter's envelope
//   macro, wavetable, wave sequencer      0 amp, 6 the voice filter's envelope
//   sampler                               0 amp
// (The drum kit has no such envelopes; it uses the same block for its kick page.)
// Where an envelope also has a CURV cell, a stage's curve is CURV + its own amount, limited to
// -1..1: CURV bends all three together, the stage amounts bend each one on top of that.
namespace StageCurveParam
{
    static constexpr int BASE = EngineParamRange::THIRD_BASE;     // 400
    static constexpr int STAGES = 3;
    static constexpr int MAX_ENVELOPES = 8;
    static constexpr int END = BASE + MAX_ENVELOPES * STAGES;     // 424, one past the last id
    static constexpr int FILTER_ENV = 6;                          // slot of VoiceFilterSection's envelope

    enum Stage { Attack = 0, Decay, Release };

    static constexpr int id(int envelope, int stage) { return BASE + envelope * STAGES + stage; }
    static constexpr bool owns(int paramId) { return paramId >= BASE && paramId < END; }
    static constexpr int envelopeOf(int paramId) { return (paramId - BASE) / STAGES; }
    static constexpr int stageOf(int paramId) { return (paramId - BASE) % STAGES; }

    // The value Shift + turn edits: 41 positions, shown as -100..+100.
    inline ParamSpec spec(int paramId)
    {
        static const char* const labels[STAGES] = { "ACRV", "DCRV", "RCRV" };
        if (!owns(paramId))
            return {};
        return { labels[stageOf(paramId)], paramId, -1.0f, 1.0f, 0.0f, ParamKind::Linear, "", nullptr, 0, 0.05f };
    }

    // "ATK CRV": what the name bar calls the value.
    inline const char* stageName(int paramId)
    {
        static const char* const names[STAGES] = { "ATK CRV", "DEC CRV", "REL CRV" };
        return owns(paramId) ? names[stageOf(paramId)] : "";
    }
}

// The stage-curve values of one engine (all zero = straight lines).
struct StageCurves
{
    std::array<float, StageCurveParam::MAX_ENVELOPES * StageCurveParam::STAGES> values {};

    float get(int paramId) const
    {
        return StageCurveParam::owns(paramId) ? values[(size_t) (paramId - StageCurveParam::BASE)] : 0.0f;
    }
    void set(int paramId, float value)
    {
        if (StageCurveParam::owns(paramId))
            values[(size_t) (paramId - StageCurveParam::BASE)] = juce::jlimit(-1.0f, 1.0f, value);
    }
    // The curve a stage of envelope slot `envelope` has; `all` is the envelope's CURV cell.
    float of(int envelope, int stage, float all = 0.0f) const
    {
        return juce::jlimit(-1.0f, 1.0f, all + values[(size_t) (envelope * StageCurveParam::STAGES + stage)]);
    }
    // Write the three curves into an Envelope::Params.
    template <typename EnvelopeParams>
    void applyTo(EnvelopeParams& p, int envelope, float all = 0.0f) const
    {
        p.attackCurve = of(envelope, StageCurveParam::Attack, all);
        p.decayCurve = of(envelope, StageCurveParam::Decay, all);
        p.releaseCurve = of(envelope, StageCurveParam::Release, all);
    }
};

namespace MixParam
{
    static constexpr int VOLUME = 190;      // 0..1
    static constexpr int PAN = 191;         // -1..1
    static constexpr int SEND_BASE = 192;   // + bus index (0..7), 0..1
    static constexpr int MAX_SENDS = 8;

    static constexpr float DEFAULT_VOLUME = 0.8f;

    inline bool isMixParam(int id) { return id >= VOLUME && id < SEND_BASE + MAX_SENDS; }
}
