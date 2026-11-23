#pragma once

#include "../dsp/Envelope.h"
#include "../dsp/DSPUtils.h"
#include "../dsp/LevelCrusher.h"
#include "../dsp/filters/SVFilter.h"
#include "../dsp/PhaseOsc.h"
#include "ModRouter.h"
#include "CymbalMachine.h"
#include "OscBank.h"
#include "ResonatorVoice.h"
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <array>

#include "DrumTypes.h"

// What the cells do, by type (every cell is heard on every type: the "Dead Controls" tests).
//   DEC   the length: the amp decay (hats, cymbal, clave and rim shot scale it to their own
//         range); on SUB, which sounds for as long as it is held, the fade after it is let go.
//   TONE  brightness, in the way of each drum: SNAR / FMSN the noise band (and FMSN's FM
//         index); CLAP the band's width; hats and cymbal the high-pass; TOM the stick noise; RIM
//         its noise; SHKR from a broad hiss to a narrow, pitched rattle; CLAV the upper partial;
//         SUB sine -> triangle -> squared-off; the kicks (KSYN and its points FMKK KICK MKIK) a
//         low-pass; CSYN how much of the top there is (the middle = the cymbal as it is).
//   DRIV  saturation (a soft clip that keeps the peak level) on every type; SNAR / FMSN also
//         shift from the body to the noise with it, CLAP adds claps, SHKR deepens its rattle.
//   SNAP  the front edge, 0.5 = the drum as it is: below, the onset is softened (it fades in
//         over up to about 12 ms); above, the first milliseconds are lifted (to +6 dB) and the
//         drums that start from a sine get a short noise click. TOM and the kicks set their attack
//         time with it instead (the kicks: EDGE of their KCK2 page is SNAP as the others have it),
//         CLAP also the length of each clap. RSN, and BANK's METL and RING: what strikes it, from
//         a soft mallet (0) through a stick (0.5) to a hard tip (1).
// [KICKS] FMKK, KICK and MKIK were three sine-sweep kicks of their own. They are points of the
// kick machine now (KickPoint below: its KICK and KCK2 cells at a setting): a pad of one of those
// TYPEs is a kick machine whose cells start where that kick was. kickPoint() turns a kick written
// with the old type's cells (TUNE DEC TONE DRIV SNAP LEVL) into the kick machine's, which is how
// the kits and the sample bank's hits were re-made ("Kick Points" measures old against new).
// [U2b] RSN, the resonator drum: TUNE is its pitch in Hz, DEC how long it rings (times what the
// sound says: a gong's second is longer than a clave's), TONE its tilt, SNAP the exciter's
// hardness, DRIV the saturation after it. Everything else is the sound (SND) and the seven cells
// of its RSN page, which are added to the sound's own.
class DrumVoice
{
public:
    // [U4] What the kit's modulation router can move on a drum (DrumEngine::kitDests builds the
    // table in this order; a DEST cell stores the index, so the first 42 are where the kit's LFO
    // destinations were before the router). One router for the kit: its LFO, STRIP, XIN and slot
    // cells are the kit's, each drum has its own LFOs (mod::Voice), and the cells that are a
    // drum's own - the kick machine's, the resonator drum's - are read from this drum's own set
    // values (mod::Voice::useBase on modBase).
    //   PTCH          TUNE as a ratio, an octave either way with the amount squared (per sample)
    //   DEC           DEC x 1/4 .. x 4 at full depth (+-2 octaves of time), read at the strike
    //                 (and by the resonator drum, which follows its DEC while it rings)
    //   TONE DRIV     +-1 around the cell (0..1), per sample
    //   SNAP          +-1 around the cell, read at the strike
    //   AMP           a gain 0..2 (below 0 from an audio-rate slot: ring modulation), per sample
    //   PAN           +-0.5 around PAN (the whole width), per sample
    //   SWEP .. ACRV  the kick machine's eight cells, this drum's own, in each cell's own scale:
    //                 SWEP SHAP KTON while the hit sounds, the others at the strike
    //   LRAT LDPT     the other LFOs' rate and depth
    //   MRPH .. HARD  the resonator drum's 24 cells (its page's sound, this drum's own values)
    //   PM            the phase of the drum's oscillator, a cycle either way at full depth (several
    //                 slots up to two), per sample: the
    //                 track input on it (XIN, or a free slot with source XIN) is phase modulation
    //                 of a kick or a tom by another track
    //   WPOS          where in its WAVE (a table) the oscillator reads, 0..1, per sample
    //   ETIM FTIM     the time of the amp envelope / of the drum's second envelope (the sweep, the
    //                 body: on the resonator drum its filter's), x 1/8 .. x 8
    enum ModDest
    {
        ModOff = 0, ModPitch, ModDecay, ModTone, ModDrive, ModSnap, ModAmp, ModPan,
        ModKickFirst,                                       // the kick machine's eight
        ModLfoRate = ModKickFirst + 8, ModLfoDepth,
        ModRsnFirst,                                        // the resonator drum's 24 (rsnDestCells())
        ModPhase = ModRsnFirst + 24, ModWavePos,
        ModEnvTime, ModFilterTime,
        NumModDests
    };
    static constexpr int NUM_RSN_DESTS = 24;
    static constexpr float PHASE_LIMIT = 4.0f * 3.14159265f;    // radians, what PM may add up to

    // The resonator drum's destinations, in the table's order: the voice cell behind each.
    static const std::array<int, NUM_RSN_DESTS>& rsnDestCells()
    {
        static const std::array<int, NUM_RSN_DESTS> cells = []
        {
            using V = ResonatorVoice;
            std::array<int, NUM_RSN_DESTS> c {};
            int n = 0;
            for (int cell : { (int) V::Morph, (int) V::ExciterMorph, (int) V::Energy })
                c[(size_t) n++] = cell;
            for (int i = 0; i < res::NumBody; ++i)   c[(size_t) n++] = V::BodyFirst + i;
            for (int i = 0; i < res::NumMore; ++i)   c[(size_t) n++] = V::MoreFirst + i;
            for (int i = 0; i < res::NumExcite; ++i) c[(size_t) n++] = V::ExciteFirst + i;
            jassert(n == NUM_RSN_DESTS);
            return c;
        }();
        return cells;
    }

    // This drum's side of the kit's router, and its own set values (one per destination of the
    // table; the engine fills them from the table's bases: 1 for PTCH, DEC, AMP, ETIM, FTIM).
    mod::Voice& getMod() { return mods; }
    const mod::Voice& getMod() const { return mods; }
    void initModBase(const mod::DestTable& table)
    {
        for (int i = 0; i < mod::DestTable::MAX; ++i)
            modBase[(size_t) i] = table[i].base;
        for (int k = 0; k < NumKickParams; ++k)
            modBase[(size_t) (ModKickFirst + k)] = kick[(size_t) k];
    }
    float modBaseOf(int dest) const { return modBase[(size_t) std::clamp(dest, 0, mod::DestTable::MAX - 1)]; }

    // The oscillator's wave (phaseosc::waveLabels(): SINE, CYCL, the built-in tables). It is the
    // wave of the drum's own oscillator - the body of the kicks, SUB, TOM, RIM, CLAV, the snares'
    // body and the FM drums' carrier - read with PM on its phase; the FM drums' modulator stays a
    // sine. CYCL has no sample here and reads as the sine.
    void setWave(int index)
    {
        waveIndex = std::clamp(index, 0, phaseosc::waveCount() - 1);
        wave = phaseosc::Wave::of(phaseosc::waveTable(waveIndex, nullptr));
    }
    int getWave() const { return waveIndex; }
    // Whether this drum's type reads the wave at all.
    bool readsWave() const
    {
        switch (synthType)
        {
            case DrumSynthType::NoiseSnare: case DrumSynthType::FMSnare: case DrumSynthType::Tom:
            case DrumSynthType::Rim: case DrumSynthType::Clave: case DrumSynthType::SubBass: case DrumSynthType::KickSynth:
                return true;
            default:
                return false;
        }
    }

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        ampEnv.setSampleRate(sr);
        pitchEnv.setSampleRate(sr);
        filter.setSampleRate(sr);
        cym.setSampleRate(sr);
        bank.setSampleRate(sr);
        rsn.setSampleRate(sr);
        rsn.setDrum(true);
        mods.prepare(sr);
        for (auto* o : { &osc1, &osc2, &knockOsc })
            o->setSampleRate(sr);
        strikeDeclick.setSampleRate(sr);
        crusher.prepare(sr);
        levelStep = (float) (1.0 / (0.004 * sr));       // a level's whole range in 4 ms
    }

    // [FOLD] What a pad's TYPE cell offers: the first ten. TOM, SNAR, FMSN, CSYN, RIM and CLAV
    // (10-15) are no longer offered: their stored numbers keep their meaning, and what each is now
    // is ONE rule, CymbalPresets::typeNow (a sound of the resonator drum), which DrumEngine applies
    // wherever a type reaches a pad. Not setType: see there.
    static constexpr int NUM_OFFERED = (int) DrumSynthType::OscBank + 1;

    void setType(DrumSynthType type)
    {
        // (What were drum types and are sounds now: see the top of the file. The three kicks that
        // are points of the kick machine are the kick machine: its cells are given with
        // applyKickPoint.)
        if (isKickPoint(type))
            type = DrumSynthType::KickSynth;
        if (type == DrumSynthType::ClosedHat || type == DrumSynthType::OpenHat)
        {
            bank.setParam(OscBank::ParModel, type == DrumSynthType::OpenHat ? (float) OscBank::OpenHat : (float) OscBank::Hat);
            type = DrumSynthType::OscBank;
        }
        // [FOLD] TOM, SNAR, FMSN, CSYN, RIM and CLAV are resonator drum sounds now. The fold is
        // DrumEngine's (CymbalPresets::typeNow), not here: setType(Tom) still runs processTom,
        // which is what SampleBank::renderHit uses to render its built-in hits unchanged.
        if (type != synthType)
        {
            cym.setPressure(0.0f);
            cym.reset();
            bank.setPressure(0.0f);
            bank.reset();
            rsn.setPadEnergy(0.0f);
            rsn.reset();
        }
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
        {
            kick[(size_t) index] = value;
            modBase[(size_t) (ModKickFirst + index)] = value;      // (this drum's own set value for the router)
        }
    }
    float getKickParam(int index) const { return (index >= 0 && index < NumKickParams) ? kick[(size_t) index] : 0.0f; }
    static float kickDefault(int index)
    {
        static constexpr float defaults[NumKickParams] = { 36.0f, 0.06f, -0.6f, 0.0f, 0.3f, 0.5f, 0.0f, -0.4f };
        return (index >= 0 && index < NumKickParams) ? defaults[index] : 0.0f;
    }

    // The kick machine's second page, KCK2: what FMKK, KICK and MKIK had that it had not. Each at 0
    // is the kick machine as it was (and 0 is what every kick pad of a project saved before this
    // page stored there). Read at the strike.
    enum Kick2Param
    {
        KickKind = 0,    // KIND  what KNCK and KTON make: TICK, the knock (a damped sine tick, as
                         //       before), or FM: a sine at a ratio of TUNE (KTON: x0.5 .. x8, x2 at
                         //       0.5) moves the body's frequency, as deep as KNCK says (x8 at 1) at
                         //       the start and falling with the square of the sweep: the FM click
        KickLin,         // LIN   0..1: the sweep falls in pitch (0: as before) .. in frequency (1:
                         //       it stays high longer, as FMKK, KICK and MKIK fell)
        KickRes,         // RES   0..1: the low-pass's resonance (Q 0.71 .. 3.71)
        KickFiltSweep,   // LPSW  0..20: the low-pass opens with the sweep, x(1 + LPSW) at its start
        KickFiltTrack,   // LPKT  0..1: what TONE's low-pass is measured from: 200 Hz (0) .. TUNE (1)
        KickSat,         // SAT   0..1: the body saturated before the amp envelope, so the tail is as
                         //       rough as the start (MKIK's drive)
        KickEdge,        // EDGE  -1..1: the front edge as SNAP is on the other drums, 0 = none:
                         //       below 0 the onset softened (it starts at 1 + EDGE of its level and
                         //       comes back over half the decay, 2 .. 25 ms); above 0 the first 3 ms
                         //       lifted (to +6 dB) and a noise click of about a millisecond
        KickPath,        // PATH  the order of the stages. KSYN (as before): the body through the
                         //       envelope, the knock added, the kick machine's own drive (back up to
                         //       full scale), then the low-pass over all of it. DRUM (as FMKK, KICK and
                         //       MKIK were, and as the other drums are): the low-pass on the body only,
                         //       SAT, the envelope, the knock added, and DRIV the kit's common drive
                         //       (a clip that keeps the kit about as loud) after everything
        NumKick2Params
    };
    enum KickKindChoice { KindTick = 0, KindFm, NumKickKinds };
    enum KickPathChoice { PathKsyn = 0, PathDrum, NumKickPaths };
    static constexpr float FM_CLICK_DEPTH = 8.0f;           // KIND FM: the depth at KNCK 1
    static constexpr float KICK_SWEEP_OPEN = 20.0f;         // LPSW's top

    void setKick2Param(int index, float value)
    {
        if (index >= 0 && index < NumKick2Params && std::isfinite(value))
            kick2[(size_t) index] = value;
    }
    float getKick2Param(int index) const { return (index >= 0 && index < NumKick2Params) ? kick2[(size_t) index] : 0.0f; }

    // Whether a type is one of the three kicks that are points of the kick machine now.
    static bool isKickPoint(DrumSynthType t)
    {
        return t == DrumSynthType::FMKick || t == DrumSynthType::AnalogKick || t == DrumSynthType::ModCyclesKick;
    }
    // A kick as the kick machine: the pad's DR cells and its KICK and KCK2 pages.
    struct KickPoint
    {
        float tune = 100.0f, decay = 0.3f, tone = 0.5f, drive = 0.0f, snap = 0.5f, level = 0.8f;
        std::array<float, NumKickParams> kick {};
        std::array<float, NumKick2Params> kick2 {};
    };

    // A kick written with the cells of one of the old kick types (or of the kick machine itself,
    // which is returned as it is), as the kick machine. What the old types did, and where it is now:
    //   FMKK  a sine falling in frequency from 4 x TUNE (0.1 s, curve -0.8) with an FM click at 2 x
    //         TUNE whose depth was 4 x TONE (x 1.5 at full DRIV)  -> SWEP 24 st, LIN 1, KIND FM,
    //         KNCK TONE / 2, TONE 1 (the low-pass at 80 x TUNE)
    //   KICK  a sine falling in frequency from (1 + 8 TONE^2) x TUNE (0.15 s, curve -0.6)
    //         -> SWEP 12 log2(1 + 8 TONE^2), LIN 1, TONE 1
    //   MKIK  FMKK's sweep and a click of depth 2, through a low-pass on the body at 2 x TUNE that
    //         opened ten times wider with the sweep, resonance TONE, saturated by DRIV after it and
    //         before the envelope -> as FMKK with KNCK 0.25, TONE log 2 / log 80 (2 x TUNE), LPSW 9,
    //         RES TONE, SAT DRIV, DRIV 0
    //   All three: PATH DRUM (their order of stages; DRIV the common drive, as it was), LPKT 1 (TONE
    //   a low-pass from TUNE, so it has something to take from a sine), the decay as the kick
    //   machine's (HOLD 0, ACRV -0.5, the old curve); SNAP is EDGE at 2 x (SNAP - 0.5) with the
    //   kick machine's shortest attack (1 ms, the old one).
    static KickPoint kickPoint(DrumSynthType type, float tune, float decay, float tone, float drive, float snap, float level)
    {
        KickPoint p;
        p.tune = tune; p.decay = decay; p.tone = tone; p.drive = drive; p.snap = snap; p.level = level;
        for (int n = 0; n < NumKickParams; ++n)
            p.kick[(size_t) n] = kickDefault(n);
        if (! isKickPoint(type))
            return p;

        tone = std::clamp(tone, 0.0f, 1.0f);
        drive = std::clamp(drive, 0.0f, 1.0f);
        snap = std::clamp(snap, 0.0f, 1.0f);
        const bool fm = type == DrumSynthType::FMKick, kick = type == DrumSynthType::AnalogKick;
        p.kick[KickSweep] = kick ? 12.0f * std::log2(1.0f + 8.0f * tone * tone) : 24.0f;
        p.kick[KickSweepTime] = kick ? 0.15f : 0.1f;
        p.kick[KickSweepCurve] = kick ? -0.6f : -0.8f;
        p.kick[KickShape] = 0.0f;
        p.kick[KickKnock] = fm ? std::min(1.0f, tone * 0.5f * (drive > 0.0f ? 1.0f + drive * 0.5f : 1.0f)) : (kick ? 0.0f : 0.25f);
        p.kick[KickKnockTone] = 0.5f;
        p.kick[KickHold] = 0.0f;
        p.kick[KickAmpCurve] = -0.5f;
        p.kick2[KickKind] = kick ? (float) KindTick : (float) KindFm;
        p.kick2[KickLin] = 1.0f;
        p.kick2[KickPath] = (float) PathDrum;
        p.kick2[KickFiltTrack] = 1.0f;
        if (type == DrumSynthType::ModCyclesKick)
        {
            p.tone = std::log(2.0f) / std::log(80.0f);
            p.kick2[KickFiltSweep] = 9.0f;
            p.kick2[KickRes] = tone;
            p.kick2[KickSat] = drive;
            p.drive = 0.0f;
        }
        else
            p.tone = 1.0f;
        p.snap = 1.0f;
        p.kick2[KickEdge] = (snap - 0.5f) * 2.0f;
        return p;
    }

    // The point a pad's TYPE starts from when it is turned to one of the old kicks (its other
    // cells as they stand): the kick at the old type's middle TONE, no DRIV, SNAP 0.5.
    static KickPoint kickPointOf(DrumSynthType type)
    {
        return kickPoint(type, 100.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.8f);
    }

    // The kick machine's cells (and, withCells, the DR cells) from a point.
    void applyKickPoint(const KickPoint& p, bool withCells = true)
    {
        if (withCells)
        {
            setTune(p.tune); setDecay(p.decay); setTone(p.tone); setDrive(p.drive); setSnap(p.snap); setLevel(p.level);
        }
        for (int n = 0; n < NumKickParams; ++n)
            setKickParam(n, p.kick[(size_t) n]);
        for (int n = 0; n < NumKick2Params; ++n)
            setKick2Param(n, p.kick2[(size_t) n]);
    }

    // --- The cymbal machine (type CymbalSynth; kept until the owner has decided: see the top) ---------
    // TUNE is its size (8000 = the model's own pitch), DEC how long it rings, TONE its brightness,
    // SNAP what strikes it; the eight cells of CymbalMachine::Param are the rest.
    void setCymbalParam(int index, float value) { cym.setParam(index, value); }
    float getCymbalParam(int index) const { return cym.getParam(index); }
    const CymbalMachine& getCymbalMachine() const { return cym; }

    // --- The oscillator bank (type OscBank) ------------------------------------------------------
    // TUNE moves its squares (8000 = their own pitches), DEC is how long it rings, TONE its
    // brightness, SNAP what strikes METL and RING (the hats: the front edge, as on every drum);
    // the eight cells of OscBank::Param are the rest. Read at the strike (LOFI at once).
    void setBankParam(int index, float value) { bank.setParam(index, value); }
    float getBankParam(int index) const { return bank.getParam(index); }
    const OscBank& getBank() const { return bank; }
    bool bankIsHat() const { return OscBank::isHat(bank.isActive() ? bank.struck() : bank.model()); }

    // --- The resonator drum (type Resonator) -----------------------------------------------------
    // The voice is the RSN engine's (ResonatorVoice.h, as a drum). The sound is given cell by cell:
    // a cell of the voice has the sound's own value (setResonatorBase) and what the pad adds
    // (setResonatorOffset: the cells of the pad's RSN page); DEC, TONE and SNAP act on three of
    // them. What a sound holds beside the voice's cells (level, envelope, filter, curve) goes
    // to getResonator() directly.
    ResonatorVoice& getResonator() { return rsn; }
    const ResonatorVoice& getResonator() const { return rsn; }
    void setResonatorBase(int cell, float value)
    {
        if (cell >= 0 && cell < ResonatorVoice::NumCells && std::isfinite(value))
        {
            rsnBase[(size_t) cell] = value;
            rsnStale = true;
        }
    }
    float getResonatorBase(int cell) const { return rsnBase[(size_t) std::clamp(cell, 0, (int) ResonatorVoice::NumCells - 1)]; }
    void setResonatorOffset(int cell, float value)
    {
        if (cell >= 0 && cell < ResonatorVoice::NumCells && std::isfinite(value))
        {
            rsnOffset[(size_t) cell] = value;
            rsnStale = true;
        }
    }
    // What the sound says of its host: how many seconds a second of DEC is, what the pad's
    // pressure is (energy for a bow or a brush; damping: a hat closed by the finger), and whether
    // it sounds only for as long as the pad is held (the open hat).
    // envelopeLength: DEC is the length of the sound's envelope (a sound cut to length by it:
    // not CHAN RING) and the body's own decay stays the sound's; else DEC is the body's decay.
    // driveWire: how much of DRIV is added to WIRE (a snare driven harder is more wires).
    // soundDec: the DEC the sound was made at (with envelopeLength the body's own decay goes
    // with DEC from there: a tom twice as long rings twice as long inside its envelope too).
    // lofi: the base amount of level-following crush (the cymbal machine's LOFI). DRIV adds on top
    // of it (the host hint driveLofi says how much): so a sound that specifies lofi and driveLofi
    // gets its grain from DRIV the way a snare gets its wires from driveWire.
    void setResonatorHost(float decScale, float pressEnergy, float pressDamp, bool gated, bool envelopeLength, float driveWire = 0.0f,
                          float soundDec = 0.3f, float lofi = 0.0f, float driveLofi = 0.0f)
    {
        rsnEnvelopeLength = envelopeLength;
        rsnDriveWire = driveWire;
        rsnLofi = std::clamp(lofi, 0.0f, 1.0f);
        rsnDriveLofi = std::clamp(driveLofi, 0.0f, 1.0f);
        rsnSoundDec = soundDec >= 0.001f ? soundDec : 0.3f;
        rsnDecScale = decScale >= 0.01f ? std::min(decScale, 16.0f) : 1.0f;
        rsnPressEnergy = pressEnergy;
        rsnPressDamp = pressDamp;
        rsnGated = gated;
        rsnStale = true;
    }
    // A drum of the kit against a voice of the RSN engine at the same settings (a kit has no
    // sixteen voices sounding at once to leave room for).
    static constexpr float RSN_HOST_GAIN = 2.0f;
    // How often (in samples) a resonator drum's cells are worked out again under an LFO, the strip or a slot.
    static constexpr int RSN_LOOK = 16;

    // The pad's own pressure, whatever the kit routes it to: the energy of a bowed or brushed
    // resonator drum and the hand on a hat (as its sound says), and what holds the bank's bands
    // up with its EXC cell (a sound that has rung out starts again under the pad).
    void setPadPressure(float pressure)
    {
        if (pending)
        {
            // (Struck at the next block: the pressure comes after the strike, as it came after the note.)
            waitingPad = pressure;
            padWaiting = true;
            return;
        }
        padPress = pressure > 0.0f ? std::min(pressure, 1.0f) : 0.0f;
        if (synthType == DrumSynthType::CymbalSynth)
        {
            cym.setPressure(padPress);
            active = active || cym.isActive();
        }
        else if (synthType == DrumSynthType::OscBank)
        {
            bank.setPressure(padPress);
            active = active || bank.isActive();
        }
        else if (synthType == DrumSynthType::Resonator)
        {
            rsn.setPadEnergy(padPress * rsnPressEnergy);
            if (rsnPressDamp != 0.0f)
                rsnStale = true;
            active = active || rsn.isActive();
        }
    }

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
        if (pending)
        {
            // (As setPadPressure: applied after the strike. The pan is the engine's: at once.)
            waitingPressure = pressure;
            pressureWaiting = true;
            return atDest == DrumVoiceATDest::Pan ? (mod * 2.0f - 1.0f) * 0.5f : 0.0f;
        }

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
        pressureWaiting = false;        // (a pressure that came before the strike and is gone again)
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
        if (synthType == DrumSynthType::OscBank)
        {
            bank.setDecayStretch(longer);
            return;
        }
        if (synthType == DrumSynthType::Resonator)
        {
            rsnStale = true;                    // its modes ring on (pushResonator)
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

    // A hit, struck at once, with nothing modulating it: the voice on its own (process()).
    void noteOn(float vel)
    {
        // Struck again while it rings: its oscillators and filter start over, which would cut the
        // ringing off at whatever value it had (a click). That value is carried down over 2 ms under
        // the new hit, whose own first sample is not touched. (A cymbal of the bank and the
        // resonator drum do not start over: a second stroke adds to what is ringing.)
        if (active && ! strokeAdds())
            strikeDeclick.armTail();
        pending = false;
        struck = true;
        strike(vel);
    }

    // Per-pad modulation (U5): what the pad's own LFO and two slots add on top of the kit's
    // router, at control rate. Each destination's entry is a share of the destination's span
    // (-1..1), as a slot's sum is in the router. Written by DrumEngine::computePadMod before
    // render, every sub-block.
    struct PadModOffsets
    {
        float v[NumModDests] {};
        bool active = false;
    };
    PadModOffsets padModOffsets {};
    // What a pad's own LFO and slots can move: the entries render() has a branch for. Not the
    // other LFOs' rate and depth (LRAT, LDPT: the kit's LFOs are the router's), nor the two that
    // need a value per sample (PM, WPOS), nor the cells of the track's effects (which are moved by
    // one voice for the whole track: the router's leader). The pad's DEST cells offer these only.
    static bool padDestActs(int dest)
    {
        return dest > ModOff && dest < NumModDests && dest != ModLfoRate && dest != ModLfoDepth
            && dest != ModPhase && dest != ModWavePos;
    }

    // A hit in the kit (DrumEngine::noteOn): the router's note-on now (the LFOs restart, the
    // randoms are drawn), the strike at this drum's next sub-block, after the router has been read
    // (mod::Voice::tick), so that DEC, SNAP, the kick machine's cells and the strip take the values
    // they have at the strike: the note-on's snap. The voice counts as sounding from now on.
    // key: what KEY reads (the drum's place in the kit).
    // What comes for this drum before the strike - its pressure, a note-off, a choke - is kept and
    // done right after it, in that order: as it came after the note.
    void trigger(const mod::Router& router, int key, float vel)
    {
        mods.noteOn(router, key, vel);
        if (! pending)
            tailOnStrike = active && ! strokeAdds();
        pending = true;                 // (two hits before a block: one, at the later velocity)
        pendingVelocity = vel;
        pendingOff = pendingChoke = false;
        active = true;
    }
    bool isPending() const { return pending; }

    // Letting the pad (or the step) go. A drum is a one-shot: it rings for its DEC however short
    // the tap was. SUB, the held bass note, sounds for as long as it is held and then fades
    // over its DEC.
    // The open hat is the other exception: it is open for as long as the pad, key or step lasts
    // and closes when that ends, so a tap on it is a closed hat and a held hit an open one.
    void noteOff()
    {
        if (pending)
        {
            pendingOff = true;
            return;
        }
        if (synthType == DrumSynthType::OscBank)
        {
            bank.release();             // (the open hat closes; nothing else of the bank hears it)
            return;
        }
        if (synthType == DrumSynthType::Resonator)
        {
            if (rsnGated)
                rsn.choke();
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
        if (pending)
        {
            pendingChoke = true;
            return;
        }
        if (synthType == DrumSynthType::CymbalSynth)
        {
            cym.choke();                        // a hand on it: gone in about a twentieth of a second
            return;
        }
        if (synthType == DrumSynthType::OscBank)
        {
            bank.choke();                       // a hat closes; a hand on a cymbal: gone in about a twentieth of a second
            return;
        }
        if (synthType == DrumSynthType::Resonator)
        {
            rsn.choke();
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

    // One sample of the voice on its own: nothing modulates it.
    float process()
    {
        if (!active)
            return 0.0f;
        if (pending)
            strikePending();
        return strikeDeclick.process(processCore());
    }

    // The envelopes the router's ENV and FENV read (a drum has no filter envelope; the resonator
    // drum's are its voice's).
    float ampLevel() { return synthType == DrumSynthType::Resonator ? rsn.getAmpEnvelope().getCurrentLevel() : ampEnv.getCurrentLevel(); }
    float filterLevel() { return synthType == DrumSynthType::Resonator ? rsn.getFilter().getEnvelope().getCurrentLevel() : 0.0f; }

    // One sub-block of the kit (DrumEngine::processBlock), for a drum that sounds: the samples
    // into out[] for as long as it sounds (how many: returned; the rest of the n is silence), and
    // the pan the router adds to this drum's (-0.5 .. 0.5) into pan[] when panMoved comes back
    // true. pressure: this drum's pad. (router: the kit's. A pad's own LFO and slots are not
    // router slots yet: they come as padModOffsets.)
    int render(const mod::Router& router, int n, float* out, float* pan, bool& panMoved, float pressure)
    {
        mods.useBase(modBase.data());
        if (synthType == DrumSynthType::Resonator)
        {
            // The resonator drum's cells as they are without modulation are their set values.
            const auto& cells = rsnDestCells();
            const float longer = 1.0f + 2.0f * atDecayMod;
            for (int d = 0; d < NUM_RSN_DESTS; ++d)
                modBase[(size_t) (ModRsnFirst + d)] = rsnCell(cells[(size_t) d], longer);
        }
        const bool kitMoves = mods.tick(router, n, ampLevel(), filterLevel(), pressure);
        const bool padMoves = padModOffsets.active;
        if (! kitMoves && ! padMoves)
        {
            // Nothing moves this drum: the voice as it is on its own.
            rsnKeyed = false;
            panMoved = false;
            // (An envelope time the pad's own modulation was holding goes back to its set value.)
            for (int t = 0; t < 2; ++t)
                if (padTimeHeld[t])
                {
                    padTimeHeld[t] = false;
                    setControl(ModEnvTime + t, modBase[(size_t) (ModEnvTime + t)]);
                }
            if (pending)
                strikePending();
            int i = 0;
            for (; i < n && active; ++i)
                out[i] = strikeDeclick.process(processCore());
            return i;
        }

        // What moves at control rate: for this sub-block (the hand-over lists every one still
        // routed; one that is let go comes once with its set value).
        float decayFactor = 1.0f, snapOffset = 0.0f;
        float kitTime[2] = { modBase[(size_t) ModEnvTime], modBase[(size_t) ModFilterTime] };    // ETIM, FTIM as the kit has them
        std::array<float, NumKickParams> setKick = kick;
        rsnMoved = 0;
        if (kitMoves)
        {
            mods.forEachControl([&](int dest, float value)
            {
                if (dest == ModDecay)                                   decayFactor = value;
                else if (dest == ModSnap)                               snapOffset = value;
                else if (dest >= ModKickFirst && dest < ModKickFirst + NumKickParams)
                    kick[(size_t) (dest - ModKickFirst)] = value;
                else if (dest >= ModRsnFirst && dest < ModRsnFirst + NUM_RSN_DESTS)
                {
                    const int c = rsnDestCells()[(size_t) (dest - ModRsnFirst)];
                    rsnMod[(size_t) c] = value;
                    rsnMoved |= uint64_t (1) << c;
                }
                else
                {
                    if (dest == ModEnvTime || dest == ModFilterTime)
                        kitTime[dest - ModEnvTime] = value;
                    setControl(dest, value);
                }
            });
        }

        // Per-pad modulation (U5): what the pad's own LFO and slots add to each destination
        // (DrumEngine::computePadMod, every sub-block), on top of what the kit's router made of it.
        // The control-rate destinations go by the router's own law (mod::Voice::lawOf: the share
        // times the entry's span - 72 st for SWEP, octaves of time for DEC, ETIM and FTIM -, held
        // to the entry's limits), so a pad's LFO on a cell moves it as far as the kit's would.
        float padPitchOffset = 0.0f, padToneOffset = 0.0f, padDriveOffset = 0.0f;
        float padAmpOffset = 0.0f, padPanOffset = 0.0f;
        const auto& po = padModOffsets.v;
        if (padMoves)
        {
            const auto& table = router.dests();
            if (po[ModDecay] != 0.0f)
                decayFactor = mod::Voice::lawOf(table[ModDecay], decayFactor, po[ModDecay]);
            if (po[ModSnap] != 0.0f)
                snapOffset = mod::Voice::lawOf(table[ModSnap], snapOffset, po[ModSnap]);
            // The kick machine's cells (as the kit's router left them for this sub-block).
            for (int k = 0; k < NumKickParams; ++k)
                if (po[ModKickFirst + k] != 0.0f)
                    kick[(size_t) k] = mod::Voice::lawOf(table[ModKickFirst + k], kick[(size_t) k], po[ModKickFirst + k]);
            // The resonator drum's cells: from what the kit's router made of the cell this
            // sub-block, or from the cell as it is set (never from what was left there before).
            for (int d = 0; d < NUM_RSN_DESTS; ++d)
                if (po[ModRsnFirst + d] != 0.0f)
                {
                    const int c = rsnDestCells()[(size_t) d];
                    const float from = ((rsnMoved >> c) & 1u) ? rsnMod[(size_t) c] : modBase[(size_t) (ModRsnFirst + d)];
                    rsnMod[(size_t) c] = mod::Voice::lawOf(table[ModRsnFirst + d], from, po[ModRsnFirst + d]);
                    rsnMoved |= uint64_t (1) << c;
                }
            // Per-sample destinations: constant offsets applied in the sample loop below.
            if (po[ModPitch] != 0.0f)   padPitchOffset = std::exp2(po[ModPitch]);      // a ratio
            if (po[ModTone] != 0.0f)    padToneOffset = po[ModTone];
            if (po[ModDrive] != 0.0f)   padDriveOffset = po[ModDrive];
            if (po[ModAmp] != 0.0f)     padAmpOffset = po[ModAmp];
            if (po[ModPan] != 0.0f)     padPanOffset = po[ModPan] * 0.5f;
        }
        // The envelopes' times: a factor on what the kit has (its route's value, or 1), and given
        // back when the pad's modulation lets go of it.
        for (int t = 0; t < 2; ++t)
        {
            const int dest = ModEnvTime + t;
            const bool now = padMoves && po[dest] != 0.0f;
            if (now)
                setControl(dest, mod::Voice::lawOf(router.dests()[dest], kitTime[t], po[dest]));
            else if (padTimeHeld[t])
                setControl(dest, kitTime[t]);
            padTimeHeld[t] = now;
        }

        rsnKeyed = rsnMoved != 0;
        if (rsnKeyed)
            rsnLook = 1;        // (the resonator looks at what the router has for it at the sub-block's start)
        const float* pitchIn = kitMoves ? mods.buffer(ModPitch) : nullptr;
        const float* toneIn = kitMoves ? mods.buffer(ModTone) : nullptr;
        const float* driveIn = kitMoves ? mods.buffer(ModDrive) : nullptr;
        const float* ampIn = kitMoves ? mods.buffer(ModAmp) : nullptr;
        const float* panIn = kitMoves ? mods.buffer(ModPan) : nullptr;
        const float* phaseIn = kitMoves ? mods.buffer(ModPhase) : nullptr;
        const float* placeIn = kitMoves ? mods.buffer(ModWavePos) : nullptr;

        // The cells are moved for these samples only (the set values stay).
        const float setTune = tune, setDecay = decay, setTone = tone, setDrive = drive, setSnap = snap;
        if (decayFactor != 1.0f)
            decay = std::max(0.001f, decay * decayFactor);
        snap = std::clamp(snap + snapOffset, 0.0f, 1.0f);
        int i = 0;
        for (; i < n && active; ++i)
        {
            float pitchMul = padPitchOffset != 0.0f ? padPitchOffset : 1.0f;
            if (pitchIn) tune = setTune * pitchIn[i] * pitchMul;
            else if (padPitchOffset != 0.0f) tune = setTune * padPitchOffset;
            float toneOff = padToneOffset;
            if (toneIn)  tone = std::clamp(setTone + toneIn[i] + toneOff, 0.0f, 1.0f);
            else if (toneOff != 0.0f) tone = std::clamp(setTone + toneOff, 0.0f, 1.0f);
            float drvOff = padDriveOffset;
            if (driveIn) drive = std::clamp(setDrive + driveIn[i] + drvOff, 0.0f, 1.0f);
            else if (drvOff != 0.0f) drive = std::clamp(setDrive + drvOff, 0.0f, 1.0f);
            phaseNow = phaseIn ? phaseIn[i] : 0.0f;
            placeNow = placeIn ? placeIn[i] : 0.0f;
            if (pending)
                strikePending();
            else if ((toneIn || toneOff != 0.0f) && tone != lastLfoTone)
                retuneFilterForTone();
            float ampMul = 1.0f + padAmpOffset;
            if (ampIn) ampMul = ampIn[i] + padAmpOffset;
            else if (padAmpOffset == 0.0f) ampMul = 1.0f;
            out[i] = strikeDeclick.process(processCore() * ampMul);
            float panVal = padPanOffset;
            if (panIn) panVal += panIn[i];
            pan[i] = panVal;
        }
        tune = setTune;
        decay = setDecay;
        tone = setTone;
        drive = setDrive;
        snap = setSnap;
        kick = setKick;
        phaseNow = placeNow = 0.0f;
        panMoved = panIn != nullptr || padPanOffset != 0.0f;
        return i;
    }

    // A control-rate destination that is not read for the sub-block only: the envelopes' times.
    // (Also what the engine hands back to a drum that has stopped sounding: restore().)
    void setControl(int dest, float value)
    {
        if (dest == ModEnvTime)
        {
            ampEnv.setTimeScale(value);
            rsn.getAmpEnvelope().setTimeScale(value);
        }
        else if (dest == ModFilterTime)
        {
            pitchEnv.setTimeScale(value);
            rsn.getFilter().setEnvTimeScale(value);
        }
    }

private:
    // The strike that trigger() left for this sub-block, then what came for the drum after it.
    void strikePending()
    {
        pending = false;
        if (tailOnStrike)
            strikeDeclick.armTail();
        tailOnStrike = false;
        struck = true;
        strike(pendingVelocity);
        if (pressureWaiting)
        {
            pressureWaiting = false;
            applyAftertouch(waitingPressure);
        }
        if (padWaiting)
        {
            padWaiting = false;
            setPadPressure(waitingPad);
        }
        if (pendingChoke)
            choke();
        if (pendingOff)
            noteOff();
        pendingOff = pendingChoke = false;
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
            case DrumSynthType::Cymbal:
                filter.setCutoff(tone * 10000.0f + 500.0f);
                break;
            case DrumSynthType::NoiseSnare:
            case DrumSynthType::FMSnare:
                filter.setCutoff(tone * 5000.0f + 500.0f);
                break;
            case DrumSynthType::KickSynth:
                kickLpTune = -1.0f;         // (worked out again: kickLowPass)
                kickFilterCount = 0;        // fresh coefficients now
                filter.setCutoff(kickLowPass(pitchEnv.getCurrentLevel()));
                break;
            case DrumSynthType::Shaker:
                filter.setResonance(shakerQ(tone));
                break;
            case DrumSynthType::CymbalSynth:
                cymTop = cymbalTop(tone);
                break;
            case DrumSynthType::OscBank:
                if (bankIsHat())
                    bank.setHatTone(tone);
                else
                    cymTop = cymbalTop(tone);
                break;
            default:
                break;
        }
    }

    // A stroke on these adds to what still rings; every other drum starts over.
    bool strokeAdds() const
    {
        return synthType == DrumSynthType::Resonator || synthType == DrumSynthType::CymbalSynth
            || (synthType == DrumSynthType::OscBank && ! bankIsHat());
    }

    // TONE of the bank's METL and RING while they sound: the gain of everything above about 2.5 kHz,
    // -18 dB .. 0 (the middle) .. +9 dB.
    static float cymbalTop(float tone) { return std::exp2((tone - 0.5f) * (tone < 0.5f ? 6.0f : 3.0f)); }

    // The shaker's band: Q 0.5 (a broad hiss) .. 2 at the middle .. 8 (a narrow, pitched rattle).
    static float shakerQ(float tone) { return 2.0f * std::pow(4.0f, 2.0f * (tone - 0.5f)); }

    // The types that have no drive of their own in their process function.
    bool usesCommonDrive() const
    {
        // (The kick machine has its own, before its low-pass; with PATH DRUM it has the common one.)
        return synthType != DrumSynthType::KickSynth || kickDrumPath;
    }

    // The types whose sound starts from a sine at zero: SNAP gives them a click.
    bool hasSnapClick() const
    {
        switch (synthType)
        {
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
        return synthType != DrumSynthType::Tom && synthType != DrumSynthType::KickSynth && ! strokeAdds();
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
        else if (synthType == DrumSynthType::KickSynth && kickEdge != 0.0f && edgeEnv > 1.0e-4f)
        {
            // The kick machine's EDGE: what SNAP is on the other drums, as a cell of its own.
            if (kickEdge < 0.0f)
            {
                sample *= 1.0f + kickEdge * edgeEnv;
            }
            else
            {
                sample *= 1.0f + kickEdge * EDGE_LIFT * edgeEnv;
                sample += nextNoise() * kickEdge * CLICK_LEVEL * clickEnv * ampEnv.getCurrentLevel();
                clickEnv *= clickDecay;
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

        osc1.reset();
        osc2.reset();
        // (What a stroke adds to is struck again while it rings: nothing of it starts over.)
        if (! (strokeAdds() && (bank.isActive() || rsn.isActive() || cym.isActive())))
            filter.reset();

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
        // (The kick machine's is its EDGE, its decay the DEC cell.)
        const bool kickSoft = synthType == DrumSynthType::KickSynth && kick2[KickEdge] < 0.0f;
        const float edgeSeconds = kickSoft ? std::clamp(0.5f * decay, EDGE_SOFT_MIN, EDGE_SOFT_MAX)
                                : struckSnap < 0.5f && synthType != DrumSynthType::KickSynth
                                      ? std::clamp(0.5f * struckDecay, EDGE_SOFT_MIN, EDGE_SOFT_MAX)
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
            case DrumSynthType::KickSynth:
            {
                // The second page as it stands at the strike.
                kickFm = kick2[KickKind] >= 0.5f;
                kickLin = std::clamp(kick2[KickLin], 0.0f, 1.0f);
                kickSat = std::clamp(kick2[KickSat], 0.0f, 1.0f);
                kickLpSweep = std::clamp(kick2[KickFiltSweep], 0.0f, KICK_SWEEP_OPEN);
                kickLpTrack = std::clamp(kick2[KickFiltTrack], 0.0f, 1.0f);
                kickEdge = std::clamp(kick2[KickEdge], -1.0f, 1.0f);
                kickDrumPath = kick2[KickPath] >= 0.5f;
                // TONE: a low-pass over the whole drum, 200 Hz .. 16 kHz (from TUNE x 1 .. x 80 with
                // LPKT; opened by the sweep with LPSW); RES its resonance.
                filter.setMode(SVFilter::Mode::LowPass);
                kickLpTune = -1.0f;
                filter.setCutoff(kickLowPass(1.0f));
                filter.setResonance(0.707f + std::clamp(kick2[KickRes], 0.0f, 1.0f) * 3.0f);
                kickSample = 0;
                kickFilterCount = 0;    // fresh coefficients on the first sample
                kickHoldSamples = (int) (std::clamp(kick[KickHold], 0.0f, 0.3f) * sampleRate);
                kickReleased = false;
                // The knock: a damped sine tick, 300 Hz .. 6 kHz, lasting 12 ms (dull) .. 1.5 ms (bright).
                knockLevel = std::clamp(kick[KickKnock], 0.0f, 1.0f);
                const float knockSeconds = 0.012f * std::pow(0.125f, std::clamp(kick[KickKnockTone], 0.0f, 1.0f));
                knockDecay = std::exp(-1.0f / (knockSeconds * (float) sampleRate));
                knockOsc.reset();
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
            case DrumSynthType::OscBank:
            {
                filter.setMode(SVFilter::Mode::LowPass);
                filter.setCutoff(2500.0f);
                filter.setResonance(0.5f);
                cymTop = cymbalTop(tone);
                OscBank::Strike hit;
                hit.velocity = velocity;
                hit.tune = tune * bendRatio * std::exp2(atPitchMod);
                hit.decay = decay;
                hit.tone = tone;
                hit.snap = struckSnap;
                bank.strike(hit);
                break;
            }
            case DrumSynthType::Resonator:
                rsnStale = true;
                pushResonator();
                rsn.setFrequency((double) (tune * bendRatio * std::exp2(atPitchMod)));
                rsn.setStroke(velocity);
                rsn.noteOn();
                break;
            default:
                break;
        }
    }

    // The resonator's cells that follow the pad: each is the sound's own value plus what the pad
    // adds, DEC, TONE, SNAP and the pad's pressure on top, and whatever an LFO, the strip or a
    // slot is routed to. Worked out again only when one of them moved.
    void pushResonator()
    {
        const float longer = 1.0f + 2.0f * atDecayMod;
        const bool keyed = rsnKeyed;
        if (! rsnStale && decay == rsnSeen[0] && tone == rsnSeen[1] && snap + atSnapMod == rsnSeen[2] && longer == rsnSeen[3]
            && drive == rsnSeen[4])
        {
            // (What an LFO, the strip or a slot moves is looked at as often as the resonator looks
            // at its cells, not every sample: sixteen pads under two LFOs cost fifteen times what
            // they cost without, and nothing of it was heard - the resonator glides between looks.)
            if (! keyed || --rsnLook > 0)
                return;
        }
        rsnLook = RSN_LOOK;
        rsnStale = false;
        rsnSeen = { decay, tone, snap + atSnapMod, longer, drive };
        if (rsnEnvelopeLength)
            rsn.getAmpEnvelope().setDecay(std::clamp(decay * rsnDecScale * longer, 0.001f, 20.0f));
        for (int c = 0; c < ResonatorVoice::NumCells; ++c)
        {
            float v = rsnCell(c, longer);
            // (What the router made of the cell, as of this sub-block's start, is what the resonator hears.)
            if (keyed && ((rsnMoved >> c) & 1u))
                v = rsnMod[(size_t) c];
            rsn.setCell(c, v);
        }
    }

    // A cell of the resonator drum without modulation: the sound's own value, what the pad adds,
    // DEC, TONE, SNAP, DRIV and the pad's pressure. (Also the router's set value of the cell.)
    float rsnCell(int c, float longer) const
    {
        using V = ResonatorVoice;
        {
            float v = rsnBase[(size_t) c] + rsnOffset[(size_t) c];
            if (c == V::Decay)
            {
                v = rsnEnvelopeLength ? std::clamp(v * decay * longer / rsnSoundDec, 0.01f, 60.0f)
                                      : std::clamp(decay * rsnDecScale * longer, 0.01f, 60.0f);
                // (A finger that damps - a hat closed by it - shortens the whole of it too.)
                if (rsnPressDamp > 0.0f)
                    v = std::max(0.01f, v * (1.0f - std::min(0.95f, padPress * rsnPressDamp)));
            }
            else if (c == V::BodyFirst + res::Tilt)     v -= (tone - 0.5f) * 0.6f;
            else if (c == V::ExciteFirst + res::Hard)   v += (std::min(1.0f, snap + atSnapMod) - 0.5f) * 0.8f;
            else if (c == V::BodyFirst + res::Damp)     v += padPress * rsnPressDamp;
            else if (c == V::ExciteFirst + res::Cont && rsnPressDamp > 0.0f)
                v -= 0.5f * padPress * rsnPressDamp;        // (... and what feeds it is cut short: the plates' burst)
            else if (c == V::MoreFirst + res::Wire)     v += drive * rsnDriveWire;
            else if (c == V::Energy)                    v = std::clamp(v, 0.0f, 1.0f);
            return v;
        }
    }

    float processResonator()
    {
        pushResonator();
        rsn.setFrequency((double) tune);
        float out = rsn.process(0.0f, 0.0f) * RSN_HOST_GAIN;
        // The level-following crusher from the sound's hint: its own base amount plus what DRIV adds.
        const float lofiAmount = rsnLofi + drive * rsnDriveLofi;
        if (lofiAmount > 0.001f)
            out = crusher.process(out, std::min(lofiAmount, 1.0f));
        return out;
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
            case DrumSynthType::NoiseSnare:    sample = processNoiseSnare(); break;
            case DrumSynthType::FMSnare:       sample = processFMSnare(); break;
            case DrumSynthType::Clap:          sample = processClap(); break;
            case DrumSynthType::Cymbal:        sample = processCymbal(); break;
            case DrumSynthType::Tom:           sample = processTom(); break;
            case DrumSynthType::Rim:           sample = processRim(); break;
            case DrumSynthType::Shaker:        sample = processShaker(); break;
            case DrumSynthType::Clave:         sample = processClave(); break;
            case DrumSynthType::SubBass:       sample = processSubBass(); break;
            case DrumSynthType::KickSynth:     sample = processKickSynth(); break;
            case DrumSynthType::CymbalSynth:   sample = processCymbalSynth(); break;
            case DrumSynthType::OscBank:       sample = processBank(); break;
            case DrumSynthType::Resonator:     sample = processResonator(); break;
            default: break;
        }
        sample = finish(sample);

        if (ampEnv.getStage() == Envelope::Stage::Sustain && ampEnv.getCurrentLevel() < 0.001f)
        {
            ampEnv.noteOff();
            pitchEnv.noteOff();
        }

        if (synthType == DrumSynthType::CymbalSynth)
            active = cym.isActive();
        else if (synthType == DrumSynthType::OscBank)
            active = bank.isActive();           // (they have no envelope here)
        else if (synthType == DrumSynthType::Resonator)
            active = rsn.isActive();            // it rings until its modes have died
        else if (!ampEnv.isActive() && !pitchEnv.isActive())
            active = false;

        tune = setTune;
        drive = setDrive;
        tone = setTone;
        // (What a stroke adds to takes the velocity at each stroke: a soft stroke does not turn
        // down what a hard one left ringing.)
        return sample * levelNow * (strokeAdds() ? 1.0f : velocity);
    }

public:
    void reset()
    {
        ampEnv.reset();
        pitchEnv.reset();
        filter.reset();
        osc1.reset();
        osc2.reset();
        active = false;
        pending = pendingOff = pendingChoke = tailOnStrike = false;
        pressureWaiting = padWaiting = false;
        clapBurstCount = 0;
        clapBurstSample = 0;
        cym.setPressure(0.0f);
        cym.reset();
        bank.setPressure(0.0f);
        bank.reset();
        padPress = 0.0f;
        rsn.setPadEnergy(0.0f);
        rsn.reset();
        crusher.reset();
        clearAftertouch();
        strikeDeclick.reset();
    }

private:
    // --- Synthesis internals ---
    double sampleRate = 44100.0;
    DrumSynthType synthType = DrumSynthType::KickSynth;
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

    // The oscillators (dsp/PhaseOsc.h): the drum's own (its WAVE, PM on its phase; the cymbal's
    // and the shaker's phase counter), the second (a sine: the FM drums' modulator, the clave's
    // upper partial), the kick machine's knock (a sine)
    phaseosc::PhaseOsc osc1, osc2, knockOsc;
    phaseosc::Wave wave;                // the sine until WAVE says otherwise
    int waveIndex = phaseosc::WAVE_SINE;
    float phaseNow = 0.0f;              // PM, radians, for this sample (render)
    float placeNow = 0.0f;              // WPOS, for this sample

    // Kick machine
    std::array<float, NumKickParams> kick { { 36.0f, 0.06f, -0.6f, 0.0f, 0.3f, 0.5f, 0.0f, -0.4f } };
    int kickSample = 0;            // samples since the strike
    int kickHoldSamples = 0;
    bool kickReleased = false;
    float knockLevel = 0.0f, knockDecay = 0.0f;
    std::array<float, NumKick2Params> kick2 {};                     // its second page (0: as it was)
    bool kickFm = false, kickDrumPath = false;                    // ... as struck
    float kickLin = 0.0f, kickSat = 0.0f, kickLpSweep = 0.0f, kickLpTrack = 0.0f, kickEdge = 0.0f;
    float kickLpTune = -1.0f, kickLpTone = -1.0f, kickLpBase = 1000.0f, kickLpSet = 1000.0f;     // the low-pass as last set
    int kickFilterCount = 0;                // samples until the next coefficient update
    static constexpr int KICK_FILTER_BLOCK = 8;     // update per sub-block of 8 samples
    float kickRatioOf = -1.0f, kickFmRatioOf = -1.0f;              // the cells the two ratios below are of
    double kickRatio = 1.0, kickFmRatio = 2.0;

    // The cymbal machine
    CymbalMachine cym;

    // The oscillator bank
    OscBank bank;
    float cymTop = 1.0f;           // TONE: the gain of its top (METL, RING)

    // The resonator drum
    ResonatorVoice rsn;
    //                                                      MRPH  XMPH  DCAY  ENRG  IN    VSEN  LAG
    std::array<float, ResonatorVoice::NumCells> rsnBase { { 6.0f, 0.0f, 0.5f, 0.5f, 0.0f, 1.0f, 0.015f } };
    std::array<float, ResonatorVoice::NumCells> rsnOffset {};
    std::array<float, 5> rsnSeen { { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f } };
    float rsnDriveWire = 0.0f, rsnLofi = 0.0f, rsnDriveLofi = 0.0f, rsnSoundDec = 0.3f;
    float rsnDecScale = 1.0f, rsnPressEnergy = 0.0f, rsnPressDamp = 0.0f, padPress = 0.0f;
    bool rsnGated = false, rsnStale = true, rsnEnvelopeLength = false;
    int rsnLook = 0;

    // The resonator drum's level-following crusher (its sound's lofi / driveLofi): the one the
    // cymbal machine and the oscillator bank hold for their LOFI (dsp/LevelCrusher.h).
    dsp::LevelCrusher crusher;

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

    // The kit's modulation router, this drum's side (DrumEngine; one router for the kit)
    mod::Voice mods;
    std::array<float, mod::DestTable::MAX> modBase {};      // this drum's set values, by destination (initModBase)
    std::array<float, ResonatorVoice::NumCells> rsnMod {};  // the resonator drum's cells as the router moves them
    uint64_t rsnMoved = 0;              // ... which of them it moves (this sub-block)
    bool rsnKeyed = false;              // ... any
    bool padTimeHeld[2] = { false, false };     // the pad's own modulation has ETIM / FTIM (render gives them back)
    float lastLfoTone = -1.0f;          // the TONE the filters were last tuned for

    // A hit waiting for this drum's next sub-block (trigger), and what came for it after the note
    bool pending = false, tailOnStrike = false, pendingOff = false, pendingChoke = false;
    float pendingVelocity = 1.0f;
    bool pressureWaiting = false, padWaiting = false;
    float waitingPressure = 0.0f, waitingPad = 0.0f;

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

    // The drum's own oscillator: one sample on, then its wave read with PM on its phase. On the
    // sine without PM: the arithmetic the drum had before PhaseOsc (the phase plus the step,
    // wrapped; the sine of it as a float), the same samples.
    float body(double freq)
    {
        const double step = freq / sampleRate;
        osc1.advance(step);
        return wave.isSine() ? osc1.sine(phaseNow) : osc1.read(wave, step, phaseNow, placeNow);
    }

    // The second oscillator, a sine.
    float sine2(double freq)
    {
        osc2.advance(freq / sampleRate);
        return osc2.sine(0.0f);
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
            default:
            {
                // (Not used: the bank and the resonator drum have their own.)
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

    float processNoiseSnare()
    {
        float ampEnvVal = ampEnv.process();
        float bodyEnvVal = pitchEnv.process();  // Shorter body envelope

        // Noise through bandpass
        float noise = nextNoise();
        float filteredNoise = filter.process(noise) * ampEnvVal;

        // Sine body at tune Hz
        float body = this->body((double)tune) * bodyEnvVal;

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
        float mod = sine2(modFreq) * modIndex;
        // Apply FM to carrier phase
        double carrierFreq = tune + mod * tune * 0.5;
        float body = this->body(carrierFreq) * bodyEnvVal;

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
            double partialPhase = osc1.getPhase() * ratios[i] * drift;
            partialPhase -= std::floor(partialPhase);
            sum += (partialPhase < 0.5 ? 1.0f : -1.0f);
        }
        osc1.advance(tune / sampleRate);

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
        float body = this->body(freq);

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
        float body = this->body((double)tune);

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

        // Tremolo: the first oscillator's phase as an LFO
        osc1.advance(80.0 / sampleRate); // ~80 Hz tremolo
        float tremoloDepth = 0.3f + drive * 0.5f;
        float tremolo = 1.0f - tremoloDepth * (0.5f + 0.5f * std::sin((float)(osc1.getPhase() * dsp::TWO_PI_D)));

        float out = filtered * ampEnvVal * tremolo;
        return out;
    }

    float processClave()
    {
        float ampEnvVal = ampEnv.process();
        (void)pitchEnv.process();

        // Two sines at tune Hz and tune*1.33 Hz
        float s1 = body((double)tune);
        float s2 = sine2((double)(tune * 1.33));
        // TONE: the level of the upper partial (0.5 = as loud as the lower one)
        float out = (s1 + s2 * (tone * 2.0f)) * 0.5f * ampEnvVal;

        return out;
    }

    // The kick machine. Body: one oscillator whose pitch falls from TUNE + SWEP semitones to TUNE
    // over SWPT (curve SCRV), shaped from sine through triangle to a squared-off wave by SHAP.
    // Knock: a short damped tick added at the start. Then drive (soft clip) and the TONE low-pass.
    // KCK2 (each at 0 the above): LIN the sweep in frequency, KIND FM the click as FM of the body,
    // PATH DRUM the low-pass on the body before SAT and the envelope and DRIV the common drive in
    // finish(), LPSW / LPKT / RES its cutoff following the sweep and TUNE and its resonance, EDGE
    // the front edge (finish()).
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

        // The sweep: in pitch (SWEP semitones above TUNE at its start), or with LIN towards a fall
        // in frequency (from 2^(SWEP/12) x TUNE, straight down to TUNE as the envelope falls).
        const float sweep = std::clamp(kick[KickSweep], 0.0f, 72.0f);
        double freq;
        if (kickLin > 0.0f)
        {
            // (The ratio at the sweep's start, and the FM ratio below, are worked out again only
            // when their cell moved: SWEP and KTON can be moved under the hit.)
            if (sweep != kickRatioOf)
            {
                kickRatioOf = sweep;
                kickRatio = std::exp2((double) sweep / 12.0);
            }
            const double inHz = (double) tune * (1.0 + (kickRatio - 1.0) * (double) pitchEnvVal);
            freq = kickLin >= 1.0f ? inHz
                                   : (double) tune * std::exp2((double) (sweep * pitchEnvVal) / 12.0);
            if (kickLin < 1.0f)
                freq += (double) kickLin * (inHz - freq);
        }
        else
            freq = (double) tune * std::exp2((double) (sweep * pitchEnvVal) / 12.0);
        // KIND FM: the click is a sine at KTON's ratio of TUNE moving the body's frequency.
        if (kickFm)
        {
            const float ktone = std::clamp(kick[KickKnockTone], 0.0f, 1.0f);
            if (ktone != kickFmRatioOf)
            {
                kickFmRatioOf = ktone;
                kickFmRatio = std::exp2(4.0 * (double) ktone - 1.0);
            }
            const float depth = std::clamp(kick[KickKnock], 0.0f, 1.0f) * FM_CLICK_DEPTH * (pitchEnvVal * pitchEnvVal);
            const float m = sine2((double) tune * kickFmRatio) * depth;
            freq += (double) m * freq * 0.5;
        }
        const float s = body(freq);

        // The low-pass follows the sweep (LPSW) or TUNE as it moves (LPKT): updated per
        // sub-block of KICK_FILTER_BLOCK samples with interpolation, not every sample.
        // The std::tan in setCutoff is the heaviest per-sample work; the sweep is smooth enough
        // that 8-sample updates are inaudible ("Kick Points" measures old against new).
        if (kickLpSweep > 0.0f || (kickLpTrack > 0.0f && tune != kickLpTune))
        {
            if (--kickFilterCount <= 0)
            {
                kickFilterCount = KICK_FILTER_BLOCK;
                const float was = kickLpSet;
                const float cutoff = kickLowPass(pitchEnvVal);
                if (cutoff != was)
                    filter.setCutoff(cutoff);
            }
        }

        // SHAP 0..0.5: sine -> triangle (same phase, same peaks). 0.5..1: the triangle pushed into
        // a limiter until it is nearly square. PATH DRUM: the low-pass here, on the body only.
        // SAT: then saturated, before the envelope.
        float body = shaped(s, std::clamp(kick[KickShape], 0.0f, 1.0f));
        if (kickDrumPath)
            body = filter.process(body);
        if (kickSat > 0.0f)
            body = saturate(body, kickSat);
        body *= ampEnvVal;

        float knock = 0.0f;
        if (knockLevel > 0.0001f && ! kickFm)
        {
            const double knockHz = 300.0 * std::pow(20.0, (double) std::clamp(kick[KickKnockTone], 0.0f, 1.0f));
            knockOsc.advance(knockHz / sampleRate);
            knock = knockOsc.sine(0.0f) * knockLevel;
            knockLevel *= knockDecay;
        }

        float out = body + knock;
        if (kickDrumPath)
            return out;                 // (DRIV: the common drive, in finish())
        if (drive > 0.0f)
        {
            const float gain = 1.0f + drive * 9.0f;
            out = dsp::fastTanh(out * gain) / dsp::fastTanh(gain);
        }
        return filter.process(out);
    }

    // The kick machine's low-pass for a sweep at p (1 at its start): TONE's 200 Hz .. 16 kHz, from
    // TUNE with LPKT, opened by the sweep with LPSW. (TONE and the base are worked out again only
    // when TONE or TUNE moved.)
    float kickLowPass(float p)
    {
        if (kickLpTune != tune || kickLpTone != tone)
        {
            kickLpTune = tune;
            kickLpTone = tone;
            kickLpBase = kickLpTrack > 0.0f ? 200.0f * std::pow(std::max(tune, 1.0f) / 200.0f, kickLpTrack) * std::pow(80.0f, tone)
                                            : 200.0f * std::pow(80.0f, tone);
        }
        kickLpSet = kickLpSweep > 0.0f ? kickLpBase * (1.0f + kickLpSweep * p) : kickLpBase;
        return kickLpSet;
    }

    // The cymbal machine (CymbalMachine.h), then TONE as after the bank's METL and RING (below).
    float processCymbalSynth()
    {
        const float in = cym.process(tune) + 1.0e-18f;
        const float low = filter.process(in);
        return low + cymTop * (in - low);
    }

    // The oscillator bank (OscBank.h). A hat has TONE's high-pass in it; after METL and RING, TONE
    // is everything above about 2.5 kHz turned down or up (the split is a 6 dB / octave one, so
    // it tilts more than it cuts).
    float processBank()
    {
        if (bankIsHat())
            return bank.process(tune);
        // (The offset keeps the split's filter out of denormal numbers when LOFI has gated the tail to nothing.)
        const float in = bank.process(tune) + 1.0e-18f;
        const float low = filter.process(in);
        return low + cymTop * (in - low);
    }

    float processSubBass()
    {
        float ampEnvVal = ampEnv.process();
        (void)pitchEnv.process();

        // Fixed pitch oscillator (no sweep). TONE: a sine (0) -> triangle -> squared-off.
        float out = shaped(body((double)tune), tone);
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
