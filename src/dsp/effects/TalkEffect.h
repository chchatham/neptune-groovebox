#pragma once

// TALK as an insert effect (CellEffect): a talking modulator — a vocal formant filter swept by
// hand, by an LFO and by the playing level. Formerly the TalkBlock of TrackFx.h; now an ordinary
// insert type.
//
// FX page:  VOWL (position A-E-I-O-U), SHFT (formant shift), RESO, MIX
// FX2 page: RATE (LFO rate), DPTH (LFO depth), ENV (envelope amount), BITE

#include "CellEffect.h"
#include "SweptFilter.h"
#include <array>
#include <cmath>

class TalkInsert : public CellEffect
{
public:
    enum Cell { Vowel = 0, Shift, Reso, Mix, LfoRate, LfoDepth, Env, Bite, NumCells };

    TalkInsert() { seed(); }

    const ParamSpec* cells() const override { return talkTable(); }
    int mixCell() const override { return Mix; }

    // Where the vowel is right now (0..1), including LFO and envelope, for display.
    float currentPosition() const { return core.currentPlace(); }

    static const char* vowelName(float pos)
    {
        static const char* const names[] = { "A", "E", "I", "O", "U" };
        return names[juce::jlimit(0, 4, (int) std::lround(pos * 4.0f))];
    }

    // VOWL shows the vowel it is nearest to, then the place as a percentage.
    juce::String cellText(int cell, float value) const override
    {
        if (cell != Vowel)
            return {};
        return juce::String(vowelName(value)) + " " + juce::String(juce::roundToInt(value * 100.0f));
    }

    // The track's custom LFO shape (what USR and USRS play); null = the rising staircase.
    void setUserShape(const float* levels) override { core.setUserShape(levels); }

    // The LFO's shape (a ninth value, like MODFX's was: set by an external call).
    void setLfoShape(int shape) { lfoShape = shape; touch(); }

protected:
    void configure(const float* v) override
    {
        core.setBase(v[Vowel]);
        core.setShift(v[Shift]);
        core.setResonance(v[Reso]);
        core.setLfo(v[LfoRate], lfoShape, 0.5f * v[LfoDepth], 0.0f);
        core.setEnvelope(v[Env]);
        core.setBite(1.0f + v[Bite] * 6.0f);
    }

    void prepareCore() override
    {
        swp::Core::Voicing v;
        v.law = swp::Core::Place;
        v.slots = false;
        v.baseMs = v.resoMs = v.depthMs = v.amountMs = 0.0f;
        v.attackMs = 5.0f;
        v.releaseMs = 150.0f;
        v.envGain = 3.0f;
        v.meanLevel = true;
        core.prepare(goodRate(sampleRate), v);
        core.setFilter(swp::filt::Vowel, 0);
    }

    void resetCore() override
    {
        core.reset();
    }

    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override
    {
        // The core runs its own mix: dry (1 - mix) + wet mix.
        core.run(left, right, n, left, right, mixFrom, mixTo);
    }

private:
    static const ParamSpec* talkTable()
    {
        static const ParamSpec t[8] = {
            { "VOWL", 0, 0, 1, 0 },
            { "SHFT", 1, -1, 1, 0 },
            { "RESO", 2, 0, 1, 0.5f },
            { "MIX",  3, 0, 1, 0 },
            { "RATE", 0, 0.05f, 12, 1, ParamKind::Log, "Hz" },
            { "DPTH", 1, 0, 1, 0 },
            { "ENV",  2, -1, 1, 0 },
            { "BITE", 3, 0, 1, 0.3f }
        };
        return t;
    }

    swp::Core core;
    int lfoShape = 0;
};
