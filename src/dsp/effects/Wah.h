#pragma once

#include "SweptFilter.h"

// Wah: a setting of the swept filter (SweptFilter.h). A resonant band-pass swept by a level's
// envelope (auto-wah) and / or an LFO (dsp/LFO.h, any of its shapes). Structure: Zoelzer, "DAFX",
// filters chapter (wah-wah = swept band-pass) with a peak envelope follower; the filter is the
// project's trapezoidal state-variable filter, which stays tuned while its cutoff is moved.
//
// FREQ  resting centre frequency
// RESO  0..1 -> Q 1..12 on the state-variable filter (its band-pass scaled so the peak gain is
//       sqrt(Q): about 0 to +11 dB); the resonance of any other filter, the phaser's feedback, how
//       sharp the vowels are
// SENS  envelope amount: a full-level input raises the centre by up to 4 octaves
// RATE  LFO rate; 0 = LFO off, otherwise the centre swings +/- one octave
// FX2:
// SHPE  the LFO's shape: SQR is two vowels in turn, RND a new one every cycle, USR the track's
//       eight-step shape as a filter sequence, EXPD a "wow" on every cycle
// FILT  what is swept (swp::filt): the band-pass it always was (S.BP), any other type and mode of
//       the filter set (state-variable, SEM, ladder, diode, comb), a phaser of two, four or six
//       stages (heard with the dry sound, so its notches are there at any WET), or the vowels A to U
// TRIG  what a beginning sound does to the LFO (dsp/LFO.h): FREE nothing, KEY starts the cycle
//       again, ONE one cycle and stop, HALF half of one, HOLD the value it has is kept. With EXPD
//       and ONE: one "wow" for every note, or (KEY at IN) for every hit of another track
// KEY   whose level SENS follows and whose onsets TRIG hears: SELF, or IN = the track's input (the
//       source of the track's IN page). With no input: as SELF.
// The slot's WET leaves the dry sound beside the filtered one (1 = the filter alone).
// The cells are CellEffect's (every continuous one moves with modulateCell); FREQ, RESO and SENS
// glide to where they are put, so one thrown under sound is a sweep and not a click.
class Wah : public swp::Effect
{
public:
    enum Param { Freq = 0, Reso, Sens, Rate };
    enum Extra { Shape = 0, Filt, Trig };
    static constexpr int KEY_EXTRA = MAX_EXTRAS - 1;

    Wah() { seed(); }

    int keyCell() const override { return 4 + KEY_EXTRA; }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "FREQ", 0, 150.0f, 2500.0f, 400.0f, K::Log, "Hz" },
            { "RESO", 1, 0, 1, 0.5f },
            { "SENS", 2, 0, 1, 0.5f },
            { "RATE", 3, 0.0f, 10.0f, 0.0f, K::Linear, "Hz" },
            fxm::Lfo::shapeSpec("SHPE", Shape),
            { "FILT", Filt, 0, (float) (swp::filt::COUNT - 1), 0, K::Choice, "", swp::filt::names(), swp::filt::COUNT },
            fxm::Lfo::triggerSpec("TRIG", Trig),
            dyn::keySpec(KEY_EXTRA) };
        return c;
    }

    swp::Core::Voicing voicing() const override
    {
        swp::Core::Voicing v;
        v.peakLevel = true;
        v.cascadeSum = true;
        return v;
    }

    void configure(const float* v) override
    {
        const auto& filter = swp::filt::entry((int) v[4 + Filt]);
        core.setFilter(filter.type, filter.mode);
        core.setBase(v[Freq]);
        // (Just over 1: a state-variable resonance of 1 or less is taken as a knob's 0..1.)
        core.setResonance(filter.type == 0 ? std::max(1.0001f, std::pow(12.0f, v[Reso]))
                                           : filter.type == swp::filt::Phaser ? 0.9f * v[Reso] : v[Reso]);
        core.setEnvelope(v[Sens] * ENV_OCTAVES);
        core.setLfo(v[Rate], (int) v[4 + Shape], v[Rate] > LFO_OFF ? 1.0f : 0.0f, 0.0f);
        core.setTrigger((int) v[4 + Trig], 0.0f, 0.0f);
        core.setBite(2.8f);
    }

private:
    static constexpr float ENV_OCTAVES = 4.0f;
    static constexpr float LFO_OFF = 0.005f;
};
