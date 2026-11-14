#pragma once

#include "../engine/CymbalMachine.h"
#include <map>

// Ready-made settings of the cymbal machine (drum type CSYN) and the two factory kits made of
// them. A sound is the pad's own page (TUNE, DEC, TONE, DRIV, SNAP, LEVL) and its CYM page.
// Each is a starting point to learn the machine from: the comment says which cells make it.
namespace CymbalPresets
{
    using M = CymbalMachine;
    using ParamValues = std::map<int, float>;       // parameter id -> value, as a preset holds them

    struct Sound
    {
        const char* name;
        float tune, decay, tone, drive, snap, level;
        int model;
        float ping, colour, noise, damp, bloom, velSens, lofi;
    };

    // The six-oscillator cymbal of the early analog drum machines: stationary, bright, three bands.
    static constexpr Sound analogCymbal { "Analog Cymbal", 8000.0f, 1.2f, 0.5f, 0.0f, 0.5f, 0.5f,
                                          M::Metal, 0.5f, 0.5f, 0.0f, 0.4f, 0.0f, 0.5f, 0.0f };
    // The same oscillators multiplied in pairs: denser and rougher, a little NOIS to blur the lines.
    static constexpr Sound ringCymbal   { "Ring Cymbal",   8000.0f, 0.9f, 0.5f, 0.0f, 0.5f, 0.45f,
                                          M::Ring, 0.4f, 0.55f, 0.25f, 0.5f, 0.0f, 0.5f, 0.0f };
    // The house ride: RIDE tuned down, a hard tip (SNAP), more ping than wash, and LOFI for the
    // grain of an early sampled cymbal.
    static constexpr Sound houseRide    { "House Ride",    6400.0f, 0.9f, 0.5f, 0.0f, 0.75f, 0.5f,
                                          M::Ride, 0.6f, 0.55f, 0.6f, 0.4f, 0.2f, 0.5f, 0.45f };
    // A clean ride: a stick on the bow, the wash left long under the ping.
    static constexpr Sound ride         { "Ride",          8000.0f, 1.3f, 0.5f, 0.0f, 0.7f, 0.5f,
                                          M::Ride, 0.5f, 0.5f, 0.5f, 0.4f, 0.3f, 0.6f, 0.0f };
    // The cup of the ride: PING up (hardly any wash), NOIS down (pure partials).
    static constexpr Sound rideBell     { "Ride Bell",     8000.0f, 1.0f, 0.5f, 0.0f, 0.75f, 0.5f,
                                          M::Bell, 0.8f, 0.5f, 0.2f, 0.4f, 0.2f, 0.5f, 0.0f };
    // A crash: nearly all wash, BLOM so the top arrives a moment after the stroke, a long DEC.
    static constexpr Sound crash        { "Crash",         8000.0f, 1.5f, 0.5f, 0.0f, 0.6f, 0.5f,
                                          M::Crash, 0.4f, 0.5f, 0.6f, 0.45f, 0.5f, 0.6f, 0.0f };
    static constexpr Sound china        { "China",         8000.0f, 1.0f, 0.5f, 0.15f, 0.7f, 0.45f,
                                          M::China, 0.5f, 0.5f, 0.7f, 0.4f, 0.5f, 0.6f, 0.0f };
    static constexpr Sound splash       { "Splash",        8000.0f, 0.8f, 0.5f, 0.0f, 0.6f, 0.45f,
                                          M::Splash, 0.4f, 0.5f, 0.6f, 0.4f, 0.4f, 0.6f, 0.0f };
    // A gong with a note: a soft mallet (SNAP low), BLOM for the glide of its pitch.
    static constexpr Sound gong         { "Gong",          8000.0f, 2.0f, 0.5f, 0.0f, 0.2f, 0.6f,
                                          M::Gong, 0.5f, 0.5f, 0.4f, 0.4f, 0.6f, 0.7f, 0.0f };
    // A tam-tam: the low cluster at once, BLOM high so the shimmer swells over the first second.
    static constexpr Sound tamTam       { "Tam-Tam",       8000.0f, 2.0f, 0.5f, 0.0f, 0.15f, 0.6f,
                                          M::TamTam, 0.5f, 0.5f, 0.5f, 0.4f, 0.8f, 0.7f, 0.0f };

    static constexpr const Sound* all[] = { &analogCymbal, &ringCymbal, &houseRide, &ride, &rideBell,
                                            &crash, &china, &splash, &gong, &tamTam };

    // The parameter ids (DrumEngine: a pad's page is pad * 8 + n; its CYM page 528 + pad * 8 + n).
    static constexpr int PAD_PARAMS = 8;
    static constexpr int TYPE_OFFSET = 7;
    static constexpr int CYMBAL_TYPE = 15;          // DrumSynthType::CymbalSynth
    static constexpr int CYMBAL_BASE = 528;         // DrumEngine::CYMBAL_BASE

    template <typename Map>
    void set(Map& params, int pad, const Sound& s, float pan)
    {
        const int b = pad * PAD_PARAMS;
        params[b + 0] = s.tune;   params[b + 1] = s.decay; params[b + 2] = s.tone;  params[b + 3] = s.drive;
        params[b + 4] = s.snap;   params[b + 5] = s.level; params[b + 6] = pan;     params[b + TYPE_OFFSET] = (float) CYMBAL_TYPE;
        const int c = CYMBAL_BASE + pad * M::NumParams;
        params[c + M::ParModel] = (float) s.model;  params[c + M::ParPing] = s.ping;      params[c + M::ParColour] = s.colour;
        params[c + M::ParNoise] = s.noise;          params[c + M::ParDamp] = s.damp;      params[c + M::ParBloom] = s.bloom;
        params[c + M::ParVelSens] = s.velSens;      params[c + M::ParLofi] = s.lofi;
    }

    // "Cymbal Works": the machine's ten sounds on ten pads, to play and to take apart.
    //   bottom row, pads 1-6: analog cymbal, house ride, ride, ride bell, crash, china
    //   top row, pads 5-8:    splash, ring cymbal, gong, tam-tam
    // (Bottom pads 7 and 8 stay the kit's closed and open hat, which cut each other off: no cymbal
    // is put there. Top pads 1-4 stay its bass notes.)
    static constexpr const char* worksName = "Cymbal Works";
    template <typename Map>
    void works(Map& p)
    {
        set(p, 8, analogCymbal, 0.4f);  set(p, 9, houseRide, 0.6f);   set(p, 10, ride, 0.6f);
        set(p, 11, rideBell, 0.55f);    set(p, 12, crash, 0.4f);      set(p, 13, china, 0.65f);
        set(p, 4, splash, 0.35f);       set(p, 5, ringCymbal, 0.65f); set(p, 6, gong, 0.5f);      set(p, 7, tamTam, 0.5f);
    }

    // "House Kit": a kit to play at once. Its drums are set in FactoryPresets.h; these are its cymbals:
    //   bottom row, pad 2 crash, pad 3 house ride, pad 4 ride bell; top row, pad 7 analog cymbal, pad 8 splash.
    static constexpr const char* houseName = "House Kit";
    template <typename Map>
    void houseCymbals(Map& p)
    {
        set(p, 9, crash, 0.4f);         set(p, 10, houseRide, 0.6f);  set(p, 11, rideBell, 0.55f);
        set(p, 6, analogCymbal, 0.35f); set(p, 7, splash, 0.65f);
    }
}
