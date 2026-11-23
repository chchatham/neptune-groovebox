#pragma once

#include "../engine/DrumVoice.h"
#include "../engine/OscBank.h"
#include "presets/ResonatorPresets.h"
#include <map>

// What the factory kits put on their pads, as parameter ids and values (a preset holds them so):
// a sound of the resonator drum (TYPE RSN: its SND and the pad's own page), a sound of the
// oscillator bank (TYPE BANK: its own page and its BANK page), and - for the kits written when
// toms, snares, claves, rim shots and hats were drum types of their own - the sound each of those
// has become. Then the kits that are made of nothing else.
// (The name is from when this file held the cymbal machine's ten sounds; they are sounds of the
// resonator drum and of the bank now.)
namespace CymbalPresets
{
    using ParamValues = std::map<int, float>;       // parameter id -> value

    // The parameter ids (DrumEngine: a pad's page is pad * 8 + n; its sound page 528 + pad * 8 + n).
    static constexpr int PAD_PARAMS = 8;
    static constexpr int TYPE_OFFSET = 7;
    static constexpr int SOUND_BASE = 528;          // DrumEngine::SOUND_BASE
    static constexpr int RSN_TYPE = (int) DrumSynthType::Resonator;
    static constexpr int BANK_TYPE = (int) DrumSynthType::OscBank;

    template <typename Map>
    void pad(Map& p, int n, float tune, float decay, float tone, float drive, float snap, float level, float pan, int type)
    {
        const int b = n * PAD_PARAMS;
        p[b + 0] = tune;  p[b + 1] = decay;  p[b + 2] = tone;  p[b + 3] = drive;
        p[b + 4] = snap;  p[b + 5] = level;  p[b + 6] = pan;   p[b + TYPE_OFFSET] = (float) type;
    }

    // A sound of the resonator drum, at the pitch and length it suggests (times these two).
    template <typename Map>
    void resonator(Map& p, int n, const char* tag, float level, float pan, float tune = 1.0f, float decay = 1.0f,
                   float tone = 0.5f, float drive = 0.0f, float snap = 0.5f)
    {
        const int index = resonatorSoundIndex(tag);
        const auto& sound = resonatorSounds()[(size_t) index];
        pad(p, n, sound.hz * tune, sound.dec * decay, tone, drive, snap, level, pan, RSN_TYPE);
        const int c = SOUND_BASE + n * PAD_PARAMS;
        p[c] = (float) index;
        for (int i = 1; i < PAD_PARAMS; ++i)
            p[c + i] = 0.0f;
    }

    // A sound of the oscillator bank.
    template <typename Map>
    void bank(Map& p, int n, int model, float tune, float decay, float tone, float drive, float snap, float level, float pan,
              float ping = 0.5f, float colour = 0.5f, float damp = 0.4f, float bloom = 0.0f, float couple = 0.0f, float lofi = 0.0f)
    {
        pad(p, n, tune, decay, tone, drive, snap, level, pan, BANK_TYPE);
        const int c = SOUND_BASE + n * PAD_PARAMS;
        p[c + OscBank::ParModel] = (float) model;  p[c + OscBank::ParPing] = ping;    p[c + OscBank::ParColour] = colour;
        p[c + OscBank::ParExcite] = 0.0f;          p[c + OscBank::ParDamp] = damp;    p[c + OscBank::ParBloom] = bloom;
        p[c + OscBank::ParCouple] = couple;        p[c + OscBank::ParLofi] = lofi;
    }

    // What the DEC of a former drum type is as the DEC of the sound it has become: the claves and
    // the rim shot scaled their cell to a few hundredths of a second.
    inline float foldedDecay(DrumSynthType type, float decay)
    {
        if (type == DrumSynthType::Clave) return 0.02f + decay * 0.03f;
        if (type == DrumSynthType::Rim)   return 0.0017f + decay * 0.011f;
        return decay;
    }
    // [FOLD] The sound a drum type becomes when it is not offered any more. TOM, SNAR, FMSN are
    // resonator drum sounds; CSYN (the cymbal machine) defaults to RIDE (the cymbal machine's
    // default model); RIM and CLAV were folded by U2b.
    inline const char* foldedSound(DrumSynthType type)
    {
        switch (type)
        {
            case DrumSynthType::NoiseSnare:  return "SNAR";
            case DrumSynthType::FMSnare:     return "FMSN";
            case DrumSynthType::Tom:         return "TOM";
            case DrumSynthType::Rim:         return "RIM";
            case DrumSynthType::Clave:       return "CLAV";
            case DrumSynthType::CymbalSynth: return "RIDE";
            default:                         return nullptr;
        }
    }

    // The sound of a drum type that is not offered any more (nullptr: it is, or was never one):
    // one list, foldedSound's.
    inline const char* retiredSound(DrumSynthType type)
    {
        return (int) type >= DrumVoice::NUM_OFFERED ? foldedSound(type) : nullptr;
    }

    // WHAT A STORED DRUM TYPE IS NOW: the one rule, for every way a type reaches a pad (the TYPE
    // cell from a file, a preset or a step lock: DrumEngine::setParameter; a kit: applyKitToVoices).
    // A type's number is stored, and all eighteen are still read; the pad's TYPE cell holds one of
    // the first DrumVoice::NUM_OFFERED only.
    //   voice   what the pad's DrumVoice is given (setType): a type that is a resonator drum sound
    //           now is the resonator drum - its old process code is for SampleBank's built-in hits
    //           alone -, a hat is the oscillator bank; the three old kicks go on as they are
    //           stored (setType makes them the kick machine)
    //   cell    what the TYPE cell holds and shows: RSN, BANK, or the type itself (an old kick
    //           keeps its number: FMKK, KICK and MKIK are offered)
    //   sound   the resonator sound a retired type became (nullptr: none)
    //   model   the bank's MODL a hat became (-1: none), set after the pad's sound page has been
    //           started from the bank's own
    struct TypeNow
    {
        DrumSynthType voice, cell;
        const char* sound;
        int model;
    };
    inline TypeNow typeNow(DrumSynthType stored)
    {
        if (const char* sound = retiredSound(stored))
            return { DrumSynthType::Resonator, DrumSynthType::Resonator, sound, -1 };
        if (stored == DrumSynthType::ClosedHat || stored == DrumSynthType::OpenHat)
            return { DrumSynthType::OscBank, DrumSynthType::OscBank, nullptr,
                     stored == DrumSynthType::OpenHat ? (int) OscBank::OpenHat : (int) OscBank::Hat };
        return { stored, stored, nullptr, -1 };
    }


    // A kick written with the cells of one of the old kick types (FMKK, KICK, MKIK): the kick
    // machine at that kick, TYPE keeping the old type's number, with its KICK page (400 + pad * 8)
    // and its KCK2 page (the sound page) as DrumVoice::kickPoint makes them.
    static constexpr int KICK_BASE = 400;           // DrumEngine::KICK_BASE
    template <typename Map>
    void kick(Map& p, int n, float tune, float decay, float tone, float drive, float snap, float level, float pan, DrumSynthType type)
    {
        const auto k = DrumVoice::kickPoint(type, tune, decay, tone, drive, snap, level);
        pad(p, n, k.tune, k.decay, k.tone, k.drive, k.snap, k.level, pan, (int) type);
        for (int i = 0; i < DrumVoice::NumKickParams; ++i)
            p[KICK_BASE + n * DrumVoice::NumKickParams + i] = k.kick[(size_t) i];
        for (int i = 0; i < DrumVoice::NumKick2Params; ++i)
            p[SOUND_BASE + n * PAD_PARAMS + i] = k.kick2[(size_t) i];
    }

    // A pad by the drum type its kit was written with: the types that are sounds now become
    // those sounds, with the cells the kit gave them.
    template <typename Map>
    void drum(Map& p, int n, float tune, float decay, float tone, float drive, float snap, float level, float pan, DrumSynthType type)
    {
        if (const char* tag = foldedSound(type))
        {
            pad(p, n, tune, foldedDecay(type, decay), tone, drive, snap, level, pan, RSN_TYPE);
            const int c = SOUND_BASE + n * PAD_PARAMS;
            p[c] = (float) resonatorSoundIndex(tag);
            for (int i = 1; i < PAD_PARAMS; ++i)
                p[c + i] = 0.0f;
        }
        else if (type == DrumSynthType::ClosedHat || type == DrumSynthType::OpenHat)
            bank(p, n, type == DrumSynthType::OpenHat ? OscBank::OpenHat : OscBank::Hat, tune, decay, tone, drive, snap, level, pan);
        else if (DrumVoice::isKickPoint(type))
            kick(p, n, tune, decay, tone, drive, snap, level, pan, type);
        else
            pad(p, n, tune, decay, tone, drive, snap, level, pan, (int) type);
    }

    // "Cymbal Works": ten metal sounds on ten pads, to play and to take apart.
    //   bottom row, pads 1-6: analog cymbal (BANK METL), sheet ride tuned down, ride, ride bell, crash, china
    //   top row, pads 5-8:    splash, ring cymbal (BANK RING), gong, tam-tam
    // (Bottom pads 7 and 8 stay the kit's closed and open hat, which cut each other off: no cymbal
    // is put there. Top pads 1-4 stay its bass notes.)
    static constexpr const char* worksName = "Cymbal Works";
    template <typename Map>
    void works(Map& p)
    {
        bank(p, 8, OscBank::Metal, 8000.0f, 1.2f, 0.5f, 0.0f, 0.5f, 0.5f, 0.4f);
        resonator(p, 9, "RID2", 0.5f, 0.6f, 0.8f, 0.7f, 0.5f, 0.0f, 0.6f);
        resonator(p, 10, "RIDE", 0.5f, 0.6f);
        resonator(p, 11, "CUP", 0.5f, 0.55f);
        resonator(p, 12, "CRSH", 0.5f, 0.4f);
        resonator(p, 13, "CHNA", 0.45f, 0.65f);
        resonator(p, 4, "SPLS", 0.45f, 0.35f);
        bank(p, 5, OscBank::Ring, 8000.0f, 0.9f, 0.5f, 0.0f, 0.5f, 0.45f, 0.65f, 0.4f, 0.55f, 0.5f);
        resonator(p, 6, "GONG", 0.6f, 0.5f);
        resonator(p, 7, "TAM", 0.6f, 0.5f);
    }

    // "House Kit": a kit to play at once. Its drums are set in FactoryPresets.h; these are its cymbals:
    //   bottom row, pad 2 crash, pad 3 sheet ride tuned down, pad 4 ride bell; top row, pad 7 analog cymbal, pad 8 splash.
    static constexpr const char* houseName = "House Kit";
    template <typename Map>
    void houseCymbals(Map& p)
    {
        resonator(p, 9, "CRSH", 0.5f, 0.4f);
        resonator(p, 10, "RID2", 0.5f, 0.6f, 0.8f, 0.7f, 0.5f, 0.0f, 0.6f);
        resonator(p, 11, "CUP", 0.5f, 0.55f);
        bank(p, 6, OscBank::Metal, 8000.0f, 1.2f, 0.5f, 0.0f, 0.5f, 0.5f, 0.35f);
        resonator(p, 7, "SPLS", 0.45f, 0.65f);
    }
}
