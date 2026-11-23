#pragma once

// The drum kit's two enums on their own: SampleBank.h renders its built-in hits with the drum
// voice and the drum voice's oscillator (dsp/PhaseOsc.h) reads the sample bank's cycles, so the
// bank names the types here and takes the voice itself at its end (U4-DRUM).

// The drum types. The first NUM_OFFERED are what a pad's TYPE cell offers, in this order (the
// number is stored).
// [U2b, FOLD] Toms, snares, claves, rim shots, hats and the cymbal machine's cymbals are sounds
// of the resonator drum (project/presets/ResonatorPresets.h) and of the oscillator bank now.
//   TOM SNAR FMSN CSYN    are no longer offered. The owner heard the pairs and accepted (the
//                         house ride was improved with a level-following crusher). The stored
//                         indices keep their meaning: DrumEngine maps them to the resonator drum.
//                         The old process methods stay in DrumVoice for SampleBank::renderHit.
//   RIM, CLAV             were folded by U2b; their code stays for SampleBank.
//   ClosedHat, OpenHat    are the oscillator bank's HAT and OHAT (setType makes them that).
// [KICKS] FMKick, AnalogKick and ModCyclesKick (FMKK KICK MKIK) are points of the kick machine:
// a pad of one of them is a KickSynth whose KICK and KCK2 pages start at that kick
// (DrumVoice::kickPoint). Their numbers keep their meaning; their code is gone.
enum class DrumSynthType
{
    FMKick, AnalogKick, ModCyclesKick,
    Clap, Shaker, SubBass,
    KickSynth,      // the kick machine: its own page of sweep, shape and knock controls
    Cymbal,         // eight plain squares high up: what is heard of it is their folding-over
    Resonator,      // RSN: the resonator drum - the RSN engine's own voice at a pitch of its own (ResonatorVoice.h)
    OscBank,        // BANK: the oscillator bank (OscBank.h)
    Tom, NoiseSnare, FMSnare,
    CymbalSynth,    // the cymbal machine (CymbalMachine.h): its own page, CYM
    Rim, Clave,
    ClosedHat, OpenHat
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


class DrumVoice;
