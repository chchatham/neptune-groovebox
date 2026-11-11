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
// RateReducer, Chorus, Flanger, Phaser, RotarySpeaker) and filters (CombFilter,
// SVFilter); nothing here is new low-level DSP.
// -----------------------------------------------------------------------------

#include "Waveshaper.h"
#include "RateReducer.h"
#include "Chorus.h"
#include "Flanger.h"
#include "Phaser.h"
#include "RotarySpeaker.h"
#include "../filters/CombFilter.h"
#include "../filters/SVFilter.h"
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
        static const char* const types[] = { "SOFT", "HARD", "FOLD", "ASYM" };
        ParamPage p;
        p.name = "DIRT";
        p.section = "FX";
        const int b = TrackFxParam::DIRT_BASE;
        p.params = {{
            { "DRIV", b + Drive, 0, 1, 0 },
            { "TYPE", b + Type, 0, 3, 0, ParamKind::Choice, "", types, 4 },
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
// Three parallel band-pass filters placed on the first three formants of a vowel. The vowel
// position moves continuously through A - E - I - O - U; it can be set by hand, swept by an LFO
// and pushed by the level of the playing, so the sound "talks". Formant frequencies and relative
// levels are standard phonetics values for a male voice (as tabulated e.g. in the Csound manual).
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
    }

    void prepare(double sr, int /*blockSize*/)
    {
        sampleRate = sr;
        for (auto& ch : filters)
            for (auto& f : ch)
            {
                f.setSampleRate(sr);
                f.setMode(SVFilter::Mode::BandPass);
                f.reset();
            }
        updateCountdown = 0;
    }

    void set(int cell, float v)
    {
        if (cell >= 0 && cell < NumCells)
            values[(size_t) cell] = page().params[(size_t) cell].clamp(v);
    }
    float get(int cell) const { return (cell >= 0 && cell < NumCells) ? values[(size_t) cell] : 0.0f; }

    bool isActive() const { return values[Mix] > 0.0f; }

    // Where the vowel is right now (0..1), including LFO and envelope, for display.
    float currentPosition() const { return position; }

    void process(float* left, float* right, int numSamples)
    {
        if (!isActive())
            return;

        const float mix = values[Mix];
        const float biteGain = 1.0f + values[Bite] * 6.0f;
        const float attack = 1.0f - std::exp(-1.0f / (0.005f * (float) sampleRate));
        const float release = 1.0f - std::exp(-1.0f / (0.150f * (float) sampleRate));

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = right[i];

            // Envelope follower on the input level.
            float level = 0.5f * (std::abs(inL) + std::abs(inR));
            envelope += (level > envelope ? attack : release) * (level - envelope);

            if (--updateCountdown <= 0)
            {
                updateCountdown = UPDATE_INTERVAL;
                lfoPhase += (double) values[LfoRate] * UPDATE_INTERVAL / sampleRate;
                lfoPhase -= std::floor(lfoPhase);
                float lfo = std::sin((float) lfoPhase * 6.2831853f);
                position = juce::jlimit(0.0f, 1.0f, values[Vowel]
                                                    + 0.5f * values[LfoDepth] * lfo
                                                    + values[Env] * juce::jmin(1.0f, envelope * 3.0f));
                updateFormants();
            }

            // A little saturation first gives the formants harmonics to pick out.
            float xL = dsp::fastTanh(inL * biteGain);
            float xR = dsp::fastTanh(inR * biteGain);

            float outL = 0.0f, outR = 0.0f;
            for (int f = 0; f < 3; ++f)
            {
                outL += filters[0][(size_t) f].process(xL) * gains[(size_t) f];
                outR += filters[1][(size_t) f].process(xR) * gains[(size_t) f];
            }

            left[i] = inL + mix * (outL * MAKEUP - inL);
            right[i] = inR + mix * (outR * MAKEUP - inR);
        }
    }

private:
    static constexpr int UPDATE_INTERVAL = 16;
    static constexpr float MAKEUP = 2.0f;

    void updateFormants()
    {
        //                               F1      F2      F3       levels (F2, F3 lifted for clarity)
        static const float table[5][6] = { { 650.0f, 1080.0f, 2650.0f, 1.0f, 0.70f, 0.50f },    // A
                                           { 400.0f, 1700.0f, 2600.0f, 1.0f, 0.45f, 0.40f },    // E
                                           { 290.0f, 1870.0f, 2800.0f, 1.0f, 0.40f, 0.30f },    // I
                                           { 400.0f,  800.0f, 2600.0f, 1.0f, 0.55f, 0.35f },    // O
                                           { 350.0f,  600.0f, 2700.0f, 1.0f, 0.35f, 0.30f } };  // U
        float p = position * 4.0f;
        int a = juce::jlimit(0, 3, (int) p);
        float t = p - (float) a;
        const float shift = std::exp2(values[Shift]);
        const float q = 3.0f + values[Reso] * values[Reso] * 27.0f;

        for (int f = 0; f < 3; ++f)
        {
            // Frequencies move on a log scale between neighbouring vowels.
            float hz = table[a][f] * std::pow(table[a + 1][f] / table[a][f], t) * shift;
            float amp = table[a][f + 3] + t * (table[a + 1][f + 3] - table[a][f + 3]);
            for (auto& ch : filters)
            {
                ch[(size_t) f].setResonance(q);
                ch[(size_t) f].setCutoff(juce::jlimit(60.0f, (float) (sampleRate * 0.45), hz));
            }
            // The band output peaks at Q: bring it back to the formant's level, with a little extra
            // loudness as the bands narrow.
            gains[(size_t) f] = amp / q * (1.0f + values[Reso]);
        }
    }

    std::array<float, NumCells> values {};
    std::array<std::array<SVFilter, 3>, 2> filters;
    std::array<float, 3> gains { 1.0f, 0.5f, 0.3f };
    double sampleRate = 44100.0;
    double lfoPhase = 0.0;
    float envelope = 0.0f;
    float position = 0.0f;
    int updateCountdown = 0;
};

// ---- MODFX --------------------------------------------------------------------
class ModFxBlock
{
public:
    enum Cell { ChorusRate = 0, ChorusDepth, FlangerRate, FlangerDepth, PhaserRate, PhaserDepth, RotarySpeed, RotaryDepth, NumCells };

    static ParamPage page()
    {
        ParamPage p;
        p.name = "MODFX";
        p.section = "FX";
        const int b = TrackFxParam::MODFX_BASE;
        auto G = ParamKind::Log;
        p.params = {{
            { "CH.R", b + ChorusRate, 0.1f, 5, 0.8f, G, "Hz" },
            { "CH.D", b + ChorusDepth, 0, 1, 0 },
            { "FL.R", b + FlangerRate, 0.1f, 5, 0.3f, G, "Hz" },
            { "FL.D", b + FlangerDepth, 0, 1, 0 },
            { "PH.R", b + PhaserRate, 0.1f, 5, 0.5f, G, "Hz" },
            { "PH.D", b + PhaserDepth, 0, 1, 0 },
            { "RO.S", b + RotarySpeed, 0, 1, 0 },        // slow .. fast
            { "RO.D", b + RotaryDepth, 0, 1, 0 } }};
        return p;
    }

    ModFxBlock()
    {
        for (auto& spec : page().params)
            values[(size_t) (spec.id - TrackFxParam::MODFX_BASE)] = spec.defaultVal;
        apply();
    }

    void prepare(double sr, int blockSize)
    {
        chorus.prepare(sr, blockSize);
        flanger.prepare(sr, blockSize);
        phaser.prepare(sr, blockSize);
        rotary.prepare(sr, blockSize);
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
        return values[ChorusDepth] > 0.0f || values[FlangerDepth] > 0.0f || values[PhaserDepth] > 0.0f || values[RotaryDepth] > 0.0f;
    }

    void process(float* left, float* right, int numSamples)
    {
        if (values[PhaserDepth] > 0.0f)  phaser.processBlock(left, right, numSamples);
        if (values[FlangerDepth] > 0.0f) flanger.processBlock(left, right, numSamples);
        if (values[ChorusDepth] > 0.0f)  chorus.processBlock(left, right, numSamples);
        if (values[RotaryDepth] > 0.0f)  rotary.processBlock(left, right, numSamples);
    }

private:
    // The depth knob brings the effect in (wet level rises over its first quarter) and then
    // deepens the modulation.
    static float wetFor(float depth) { return 0.5f * juce::jmin(1.0f, depth * 4.0f); }

    void apply()
    {
        chorus.setParameter(Chorus::Rate, values[ChorusRate]);
        chorus.setParameter(Chorus::Depth, values[ChorusDepth]);
        chorus.setParameter(Chorus::Mix, wetFor(values[ChorusDepth]));

        flanger.setParameter(Flanger::Rate, values[FlangerRate]);
        flanger.setParameter(Flanger::Depth, values[FlangerDepth]);
        flanger.setParameter(Flanger::Feedback, 0.6f);
        flanger.setParameter(Flanger::Mix, wetFor(values[FlangerDepth]));

        phaser.setParameter(Phaser::Rate, values[PhaserRate]);
        phaser.setParameter(Phaser::Depth, values[PhaserDepth]);
        phaser.setParameter(Phaser::Feedback, 0.5f);
        phaser.setParameter(Phaser::Mix, wetFor(values[PhaserDepth]));

        rotary.setParameter(RotarySpeaker::Speed, values[RotarySpeed]);
        rotary.setParameter(RotarySpeaker::Mix, values[RotaryDepth]);
    }

    std::array<float, NumCells> values {};
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
