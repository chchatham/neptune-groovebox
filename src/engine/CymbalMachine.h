#pragma once

#include "../dsp/DSPUtils.h"
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
//     Models METL and RING.
//   * A real cymbal or gong is a plate with hundreds of inharmonic modes, more of them per octave
//     the higher one looks, each ringing for a shorter time the higher it is. A light stroke gives
//     a bell-like set of partials; a hard one sends the energy up from the low modes it excites
//     into the high ones within tens of milliseconds (a crash) or over a second or more (a
//     tam-tam's bloom), and the pitch of a gong glides while it is loud (T. Rossing, "Science of
//     Percussion Instruments"; K. Legge, N. Fletcher, "Nonlinearity, chaos, and the sound of
//     shallow gongs", JASA 1989; A. Chaigne, C. Touze, O. Thomas, "Nonlinear vibrations and chaos
//     in gongs and cymbals", Acoust. Sci. & Tech. 2005; M. Ducceschi, C. Touze, "Modal approach
//     for nonlinear vibrations of damped impacted plates", JSV 2015).
//     Models RIDE, BELL, CRSH, CHNA, SPLS, GONG and TAM are a bank of 48 damped modes:
//       - 12 "ping" modes: the partials one hears as the stick's note, pure tones;
//       - 36 "wash" modes spread from the low mids to the top, each standing for the cluster of
//         real modes around it: its amplitude is moved by its own slow noise, so it is a narrow
//         band of noise that rings (NOIS: from pure partials to hiss);
//       - every mode has its own decay, shorter the higher it is (DAMP), and the high wash
//         arrives late and louder the harder the stroke: the cascade, drawn as an envelope (BLOM);
//       - a stroke ADDS to what is still ringing, in whatever phase it finds it: a ride pattern
//         builds its wash and no two strokes are the same.
//   * The sampled cymbals of the 1983 generation of drum machines were short recordings read from
//     a 6-bit memory at about 30 kHz, tuned by the read-out clock: the grain and the folded-over
//     highs are that sound (LOFI: a sample-and-hold and a coarse converter that follows the level).
//
// Nothing here allocates. One mode costs a rotation, a random number and two one-pole smoothers.
class CymbalMachine
{
public:
    enum Model { Metal = 0, Ring, Ride, Bell, Crash, China, Splash, Gong, TamTam, NumModels };

    enum Param
    {
        ParModel = 0,   // MODL  which instrument
        ParPing,        // PING  0 = wash only .. 0.5 = both .. 1 = the ping (the partials) only
        ParColour,      // COLR  where the wash sits: dark .. the model's own (0.5) .. bright
        ParNoise,       // NOIS  the wash from pure ringing partials (0) to hiss (1)
        ParDamp,        // DAMP  how much sooner the highs die than the lows (0 = together)
        ParBloom,       // BLOM  the highs arrive late and louder (and a gong's pitch glides)
        ParVelSens,     // VSNS  how much a harder hit is brighter and longer (0 = only louder)
        ParLofi,        // LOFI  the grain of an early sampled cymbal (0 = off)
        NumParams
    };

    static constexpr int NUM_PING = 12;
    static constexpr int NUM_WASH = 36;
    static constexpr int NUM_MODES = NUM_PING + NUM_WASH;
    static constexpr float NATURAL_TUNE = 8000.0f;      // TUNE at which a model has its own pitch

    static float paramDefault(int index)
    {
        static constexpr float defaults[NumParams] = { (float) Ride, 0.5f, 0.5f, 0.5f, 0.4f, 0.3f, 0.5f, 0.0f };
        return (index >= 0 && index < NumParams) ? defaults[index] : 0.0f;
    }

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
        followerDecay = std::exp(-1.0f / (0.03f * (float) sampleRate));
        reset();
    }

    void setParam(int index, float value)
    {
        if (index >= 0 && index < NumParams)
            params[(size_t) index] = value;
    }
    float getParam(int index) const { return (index >= 0 && index < NumParams) ? params[(size_t) index] : 0.0f; }

    bool isActive() const { return active || fade != 0.0f; }

    // Pad pressure on DEC: the sound that is ringing rings `stretch` times as long while it lasts.
    void setDecayStretch(float stretch)
    {
        stretch = std::clamp(stretch, 0.25f, 8.0f);
        if (stretch != decayStretch)
        {
            decayStretch = stretch;
            coefsStale = true;
        }
    }

    void reset()
    {
        x.fill(0.0f); y.fill(0.0f); u.fill(0.0f); n1.fill(0.0f); n2.fill(0.0f);
        direct.fill(1.0f); later.fill(0.0f);
        numActive = 0;
        envLow = envHighShort = envHighLong = 0.0f;
        targetLow = targetHighShort = targetHighLong = 0.0f;
        clickEnv = 0.0f;
        hp.reset(); clickBand.reset(); bandLow.reset(); bandHigh.reset();
        choking = false;
        held = 0.0f; holdPhase = 1.0; follower = 0.0f; peak = 0.0f;
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
        const int model = std::clamp((int) std::lround(params[ParModel]), 0, (int) NumModels - 1);
        if (active && model != struckModel)
        {
            const float carry = lastOut;
            reset();
            fade = carry;           // the old sound's last value glides to nothing
        }
        if (! active)
        {
            const float carry = fade;
            reset();
            fade = carry;
        }
        struckModel = model;
        active = true;
        choking = false;        // (the coefficients are worked out again below)
        const ModelDef& m = def(model);

        const float vel = std::clamp(hit.velocity, 0.0f, 1.0f);
        const float ping = clamp01(params[ParPing]), colour = clamp01(params[ParColour]);
        const float noise = clamp01(params[ParNoise]), damp = clamp01(params[ParDamp]);
        const float bloom = clamp01(params[ParBloom]), sens = clamp01(params[ParVelSens]);
        const float tone = clamp01(hit.tone), snap = clamp01(hit.snap);
        const float dec = std::clamp(hit.decay, 0.001f, 16.0f);
        const float fs = (float) sampleRate;

        tuneRatio = ratioFor(hit.tune);
        const float pingGain = ping <= 0.5f ? ping * 2.0f : 1.0f;
        const float washGain = ping >= 0.5f ? (1.0f - ping) * 2.0f : 1.0f;
        // A harder hit rings longer and (below) is brighter; a full hit is the sound as set.
        const float velLength = std::exp2(sens * (vel - 1.0f) * 1.2f);
        const float velTilt = sens * (vel - 1.0f) * 1.4f;                   // octaves of gain per octave
        // A soft mallet (SNAP below the middle) excites less of the top and takes a few ms to land.
        const float soft = snap < 0.5f ? (0.5f - snap) * 2.0f : 0.0f;
        const float softAttack = 0.012f * soft * soft;
        const float toneTilt = (tone - 0.5f) * 0.8f - soft * 0.6f;
        const float dampExp = damp * 1.6f;
        const float ring = dec * m.washT * velLength;       // seconds to -60 dB where the wash is strongest

        // The stick itself: a burst of noise, shorter and brighter the harder the tip.
        {
            const float hard = std::max(0.0f, (snap - 0.2f) / 0.8f);
            const float seconds = 0.0030f - 0.0022f * hard;
            clickDecay = std::exp(-1.0f / (seconds * fs));
            clickBand.setCutoff(std::min(0.45f * fs, (3000.0f + 5000.0f * hard) * std::sqrt(tuneRatio)));
            clickEnv = std::max(clickEnv, m.click * hard * hard * vel * (0.4f + 0.6f * vel) * 2.2f);
        }
        hp.setCutoff(std::clamp(m.hpHz * tuneRatio, 20.0f, 0.3f * fs));

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
            bankLowGain = 0.6f * pingGain * m.pingLevel;
            bankHighGain = 3.0f * washGain * m.washLevel * bright;
            jitter = noise * noise * 0.04f;
            for (int i = 0; i < 6; ++i)
            {
                cn[(size_t) i] = onePole(30.0f / 0.644f);
                gn[(size_t) i] = noiseGain(cn[(size_t) i]);
            }
            targetLow = std::max(targetLow, vel);
            targetHighShort = std::max(targetHighShort, vel);
            targetHighLong = std::max(targetHighLong, vel);
            outGain = m.gain;
            coefsStale = true;
            control = 0;
            return;
        }

        // ---- the modes ---------------------------------------------------------------------------
        // Their pitches depend on the model and TUNE only, so a mode keeps its place from stroke
        // to stroke and what it still holds can be added to.
        float amp[NUM_MODES], attack[NUM_MODES], share[NUM_MODES];     // share: the part of a mode that is there at once
        const float pingLow = m.pingHz[0];
        float sumSq = 0.0f;
        for (int k = 0; k < NUM_PING; ++k)
        {
            const float f = m.pingHz[k];
            hz[(size_t) k] = f;
            amp[k] = dsp::dbToLinear(m.pingDb[k]) * std::exp2(toneTilt * 0.5f * std::log2(f / (pingLow * 3.0f)));
            sumSq += amp[k] * amp[k];
            t60[(size_t) k] = ring * m.pingT * ringFactor(f / m.washPeak, dampExp);
            attack[k] = softAttack;
            share[k] = (1.0f - soft) * (1.0f - soft);
            const float w = std::max(0.0f, noise * 2.0f - 1.0f) * 0.4f;
            setNoise(k, w, f * tuneRatio * 0.006f);
        }
        const float pingNorm = vel * pingGain * m.pingLevel / std::sqrt(std::max(sumSq, 1.0e-12f));
        for (int k = 0; k < NUM_PING; ++k)
            amp[k] *= pingNorm * std::exp2(velTilt * std::max(0.0f, std::log2(m.pingHz[k] / pingLow)));

        const float step = std::pow(m.washHi / m.washLo, 1.0f / (float) (NUM_WASH - 1));
        const float peakHz = m.washPeak * std::exp2((colour - 0.5f) * 3.0f);
        sumSq = 0.0f;
        for (int i = 0; i < NUM_WASH; ++i)
        {
            const int k = NUM_PING + i;
            const float pos = (float) i / (float) (NUM_WASH - 1);
            // Irregular spacing and uneven strengths, the same at every stroke: the instrument's own face.
            const float f = m.washLo * std::pow(step, (float) i + 0.7f * (hash01(model, i) - 0.5f));
            hz[(size_t) k] = f;
            const float octaves = std::log2(f / peakHz) / m.washSigma;
            amp[k] = std::exp(-0.5f * octaves * octaves)
                   * dsp::dbToLinear(m.trashDb * (hash01(model, i + 100) * 2.0f - 1.0f))
                   * std::exp2(toneTilt * std::log2(f / peakHz));
            sumSq += amp[k] * amp[k];
            t60[(size_t) k] = ring * ringFactor(f / m.washPeak, dampExp);
            attack[k] = softAttack + m.bloomT * bloom * bloom * (0.15f + 0.85f * pos);
            const float w = std::min(1.0f, noise * m.noise * (0.5f + 0.5f * pos) * 1.6f);
            setNoise(k, w, f * tuneRatio * (step - 1.0f) * 0.5f * m.width * (0.15f + 0.85f * noise));
        }
        const float washNorm = vel * washGain * m.washLevel / std::sqrt(std::max(sumSq, 1.0e-12f));
        for (int i = 0; i < NUM_WASH; ++i)
        {
            const int k = NUM_PING + i;
            const float pos = (float) i / (float) (NUM_WASH - 1);
            // The cascade: the harder the hit, the more of the energy ends up in the high modes.
            // The stroke itself puts in what there would be without it, less at the top (HIDE);
            // the rest arrives over the attack time.
            const float cascade = 1.0f + m.cascade * bloom * pos * vel * vel;
            share[k] = (1.0f - soft) * (1.0f - soft) * std::max(0.0f, 1.0f - bloom * m.hide * pos) / cascade;
            amp[k] *= washNorm * cascade
                    * std::exp2(velTilt * std::max(0.0f, std::log2(hz[(size_t) k] / m.washLo)));
        }

        const float nyquistGuard = 0.45f * fs;
        for (int k = 0; k < NUM_MODES; ++k)
        {
            const size_t i = (size_t) k;
            logR[i] = -6.9078f / (std::max(0.004f, t60[i]) * fs);
            float a = hz[i] * tuneRatio < nyquistGuard ? amp[k] : 0.0f;
            // What is heard of the mode now: its motion times what has arrived of it.
            const float rise = 1.0f - u[i];
            const float heard = direct[i] + later[i] * rise * rise;
            float sx = x[i] * heard, sy = y[i] * heard;
            const float mag = std::sqrt(sx * sx + sy * sy);
            // What a mode can hold is limited (two and a half full strokes): a roll builds, not without end.
            a = std::min(a, std::max(0.0f, amp[k] * 2.5f / std::max(vel, 0.05f) - mag));
            // The part of the stroke that is there at once is added to the motion (the output,
            // x, does not jump); the rest arrives over the mode's attack time.
            const bool atOnce = attack[k] < 2.0e-4f;
            const float now = atOnce ? 1.0f : std::clamp(share[k], 0.0f, 1.0f);
            sy += a * now;
            const float sounding = std::sqrt(sx * sx + sy * sy);
            const float full = sounding + a * (1.0f - now);
            if (full < 1.0e-9f)
            {
                x[i] = y[i] = 0.0f;
                direct[i] = 1.0f; later[i] = 0.0f; u[i] = 0.0f; ua[i] = 0.0f;
                continue;
            }
            if (sounding < 1.0e-7f * full)
            {
                // From nothing: all of it is still to come.
                x[i] = 0.0f; y[i] = full;
                direct[i] = 0.0f; later[i] = 1.0f;
            }
            else
            {
                x[i] = sx * (full / sounding);
                y[i] = sy * (full / sounding);
                direct[i] = sounding / full;
                later[i] = 1.0f - direct[i];
            }
            u[i] = later[i] > 0.0f ? 1.0f : 0.0f;
            ua[i] = atOnce ? 0.0f : std::exp(-1.0f / (attack[k] * fs));
        }
        numActive = NUM_MODES;

        // A gong's pitch starts off where the stroke bends it and settles.
        glide = m.glide * bloom * vel * vel;
        glideDecay = std::exp(-(float) CONTROL_RATE / (std::max(0.02f, m.glideT) * fs));

        outGain = m.gain;
        coefsStale = true;
        control = 0;
    }

    // One sample. tuneHz: TUNE as it is now (bend, LFO and pressure move a ringing cymbal).
    float process(float tuneHz)
    {
        if (! active)
        {
            if (fade == 0.0f)
                return 0.0f;
            return fadeOut(0.0f);
        }

        if (--control <= 0)
            controlTick(tuneHz);

        const bool bank = def(struckModel).bank;
        float out = bank ? processBank() : processModes();

        if (clickEnv > 1.0e-5f)
        {
            out += clickBand.process(nextNoise() * clickEnv);
            clickEnv *= clickDecay;
        }
        out = hp.process(out) * outGain;

        const float lofi = params[ParLofi];
        if (lofi > 0.001f)
            out = crush(out, std::min(lofi, 1.0f));

        out = fadeOut(out);
        lastOut = out;
        return out;
    }

private:
    // ---- the models --------------------------------------------------------------------------------
    // Pitches are at TUNE 8000. Ping levels are in dB among themselves; pingLevel and washLevel set
    // the two layers against each other (PING at the middle).
    struct ModelDef
    {
        bool bank;                      // the six-oscillator bank instead of the modes
        float pingHz[NUM_PING];
        float pingDb[NUM_PING];
        float pingT;                    // how long a ping partial rings against a wash mode at its pitch
        float pingLevel;
        float washLo, washHi;           // the wash modes lie between these
        float washPeak, washSigma;      // where the wash is strongest, and how wide (octaves)
        float washT;                    // seconds to -60 dB at washPeak, per second of DEC
        float washLevel;
        float noise, width;             // how much of a wash mode is noise, and how wide its band
        float bloomT;                   // seconds the top of the wash takes to arrive with BLOM at the top
        float cascade;                  // how much louder the top gets with BLOM at the top
        float hide;                     // how much of the top the stroke itself leaves out with BLOM at the top
        float glide, glideT;            // semitones the pitch starts off by with BLOM at the top; its time
        float click;                    // the stick
        float hpHz;
        float trashDb;                  // how uneven the wash modes are
        float gain;
    };

    static const ModelDef& def(int model)
    {
        static const ModelDef defs[NumModels] = {
            // METL: six squares, summed (pingHz holds their pitches).
            { true, { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f }, {}, 1.0f, 1.0f,
              0, 0, 0, 0, 1.0f, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0.3f, 2000.0f, 0, 0.85f },
            // RING: the squares multiplied in pairs: many more partials, none of them a square's own.
            { true, { 263.0f, 414.5f, 317.2f, 587.1f, 731.0f, 1147.0f }, {}, 1.0f, 1.0f,
              0, 0, 0, 0, 1.0f, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0.3f, 1600.0f, 0, 0.5f },
            // RIDE: a stick on the bow of a heavy cymbal: a clear ping over a quiet, long wash.
            { false, { 431.0f, 692.0f, 1043.0f, 1489.0f, 2078.0f, 2811.0f, 3594.0f, 4467.0f, 5486.0f, 6619.0f, 7906.0f, 9372.0f },
              { -20.0f, -16.0f, -12.0f, -8.0f, -3.0f, -3.0f, 0.0f, -2.0f, -1.0f, -4.0f, -6.0f, -9.0f }, 0.12f, 1.3f,
              900.0f, 15000.0f, 5200.0f, 1.3f, 3.0f, 0.6f, 0.9f, 1.0f, 0.06f, 1.2f, 0.4f, 0.0f, 0.1f, 0.9f, 250.0f, 3.0f, 0.36f },
            // BELL: the cup of the ride: strong, long partials (two of them pairs that beat), hardly a wash.
            { false, { 622.0f, 931.0f, 938.5f, 1352.0f, 1836.0f, 2444.0f, 2459.0f, 3087.0f, 3861.0f, 4688.0f, 5637.0f, 6719.0f },
              { -4.0f, 0.0f, -6.0f, -2.0f, -1.0f, -3.0f, -9.0f, -4.0f, -5.0f, -7.0f, -9.0f, -11.0f }, 1.0f, 1.0f,
              1500.0f, 14000.0f, 6000.0f, 1.2f, 1.5f, 0.25f, 0.6f, 0.8f, 0.05f, 1.5f, 0.2f, 0.0f, 0.1f, 0.8f, 300.0f, 3.0f, 0.42f },
            // CRSH: nearly all wash, wide and long; a few low partials are left humming at the end.
            { false, { 287.0f, 398.0f, 523.0f, 741.0f, 1012.0f, 1367.0f, 1790.0f, 2333.0f, 2954.0f, 3731.0f, 4610.0f, 5720.0f },
              { -3.0f, -2.0f, 0.0f, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -6.0f, -7.0f, -8.0f, -9.0f }, 1.0f, 0.15f,
              600.0f, 16000.0f, 4800.0f, 1.7f, 2.0f, 1.0f, 1.0f, 1.2f, 0.06f, 1.2f, 0.5f, 0.0f, 0.1f, 0.8f, 180.0f, 3.0f, 0.36f },
            // CHNA: short, uneven and harsh in the middle.
            { false, { 498.0f, 713.0f, 1021.0f, 1260.0f, 1594.0f, 2011.0f, 2530.0f, 2988.0f, 3620.0f, 4387.0f, 5190.0f, 6240.0f },
              { -5.0f, -3.0f, 0.0f, -2.0f, 0.0f, -1.0f, -2.0f, -1.0f, -4.0f, -5.0f, -7.0f, -9.0f }, 0.6f, 0.6f,
              700.0f, 13000.0f, 3000.0f, 1.3f, 1.0f, 1.0f, 1.2f, 1.5f, 0.03f, 1.5f, 0.4f, 0.0f, 0.1f, 0.8f, 250.0f, 7.0f, 0.36f },
            // SPLS: small, high and quickly gone.
            { false, { 812.0f, 1190.0f, 1675.0f, 2260.0f, 2985.0f, 3790.0f, 4760.0f, 5810.0f, 7020.0f, 8390.0f, 9900.0f, 11600.0f },
              { -3.0f, -1.0f, 0.0f, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -6.0f, -7.0f, -8.0f, -9.0f }, 0.8f, 0.4f,
              1800.0f, 17000.0f, 7500.0f, 1.3f, 0.9f, 1.0f, 1.0f, 1.0f, 0.015f, 1.0f, 0.3f, 0.0f, 0.1f, 0.7f, 500.0f, 3.0f, 0.36f },
            // GONG: a gong with a note (the lowest partial is a pair that beats); its pitch falls as it settles.
            { false, { 196.0f, 197.4f, 393.0f, 471.0f, 588.0f, 762.0f, 969.0f, 1193.0f, 1466.0f, 1795.0f, 2190.0f, 2660.0f },
              { 0.0f, -5.0f, -4.0f, -8.0f, -9.0f, -11.0f, -13.0f, -15.0f, -17.0f, -19.0f, -21.0f, -23.0f }, 1.5f, 1.0f,
              350.0f, 7000.0f, 1500.0f, 1.5f, 3.0f, 0.35f, 0.5f, 0.6f, 0.5f, 2.0f, 1.0f, 0.8f, 0.45f, 0.15f, 60.0f, 3.0f, 0.5f },
            // TAM: a tam-tam: a dense low cluster at once, the shimmer blooming above it afterwards.
            { false, { 71.0f, 92.5f, 118.0f, 141.0f, 167.0f, 203.0f, 248.0f, 289.0f, 341.0f, 397.0f, 466.0f, 541.0f },
              { -2.0f, 0.0f, -1.0f, -3.0f, -2.0f, -4.0f, -4.0f, -6.0f, -6.0f, -8.0f, -9.0f, -10.0f }, 2.0f, 0.6f,
              300.0f, 10000.0f, 2500.0f, 1.8f, 3.0f, 1.2f, 0.9f, 1.0f, 1.6f, 4.0f, 1.0f, 0.3f, 1.0f, 0.1f, 35.0f, 3.0f, 0.45f } };
        return defs[std::clamp(model, 0, (int) NumModels - 1)];
    }

    static constexpr int CONTROL_RATE = 32;         // samples between retunes, trims and glide steps
    static constexpr float BAND_Q = 2.0f;
    static constexpr float CHOKE_SECONDS = 0.06f;   // to -60 dB under the hand

    // How long a mode rings against one at the middle of the wash: the higher, the shorter; the
    // lowest modes are all much alike.
    static float ringFactor(float ratio, float exponent) { return std::min(2.5f, std::pow(ratio, -exponent)); }

    static float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
    static float ratioFor(float tuneHz) { return std::clamp(tuneHz / NATURAL_TUNE, 0.125f, 2.0f); }

    // A number in 0..1 that is always the same for the same model and place.
    static float hash01(int model, int i)
    {
        uint32_t h = (uint32_t) (model * 7919 + i * 104729 + 12345);
        h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
        return (float) (h >> 8) * (1.0f / 16777216.0f);
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

    // Mode k: `amount` of it is a band of noise half-width `hertz` wide, the rest the pure partial.
    void setNoise(int k, float amount, float hertz)
    {
        const size_t i = (size_t) k;
        cn[i] = onePole(hertz / 0.644f);
        carrier[i] = 1.0f - amount;
        gn[i] = amount * noiseGain(cn[i]);
    }

    float nextNoise()
    {
        noiseSeed = noiseSeed * 1664525u + 1013904223u;
        return (float) (int32_t) noiseSeed * (1.0f / 2147483648.0f);
    }

    void controlTick(float tuneHz)
    {
        control = CONTROL_RATE;
        float ratio = ratioFor(tuneHz);
        if (glide != 0.0f)
        {
            ratio *= std::exp2(glide / 12.0f);
            glide *= glideDecay;
            if (std::abs(glide) < 1.0e-3f)
                glide = 0.0f;
        }
        const bool bank = def(struckModel).bank;
        if (ratio != tuneRatio || coefsStale)
        {
            tuneRatio = ratio;
            coefsStale = false;
            const float fs = (float) sampleRate;
            const float hand = choking ? -6.9078f / (CHOKE_SECONDS * fs) : 0.0f;
            if (bank)
            {
                const ModelDef& m = def(struckModel);
                for (int i = 0; i < 6; ++i)
                    inc[(size_t) i] = std::min(0.45, (double) (m.pingHz[i] * ratio) / sampleRate);
                decayLow = std::exp(logLow / decayStretch + hand);
                decayHighLong = std::exp(logHighLong / decayStretch + hand);
                decayHighShort = std::exp(logHighShort + hand);
            }
            else
            {
                for (int k = 0; k < numActive; ++k)
                {
                    const size_t i = (size_t) k;
                    const float w = std::min(dsp::TWO_PI_F * hz[i] * ratio / fs, 2.9f);
                    const float r = std::exp(logR[i] / decayStretch + hand);
                    c[i] = r * std::cos(w);
                    s[i] = r * std::sin(w);
                }
            }
        }

        if (bank)
        {
            if (targetLow < 1.0e-4f && targetHighLong < 1.0e-4f && targetHighShort < 1.0e-4f
                && envLow < 1.0e-4f && envHighLong < 1.0e-4f && clickEnv < 1.0e-4f)
                finish();
            return;
        }

        // The modes that have died away are dropped from the top (the highest die first).
        while (numActive > 0)
        {
            const size_t i = (size_t) (numActive - 1);
            if (x[i] * x[i] + y[i] * y[i] > 1.0e-9f)
                break;
            x[i] = y[i] = 0.0f;
            --numActive;
        }
        for (int k = 0; k < numActive; ++k)
            if (u[(size_t) k] < 1.0e-9f)
                u[(size_t) k] = 0.0f;
        if (numActive == 0 && clickEnv < 1.0e-4f)
            finish();
    }

    void finish()
    {
        const float carry = fade;
        reset();
        fade = carry;
    }

    float processModes()
    {
        float sum = 0.0f;
        const int n = numActive;
        float* const px = x.data(); float* const py = y.data(); float* const pu = u.data();
        float* const p1 = n1.data(); float* const p2 = n2.data(); uint32_t* const ps = seeds.data();
        const float* const pc = c.data(); const float* const psn = s.data(); const float* const pua = ua.data();
        const float* const pcn = cn.data(); const float* const pgn = gn.data(); const float* const pcar = carrier.data();
        const float* const pd = direct.data(); const float* const pl = later.data();
        for (int k = 0; k < n; ++k)
        {
            const float xn = px[k] * pc[k] - py[k] * psn[k];    // (auto-vectorised: keep it free of branches)
            const float yn = px[k] * psn[k] + py[k] * pc[k];
            px[k] = xn;
            py[k] = yn;
            pu[k] *= pua[k];
            ps[k] = ps[k] * 1664525u + 1013904223u;
            const float white = (float) (int32_t) ps[k] * (1.0f / 2147483648.0f);
            p1[k] += pcn[k] * (white - p1[k]);
            p2[k] += pcn[k] * (p1[k] - p2[k]);
            const float rise = 1.0f - pu[k];
            sum += xn * (pd[k] + pl[k] * rise * rise) * (pcar[k] + pgn[k] * p2[k]);
        }
        return sum;
    }

    float processBank()
    {
        const bool ring = struckModel == Ring;
        float sq[6];
        for (int i = 0; i < 6; ++i)
        {
            const size_t k = (size_t) i;
            double step = inc[k];
            if (jitter > 0.0f)
            {
                seeds[k] = seeds[k] * 1664525u + 1013904223u;
                const float white = (float) (int32_t) seeds[k] * (1.0f / 2147483648.0f);
                n1[k] += cn[k] * (white - n1[k]);
                n2[k] += cn[k] * (n1[k] - n2[k]);
                step *= 1.0 + (double) (jitter * gn[k] * n2[k]);
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
        const float sum = ring ? (sq[0] * sq[1] + sq[2] * sq[3] + sq[4] * sq[5]) * (1.0f / 3.0f)
                               : (sq[0] + sq[1] + sq[2] + sq[3] + sq[4] + sq[5]) * (1.0f / 6.0f);

        targetLow *= decayLow;
        targetHighShort *= decayHighShort;
        targetHighLong *= decayHighLong;
        envLow += attackLow * (targetLow - envLow);
        envHighShort += attackHighShort * (targetHighShort - envHighShort);
        envHighLong += attackHighLong * (targetHighLong - envHighLong);

        return bankLowGain * bandLow.process(sum) * envLow
             + bankHighGain * bandHigh.process(sum) * (0.45f * envHighShort + 0.55f * envHighLong);
    }

    // LOFI: the sound is read out at a low rate (held between readings) by a converter with few
    // steps. The steps follow the level of the sound, as the level control after an early
    // sampler's converter did, down to a floor: the tail turns to grit and then stops.
    float crush(float in, float amount)
    {
        const float mag = std::abs(in);
        follower = mag > follower ? mag : follower * followerDecay;
        peak = std::max(peak, mag);
        holdPhase += (double) (32000.0f * std::exp2(-2.6f * amount)) / sampleRate;
        if (holdPhase >= 1.0)
        {
            holdPhase -= std::floor(holdPhase);
            const float levels = std::exp2(8.0f - 4.0f * amount);
            const float stepSize = std::max(std::max(follower, 0.02f * peak), 1.0e-6f) * 2.0f / levels;
            held = std::round(in / stepSize) * stepSize;
        }
        return held;
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
    std::array<float, NumParams> params { { (float) Ride, 0.5f, 0.5f, 0.5f, 0.4f, 0.3f, 0.5f, 0.0f } };
    bool active = false;
    int struckModel = Ride;
    int control = 0;
    float tuneRatio = 1.0f;
    float decayStretch = 1.0f;
    bool coefsStale = true;
    float outGain = 1.0f;

    // The modes (structure of arrays: the loop over them is the cost of the voice).
    std::array<float, NUM_MODES> x {}, y {};            // the mode's motion; x is what is heard
    std::array<float, NUM_MODES> c {}, s {};            // its rotation per sample, decay included
    std::array<float, NUM_MODES> u {}, ua {};           // what is still to arrive of it (1 -> 0)
    std::array<float, NUM_MODES> direct {}, later {};   // the part that is there at once, and the part that arrives
    std::array<float, NUM_MODES> n1 {}, n2 {}, cn {}, gn {}, carrier {};    // its slow noise
    std::array<float, NUM_MODES> hz {}, t60 {}, logR {};
    std::array<uint32_t, NUM_MODES> seeds = [] {
        std::array<uint32_t, NUM_MODES> a {};
        for (size_t i = 0; i < a.size(); ++i) a[i] = 0x9e3779b9u * (uint32_t) (i + 1);
        return a;
    }();
    int numActive = 0;
    float glide = 0.0f, glideDecay = 0.0f;

    // The oscillator bank.
    std::array<double, 6> phase { { 0.0, 0.31, 0.62, 0.17, 0.83, 0.45 } };
    std::array<double, 6> inc {};
    SVFilter bandLow, bandHigh;
    float jitter = 0.0f;
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
    float followerDecay = 0.0f;
    float held = 0.0f, follower = 0.0f, peak = 0.0f;
    double holdPhase = 1.0;
    float fade = 0.0f, lastOut = 0.0f;
};
