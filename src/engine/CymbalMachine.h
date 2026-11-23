#pragma once

#include "../dsp/LevelCrusher.h"
#include "../dsp/DSPUtils.h"
#include "../dsp/Resonator.h"
#include "../dsp/filters/SVFilter.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// The cymbal machine (drum type CSYN): cymbals, rides, bells, crashes and gongs from one voice.
//
// Written from published principles (no samples, no code from elsewhere):
//   * The analog drum machines of about 1980 made every metal sound from six square-wave
//     oscillators at unrelated pitches, summed, split by two band-passes (about 3.4 and 7.1 kHz)
//     into bands with a decay each (K. Werner, J. Abel, J. Smith, their circuit analysis of
//     that cymbal voice in the proceedings of ICMC / SMC 2014, pp. 1453-1460; G. Reid, "Practical
//     cymbal synthesis", Sound On Sound, 2002). Later machines multiplied the squares in pairs.
//     Models METL and RING: they have no body, and keep their own few lines here.
//   * A real cymbal or gong is a plate with hundreds of inharmonic modes, each ringing for a
//     shorter time the higher it is. A light stroke gives a bell-like set of partials; a hard one
//     sends the energy up from the low modes it excites into the high ones within tens of
//     milliseconds (a crash) or over a second or more (a tam-tam's bloom), and the pitch of a
//     gong glides while it is loud (T. Rossing, "Science of Percussion Instruments"; K. Legge,
//     N. Fletcher, "Nonlinearity, chaos, and the sound of shallow gongs", JASA 1989; A. Chaigne,
//     C. Touze, O. Thomas, "Nonlinear vibrations and chaos in gongs and cymbals", Acoust. Sci. &
//     Tech. 2005; M. Ducceschi, C. Touze, "Modal approach for nonlinear vibrations of damped
//     impacted plates", JSV 2015).
//     Models RIDE, BELL, CRSH, CHNA, SPLS, GONG and TAM are seven points of the resonator's
//     latent space (res::cymbalPoints in dsp/Resonator.h: sheets and shells with heavy loss at the
//     top) on the shared bank of modes (dsp/ModeBank.h), played by the resonator's own exciter:
//       - the stroke is the exciter's strike: one impulse, as hard as SNAP: the partials (PING);
//       - the wash is the exciter FED: noise into the same body for as long as there is energy,
//         and the energy is an envelope that the stroke starts. That is the cascade: while the
//         cymbal is loud, energy keeps arriving in its high modes. Those are so heavily damped
//         (DAMP) that what they give is bands of hiss; the low ones ring as partials long after.
//         The feed reaches less far up as it falls (its brightness follows its level), so the top
//         goes first and a soft stroke is a later moment of a hard one: duller and shorter;
//       - BLOM lets the feed arrive late: the top comes after the stroke (a tam-tam's swell);
//       - a stroke ADDS to what is still ringing, in whatever phase it finds it.
//     EXC moves what plays it from the stroke to the exciter's bow, breath and scrape, where pad
//     pressure is the energy; COUP is the exciter's grip on the cymbal's own motion.
//     What the latent space does not hold, and is given up: partials placed by hand (a pair that
//     beats, a bell's own five), and a level for each mode.
//   * The sampled cymbals of the 1983 generation of drum machines were short recordings read from
//     a 6-bit memory at about 30 kHz, tuned by the read-out clock: the grain and the folded-over
//     highs are that sound (LOFI: a sample-and-hold and a coarse converter that follows the level).
//
// Nothing here allocates. A mode costs a rotation; modes that have rung out are not run.
class CymbalMachine
{
public:
    enum Model { Metal = 0, Ring, Ride, Bell, Crash, China, Splash, Gong, TamTam, NumModels };

    enum Param
    {
        ParModel = 0,   // MODL  which instrument
        ParPing,        // PING  0 = wash only .. 0.5 = both .. 1 = the stroke (the partials) only
        ParColour,      // COLR  dark .. the model's own (0.5) .. bright and thin
        ParExcite,      // EXC   what plays it: 0 the stroke .. 1 a bow .. 2 breath .. 3 a scrape
        ParDamp,        // DAMP  the loss at the top: the highs die sooner and turn from partials to hiss
        ParBloom,       // BLOM  the wash arrives late and from below (and a gong's pitch glides)
        ParCouple,      // COUP  the exciter's grip on the cymbal's own motion
        ParLofi,        // LOFI  the grain of an early sampled cymbal (0 = off)
        NumParams
    };

    static constexpr int NUM_MODES = res::MODES;
    static constexpr float NATURAL_TUNE = 8000.0f;      // TUNE at which a model has its own pitch
    static constexpr float MAX_EXCITE = (float) (res::NumExcitePoints - 1);

    static float paramDefault(int index)
    {
        static constexpr float defaults[NumParams] = { (float) Ride, 0.5f, 0.5f, 0.0f, 0.4f, 0.3f, 0.0f, 0.0f };
        return (index >= 0 && index < NumParams) ? defaults[index] : 0.0f;
    }
    static float paramMax(int index)
    {
        return index == ParModel ? (float) (NumModels - 1) : (index == ParExcite ? MAX_EXCITE : 1.0f);
    }

    // The first partial of a model's body at TUNE 8000, Hz (0: METL and RING have no body).
    static float modelPitch(int model) { return def(model).bank ? 0.0f : def(model).pitch; }

    static const char* const* modelNames()
    {
        static const char* const names[NumModels] = { "METL", "RING", "RIDE", "BELL", "CRSH", "CHNA", "SPLS", "GONG", "TAM" };
        return names;
    }

    void setSampleRate(double sr)
    {
        sampleRate = sr > 1000.0 ? sr : 44100.0;
        for (auto* f : { &hp, &clickBand, &bandLow, &bandHigh })
            f->setSampleRate(sampleRate);
        hp.setMode(SVFilter::Mode::HighPass);
        hp.setResonance(0.707f);
        clickBand.setMode(SVFilter::Mode::BandPass);
        clickBand.setResonance(0.8f);
        bandLow.setMode(SVFilter::Mode::BandPass);
        bandHigh.setMode(SVFilter::Mode::BandPass);
        bandLow.setResonance(BAND_Q);
        bandHigh.setResonance(BAND_Q);
        crusher.prepare(sampleRate);
        // The envelope, the pitch and the exciter are looked at about 750 times a second.
        controlSamples = std::clamp((int) std::lround(sampleRate / 750.0), 8, 512);
        pressK = 1.0f - std::exp(-(float) controlSamples / (0.008f * (float) sampleRate));
        jitterK = onePole(30.0f / 0.644f);
        jitterGain = noiseGain(jitterK);
        core.prepare(sampleRate);
        pressTarget = 0.0f;
        reset();
    }

    void setParam(int index, float value)
    {
        if (index >= 0 && index < NumParams)
            params[(size_t) index] = value;
    }
    float getParam(int index) const { return (index >= 0 && index < NumParams) ? params[(size_t) index] : 0.0f; }

    bool isActive() const { return active || fade != 0.0f; }
    int modesAwake() const { return active && ! def(struckModel).bank ? core.modesAwake() : 0; }

    // Pad pressure on DEC: the sound that is ringing rings `stretch` times as long while it lasts.
    void setDecayStretch(float stretch)
    {
        stretch = stretch >= 0.25f ? std::min(stretch, 8.0f) : 0.25f;       // (what is no number is the least)
        if (stretch != decayStretch)
        {
            decayStretch = stretch;
            coefsStale = true;
        }
    }

    // The pad's own pressure, 0..1: the energy of a bow, of breath, of a scrape (EXC from 1 up;
    // a stick has left the cymbal, and EXC 0 does not hear it).
    // A pad pressed again on a bowed cymbal that has rung out starts it again as it was last played.
    void setPressure(float pressure)
    {
        pressTarget = pressure > 0.0f ? std::min(pressure, 1.0f) : 0.0f;
        if (! active && pressTarget * fed > 0.0f)
        {
            active = true;
            coefsStale = true;
            control = 0;
        }
    }

    void reset()
    {
        core.reset();
        feed = feedTarget = 0.0f;
        energyNow = energyStep = 0.0f;
        pressNow = 0.0f;
        strokeHold = 0;
        envLow = envHighShort = envHighLong = 0.0f;
        targetLow = targetHighShort = targetHighLong = 0.0f;
        n1.fill(0.0f); n2.fill(0.0f);
        clickEnv = 0.0f;
        hp.reset(); clickBand.reset(); bandLow.reset(); bandHigh.reset();
        choking = false;
        crusher.reset();
        glide = 0.0f;
        fade = 0.0f; lastOut = 0.0f;
        active = false;
        control = 0;
    }

    // A hand on the cymbal: every mode is damped hard (gone in about a twentieth of a second).
    // The next stroke lifts the hand.
    void choke()
    {
        if (active && ! choking)
        {
            choking = true;
            coefsStale = true;
        }
    }

    struct Strike
    {
        float velocity = 1.0f;
        float tune = NATURAL_TUNE;      // Hz, 8000 = the model's own pitch
        float decay = 1.0f;             // the DEC cell, seconds
        float tone = 0.5f;
        float snap = 0.5f;              // 0 = a soft mallet .. 0.5 = a stick .. 1 = a hard tip, a loud tick
    };

    // A stroke. What still rings keeps ringing and the stroke adds to it (see the top of the file);
    // only a change of model starts from nothing (the old sound is faded over 2 ms).
    void strike(const Strike& hit)
    {
        const int model = params[ParModel] >= 0.0f ? (int) std::lround(std::min(params[ParModel], (float) (NumModels - 1))) : 0;
        if (active && model != struckModel)
        {
            const float carry = lastOut;
            reset();
            fade = carry;           // the old sound's last value glides to nothing
        }
        const bool ringing = active;
        if (! active)
            finish();
        struckModel = model;
        active = true;
        choking = false;        // (the coefficients are worked out again below)
        const ModelDef& m = def(model);

        const float vel = clamp01(hit.velocity);
        const float ping = clamp01(params[ParPing]), colour = clamp01(params[ParColour]);
        const float damp = clamp01(params[ParDamp]), bloom = clamp01(params[ParBloom]);
        const float tone = clamp01(hit.tone), snap = clamp01(hit.snap);
        const float dec = std::isfinite(hit.decay) ? std::clamp(hit.decay, 0.001f, 16.0f) : 1.0f;
        const float fs = (float) sampleRate;
        excite = std::isfinite(params[ParExcite]) ? std::clamp(params[ParExcite], 0.0f, MAX_EXCITE) : 0.0f;
        couple = clamp01(params[ParCouple]);
        // How much of it is fed (and hears the pad's pressure), how much is a stick's stroke.
        fed = std::min(1.0f, excite);
        const float stick = 1.0f - fed;

        tuneRatio = ratioFor(hit.tune);
        const float pingGain = ping <= 0.5f ? ping * 2.0f : 1.0f;
        const float washGain = ping >= 0.5f ? (1.0f - ping) * 2.0f : 1.0f;
        // A harder hit rings longer and (below) is brighter; a full hit is the sound as set.
        const float velLength = std::exp2(VEL_SENS * (vel - 1.0f) * 1.2f);
        const float velTilt = VEL_SENS * (vel - 1.0f) * 1.4f;
        // A soft mallet (SNAP below the middle) excites less of the top and takes a few ms to land.
        const float soft = snap < 0.5f ? (0.5f - snap) * 2.0f : 0.0f;
        const float hard = std::max(0.0f, (snap - 0.2f) / 0.8f);
        const float softAttack = 0.012f * soft * soft;
        // A harder tip is a little louder. A mallet is not quieter for being soft: what its
        // softness keeps out of the body (the resonator's strike is as loud as an impulse, and
        // knows nothing of the low-pass before it) is made up for, so the level does not hang on SNAP.
        const float tip = snap >= 0.5f ? 0.7f + 0.6f * snap : std::exp2(MALLET_EVEN * soft);

        // The stick itself, where there is no body to take it (METL, RING): a burst of noise,
        // shorter and brighter the harder the tip.
        if (m.click > 0.0f)
        {
            const float seconds = 0.0030f - 0.0022f * hard;
            clickDecay = std::exp(-1.0f / (seconds * fs));
            clickBand.setCutoff(std::min(0.45f * fs, (3000.0f + 5000.0f * hard) * std::sqrt(tuneRatio)));
            clickEnv = std::max(clickEnv, m.click * hard * hard * vel * (0.4f + 0.6f * vel) * 2.2f * stick);
        }
        // The thin voicing is the stick's: a bow, breath or a scrape is heard on the whole body (the
        // bow sings on its lowest partial).
        const float thin = m.bank ? m.hpHz : std::exp2((colour - 0.5f) * COLOUR_THIN + std::max(0.0f, tone - 0.5f) * 2.0f) * (fed < 1.0f ? m.hpHz * std::pow(FED_HP / m.hpHz, fed) : FED_HP);
        hp.setCutoff(std::clamp(thin * tuneRatio, 20.0f, 0.3f * fs));

        // What is fed: an envelope the stroke starts. Strokes add as powers do.
        const float cascade = (1.0f + m.cascade * bloom * vel * vel) / (1.0f + m.cascade * bloom);
        const float level = vel * cascade;
        washMix = m.bank ? 1.0f : washGain * m.washLevel;
        feedTarget = std::min(MAX_FEED, std::sqrt(feedTarget * feedTarget + level * level));
        feedAttack = 1.0f - std::exp(-(float) controlSamples / ((5.0e-4f + softAttack + m.bloomT * bloom * bloom) * fs));
        // (What rings is not cut short by a softer stroke.)
        const float ring = dec * m.ring * velLength;
        ringSeconds = ringing ? std::max(ringSeconds, ring) : ring;
        feedSeconds = std::max(0.004f, ringSeconds * m.feed);

        const res::ExciteLatent played = exciterAt(m, m.reach);

        if (m.bank)
        {
            // The oscillator bank: three bands, three decays.
            const float shift = std::exp2((colour - 0.5f) * 2.0f) * tuneRatio;
            bandLow.setCutoff(std::min(0.45f * fs, 3440.0f * shift));
            bandHigh.setCutoff(std::min(0.45f * fs, 7100.0f * shift));
            const float body = dec * velLength;
            logLow = -6.9078f / (std::max(0.005f, body) * fs);
            logHighLong = -6.9078f / (std::max(0.005f, body * std::exp2(-damp * 3.0f)) * fs);
            logHighShort = -6.9078f / (std::max(0.005f, 0.12f * velLength * (0.5f + dec * 0.5f)) * fs);
            attackLow = attackCoef(1.0e-4f + softAttack + bloom * bloom * 0.04f);
            attackHighLong = attackCoef(1.0e-4f + softAttack + bloom * bloom * 0.25f);
            attackHighShort = attackCoef(1.0e-4f + softAttack);
            const float bright = std::exp2((tone - 0.5f) * 3.0f + velTilt * 1.5f - soft);
            bankLowGain = 0.6f * pingGain;
            bankHighGain = 3.0f * washGain * bright;
            // The exciter's grain is the oscillators' unrest; its grip couples them in pairs
            // (METL towards RING's products, RING towards METL's sum).
            jitter = played[res::Grain] * played[res::Grain] * 0.04f;
            pairs = model == Ring ? 1.0f - couple : couple;
            targetLow = std::max(targetLow, vel * stick);
            targetHighShort = std::max(targetHighShort, vel * stick);
            targetHighLong = std::max(targetHighLong, vel * stick);
            outGain = m.gain;
            coefsStale = true;
            control = 0;
            return;
        }

        // ---- the body ------------------------------------------------------------------------
        // The model's point of the latent space; DAMP is its loss (0.4: the point's own), COLR
        // and TONE its tilt.
        res::BodyLatent body = res::cymbalPoints()[(size_t) m.point];
        body[res::Damp] = damp <= 0.4f ? body[res::Damp] * damp * 2.5f
                                       : body[res::Damp] + (1.0f - body[res::Damp]) * (damp - 0.4f) * (1.0f / 0.6f);
        body[res::Tilt] = clamp01(body[res::Tilt] - (colour - 0.5f) * COLOUR_TILT - (tone - 0.5f) * 0.3f);
        core.setBody(body);

        // The feed reaches less far up as it falls, the more the higher the loss; a darker COLR
        // lowers it as a whole.
        reach = m.reach + std::min(0.0f, colour - 0.5f) * COLOUR_REACH;
        reachSlope = REACH_SLOPE * damp;

        // The stroke: the exciter's strike for three milliseconds (one impulse through its
        // low-pass, as hard as SNAP and the velocity make it), then the feed takes over. With BLOM
        // the stroke itself leaves the top out. A bow or a scrape has no stroke: nothing is taken
        // at once, the exciter moves to where it is wanted.
        if (stick > 0.0f)
        {
            const float strokeHard = m.strokeHard + (snap - 0.5f) * (snap < 0.5f ? MALLET_HARD : TIP_HARD) + velTilt * 0.15f + (tone - 0.5f) * 0.2f
                                   + std::min(0.0f, colour - 0.5f) * COLOUR_REACH - bloom * m.hide;
            core.setExciter(res::ExciteLatent { { 0.0f, 0.0f, 0.0f, 0.0f, clamp01(strokeHard) } });
            core.setPitch(m.pitch * tuneRatio * std::exp2(m.glide * bloom * vel * vel / 12.0f));
            core.setDecay(ringSeconds * decayStretch);
            core.strike(vel * stick * pingGain * m.pingLevel * tip);
            strokeHold = (int) (STROKE_SECONDS * fs);
        }
        else if (! ringing)
        {
            // (Where the exciter grips, its strike is the first bite of a bow.)
            core.setExciter(played);
            core.setPitch(m.pitch * tuneRatio);
            core.setDecay(ringSeconds * decayStretch);
            core.strike(vel);
        }

        // A gong's pitch starts off where the stroke bends it and settles.
        glide = m.glide * bloom * vel * vel;
        glideDecay = std::exp(-(float) controlSamples / (std::max(0.02f, m.glideT) * fs));

        outGain = m.gain + (FED_GAIN - m.gain) * fed;
        coefsStale = true;
        control = 0;
    }

    // One sample. tuneHz: TUNE as it is now (bend, LFO and pressure move a ringing cymbal).
    float process(float tuneHz)
    {
        if (active && --control <= 0)
            controlTick(tuneHz);        // (it may find that the sound is over)
        if (! active)
        {
            if (fade == 0.0f)
                return 0.0f;
            return fadeOut(0.0f);
        }

        energyNow += energyStep;
        float out = def(struckModel).bank ? processBank() : core.tick(0.0f, energyNow);

        if (clickEnv > 1.0e-5f)
        {
            out += clickBand.process(nextNoise() * clickEnv);
            clickEnv *= clickDecay;
        }
        // (The offset keeps the high-pass out of the smallest numbers while a silent body is still fed.)
        out = hp.process(out + 1.0e-18f) * outGain;

        const float lofi = params[ParLofi];
        if (lofi > 0.001f)
            out = crusher.process(out, std::min(lofi, 1.0f));     // LOFI (dsp/LevelCrusher.h)

        // What fits is untouched. The tick of a stroke (every partial of the body moving off in
        // step: a peak many times the level of what follows) is bent to half as much again.
        out = res::outLimit(fadeOut(out), OUT_LIMIT);
        lastOut = out;
        return out;
    }

private:
    // ---- the models --------------------------------------------------------------------------------
    // Pitches are at TUNE 8000.
    struct ModelDef
    {
        bool bank;                      // the six-oscillator bank instead of a body
        float squares[6];               // ... and its pitches
        int point;                      // the body: which of res::cymbalPoints
        float pitch;                    // its first partial
        float ring;                     // seconds that partial rings (to -60 dB), per second of DEC
        float feed;                     // how long the wash is fed, as a part of that
        float reach;                    // how far up the feed reaches at full level (the exciter's HARD)
        float grain;                    // what the feed is made of (the exciter's GRAN: 1 = noise)
        float pingLevel, washLevel;     // the stroke and the wash (PING at the middle). A body made by its stroke
                                        // (BELL, GONG, TAM) keeps the stroke at 0.85: a soft mallet's make-up then fits the exciter's most
        float strokeHard;               // the stick's HARD at SNAP 0.5
        float bloomT;                   // seconds the wash takes to arrive with BLOM at the top
        float cascade;                  // how much more of the wash a full hit gets than a soft one, with BLOM
        float hide;                     // how much of the top the stroke itself leaves out with BLOM at the top (in HARD)
        float glide, glideT;            // semitones the pitch starts off by with BLOM at the top; its time
        float click;                    // the stick, where there is no body
        float hpHz;
        float gain;
    };

    static const ModelDef& def(int model)
    {
        static const ModelDef defs[NumModels] = {
            // METL: six squares, summed.
            { true, { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f }, 0, 0, 1.0f, 0.5f, 0, 0.0f, 1, 1, 0, 0.1f, 0.0f, 0, 0, 0.1f, 0.3f, 2000.0f, 0.85f },
            // RING: the squares multiplied in pairs: many more partials, none of them a square's own.
            { true, { 263.0f, 414.5f, 317.2f, 587.1f, 731.0f, 1147.0f }, 0, 0, 1.0f, 0.5f, 0, 0.25f, 1, 1, 0, 0.1f, 0.0f, 0, 0, 0.1f, 0.3f, 1600.0f, 0.5f },
            // RIDE: a stick on the bow of a heavy cymbal: a clear ping over a quiet, long wash.
            { false, {}, res::CymRide, 240.0f, 5.0f, 1.0f, 0.93f, 1.0f, 1.2f, 0.6f, 0.8f, 0.06f, 1.2f, 0.1f, 0.0f, 0.1f, 0.0f, 2000.0f, 1.5f },
            // BELL: the cup of the ride: strong, long partials, hardly a wash.
            { false, {}, res::CymBell, 622.0f, 4.0f, 0.3f, 0.7f, 1.0f, 0.85f, 0.34f, 0.7f, 0.05f, 1.5f, 0.05f, 0.0f, 0.1f, 0.0f, 300.0f, 0.97f },
            // CRSH: nearly all wash, wide and long; the low partials are left humming at the end.
            { false, {}, res::CymCrash, 175.0f, 4.7f, 0.93f, 0.95f, 1.0f, 0.4f, 1.0f, 0.86f, 0.06f, 1.2f, 0.1f, 0.0f, 0.1f, 0.0f, 1800.0f, 2.3f },
            // CHNA: short, uneven and harsh in the middle.
            { false, {}, res::CymChina, 190.0f, 2.0f, 0.9f, 0.92f, 1.0f, 0.8f, 1.0f, 0.88f, 0.03f, 1.5f, 0.1f, 0.0f, 0.1f, 0.0f, 1400.0f, 2.4f },
            // SPLS: small, high and quickly gone.
            { false, {}, res::CymSplash, 240.0f, 2.0f, 1.0f, 0.92f, 1.0f, 0.9f, 1.0f, 0.86f, 0.015f, 1.0f, 0.05f, 0.0f, 0.1f, 0.0f, 3000.0f, 1.3f },
            // GONG: a gong with a note; its pitch falls as it settles.
            { false, {}, res::CymGong, 196.0f, 8.0f, 0.35f, 0.6f, 1.0f, 0.85f, 0.855f, 0.7f, 0.5f, 2.0f, 0.2f, 0.45f, 0.8f, 0.0f, 60.0f, 1.36f },
            // TAM: a tam-tam: a dense low cluster at once, the shimmer blooming above it afterwards.
            { false, {}, res::CymTam, 71.0f, 12.0f, 0.4f, 0.9f, 1.0f, 0.85f, 0.45f, 0.75f, 1.0f, 4.0f, 0.3f, 0.3f, 1.0f, 0.0f, 35.0f, 4.36f } };
        return defs[std::clamp(model, 0, (int) NumModels - 1)];
    }

    static constexpr float BAND_Q = 2.0f;
    static constexpr float CHOKE_SECONDS = 0.06f;   // to -60 dB under the hand
    static constexpr float STROKE_SECONDS = 0.003f; // the exciter is a stick for this long
    static constexpr float VEL_SENS = 0.55f;        // how much a harder hit is brighter and longer
    static constexpr float MAX_FEED = 1.6f;         // what a roll can build the feed to
    static constexpr float FED_HP = 90.0f;          // the high-pass under a bow (the stick's is the model's)
    static constexpr float FED_GAIN = 0.5f;         // ... and the gain
    static constexpr float OUT_LIMIT = 1.5f;
    static constexpr float FEED_OVER = 1.0e-4f;     // the feed's end
    static constexpr float SILENT = 1.0e-10f;       // what the body holds when the voice is let go (-100 dB)
    static constexpr float REACH_SLOPE = 0.35f;      // HARD the feed loses per halving of its level, at DAMP 1
    static constexpr float COLOUR_TILT = 0.5f, COLOUR_REACH = 0.6f, COLOUR_THIN = 3.0f, MALLET_HARD = 1.4f, TIP_HARD = 0.5f;
    static constexpr float MALLET_EVEN = 1.2f;      // octaves of level the softest mallet is made up by

    static float clamp01(float v) { return v >= 0.0f ? std::min(v, 1.0f) : 0.0f; }     // (not-a-number is 0)
    static float ratioFor(float tuneHz) { return tuneHz >= 0.0f ? std::clamp(tuneHz / NATURAL_TUNE, 0.125f, 2.0f) : 1.0f; }

    // The exciter at EXC: the cymbal's own feed at 0 (noise as the model grains it, reaching as far
    // up as `hard`), then the resonator's bow, breath and scrape (dsp/Resonator.h), latent by
    // latent between two neighbours; COUP is added to its grip.
    res::ExciteLatent exciterAt(const ModelDef& m, float hard) const
    {
        const auto& points = res::excitePoints();
        const res::ExciteLatent own { { 1.0f, m.grain, 0.0f, 0.0f, hard } };
        const int a = std::min((int) excite, (int) res::NumExcitePoints - 2);
        const float t = excite - (float) a;
        res::ExciteLatent out;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const float from = a == 0 ? own[i] : points[(size_t) a][i];
            out[i] = clamp01(from + (points[(size_t) a + 1][i] - from) * t);
        }
        out[res::Couple] = clamp01(out[res::Couple] + couple);
        return out;
    }

    float onePole(float hertz) const
    {
        return 1.0f - std::exp(-dsp::TWO_PI_F * std::clamp(hertz, 0.2f, (float) sampleRate * 0.2f) / (float) sampleRate);
    }
    float attackCoef(float seconds) const { return 1.0f - std::exp(-1.0f / (seconds * (float) sampleRate)); }

    // White noise in -1..1 through two equal one-poles with coefficient a: the gain that brings it
    // back to a variance of one.
    static float noiseGain(float a)
    {
        const double p = 1.0 - (double) a, q = 1.0 - p * p;
        const double variance = (1.0 / 3.0) * std::pow((double) a, 4.0) * (1.0 + p * p) / (q * q * q);
        return (float) (1.0 / std::sqrt(std::max(variance, 1.0e-20)));
    }

    float nextNoise()
    {
        noiseSeed = noiseSeed * 1664525u + 1013904223u;
        return (float) (int32_t) noiseSeed * (1.0f / 2147483648.0f);
    }

    void controlTick(float tuneHz)
    {
        control = controlSamples;
        const ModelDef& m = def(struckModel);
        const float fs = (float) sampleRate;
        float ratio = ratioFor(tuneHz);
        if (glide != 0.0f)
        {
            ratio *= std::exp2(glide / 12.0f);
            glide *= glideDecay;
            if (std::abs(glide) < 1.0e-3f)
                glide = 0.0f;
        }
        const bool moved = ratio != tuneRatio || coefsStale;
        if (moved)
        {
            tuneRatio = ratio;
            coefsStale = false;
            feedDecay = std::exp(-6.9078f * (float) controlSamples / (feedSeconds * decayStretch * fs));
        }

        // The feed's envelope and the pad's pressure, a straight line from one look to the next.
        // (A hand on it: nothing is fed any more.)
        if (choking)
            feedTarget = feed = pressNow = 0.0f;
        // (It rises as slowly as BLOM says and falls as fast as it is told to. 80 dB down it is
        // over: what it still fed would keep every partial of the body awake for nothing.)
        feedTarget *= feedDecay;
        if (feedTarget < FEED_OVER) feedTarget = 0.0f;
        feed += (feedTarget > feed ? feedAttack : std::max(feedAttack, 1.0f - feedDecay)) * (feedTarget - feed);
        if (feedTarget <= 0.0f && feed < FEED_OVER) feed = 0.0f;
        const float pressed = choking ? 0.0f : pressTarget * fed;
        pressNow += pressK * (pressed - pressNow);
        if (pressNow < 1.0e-6f) pressNow = 0.0f;
        const float energy = std::min(res::Resonator::MAX_ENERGY, (m.bank ? fed : washMix) * feed + pressNow);
        energyStep = (energy - energyNow) / (float) controlSamples;
        const bool fedOut = energy <= 0.0f && energyNow < 1.0e-5f && pressed <= 0.0f;

        if (m.bank)
        {
            if (moved)
            {
                const float hand = choking ? -6.9078f / (CHOKE_SECONDS * fs) : 0.0f;
                for (int i = 0; i < 6; ++i)
                    inc[(size_t) i] = std::min(0.45, (double) (m.squares[i] * ratio) / sampleRate);
                decayLow = std::exp(logLow / decayStretch + hand);
                decayHighLong = std::exp(logHighLong / decayStretch + hand);
                decayHighShort = std::exp(logHighShort + hand);
            }
            if (fedOut && targetLow < 1.0e-4f && targetHighLong < 1.0e-4f && targetHighShort < 1.0e-4f
                && envLow < 1.0e-4f && envHighLong < 1.0e-4f && clickEnv < 1.0e-4f)
                finish();
            return;
        }

        if (moved)
        {
            core.setPitch(m.pitch * ratio);
            if (! choking)
                core.setDecay(ringSeconds * decayStretch);
        }
        // (The hand damps the body once the feed has stopped: the exciter makes a short body's
        // feed louder, and what was still being fed would burst out.)
        if (choking && energyNow < 1.0e-4f)
            core.setDecay(CHOKE_SECONDS);
        if (strokeHold > 0)
            strokeHold -= controlSamples;
        else
        {
            // The feed's brightness follows its level (see the top of the file).
            const float level = (feed + pressNow) > 1.0e-6f ? std::log2(feed + pressNow) : -20.0f;
            core.setExciter(exciterAt(m, clamp01(reach + reachSlope * level)));
            if (fedOut && core.held() < SILENT)
                finish();
        }
    }

    void finish()
    {
        const float carry = fade;
        reset();
        fade = carry;
    }

    float processBank()
    {
        float sq[6];
        for (int i = 0; i < 6; ++i)
        {
            const size_t k = (size_t) i;
            double step = inc[k];
            if (jitter > 0.0f)
            {
                seeds[k] = seeds[k] * 1664525u + 1013904223u;
                const float white = (float) (int32_t) seeds[k] * (1.0f / 2147483648.0f);
                n1[k] += jitterK * (white - n1[k]);
                n2[k] += jitterK * (n1[k] - n2[k]);
                step *= 1.0 + (double) (jitter * jitterGain * n2[k]);
                step = std::clamp(step, 1.0e-5, 0.45);
            }
            double& ph = phase[k];
            ph += step;
            if (ph >= 1.0) ph -= 1.0;
            float v = ph < 0.5 ? 1.0f : -1.0f;
            v += dsp::polyBLEP(ph, step);
            double half = ph + 0.5;
            if (half >= 1.0) half -= 1.0;
            v -= dsp::polyBLEP(half, step);
            sq[i] = v;
        }
        const float summed = pairs < 1.0f ? (sq[0] + sq[1] + sq[2] + sq[3] + sq[4] + sq[5]) * (1.0f / 6.0f) : 0.0f;
        const float sum = pairs <= 0.0f ? summed
                        : summed + pairs * ((sq[0] * sq[1] + sq[2] * sq[3] + sq[4] * sq[5]) * (1.0f / 3.0f) - summed);

        // (What is fed - a bow, the pad's pressure - holds the bands up.)
        targetLow *= decayLow;
        targetHighShort *= decayHighShort;
        targetHighLong *= decayHighLong;
        envLow += attackLow * (std::max(targetLow, energyNow) - envLow);
        envHighShort += attackHighShort * (std::max(targetHighShort, energyNow) - envHighShort);
        envHighLong += attackHighLong * (std::max(targetHighLong, energyNow) - envHighLong);

        return bankLowGain * bandLow.process(sum) * envLow
             + bankHighGain * bandHigh.process(sum) * (0.45f * envHighShort + 0.55f * envHighLong);
    }

    // The 2 ms glide to nothing of a sound that was cut off by a change of model.
    float fadeOut(float out)
    {
        if (fade != 0.0f)
        {
            out += fade;
            fade *= fadeCoef();
            if (std::abs(fade) < 1.0e-5f)
                fade = 0.0f;
        }
        return out;
    }
    float fadeCoef() const { return std::exp(-1.0f / (0.0007f * (float) sampleRate)); }

    double sampleRate = 44100.0;
    std::array<float, NumParams> params { { (float) Ride, 0.5f, 0.5f, 0.0f, 0.4f, 0.3f, 0.0f, 0.0f } };
    bool active = false;
    int struckModel = Ride;
    int control = 0, controlSamples = 64;
    float tuneRatio = 1.0f;
    float decayStretch = 1.0f;
    bool coefsStale = true;
    float outGain = 1.0f;

    // The body and what plays it (dsp/Resonator.h on dsp/ModeBank.h).
    res::Resonator core;
    float excite = 0.0f, couple = 0.0f, fed = 0.0f;         // EXC and COUP as struck; EXC's fed part
    float feed = 0.0f, feedTarget = 0.0f, feedAttack = 1.0f, feedDecay = 0.0f;
    float ringSeconds = 1.0f, feedSeconds = 1.0f;
    float reach = 0.8f, reachSlope = 0.04f, washMix = 1.0f;
    float energyNow = 0.0f, energyStep = 0.0f;
    float pressTarget = 0.0f, pressNow = 0.0f, pressK = 0.1f;
    int strokeHold = 0;
    float glide = 0.0f, glideDecay = 0.0f;

    // The oscillator bank.
    std::array<double, 6> phase { { 0.0, 0.31, 0.62, 0.17, 0.83, 0.45 } };
    std::array<double, 6> inc {};
    std::array<float, 6> n1 {}, n2 {};                      // each oscillator's slow noise
    float jitterK = 0.0f, jitterGain = 1.0f;                // ... its rate and what brings it to a variance of one
    std::array<uint32_t, 6> seeds { { 0x9e3779b9u, 0x3c6ef372u, 0xdaa66d2bu, 0x78dde6e4u, 0x1715609du, 0xb54cda56u } };
    SVFilter bandLow, bandHigh;
    float jitter = 0.0f, pairs = 0.0f;
    float bankLowGain = 1.0f, bankHighGain = 1.0f;
    float envLow = 0.0f, envHighShort = 0.0f, envHighLong = 0.0f;
    float targetLow = 0.0f, targetHighShort = 0.0f, targetHighLong = 0.0f;
    float logLow = 0.0f, logHighLong = 0.0f, logHighShort = 0.0f;
    float decayLow = 0.0f, decayHighLong = 0.0f, decayHighShort = 0.0f;
    float attackLow = 1.0f, attackHighLong = 1.0f, attackHighShort = 1.0f;

    // The stick, the output.
    SVFilter hp, clickBand;
    float clickEnv = 0.0f, clickDecay = 0.0f;
    uint32_t noiseSeed = 0x2545f491u;
    bool choking = false;
    dsp::LevelCrusher crusher;      // LOFI
    float fade = 0.0f, lastOut = 0.0f;
};
