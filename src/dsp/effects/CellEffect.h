#pragma once

#include "EffectBase.h"
#include <algorithm>
#include <array>
#include <cmath>

// An effect as a table of cells: the one mechanism every effect built on a shared core uses (the
// modulation effects, the delays and reverbs of DelayCore.h / ReverbParts.h, the dynamics and
// equalisers of Detector.h / EqBand.h).
//
//   the table    cells() names eight cells, four on the FX page and four on FX2; the slot's WET is
//                a ninth for modulation. The table gives the page convention once (specs, set,
//                read back truly, held to the range, lists and counts rounded, what is not a
//                number not taken);
//   modulation   EffectBase::modulateCell(cell, offset) adds a share of the cell's range along
//                the cell's own curve (moved()); the cell reads as it was set;
//   the clock    every CONTROL samples, counted from reset() and whatever the block size, the
//                cells are looked at: when one is worth something new, configure() turns all of
//                them into the core's settings, on the audio thread. Then controlCore() steps
//                what the core glides or sweeps (its LFOs step on this count too: FxLfo.h), and
//                runCore() plays the samples up to the next look;
//   the mix      dry (1 - mix) + wet mix. Wherever a new mix comes from (the MIX cell, the slot's
//                WET, modulation) it is gone to in a straight line, a full swing in
//                MIX_SWING_SECONDS: small moves arrive within one look, a jump is not a click.
//                In an EffectChain the chain does this (one mix for every effect, EffectChain.h):
//                the core is then run at mix 1 and gives its wet sound alone.
//
// A core with nothing waiting for the clock (at rest, or one that counts its own steps) is run a
// block at a time: the looks inside the block would find nothing to do (coasting()).
//
// What an effect writes itself: the table, configure(), and its core. A cell's own glide (a
// time, a level, a frequency) is the core's, since only the core knows what gliding it means.
class CellEffect : public EffectBase
{
public:
    static constexpr int CELLS = 4 + MAX_EXTRAS;
    static constexpr int WET = WET_CELL;                // modulateCell's ninth: the slot's mix
    static constexpr int TARGETS = CELLS + 1;
    static constexpr int CONTROL = 32;
    static constexpr float MIX_SWING_SECONDS = 0.004f;

    // An input no sound has is held to this level (36 dB over full scale); what is not a number
    // is silence. The base does it to every sample before a core sees it (processBlock, clock):
    // a core is handed numbers within the limit and holds no guard of its own.
    static constexpr float IN_LIMIT = 64.0f;
    static float sane(float x)
    {
        return (x >= -IN_LIMIT && x <= IN_LIMIT) ? x : (x > IN_LIMIT ? IN_LIMIT : (x < -IN_LIMIT ? -IN_LIMIT : 0.0f));
    }
    // A stretch of input, in place: looked through once (which costs next to nothing), and put
    // right only if something in it is out of bounds.
    static void saneBlock(float* left, float* right, int n)
    {
        bool fit = true;
        for (int i = 0; i < n; ++i)
            fit = fit & (std::abs(left[i]) <= IN_LIMIT) & (std::abs(right[i]) <= IN_LIMIT);
        if (fit) return;
        for (int i = 0; i < n; ++i)
        {
            left[i] = sane(left[i]);
            right[i] = sane(right[i]);
        }
    }
    // A sample rate an effect can be built for, whatever it was given.
    static double goodRate(double sr) { return (sr >= 1000.0 && sr <= 1.0e6) ? sr : 44100.0; }

    // What modulateCell is handed, made safe: -1..1, and 0 for what is not a number.
    static float offsetOf(float offset)
    {
        return std::isfinite(offset) ? (offset < -1.0f ? -1.0f : (offset > 1.0f ? 1.0f : offset)) : 0.0f;
    }

    // A cell's value moved by a modulation offset: a share of the cell's range, -1..1, along the
    // cell's own scale (a time or a frequency moves by ratio), held to the range. Lists and
    // counts are not moved. An offset of 0 is the value itself, to the bit.
    static float moved(const ParamSpec& spec, float value, float offset)
    {
        offset = offsetOf(offset);
        if (offset == 0.0f || ! spec.isValid() || spec.isDiscrete() || ! (spec.maxVal > spec.minVal))
            return value;
        if (spec.kind == ParamKind::Log && spec.minVal > 0.0f)
            return std::clamp(value * std::pow(spec.maxVal / spec.minVal, offset), spec.minVal, spec.maxVal);
        return std::clamp(value + offset * (spec.maxVal - spec.minVal), spec.minVal, spec.maxVal);
    }

    // ---- the page convention ------------------------------------------------------------------
    ParamSpec getParamSpec(int index) const override { return index >= 0 && index < 4 ? specs[index] : ParamSpec(); }
    ParamSpec getExtraSpec(int index) const override { return index >= 0 && index < MAX_EXTRAS ? specs[4 + index] : ParamSpec(); }
    void setParameter(int id, float value) override { if (id >= 0 && id < 4) setCell(id, value); }
    float getParameter(int id) const override { return id >= 0 && id < 4 ? cell(id) : 0.0f; }
    void setExtra(int index, float value) override { if (index >= 0 && index < MAX_EXTRAS) setCell(4 + index, value); }
    float getExtra(int index) const override { return index >= 0 && index < MAX_EXTRAS ? cell(4 + index) : 0.0f; }

    // ---- modulation -----------------------------------------------------------------------------
    // Cells 0..3 are the four, 4..7 those of FX2, 8 (WET) the slot's mix. The offset stays until
    // the next call; the core glides to the moved value, so a new offset every block does not click.
    void modulateCell(int cell, float offset) override
    {
        if (! moves(cell)) return;
        modulation[(size_t) cell] = offsetOf(offset);
        modulated = false;
        for (float m : modulation) modulated = modulated || m != 0.0f;
    }
    float getModulation(int cell) const { return cell >= 0 && cell < TARGETS ? modulation[(size_t) cell] : 0.0f; }
    bool takesModulation(int cell) const override { return moves(cell); }
    // Whether modulateCell moves a cell: it is one, and not a list or a count.
    bool moves(int cell) const
    {
        if (cell == WET) return true;
        if (cell < 0 || cell >= CELLS) return false;
        const ParamSpec& spec = specs[cell];
        return spec.isValid() && ! spec.isDiscrete() && spec.maxVal > spec.minVal;
    }
    // The cell as modulateCell sees it (WET: the slot's mix, 0..1); invalid = no such cell.
    ParamSpec cellSpec(int cell) const
    {
        if (cell >= 0 && cell < CELLS) return specs[cell];
        if (cell == WET)               return { "WET", WET, 0, 1, 1 };
        return {};
    }
    // What a cell is worth now, modulation included (for a test, or a screen that shows it move).
    float getMovedCell(int cell) const
    {
        if (cell == WET || (cell >= 0 && cell == mixIndex)) return wetWanted();
        return cell >= 0 && cell < CELLS ? moved(specs[cell], value[(size_t) cell], modulation[(size_t) cell]) : 0.0f;
    }

    // ---- the clock ------------------------------------------------------------------------------
    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = goodRate(sr);
        mixStep = (float) CONTROL / (MIX_SWING_SECONDS * (float) sampleRate);
        prepareCore();
        reset();
    }

    // Everything where it is set, nothing sounding: the next sample begins a look.
    void reset() override
    {
        look();
        resetCore();
        mixNow = mixAim = mixWanted();
        mixInc = 0.0f;
        untilControl = 0;
        started = false;
    }

    // One sample (the master section runs its effects this way): the clock, then the core.
    void process(float& left, float& right) override
    {
        if (! enabled) return;
        const float from = mixNow, to = clock(left, right);
        runCore(&left, &right, 1, mixInc == 0.0f ? to : from, to);
    }

    void processBlock(float* left, float* right, int numSamples) override
    {
        if (! enabled) return;
        while (numSamples > 0)
        {
            const bool looked = untilControl <= 0;
            if (looked)
                control();
            int n = std::min(numSamples, untilControl);
            // Nothing on its way (the cells were looked at just now, and none can change before the
            // block ends): the looks that fall inside the rest of the block would find nothing to
            // do, so the core takes the rest in one run and the count goes on as if they had been.
            if (looked && n < numSamples && mixInc == 0.0f && coasting())
            {
                n = numSamples;
                const int past = n % CONTROL;
                untilControl = past == 0 ? 0 : CONTROL - past;
            }
            else
                untilControl -= n;
            const float to = mixInc == 0.0f || untilControl == 0 ? mixAim : mixNow + mixInc * (float) n;
            saneBlock(left, right, n);
            runCore(left, right, n, mixNow, to);
            mixNow = to;
            left += n; right += n;
            numSamples -= n;
        }
    }

protected:
    // The eight cells: [0..3] the FX page, [4..7] FX2 (ids 0..3 again). An entry with id < 0 is no
    // cell. The table lives as long as the effect (a static one, or one the effect holds).
    virtual const ParamSpec* cells() const = 0;
    // The cell that is the dry / wet mix (EffectBase's own `mix`, which the slot's WET sets too); -1 = none.
    virtual int mixCell() const { return -1; }
    // The cells' values (v[0..7], modulation included) into the core. Audio thread, at a look, and
    // only when one has changed (or the sample rate, or the effect said touch()).
    virtual void configure(const float* v) = 0;

    virtual void prepareCore() = 0;         // memory, for sampleRate
    virtual void resetCore() = 0;           // everything where it is set, the lines empty
    virtual void controlCore() {}           // every CONTROL samples, after configure()
    // True while nothing in the core waits for controlCore() (no glide of its on the way, and
    // what sweeps in it counts its own steps): until a cell changes, controlCore() would do
    // nothing and runCore() may be handed any number of samples.
    virtual bool coasting() const { return false; }
    // `n` samples in place (numbers within IN_LIMIT), no more than are left until the next
    // controlCore(): in (1 - mix) + wet mix, the mix going from mixFrom to mixTo over them (equal:
    // exactly that mix).
    virtual void runCore(float* left, float* right, int n, float mixFrom, float mixTo) = 0;

    // To be called by the effect's constructor: every cell at its default, ready at 44.1 kHz.
    void seed()
    {
        specs = cells();
        setWetOnly(true);       // (in a chain the chain mixes: EffectBase::givesWetOnly)
        mixIndex = mixCell();
        for (int c = 0; c < CELLS; ++c)
            value[(size_t) c] = specs[c].isValid() ? specs[c].defaultVal : 0.0f;
        if (mixIndex >= 0) mix = specs[mixIndex].defaultVal;
        prepare(44100.0, 0);
    }

    // A cell as it is set (no modulation), and setting it: what a setter by name goes through.
    float cell(int c) const { return c == mixIndex ? mix : value[(size_t) c]; }
    void setCell(int c, float v)
    {
        const ParamSpec& spec = specs[c];
        if (! spec.isValid() || ! std::isfinite(v)) return;
        v = std::clamp(v, spec.minVal, spec.maxVal);
        if (spec.isDiscrete()) v = std::round(v);
        if (c == mixIndex) setMix(v); else value[(size_t) c] = v;
    }
    // Something configure() reads that is not a cell has changed: configure again at the next look.
    void touch() { stale = true; }

    // The clock for an effect that is run one sample at a time (the master section): to be called
    // once with each sample before it is worked on; it makes the sample fit for the core and gives
    // the mix for it.
    float clock(float& left, float& right)
    {
        if (! (std::abs(left) + std::abs(right) <= IN_LIMIT))       // (one test for the two; seldom true)
        {
            left = sane(left);
            right = sane(right);
        }
        if (untilControl <= 0)
            control();
        --untilControl;
        if (mixInc != 0.0f)
            mixNow = untilControl == 0 ? mixAim : mixNow + mixInc;
        return mixNow;
    }

public:
    // The mix asked for: the MIX cell or the slot's WET, with what modulation adds to either.
    float wetWanted() const override
    {
        const float own = mixIndex >= 0 ? modulation[(size_t) mixIndex] : 0.0f;
        return std::clamp(mix + own + modulation[(size_t) WET], 0.0f, 1.0f);
    }

private:
    // The mix the core is run at: in a chain the wet sound alone (the chain mixes), else the mix asked for.
    float mixWanted() const { return givesWetOnly() ? 1.0f : wetWanted(); }

    // The cells as they stand now, with what modulation adds; into the core if anything moved.
    void look()
    {
        std::array<float, CELLS> v = value;
        if (modulated)
            for (int c = 0; c < CELLS; ++c)
                if (modulation[(size_t) c] != 0.0f) v[(size_t) c] = moved(specs[c], v[(size_t) c], modulation[(size_t) c]);
        if (mixIndex >= 0) v[(size_t) mixIndex] = mixWanted();
        if (v != seen || sampleRate != seenRate || stale)
        {
            seen = v;
            seenRate = sampleRate;
            stale = false;
            configure(v.data());
        }
    }

    void control()
    {
        look();
        controlCore();
        const float wanted = mixWanted();
        mixNow = started ? mixAim : wanted;         // (nothing has sounded since reset(): no line to draw)
        mixAim = std::abs(wanted - mixNow) <= mixStep * 1.0001f ? wanted : mixNow + std::copysign(mixStep, wanted - mixNow);
        mixInc = (mixAim - mixNow) * (1.0f / (float) CONTROL);
        if (mixInc == 0.0f) mixNow = mixAim;        // (standing: where it is, is where it is put)
        started = true;
        untilControl = CONTROL;
    }

    const ParamSpec* specs = nullptr;
    int mixIndex = -1;
    bool modulated = false, stale = false, started = false;
    std::array<float, CELLS> value {}, seen {};
    std::array<float, TARGETS> modulation {};
    double seenRate = 0.0;
    float mixNow = 1.0f, mixAim = 1.0f, mixInc = 0.0f, mixStep = 1.0f;
    int untilControl = 0;
};
