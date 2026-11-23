#pragma once

// -----------------------------------------------------------------------------
// The three effect blocks every track has, each with its own 8-knob page:
//
//   DIRT    drive / distortion type / bit crusher / sample-rate reducer and a
//           filtered, tuned feedback path, all in one
//   TALK    talking modulator: a vocal formant filter swept by hand, by an LFO
//           and by the playing level
//   MODFX   chorus, flanger, phaser and rotary speaker side by side, two knobs
//           each (rate / depth); an effect whose depth is 0 is off
//
// They run in that order after the track's engine and before its insert slots.
// A block with everything at zero costs nothing. Parameter ids are
// TrackFxParam::*_BASE + cell index, so every cell can be locked per step.
//
// The blocks are built from the project's existing effect classes (Waveshaper,
// RateReducer, Chorus, Flanger, Phaser, RotarySpeaker), the comb filter and the
// swept filter (SweptFilter.h); nothing here is new low-level DSP.
// -----------------------------------------------------------------------------

#include "Waveshaper.h"
#include "RateReducer.h"
#include "Chorus.h"
#include "Flanger.h"
#include "Phaser.h"
#include "RotarySpeaker.h"
#include "SweptFilter.h"
#include "../filters/CombFilter.h"
#include "../../engine/ParamSpec.h"
#include <array>

namespace TrackFxParam
{
    static constexpr int DIRT_BASE = 240;
    static constexpr int TALK_BASE = 248;
    static constexpr int MODFX_BASE = 256;
    static constexpr int END = 264;

    inline bool isTrackFxParam(int id) { return id >= DIRT_BASE && id < END; }
}

// ---- DIRT ---------------------------------------------------------------------
class DirtBlock
{
public:
    enum Cell { Drive = 0, Type, Bits, Rate, Feedback, Tune, Damp, Mix, NumCells };

    static ParamPage page()
    {
        ParamPage p;
        p.name = "DIRT";
        p.section = "FX";
        const int b = TrackFxParam::DIRT_BASE;
        p.params = {{
            { "DRIV", b + Drive, 0, 1, 0 },
            { "TYPE", b + Type, 0, (float) (sat::Count - 1), 0, ParamKind::Choice, "", sat::names(), sat::Count },
            { "BITS", b + Bits, 0, 1, 0 },                              // bit crusher
            { "RATE", b + Rate, 0, 1, 0 },                              // sample-rate reducer
            { "FDBK", b + Feedback, 0, 1, 0 },                          // tuned feedback amount
            { "TUNE", b + Tune, 30, 4000, 220, ParamKind::Log, "Hz" },  // its pitch
            { "DAMP", b + Damp, 0, 1, 0.3f },                           // its filter (0 bright .. 1 dark)
            { "MIX",  b + Mix, 0, 1, 1 } }};
        return p;
    }

    DirtBlock()
    {
        for (auto& spec : page().params)
            values[(size_t) (spec.id - TrackFxParam::DIRT_BASE)] = spec.defaultVal;
        apply();
    }

    void prepare(double sr, int blockSize)
    {
        shaper.prepare(sr, blockSize);
        reducer.prepare(sr, blockSize);
        combL.setSampleRate(sr);
        combR.setSampleRate(sr);
        dryL.assign((size_t) juce::jmax(1, blockSize), 0.0f);
        dryR.assign((size_t) juce::jmax(1, blockSize), 0.0f);
        apply();
    }

    void set(int cell, float v)
    {
        if (cell < 0 || cell >= NumCells) return;
        values[(size_t) cell] = page().params[(size_t) cell].clamp(v);
        apply();
    }
    float get(int cell) const { return (cell >= 0 && cell < NumCells) ? values[(size_t) cell] : 0.0f; }

    bool isActive() const
    {
        return values[Mix] > 0.0f && (values[Drive] > 0.0f || values[Bits] > 0.0f || values[Rate] > 0.0f || values[Feedback] > 0.0f);
    }

    void process(float* left, float* right, int numSamples)
    {
        if (!isActive() || numSamples > (int) dryL.size())
            return;

        std::copy(left, left + numSamples, dryL.begin());
        std::copy(right, right + numSamples, dryR.begin());

        if (values[Drive] > 0.0f)
            shaper.processBlock(left, right, numSamples);
        if (values[Bits] > 0.0f || values[Rate] > 0.0f)
            reducer.processBlock(left, right, numSamples);
        if (values[Feedback] > 0.0f)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                left[i] = combL.process(left[i]);
                right[i] = combR.process(right[i]);
            }
        }

        const float mix = values[Mix];
        if (mix < 0.999f)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                left[i] = dryL[(size_t) i] + mix * (left[i] - dryL[(size_t) i]);
                right[i] = dryR[(size_t) i] + mix * (right[i] - dryR[(size_t) i]);
            }
        }
    }

private:
    void apply()
    {
        shaper.setParameter(Waveshaper::Drive, values[Drive]);
        shaper.setParameter(Waveshaper::Curve, values[Type]);
        shaper.setParameter(Waveshaper::Tone, 0.8f);
        shaper.setParameter(Waveshaper::Asymmetry, 0.5f);
        shaper.setParameter(Waveshaper::Mix, 1.0f);
        reducer.setParameter(RateReducer::RateReduction, values[Rate]);
        reducer.setParameter(RateReducer::BitDepth, values[Bits]);
        reducer.setParameter(RateReducer::PreFilter, 0.0f);
        reducer.setParameter(RateReducer::Mix, 1.0f);
        for (auto* c : { &combL, &combR })
        {
            c->setCutoff(values[Tune]);
            c->setResonance(values[Feedback]);
            c->setDamping(values[Damp]);
        }
    }

    std::array<float, NumCells> values {};
    Waveshaper shaper;
    RateReducer reducer;
    CombFilter combL, combR;
    std::vector<float> dryL, dryR;
};

// ---- TALK ---------------------------------------------------------------------
// A setting of the swept filter (SweptFilter.h): its vowel filter, three parallel band-pass
// filters placed on the first three formants of a vowel. The vowel position moves continuously
// through A - E - I - O - U; it can be set by hand, swept by the effects' LFO and pushed by the
// level of the playing (the shared detector), so the sound "talks".
class TalkBlock
{
public:
    enum Cell { Vowel = 0, Shift, Reso, Mix, LfoRate, LfoDepth, Env, Bite, NumCells };

    static ParamPage page()
    {
        ParamPage p;
        p.name = "TALK";
        p.section = "FX";
        const int b = TrackFxParam::TALK_BASE;
        p.params = {{
            { "VOWL", b + Vowel, 0, 1, 0 },                              // A .. E .. I .. O .. U
            { "SHFT", b + Shift, -1, 1, 0 },                             // formants down / up an octave
            { "RESO", b + Reso, 0, 1, 0.5f },                            // how sharp the formants are
            { "MIX",  b + Mix, 0, 1, 0 },                                // 0 = off
            { "RATE", b + LfoRate, 0.05f, 12, 1, ParamKind::Log, "Hz" }, // LFO on the vowel
            { "DPTH", b + LfoDepth, 0, 1, 0 },
            { "ENV",  b + Env, -1, 1, 0 },                               // playing level -> vowel
            { "BITE", b + Bite, 0, 1, 0.3f } }};                         // harmonics fed into the formants
        return p;
    }

    static const char* vowelName(float pos)
    {
        static const char* const names[] = { "A", "E", "I", "O", "U" };
        return names[juce::jlimit(0, 4, (int) std::lround(pos * 4.0f))];
    }

    TalkBlock()
    {
        for (auto& spec : page().params)
            values[(size_t) (spec.id - TrackFxParam::TALK_BASE)] = spec.defaultVal;
        prepare(44100.0, 0);
    }

    void prepare(double sr, int /*blockSize*/)
    {
        swp::Core::Voicing v;
        v.law = swp::Core::Place;
        v.slots = false;                // (the vowels alone: no room is taken for the filter set)
        v.baseMs = v.resoMs = v.depthMs = v.amountMs = 0.0f;      // (the cells act at the next step, as they always did)
        v.attackMs = 5.0f;
        v.releaseMs = 150.0f;
        v.envGain = 3.0f;
        v.meanLevel = true;             // (the mean of the two sides, as TALK always listened)
        core.prepare(CellEffect::goodRate(sr), v);
        core.setFilter(swp::filt::Vowel, 0);
        apply();
        core.reset();
    }

    void set(int cell, float v)
    {
        if (cell >= 0 && cell < NumCells)
            values[(size_t) cell] = page().params[(size_t) cell].clamp(v);
    }
    float get(int cell) const { return (cell >= 0 && cell < NumCells) ? values[(size_t) cell] : 0.0f; }

    bool isActive() const { return values[Mix] > 0.0f; }

    // Where the vowel is right now (0..1), including LFO and envelope, for display.
    float currentPosition() const { return core.currentPlace(); }

    // The track's custom LFO shape, and the LFO's shape (a cell for it has no room on the page).
    void setUserShape(const float* levels) { core.setUserShape(levels); }
    void setLfoShape(int shape) { lfoShape = shape; }

    void process(float* left, float* right, int numSamples)
    {
        if (!isActive())
            return;
        apply();
        const float mix = values[Mix];
        core.run(left, right, numSamples, left, right, mix, mix);
    }

private:
    // The cells into the core (audio thread: the values are plain numbers a knob or a step lock writes).
    void apply()
    {
        core.setBase(values[Vowel]);
        core.setShift(values[Shift]);
        core.setResonance(values[Reso]);
        core.setLfo(values[LfoRate], lfoShape, 0.5f * values[LfoDepth], 0.0f);
        core.setEnvelope(values[Env]);
        core.setBite(1.0f + values[Bite] * 6.0f);
    }

    std::array<float, NumCells> values {};
    swp::Core core;
    int lfoShape = 0;
};

// ---- MODFX --------------------------------------------------------------------
class ModFxBlock
{
public:
    enum Cell { ChorusRate = 0, ChorusDepth, FlangerRate, FlangerDepth, PhaserRate, PhaserDepth, RotarySpeed, RotaryDepth, NumCells };

    // The eight cells (no text is built here: played() reads them on the audio thread).
    static ParamSpec spec(int cell)
    {
        const int b = TrackFxParam::MODFX_BASE;
        const auto G = ParamKind::Log;
        switch (cell)
        {
            case ChorusRate:   return { "CH.R", b + ChorusRate, 0.1f, 5, 0.8f, G, "Hz" };
            case ChorusDepth:  return { "CH.D", b + ChorusDepth, 0, 1, 0 };
            case FlangerRate:  return { "FL.R", b + FlangerRate, 0.1f, 5, 0.3f, G, "Hz" };
            case FlangerDepth: return { "FL.D", b + FlangerDepth, 0, 1, 0 };
            case PhaserRate:   return { "PH.R", b + PhaserRate, 0.1f, 5, 0.5f, G, "Hz" };
            case PhaserDepth:  return { "PH.D", b + PhaserDepth, 0, 1, 0 };
            case RotarySpeed:  return { "RO.S", b + RotarySpeed, 0, 1, 0 };        // slow .. fast
            case RotaryDepth:  return { "RO.D", b + RotaryDepth, 0, 1, 0 };
            default:           return {};
        }
    }

    static ParamPage page()
    {
        ParamPage p;
        p.name = "MODFX";
        p.section = "FX";
        for (int cell = 0; cell < NumCells; ++cell)
            p.params[(size_t) cell] = spec(cell);
        return p;
    }

    // A ninth value, the LFO shape of all four (dsp/LFO.h): set(ShapeCell, index). The page's eight
    // cells are taken, so no knob reaches it yet; its id would be MODFX_BASE + ShapeCell.
    static constexpr int ShapeCell = NumCells;
    static ParamSpec shapeSpec() { return fxm::Lfo::shapeSpec("SHPE", TrackFxParam::MODFX_BASE + ShapeCell); }

    ModFxBlock()
    {
        for (int cell = 0; cell < NumCells; ++cell)
            values[(size_t) cell] = spec(cell).defaultVal;
        apply();
    }

    void prepare(double sr, int blockSize)
    {
        restSamples = (int) (0.05 * CellEffect::goodRate(sr));
        rest = {};
        chorus.prepare(sr, blockSize);
        flanger.prepare(sr, blockSize);
        phaser.prepare(sr, blockSize);
        rotary.prepare(sr, blockSize);
        apply();
    }

    void set(int cell, float v)
    {
        if (cell == ShapeCell && std::isfinite(v))
            shape = (int) fxm::Lfo::shapeOf((int) std::lround(std::clamp(v, 0.0f, 100.0f)));
        else if (cell >= 0 && cell < NumCells)
            values[(size_t) cell] = spec(cell).clamp(v);
        else
            return;
        apply();
    }
    float get(int cell) const
    {
        return cell == ShapeCell ? (float) shape : (cell >= 0 && cell < NumCells) ? values[(size_t) cell] : 0.0f;
    }

    // A cell moved from outside while it sounds: EffectBase::modulateCell's contract and
    // CellEffect's arithmetic (a share of the cell's range, added along its curve, staying until
    // set again). What the block's cells set are cells of its four effects, which glide.
    void modulateCell(int cell, float offset)
    {
        if (cell < 0 || cell >= NumCells) return;
        offset = CellEffect::offsetOf(offset);
        if (offset == offsets[(size_t) cell]) return;
        offsets[(size_t) cell] = offset;
        apply();
    }
    float getModulation(int cell) const { return (cell >= 0 && cell < NumCells) ? offsets[(size_t) cell] : 0.0f; }

    // What a cell plays: the knob and its offset.
    float played(int cell) const
    {
        const float offset = offsets[(size_t) cell];
        return offset == 0.0f ? values[(size_t) cell] : CellEffect::moved(spec(cell), values[(size_t) cell], offset);
    }

    // The track's custom LFO shape (what USR and USRS play); null = the rising staircase.
    void setUserShape(const float* levels)
    {
        chorus.setUserShape(levels);
        flanger.setUserShape(levels);
        phaser.setUserShape(levels);
        rotary.setUserShape(levels);
    }

    bool isActive() const
    {
        return on[0] || on[1] || on[2] || on[3] || rest[0] > 0 || rest[1] > 0 || rest[2] > 0 || rest[3] > 0;
    }

    void process(float* left, float* right, int numSamples)
    {
        run(phaser, 2, left, right, numSamples);
        run(flanger, 1, left, right, numSamples);
        run(chorus, 0, left, right, numSamples);
        run(rotary, 3, left, right, numSamples);
    }

private:
    // The depth knob brings the effect in (wet level rises over its first quarter) and then
    // deepens the modulation.
    static float wetFor(float depth) { return 0.5f * juce::jmin(1.0f, depth * 4.0f); }

    // An effect whose depth has come down to nothing (a knob, a lock, an offset from outside) runs
    // on for a moment: its mix is glided, and stopping at once would cut the rest of the glide off.
    void run(EffectBase& fx, int which, float* left, float* right, int numSamples)
    {
        if (on[(size_t) which])             rest[(size_t) which] = restSamples;
        else if (rest[(size_t) which] <= 0) return;
        else                                rest[(size_t) which] -= numSamples;
        fx.processBlock(left, right, numSamples);
    }

    void apply()
    {
        const float chorusDepth = played(ChorusDepth), flangerDepth = played(FlangerDepth), phaserDepth = played(PhaserDepth),
                    rotaryDepth = played(RotaryDepth);
        on = { chorusDepth > 0.0f, flangerDepth > 0.0f, phaserDepth > 0.0f, rotaryDepth > 0.0f };

        chorus.setParameter(Chorus::Rate, played(ChorusRate));
        chorus.setParameter(Chorus::Depth, chorusDepth);
        chorus.setParameter(Chorus::Voices, 3.0f);
        chorus.setParameter(Chorus::Mix, wetFor(chorusDepth));
        chorus.setExtra(Chorus::Shape, (float) shape);

        flanger.setParameter(Flanger::Rate, played(FlangerRate));
        flanger.setParameter(Flanger::Depth, flangerDepth);
        flanger.setParameter(Flanger::Feedback, 0.6f);
        flanger.setParameter(Flanger::Mix, wetFor(flangerDepth));
        flanger.setExtra(Flanger::Shape, (float) shape);

        phaser.setParameter(Phaser::Rate, played(PhaserRate));
        phaser.setParameter(Phaser::Depth, phaserDepth);
        phaser.setParameter(Phaser::Feedback, 0.5f);
        phaser.setParameter(Phaser::Stages, 6.0f);
        phaser.setMix(wetFor(phaserDepth));
        phaser.setExtra(Phaser::Shape, (float) shape);

        rotary.setParameter(RotarySpeaker::Speed, played(RotarySpeed));
        rotary.setMix(rotaryDepth);
        rotary.setExtra(RotarySpeaker::Shape, (float) shape);
    }

    std::array<float, NumCells> values {};
    std::array<float, NumCells> offsets {};
    std::array<bool, 4> on { false, false, false, false };
    std::array<int, 4> rest {};         // samples an effect still runs after its depth reached nothing
    int restSamples = 2400;
    int shape = 0;
    Chorus chorus;
    Flanger flanger;
    Phaser phaser;
    RotarySpeaker rotary;
};

struct TrackFx
{
    DirtBlock dirt;
    TalkBlock talk;
    ModFxBlock mod;

    void prepare(double sr, int blockSize)
    {
        dirt.prepare(sr, blockSize);
        talk.prepare(sr, blockSize);
        mod.prepare(sr, blockSize);
    }

    void process(float* left, float* right, int numSamples)
    {
        dirt.process(left, right, numSamples);
        talk.process(left, right, numSamples);
        if (mod.isActive())
            mod.process(left, right, numSamples);
    }

    void set(int id, float v)
    {
        if (id >= TrackFxParam::MODFX_BASE)     mod.set(id - TrackFxParam::MODFX_BASE, v);
        else if (id >= TrackFxParam::TALK_BASE) talk.set(id - TrackFxParam::TALK_BASE, v);
        else                                    dirt.set(id - TrackFxParam::DIRT_BASE, v);
    }

    float get(int id) const
    {
        if (id >= TrackFxParam::MODFX_BASE)     return mod.get(id - TrackFxParam::MODFX_BASE);
        if (id >= TrackFxParam::TALK_BASE)      return talk.get(id - TrackFxParam::TALK_BASE);
        return dirt.get(id - TrackFxParam::DIRT_BASE);
    }
};
