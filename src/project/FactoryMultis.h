#pragma once

#include "MultiPreset.h"
#include "FactoryPresets.h"
#include "presets/PresetRoutes.h"
#include "../dsp/Saturation.h"
#include <vector>

// -----------------------------------------------------------------------------
// The built-in multi presets. They are NOT in FactoryPresets::getAll() (the
// sweeps over single presets count that list exactly); the "Multi Presets" suite
// loads and renders every one of these, and "Factory Multis" measures what each
// one's name promises.
//
// Adding one: write a function that returns it (routing by member number, see
// MultiPreset.h; router destinations and sources BY NAME, presets/PresetRoutes.h)
// and name it in builtIn() below, at the end. Code that cannot edit this file
// calls FactoryMultis::add() before the PresetManager is made.
// -----------------------------------------------------------------------------
namespace FactoryMultis
{
    // A step with a trig and these notes, `length` steps long.
    inline TrackData::StepData step(std::initializer_list<int> pitches, float length = 1.0f, float velocity = 1.0f)
    {
        TrackData::StepData s;
        s.trig = true;
        s.velocity = velocity;
        s.length = length;
        for (int pitch : pitches)
        {
            Note n;
            n.pitch = pitch;
            n.velocity = velocity;
            s.notes.push_back(n);
        }
        return s;
    }

    // A pattern of `length` empty steps for a member, to put trigs into.
    inline void blankPattern(MultiPreset::Member& member, int length = 16)
    {
        member.hasPattern = true;
        member.length = length;
        member.steps.assign((size_t) length, TrackData::StepData());
    }

    // One insert slot of a member's sound (the effect type and its first four cells, switched on,
    // fully wet), and its cells beyond four: PresetRoute's.
    using PresetRoute::insert;
    using PresetRoute::insertExtra;

    // Set the track input (TrackInParam) of a member's sound.
    inline void trackInput(ParamMap& sound, int srcMember, float level = 1.0f, int mode = 0)
    {
        sound[TrackInParam::SRC] = (float) srcMember;
        sound[TrackInParam::LEVEL] = level;
        sound[TrackInParam::MODE] = (float) mode;
    }

    // Duck Pair: a drum kit playing four on the floor, and a pad that ducks under it (DUCK on the
    // pad listens to member 1). The keys play the pad.
    inline MultiPreset duckPair()
    {
        MultiPreset m;
        m.name = "Duck Pair";
        m.isFactory = true;
        m.playMember = 2;

        MultiPreset::Member kit;
        kit.preset.name = "Duck Kit";
        kit.preset.category = "KIT";
        kit.preset.engineType = EngineType::Drum;
        blankPattern(kit);
        for (int beat = 0; beat < 4; ++beat)
            kit.steps[(size_t) (beat * 4)] = step({ 8 });           // (pad 9 of the kit: the kick)
        m.members.push_back(kit);

        MultiPreset::Member pad;
        pad.preset.name = "Duck Pad";
        pad.preset.category = "PAD";
        pad.preset.engineType = EngineType::UnifiedVoice;
        pad.preset.soundParams = {
            { 0, 0.5f }, { 1, 0.5f }, { 2, 0.5f }, { 5, 0.08f },
            { 10, 3000.0f }, { 11, 0.1f },
            { 20, 0.05f }, { 21, 2.0f }, { 22, 0.8f }, { 23, 1.5f }, { 24, 0.6f } };
        // SRC = member 1, THRS -30 dB, RATI 10, RELS 70 ms: the pad is back well before the next kick.
        insert(pad.preset.soundParams, 0, EffectType::SidechainEffect, { 1.0f, -30.0f, 10.0f, 70.0f });
        blankPattern(pad);
        pad.steps[0] = step({ 48, 55, 60, 64 }, 16.0f, 0.8f);       // one chord held for the bar
        m.members.push_back(pad);
        return m;
    }

    // ---- Building blocks of the members below ------------------------------------------------

    // A unified-voice sound in the engine's own ids (UnifiedParam): WAVE 0 SIN 1 SAW 2 SQR 3 TRI.
    inline ParamMap voice(float wave1, float wave2, float mix, float cutoffHz, float resonance,
                          float attack, float decay, float sustain, float release, float level)
    {
        namespace U = UnifiedParam;
        return { { U::WAVE1, wave1 }, { U::WAVE2, wave2 }, { U::MIX, mix }, { U::CUTOFF, cutoffHz }, { U::RESONANCE, resonance },
                 { U::AMP_ATTACK, attack }, { U::AMP_DECAY, decay }, { U::AMP_SUSTAIN, sustain }, { U::AMP_RELEASE, release },
                 { U::LEVEL, level } };
    }

    // A member that plays the kit a new track has (pad 9, note 8, is its kick machine).
    inline MultiPreset::Member kitMember(const char* name)
    {
        MultiPreset::Member kit;
        kit.preset.name = name;
        kit.preset.category = "KIT";
        kit.preset.engineType = EngineType::Drum;
        blankPattern(kit);
        return kit;
    }

    // A member that loops a drum break in time with the tempo (B100 and B170 are rendered from
    // the drum voices at start-up: always in the bank). The sample is named, so it is found again
    // wherever it sits in the bank; one held key plays it round and round.
    inline MultiPreset::Member breakMember(const char* name, const char* breakName, float level)
    {
        MultiPreset::Member m;
        m.preset.name = name;
        m.preset.category = "PERC";
        m.preset.engineType = EngineType::Sampler;
        const float index = PresetRoute::breakIndex(breakName);
        const auto* entry = SampleBank::get().entry((int) index);
        const float bars = entry != nullptr && entry->bars > 0.0f ? entry->bars : 1.0f;
        m.preset.soundParams = {
            { 24, index },      // SMPL
            { 6, 4.0f },        // MODE LOOP
            { 40, 1.0f },       // slices at the hits
            { 48, 4.0f },       // ALGO SLCE: the hits are moved, not stretched
            { 50, 1.0f },       // SYNC
            { 51, (float) juce::jlimit(0, 9, 4 + (int) std::lround(std::log2((double) bars))) },   // LEN: the break's bars
            { 22, 1.0f },       // VOX 1
            { 11, level } };
        m.preset.sampleName = breakName;
        blankPattern(m);
        m.steps[0] = step({ 60 }, 16.0f, 0.9f);
        return m;
    }

    // ---- THE TRACK-TO-TRACK MULTIS OF .ralph/spec-track-input.md ---------------------------------
    // What each one promises is measured in tests/factory_multi_tests.h ("Factory Multis").

    // Drum Harp: the kit strikes four strings. The resonator (RSN, STRG) holds a chord and takes
    // the kit as its exciter (IN): every hit rings the chord.
    inline MultiPreset drumHarp()
    {
        using E = ResonatorEngine;
        MultiPreset m;
        m.name = "DrumHarp";
        m.isFactory = true;
        m.playMember = 2;

        auto kit = kitMember("Harp Kit");
        kit.steps[0]  = step({ 8 });
        kit.steps[4]  = step({ 12 });
        kit.steps[8]  = step({ 8 });
        kit.steps[10] = step({ 8 });
        kit.steps[12] = step({ 12 });
        m.members.push_back(kit);

        MultiPreset::Member rsn;
        rsn.preset.name = "Harp Str";
        rsn.preset.category = "PAD";
        rsn.preset.engineType = EngineType::Resonator;
        rsn.preset.soundParams = {
            { E::MorphParam, 0.0f }, { E::ExciterMorphParam, 0.0f },       // STRG, struck
            { E::DecayParam, 3.0f }, { E::EnergyParam, 0.25f },             // (the chord's own strike is soft: the kit plays it)
            { E::LevelParam, 0.7f }, { E::InputParam, 0.9f },
            { E::AmpAttack, 0.001f }, { E::AmpDecay, 0.5f }, { E::AmpSustain, 1.0f }, { E::AmpRelease, 2.0f } };
        trackInput(rsn.preset.soundParams, 1, 1.0f);
        blankPattern(rsn);
        rsn.steps[0] = step({ 48, 52, 55, 60 }, 16.0f, 0.7f);
        m.members.push_back(rsn);
        return m;
    }

    // Voice Plate: a drum break through a sheet of metal (RSN, PLAT, the break as its exciter).
    inline MultiPreset voicePlate()
    {
        using E = ResonatorEngine;
        MultiPreset m;
        m.name = "VoxPlate";
        m.isFactory = true;
        m.playMember = 2;
        m.members.push_back(breakMember("Plate Src", "B100", 1.0f));

        MultiPreset::Member rsn;
        rsn.preset.name = "PlateRSN";
        rsn.preset.category = "PAD";
        rsn.preset.engineType = EngineType::Resonator;
        rsn.preset.soundParams = {
            { E::MorphParam, 3.0f }, { E::ExciterMorphParam, 0.0f },       // PLAT, struck
            { E::DecayParam, 4.0f }, { E::EnergyParam, 0.2f },
            { E::LevelParam, 1.0f }, { E::InputParam, 1.0f },
            { E::AmpAttack, 0.001f }, { E::AmpDecay, 1.0f }, { E::AmpSustain, 1.0f }, { E::AmpRelease, 1.5f } };
        trackInput(rsn.preset.soundParams, 1, 1.0f);
        // (What a break leaves in a plate is some 20 dB under the break: made up for here.)
        insert(rsn.preset.soundParams, 0, EffectType::CompressorEffect, { 0.0f, 1.0f, 5.0f, 18.0f });
        rsn.preset.soundParams[MixParam::VOLUME] = 1.0f;
        blankPattern(rsn);
        rsn.steps[0] = step({ 48 }, 16.0f, 0.8f);
        m.members.push_back(rsn);
        return m;
    }

    // Robot Break: the break speaks a chord. Four detuned saws a note through the vocoder (VOC,
    // BAND), the break its modulator: where the break is silent the chord is.
    inline MultiPreset robotBreak()
    {
        namespace U = UnifiedParam;
        MultiPreset m;
        m.name = "RobtBrk";
        m.isFactory = true;
        m.playMember = 2;
        m.members.push_back(breakMember("Break", "B100", 0.8f));

        MultiPreset::Member carrier;
        carrier.preset.name = "RobtSaw";
        carrier.preset.category = "LEAD";
        carrier.preset.engineType = EngineType::UnifiedVoice;
        carrier.preset.soundParams = voice(1.0f, 1.0f, 0.5f, 18000.0f, 0.0f, 0.005f, 0.1f, 1.0f, 0.3f, 0.7f);
        carrier.preset.soundParams[U::DETUNE2] = 12.0f;
        carrier.preset.soundParams[U::STACK] = 2.0f;        // four saws a note
        carrier.preset.soundParams[U::SPREAD] = 0.4f;
        // VOC: MODE BAND, BAND 0, a little noise for the consonants, all of it.
        insert(carrier.preset.soundParams, 0, EffectType::VocoderEffect, { 0.0f, 0.0f, 0.1f, 1.0f });
        trackInput(carrier.preset.soundParams, 1, 1.0f);
        PresetRoute::strip(carrier.preset.soundParams, EngineType::UnifiedVoice, 0, "CUT", -0.8f);     // the strip muffles the voice
        blankPattern(carrier);
        carrier.steps[0] = step({ 48, 55, 60 }, 16.0f, 0.8f);
        m.members.push_back(carrier);
        return m;
    }

    // Ring Choir: a pad multiplied by a wavetable. The track's input in MODE RING: the vowel
    // table of member 1 is the other side of a ring modulator, and what is heard are the sums and
    // differences of the two sounds' partials.
    inline MultiPreset ringChoir()
    {
        namespace U = UnifiedParam;
        using W = WavetableEngine;
        MultiPreset m;
        m.name = "RingVox";
        m.isFactory = true;
        m.playMember = 2;

        MultiPreset::Member table;
        table.preset.name = "Ring Tbl";
        table.preset.category = "PAD";
        table.preset.engineType = EngineType::Wavetable;
        table.preset.soundParams = {
            { W::TableSelect, FactoryPresets::wavetableByLabel("VOWL") }, { W::Position, 0.3f },
            { W::MorphSpeed, 0.2f }, { W::MorphDepth, 0.4f },
            { W::AmpAttack, 0.05f }, { W::AmpDecay, 1.0f }, { W::AmpSustain, 1.0f }, { W::AmpRelease, 1.0f },
            { W::Level, 0.9f } };
        blankPattern(table);
        table.steps[0] = step({ 55 }, 16.0f, 0.9f);
        m.members.push_back(table);

        MultiPreset::Member pad;
        pad.preset.name = "Choir";
        pad.preset.category = "PAD";
        pad.preset.engineType = EngineType::UnifiedVoice;
        pad.preset.soundParams = voice(3.0f, 1.0f, 0.3f, 3000.0f, 0.1f, 0.1f, 1.0f, 0.8f, 2.0f, 0.6f);
        pad.preset.soundParams[U::DETUNE2] = 7.0f;
        trackInput(pad.preset.soundParams, 1, 1.0f, 2);         // MODE RING
        PresetRoute::strip(pad.preset.soundParams, EngineType::UnifiedVoice, 0, "CUT", 0.5f);
        blankPattern(pad);
        pad.steps[0] = step({ 48, 55, 60, 64 }, 16.0f, 0.8f);
        m.members.push_back(pad);
        return m;
    }

    // Sample FM: a drum break as a seventh operator. The break at audio rate on the phase of
    // every operator of an FM bell (the XIN page: both routes on PMAL, one of them high-passed)
    // and twice more on the modulator's own (two slots: XIN > PM2): each hit tears the bell into
    // grain. A radian at full depth and full swing a route: four routes.
    inline MultiPreset sampleFM()
    {
        MultiPreset m;
        m.name = "SmplFM";
        m.isFactory = true;
        m.playMember = 2;
        m.members.push_back(breakMember("FM Grain", "B170", 1.0f));

        MultiPreset::Member fm;
        fm.preset.name = "FM Bell";
        fm.preset.category = "KEYS";
        fm.preset.engineType = EngineType::FM;
        fm.preset.soundParams = {
            { 0, 1.0f }, { 1, 0.8f }, { 4, 0.001f }, { 5, 2.5f }, { 6, 0.3f }, { 7, 1.5f },         // operator 1: the carrier
            { 10, 3.5f }, { 11, 0.45f }, { 14, 0.001f }, { 15, 1.5f }, { 16, 0.1f }, { 17, 1.0f },  // operator 2: the bell's ratio
            { 60, 0.0f }, { 61, 0.7f } };
        PresetRoute::xin(fm.preset.soundParams, EngineType::FM, 0, "PMAL", 1.0f);
        PresetRoute::xin(fm.preset.soundParams, EngineType::FM, 1, "PMAL", 1.0f, 2);
        PresetRoute::slot(fm.preset.soundParams, EngineType::FM, 0, "XIN", "PM2", 1.0f);
        PresetRoute::slot(fm.preset.soundParams, EngineType::FM, 1, "XIN", "PM2", 1.0f);
        trackInput(fm.preset.soundParams, 1, 1.0f);
        blankPattern(fm);
        fm.steps[0] = step({ 72 }, 4.0f, 0.8f);
        fm.steps[4] = step({ 76 }, 4.0f, 0.7f);
        fm.steps[8] = step({ 79 }, 4.0f, 0.8f);
        fm.steps[12] = step({ 84 }, 4.0f, 0.6f);
        m.members.push_back(fm);
        return m;
    }

    // Sync Saw: a lead whose saw is restarted by the bass. The sub of member 1 at audio rate on
    // SYNC of the lead (the XIN page: DST1 SYNC): the lead's wave begins again with every cycle
    // of the bass, so whatever note the lead plays, its overtones are the bass note's.
    inline MultiPreset syncSaw()
    {
        namespace U = UnifiedParam;
        MultiPreset m;
        m.name = "Sync Saw";
        m.isFactory = true;
        m.playMember = 2;

        MultiPreset::Member sub;
        sub.preset.name = "Sync Sub";
        sub.preset.category = "BASS";
        sub.preset.engineType = EngineType::UnifiedVoice;
        sub.preset.soundParams = voice(1.0f, 0.0f, 0.3f, 600.0f, 0.1f, 0.005f, 0.3f, 1.0f, 0.2f, 0.6f);
        sub.preset.soundParams[U::VOICE_COUNT] = 1.0f;
        blankPattern(sub);
        sub.steps[0] = step({ 36 }, 8.0f, 0.9f);
        sub.steps[8] = step({ 39 }, 4.0f, 0.9f);
        sub.steps[12] = step({ 34 }, 4.0f, 0.9f);
        m.members.push_back(sub);

        MultiPreset::Member lead;
        lead.preset.name = "SyncLead";
        lead.preset.category = "LEAD";
        lead.preset.engineType = EngineType::UnifiedVoice;
        lead.preset.soundParams = voice(1.0f, 1.0f, 0.0f, 9000.0f, 0.2f, 0.005f, 0.3f, 0.9f, 0.3f, 0.45f);
        PresetRoute::strip(lead.preset.soundParams, EngineType::UnifiedVoice, 0, "CUT", -0.6f);
        lead.preset.soundParams[U::VOICE_COUNT] = 1.0f;
        PresetRoute::xin(lead.preset.soundParams, EngineType::UnifiedVoice, 0, "SYNC", 1.0f);
        trackInput(lead.preset.soundParams, 1, 1.0f);
        blankPattern(lead);
        lead.steps[0] = step({ 66 }, 6.0f, 0.9f);
        lead.steps[6] = step({ 73 }, 2.0f, 0.8f);
        lead.steps[8] = step({ 70 }, 8.0f, 0.9f);
        m.members.push_back(lead);
        return m;
    }

    // Howl Loop: two tracks that listen to each other. Each adds the other to what it plays
    // (the input in MODE MIX); on the way round are a wavefolder (DIRT, TYPE FOLD) and a short tape echo,
    // and LEVL on the two IN pages is the gain of the loop: turned down, a note rings round a few
    // times and dies; at the stored 1 the pair howls by itself once a note has started it, and the
    // fold bends the howl the harder it is driven. Bounded by the input's own soft limit.
    // (The first track hears the second one block late: the loop's time is the echo's and a block.)
    inline MultiPreset howlLoop()
    {
        MultiPreset m;
        m.name = "HowlLoop";
        m.isFactory = true;
        m.playMember = 1;

        MultiPreset::Member a;
        a.preset.name = "Howl A";
        a.preset.category = "FX";
        a.preset.engineType = EngineType::UnifiedVoice;
        a.preset.soundParams = voice(3.0f, 0.0f, 0.0f, 3000.0f, 0.2f, 0.002f, 0.2f, 0.0f, 0.2f, 0.6f);
        // DIRT: DRIV 55, TYPE FOLD, no crusher, no rate reduction; of its second four only MIX, all of it.
        // (DIRT and not WSH: an effect of cells, so the track's router can move its DRIV - the strip does.)
        insert(a.preset.soundParams, 0, EffectType::DirtInsertEffect, { 0.55f, (float) sat::Fold, 0.0f, 0.0f });
        a.preset.soundParams[AudioEngine::INSERT_EXTRA_BASE + (DirtInsert::Mix - 4)] = 1.0f;
        trackInput(a.preset.soundParams, 2, 1.0f, 1);           // hears member 2, MODE MIX
        PresetRoute::strip(a.preset.soundParams, EngineType::UnifiedVoice, 0, "1:C1", 0.45f);    // the strip is the fold's DRIV
        a.preset.soundParams[MixParam::VOLUME] = 0.8f;
        blankPattern(a);
        a.steps[0] = step({ 45 }, 1.0f, 0.9f);
        m.members.push_back(a);

        MultiPreset::Member b;
        b.preset.name = "Howl B";
        b.preset.category = "FX";
        b.preset.engineType = EngineType::UnifiedVoice;
        b.preset.soundParams = voice(0.0f, 0.0f, 0.0f, 2000.0f, 0.0f, 0.01f, 0.2f, 0.0f, 0.2f, 0.3f);
        // TAPE: TIME 30 ms, no feedback of its own, a worn tape, the echo alone.
        insert(b.preset.soundParams, 0, EffectType::TapeDelayEffect, { 0.03f, 0.0f, 0.5f, 1.0f });
        trackInput(b.preset.soundParams, 1, 1.0f, 1);           // hears member 1, MODE MIX
        b.preset.soundParams[MixParam::VOLUME] = 0.8f;
        blankPattern(b);
        m.members.push_back(b);
        return m;
    }

    // Pump Pad: the kick's own loudness moves the pad, with no compressor anywhere. The follower
    // of the pad's input (the source IN) takes its level down and closes its filter, two slots.
    inline MultiPreset pumpPad()
    {
        namespace U = UnifiedParam;
        const auto T = EngineType::UnifiedVoice;
        MultiPreset m;
        m.name = "PumpPad";
        m.isFactory = true;
        m.playMember = 2;

        auto kit = kitMember("Pump Kit");
        for (int beat = 0; beat < 4; ++beat)
            kit.steps[(size_t) (beat * 4)] = step({ 8 });
        m.members.push_back(kit);

        MultiPreset::Member pad;
        pad.preset.name = "Pump Syn";
        pad.preset.category = "PAD";
        pad.preset.engineType = T;
        pad.preset.soundParams = voice(1.0f, 2.0f, 0.3f, 5000.0f, 0.2f, 0.1f, 1.5f, 0.8f, 2.0f, 0.4f);
        pad.preset.soundParams[U::DETUNE2] = 7.0f;
        PresetRoute::slot(pad.preset.soundParams, T, 0, "IN", "AMP", -1.0f);
        PresetRoute::slot(pad.preset.soundParams, T, 1, "IN", "CUT", -1.0f);
        PresetRoute::strip(pad.preset.soundParams, T, 0, "CUT", -0.5f);
        trackInput(pad.preset.soundParams, 1, 1.0f);
        pad.preset.soundParams[TrackInParam::FOLLOW_ATK] = 2.0f;
        pad.preset.soundParams[TrackInParam::FOLLOW_REL] = 180.0f;
        blankPattern(pad);
        pad.steps[0] = step({ 48, 55, 60, 64 }, 16.0f, 0.8f);
        m.members.push_back(pad);
        return m;
    }

    // ---- WHAT THE UNIFICATION ADDED ---------------------------------------------------------------

    // Key CMP: an ordinary compressor on the pad, keyed from the kit's track (KEY = a track, not
    // SELF and not the input).
    inline MultiPreset keyCmp()
    {
        namespace U = UnifiedParam;
        MultiPreset m;
        m.name = "Key CMP";
        m.isFactory = true;
        m.playMember = 2;

        auto kit = kitMember("CMP Kit");
        for (int beat = 0; beat < 4; ++beat)
            kit.steps[(size_t) (beat * 4)] = step({ 8 });
        m.members.push_back(kit);

        MultiPreset::Member pad;
        pad.preset.name = "CMP Pad";
        pad.preset.category = "PAD";
        pad.preset.engineType = EngineType::UnifiedVoice;
        pad.preset.soundParams = voice(1.0f, 1.0f, 0.3f, 2500.0f, 0.15f, 0.08f, 1.5f, 0.8f, 2.0f, 0.3f);
        pad.preset.soundParams[U::DETUNE2] = 6.0f;
        PresetRoute::strip(pad.preset.soundParams, EngineType::UnifiedVoice, 0, "CUT", 0.5f);
        // CMP: THR -20 dB, RATI 6, ATK 5 ms, MAKE 6 dB; then KEY = member 1, REL 80 ms, KNEE 3 dB, SCHP 20 Hz.
        insert(pad.preset.soundParams, 0, EffectType::CompressorEffect, { -20.0f, 6.0f, 5.0f, 6.0f });
        insertExtra(pad.preset.soundParams, 0, { 1.0f, 80.0f, 3.0f, 20.0f });
        blankPattern(pad);
        pad.steps[0] = step({ 48, 55, 60, 64 }, 16.0f, 0.8f);
        m.members.push_back(pad);
        return m;
    }

    // Voc Tune: the break speaks through a tuned body. VOC in MODE TUNE: the break's sixteen band
    // levels are the gains of a bank of modes tuned to the note the track plays.
    inline MultiPreset vocTune()
    {
        MultiPreset m;
        m.name = "VocTune";
        m.isFactory = true;
        m.playMember = 2;
        m.members.push_back(breakMember("Tune Src", "B100", 0.8f));

        MultiPreset::Member carrier;
        carrier.preset.name = "TunePad";
        carrier.preset.category = "PAD";
        carrier.preset.engineType = EngineType::UnifiedVoice;
        carrier.preset.soundParams = voice(1.0f, 1.0f, 0.0f, 16000.0f, 0.0f, 0.005f, 0.1f, 1.0f, 1.0f, 0.7f);
        // VOC: MODE TUNE, BAND 0, NOIS 5, all of it; then REL 60 ms, RING 0.6 s, BODY STRG, FRZ off.
        insert(carrier.preset.soundParams, 0, EffectType::VocoderEffect, { 1.0f, 0.0f, 0.05f, 1.0f });
        insertExtra(carrier.preset.soundParams, 0, { 60.0f, 0.6f, 0.0f, 0.0f });
        trackInput(carrier.preset.soundParams, 1, 1.0f);
        PresetRoute::strip(carrier.preset.soundParams, EngineType::UnifiedVoice, 0, "CUT", -0.8f);
        carrier.preset.soundParams[MixParam::VOLUME] = 0.3f;
        blankPattern(carrier);
        carrier.steps[0]  = step({ 48 }, 4.0f, 0.8f);
        carrier.steps[4]  = step({ 55 }, 4.0f, 0.7f);
        carrier.steps[8]  = step({ 60 }, 4.0f, 0.8f);
        carrier.steps[12] = step({ 64 }, 4.0f, 0.7f);
        m.members.push_back(carrier);
        return m;
    }

    // Mod Dly: a track's own router moving the cells of its own insert effects (the eighty effect
    // destinations, "1:C1" .. "8:WT").
    //   member 1  an FM pluck into a tape echo. LFO 1 leans on the echo's TIME (1:C1): the tape
    //             speeds up and slows down, and every repeat comes back a little sharp or flat.
    //             The pad's pressure is on the echo's WET (1:WT, a free slot from PRES): played
    //             softly it is nearly dry, pressed it is thrown into the echo.
    //   member 2  a pad through a filter effect whose cutoff (1:C1) LFO 2 sweeps, once in two bars.
    inline MultiPreset modDelay()
    {
        const auto FM = EngineType::FM, WT = EngineType::Wavetable;
        using W = WavetableEngine;
        MultiPreset m;
        m.name = "Mod Dly";
        m.isFactory = true;
        m.playMember = 1;

        MultiPreset::Member lead;
        lead.preset.name = "Dly Lead";
        lead.preset.category = "LEAD";
        lead.preset.engineType = FM;
        lead.preset.soundParams = {
            { 0, 1.0f }, { 1, 0.9f }, { 4, 0.002f }, { 5, 0.25f }, { 6, 0.0f }, { 7, 0.2f },
            { 10, 2.0f }, { 11, 0.25f }, { 14, 0.001f }, { 15, 0.15f }, { 16, 0.0f }, { 17, 0.1f },
            { 60, 0.0f }, { 61, 0.8f } };
        // TAPE: TIME 250 ms, FDBK 45, AGE 0 (no wow of the tape's own: the LFO is the wow), MIX all.
        insert(lead.preset.soundParams, 0, EffectType::TapeDelayEffect, { 0.25f, 0.45f, 0.0f, 1.0f });
        lead.preset.soundParams[AudioEngine::INSERT_WET_BASE + 0] = 0.15f;
        PresetRoute::lfo(lead.preset.soundParams, FM, 0, "1:C1", 0.01f, 0.2f, "TRI", "FREE");
        PresetRoute::slot(lead.preset.soundParams, FM, 0, "PRES", "1:WT", 0.7f);
        blankPattern(lead);
        lead.steps[0]  = step({ 72 }, 1.0f, 0.9f);
        lead.steps[3]  = step({ 76 }, 1.0f, 0.8f);
        lead.steps[6]  = step({ 79 }, 1.0f, 0.9f);
        lead.steps[10] = step({ 84 }, 1.0f, 0.7f);
        m.members.push_back(lead);

        MultiPreset::Member pad;
        pad.preset.name = "Swp Pad";
        pad.preset.category = "PAD";
        pad.preset.engineType = WT;
        pad.preset.soundParams = {
            { W::TableSelect, FactoryPresets::wavetableByLabel("PRIM") }, { W::Position, 0.6f }, { W::UnisonSpread, 0.4f },
            { W::AmpAttack, 0.3f }, { W::AmpDecay, 1.5f }, { W::AmpSustain, 0.8f }, { W::AmpRelease, 1.5f }, { W::Level, 0.5f } };
        // FLT: FREQ 900 Hz, RESO 45, the other two cells as the effect has them.
        insert(pad.preset.soundParams, 0, EffectType::FilterEffectType, { 900.0f, 0.45f });
        PresetRoute::lfo(pad.preset.soundParams, WT, 1, "1:C1", 0.35f, 0.25f, "SIN", "FREE");
        blankPattern(pad);
        pad.steps[0] = step({ 48, 55, 60 }, 16.0f, 0.7f);
        m.members.push_back(pad);
        return m;
    }

    // Rvb Pump: a hall on the pad and, after it, a compressor keyed from the kit: the tail
    // itself pumps. Both sit in the pad's own insert chain.
    // (It was written as "SndPump" and meant for a send bus. A multi preset cannot carry a send
    // bus's chain: a multi is members, each one track's sound, and loadMulti puts them on empty
    // tracks and touches nothing else, while the sends are the project's, shared by every track.
    // What it would take: a "sends" list in the multi's file, a bus ADDED for each on load (never
    // one replaced: a multi takes nothing away), the KEY cells of its chain and the members' send
    // levels rewritten like the other routings, and the bus taken away again by the load's undo.)
    inline MultiPreset reverbPump()
    {
        namespace U = UnifiedParam;
        MultiPreset m;
        m.name = "Rvb Pump";
        m.isFactory = true;
        m.playMember = 2;

        auto kit = kitMember("Pump Kit");
        for (int beat = 0; beat < 4; ++beat)
            kit.steps[(size_t) (beat * 4)] = step({ 8 });
        m.members.push_back(kit);

        MultiPreset::Member pad;
        pad.preset.name = "Rev Pad";
        pad.preset.category = "PAD";
        pad.preset.engineType = EngineType::UnifiedVoice;
        pad.preset.soundParams = voice(1.0f, 2.0f, 0.4f, 2500.0f, 0.1f, 0.05f, 1.5f, 0.7f, 2.0f, 0.4f);
        pad.preset.soundParams[U::DETUNE2] = 8.0f;
        PresetRoute::strip(pad.preset.soundParams, EngineType::UnifiedVoice, 0, "CUT", 0.5f);
        insert(pad.preset.soundParams, 0, EffectType::HallReverbEffect, { 0.7f, 0.6f, 0.4f, 0.7f });
        // CMP: THR -24 dB, RATI 8, ATK 2 ms, MAKE 4 dB; KEY = member 1, REL 60 ms, KNEE 4 dB, SCHP 20 Hz.
        insert(pad.preset.soundParams, 1, EffectType::CompressorEffect, { -24.0f, 8.0f, 2.0f, 4.0f });
        insertExtra(pad.preset.soundParams, 1, { 1.0f, 60.0f, 4.0f, 20.0f });
        blankPattern(pad);
        pad.steps[0] = step({ 48, 55, 60, 64 }, 16.0f, 0.8f);
        m.members.push_back(pad);
        return m;
    }

    // Band Duck: another track's SPECTRUM as control signals (the sources BND1 .. BN16: the
    // sixteen band levels of the track input, 100 Hz to 8 kHz). The pad ducks under the kit's
    // three lowest bands alone - the kick takes it down, the clap does not - and the bands a clap
    // fills open its filter: the clap is heard as a flash of brightness in the pad, the kick is not.
    inline MultiPreset bandDuck()
    {
        const auto T = EngineType::Wavetable;
        using W = WavetableEngine;
        MultiPreset m;
        m.name = "BandDuck";
        m.isFactory = true;
        m.playMember = 2;

        auto kit = kitMember("Band Kit");
        for (int beat = 0; beat < 4; ++beat)
            kit.steps[(size_t) (beat * 4)] = step({ 8 });
        kit.steps[4] = step({ 13 });
        kit.steps[12] = step({ 13 });
        m.members.push_back(kit);

        MultiPreset::Member pad;
        pad.preset.name = "Band Pad";
        pad.preset.category = "PAD";
        pad.preset.engineType = T;
        pad.preset.soundParams = {
            { W::TableSelect, FactoryPresets::wavetableByLabel("RESO") }, { W::Position, 0.35f }, { W::UnisonSpread, 0.4f },
            { W::AmpAttack, 0.2f }, { W::AmpDecay, 1.5f }, { W::AmpSustain, 0.9f }, { W::AmpRelease, 1.5f }, { W::Level, 0.4f },
            { W::FilterBase + VoiceFilterSection::Cutoff, 900.0f }, { W::FilterBase + VoiceFilterSection::Resonance, 0.2f } };
        PresetRoute::slot(pad.preset.soundParams, T, 0, "BND1", "AMP", -1.0f);
        PresetRoute::slot(pad.preset.soundParams, T, 1, "BND2", "AMP", -1.0f);
        PresetRoute::slot(pad.preset.soundParams, T, 2, "BND3", "AMP", -1.0f);
        PresetRoute::slot(pad.preset.soundParams, T, 3, "BN10", "CUT", 1.0f);
        PresetRoute::slot(pad.preset.soundParams, T, 4, "BN11", "CUT", 1.0f);
        PresetRoute::slot(pad.preset.soundParams, T, 5, "BN12", "CUT", 1.0f);
        trackInput(pad.preset.soundParams, 1, 1.0f);
        blankPattern(pad);
        pad.steps[0] = step({ 48, 55, 60, 64 }, 16.0f, 0.8f);
        m.members.push_back(pad);
        return m;
    }

    // Hit Sweep: another track's HITS as triggers. The pad's LFO 1 is a one-shot that waits for
    // a sound to begin in the track input (TRIG XONE): every hit of the kit throws the filter open
    // and it falls shut again in half a second, and the source ONST nudges the table's position
    // with it. Between hits nothing moves.
    inline MultiPreset hitSweep()
    {
        const auto T = EngineType::Wavetable;
        using W = WavetableEngine;
        MultiPreset m;
        m.name = "HitSweep";
        m.isFactory = true;
        m.playMember = 2;

        auto kit = kitMember("Hit Kit");
        kit.steps[0]  = step({ 8 });
        kit.steps[6]  = step({ 12 });
        kit.steps[8]  = step({ 8 });
        kit.steps[12] = step({ 12 });
        m.members.push_back(kit);

        MultiPreset::Member pad;
        pad.preset.name = "Hit Pad";
        pad.preset.category = "PAD";
        pad.preset.engineType = T;
        pad.preset.soundParams = {
            { W::TableSelect, FactoryPresets::wavetableByLabel("SYNC") }, { W::Position, 0.2f }, { W::UnisonSpread, 0.3f },
            { W::AmpAttack, 0.1f }, { W::AmpDecay, 1.5f }, { W::AmpSustain, 0.9f }, { W::AmpRelease, 1.5f }, { W::Level, 0.4f },
            { W::FilterBase + VoiceFilterSection::Cutoff, 500.0f }, { W::FilterBase + VoiceFilterSection::Resonance, 0.3f } };
        PresetRoute::lfo(pad.preset.soundParams, T, 0, "CUT", 0.8f, 2.0f, "EXPD", "XONE");
        PresetRoute::slot(pad.preset.soundParams, T, 0, "ONST", "POS", 0.3f);
        trackInput(pad.preset.soundParams, 1, 1.0f);
        blankPattern(pad);
        pad.steps[0] = step({ 48, 55, 60 }, 16.0f, 0.8f);
        m.members.push_back(pad);
        return m;
    }

    inline std::vector<MultiPreset> builtIn()
    {
        return { duckPair(), drumHarp(), voicePlate(), robotBreak(), ringChoir(),
                 sampleFM(), pumpPad(), keyCmp(), vocTune(), modDelay(), reverbPump(),
                 syncSaw(), howlLoop(), bandDuck(), hitSweep() };
    }

    inline std::vector<MultiPreset>& registry()
    {
        static std::vector<MultiPreset> list = builtIn();
        return list;
    }

    // Adds a factory multi (one with the same name is replaced). Call before the PresetManager is made.
    inline void add(MultiPreset multi)
    {
        multi.isFactory = true;
        multi.category = MultiPreset::categoryName();
        for (auto& have : registry())
            if (have.name == multi.name)
            {
                have = std::move(multi);
                return;
            }
        registry().push_back(std::move(multi));
    }

    inline const std::vector<MultiPreset>& getAll() { return registry(); }
}
