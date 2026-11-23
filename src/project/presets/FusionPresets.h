#pragma once

#include "../Preset.h"
#include "../FactoryPresets.h"
#include "../CymbalPresets.h"
#include "PresetRoutes.h"
#include "../../engine/FMEngine.h"
#include "../../engine/MacroOscEngine.h"
#include "../../engine/WavetableEngine.h"
#include "../../engine/WaveSeqEngine.h"
#include "../../engine/SamplerEngine.h"
#include "../../engine/UnifiedVoiceEngine.h"
#include "../../engine/ResonatorEngine.h"
#include "../../engine/DrumEngine.h"
#include "../../engine/VoiceBodySection.h"
#include "../../engine/VoiceSpread.h"
#include "../../engine/ModRouter.h"
#include "../../dsp/PhaseOsc.h"
#include <vector>

// -----------------------------------------------------------------------------
// The fusion presets: single-track sounds that SHOW what the unification built
// (.ralph/spec-unify.md: one resonator as a stage of every voice, one router
// with audio-rate and engine-own sources, one phase-modulatable oscillator, the
// shared filter section, the seeded spread, VOX, the reverb network, the
// through-zero dry line, per-pad modulation in the kit) and the six
// single-track demonstration presets of .ralph/spec-track-input.md.
//
// Every preset whose point is a modulation ROUTES it: a slot, an LFO's DEST, a
// pressure slot, a strip route, by the NAME the display shows (PresetRoutes.h).
// What each name promises is measured in tests/fusion_presets_tests.h
// ("Fusion Presets"); that suite also fails on a name that does not resolve.
//
// Names: at most eight characters. A preset with insert effects stores them
// with its sound (ids 200-343: a preset is the whole of a track's sound).
//
// The mod strip: every preset answers it. The engines other than the unified
// voice have a default strip route each (FM: the modulation index; macro: the
// fourth macro; wavetable: the position; wave sequencer: the rate; resonator:
// the energy; drums: decay and drive); a preset here sets a STRIP cell where
// that default has nothing to work on.
// -----------------------------------------------------------------------------
namespace FusionPresets
{
    using namespace FactoryPresets;
    namespace R = PresetRoute;
    using VF = VoiceFilterSection;

    inline Preset preset(const char* name, const char* category, EngineType type, ParamMap params)
    {
        Preset p;
        p.name = name;
        p.category = category;
        p.engineType = type;
        p.isFactory = true;
        p.soundParams = std::move(params);
        return p;
    }

    // ---- The single-track demonstration presets of spec-track-input.md ---------------------------

    // BOW CELLO: a bow on a string (RSN: BOW on STRG). The pad's pressure is the bow's energy
    // (pressure slot 1 on EXC) and leans on the string's damping (slot 2 on DAMP: pressed, it is
    // brighter): it swells and opens under each finger. The strip is vibrato.
    inline Preset bowCello()
    {
        using E = ResonatorEngine;
        const auto T = EngineType::Resonator;
        auto p = preset("BowCello", "LEAD", T, {
            { E::MorphParam, 0.0f }, { E::ExciterMorphParam, 1.0f },
            { E::DecayParam, 4.0f }, { E::EnergyParam, 0.35f }, { E::VelSenseParam, 0.5f }, { E::LagParam, 0.04f },
            { E::LevelParam, 0.75f },
            { E::AmpAttack, 0.08f }, { E::AmpDecay, 1.0f }, { E::AmpSustain, 1.0f }, { E::AmpRelease, 0.8f },
            { E::ExciteBase + res::Couple, 0.4f }, { E::ExciteBase + res::Hard, -0.1f },
            { E::BodyBase + res::Damp, 0.15f } });
        R::pressure(p.soundParams, T, 0, "EXC", 1.0f);
        R::pressure(p.soundParams, T, 1, "DAMP", -0.3f);
        R::strip(p.soundParams, T, 0, "PTCH", 0.12f);
        R::strip(p.soundParams, T, 1, "ENRG", 0.4f);
        return p;
    }

    // BOW GLASS: a bow on a bell (RSN: BOW on BELL), a singing bowl. Pressure is the bow, and it
    // moves the body itself: pressed down, the bell is a point further along MRPH (a head's
    // partials come in under it).
    inline Preset bowGlass()
    {
        using E = ResonatorEngine;
        const auto T = EngineType::Resonator;
        auto p = preset("BowGlass", "PAD", T, {
            { E::MorphParam, 5.0f }, { E::ExciterMorphParam, 1.0f },
            { E::DecayParam, 8.0f }, { E::EnergyParam, 0.5f }, { E::VelSenseParam, 0.4f }, { E::LagParam, 0.06f },
            { E::LevelParam, 1.0f },
            { E::AmpAttack, 0.15f }, { E::AmpDecay, 2.0f }, { E::AmpSustain, 1.0f }, { E::AmpRelease, 3.0f },
            { E::ExciteBase + res::Couple, 0.35f },
            { E::MoreBase + res::Meet, 0.2f }, { E::MoreBase + res::Spread, 0.15f } });
        R::pressure(p.soundParams, T, 0, "EXC", 1.0f);
        R::pressure(p.soundParams, T, 1, "MRPH", -1.0f / (float) (E::NUM_BODIES - 1));     // (one point: BELL towards BAR)
        // (A bowed bell is soft: 12 dB of make-up after it. CMP at THR 0, RATI 1 is a gain.)
        R::insert(p.soundParams, 0, EffectType::CompressorEffect, { 0.0f, 1.0f, 5.0f, 12.0f });
        R::strip(p.soundParams, T, 0, "PTCH", 0.08f);
        R::strip(p.soundParams, T, 1, "ENRG", 0.4f);
        return p;
    }

    // BREATH PIPE: breath into a tube (RSN: BRTH on TUBE). Pressure is the breath, and it moves
    // the exciter itself: pressed hard, XMPH goes two points on, from breath through a scrape to a
    // brush - the pipe is overblown and turns to noise.
    inline Preset breathPipe()
    {
        using E = ResonatorEngine;
        const auto T = EngineType::Resonator;
        auto p = preset("BrthPipe", "LEAD", T, {
            { E::MorphParam, 1.0f }, { E::ExciterMorphParam, 2.0f },
            { E::DecayParam, 1.5f }, { E::EnergyParam, 0.3f }, { E::VelSenseParam, 0.6f }, { E::LagParam, 0.02f },
            { E::LevelParam, 0.8f },
            { E::AmpAttack, 0.04f }, { E::AmpDecay, 0.5f }, { E::AmpSustain, 1.0f }, { E::AmpRelease, 0.3f },
            { E::ExciteBase + res::Couple, 0.35f }, { E::ExciteBase + res::Shape, 0.6f },
            { E::MoreBase + res::Direct, 0.3f } });
        R::pressure(p.soundParams, T, 0, "EXC", 1.0f);
        R::pressure(p.soundParams, T, 1, "XMPH", 2.0f / (float) (E::NUM_EXCITERS - 1));
        R::strip(p.soundParams, T, 0, "ENRG", 0.5f);
        return p;
    }

    // SCRAPE BAR: a scrape along a bar (RSN: SCRP on BAR), dry guiro to shrieking metal: pressure
    // is how hard it is scraped (EXC) and how hard the thing is that scrapes (HARD).
    inline Preset scrapeBar()
    {
        using E = ResonatorEngine;
        const auto T = EngineType::Resonator;
        auto p = preset("ScrapBar", "FX", T, {
            { E::MorphParam, 4.0f }, { E::ExciterMorphParam, 3.0f },
            { E::DecayParam, 1.0f }, { E::EnergyParam, 0.5f }, { E::VelSenseParam, 0.7f }, { E::LagParam, 0.01f },
            { E::LevelParam, 0.75f },
            { E::AmpAttack, 0.01f }, { E::AmpDecay, 0.3f }, { E::AmpSustain, 0.8f }, { E::AmpRelease, 0.3f },
            { E::ExciteBase + res::Grain, 0.4f }, { E::ExciteBase + res::Hard, 0.0f },
            { E::MoreBase + res::Direct, 0.4f } });
        R::pressure(p.soundParams, T, 0, "EXC", 1.0f);
        R::pressure(p.soundParams, T, 1, "HARD", 0.6f);
        R::strip(p.soundParams, T, 0, "ENRG", 0.5f);
        return p;
    }

    // DEEP MOD: four slots on the unified voice. LFO 1 moves the pulse width only as far as the
    // pad is pressed (VIA PRES), and pressure itself goes to three places: the cutoff, the
    // cross-modulation and the resonance. A pad that opens and growls under each finger, and
    // lies still without one. (Pressure on the level is the track's first AT ROUTE as always.)
    inline Preset deepMod()
    {
        namespace U = UnifiedParam;
        const auto T = EngineType::UnifiedVoice;
        auto p = preset("Deep Mod", "PAD", T, {
            { U::WAVE1, 2.0f }, { U::WAVE2, 1.0f }, { U::MIX, 0.5f }, { U::DETUNE2, 7.0f }, { U::PULSE_WIDTH, 0.5f },
            { U::CUTOFF, 900.0f }, { U::RESONANCE, 0.25f }, { U::FILTER_ENV, 0.2f },
            { U::AMP_ATTACK, 0.3f }, { U::AMP_DECAY, 2.0f }, { U::AMP_SUSTAIN, 0.8f }, { U::AMP_RELEASE, 2.0f }, { U::LEVEL, 0.6f },
            { U::FENV_ATTACK, 0.01f }, { U::FENV_DECAY, 1.5f }, { U::FENV_SUSTAIN, 0.3f }, { U::FENV_RELEASE, 1.5f },
            // LFO 1: three seconds a cycle, aimed nowhere by itself (the slot reads it).
            { ModParam::lfoParamId(0, ModParam::LFO_RATE), 0.33f }, { ModParam::lfoParamId(0, ModParam::LFO_DEPTH), 0.0f },
            { ModParam::LFO_COUNT, 1.0f } });
        R::slot(p.soundParams, T, 0, "LFO1", "PW", 0.9f, "PRES");
        R::slot(p.soundParams, T, 1, "PRES", "CUT", 0.5f);
        R::slot(p.soundParams, T, 2, "PRES", "XMOD", 0.4f);
        R::slot(p.soundParams, T, 3, "PRES", "RESO", 0.6f);
        R::strip(p.soundParams, T, 0, "CUT", 0.4f);
        return p;
    }

    // STEP SHAPE: one held note plays a riff. The custom LFO shape (LSHP: eight levels) is run
    // once a bar by LFO 1 on the pitch, at full depth - a level is then twelfths of an octave, so
    // the eight levels are eight notes: 0 7 12 7 3 10 0 -5 semitones - and by LFO 2 on the cutoff.
    inline Preset stepShape()
    {
        using W = WavetableEngine;
        const auto T = EngineType::Wavetable;
        auto p = preset("StepShpe", "LEAD", T, {
            { W::TableSelect, wavetableByLabel("SAW") }, { W::Position, 0.7f },
            { W::AmpAttack, 0.003f }, { W::AmpDecay, 0.3f }, { W::AmpSustain, 0.9f }, { W::AmpRelease, 0.15f },
            { W::Level, 0.7f },
            { W::FilterBase + VF::Cutoff, 1200.0f }, { W::FilterBase + VF::Resonance, 0.35f } });
        static constexpr float semis[LfoShapeParam::POINTS] = { 0.0f, 7.0f, 12.0f, 7.0f, 3.0f, 10.0f, 0.0f, -5.0f };
        for (int i = 0; i < LfoShapeParam::POINTS; ++i)
            p.soundParams[LfoShapeParam::BASE + i] = semis[i] / 12.0f;
        R::lfo(p.soundParams, T, 0, "PTCH", 1.0f, 0.5f, "USR", "KEY");
        R::lfoSync(p.soundParams, 0, "1BAR");
        R::lfo(p.soundParams, T, 1, "CUT", 0.35f, 0.5f, "USR", "KEY");
        R::lfoSync(p.soundParams, 1, "1BAR");
        return p;
    }

    // ---- BODY: the resonator as a stage of every voice (fusion 1) --------------------------------

    // PLUCK BODY: a short click of the macro oscillator through a string (BODY: STRG). The two
    // cells that make it a plucked string: RDAM (the string is damped when the key is let go)
    // and KDCY (high notes die sooner, as on a real one). The strip takes the string's damping
    // away: brighter, and longer in its top.
    inline Preset pluckBody()
    {
        using M = MacroOscEngine;
        auto p = preset("PluckBdy", "KEYS", EngineType::MacroOsc, {
            { M::ModelParam, 0.0f }, { M::TimbreParam, 0.5f }, { M::ColorParam, 0.4f },
            { M::AmpAttack, 0.001f }, { M::AmpDecay, 0.03f }, { M::AmpSustain, 0.0f }, { M::AmpRelease, 0.03f },
            { M::LevelParam, 0.75f },
            { M::FilterBase + VF::Cutoff, 4000.0f },
            { M::BodyBase + VoiceBody::Mix, 1.0f }, { M::BodyBase + VoiceBody::Morph, 0.0f },
            { M::BodyBase + VoiceBody::Decay, 3.0f }, { M::BodyBase + VoiceBody::KeyTrack, 1.0f },
            { M::BodyBase + VoiceBody::ReleaseDamp, 0.9f }, { M::BodyBase + VoiceBody::KeyDecay, 0.6f },
            { M::BodyBase + VoiceBody::Damp, 0.3f } });
        R::strip(p.soundParams, EngineType::MacroOsc, 0, "BDMP", -0.6f);
        return p;
    }

    // DUST BODY: dust through a plate. The body makes its own grains (BDY2: GRAN) and they are
    // heard beside the plate they strike (DIRE); the wave sequencer under it is only a breath.
    // The strip thins the dust out (a STRIP route on BGRN).
    inline Preset dustBody()
    {
        using W = WaveSeqEngine;
        const auto T = EngineType::WaveSeq;
        auto p = preset("DustBody", "FX", T, {
            { W::SeqLength, 1.0f }, { W::Crossfade, 0.5f }, { W::Level, 0.12f },
            { W::AmpAttack, 0.3f }, { W::AmpDecay, 2.0f }, { W::AmpSustain, 0.6f }, { W::AmpRelease, 2.0f },
            { W::FilterBase + VF::Cutoff, 3000.0f },
            { W::BodyBase + VoiceBody::Mix, 0.9f }, { W::BodyBase + VoiceBody::Morph, 3.0f },
            { W::BodyBase + VoiceBody::Decay, 3.0f }, { W::BodyBase + VoiceBody::KeyTrack, 1.0f },
            { W::BodyBase + VoiceBody::Grain, 0.35f }, { W::BodyBase + VoiceBody::Hard, 0.3f },
            { W::BodyBase + VoiceBody::Direct, 0.5f }, { W::BodyBase + VoiceBody::Couple, 0.3f } });
        R::strip(p.soundParams, T, 0, "BGRN", -0.75f);
        return p;
    }

    // REED BODY: the voice blows a pipe. The macro oscillator's level grips a tube (BODY: TUBE,
    // COUP) through a reed's curve (BDY2: REED) instead of a bow's; LFO 1 lets the tube wander two
    // points along MRPH and back, slowly.
    inline Preset reedBody()
    {
        using M = MacroOscEngine;
        const auto T = EngineType::MacroOsc;
        auto p = preset("ReedBody", "LEAD", T, {
            { M::ModelParam, 0.0f }, { M::TimbreParam, 0.6f }, { M::ColorParam, 0.3f },
            { M::AmpAttack, 0.05f }, { M::AmpDecay, 0.5f }, { M::AmpSustain, 1.0f }, { M::AmpRelease, 0.4f },
            { M::LevelParam, 0.3f },
            { M::FilterBase + VF::Cutoff, 5000.0f },
            { M::BodyBase + VoiceBody::Mix, 0.8f }, { M::BodyBase + VoiceBody::Morph, 1.0f },
            { M::BodyBase + VoiceBody::Decay, 2.0f }, { M::BodyBase + VoiceBody::Couple, 0.75f },
            { M::BodyBase + VoiceBody::Reed, 0.8f }, { M::BodyBase + VoiceBody::KeyTrack, 1.0f },
            { M::BodyBase + VoiceBody::Direct, 0.2f } });
        R::lfo(p.soundParams, T, 0, "BMRP", 2.0f / (float) (VoiceBodySection::NUM_BODIES - 1), 0.15f, "TRI", "FREE");
        return p;
    }

    // MORPH BODY: the body changes within the note. A click of the oscillator strikes it, and the
    // filter envelope (it moves no cutoff here: ENV 0) carries BODY's MRPH from a string to a bell
    // in a second: struck as a string, the partials leave the harmonics and settle on a bell's.
    inline Preset morphBody()
    {
        using M = MacroOscEngine;
        const auto T = EngineType::MacroOsc;
        auto p = preset("MorphBdy", "PAD", T, {
            { M::ModelParam, 0.0f }, { M::TimbreParam, 0.5f }, { M::ColorParam, 0.4f },
            { M::AmpAttack, 0.001f }, { M::AmpDecay, 0.05f }, { M::AmpSustain, 0.0f }, { M::AmpRelease, 0.05f },
            { M::LevelParam, 0.75f },
            { M::FilterBase + VF::Cutoff, 6000.0f }, { M::FilterBase + VF::EnvAmount, 0.0f },
            { M::FilterBase + VF::Attack, 0.001f }, { M::FilterBase + VF::Decay, 1.0f }, { M::FilterBase + VF::Sustain, 0.0f },
            { M::FilterBase + VF::Release, 1.0f },
            { M::BodyBase + VoiceBody::Mix, 1.0f }, { M::BodyBase + VoiceBody::Morph, 5.0f },       // where it ends: BELL
            { M::BodyBase + VoiceBody::Decay, 6.0f }, { M::BodyBase + VoiceBody::KeyTrack, 1.0f } });
        R::slot(p.soundParams, T, 0, "FENV", "BMRP", -5.0f / (float) (VoiceBodySection::NUM_BODIES - 1));   // (from STRG while the envelope is up)
        // The strip carries the morph on, two points past the bell: to a gong.
        R::strip(p.soundParams, T, 0, "BMRP", 2.0f / (float) (VoiceBodySection::NUM_BODIES - 1));
        return p;
    }

    // ---- The router's audio-rate and engine-own sources (fusion 2) --------------------------------

    // FM FILTER: an operator on the filter. Operator 6 takes no part in the sound (in the chain
    // 6>5>4>3>2>1 the operators 3 to 5 are at level 0: nothing of it reaches a carrier): its
    // output, at audio rate, is on the cutoff (a slot: OP6 > CUT). The filter is
    // frequency-modulated at five times the note, and the side bands that makes die away with the
    // operator's own envelope.
    inline Preset fmOnFilter()
    {
        using F = FMEngine::Param;
        const auto T = EngineType::FM;
        auto p = preset("FMFilter", "LEAD", T, {
            { F::AlgorithmParam, 0.0f },
            { F::Op1Ratio, 1.0f }, { F::Op1Level, 0.9f },
            { F::Op1Attack, 0.005f }, { F::Op1Decay, 0.6f }, { F::Op1Sustain, 0.7f }, { F::Op1Release, 0.3f },
            { F::Op2Ratio, 1.0f }, { F::Op2Level, 0.3f },
            { F::Op2Attack, 0.005f }, { F::Op2Decay, 0.4f }, { F::Op2Sustain, 0.4f }, { F::Op2Release, 0.2f },
            { 2 * 10 + 1, 0.0f }, { 3 * 10 + 1, 0.0f }, { 4 * 10 + 1, 0.0f },       // operators 3, 4, 5: level 0
            { 5 * 10 + 0, 5.0f }, { 5 * 10 + 1, 1.0f },                             // operator 6: ratio 5, level 100
            { 5 * 10 + 4, 0.001f }, { 5 * 10 + 5, 1.2f }, { 5 * 10 + 6, 0.2f }, { 5 * 10 + 7, 0.5f },
            { F::MasterLevel, 0.7f },
            { F::FilterBase + VF::Cutoff, 1200.0f }, { F::FilterBase + VF::Resonance, 0.5f } });
        R::slot(p.soundParams, T, 0, "OP6", "CUT", 0.7f);
        return p;
    }

    // SEQ MORPH: the wave sequencer's lanes as a source. The pitch lane (PLAN: the step the
    // sequence stands at) moves BODY's MRPH: each step of the sequence rings in another body,
    // and the level lane (LLAN) opens the filter with it.
    inline Preset seqMorph()
    {
        using W = WaveSeqEngine;
        const auto T = EngineType::WaveSeq;
        auto p = preset("SeqMorph", "KEYS", T, {
            { W::SeqLength, 4.0f }, { W::Crossfade, 0.3f }, { W::Level, 0.7f }, { W::PitchRange, 12.0f },
            { W::StepPitch + 0, 0.0f }, { W::StepPitch + 1, 7.0f }, { W::StepPitch + 2, 12.0f }, { W::StepPitch + 3, -5.0f },
            { W::StepLevel + 0, 1.0f }, { W::StepLevel + 1, 0.5f }, { W::StepLevel + 2, 0.8f }, { W::StepLevel + 3, 0.4f },
            { W::AmpAttack, 0.01f }, { W::AmpDecay, 0.5f }, { W::AmpSustain, 0.8f }, { W::AmpRelease, 0.5f },
            { W::FilterBase + VF::Cutoff, 2000.0f },
            { W::BodyBase + VoiceBody::Mix, 0.7f }, { W::BodyBase + VoiceBody::Morph, 2.0f },
            { W::BodyBase + VoiceBody::Decay, 1.5f }, { W::BodyBase + VoiceBody::KeyTrack, 1.0f } });
        R::slot(p.soundParams, T, 0, "PLAN", "BMRP", 4.0f / (float) (VoiceBodySection::NUM_BODIES - 1));    // (a fifth up = two points on)
        R::slot(p.soundParams, T, 1, "LLAN", "CUT", 0.3f);
        return p;
    }

    // RND TIME: no two notes alike, and the same again every time. Three more randoms a note
    // (RND2 RND3 RND4, beside RAND) on the envelope's time (ETIM: some notes are plucks, some
    // swells), the filter envelope's time (FTIM) and the table position; RAND on the pan. SEED
    // makes the sequence of notes repeat.
    inline Preset rndEnvTime()
    {
        using W = WavetableEngine;
        const auto T = EngineType::Wavetable;
        auto p = preset("Rnd Time", "PAD", T, {
            { W::TableSelect, wavetableByLabel("FOLD") }, { W::Position, 0.3f }, { W::UnisonSpread, 0.4f },
            { W::AmpAttack, 0.05f }, { W::AmpDecay, 0.6f }, { W::AmpSustain, 0.2f }, { W::AmpRelease, 0.8f },
            { W::Level, 0.65f },
            { W::FilterBase + VF::Cutoff, 1500.0f }, { W::FilterBase + VF::EnvAmount, 0.4f }, { W::FilterBase + VF::Decay, 0.6f },
            { W::SpreadBase + VoiceSpread::Seed, 42.0f } });
        R::slot(p.soundParams, T, 0, "RND2", "ETIM", 0.8f);
        R::slot(p.soundParams, T, 1, "RND3", "FTIM", 0.8f);
        R::slot(p.soundParams, T, 2, "RND4", "POS", 0.3f);
        R::slot(p.soundParams, T, 3, "RAND", "PAN", 0.6f);
        return p;
    }

    // ---- One oscillator for every engine (fusion 6): WAVE and POS on an FM operator --------------

    // FM WAVE OP: operator 1 reads the BELL table instead of a sine, and the envelope moves
    // its position (PENV); operator 2 modulates it.
    inline Preset fmWaveOps()
    {
        using F = FMEngine::Param;
        const float bellWave = R::inList(phaseosc::waveLabels(), phaseosc::waveCount(), "WAVE", "BELL");
        return preset("FMWaveOp", "KEYS", EngineType::FM, {
            { F::AlgorithmParam, 0.0f },
            { F::Op1Ratio, 1.0f }, { F::Op1Level, 0.85f },
            { F::Op1Attack, 0.002f }, { F::Op1Decay, 1.5f }, { F::Op1Sustain, 0.15f }, { F::Op1Release, 1.0f },
            { 0 * 10 + FMEngine::OP_WAVE, bellWave }, { 0 * 10 + FMEngine::OP_POSITION, 0.3f },
            { F::Op2Ratio, 3.5f }, { F::Op2Level, 0.5f },
            { F::Op2Attack, 0.001f }, { F::Op2Decay, 0.8f }, { F::Op2Sustain, 0.0f }, { F::Op2Release, 0.5f },
            { F::PositionEnv, 0.4f }, { F::MasterLevel, 0.65f },
            { F::FilterBase + VF::Cutoff, 6000.0f }, { F::FilterBase + VF::Resonance, 0.2f },
            { F::FilterBase + VF::EnvAmount, 0.2f }, { F::FilterBase + VF::Decay, 1.0f } });
    }

    // FM CYCLE: operator 1 reads the cycles of a sample (WAVE CYCL), modulated by a sine an
    // octave and a fifth up: a hollow, reedy organ no sine carrier gives.
    inline Preset fmCycleOp()
    {
        using F = FMEngine::Param;
        return preset("FM Cycle", "KEYS", EngineType::FM, {
            { F::AlgorithmParam, 0.0f },
            { F::Op1Ratio, 1.0f }, { F::Op1Level, 0.8f },
            { F::Op1Attack, 0.02f }, { F::Op1Decay, 1.0f }, { F::Op1Sustain, 0.8f }, { F::Op1Release, 0.4f },
            { 0 * 10 + FMEngine::OP_WAVE, (float) phaseosc::WAVE_CYCLES }, { 0 * 10 + FMEngine::OP_POSITION, 0.6f },
            { F::Op2Ratio, 3.0f }, { F::Op2Level, 0.35f },
            { F::Op2Attack, 0.02f }, { F::Op2Decay, 1.5f }, { F::Op2Sustain, 0.6f }, { F::Op2Release, 0.4f },
            { F::MasterLevel, 0.65f } });
    }

    // ---- The shared filter section: filter B, ROUT, BAL, FFM --------------------------------------

    // DUAL FILT: two filters side by side (ROUT PAR): a low-pass and, two octaves and a half above
    // it, a resonant band-pass - a second voice in the top of the same sound; the oscillator itself
    // on the cutoff (FFM).
    inline Preset dualFilter()
    {
        using W = WavetableEngine;
        const int F = W::FilterBase, D = W::FilterDualBase - (int) VF::Count;
        return preset("DualFilt", "PAD", EngineType::Wavetable, {
            { W::TableSelect, wavetableByLabel("SAW") }, { W::Position, 0.8f }, { W::UnisonSpread, 0.5f },
            { W::AmpAttack, 0.5f }, { W::AmpDecay, 2.0f }, { W::AmpSustain, 0.75f }, { W::AmpRelease, 2.5f },
            { W::Level, 0.6f },
            { F + VF::Cutoff, 500.0f }, { F + VF::Resonance, 0.3f }, { F + VF::EnvAmount, 0.2f }, { F + VF::Decay, 1.2f },
            { D + VF::TypeB, (float) FilterSlot::indexFromModel(FilterModel::SVF) }, { D + VF::ModeB, 1.0f },      // BP
            { D + VF::OffsetB, 30.0f }, { D + VF::ResonanceB, 0.7f },
            { D + VF::Routing, (float) (int) FilterRouting::Parallel }, { D + VF::Balance, 0.5f }, { D + VF::FilterFM, 0.15f } });
    }

    // SERL FILT: two filters one after the other (ROUT SER): a ladder low-pass into a diode
    // low-pass an octave below it, both resonant: two peaks that sweep together under the envelope.
    inline Preset serialFilter()
    {
        using W = WavetableEngine;
        const int F = W::FilterBase, D = W::FilterDualBase - (int) VF::Count;
        return preset("SerlFilt", "BASS", EngineType::Wavetable, {
            { W::TableSelect, wavetableByLabel("SAW") }, { W::Position, 0.8f }, { W::SubLevel, 0.2f },
            { W::AmpAttack, 0.003f }, { W::AmpDecay, 0.4f }, { W::AmpSustain, 0.6f }, { W::AmpRelease, 0.15f },
            { W::Level, 0.8f },
            { F + VF::Type, (float) FilterSlot::indexFromModel(FilterModel::Ladder) },
            { F + VF::Cutoff, 2500.0f }, { F + VF::Resonance, 0.5f }, { F + VF::EnvAmount, 0.3f }, { F + VF::Decay, 0.35f },
            { F + VF::Drive, 0.3f },
            { D + VF::TypeB, (float) FilterSlot::indexFromModel(FilterModel::Diode) }, { D + VF::ModeB, 0.0f },
            { D + VF::OffsetB, -12.0f }, { D + VF::ResonanceB, 0.5f },
            { D + VF::Routing, (float) (int) FilterRouting::Series } });
    }

    // ---- The seeded per-note spread (ANLG, SEED) --------------------------------------------------

    // SEEDED: every note a little off in pitch, cutoff, envelope time and timbre (the ANLG page),
    // and SEED makes the same notes come out the same again.
    inline Preset seededSpread()
    {
        using M = MacroOscEngine;
        return preset("Seeded", "KEYS", EngineType::MacroOsc, {
            { M::ModelParam, 0.0f }, { M::TimbreParam, 0.4f }, { M::ColorParam, 0.5f },
            { M::AmpAttack, 0.01f }, { M::AmpDecay, 0.8f }, { M::AmpSustain, 0.3f }, { M::AmpRelease, 0.5f },
            { M::LevelParam, 0.75f },
            { M::FilterBase + VF::Cutoff, 3500.0f }, { M::FilterBase + VF::EnvAmount, 0.3f }, { M::FilterBase + VF::Decay, 0.5f },
            { M::SpreadBase + VoiceSpread::Slop, 15.0f }, { M::SpreadBase + VoiceSpread::Cutoff, 0.4f },
            { M::SpreadBase + VoiceSpread::EnvTime, 0.3f }, { M::SpreadBase + VoiceSpread::Wave, 0.4f },
            { M::SpreadBase + VoiceSpread::Seed, 7.0f } });
    }

    inline Preset seededWavetable()
    {
        using W = WavetableEngine;
        return preset("SeedTabl", "PAD", EngineType::Wavetable, {
            { W::TableSelect, wavetableByLabel("PRIM") }, { W::Position, 0.15f }, { W::UnisonSpread, 0.5f },
            { W::AmpAttack, 0.6f }, { W::AmpDecay, 2.0f }, { W::AmpSustain, 0.7f }, { W::AmpRelease, 3.0f },
            { W::Level, 0.6f },
            { W::SpreadBase + VoiceSpread::Slop, 8.0f }, { W::SpreadBase + VoiceSpread::Cutoff, 0.3f },
            { W::SpreadBase + VoiceSpread::EnvTime, 0.25f }, { W::SpreadBase + VoiceSpread::Wave, 0.3f },
            { W::SpreadBase + VoiceSpread::Seed, 23.0f } });
    }

    // ---- VOX: how many notes sound at once --------------------------------------------------------

    // MONO WT: VOX 1. One voice: a new note takes it, with a glide.
    inline Preset monoLead()
    {
        using W = WavetableEngine;
        return preset("Mono Wt", "LEAD", EngineType::Wavetable, {
            { W::TableSelect, wavetableByLabel("SYNC") }, { W::Position, 0.15f }, { W::EnvToPosition, 0.4f },
            { W::Glide, 0.04f },
            { W::AmpAttack, 0.003f }, { W::AmpDecay, 0.5f }, { W::AmpSustain, 0.6f }, { W::AmpRelease, 0.2f },
            { W::Level, 0.8f }, { W::VoiceCountParam, 1.0f },
            { W::FilterBase + VF::Cutoff, 3000.0f }, { W::FilterBase + VF::Resonance, 0.4f },
            { W::FilterBase + VF::EnvAmount, 0.4f }, { W::FilterBase + VF::Decay, 0.4f } });
    }

    // WIDE PAD: VOX 16. Sixteen notes ring at once: a chord with both hands and its tails.
    inline Preset widePad()
    {
        using W = WavetableEngine;
        return preset("Wide Pad", "PAD", EngineType::Wavetable, {
            { W::TableSelect, wavetableByLabel("VOWL") }, { W::Position, 0.2f },
            { W::MorphSpeed, 0.06f }, { W::MorphDepth, 0.5f }, { W::UnisonSpread, 0.6f },
            { W::AmpAttack, 1.0f }, { W::AmpDecay, 3.0f }, { W::AmpSustain, 0.8f }, { W::AmpRelease, 4.0f },
            { W::Level, 0.4f }, { W::VoiceCountParam, 16.0f } });
    }

    // ---- The reverb network and the through-zero dry line, as inserts stored with the sound -------

    // SPRNG KEY: an electric piano through a spring (insert SPRG: the reverb network's
    // dispersive loop). Its drip is the spring's DISP.
    inline Preset springKeys()
    {
        using F = FMEngine::Param;
        auto p = preset("SprngKey", "KEYS", EngineType::FM, {
            { F::AlgorithmParam, 0.0f },
            { F::Op1Ratio, 1.0f }, { F::Op1Level, 0.8f },
            { F::Op1Attack, 0.002f }, { F::Op1Decay, 0.9f }, { F::Op1Sustain, 0.0f }, { F::Op1Release, 0.3f },
            { F::Op2Ratio, 14.0f }, { F::Op2Level, 0.25f },
            { F::Op2Attack, 0.001f }, { F::Op2Decay, 0.15f }, { F::Op2Sustain, 0.0f }, { F::Op2Release, 0.1f },
            { F::MasterLevel, 0.7f } });
        // SPRG: DCAY 2.2 s, DISP 80, TENS 50, DAMP 30; half of it.
        R::insert(p.soundParams, 0, EffectType::SpringReverbEffect, { 2.2f, 0.8f, 0.5f, 0.3f });
        R::insertWet(p.soundParams, 0, 0.5f);
        return p;
    }

    // SHIMR PAD: a soft pad into the shimmer (insert SHIM: the network with an octave in its
    // loop). The tail climbs an octave with every turn round the loop.
    inline Preset shimmerPad()
    {
        using W = WavetableEngine;
        auto p = preset("ShimrPad", "PAD", EngineType::Wavetable, {
            { W::TableSelect, wavetableByLabel("SIN") }, { W::Position, 0.0f }, { W::UnisonSpread, 0.3f },
            { W::AmpAttack, 0.2f }, { W::AmpDecay, 1.0f }, { W::AmpSustain, 0.7f }, { W::AmpRelease, 1.0f },
            { W::Level, 0.6f } });
        // SHIM: DCAY 5 s, SHIM 70, MODE (the first: an octave up), DAMP 30.
        R::insert(p.soundParams, 0, EffectType::ShimmerReverbEffect, { 5.0f, 0.7f, 0.0f, 0.3f });
        R::insertWet(p.soundParams, 0, 0.5f);
        // The strip brings in the octave below (SUB), under a tail that climbs the octave above.
        R::strip(p.soundParams, EngineType::Wavetable, 1, "SUB", 0.7f);
        return p;
    }

    // THRU FLNG: a flanger that goes through zero (insert FLG with THRU on: the chain delays the
    // dry sound by the flanger's centre, so the swept copy passes through it and the two cancel
    // completely once a cycle). Two saws under it.
    inline Preset thruFlanger()
    {
        using M = MacroOscEngine;
        auto p = preset("ThruFlng", "LEAD", EngineType::MacroOsc, {
            { M::ModelParam, 0.0f }, { M::TimbreParam, 0.7f }, { M::ColorParam, 0.5f },
            { M::AmpAttack, 0.01f }, { M::AmpDecay, 0.5f }, { M::AmpSustain, 0.9f }, { M::AmpRelease, 0.3f },
            { M::LevelParam, 0.7f },
            { M::FilterBase + VF::Cutoff, 9000.0f } });
        // FLG: RATE 0.25 Hz, DPTH 100, FDBK 0, MIX 50 (the slot's WET is that mix: the chain makes
        // it, and at 100 there would be no dry sound for the copy to pass through); then SHPE (as
        // it is), SPRD 0, THRU ON, TIME 3 ms.
        R::insert(p.soundParams, 0, EffectType::FlangerEffect, { 0.25f, 1.0f, 0.0f, 0.5f });
        R::insertWet(p.soundParams, 0, 0.5f);
        p.soundParams[AudioEngine::INSERT_EXTRA_BASE + Flanger::Spread] = 0.0f;
        p.soundParams[AudioEngine::INSERT_EXTRA_BASE + Flanger::Through] = 1.0f;
        p.soundParams[AudioEngine::INSERT_EXTRA_BASE + Flanger::Time] = 0.003f;
        return p;
    }

    // ---- The kit: kick-machine points and resonator drums only, a pad with an LFO of its own -----

    // POINT KIT: every pad is a point of one of two machines. Three kicks are points of the kick
    // machine (the three old kick types, as where its cells start); everything else is a sound of
    // the resonator drum. Two pads carry modulation of their own (the PLFO and PSLT pages):
    //   the gong (top row, pad 5)    its own LFO on its level: it beats five times a second,
    //                                and no other pad does
    //   the low kick (bottom, pad 1) its own slot: VEL on its pitch, a harder stroke is tuned
    //                                higher, as a struck head is
    inline Preset pointKit()
    {
        namespace C = CymbalPresets;
        const auto T = EngineType::Drum;
        ParamMap p;
        C::kick(p, 8, 46.0f, 0.5f, 0.5f, 0.1f, 0.6f, 0.9f, 0.5f, DrumSynthType::AnalogKick);
        C::kick(p, 9, 62.0f, 0.35f, 0.6f, 0.3f, 0.5f, 0.8f, 0.45f, DrumSynthType::FMKick);
        C::kick(p, 10, 90.0f, 0.25f, 0.7f, 0.0f, 0.7f, 0.75f, 0.55f, DrumSynthType::ModCyclesKick);
        C::resonator(p, 11, "TOM", 0.75f, 0.4f);
        C::resonator(p, 12, "SNAR", 0.8f, 0.5f);
        C::resonator(p, 13, "RIM", 0.7f, 0.45f);
        C::resonator(p, 14, "HATC", 0.6f, 0.6f);
        C::resonator(p, 15, "HATO", 0.6f, 0.6f);
        C::resonator(p, 0, "CLVB", 0.7f, 0.35f);
        C::resonator(p, 1, "TOM", 0.75f, 0.65f, 1.5f);
        C::resonator(p, 2, "SNRS", 0.8f, 0.5f);
        C::resonator(p, 3, "CUP", 0.5f, 0.55f);
        C::resonator(p, 4, "GONG", 0.7f, 0.5f);
        C::resonator(p, 5, "RIDE", 0.5f, 0.6f);
        C::resonator(p, 6, "CRSH", 0.5f, 0.4f);
        C::resonator(p, 7, "SPLS", 0.45f, 0.65f);
        // The gong's own LFO (PLFO): 5 Hz, on its level, started by its stroke.
        p[PadModParam::id(4, PadModParam::LFO_RATE)] = 5.0f;
        p[PadModParam::id(4, PadModParam::LFO_DEPTH)] = 0.6f;
        p[PadModParam::id(4, PadModParam::LFO_SHAPE)] = R::shape("SIN");
        p[PadModParam::id(4, PadModParam::LFO_TRIGGER)] = R::trigger("KEY");
        p[PadModParam::id(4, PadModParam::LFO_DEST)] = R::cellDest(T, "AMP");
        // The low kick's own slot (PSLT): VEL on its pitch.
        p[PadModParam::id(8, PadModParam::SLOT1_SRC)] = R::source(T, "VEL");
        p[PadModParam::id(8, PadModParam::SLOT1_DEST)] = R::cellDest(T, "PTCH");
        p[PadModParam::id(8, PadModParam::SLOT1_AMT)] = 0.2f;
        return preset("PointKit", "KIT", T, std::move(p));
    }

    // ---- All of them ------------------------------------------------------------------------------
    inline std::vector<Preset> getAll()
    {
        return { bowCello(), bowGlass(), breathPipe(), scrapeBar(), deepMod(), stepShape(),
                 pluckBody(), dustBody(), reedBody(), morphBody(),
                 fmOnFilter(), seqMorph(), rndEnvTime(),
                 fmWaveOps(), fmCycleOp(),
                 dualFilter(), serialFilter(),
                 seededSpread(), seededWavetable(),
                 monoLead(), widePad(),
                 springKeys(), shimmerPad(), thruFlanger(),
                 pointKit() };
    }
}
