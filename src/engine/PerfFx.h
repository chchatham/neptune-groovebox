#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>
#include "ParamSpec.h"
#include "../dsp/effects/EffectBase.h"
#include "../dsp/effects/PerfEffects.h"

// -----------------------------------------------------------------------------------------------
// PERF: effects played from the pads.
//
// Each of the sixteen pads holds up to four effects (layers A - D) that one finger plays together.
// A layer is an effect with its own settings, what it acts on (the selected track, the whole mix,
// or one track by number) and how a press brings it in:
//
//   PUNCH  the effect is in place of the sound while the pad is down (faded in and out)
//   THROW  the sound is fed to the effect only while the pad is down; what the effect then
//          gives is added, and rings on after the pad is let go
//
// Each of a layer's five values (the effect's own four, and its WET) has two amounts: how far the
// PRESS moves it for as long as the pad is down, and how far the pad's PRESSURE moves it. So one
// pad can move any number of values on any of its effects at once, each its own way.
// For the whole pad: latch (a press switches it on, the next one off), the grid a press waits
// for, and the curve of its pressure.
//
// Every layer owns its effect object (made and replaced under the engine's structure lock, never
// on the audio thread). A layer that is up and has gone quiet is not processed at all.
// A track's layers act after its fader, pan and sends (a throw's tail outlives a fader pulled
// down; it does not go to the track's sends); MSTR's act on the sum, before the master EQ,
// compressor and limiter. Layers on one target run in pad order, then layer order.
//
// What the screen sets is written to atomics; the audio thread reads them at the start of a
// block and is the only one that touches an effect's parameters.
// -----------------------------------------------------------------------------------------------
namespace PerfFx
{
    static constexpr int PADS = 16;
    static constexpr int LAYERS = 4;
    static constexpr int VALUES = 5;            // the effect's four cells, then WET
    static constexpr int WET_VALUE = 4;

    // What a layer holds. 1..99 are the insert effect types (EffectType + 1); the pads' own
    // effects follow from 100. Stored in projects: numbers are never reused.
    enum Kind { NONE = 0, INSERT_BASE = 1, DUB = 100, FRZ = 101, RPT = 102, STOP = 103 };

    enum Target { SEL = 0, MASTER = 1, TRACK_BASE = 2 };        // TRACK_BASE + n = track n
    enum Mode { PUNCH = 0, THROW = 1 };
    enum Curve { LINEAR = 0, SOFT = 1, HARD = 2, NUM_CURVES = 3 };
    // A press waits for the next of these while the sequencer plays.
    enum Quantize { Q_OFF = 0, Q_16, Q_8, Q_4, Q_BAR, NUM_QUANT };
    inline double quantizeBeats(int q)
    {
        static const double b[NUM_QUANT] = { 0.0, 0.25, 0.5, 1.0, 4.0 };
        return b[juce::jlimit(0, NUM_QUANT - 1, q)];
    }

    // Where a layer's sound is taken, once pressed: a track number, or these.
    static constexpr int ON_MASTER = -1;
    static constexpr int NOWHERE = -2;

    struct Layer
    {
        int kind = NONE;
        int target = SEL;
        int mode = PUNCH;
        float wet = 1.0f;
        std::array<float, 4> cell { 0.0f, 0.0f, 0.0f, 0.0f };   // the effect's own four values
        bool sync = false;
        int division = FxSync::DEFAULT_DIVISION;
        // How far each value moves, -1..1 of its range: [0..3] the cells, [4] WET.
        std::array<float, VALUES> press { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };          // while the pad is down
        std::array<float, VALUES> pressure { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };       // with the pad's pressure
        // The effect's cells beyond its four (EffectBase::getExtraSpec): set, not played.
        std::array<float, EffectBase::MAX_EXTRAS> extra { 0.0f, 0.0f, 0.0f, 0.0f };
    };
    static constexpr int LAYER_FIELDS = 20 + EffectBase::MAX_EXTRAS;

    struct Setting
    {
        std::array<Layer, LAYERS> layer;
        bool latch = false;
        int quantize = Q_OFF;
        int pressureCurve = LINEAR;

        bool empty() const
        {
            for (auto& l : layer) if (l.kind != NONE) return false;
            return true;
        }
        // The first layer that holds an effect (0 when none does).
        int firstLayer() const
        {
            for (int i = 0; i < LAYERS; ++i) if (layer[(size_t) i].kind != NONE) return i;
            return 0;
        }
    };
    static constexpr int PAD_FIELDS = 3;

    // Which ways a kind can be brought in.
    struct Ways { bool punch = true, thrown = false; };
}

class PerfFxRack
{
public:
    using Creator = std::function<std::unique_ptr<EffectBase>(int kind)>;

    struct Context
    {
        float bpm = 120.0f;
        double beatAtStart = 0.0, beatsPerSample = 0.0;
        bool playing = false;
    };

    // ---- set-up (the engine) ---------------------------------------------------------------------
    void setCreator(Creator c, std::function<PerfFx::Ways(int kind)> waysOf, juce::CriticalSection* structureLock)
    {
        creator = std::move(c);
        ways = std::move(waysOf);
        structure = structureLock;
    }

    void prepare(double sr, int blockSize)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        const int n = juce::jmax(64, blockSize);
        for (auto* b : { &wetL, &wetR, &inL, &inR, &gate })
            b->assign((size_t) n, 0.0f);
        for (auto& p : pads)
            for (auto& s : p.slot)
                if (s.fx)
                {
                    s.fx->prepare(sampleRate, n);
                    s.fx->reset();
                    s.forgetApplied();
                }
    }

    // ---- what the pads hold (message / MIDI thread, under the screens' lock) ---------------------
    const PerfFx::Setting& get(int pad) const { return pads[(size_t) index(pad)].setting; }
    const PerfFx::Layer& get(int pad, int layer) const { return get(pad).layer[(size_t) layerIndex(layer)]; }

    PerfFx::Ways waysOf(int kind) const { return ways ? ways(kind) : PerfFx::Ways(); }

    // The effect's description of cell 0..3 (an empty spec where it has fewer).
    ParamSpec cellSpec(int pad, int layer, int cell) const
    {
        auto& s = slotOf(pad, layer);
        return (s.fx && cell >= 0 && cell < 4) ? s.fx->getParamSpec(cell) : ParamSpec();
    }
    // Its cells beyond the four (EffectBase::getExtraSpec).
    ParamSpec extraSpec(int pad, int layer, int index) const
    {
        auto& s = slotOf(pad, layer);
        return s.fx ? s.fx->getExtraSpec(index) : ParamSpec();
    }
    // The cell the effect locks to the tempo (its TIME or RATE), or -1.
    int syncCell(int pad, int layer) const { auto& s = slotOf(pad, layer); return s.fx ? s.fx->getSyncParamIndex() : -1; }
    bool canSync(int pad, int layer) const { return syncCell(pad, layer) >= 0; }

    // The whole pad. A layer whose kind differs gets that kind first; values are brought into range.
    void set(int pad, PerfFx::Setting wanted)
    {
        auto& p = pads[(size_t) index(pad)];
        for (int l = 0; l < PerfFx::LAYERS; ++l)
        {
            if (wanted.layer[(size_t) l].kind != p.setting.layer[(size_t) l].kind)
                setKind(pad, l, wanted.layer[(size_t) l].kind);
            wanted.layer[(size_t) l].kind = p.setting.layer[(size_t) l].kind;
            p.setting.layer[(size_t) l] = sanitized(p.slot[(size_t) l], wanted.layer[(size_t) l]);
        }
        p.setting.latch = wanted.latch;
        p.setting.quantize = juce::jlimit(0, PerfFx::NUM_QUANT - 1, wanted.quantize);
        p.setting.pressureCurve = juce::jlimit(0, PerfFx::NUM_CURVES - 1, wanted.pressureCurve);
        p.publish();
    }

    void setLayer(int pad, int layer, const PerfFx::Layer& wanted)
    {
        PerfFx::Setting s = get(pad);
        s.layer[(size_t) layerIndex(layer)] = wanted;
        set(pad, s);
    }

    // Another effect on a layer: its own starting values, the way of pressing that suits it, and
    // pressure on what it is best played with. The layer's target stays.
    void setKind(int pad, int layer, int kind)
    {
        auto& p = pads[(size_t) index(pad)];
        auto& s = p.slot[(size_t) layerIndex(layer)];
        std::unique_ptr<EffectBase> fx = (kind != PerfFx::NONE && creator) ? creator(kind) : nullptr;
        if (! fx)
            kind = PerfFx::NONE;
        else
        {
            fx->prepare(sampleRate, (int) wetL.size());
            fx->reset();
        }
        PerfFx::Layer fresh;
        fresh.kind = kind;
        fresh.target = p.setting.layer[(size_t) layerIndex(layer)].target;
        bool hasMixCell = false;
        if (fx)
        {
            bool routed = false;
            for (int i = 0; i < 4; ++i)
            {
                const ParamSpec spec = fx->getParamSpec(i);
                fresh.cell[(size_t) i] = spec.isValid() ? spec.defaultVal : 0.0f;
                const ParamSpec more = fx->getExtraSpec(i);
                fresh.extra[(size_t) i] = more.isValid() ? more.defaultVal : 0.0f;
                const std::string label(spec.isValid() && spec.label != nullptr ? spec.label : "");
                if (label == "MIX") hasMixCell = true;
                if (! routed && (label == "DPTH" || label == "CUT" || label == "SHIM"
                                 || (kind == PerfFx::DUB && label == "FDBK") || (kind == PerfFx::RPT && label == "DIV")))
                {
                    routed = true;
                    fresh.pressure[(size_t) i] = label == "FDBK" ? 0.5f : 1.0f;
                }
            }
            const auto w = waysOf(kind);
            fresh.mode = w.thrown ? PerfFx::THROW : PerfFx::PUNCH;
            // An echo in time with the music is what a throw wants.
            fresh.sync = kind == PerfFx::DUB;
            fresh.division = kind == PerfFx::DUB ? 11 : FxSync::DEFAULT_DIVISION;      // 1/8D
        }
        if (fx || s.fx)
        {
            // Only the exchange is done under the lock the audio thread tries: the effect that was
            // there is destroyed (megabytes, for a repeat or a stop) after it is let go of.
            auto* gesture = dynamic_cast<PerfGesture*>(fx.get());
            const float ownMix = fx ? fx->getMix() : 1.0f;
            const juce::CriticalSection::ScopedLockType lock(structure != nullptr ? *structure : ownLock);
            std::swap(s.fx, fx);
            s.gesture = gesture;
            s.hasMixCell = hasMixCell;
            s.ownMix = ownMix;
            s.resetAudio();
        }
        p.setting.layer[(size_t) layerIndex(layer)] = sanitized(s, fresh);
        if (p.setting.empty())
            p.down.store(false);
        p.publish();
    }

    void clear(int pad)
    {
        for (int l = 0; l < PerfFx::LAYERS; ++l)
            setKind(pad, l, PerfFx::NONE);
        auto& p = pads[(size_t) index(pad)];
        p.setting = PerfFx::Setting();
        p.down.store(false);
        p.publish();
    }

    // ---- playing (MIDI thread) -------------------------------------------------------------------
    void press(int pad, int selectedTrack)
    {
        auto& p = pads[(size_t) index(pad)];
        if (p.setting.empty())
            return;
        const bool on = p.setting.latch ? ! p.down.load() : true;
        if (on)
        {
            for (int l = 0; l < PerfFx::LAYERS; ++l)
            {
                const int t = p.setting.layer[(size_t) l].target;
                p.slot[(size_t) l].where.store(t == PerfFx::SEL ? selectedTrack
                                                                : (t == PerfFx::MASTER ? PerfFx::ON_MASTER : t - PerfFx::TRACK_BASE));
            }
            p.pressure.store(0.0f);
        }
        p.down.store(on);
    }

    void release(int pad)
    {
        auto& p = pads[(size_t) index(pad)];
        if (! p.setting.latch)
            p.down.store(false);
        p.pressure.store(0.0f);
    }

    void setPressure(int pad, float amount) { pads[(size_t) index(pad)].pressure.store(juce::jlimit(0.0f, 1.0f, amount)); }

    // Every pad let go, every effect emptied (Shift + Play, a project loaded).
    void silence()
    {
        for (auto& p : pads)
            p.down.store(false);
        silenceWanted.store(true);
    }

    bool isDown(int pad) const { return pads[(size_t) index(pad)].down.load(); }
    bool isSounding(int pad) const
    {
        for (auto& s : pads[(size_t) index(pad)].slot)
            if (s.active.load()) return true;
        return false;
    }
    // Where a layer that is down or ringing acts: a track, PerfFx::ON_MASTER, or NOWHERE.
    int actingOn(int pad, int layer = 0) const
    {
        auto& p = pads[(size_t) index(pad)];
        auto& s = p.slot[(size_t) layerIndex(layer)];
        return (s.fx && (p.down.load() || s.active.load())) ? s.where.load() : PerfFx::NOWHERE;
    }
    int getNonFiniteCount() const { return nonFinite.load(); }

    // ---- projects --------------------------------------------------------------------------------
    // { version 2, banks 1, pads, layers per pad, fields per pad, fields per layer }, then for every
    // pad its own fields and its layers' fields. Fields can be added at the end of a pad's or a
    // layer's, layers and banks after the last: an older reader takes what it knows.
    // (Version 1, one effect a pad, lived for a few hours of one day and was never saved by a build anyone kept.)
    std::vector<float> toFlat() const
    {
        std::vector<float> v { 2.0f, 1.0f, (float) PerfFx::PADS, (float) PerfFx::LAYERS, (float) PerfFx::PAD_FIELDS, (float) PerfFx::LAYER_FIELDS };
        for (auto& p : pads)
        {
            auto& s = p.setting;
            v.insert(v.end(), { s.latch ? 1.0f : 0.0f, (float) s.quantize, (float) s.pressureCurve });
            for (auto& l : s.layer)
            {
                v.insert(v.end(), { (float) l.kind, (float) l.target, (float) l.mode, l.wet, l.cell[0], l.cell[1], l.cell[2], l.cell[3],
                                    l.sync ? 1.0f : 0.0f, (float) l.division });
                v.insert(v.end(), l.press.begin(), l.press.end());
                v.insert(v.end(), l.pressure.begin(), l.pressure.end());
                v.insert(v.end(), l.extra.begin(), l.extra.end());
            }
        }
        return v;
    }

    // False (and nothing changed) when the list is not one of these.
    bool fromFlat(const std::vector<float>& v)
    {
        if (v.empty())
            return false;
        if (whole(v[0], 0) != 2 || v.size() < 6)
            return false;
        const int padCount = whole(v[2], 0), layers = whole(v[3], 0), padFields = whole(v[4], 0), layerFields = whole(v[5], 0);
        if (padCount < 1 || padCount > 256 || layers < 1 || layers > 64 || padFields < 1 || padFields > 256 || layerFields < 10 || layerFields > 256)
            return false;
        const size_t perPad = (size_t) padFields + (size_t) layers * (size_t) layerFields;
        if (v.size() < 6 + perPad)
            return false;
        silence();
        for (int pad = 0; pad < PerfFx::PADS; ++pad)
        {
            const size_t at = 6 + (size_t) pad * perPad;
            clear(pad);
            if (pad >= padCount || at + perPad > v.size())
                continue;
            auto number = [&v](size_t i, float fallback) { return std::isfinite(v[i]) ? v[i] : fallback; };
            for (int l = 0; l < PerfFx::LAYERS && l < layers; ++l)
                setKind(pad, l, whole(v[at + (size_t) padFields + (size_t) l * (size_t) layerFields], 0));
            PerfFx::Setting s = get(pad);       // (each kind's own values where the list has none)
            s.latch = number(at, 0) > 0.5f;
            if (padFields > 1) s.quantize = whole(v[at + 1], 0);
            if (padFields > 2) s.pressureCurve = whole(v[at + 2], 0);
            for (int l = 0; l < PerfFx::LAYERS && l < layers; ++l)
            {
                const size_t la = at + (size_t) padFields + (size_t) l * (size_t) layerFields;
                auto f = [&](int i, float fallback) { return i < layerFields ? number(la + (size_t) i, fallback) : fallback; };
                auto& p = s.layer[(size_t) l];
                p.target = whole(f(1, 0), 0);
                p.mode = whole(f(2, (float) p.mode), p.mode);
                p.wet = f(3, 1);
                for (int i = 0; i < 4; ++i)
                    p.cell[(size_t) i] = f(4 + i, p.cell[(size_t) i]);
                p.sync = f(8, 0) > 0.5f;
                p.division = whole(f(9, (float) p.division), p.division);
                for (int i = 0; i < PerfFx::VALUES; ++i)
                {
                    p.press[(size_t) i] = f(10 + i, 0);
                    p.pressure[(size_t) i] = f(15 + i, 0);
                }
                // (A list from before the extra cells: each keeps the value its kind starts with.)
                for (int i = 0; i < EffectBase::MAX_EXTRAS; ++i)
                    p.extra[(size_t) i] = f(20 + i, p.extra[(size_t) i]);
            }
            set(pad, s);
        }
        return true;
    }

    // ---- audio thread ----------------------------------------------------------------------------
    // Once a block, before any target is processed: what the pads are doing is read here.
    void beginBlock(const Context& c, int numSamples)
    {
        context = c;
        if (silenceWanted.exchange(false))
            for (auto& p : pads)
            {
                for (auto& s : p.slot)
                {
                    if (s.fx && s.ranSinceReset) s.fx->reset();     // (megabytes of delay memory: only where it was used)
                    s.resetAudio();
                }
                p.gateOn = false;
                p.pressNow = p.pressureNow = p.pressureShaped = 0.0f;
            }
        anyLive = false;
        // The press and the pressure are followed over a few milliseconds (pressure arrives in
        // steps, some 75 times a second; a press is a step).
        const float followPress = 1.0f - std::exp(-(float) numSamples / (0.008f * (float) sampleRate));
        const float followPressure = 1.0f - std::exp(-(float) numSamples / (0.015f * (float) sampleRate));
        for (auto& p : pads)
        {
            p.startOffset = -1;
            const bool down = p.down.load(std::memory_order_relaxed);
            if (! down)
                p.gateOn = false;
            else if (! p.gateOn)
                p.gateFromOffset(c, numSamples);        // (it may wait for the grid)

            bool live = false;
            for (auto& s : p.slot)
                if (s.fx && (p.gateOn || s.active.load(std::memory_order_relaxed)))
                {
                    // A layer that is ringing stays where it is until the pad is pressed again.
                    if (p.startOffset >= 0 || s.target == PerfFx::NOWHERE)
                        s.target = s.where.load(std::memory_order_relaxed);
                    live = true;
                }
            if (! live)
            {
                p.pressNow = p.pressureNow = p.pressureShaped = 0.0f;
                continue;
            }
            anyLive = true;
            p.pressNow += followPress * ((p.gateOn ? 1.0f : 0.0f) - p.pressNow);
            p.pressureNow += followPressure * ((p.gateOn ? p.pressure.load(std::memory_order_relaxed) : 0.0f) - p.pressureNow);
            float shaped = p.pressureNow;
            const int curve = p.curve.load(std::memory_order_relaxed);
            if (curve == PerfFx::SOFT) shaped = std::sqrt(std::max(0.0f, shaped));
            if (curve == PerfFx::HARD) shaped *= shaped;
            p.pressureShaped = shaped;
        }
    }

    // target: a track number or PerfFx::ON_MASTER.
    void process(int target, float* left, float* right, int numSamples)
    {
        if (! anyLive)
            return;
        for (auto& p : pads)
            for (auto& s : p.slot)
            {
                if (! s.fx || s.target != target || ! (p.gateOn || s.active.load(std::memory_order_relaxed)))
                    continue;
                const int split = juce::jlimit(0, numSamples, p.startOffset);
                if (p.startOffset > 0)
                {
                    // The press takes hold part of the way through the block (on the grid).
                    run(p, s, left, right, split, false);
                    run(p, s, left + split, right + split, numSamples - split, true);
                }
                else
                    run(p, s, left, right, numSamples, p.gateOn);
            }
    }

private:
    struct Slot
    {
        std::unique_ptr<EffectBase> fx;
        PerfGesture* gesture = nullptr;
        bool hasMixCell = false;
        float ownMix = 1.0f;

        // Screens -> audio.
        struct Live
        {
            std::atomic<int> mode { 0 }, division { 0 };
            std::atomic<bool> sync { false };
            std::atomic<float> wet { 1.0f };
            std::array<std::atomic<float>, 4> cell;
            std::array<std::atomic<float>, EffectBase::MAX_EXTRAS> extra;
            std::array<std::atomic<float>, PerfFx::VALUES> press, pressure;
        } live;
        std::atomic<int> where { PerfFx::NOWHERE };
        // Audio -> screens.
        std::atomic<bool> active { false };

        // Audio thread only.
        bool gestureDown = false;
        bool ranSinceReset = false;      // the effect holds sound or state that a reset would empty
        int target = PerfFx::NOWHERE;
        float env = 0.0f, wetNow = 1.0f;
        std::array<float, 4> applied;
        std::array<float, EffectBase::MAX_EXTRAS> appliedExtra;
        int appliedDivision = -1;
        int quietSamples = 0;

        Slot()
        {
            forgetApplied();
            for (auto& c : live.cell) c.store(0.0f);
            for (auto& c : live.press) c.store(0.0f);
            for (auto& c : live.pressure) c.store(0.0f);
        }

        void forgetApplied()
        {
            applied.fill(std::numeric_limits<float>::quiet_NaN());
            appliedExtra.fill(std::numeric_limits<float>::quiet_NaN());
            appliedDivision = -1;
        }

        void resetAudio()
        {
            gestureDown = ranSinceReset = false;
            target = PerfFx::NOWHERE;
            env = 0.0f;
            quietSamples = 0;
            forgetApplied();
            active.store(false);
        }

        void publish(const PerfFx::Layer& l)
        {
            live.mode = l.mode;
            live.division = l.division;
            live.sync = l.sync;
            live.wet = l.wet;
            for (int i = 0; i < 4; ++i)
                live.cell[(size_t) i] = l.cell[(size_t) i];
            for (int i = 0; i < EffectBase::MAX_EXTRAS; ++i)
                live.extra[(size_t) i] = l.extra[(size_t) i];
            for (int i = 0; i < PerfFx::VALUES; ++i)
            {
                live.press[(size_t) i] = l.press[(size_t) i];
                live.pressure[(size_t) i] = l.pressure[(size_t) i];
            }
        }
    };

    struct Pad
    {
        PerfFx::Setting setting;                    // the screens' copy
        std::array<Slot, PerfFx::LAYERS> slot;

        // Screens -> audio.
        std::atomic<bool> down { false };
        std::atomic<float> pressure { 0.0f };
        std::atomic<int> quantize { 0 }, curve { 0 };

        // Audio thread only.
        bool gateOn = false;
        int startOffset = -1;
        float pressNow = 0.0f, pressureNow = 0.0f, pressureShaped = 0.0f;

        void publish()
        {
            quantize = setting.quantize;
            curve = setting.pressureCurve;
            for (int l = 0; l < PerfFx::LAYERS; ++l)
                slot[(size_t) l].publish(setting.layer[(size_t) l]);
        }

        // The pad has just gone down: does its press take hold in this block? (startOffset says where.)
        void gateFromOffset(const Context& c, int numSamples)
        {
            const double grid = PerfFx::quantizeBeats(quantize.load(std::memory_order_relaxed));
            int offset = 0;
            if (grid > 0.0 && c.playing && c.beatsPerSample > 0.0)
            {
                const double next = std::ceil(c.beatAtStart / grid - 1.0e-9) * grid;
                const double samples = (next - c.beatAtStart) / c.beatsPerSample;
                if (samples >= (double) numSamples)
                    return;
                offset = juce::jlimit(0, numSamples - 1, (int) std::lround(samples));
            }
            gateOn = true;
            startOffset = offset;
        }
    };

    // A thrown layer is at rest after this long with nothing coming out of it: longer than the
    // longest echo time of any effect (2 s), or an echo not yet heard would be thrown away.
    static constexpr double THROW_REST_SECONDS = 2.5;

    static int index(int pad) { return juce::jlimit(0, PerfFx::PADS - 1, pad); }
    static int layerIndex(int layer) { return juce::jlimit(0, PerfFx::LAYERS - 1, layer); }
    static int whole(float x, int fallback) { return std::isfinite(x) && std::abs(x) < 1.0e6f ? (int) std::lround(x) : fallback; }
    const Slot& slotOf(int pad, int layer) const { return pads[(size_t) index(pad)].slot[(size_t) layerIndex(layer)]; }

    PerfFx::Layer sanitized(const Slot& s, PerfFx::Layer p) const
    {
        auto unit = [](float x, float lo, float hi, float fallback) { return std::isfinite(x) ? juce::jlimit(lo, hi, x) : fallback; };
        p.target = juce::jlimit(0, PerfFx::TRACK_BASE + 255, p.target);
        const auto w = waysOf(p.kind);
        p.mode = juce::jlimit(0, 1, p.mode);
        if (p.mode == PerfFx::THROW && ! w.thrown) p.mode = PerfFx::PUNCH;
        if (p.mode == PerfFx::PUNCH && ! w.punch)  p.mode = PerfFx::THROW;
        p.wet = unit(p.wet, 0.0f, 1.0f, 1.0f);
        for (int i = 0; i < PerfFx::VALUES; ++i)
        {
            p.press[(size_t) i] = unit(p.press[(size_t) i], -1.0f, 1.0f, 0.0f);
            p.pressure[(size_t) i] = unit(p.pressure[(size_t) i], -1.0f, 1.0f, 0.0f);
        }
        for (int i = 0; i < 4; ++i)
        {
            const ParamSpec spec = s.fx ? s.fx->getParamSpec(i) : ParamSpec();
            float& c = p.cell[(size_t) i];
            if (! spec.isValid())
            {
                // (No such cell on this effect: nothing to move either.)
                c = 0.0f;
                p.press[(size_t) i] = p.pressure[(size_t) i] = 0.0f;
            }
            else if (! std::isfinite(c))     c = spec.defaultVal;
            else                             c = spec.isDiscrete() ? std::round(spec.clamp(c)) : spec.clamp(c);
        }
        for (int i = 0; i < EffectBase::MAX_EXTRAS; ++i)
        {
            const ParamSpec spec = s.fx ? s.fx->getExtraSpec(i) : ParamSpec();
            float& c = p.extra[(size_t) i];
            if (! spec.isValid())            c = 0.0f;
            else if (! std::isfinite(c))     c = spec.defaultVal;
            else                             c = spec.isDiscrete() ? std::round(spec.clamp(c)) : spec.clamp(c);
        }
        p.sync = p.sync && s.fx && s.fx->canSync();
        p.division = juce::jlimit(0, FxSync::NUM_DIVISIONS - 1, p.division);
        return p;
    }

    // The effect's values for this stretch: what the screen set, moved by the press and by the pressure.
    void applyParameters(const Pad& p, Slot& s)
    {
        auto& fx = *s.fx;
        fx.setTempo(context.bpm);
        const bool sync = s.live.sync.load(std::memory_order_relaxed);
        if (sync != fx.isSynced()) fx.setSync(sync);
        const int division = s.live.division.load(std::memory_order_relaxed);
        if (division != s.appliedDivision) { s.appliedDivision = division; fx.setSyncDivision(division); }

        auto moveOf = [&](int value)
        {
            return s.live.press[(size_t) value].load(std::memory_order_relaxed) * p.pressNow
                 + s.live.pressure[(size_t) value].load(std::memory_order_relaxed) * p.pressureShaped;
        };
        for (int i = 0; i < 4; ++i)
        {
            const ParamSpec spec = fx.getParamSpec(i);
            if (! spec.isValid()) continue;
            float v = s.live.cell[(size_t) i].load(std::memory_order_relaxed);
            const float move = moveOf(i);
            if (move != 0.0f)
                v = spec.fromNorm(spec.toNorm(v) + move);
            if (v != s.applied[(size_t) i])
            {
                s.applied[(size_t) i] = v;
                fx.setUserParameter(i, v);
            }
        }
        for (int i = 0; i < EffectBase::MAX_EXTRAS; ++i)
        {
            const float v = s.live.extra[(size_t) i].load(std::memory_order_relaxed);
            if (v != s.appliedExtra[(size_t) i])
            {
                s.appliedExtra[(size_t) i] = v;
                fx.setExtra(i, v);
            }
        }
        s.wetNow = juce::jlimit(0.0f, 1.0f, s.live.wet.load(std::memory_order_relaxed) + moveOf(PerfFx::WET_VALUE));
        // A throw adds the effect's wet part alone: an effect with no MIX of its own gives all of it.
        if (! s.hasMixCell)
            fx.setMix(s.live.mode.load(std::memory_order_relaxed) == PerfFx::THROW ? 1.0f : s.ownMix);
    }

    void run(const Pad& p, Slot& s, float* left, float* right, int numSamples, bool gateOn)
    {
        if (numSamples <= 0 || (! gateOn && ! s.active.load(std::memory_order_relaxed)))
            return;
        if (gateOn && ! s.gestureDown)
        {
            s.gestureDown = true;
            if (s.gesture) s.gesture->padGate(true);
            s.active.store(true, std::memory_order_relaxed);
            s.quietSamples = 0;
        }
        else if (! gateOn && s.gestureDown)
        {
            s.gestureDown = false;
            if (s.gesture) s.gesture->padGate(false);
        }
        applyParameters(p, s);
        s.ranSinceReset = true;

        const bool thrown = s.live.mode.load(std::memory_order_relaxed) == PerfFx::THROW;
        const float up = 1.0f / (0.004f * (float) sampleRate);
        const float downRate = 1.0f / ((thrown ? 0.010f : 0.025f) * (float) sampleRate);
        const float wet = s.wetNow;
        const float dryShare = thrown ? 1.0f - s.fx->getMix() : 0.0f;
        float peak = 0.0f;

        for (int done = 0; done < numSamples;)
        {
            const int n = juce::jmin(numSamples - done, (int) wetL.size());
            float* l = left + done;
            float* r = right + done;
            float env = s.env;
            // What the effect is fed: the sound (a punch), or the sound while the pad is down (a throw).
            for (int i = 0; i < n; ++i)
            {
                env = gateOn ? std::min(1.0f, env + up) : std::max(0.0f, env - downRate);
                gate[(size_t) i] = env;
                wetL[(size_t) i] = thrown ? l[i] * env : l[i];
                wetR[(size_t) i] = thrown ? r[i] * env : r[i];
            }
            s.env = env;
            if (thrown)
                for (int i = 0; i < n; ++i) { inL[(size_t) i] = wetL[(size_t) i]; inR[(size_t) i] = wetR[(size_t) i]; }
            s.fx->processBlock(wetL.data(), wetR.data(), n);

            bool finite = true;
            for (int i = 0; i < n; ++i)
                finite = finite && std::isfinite(wetL[(size_t) i]) && std::isfinite(wetR[(size_t) i]);
            if (! finite)
            {
                // An effect gone wrong: it is emptied and the layer falls silent (counted, so that it is seen).
                s.fx->reset();
                s.resetAudio();
                nonFinite.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            for (int i = 0; i < n; ++i)
            {
                if (thrown)
                {
                    const float addL = (wetL[(size_t) i] - inL[(size_t) i] * dryShare) * wet;
                    const float addR = (wetR[(size_t) i] - inR[(size_t) i] * dryShare) * wet;
                    peak = std::max(peak, std::max(std::abs(addL), std::abs(addR)));
                    l[i] += addL;
                    r[i] += addR;
                }
                else
                {
                    // (Written so that the effect alone is exactly the effect, and none of it exactly the sound.)
                    const float g = gate[(size_t) i] * wet;
                    l[i] = l[i] * (1.0f - g) + wetL[(size_t) i] * g;
                    r[i] = r[i] * (1.0f - g) + wetR[(size_t) i] * g;
                }
            }
            done += n;
        }

        // Up, and nothing left of it: the layer rests (and its effect starts empty next time).
        if (! gateOn)
        {
            const bool quiet = thrown ? peak < 3.0e-5f : s.env <= 0.0f;
            s.quietSamples = quiet ? s.quietSamples + numSamples : 0;
            if (s.env <= 0.0f && s.quietSamples > (thrown ? (int) (THROW_REST_SECONDS * sampleRate) : 0))
            {
                s.fx->reset();
                s.resetAudio();
            }
        }
    }

    std::array<Pad, PerfFx::PADS> pads;
    Creator creator;
    std::function<PerfFx::Ways(int)> ways;
    juce::CriticalSection* structure = nullptr;
    juce::CriticalSection ownLock;
    double sampleRate = 44100.0;
    std::vector<float> wetL = std::vector<float>(64, 0.0f), wetR = std::vector<float>(64, 0.0f);
    std::vector<float> inL = std::vector<float>(64, 0.0f), inR = std::vector<float>(64, 0.0f), gate = std::vector<float>(64, 0.0f);
    Context context;
    bool anyLive = false;
    std::atomic<bool> silenceWanted { false };
    std::atomic<int> nonFinite { 0 };
};
