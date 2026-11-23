#pragma once

#include "../dsp/LevelCrusher.h"
#include "../dsp/DSPUtils.h"
#include "../dsp/Envelope.h"
#include "../dsp/Resonator.h"
#include "../dsp/filters/SVFilter.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// The oscillator bank (drum type BANK): the metal of the analog drum machines.
//
// Those machines made every metal sound from six square-wave oscillators at unrelated pitches,
// summed, split by two band-passes (about 3.4 and 7.1 kHz) into bands with a decay each
// (K. Werner, J. Abel, J. Smith, their circuit analysis of that cymbal voice in the proceedings
// of ICMC / SMC 2014, pp. 1453-1460; G. Reid, "Practical cymbal synthesis", Sound On Sound,
// 2002). Later machines multiplied the squares in pairs. Written from those principles; no
// samples, no code from elsewhere.
//
// It is ONE bank - six band-limited squares, their sum or their products in pairs (COUP), the two
// band-passes - with four sounds of it (MODL), which differ in the squares' pitches and in what
// follows the bands:
//   HAT   the classic closed hat: both bands under one short envelope, then TONE's high-pass.
//         The same hit every time (the squares start from the same place).
//   OHAT  the same sound long, and open only for as long as the pad, key or step lasts.
//   METL  the cymbal: the low band and the high band (a short and a long part) each with its own
//         decay, a stick's click, a high-pass.
//   RING  other pitches, multiplied in pairs: denser, none of the partials a square's own.
// (Before: the hats were two drum types of their own with their own six squares, and METL and
// RING were two models of the cymbal machine, whose bodies are now sounds of the resonator drum.)
//
// The cells (the pad's BANK page):
//   PING  the low band against the high band (the middle = the sound as it is)
//   COLR  the bands lower or higher
//   EXC   how much the pad's pressure feeds it: the bands are held up for as long as it is
//         pressed (above 1 the squares get the unrest of the resonator's breath and scrape)
//   DAMP  how much sooner the top dies (the middle of the hats' range = as they are)
//   BLOM  the bands arrive late
//   COUP  from the squares' sum to their products in pairs (RING: the other way)
//   LOFI  the grain of an early sampled cymbal: a sample-and-hold and a coarse converter that
//         follows the level
//
// Nothing here allocates.
class OscBank
{
public:
    enum Model { Hat = 0, OpenHat, Metal, Ring, NumModels };

    enum Param { ParModel = 0, ParPing, ParColour, ParExcite, ParDamp, ParBloom, ParCouple, ParLofi, NumParams };

    static constexpr float NATURAL_TUNE = 8000.0f;      // TUNE at which the squares have their own pitches
    static constexpr float MAX_EXCITE = 3.0f;
    static constexpr float HAT_CLOSE_SECONDS = 0.045f;  // an open hat closing: the closed hat's tail

    static float paramDefault(int index)
    {
        static constexpr float defaults[NumParams] = { (float) Hat, 0.5f, 0.5f, 0.0f, 0.4f, 0.0f, 0.0f, 0.0f };
        return (index >= 0 && index < NumParams) ? defaults[index] : 0.0f;
    }
    static float paramMax(int index)
    {
        return index == ParModel ? (float) (NumModels - 1) : (index == ParExcite ? MAX_EXCITE : 1.0f);
    }
    static const char* const* modelNames()
    {
        static const char* const names[NumModels] = { "HAT", "OHAT", "METL", "RING" };
        return names;
    }
    static bool isHat(int model) { return model == Hat || model == OpenHat; }

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
        hatEnv.setSampleRate(sampleRate);
        crusher.prepare(sampleRate);
        // The pitch and the pad's pressure are looked at about 750 times a second.
        controlSamples = std::clamp((int) std::lround(sampleRate / 750.0), 8, 512);
        pressK = 1.0f - std::exp(-(float) controlSamples / (0.008f * (float) sampleRate));
        jitterK = onePole(30.0f / 0.644f);
        jitterGain = noiseGain(jitterK);
        pressTarget = 0.0f;
        reset();
    }

    void setParam(int index, float value)
    {
        if (index >= 0 && index < NumParams)
            params[(size_t) index] = value;
    }
    float getParam(int index) const { return (index >= 0 && index < NumParams) ? params[(size_t) index] : 0.0f; }
    int model() const
    {
        return params[ParModel] >= 0.0f ? (int) std::lround(std::min(params[ParModel], (float) (NumModels - 1))) : 0;
    }
    int struck() const { return struckModel; }

    bool isActive() const { return active || fade != 0.0f; }

    // Pad pressure on DEC: the sound that is ringing rings `stretch` times as long while it lasts.
    void setDecayStretch(float stretch)
    {
        stretch = stretch >= 0.25f ? std::min(stretch, 8.0f) : 0.25f;       // (what is no number is the least)
        if (stretch != decayStretch)
        {
            decayStretch = stretch;
            coefsStale = true;
            if (isHat(struckModel))
                hatEnv.setDecay(hatDecay * decayStretch);
        }
    }

    // The pad's own pressure, 0..1: with EXC up it holds the bands up. A pad pressed again on a
    // sound that has rung out starts it again as it was last played.
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

    // TONE of a hat while it sounds: its high-pass.
    void setHatTone(float tone)
    {
        if (isHat(struckModel))
            hp.setCutoff(std::clamp(tone, 0.0f, 1.0f) * 10000.0f + 500.0f);
    }

    void reset()
    {
        feed = feedTarget = 0.0f;
        energyNow = energyStep = 0.0f;
        pressNow = 0.0f;
        envLow = envHighShort = envHighLong = 0.0f;
        targetLow = targetHighShort = targetHighLong = 0.0f;
        n1.fill(0.0f); n2.fill(0.0f);
        clickEnv = 0.0f;
        hp.reset(); clickBand.reset(); bandLow.reset(); bandHigh.reset();
        hatEnv.reset();
        choking = false;
        crusher.reset();
        fade = 0.0f; lastOut = 0.0f;
        active = false;
        control = 0;
    }

    // Cut short: a hat closes over its release, a cymbal dies under the hand in about a twentieth
    // of a second. The next stroke lifts the hand.
    void choke()
    {
        if (! active)
            return;
        if (isHat(struckModel))
        {
            hatEnv.noteOff();
            return;
        }
        if (! choking)
        {
            choking = true;
            coefsStale = true;
        }
    }

    // The pad, the key or the step has ended: the open hat closes.
    void release()
    {
        if (active && struckModel == OpenHat)
        {
            hatEnv.setRelease(HAT_CLOSE_SECONDS);
            hatEnv.noteOff();
        }
    }

    struct Strike
    {
        float velocity = 1.0f;
        float tune = NATURAL_TUNE;      // Hz, 8000 = the squares' own pitches
        float decay = 1.0f;             // the DEC cell, seconds
        float tone = 0.5f;
        float snap = 0.5f;              // METL, RING: 0 = a soft mallet .. 0.5 = a stick .. 1 = a hard tip
    };

    // A stroke. A cymbal that still rings keeps ringing and the stroke adds to it; a hat starts
    // over (the same hit every time); a change of model starts from nothing (the old sound is
    // faded over 2 ms).
    void strike(const Strike& hit)
    {
        const int m = model();
        if (active && (m != struckModel))
        {
            const float carry = lastOut;
            reset();
            fade = carry;
        }
        if (! active)
            finish();
        struckModel = m;
        active = true;
        choking = false;
        const Voicing& v = voicing(m);

        const float vel = clamp01(hit.velocity);
        const float ping = clamp01(params[ParPing]), colour = clamp01(params[ParColour]);
        const float damp = clamp01(params[ParDamp]), bloom = clamp01(params[ParBloom]);
        const float tone = clamp01(hit.tone), snap = clamp01(hit.snap);
        const float dec = std::isfinite(hit.decay) ? std::clamp(hit.decay, 0.001f, 16.0f) : 1.0f;
        const float fs = (float) sampleRate;
        const float excite = std::isfinite(params[ParExcite]) ? std::clamp(params[ParExcite], 0.0f, MAX_EXCITE) : 0.0f;
        const float couple = clamp01(params[ParCouple]);
        fed = std::min(1.0f, excite);
        const float stick = 1.0f - fed;

        tuneRatio = ratioFor(hit.tune, v.hat);
        const float pingGain = ping <= 0.5f ? ping * 2.0f : 1.0f;
        const float washGain = ping >= 0.5f ? (1.0f - ping) * 2.0f : 1.0f;
        const float shift = std::exp2((colour - 0.5f) * 2.0f);
        // The exciter's grain is the oscillators' unrest; its grip couples them in pairs (towards
        // RING's products; RING towards the sum).
        const float grain = grainAt(v.grain, excite);
        jitter = grain * grain * 0.04f;
        pairs = m == Ring ? 1.0f - couple : couple;
        bandLow.setResonance(v.q);
        bandHigh.setResonance(v.q);

        if (v.hat)
        {
            // Both bands (they stay where they are when TUNE moves the squares), one envelope,
            // TONE's high-pass. Every hit starts the squares and the filters from the same place.
            static constexpr double start[6] = { 0.0, 0.31, 0.62, 0.17, 0.83, 0.45 };
            for (int i = 0; i < 6; ++i) phase[(size_t) i] = start[i];
            bandLow.reset();
            bandHigh.reset();
            hp.reset();
            bandLow.setCutoff(std::min(0.45f * fs, 3440.0f * shift));
            bandHigh.setCutoff(std::min(0.45f * fs, 7100.0f * shift));
            hp.setCutoff(tone * 10000.0f + 500.0f);
            Envelope::Params p;
            p.attack = bloom * bloom * 0.04f;
            p.decay = hatDecay = (m == OpenHat ? 0.1f + dec * 0.9f : 0.03f + dec * 0.25f) * std::exp2((0.4f - damp) * 2.5f);
            p.sustain = 0.0f;
            p.release = m == OpenHat ? 0.02f : 0.005f;
            p.decayCurve = -0.3f;
            hatEnv.setParams(p);
            if (decayStretch != 1.0f)
                hatEnv.setDecay(hatDecay * decayStretch);
            hatEnv.noteOn();
            bankLowGain = 0.35f * pingGain;
            bankHighGain = washGain;
            outGain = v.gain;
            coefsStale = true;
            control = 0;
            return;
        }

        // A harder hit rings longer and is brighter; a full hit is the sound as set.
        const float velLength = std::exp2(VEL_SENS * (vel - 1.0f) * 1.2f);
        const float velTilt = VEL_SENS * (vel - 1.0f) * 1.4f;
        // A soft mallet (SNAP below the middle) excites less of the top and takes a few ms to land.
        const float soft = snap < 0.5f ? (0.5f - snap) * 2.0f : 0.0f;
        const float hard = std::max(0.0f, (snap - 0.2f) / 0.8f);
        const float softAttack = 0.012f * soft * soft;

        // With EXC up the stroke also feeds the bands for half its length (strokes add as powers do).
        const bool ringing = feedTarget > 0.0f;
        feedTarget = std::min(MAX_FEED, std::sqrt(feedTarget * feedTarget + vel * vel));
        feedAttack = 1.0f - std::exp(-(float) controlSamples / ((5.0e-4f + softAttack + 0.1f * bloom * bloom) * fs));
        ringSeconds = ringing ? std::max(ringSeconds, dec * velLength) : dec * velLength;

        // The stick itself: a burst of noise, shorter and brighter the harder the tip.
        {
            const float seconds = 0.0030f - 0.0022f * hard;
            clickDecay = std::exp(-1.0f / (seconds * fs));
            clickBand.setCutoff(std::min(0.45f * fs, (3000.0f + 5000.0f * hard) * std::sqrt(tuneRatio)));
            clickEnv = std::max(clickEnv, v.click * hard * hard * vel * (0.4f + 0.6f * vel) * 2.2f * stick);
        }
        // (The thin voicing is the stick's: what is fed by the pad is heard on the whole of it.)
        const float thin = std::exp2((colour - 0.5f) * COLOUR_THIN + std::max(0.0f, tone - 0.5f) * 2.0f)
                           * (fed < 1.0f ? v.hpHz * std::pow(FED_HP / v.hpHz, fed) : FED_HP);
        hp.setCutoff(std::clamp(thin * tuneRatio, 20.0f, 0.3f * fs));

        // The oscillator bank: three bands, three decays.
        bandLow.setCutoff(std::min(0.45f * fs, 3440.0f * shift * tuneRatio));
        bandHigh.setCutoff(std::min(0.45f * fs, 7100.0f * shift * tuneRatio));
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
        targetLow = std::max(targetLow, vel * stick);
        targetHighShort = std::max(targetHighShort, vel * stick);
        targetHighLong = std::max(targetHighLong, vel * stick);
        outGain = v.gain;
        coefsStale = true;
        control = 0;
    }

    // One sample. tuneHz: TUNE as it is now (bend, LFO and pressure move a ringing sound).
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
        const bool hat = isHat(struckModel);
        const float sum = squares();
        float out;
        if (hat)
        {
            // (What is fed - the pad's pressure with EXC up - holds the hat open.)
            const float env = std::max(hatEnv.process(), energyNow);
            const float metal = bankHighGain * bandHigh.process(sum) + bankLowGain * bandLow.process(sum);
            out = hp.process(metal) * env * outGain;
            // (An envelope stays in its sustain until it is let go: at the end of the decay it is.)
            if (hatEnv.getStage() == Envelope::Stage::Sustain && hatEnv.getCurrentLevel() < 0.001f)
                hatEnv.noteOff();
            if (! hatEnv.isActive() && energyNow < 1.0e-5f && pressTarget * fed <= 0.0f)
                finish();
        }
        else
        {
            targetLow *= decayLow;
            targetHighShort *= decayHighShort;
            targetHighLong *= decayHighLong;
            envLow += attackLow * (std::max(targetLow, energyNow) - envLow);
            envHighShort += attackHighShort * (std::max(targetHighShort, energyNow) - envHighShort);
            envHighLong += attackHighLong * (std::max(targetHighLong, energyNow) - envHighLong);
            out = bankLowGain * bandLow.process(sum) * envLow
                + bankHighGain * bandHigh.process(sum) * (0.45f * envHighShort + 0.55f * envHighLong);
            if (clickEnv > 1.0e-5f)
            {
                out += clickBand.process(nextNoise() * clickEnv);
                clickEnv *= clickDecay;
            }
            out = hp.process(out + 1.0e-18f) * outGain;
        }

        const float lofi = params[ParLofi];
        if (lofi > 0.001f)
            out = crusher.process(out, std::min(lofi, 1.0f));     // LOFI (dsp/LevelCrusher.h)
        if (! hat || fade != 0.0f)
            out = res::outLimit(fadeOut(out), OUT_LIMIT);       // (a hat that fits is untouched)
        lastOut = out;
        return out;
    }

private:
    // What a sound of the bank is made of. Pitches are at TUNE 8000.
    struct Voicing
    {
        bool hat;               // one envelope and TONE's high-pass, instead of three decays and a click
        float squares[6];
        float q;                // of the two band-passes
        float grain;            // the oscillators' own unrest
        float click;            // the stick
        float hpHz;
        float gain;
    };

    static const Voicing& voicing(int model)
    {
        static const Voicing v[NumModels] = {
            { true,  { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f },  1.6f, 0.0f,  0.0f, 0.0f,    3.0f },   // HAT
            { true,  { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f },  1.6f, 0.0f,  0.0f, 0.0f,    3.0f },   // OHAT
            { false, { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f },  2.0f, 0.0f,  0.3f, 2000.0f, 0.85f },  // METL
            { false, { 263.0f, 414.5f, 317.2f, 587.1f, 731.0f, 1147.0f }, 2.0f, 0.25f, 0.3f, 1600.0f, 0.5f } }; // RING
        return v[std::clamp(model, 0, (int) NumModels - 1)];
    }

    static constexpr float CHOKE_SECONDS = 0.06f;   // to -60 dB under the hand
    static constexpr float VEL_SENS = 0.55f;        // how much a harder hit is brighter and longer
    static constexpr float FED_HP = 90.0f;          // the high-pass of what the pad feeds (the stick's is the sound's own)
    static constexpr float OUT_LIMIT = 1.5f;
    static constexpr float COLOUR_THIN = 3.0f;
    static constexpr float MAX_FEED = 1.6f;         // what a roll can build the feed to
    static constexpr float FEED_OVER = 1.0e-4f;     // the feed's end

    static float clamp01(float v) { return v >= 0.0f ? std::min(v, 1.0f) : 0.0f;  }    // (not-a-number is 0)
    // (The hats' squares go half as far up as the cymbals': their range as it always was.)
    static float ratioFor(float tuneHz, bool hat)
    {
        return tuneHz >= 0.0f ? std::clamp(tuneHz / NATURAL_TUNE, 0.125f, hat ? 1.5f : 2.0f) : 1.0f;
    }

    // GRAN of the resonator's exciter at EXC: the sound's own at 0, then the bow's, the breath's,
    // the scrape's, and everything between.
    static float grainAt(float own, float excite)
    {
        const auto& points = res::excitePoints();
        const int a = std::min((int) excite, 2);
        const float t = excite - (float) a;
        const float from = a == 0 ? own : points[(size_t) a][res::Grain];
        return clamp01(from + (points[(size_t) a + 1][res::Grain] - from) * t);
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
        const Voicing& v = voicing(struckModel);
        const float fs = (float) sampleRate;
        const float ratio = ratioFor(tuneHz, v.hat);
        const bool moved = ratio != tuneRatio || coefsStale;
        if (moved)
        {
            tuneRatio = ratio;
            coefsStale = false;
            const float hand = choking ? -6.9078f / (CHOKE_SECONDS * fs) : 0.0f;
            // (The hats' steps are worked out as they always were, to the last bit: the same hit.)
            static constexpr double hatHz[6] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
            const double hatScale = tuneHz >= 0.0f ? std::clamp((double) tuneHz / 8000.0, 0.125, 1.5) : 1.0;
            for (int i = 0; i < 6; ++i)
                inc[(size_t) i] = v.hat ? hatHz[i] * hatScale / sampleRate
                                        : std::min(0.45, (double) (v.squares[i] * ratio) / sampleRate);
            feedDecay = std::exp(-6.9078f * (float) controlSamples / (std::max(0.004f, 0.5f * ringSeconds) * decayStretch * fs));
            decayLow = std::exp(logLow / decayStretch + hand);
            decayHighLong = std::exp(logHighLong / decayStretch + hand);
            decayHighShort = std::exp(logHighShort + hand);
        }

        // The pad's pressure, a straight line from one look to the next. (A hand on it: nothing
        // is fed any more.)
        if (choking)
            feedTarget = feed = pressNow = 0.0f;
        feedTarget *= feedDecay;
        if (feedTarget < FEED_OVER) feedTarget = 0.0f;
        feed += (feedTarget > feed ? feedAttack : std::max(feedAttack, 1.0f - feedDecay)) * (feedTarget - feed);
        if (feedTarget <= 0.0f && feed < FEED_OVER) feed = 0.0f;
        const float pressed = choking ? 0.0f : pressTarget * fed;
        pressNow += pressK * (pressed - pressNow);
        if (pressNow < 1.0e-6f) pressNow = 0.0f;
        const float energy = std::min(2.0f, (v.hat ? 0.0f : fed * feed) + pressNow);
        energyStep = (energy - energyNow) / (float) controlSamples;
        const bool fedOut = energy <= 0.0f && energyNow < 1.0e-5f && pressed <= 0.0f;

        if (! v.hat && fedOut && targetLow < 1.0e-4f && targetHighLong < 1.0e-4f && targetHighShort < 1.0e-4f
            && envLow < 1.0e-4f && envHighLong < 1.0e-4f && clickEnv < 1.0e-4f)
            finish();
    }

    void finish()
    {
        const float carry = fade;
        reset();
        fade = carry;
    }

    // The six squares, summed or multiplied in pairs.
    float squares()
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
            // A band-limited square: the two edges are rounded over one sample each.
            float v = ph < 0.5 ? 1.0f : -1.0f;
            v += dsp::polyBLEP(ph, step);
            double half = ph + 0.5;
            if (half >= 1.0) half -= 1.0;
            v -= dsp::polyBLEP(half, step);
            sq[i] = v;
        }
        if (pairs <= 0.0f)
        {
            float sum = 0.0f;
            for (int i = 0; i < 6; ++i)
                sum += sq[i];
            return sum * (1.0f / 6.0f);
        }
        const float summed = pairs < 1.0f ? (sq[0] + sq[1] + sq[2] + sq[3] + sq[4] + sq[5]) * (1.0f / 6.0f) : 0.0f;
        return summed + pairs * ((sq[0] * sq[1] + sq[2] * sq[3] + sq[4] * sq[5]) * (1.0f / 3.0f) - summed);
    }

    // The 2 ms glide to nothing of a sound that was cut off by a change of model.
    float fadeOut(float out)
    {
        if (fade != 0.0f)
        {
            out += fade;
            fade *= std::exp(-1.0f / (0.0007f * (float) sampleRate));
            if (std::abs(fade) < 1.0e-5f)
                fade = 0.0f;
        }
        return out;
    }

    double sampleRate = 44100.0;
    std::array<float, NumParams> params { { (float) Hat, 0.5f, 0.5f, 0.0f, 0.4f, 0.0f, 0.0f, 0.0f } };
    bool active = false;
    int struckModel = Hat;
    int control = 0, controlSamples = 64;
    float tuneRatio = 1.0f;
    float decayStretch = 1.0f;
    bool coefsStale = true;
    float outGain = 1.0f;
    float fed = 0.0f;
    float feed = 0.0f, feedTarget = 0.0f, feedAttack = 1.0f, feedDecay = 0.0f, ringSeconds = 1.0f;
    float energyNow = 0.0f, energyStep = 0.0f;
    float pressTarget = 0.0f, pressNow = 0.0f, pressK = 0.1f;

    std::array<double, 6> phase { { 0.0, 0.31, 0.62, 0.17, 0.83, 0.45 } };
    std::array<double, 6> inc {};
    std::array<float, 6> n1 {}, n2 {};                      // each oscillator's slow noise
    float jitterK = 0.0f, jitterGain = 1.0f;                // ... its rate and what brings it to a variance of one
    std::array<uint32_t, 6> seeds { { 0x9e3779b9u, 0x3c6ef372u, 0xdaa66d2bu, 0x78dde6e4u, 0x1715609du, 0xb54cda56u } };
    SVFilter bandLow, bandHigh;
    float jitter = 0.0f, pairs = 0.0f;
    float bankLowGain = 1.0f, bankHighGain = 1.0f;
    Envelope hatEnv;
    float hatDecay = 0.04f;
    float envLow = 0.0f, envHighShort = 0.0f, envHighLong = 0.0f;
    float targetLow = 0.0f, targetHighShort = 0.0f, targetHighLong = 0.0f;
    float logLow = 0.0f, logHighLong = 0.0f, logHighShort = 0.0f;
    float decayLow = 0.0f, decayHighLong = 0.0f, decayHighShort = 0.0f;
    float attackLow = 1.0f, attackHighLong = 1.0f, attackHighShort = 1.0f;

    SVFilter hp, clickBand;
    float clickEnv = 0.0f, clickDecay = 0.0f;
    uint32_t noiseSeed = 0x2545f491u;
    bool choking = false;
    dsp::LevelCrusher crusher;      // LOFI
    float fade = 0.0f, lastOut = 0.0f;
};
