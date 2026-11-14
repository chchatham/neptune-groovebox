#pragma once

#include "../dsp/Envelope.h"
#include "../dsp/DSPUtils.h"
#include "../dsp/filters/SVFilter.h"
#include "EngineLfoSection.h"
#include "CymbalMachine.h"
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <array>

enum class DrumSynthType
{
    FMKick, AnalogKick, ModCyclesKick,
    NoiseSnare, FMSnare,
    Clap,
    ClosedHat, OpenHat,
    Cymbal, Tom, Rim, Shaker, Clave,
    SubBass,
    KickSynth,      // the kick machine: its own page of sweep, shape and knock controls (appended: projects store the number)
    CymbalSynth     // the cymbal machine (CymbalMachine.h): its own page, CYM
};

// Per-slot aftertouch destination enum (matches DrumATDest in DrumKit.h)
enum class DrumVoiceATDest
{
    Off = 0,
    Pitch,
    Decay,
    Filter,
    Level,
    Drive,
    Snap,
    Pan
};

// What the cells do, by type (every cell is heard on every type: the "Dead Controls" tests).
//   DEC   the length: the amp decay (hats, cymbal, clave and rim shot scale it to their own
//         range); on SUB, which sounds for as long as it is held, the fade after it is let go.
//   TONE  brightness, in the way of each drum: FMKK the level of the FM click; KICK how far the
//         pitch falls (0 = no fall); MKIK the resonance of its filter; SNAR / FMSN the noise
//         band (and FMSN's FM index); CLAP the band's width; hats and cymbal the high-pass; TOM
//         the stick noise; RIM its noise; SHKR from a broad hiss to a narrow, pitched rattle;
//         CLAV the upper partial; SUB sine -> triangle -> squared-off; KSYN a low-pass; CSYN how
//         much of the top there is (the middle = the cymbal as it is).
//   DRIV  saturation (a soft clip that keeps the peak level) on every type; SNAR / FMSN also
//         shift from the body to the noise with it, CLAP adds claps, SHKR deepens its rattle.
//   SNAP  the front edge, 0.5 = the drum as it is: below, the onset is softened (it fades in
//         over up to about 12 ms); above, the first milliseconds are lifted (to +6 dB) and the
//         drums that start from a sine get a short noise click. TOM and KSYN set their attack
//         time with it instead, CLAP also the length of each clap. CSYN: what strikes the cymbal,
//         from a soft mallet (0) through a stick (0.5) to a hard tip with a loud tick (1).
class DrumVoice
{
public:
    // LFO destinations (EngineLfoSection), in the order of the DEST cell. Every drum voice has its
    // own pair of LFOs, set from the kit's two LFO pages, so KEY / ONE / HALF restart with each
    // hit of that drum and HOLD freezes a new value at each hit.
    //   Pitch, tone, drive, level and pan follow the LFO while the hit sounds.
    //   Decay and snap shape the envelopes, which are set up when the drum is struck, so they
    //   take the LFO's value at that moment (as do the parts of tone and drive that are fixed at
    //   the strike: the clap's burst count and spacing, filters tuned from TUNE).
    // The list goes on with the kick machine's eight cells (DrumEngine::lfoDests; their key is the
    // KickParam index): SWEP and SHAP follow the LFO while the hit sounds, as does the pitch of
    // the knock (KTON); SWPT, SCRV, KNCK, HOLD, ACRV and the knock's length are set at the strike.
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // TUNE, +-12 semitones, depth squared
        LfoDecay,       // DEC x 1/4 .. x 4 (+-2 octaves of time), per hit
        LfoTone,        // +-1 around TONE (0..1)
        LfoDrive,       // +-1 around DRIV (0..1)
        LfoSnap,        // +-1 around SNAP (0..1), per hit
        LfoAmp,         // gain 0..2
        LfoPan,         // +-0.5 around PAN (0..1), i.e. the whole width
        NumLfoDests
    };

    static const char* const* lfoDestLabels()
    {
        static const char* const names[NumLfoDests] = {
            "OFF", "PTCH", "DEC", "TONE", "DRIV", "SNAP", "AMP", "PAN" };
        return names;
    }

    VoiceLfos& getLfos() { return lfos; }
    // -0.5..0.5 from LFOs routed to the pan (as of the last processed sample); the engine adds it.
    float getLfoPan() const { return lfoPan; }

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        ampEnv.setSampleRate(sr);
        pitchEnv.setSampleRate(sr);
        filter.setSampleRate(sr);
        for (auto* band : { &hatBandHigh, &hatBandLow })
        {
            band->setSampleRate(sr);
            band->setMode(SVFilter::Mode::BandPass);
            band->setResonance(1.6f);
        }
        hatBandHigh.setCutoff(7100.0f);
        hatBandLow.setCutoff(3440.0f);
        cym.setSampleRate(sr);
        lfos.setSampleRate(sr);
        strikeDeclick.setSampleRate(sr);
        levelStep = (float) (1.0 / (0.004 * sr));       // a level's whole range in 4 ms
    }

    void setType(DrumSynthType type)
    {
        if (synthType == DrumSynthType::CymbalSynth && type != synthType)
            cym.reset();
        synthType = type;
    }
    DrumSynthType getType() const { return synthType; }

    void setTune(float hz)   { tune = hz; }
    void setDecay(float sec) { decay = sec; }
    void setTone(float val)  { tone = std::clamp(val, 0.0f, 1.0f); }
    void setDrive(float val) { drive = std::clamp(val, 0.0f, 1.0f); }
    void setSnap(float val)  { snap = std::clamp(val, 0.0f, 1.0f); }
    void setLevel(float val) { level = std::clamp(val, 0.0f, 1.0f); }
    void setBendRatio(float ratio) { bendRatio = std::clamp(ratio, 0.25f, 4.0f); }

    // --- The kick machine (type KickSynth) --------------------------------------------------------
    // TUNE is the pitch the drum settles on and DEC how long the body rings; these eight shape
    // the rest. They are read by the KickSynth type only.
    enum KickParam
    {
        KickSweep = 0,   // SWEP  semitones the pitch starts above TUNE (0..72)
        KickSweepTime,   // SWPT  seconds the pitch takes to fall (0.002..1)
        KickSweepCurve,  // SCRV  -1..1: how the fall is spread (-1 = nearly all at the start)
        KickShape,       // SHAP  0..1: sine -> triangle -> squared-off
        KickKnock,       // KNCK  0..1: level of the click at the very start
        KickKnockTone,   // KTON  0..1: dull thud -> bright tick (also how long it lasts)
        KickHold,        // HOLD  seconds at full level before the decay starts (0..0.3)
        KickAmpCurve,    // ACRV  -1..1: shape of the decay
        NumKickParams
    };
    void setKickParam(int index, float value)
    {
        if (index >= 0 && index < NumKickParams)
            kick[(size_t) index] = value;
    }
    float getKickParam(int index) const { return (index >= 0 && index < NumKickParams) ? kick[(size_t) index] : 0.0f; }
    static float kickDefault(int index)
    {
        static constexpr float defaults[NumKickParams] = { 36.0f, 0.06f, -0.6f, 0.0f, 0.3f, 0.5f, 0.0f, -0.4f };
        return (index >= 0 && index < NumKickParams) ? defaults[index] : 0.0f;
    }

    // --- The cymbal machine (type CymbalSynth) -----------------------------------------------------
    // TUNE is its size (8000 = the model's own pitch), DEC how long it rings, TONE its brightness,
    // SNAP what strikes it; the eight cells of CymbalMachine::Param are the rest. Read at the strike
    // (LOFI at once).
    void setCymbalParam(int index, float value) { cym.setParam(index, value); }
    float getCymbalParam(int index) const { return cym.getParam(index); }

    // --- Per-slot expression routing ---
    void setAftertouchDest(DrumVoiceATDest dest) { atDest = dest; }
    DrumVoiceATDest getAftertouchDest() const { return atDest; }

    void setAftertouchDepth(float depth) { atDepth = std::clamp(depth, 0.0f, 1.0f); }
    float getAftertouchDepth() const { return atDepth; }

    // Apply aftertouch pressure. Returns pan modulation (if dest=Pan).
    // Other destinations are applied internally to the voice parameters.
    // Call this from the audio thread; no allocations.
    float applyAftertouch(float pressure)
    {
        float panMod = 0.0f;
        float mod = pressure * atDepth;

        switch (atDest)
        {
            case DrumVoiceATDest::Off:
                break;
            case DrumVoiceATDest::Pitch:
                // Modulate tune up to +1 octave (factor 2x)
                atPitchMod = mod;
                break;
            case DrumVoiceATDest::Decay:
                // Extend decay up to 3x: the hit that is sounding rings on while the pad is squeezed
                atDecayMod = mod;
                applyDecayPressure();
                break;
            case DrumVoiceATDest::Filter:
                // TONE up, by as much as the whole cell at full depth
                atFilterMod = mod;
                break;
            case DrumVoiceATDest::Level:
                // Scale level 0..+1
                atLevelMod = mod;
                break;
            case DrumVoiceATDest::Drive:
                // Add drive
                atDriveMod = mod;
                break;
            case DrumVoiceATDest::Snap:
                // Add snap
                atSnapMod = mod;
                break;
            case DrumVoiceATDest::Pan:
                // Return pan offset for engine to apply
                panMod = (mod * 2.0f - 1.0f) * 0.5f; // bipolar, +/- 0.5
                break;
        }

        return panMod;
    }

    // Clear all aftertouch modulation (call on noteOff or pressure=0)
    void clearAftertouch()
    {
        atPitchMod = 0.0f;
        if (atDecayMod != 0.0f)
        {
            atDecayMod = 0.0f;
            applyDecayPressure();
        }
        atFilterMod = 0.0f;
        atLevelMod = 0.0f;
        atDriveMod = 0.0f;
        atSnapMod = 0.0f;
    }

    // The decay of the hit that is sounding, with the pressure on it (the envelope reads its times
    // as it runs). The kick machine's decay is its envelope's release.
    void applyDecayPressure()
    {
        const float longer = 1.0f + 2.0f * atDecayMod;
        if (synthType == DrumSynthType::CymbalSynth)
        {
            cym.setDecayStretch(longer);        // its modes ring on
            return;
        }
        if (synthType == DrumSynthType::KickSynth || synthType == DrumSynthType::SubBass)
            ampEnv.setRelease(struckRelease * longer);
        else
            ampEnv.setDecay(struckDecay * longer);
    }

    // Get effective tune with aftertouch modulation
    float getEffectiveTune() const
    {
        // Pitch mod: semitones -> ratio. mod=1 -> +12 semitones (octave up)
        float ratio = std::exp2f(atPitchMod);
        return tune * ratio;
    }

    // Get effective level with aftertouch modulation
    float getEffectiveLevel() const
    {
        return std::clamp(level + atLevelMod, 0.0f, 1.0f);
    }

    void noteOn(float vel)
    {
        // Struck again while it rings: its oscillators and filter start over, which would cut the
        // ringing off at whatever value it had (a click). That value is carried down over 2 ms under
        // the new hit, whose own first sample is not touched. (The cymbal machine does not start
        // over: a second stroke adds to what is ringing.)
        if (active && synthType != DrumSynthType::CymbalSynth)
            strikeDeclick.armTail();
        struck = true;
        // The LFOs are read at the strike (decay, snap, ...) and run on from that value.
        lfos.noteOnAndSample();
        lfos.snapSlews();       // a strike starts the drum again
        lfoGain = 1.0f;
        lfoPan = 0.0f;
        if (! lfos.anyOn())
        {
            strike(vel);
            return;
        }
        const SetValues set = applyLfos();
        strike(vel);
        restore(set);
    }

    // Letting the pad (or the step) go. A drum is a one-shot: it rings for its DEC however short
    // the tap was. SUB, the held bass note, sounds for as long as it is held and then fades
    // over its DEC.
    // The open hat is the other exception: it is open for as long as the pad, key or step lasts
    // and closes when that ends, so a tap on it is a closed hat and a held hit an open one.
    void noteOff()
    {
        if (synthType == DrumSynthType::OpenHat)
        {
            ampEnv.setRelease(HAT_CLOSE_SECONDS);
            ampEnv.noteOff();
            return;
        }
        if (synthType != DrumSynthType::SubBass)
            return;
        ampEnv.noteOff();
        pitchEnv.noteOff();
    }

    // Cut short by another drum of its choke group (the closed hat stops the open one).
    void choke()
    {
        if (synthType == DrumSynthType::CymbalSynth)
        {
            cym.choke();                        // a hand on it: gone in about a twentieth of a second
            return;
        }
        if (synthType == DrumSynthType::KickSynth)
        {
            // Its decay is its envelope's release: stop it over a few milliseconds instead.
            ampEnv.setRelease(0.005f);
            kickReleased = true;
        }
        ampEnv.noteOff();
        pitchEnv.noteOff();
    }

    bool isActive() const { return active; }

    float process()
    {
        if (!active)
            return 0.0f;

        if (! lfos.anyOn())
        {
            lfoGain = 1.0f;
            lfoPan = 0.0f;
            lfos.snapSlews();
            return strikeDeclick.process(processCore());
        }

        // The voice's parameters are moved for this sample only (the set values stay).
        lfos.tick();
        const SetValues set = applyLfos();
        if (lfos.routed(LfoTone) && tone != lastLfoTone)
            retuneFilterForTone();
        lfoGain = lfos.gain(LfoAmp);
        lfoPan = std::clamp(lfos.panAmount(LfoPan) * 0.5f, -0.5f, 0.5f);
        const float out = strikeDeclick.process(processCore() * lfoGain);
        restore(set);
        return out;
    }

private:
    // kickKey / kickValue: one per source that can move a kick-machine cell (two LFOs, the strip's routes).
    struct SetValues
    {
        float tune, decay, tone, drive, snap;
        int kickKey[VoiceLfos::NUM_SOURCES];
        float kickValue[VoiceLfos::NUM_SOURCES];
    };

    // Replaces the parameters by their LFO-modulated values; restore() puts the set values back.
    SetValues applyLfos()
    {
        SetValues set { tune, decay, tone, drive, snap, {}, {} };
        for (int& k : set.kickKey)
            k = -1;
        if (lfos.keyed())
            for (int i = 0; i < VoiceLfos::NUM_SOURCES; ++i)
            {
                // A kick-machine cell: this voice's own value, moved in the cell's own scale
                // (once, by every source routed to it).
                const int k = lfos.keyAt(i);
                bool seen = false;
                for (int j = 0; j < i; ++j)
                    seen = seen || set.kickKey[j] == k;
                if (k < 0 || k >= NumKickParams || seen)
                    continue;
                set.kickKey[i] = k;
                set.kickValue[i] = kick[(size_t) k];
                kick[(size_t) k] = lfos.value(k, kick[(size_t) k]);
            }
        const float semis = lfos.pitchSemitones(LfoPitch);
        if (semis != 0.0f)
            tune *= std::exp2(semis / 12.0f);
        const float dec = lfos.amount(LfoDecay);
        if (dec != 0.0f)
            decay = std::max(0.001f, decay * std::exp2(2.0f * dec));
        tone = std::clamp(tone + lfos.amount(LfoTone), 0.0f, 1.0f);
        drive = std::clamp(drive + lfos.amount(LfoDrive), 0.0f, 1.0f);
        snap = std::clamp(snap + lfos.amount(LfoSnap), 0.0f, 1.0f);
        return set;
    }

    void restore(const SetValues& set)
    {
        tune = set.tune;
        decay = set.decay;
        tone = set.tone;
        drive = set.drive;
        snap = set.snap;
        for (int i = 0; i < VoiceLfos::NUM_SOURCES; ++i)
            if (set.kickKey[i] >= 0)
                kick[(size_t) set.kickKey[i]] = set.kickValue[i];
    }

    // The filters whose tuning comes from TONE (see strike()), for a tone that moves during the hit.
    void retuneFilterForTone()
    {
        lastLfoTone = tone;
        switch (synthType)
        {
            case DrumSynthType::Clap:
                filter.setResonance(0.7f + tone * 4.0f);
                break;
            case DrumSynthType::ClosedHat:
            case DrumSynthType::OpenHat:
            case DrumSynthType::Cymbal:
                filter.setCutoff(tone * 10000.0f + 500.0f);
                break;
            case DrumSynthType::NoiseSnare:
            case DrumSynthType::FMSnare:
                filter.setCutoff(tone * 5000.0f + 500.0f);
                break;
            case DrumSynthType::ModCyclesKick:
                filter.setResonance(0.707f + tone * 3.0f);
                break;
            case DrumSynthType::KickSynth:
                filter.setCutoff(200.0f * std::pow(80.0f, tone));
                break;
            case DrumSynthType::Shaker:
                filter.setResonance(shakerQ(tone));
                break;
            case DrumSynthType::CymbalSynth:
                cymTop = cymbalTop(tone);
                break;
            default:
                break;
        }
    }

    // The cymbal machine's TONE while it sounds: the gain of everything above about 2.5 kHz,
    // -18 dB .. 0 (the middle) .. +9 dB.
    static float cymbalTop(float tone) { return std::exp2((tone - 0.5f) * (tone < 0.5f ? 6.0f : 3.0f)); }

    // The shaker's band: Q 0.5 (a broad hiss) .. 2 at the middle .. 8 (a narrow, pitched rattle).
    static float shakerQ(float tone) { return 2.0f * std::pow(4.0f, 2.0f * (tone - 0.5f)); }

    // The types that have no drive of their own in their process function.
    bool usesCommonDrive() const
    {
        switch (synthType)
        {
            case DrumSynthType::ModCyclesKick:      // saturates before its amp envelope
            case DrumSynthType::KickSynth:          // before its low-pass
                return false;
            default:
                return true;
        }
    }

    // The types whose sound starts from a sine at zero: SNAP gives them a click.
    bool hasSnapClick() const
    {
        switch (synthType)
        {
            case DrumSynthType::FMKick:
            case DrumSynthType::AnalogKick:
            case DrumSynthType::ModCyclesKick:
            case DrumSynthType::NoiseSnare:
            case DrumSynthType::FMSnare:
            case DrumSynthType::Rim:
            case DrumSynthType::Clave:
            case DrumSynthType::SubBass:
                return true;
            default:
                return false;
        }
    }

    // SNAP on every type but TOM and KSYN (their attack time): 0.5 leaves the drum as it is.
    bool usesCommonSnap() const
    {
        return synthType != DrumSynthType::Tom && synthType != DrumSynthType::KickSynth
            && synthType != DrumSynthType::CymbalSynth;
    }

    // DRIV and SNAP after the drum itself.
    float finish(float sample)
    {
        const bool driven = drive > 0.0f && usesCommonDrive();
        if (driven || driveMix > 0.0f)
        {
            // Gain 1..7 into a soft clipper (flat from +-3 on, where the tanh approximation reaches
            // exactly 1), then down by gain^0.3: the body is squared off and the tail comes up, and
            // a kit with every drum driven stays about as loud as it was.
            if (driven && drive != finishDrive)
            {
                finishDrive = drive;
                finishGain = 1.0f + drive * 6.0f;
                finishTrim = std::pow(finishGain, -0.3f);
            }
            // (The clipper is not transparent at its lowest drive: where the drive comes in or
            // goes out under a ringing drum, the stage is faded in or out over 4 ms.)
            const float clipped = dsp::fastTanh(std::clamp(sample * finishGain, -3.0f, 3.0f)) * finishTrim;
            driveMix = hitFirst ? (driven ? 1.0f : 0.0f) : dsp::towards(driveMix, driven ? 1.0f : 0.0f, levelStep);
            sample = driveMix >= 1.0f ? clipped : sample + driveMix * (clipped - sample);
        }
        else if (hitFirst)
            driveMix = 0.0f;
        if (struckSnap != 0.5f && usesCommonSnap() && edgeEnv > 1.0e-4f)
        {
            if (struckSnap < 0.5f)
            {
                sample *= 1.0f - (0.5f - struckSnap) * 2.0f * edgeEnv;
            }
            else
            {
                const float hard = (struckSnap - 0.5f) * 2.0f;
                sample *= 1.0f + hard * EDGE_LIFT * edgeEnv;
                if (hasSnapClick())
                {
                    sample += nextNoise() * hard * CLICK_LEVEL * clickEnv * ampEnv.getCurrentLevel();
                    clickEnv *= clickDecay;
                }
            }
            edgeEnv *= edgeDecay;
        }
        return sample;
    }

    void strike(float vel)
    {
        velocity = std::clamp(vel, 0.0f, 1.0f);
        active = true;
        lastLfoTone = tone;

        osc1Phase = 0.0;
        osc2Phase = 0.0;
        // (A cymbal machine that is still ringing is struck again: nothing of it starts over.)
        if (! (synthType == DrumSynthType::CymbalSynth && cym.isActive()))
            filter.reset();
        // (The hats' oscillators start every hit from the same place: the same hit every time.)
        static constexpr double hatStart[6] = { 0.0, 0.31, 0.62, 0.17, 0.83, 0.45 };
        for (int i = 0; i < 6; ++i) hatPhase[i] = hatStart[i];
        hatBandHigh.reset();
        hatBandLow.reset();

        // Clap burst state
        clapBurstCount = 0;
        clapBurstSample = 0;
        clapBurstSpacing = 0;
        clapBurstEnvLevel = 0.0f;

        configureEnvelopes();
        struckDecay = ampEnv.getParams().decay;
        struckRelease = ampEnv.getParams().release;
        if (atDecayMod > 0.0f)
            applyDecayPressure();

        // SNAP: the front edge of this hit (finish()). A softened onset takes half the drum's
        // decay, within 2..25 ms, so a short drum is not swallowed by it.
        struckSnap = std::clamp(snap + atSnapMod, 0.0f, 1.0f);
        edgeEnv = 1.0f;
        clickEnv = 1.0f;
        const float edgeSeconds = struckSnap < 0.5f ? std::clamp(0.5f * struckDecay, EDGE_SOFT_MIN, EDGE_SOFT_MAX)
                                                    : EDGE_HARD_SECONDS;
        edgeDecay = std::exp(-1.0f / (edgeSeconds * (float) sampleRate));
        clickDecay = std::exp(-1.0f / (CLICK_SECONDS * (float) sampleRate));

        ampEnv.noteOn();
        pitchEnv.noteOn();

        // Type-specific noteOn setup
        switch (synthType)
        {
            case DrumSynthType::Clap:
            {
                int numBursts = 3 + (int)(drive * 3.0f);
                clapBurstCount = numBursts;
                clapBurstSample = 0;
                // Spacing in samples: 2ms base + drive * 10ms
                clapBurstSpacing = (int)((0.002 + drive * 0.010) * sampleRate);
                if (clapBurstSpacing < 1) clapBurstSpacing = 1;
                clapBurstEnvLevel = 1.0f;
                filter.setMode(SVFilter::Mode::BandPass);
                float q = 0.7f + tone * 4.0f;
                filter.setCutoff(tune);
                filter.setResonance(q);
                break;
            }
            case DrumSynthType::ClosedHat:
            case DrumSynthType::OpenHat:
            case DrumSynthType::Cymbal:
                filter.setMode(SVFilter::Mode::HighPass);
                filter.setCutoff(tone * 10000.0f + 500.0f);
                filter.setResonance(0.707f);
                break;
            case DrumSynthType::NoiseSnare:
            case DrumSynthType::FMSnare:
                filter.setMode(SVFilter::Mode::BandPass);
                filter.setCutoff(tone * 5000.0f + 500.0f);
                filter.setResonance(1.5f);
                break;
            case DrumSynthType::Shaker:
                filter.setMode(SVFilter::Mode::BandPass);
                filter.setCutoff(tune);
                filter.setResonance(shakerQ(tone));
                break;
            case DrumSynthType::ModCyclesKick:
                filter.setMode(SVFilter::Mode::LowPass);
                filter.setCutoff(tune * 20.0f);
                filter.setResonance(0.707f + tone * 3.0f);
                break;
            case DrumSynthType::KickSynth:
            {
                // TONE: a low-pass over the whole drum, 200 Hz .. 16 kHz.
                filter.setMode(SVFilter::Mode::LowPass);
                filter.setCutoff(200.0f * std::pow(80.0f, tone));
                filter.setResonance(0.707f);
                kickSample = 0;
                kickHoldSamples = (int) (std::clamp(kick[KickHold], 0.0f, 0.3f) * sampleRate);
                kickReleased = false;
                // The knock: a damped sine tick, 300 Hz .. 6 kHz, lasting 12 ms (dull) .. 1.5 ms (bright).
                knockLevel = std::clamp(kick[KickKnock], 0.0f, 1.0f);
                const float knockSeconds = 0.012f * std::pow(0.125f, std::clamp(kick[KickKnockTone], 0.0f, 1.0f));
                knockDecay = std::exp(-1.0f / (knockSeconds * (float) sampleRate));
                knockPhase = 0.0;
                break;
            }
            case DrumSynthType::CymbalSynth:
            {
                filter.setMode(SVFilter::Mode::LowPass);
                filter.setCutoff(2500.0f);
                filter.setResonance(0.5f);
                cymTop = cymbalTop(tone);
                CymbalMachine::Strike hit;
                hit.velocity = velocity;
                hit.tune = tune * bendRatio * std::exp2(atPitchMod);
                hit.decay = decay;
                hit.tone = tone;
                hit.snap = struckSnap;
                cym.strike(hit);
                break;
            }
            default:
                break;
        }
    }

    float processCore()
    {
        float sample = 0.0f;
        // Pitch bend: the whole kit moves together (DrumEngine::pitchBend). Pad pressure on TUNE:
        // up to an octave up.
        const float setTune = tune;
        tune *= bendRatio;
        if (atPitchMod != 0.0f || tunePressed)
        {
            tune *= std::exp2(atPitchMod);
            tunePressed = atPitchMod != 0.0f;
            // The two drums whose pitch is a filter tuned at the strike.
            if (tune != lastPressedTune && (synthType == DrumSynthType::Clap || synthType == DrumSynthType::Shaker))
                filter.setCutoff(tune);
            lastPressedTune = tune;
        }
        // Pad pressure on DRIV (the mod strip's drive comes through its STRIP route, which sets `drive`).
        // (Under a ringing hit the drive, the level and the tone are moved to, not jumped to - pad
        // pressure and step locks arrive between two blocks; a hit takes them as they stand. The
        // drive's range is a gain of 1 to 7: it takes 8 ms, the others 4.)
        const float setDrive = drive;
        const float driveTarget = std::min(1.0f, drive + atDriveMod), levelTarget = getEffectiveLevel();
        const bool first = struck;
        struck = false;
        hitFirst = first;
        driveNow = first ? driveTarget : dsp::towards(driveNow, driveTarget, 0.5f * levelStep);
        levelNow = first ? levelTarget : dsp::towards(levelNow, levelTarget, levelStep);
        atFilterNow = first ? atFilterMod : dsp::towards(atFilterNow, atFilterMod, levelStep);
        drive = driveNow;
        // Pad pressure on TONE.
        const float setTone = tone;
        if (atFilterNow != 0.0f || tonePressed)
        {
            tone = std::min(1.0f, tone + atFilterNow);
            tonePressed = atFilterNow != 0.0f;
            if (tone != lastLfoTone)
                retuneFilterForTone();
        }
        // (What reads the tone as it sounds - the bass's shape - hears it on the same lead.)
        toneNow = first ? tone : dsp::towards(toneNow, tone, levelStep);
        tone = toneNow;

        switch (synthType)
        {
            case DrumSynthType::FMKick:       sample = processFMKick(); break;
            case DrumSynthType::AnalogKick:    sample = processAnalogKick(); break;
            case DrumSynthType::ModCyclesKick: sample = processModCyclesKick(); break;
            case DrumSynthType::NoiseSnare:    sample = processNoiseSnare(); break;
            case DrumSynthType::FMSnare:       sample = processFMSnare(); break;
            case DrumSynthType::Clap:          sample = processClap(); break;
            case DrumSynthType::ClosedHat:     sample = processClosedHat(); break;
            case DrumSynthType::OpenHat:       sample = processOpenHat(); break;
            case DrumSynthType::Cymbal:        sample = processCymbal(); break;
            case DrumSynthType::Tom:           sample = processTom(); break;
            case DrumSynthType::Rim:           sample = processRim(); break;
            case DrumSynthType::Shaker:        sample = processShaker(); break;
            case DrumSynthType::Clave:         sample = processClave(); break;
            case DrumSynthType::SubBass:       sample = processSubBass(); break;
            case DrumSynthType::KickSynth:     sample = processKickSynth(); break;
            case DrumSynthType::CymbalSynth:   sample = processCymbalSynth(); break;
        }
        sample = finish(sample);

        if (ampEnv.getStage() == Envelope::Stage::Sustain && ampEnv.getCurrentLevel() < 0.001f)
        {
            ampEnv.noteOff();
            pitchEnv.noteOff();
        }

        if (synthType == DrumSynthType::CymbalSynth)
            active = cym.isActive();            // it has no envelope here: it rings until its modes have died
        else if (!ampEnv.isActive() && !pitchEnv.isActive())
            active = false;

        tune = setTune;
        drive = setDrive;
        tone = setTone;
        // (The cymbal machine takes the velocity at each stroke: a soft stroke does not turn down
        // what a hard one left ringing.)
        return sample * levelNow * (synthType == DrumSynthType::CymbalSynth ? 1.0f : velocity);
    }

public:
    void reset()
    {
        ampEnv.reset();
        pitchEnv.reset();
        filter.reset();
        osc1Phase = 0.0;
        osc2Phase = 0.0;
        active = false;
        clapBurstCount = 0;
        clapBurstSample = 0;
        cym.reset();
        clearAftertouch();
        strikeDeclick.reset();
    }

private:
    // --- Synthesis internals ---
    double sampleRate = 44100.0;
    DrumSynthType synthType = DrumSynthType::FMKick;
    bool active = false;

    // Parameters
    float tune = 100.0f;
    float decay = 0.3f;
    float tone = 0.5f;
    float drive = 0.0f;
    float snap = 0.5f;
    float level = 0.8f;
    float velocity = 1.0f;
    float bendRatio = 1.0f;

    // Aftertouch expression state
    DrumVoiceATDest atDest = DrumVoiceATDest::Off;
    float atDepth = 1.0f;
    float atPitchMod = 0.0f;
    float atDecayMod = 0.0f;
    float atFilterMod = 0.0f;
    float atLevelMod = 0.0f;
    float atDriveMod = 0.0f;
    float atSnapMod = 0.0f;
    bool tonePressed = false;       // the filters were last tuned with pressure on TONE
    bool tunePressed = false;
    float lastPressedTune = 0.0f;

    // Oscillators (simple phase-accumulating sines)
    double osc1Phase = 0.0, osc2Phase = 0.0;

    // Kick machine
    std::array<float, NumKickParams> kick { { 36.0f, 0.06f, -0.6f, 0.0f, 0.3f, 0.5f, 0.0f, -0.4f } };
    int kickSample = 0;            // samples since the strike
    int kickHoldSamples = 0;
    bool kickReleased = false;
    float knockLevel = 0.0f, knockDecay = 0.0f;
    double knockPhase = 0.0;

    // Cymbal machine
    CymbalMachine cym;
    float cymTop = 1.0f;           // TONE: the gain of its top

    // Noise
    uint32_t noiseSeed = 12345;

    float nextNoise()
    {
        noiseSeed ^= noiseSeed << 13;
        noiseSeed ^= noiseSeed >> 17;
        noiseSeed ^= noiseSeed << 5;
        return (float)(noiseSeed) / 2147483648.0f - 1.0f;
    }

    // Envelopes
    Envelope ampEnv;
    Envelope pitchEnv;

    // Filter
    SVFilter filter;
    SVFilter hatBandHigh, hatBandLow;           // the hats' two band-passes
    double hatPhase[6] = {};
    static constexpr float HAT_MAKEUP = 3.0f;
    static constexpr float HAT_CLOSE_SECONDS = 0.045f;      // an open hat closing: the closed hat's tail

    // LFOs
    VoiceLfos lfos;
    float lfoGain = 1.0f;
    float lfoPan = 0.0f;
    float lastLfoTone = -1.0f;

    // SNAP: the front edge of the hit
    static constexpr float EDGE_SOFT_MIN = 0.002f;          // the fade-in below 0.5: half the decay, within these
    static constexpr float EDGE_SOFT_MAX = 0.025f;
    static constexpr float EDGE_HARD_SECONDS = 0.003f;      // the lift above 0.5
    static constexpr float EDGE_LIFT = 1.0f;                // x2 at the very start with SNAP at the top
    static constexpr float CLICK_SECONDS = 0.0012f;
    static constexpr float CLICK_LEVEL = 0.5f;
    float finishDrive = -1.0f, finishGain = 1.0f, finishTrim = 1.0f;
    dsp::Declick strikeDeclick;     // on the voice's output: a hit on a drum that still rings (noteOn)
    bool struck = true;             // the next sample is a hit's first
    bool hitFirst = false;          // ... and this one is
    float driveMix = 0.0f;          // how much of the drive stage is heard (finish)
    float driveNow = 0.0f, levelNow = 0.8f, toneNow = 0.5f, atFilterNow = 0.0f, levelStep = 0.005f;
    float struckSnap = 0.5f;
    float struckDecay = 0.3f, struckRelease = 0.01f;       // the amp envelope's times as struck
    float edgeEnv = 0.0f, edgeDecay = 0.0f, clickEnv = 0.0f, clickDecay = 0.0f;

    // Clap-specific: burst state
    int clapBurstCount = 0;
    int clapBurstSample = 0;
    int clapBurstSpacing = 0;
    float clapBurstEnvLevel = 0.0f;

    // --- Helpers ---

    float sine(double& phase, double freq)
    {
        phase += freq / sampleRate;
        phase = dsp::wrapPhase(phase);
        return std::sin((float)(phase * dsp::TWO_PI_D));
    }

    float square(double& phase, double freq)
    {
        phase += freq / sampleRate;
        phase = dsp::wrapPhase(phase);
        return phase < 0.5 ? 1.0f : -1.0f;
    }

    static float saturate(float x, float amount)
    {
        return dsp::fastTanh(x * (1.0f + amount * 3.0f));
    }

    // --- Envelope configuration per type ---

    void configureEnvelopes()
    {
        Envelope::Params ampP;
        Envelope::Params pitchP;

        // Snap affects attack sharpness: higher snap = shorter attack transient
        float snapAttack = 0.001f + (1.0f - snap) * 0.01f;

        switch (synthType)
        {
            case DrumSynthType::FMKick:
            {
                ampP.attack = 0.001f;
                ampP.decay = decay;
                ampP.sustain = 0.0f;
                ampP.release = 0.01f;
                ampP.decayCurve = -0.5f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.1f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                pitchP.decayCurve = -0.8f;
                break;
            }
            case DrumSynthType::AnalogKick:
            {
                ampP.attack = 0.001f;
                ampP.decay = decay;
                ampP.sustain = 0.0f;
                ampP.release = 0.01f;
                ampP.decayCurve = -0.5f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.15f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                pitchP.decayCurve = -0.6f;
                break;
            }
            case DrumSynthType::ModCyclesKick:
            {
                ampP.attack = 0.001f;
                ampP.decay = decay;
                ampP.sustain = 0.0f;
                ampP.release = 0.01f;
                ampP.decayCurve = -0.5f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.1f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                pitchP.decayCurve = -0.8f;
                break;
            }
            case DrumSynthType::NoiseSnare:
            {
                // Amp envelope controls overall (noise-dominated)
                ampP.attack = 0.001f;
                ampP.decay = decay * 0.7f;
                ampP.sustain = 0.0f;
                ampP.release = 0.01f;
                ampP.decayCurve = -0.3f;

                // Pitch envelope used for the body sine (shorter)
                pitchP.attack = 0.001f;
                pitchP.decay = decay * 0.3f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                pitchP.decayCurve = -0.5f;
                break;
            }
            case DrumSynthType::FMSnare:
            {
                ampP.attack = 0.001f;
                ampP.decay = decay * 0.7f;
                ampP.sustain = 0.0f;
                ampP.release = 0.01f;
                ampP.decayCurve = -0.3f;

                pitchP.attack = 0.001f;
                pitchP.decay = decay * 0.3f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                pitchP.decayCurve = -0.5f;
                break;
            }
            case DrumSynthType::Clap:
            {
                // Main envelope for the reverb tail after bursts
                ampP.attack = 0.001f;
                ampP.decay = decay;
                ampP.sustain = 0.0f;
                ampP.release = 0.01f;
                ampP.decayCurve = -0.4f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.001f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                break;
            }
            case DrumSynthType::ClosedHat:
            {
                ampP.attack = 0.0f;
                ampP.decay = 0.03f + decay * 0.25f;         // (the kit's 0.04 = 40 ms)
                ampP.sustain = 0.0f;
                ampP.release = 0.005f;
                ampP.decayCurve = -0.3f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.001f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                break;
            }
            case DrumSynthType::OpenHat:
            {
                ampP.attack = 0.0f;
                ampP.decay = 0.1f + decay * 0.9f;
                ampP.sustain = 0.0f;
                ampP.release = 0.02f;
                ampP.decayCurve = -0.3f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.001f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                break;
            }
            case DrumSynthType::Cymbal:
            {
                ampP.attack = 0.001f;
                ampP.decay = 0.5f + decay * 2.5f;
                ampP.sustain = 0.0f;
                ampP.release = 0.05f;
                ampP.decayCurve = -0.4f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.001f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                break;
            }
            case DrumSynthType::Tom:
            {
                ampP.attack = snapAttack;
                ampP.decay = decay;
                ampP.sustain = 0.0f;
                ampP.release = 0.01f;
                ampP.decayCurve = -0.4f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.05f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                pitchP.decayCurve = -0.6f;
                break;
            }
            case DrumSynthType::Rim:
            {
                ampP.attack = 0.0f;
                ampP.decay = 0.0017f + decay * 0.011f;      // 5 ms at DEC 0.3 .. 46 ms
                ampP.sustain = 0.0f;
                ampP.release = 0.001f;
                ampP.decayCurve = -0.5f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.001f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                break;
            }
            case DrumSynthType::Shaker:
            {
                ampP.attack = 0.002f;
                ampP.decay = decay;
                ampP.sustain = 0.0f;
                ampP.release = 0.01f;
                ampP.decayCurve = -0.2f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.001f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                break;
            }
            case DrumSynthType::Clave:
            {
                ampP.attack = 0.0f;
                ampP.decay = 0.02f + decay * 0.03f;
                ampP.sustain = 0.0f;
                ampP.release = 0.005f;
                ampP.decayCurve = -0.3f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.001f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                break;
            }
            case DrumSynthType::SubBass:
            {
                // Velocity-sensitive attack: vel=1 -> 0.001s, vel=0.1 -> 0.05s
                float velAttack = 0.001f + (1.0f - velocity) * 0.049f;
                ampP.attack = velAttack;
                ampP.decay = 0.1f;
                ampP.sustain = 1.0f;  // Holds at full level
                ampP.release = decay * 0.5f;        // DEC: the fade once it is let go (50 ms at 0.1)
                ampP.decayCurve = 0.0f;

                pitchP.attack = 0.0f;
                pitchP.decay = 0.001f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                break;
            }
            case DrumSynthType::CymbalSynth:
            {
                // (Not used: the cymbal machine's modes each have their own decay.)
                ampP.attack = 0.0f;
                ampP.decay = decay;
                ampP.sustain = 1.0f;
                ampP.release = 0.01f;
                pitchP.attack = 0.0f;
                pitchP.decay = 0.001f;
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                break;
            }
            case DrumSynthType::KickSynth:
            {
                // The amp envelope holds at full level (sustain 1) for HOLD and is then released:
                // its release stage is the drum's decay, so DEC and ACRV shape the tail.
                // SNAP: how hard the front edge is, 30 ms (soft) .. 0.5 ms (1.7 ms at 0.7, the default kit's)
                ampP.attack = 0.0005f * std::pow(59.0f, 1.0f - snap);
                ampP.decay = 0.001f;
                ampP.sustain = 1.0f;
                ampP.release = decay;
                ampP.releaseCurve = std::clamp(kick[KickAmpCurve], -1.0f, 1.0f);

                pitchP.attack = 0.0f;
                pitchP.decay = std::clamp(kick[KickSweepTime], 0.002f, 1.0f);
                pitchP.sustain = 0.0f;
                pitchP.release = 0.001f;
                pitchP.decayCurve = std::clamp(kick[KickSweepCurve], -1.0f, 1.0f);
                break;
            }
        }

        ampEnv.setParams(ampP);
        pitchEnv.setParams(pitchP);
    }

    // --- Per-type process methods ---

    float processFMKick()
    {
        float pitchEnvVal = pitchEnv.process();
        float ampEnvVal = ampEnv.process();

        // Carrier frequency: sweeps from tune*4 down to tune
        double carrierFreq = tune + tune * 3.0 * pitchEnvVal;

        // Modulator at tune*2 with very short decay (reuse pitchEnv for timing)
        // Modulator has extra-short decay: dies faster than pitch sweep
        double modFreq = tune * 2.0;
        float modAmp = pitchEnvVal * pitchEnvVal; // Squared for faster click decay
        // TONE: the level of the click (0.5 = x1)
        float modSample = sine(osc2Phase, modFreq) * modAmp * 2.0f * (tone * 2.0f);

        // Feedback on modulator when drive > 0
        if (drive > 0.0f)
            modSample += modSample * drive * 0.5f;

        // Apply FM: modulator offsets carrier phase
        double modulatedFreq = carrierFreq + modSample * carrierFreq * 0.5;
        float out = sine(osc1Phase, modulatedFreq);

        out *= ampEnvVal;
        return out;
    }

    float processAnalogKick()
    {
        float pitchEnvVal = pitchEnv.process();
        float ampEnvVal = ampEnv.process();

        // Pure sine, gentler pitch sweep: tune*3 down to tune. TONE: how far it falls, from not at
        // all (0) through tune*3 (0.5) to tune*9 (1).
        const double fall = (double) (tone * 2.0f) * (double) (tone * 2.0f);
        double freq = tune + tune * 2.0 * fall * pitchEnvVal;
        float out = sine(osc1Phase, freq);

        out *= ampEnvVal;
        return out;
    }

    float processModCyclesKick()
    {
        float pitchEnvVal = pitchEnv.process();
        float ampEnvVal = ampEnv.process();

        // Sine body with FM click (like FMKick)
        double carrierFreq = tune + tune * 3.0 * pitchEnvVal;
        double modFreq = tune * 2.0;
        float modAmp = pitchEnvVal * pitchEnvVal;
        float modSample = sine(osc2Phase, modFreq) * modAmp * 2.0f;
        double modulatedFreq = carrierFreq + modSample * carrierFreq * 0.5;
        float out = sine(osc1Phase, modulatedFreq);

        // Resonant lowpass filter sweeping cutoff
        float filterCutoff = (float)(tune * 2.0 + tune * 18.0 * pitchEnvVal);
        filter.setCutoff(filterCutoff);
        out = filter.process(out);

        // Soft saturation from drive
        if (drive > 0.0f)
            out = saturate(out, drive);

        out *= ampEnvVal;
        return out;
    }

    float processNoiseSnare()
    {
        float ampEnvVal = ampEnv.process();
        float bodyEnvVal = pitchEnv.process();  // Shorter body envelope

        // Noise through bandpass
        float noise = nextNoise();
        float filteredNoise = filter.process(noise) * ampEnvVal;

        // Sine body at tune Hz
        float body = sine(osc1Phase, (double)tune) * bodyEnvVal;

        // Drive controls noise-to-body mix: 0 = more body, 1 = more noise
        float bodyMix = 1.0f - drive;
        float noiseMix = 0.5f + drive * 0.5f;
        float out = body * bodyMix + filteredNoise * noiseMix;

        return out;
    }

    float processFMSnare()
    {
        float ampEnvVal = ampEnv.process();
        float bodyEnvVal = pitchEnv.process();

        // FM body: carrier at tune, modulator at tune*1.47 (inharmonic)
        double modFreq = tune * 1.47;
        float modIndex = tone * 3.0f;
        float mod = sine(osc2Phase, modFreq) * modIndex;
        // Apply FM to carrier phase
        double carrierFreq = tune + mod * tune * 0.5;
        float body = sine(osc1Phase, carrierFreq) * bodyEnvVal;

        // Noise component
        float noise = nextNoise();
        float filteredNoise = filter.process(noise) * ampEnvVal;

        float bodyMix = 1.0f - drive;
        float noiseMix = 0.5f + drive * 0.5f;
        float out = body * bodyMix + filteredNoise * noiseMix;

        return out;
    }

    float processClap()
    {
        float ampEnvVal = ampEnv.process();

        float out = 0.0f;

        if (clapBurstCount > 0)
        {
            // During burst phase
            // Each burst is a short noise envelope (3-8ms)
            int burstDurationSamples = (int)((0.003 + snap * 0.005) * sampleRate);
            if (burstDurationSamples < 1) burstDurationSamples = 1;

            if (clapBurstSample < burstDurationSamples)
            {
                float burstEnv = 1.0f - (float)clapBurstSample / (float)burstDurationSamples;
                float noise = nextNoise();
                out = filter.process(noise) * burstEnv * clapBurstEnvLevel;
            }

            clapBurstSample++;

            // Check if time for next burst
            if (clapBurstSample >= clapBurstSpacing)
            {
                clapBurstSample = 0;
                clapBurstCount--;
                clapBurstEnvLevel *= 0.85f; // Each burst slightly quieter
            }
        }
        else
        {
            // Reverb tail: filtered noise shaped by amp envelope
            float noise = nextNoise();
            out = filter.process(noise) * ampEnvVal * 0.7f;
        }

        return out;
    }

    // The hats: six square oscillators at fixed, unrelated pitches (the classic analog drum-machine
    // recipe: a dense metallic cluster, the same at every hit), through two band-passes and then
    // the TONE high-pass. TUNE moves the whole cluster (8000 = the classic pitches). The closed and
    // the open hat are one sound with two lengths.
    float processClosedHat()
    {
        float ampEnvVal = ampEnv.process();
        (void)pitchEnv.process(); // Advance unused envelope

        static constexpr double hz[6] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
        const double scale = std::clamp((double) tune / 8000.0, 0.125, 1.5);

        float sum = 0.0f;
        for (int i = 0; i < 6; ++i)
        {
            const double inc = hz[i] * scale / sampleRate;
            double& ph = hatPhase[i];
            ph += inc;
            if (ph >= 1.0) ph -= 1.0;
            // A band-limited square: the two edges are rounded over one sample each.
            float sq = ph < 0.5 ? 1.0f : -1.0f;
            sq += dsp::polyBLEP(ph, inc);
            double half = ph + 0.5;
            if (half >= 1.0) half -= 1.0;
            sq -= dsp::polyBLEP(half, inc);
            sum += sq;
        }
        sum *= (1.0f / 6.0f);

        const float metal = hatBandHigh.process(sum) + 0.35f * hatBandLow.process(sum);
        return filter.process(metal) * ampEnvVal * HAT_MAKEUP;
    }

    float processOpenHat()
    {
        // The same sound with a long decay (configureEnvelopes); noteOff() closes it.
        return processClosedHat();
    }

    float processCymbal()
    {
        float ampEnvVal = ampEnv.process();
        (void)pitchEnv.process();

        // 8 partials at inharmonic ratios with slight random pitch drift
        static constexpr double ratios[8] = { 1.0, 1.34, 1.68, 2.0, 2.38, 2.68, 3.15, 3.56 };

        float sum = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            // Slight drift: use noise-seeded offset per partial
            double drift = 1.0 + (double)((noiseSeed >> (i * 3)) & 0xFF) * 0.0001;
            double freq = tune * ratios[i] * drift;
            double partialPhase = osc1Phase * ratios[i] * drift;
            partialPhase -= std::floor(partialPhase);
            sum += (partialPhase < 0.5 ? 1.0f : -1.0f);
        }
        osc1Phase += tune / sampleRate;
        osc1Phase = dsp::wrapPhase(osc1Phase);

        sum *= (1.0f / 8.0f);
        float out = filter.process(sum) * ampEnvVal;
        return out;
    }

    float processTom()
    {
        float pitchEnvVal = pitchEnv.process();
        float ampEnvVal = ampEnv.process();

        // Sine with mild pitch sweep: tune*1.5 to tune over 50ms
        double freq = tune + tune * 0.5 * pitchEnvVal;
        float body = sine(osc1Phase, freq);

        // Short noise transient through bandpass
        float noise = nextNoise();
        float noiseEnv = pitchEnvVal * pitchEnvVal; // Short noise burst
        float noisePart = noise * noiseEnv * 0.6f * tone;      // TONE: the stick noise (0.3 at the middle)

        float out = (body + noisePart) * ampEnvVal;
        return out;
    }

    float processRim()
    {
        float ampEnvVal = ampEnv.process();
        (void)pitchEnv.process();

        // Single sine burst at tune Hz (default 800Hz)
        float body = sine(osc1Phase, (double)tune);

        // Tone adds noise component
        float noise = 0.0f;
        if (tone > 0.0f)
            noise = nextNoise() * tone * 0.5f;

        float out = (body + noise) * ampEnvVal;
        return out;
    }

    float processShaker()
    {
        float ampEnvVal = ampEnv.process();
        (void)pitchEnv.process();

        // Filtered noise with amplitude tremolo
        float noise = nextNoise();
        // (The band-pass has a gain of Q; noise through it grows with sqrt(Q). Level as at Q 2.)
        float filtered = filter.process(noise) * std::sqrt(2.0f / shakerQ(tone));

        // Tremolo: use osc1Phase as LFO
        osc1Phase += 80.0 / sampleRate; // ~80 Hz tremolo
        osc1Phase = dsp::wrapPhase(osc1Phase);
        float tremoloDepth = 0.3f + drive * 0.5f;
        float tremolo = 1.0f - tremoloDepth * (0.5f + 0.5f * std::sin((float)(osc1Phase * dsp::TWO_PI_D)));

        float out = filtered * ampEnvVal * tremolo;
        return out;
    }

    float processClave()
    {
        float ampEnvVal = ampEnv.process();
        (void)pitchEnv.process();

        // Two sines at tune Hz and tune*1.33 Hz
        float s1 = sine(osc1Phase, (double)tune);
        float s2 = sine(osc2Phase, (double)(tune * 1.33));
        // TONE: the level of the upper partial (0.5 = as loud as the lower one)
        float out = (s1 + s2 * (tone * 2.0f)) * 0.5f * ampEnvVal;

        return out;
    }

    // The kick machine. Body: one oscillator whose pitch falls from TUNE + SWEP semitones to TUNE
    // over SWPT (curve SCRV), shaped from sine through triangle to a squared-off wave by SHAP.
    // Knock: a short damped tick added at the start. Then drive (soft clip) and the TONE low-pass.
    float processKickSynth()
    {
        const float pitchEnvVal = pitchEnv.process();
        if (!kickReleased && kickSample >= kickHoldSamples && ampEnv.getStage() == Envelope::Stage::Sustain)
        {
            ampEnv.noteOff();           // HOLD is over: the decay (the envelope's release) starts
            kickReleased = true;
        }
        const float ampEnvVal = ampEnv.process();
        ++kickSample;
        if (kickReleased && !ampEnv.isActive())
            pitchEnv.noteOff();         // the tail is over: let the voice go idle

        const double freq = (double) tune * std::exp2((double) (std::clamp(kick[KickSweep], 0.0f, 72.0f) * pitchEnvVal) / 12.0);
        osc1Phase = dsp::wrapPhase(osc1Phase + freq / sampleRate);
        const float s = std::sin((float) (osc1Phase * dsp::TWO_PI_D));

        // SHAP 0..0.5: sine -> triangle (same phase, same peaks). 0.5..1: the triangle pushed into
        // a limiter until it is nearly square.
        float body = shaped(s, std::clamp(kick[KickShape], 0.0f, 1.0f)) * ampEnvVal;

        float knock = 0.0f;
        if (knockLevel > 0.0001f)
        {
            const double knockHz = 300.0 * std::pow(20.0, (double) std::clamp(kick[KickKnockTone], 0.0f, 1.0f));
            knockPhase = dsp::wrapPhase(knockPhase + knockHz / sampleRate);
            knock = std::sin((float) (knockPhase * dsp::TWO_PI_D)) * knockLevel;
            knockLevel *= knockDecay;
        }

        float out = body + knock;
        if (drive > 0.0f)
        {
            const float gain = 1.0f + drive * 9.0f;
            out = dsp::fastTanh(out * gain) / dsp::fastTanh(gain);
        }
        return filter.process(out);
    }

    // The cymbal machine (CymbalMachine.h), then TONE: everything above about 2.5 kHz turned
    // down or up (the split is a 6 dB / octave one, so it tilts more than it cuts).
    float processCymbalSynth()
    {
        // (The offset keeps the split's filter out of denormal numbers when LOFI has gated the tail to nothing.)
        const float in = cym.process(tune) + 1.0e-18f;
        const float low = filter.process(in);
        return low + cymTop * (in - low);
    }

    float processSubBass()
    {
        float ampEnvVal = ampEnv.process();
        (void)pitchEnv.process();

        // Fixed pitch oscillator (no sweep). TONE: a sine (0) -> triangle -> squared-off.
        float out = shaped(sine(osc1Phase, (double)tune), tone);
        out *= ampEnvVal;
        return out;
    }

    // A sine s shaped by 0..1: up to 0.5 into a triangle (same phase, same peaks), from there the
    // triangle pushed into a limiter until it is nearly square.
    static float shaped(float s, float shape)
    {
        if (shape <= 0.0f)
            return s;
        const float tri = (2.0f / 3.14159265f) * std::asin(std::clamp(s, -1.0f, 1.0f));
        const float toTri = std::min(1.0f, shape * 2.0f);
        float body = s + (tri - s) * toTri;
        if (shape > 0.5f)
        {
            const float push = 1.0f + (shape - 0.5f) * 2.0f * 7.0f;        // 1 .. 8
            body = dsp::fastTanh(body * push) / dsp::fastTanh(push);
        }
        return body;
    }
};
