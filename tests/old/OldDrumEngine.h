#pragma once

// -----------------------------------------------------------------------------
// THE DRUM KIT AS IT WAS before it moved onto the modulation router (phase U4-DRUM):
// src/engine/OldDrumEngine.h of commit 46fedbc, the classes renamed (OldDrumEngine, OldDrumVoice,
// OldDrumKitVoiceConfig), the includes pointed at src/, and nothing else changed.
// -----------------------------------------------------------------------------

#include "../../src/engine/EngineBase.h"
#include "OldDrumVoice.h"
#include "../../src/engine/ResonatorEngine.h"
#include "../../src/project/DrumKit.h"
#include "../../src/project/CymbalPresets.h"
#include <array>
#include <atomic>

struct OldDrumKitVoiceConfig
{
    DrumSynthType type;
    float tune;
    float decay;
    float tone;
    float drive;
    float snap;
    float level;
};

class OldDrumEngine : public EngineBase
{
public:
    static constexpr int NUM_VOICES = 16;
    static constexpr int PARAMS_PER_VOICE = 8;

    enum VoiceParam { TUNE = 0, DECAY, TONE, DRIVE, SNAP, LEVEL, PAN, TYPE };

    static constexpr int CHOKE_GROUP_HAT = 1;

    // Pad-to-voice mapping: decouples pad index from voice index
    // padToVoice[padIndex] = voiceIndex (-1 = unmapped)
    struct PadMapping
    {
        std::array<int, NUM_VOICES> padToVoice;
        std::array<int, NUM_VOICES> voiceToPad;

        PadMapping()
        {
            // Default: 1:1 identity mapping
            for (int i = 0; i < NUM_VOICES; ++i)
            {
                padToVoice[static_cast<size_t>(i)] = i;
                voiceToPad[static_cast<size_t>(i)] = i;
            }
        }

        int voiceForPad(int pad) const
        {
            if (pad < 0 || pad >= NUM_VOICES) return -1;
            return padToVoice[static_cast<size_t>(pad)];
        }

        int padForVoice(int voice) const
        {
            if (voice < 0 || voice >= NUM_VOICES) return -1;
            return voiceToPad[static_cast<size_t>(voice)];
        }

        void setMapping(int pad, int voice)
        {
            if (pad < 0 || pad >= NUM_VOICES) return;
            if (voice < -1 || voice >= NUM_VOICES) return;

            // Clear old reverse mapping
            int oldVoice = padToVoice[static_cast<size_t>(pad)];
            if (oldVoice >= 0)
                voiceToPad[static_cast<size_t>(oldVoice)] = -1;

            padToVoice[static_cast<size_t>(pad)] = voice;
            if (voice >= 0)
                voiceToPad[static_cast<size_t>(voice)] = pad;
        }
    };

    OldDrumEngine()
    {
        resonatorSounds();      // built now, never on the audio thread
        resonatorDefaults();
        loadDefaultKit();
        lfoSection.applyAll(EngineLfoSection::each(voices));
    }

    EngineType getType() const override { return EngineType::Drum; }
    juce::String getName() const override { return "Drum"; }

    void prepareToPlay(double sampleRate, int /*samplesPerBlock*/) override
    {
        sr = sampleRate;
        panStep = (float) (1.0 / (0.004 * std::max(sampleRate, 1.0)));
        for (auto& v : voices)
            v.setSampleRate(sampleRate);
    }

    void setTempo(float bpm) override { lfoSection.setTempo(bpm, EngineLfoSection::each(voices)); }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        int numSamples = buffer.getNumSamples();
        int numChannels = buffer.getNumChannels();

        // The mod strip's routes (STRIP page), and the vibrato it brings in.
        lfoSection.stripBlock(wheel, numSamples, sr, EngineLfoSection::each(voices));
        // What the mod slots read once a block: each drum's own pad pressure, the strip, the input.
        if (lfoSection.slotsOn())
            for (int v = 0; v < NUM_VOICES; ++v)
                voices[v].getLfos().setBlockSources(padPressure[(size_t) v], wheel, audioInput.envelope);

        for (int s = 0; s < numSamples; ++s)
        {
            float mixL = 0.0f, mixR = 0.0f;
            for (int v = 0; v < NUM_VOICES; ++v)
            {
                if (!voices[v].isActive())
                    continue;

                float sample = voices[v].process();
                float pan = voicePan[v] + voicePanAT[v] + voices[v].getLfoPan();
                pan = std::clamp(pan, 0.0f, 1.0f);
                // Constant-power pan (-3 dB at centre). The earlier linear law put a centred voice
                // 6 dB down on each side, which left the whole kit far below the synth tracks.
                // The two gains are moved to over at most 4 ms where the pan is set under a
                // ringing drum (a step lock, pad pressure, an LFO's edge); a hit takes them as
                // they stand.
                const float gainL = std::sqrt(1.0f - pan), gainR = std::sqrt(pan);
                panGainL[v] = panStruck[v] ? gainL : dsp::towards(panGainL[v], gainL, panStep);
                panGainR[v] = panStruck[v] ? gainR : dsp::towards(panGainR[v], gainR, panStep);
                panStruck[v] = false;
                float left = sample * panGainL[v];
                float right = sample * panGainR[v];
                mixL += left;
                mixR += right;
            }

            if (numChannels >= 1) buffer.addSample(0, s, mixL);
            if (numChannels >= 2) buffer.addSample(1, s, mixR);
        }

        lfoSection.endBlock(numSamples, EngineLfoSection::each(voices));
    }

    void releaseResources() override {}

    void noteOn(int note, float velocity, int /*voiceIndex*/ = -1) override
    {
        int pad = mapNoteToPad(note);
        if (pad < 0) return;

        int vi = mapping.voiceForPad(pad);
        if (vi < 0 || vi >= NUM_VOICES) return;

        applyChokeGroups(vi);
        // For a mod slot KEY is the drum's place in the kit: -1 at the first voice, +1 at the last.
        voices[vi].getLfos().setNote(vi * 8, velocity);
        voices[vi].noteOn(velocity);
        panStruck[(size_t) vi] = true;
        padHeld[(size_t) vi] = true;
    }

    void noteOff(int note, int /*voiceIndex*/ = -1) override
    {
        int pad = mapNoteToPad(note);
        if (pad < 0) return;

        int vi = mapping.voiceForPad(pad);
        if (vi < 0 || vi >= NUM_VOICES) return;

        voices[vi].noteOff();
        voices[vi].clearAftertouch();
        voices[vi].setPadPressure(0.0f);
        padHeld[(size_t) vi] = false;
        voicePanAT[vi] = 0.0f;
        padPressure[(size_t) vi] = 0.0f;
    }

    void allNotesOff() override
    {
        soundCut();
        for (int i = 0; i < NUM_VOICES; ++i)
        {
            voices[i].reset();
            padHeld[(size_t) i] = false;
            voicePanAT[i] = 0.0f;
            padPressure[(size_t) i] = 0.0f;
        }
    }

    // Per-pad poly aftertouch -> per-slot expression routing
    void aftertouch(int note, float pressure) override
    {
        int pad = mapNoteToPad(note);
        if (pad < 0) return;

        int vi = mapping.voiceForPad(pad);
        if (vi < 0 || vi >= NUM_VOICES) return;

        float panMod = voices[vi].applyAftertouch(pressure);
        voicePanAT[vi] = panMod;
        padPressure[(size_t) vi] = std::clamp(pressure, 0.0f, 1.0f);    // the mod slots' PRES
        // ... and the cymbal machine's energy (EXC), for as long as the pad or the step lasts: pressure
        // that comes after the note has ended (a late message, a lane's last value) feeds nothing,
        // or a bowed cymbal would sound until that pad was played again.
        voices[vi].setPadPressure(padHeld[(size_t) vi] ? padPressure[(size_t) vi] : 0.0f);
    }

    // The pitch strip bends every drum of the kit, two semitones either way (the kit has no BEND cell).
    static constexpr float BEND_SEMITONES = 2.0f;
    void pitchBend(float amount) override
    {
        const float ratio = std::exp2(std::clamp(amount, -1.0f, 1.0f) * BEND_SEMITONES / 12.0f);
        for (auto& v : voices)
            v.setBendRatio(ratio);
    }

    // The mod strip: the routes of the kit's STRIP page. By default it opens the whole kit up:
    // every drum rings longer (DEC +79 = up to three times, from its next hit) and is driven
    // harder (DRIV +50). The voices are told at once, not at the next block and not smoothed: a
    // drum reads its decay at the strike.
    void modWheel(float value) override
    {
        wheel = std::clamp(value, 0.0f, 1.0f);
        lfoSection.stripBlock(wheel, 0, sr, EngineLfoSection::each(voices));
    }

    void setParameter(ParamId id, float value) override
    {
        // The two LFO pages (ids 158-173) belong to the whole kit.
        if (lfoSection.owns(id))
        {
            lfoSection.set(id, value, EngineLfoSection::each(voices));
            return;
        }

        // (What is no number is not taken: a clamp lets it through, and one pad's level then
        // silences the whole kit and everything after it.)
        if (! std::isfinite(value))
            return;

        // The kick-machine page of each pad (ids 400-527).
        if (isKickParam(id))
        {
            const int voice = (id - KICK_BASE) / OldDrumVoice::NumKickParams, param = (id - KICK_BASE) % OldDrumVoice::NumKickParams;
            value = kickSpec(voice, param).clamp(value);
            voices[voice].setKickParam(param, value);
            kickValues[(size_t) (id - KICK_BASE)] = value;
            return;
        }

        // The sound page of each pad (ids 528-655): the bank's cells on a BANK pad, the resonator
        // drum's on any other (heard on the pads whose TYPE is RSN).
        if (isSoundParam(id))
        {
            const int voice = (id - SOUND_BASE) / SOUND_CELLS, param = (id - SOUND_BASE) % SOUND_CELLS;
            const bool onBank = isBank(voice), onCymbal = isCymbalMachine(voice);
            const ParamSpec spec = soundSpec(voice, param);
            value = spec.clamp(value);
            if (spec.isDiscrete())
                value = (float) std::lround(value);
            soundValues[(size_t) (id - SOUND_BASE)] = value;
            if (onBank)
                voices[voice].setBankParam(param, value);
            else if (onCymbal)
                voices[voice].setCymbalParam(param, value);
            else if (isResonator(voice))
                applyResonatorCell(voice, param, value);
            return;
        }

        int voiceIdx = id / PARAMS_PER_VOICE;
        int param = id % PARAMS_PER_VOICE;
        if (voiceIdx < 0 || voiceIdx >= NUM_VOICES) return;

        auto& v = voices[voiceIdx];
        switch (param)
        {
            case TUNE:  value = std::clamp(value, 1.0f, 24000.0f); v.setTune(value); break;
            case DECAY: value = std::clamp(value, 0.001f, 60.0f); v.setDecay(value); break;
            case TONE:  value = std::clamp(value, 0.0f, 1.0f); v.setTone(value); break;
            case DRIVE: value = std::clamp(value, 0.0f, 1.0f); v.setDrive(value); break;
            case SNAP:  value = std::clamp(value, 0.0f, 1.0f); v.setSnap(value); break;
            case LEVEL: value = std::clamp(value, 0.0f, 1.0f); v.setLevel(value); break;
            case PAN:   value = std::clamp(value, 0.0f, 1.0f); voicePan[voiceIdx] = value; break;
            case TYPE:
            {
                value = (float) std::clamp((int) std::lround(value), 0, NUM_DRUM_TYPES - 1);
                const bool changes = v.getType() != static_cast<DrumSynthType>((int) value);
                if (v.isActive() && changes)
                    soundSwapped();     // another drum takes over a hit that is ringing
                v.setType(static_cast<DrumSynthType>((int) value));
                paramValues[(size_t) id] = value;
                // (The pad's sound page belongs to the type: it starts from that type's own.)
                if (changes)
                    resetSoundPage(voiceIdx);
                break;
            }
        }
        paramValues[(size_t) id] = value;
    }

    // The true current value of every voice parameter.
    float getParameter(ParamId id) const override
    {
        if (lfoSection.owns(id))
            return lfoSection.get(id);
        if (id >= 0 && id < NUM_VOICES * PARAMS_PER_VOICE)
            return paramValues[(size_t) id];
        if (isKickParam(id))
            return kickValues[(size_t) (id - KICK_BASE)];
        if (isSoundParam(id))
            return soundValues[(size_t) (id - SOUND_BASE)];
        return 0.0f;
    }

    // ---- Which pad a note plays --------------------------------------------------------------
    // Every note 0-127 plays a pad, so the keys, the pads, recorded steps and the arpeggiator all
    // reach the kit through noteOn and agree on the voice.
    //   0-15              the pad itself (what the DRUMS pads and recorded pad hits send)
    //   96-103, 112-119   the Launchkey's own pad notes: top row, bottom row
    //   any other note    by its place in a two-octave span starting on an even C (C1, C3, C5 ...;
    //                     the 25 keys are exactly one span at octave 0, and the firmware's octave
    //                     buttons move by whole octaves):
    //                       C  .. G   the bottom pad row, left to right (lowest C = the lower left pad)
    //                       G# .. D#  the top pad row, left to right
    //                       E  .. B   the bottom pad row again (the drums under the right hand too)
    // The keys reach all sixteen pads at every octave setting but one: three octaves down the
    // lowest keys are notes 12-15, which are pads, and top pads 6-8 have no key.
    static constexpr int KEY_SPAN = 24;
    static int padForNote(int note)
    {
        if (note < 0 || note > 127) return -1;
        if (note < NUM_VOICES) return note;
        if (note >= 96 && note < 104) return note - 96;
        if (note >= 112 && note < 120) return note - 112 + 8;
        const int k = note % KEY_SPAN;
        return k < 8 ? k + 8 : k - 8;
    }

    // What TYPE offers (OldDrumVoice.h says what became of the rest).
    static constexpr int NUM_DRUM_TYPES = OldDrumVoice::NUM_OFFERED;

    // SND is a whole sound set at once (some forty cells of the voice): no work for a step lock.
    bool isRebuildParam(ParamId id) const override
    {
        return isSoundParam(id) && (id - SOUND_BASE) % SOUND_CELLS == 0 && isResonator((id - SOUND_BASE) / SOUND_CELLS);
    }

    // ---- The kick machine's page (type KSYN): eight more cells per pad, ids 400 + pad * 8 + n ------
    static constexpr int KICK_BASE = EngineParamRange::THIRD_BASE;
    static bool isKickParam(int id) { return id >= KICK_BASE && id < KICK_BASE + NUM_VOICES * OldDrumVoice::NumKickParams; }

    static ParamSpec kickSpec(int voice, int param)
    {
        auto L = ParamKind::Linear; auto G = ParamKind::Log;
        const int b = KICK_BASE + voice * OldDrumVoice::NumKickParams;
        const float d = OldDrumVoice::kickDefault(param);
        switch (param)
        {
            case OldDrumVoice::KickSweep:      return { "SWEP", b + param, 0, 72, d, L, "st" };
            case OldDrumVoice::KickSweepTime:  return { "SWPT", b + param, 0.002f, 1, d, G, "s" };
            case OldDrumVoice::KickSweepCurve: return { "SCRV", b + param, -1, 1, d, L };
            case OldDrumVoice::KickShape:      return { "SHAP", b + param, 0, 1, d, L };
            case OldDrumVoice::KickKnock:      return { "KNCK", b + param, 0, 1, d, L };
            case OldDrumVoice::KickKnockTone:  return { "KTON", b + param, 0, 1, d, L };
            case OldDrumVoice::KickHold:       return { "HOLD", b + param, 0, 0.3f, d, L, "s", nullptr, 0, 0.005f };
            default:                        return { "ACRV", b + param, -1, 1, d, L };
        }
    }

    // The LFO destinations: the original seven, then the kick machine's eight cells (each drum
    // moves its own; they are heard on the pads whose TYPE is KSYN) and the other LFO.
    // Not offered: TYPE (a choice); TUNE and LEVL are the original PTCH and AMP.
    static const LfoDestList& lfoDests()
    {
        static const LfoDestList list = []
        {
            LfoDestList l(OldDrumVoice::lfoDestLabels(), OldDrumVoice::NumLfoDests);
            l.group();
            for (int n = 0; n < OldDrumVoice::NumKickParams; ++n)
                l.add(kickSpec(0, n), nullptr, n);
            l.addCross();
            // [U2b] The resonator drum's cells, last (what was in the list keeps its place): each
            // drum moves its own, in the cell's own scale; they are heard on the pads whose TYPE is
            // RSN. Not DCAY (DEC is the drum's), nor what a note starts with.
            {
                using R = ResonatorEngine;
                using V = ResonatorVoice;
                l.group();
                for (int id : { (int) R::MorphParam, (int) R::ExciterMorphParam, (int) R::EnergyParam })
                    l.add(R::cellSpec(id), nullptr, OldDrumVoice::RSN_KEY + R::voiceCell(id));
                l.group();
                for (int i = 0; i < res::NumBody; ++i)
                    l.add(R::cellSpec(R::BodyBase + i), nullptr, OldDrumVoice::RSN_KEY + V::BodyFirst + i);
                l.group();
                for (int i = 0; i < res::NumMore; ++i)
                    l.add(R::cellSpec(R::MoreBase + i), nullptr, OldDrumVoice::RSN_KEY + V::MoreFirst + i);
                l.group();
                for (int i = 0; i < res::NumExcite; ++i)
                    l.add(R::cellSpec(R::ExciteBase + i), nullptr, OldDrumVoice::RSN_KEY + V::ExciteFirst + i);
            }
            // The mod strip until the STRIP page says otherwise (see modWheel). DEC moves by
            // octaves of time: 2 ^ (2 x 0.7925) = 3.
            l.stripRoute(0, OldDrumVoice::LfoDecay, 0.7925f);
            l.stripRoute(1, OldDrumVoice::LfoDrive, 0.5f);
            return l;
        }();
        static const bool registered = (list.registerGroups(), true);
        juce::ignoreUnused(registered);
        return list;
    }

    // ---- The sound page of a pad: eight more cells, ids 528 + pad * 8 + n -------------------------
    // What they are depends on the pad's TYPE: the oscillator bank's cells (BANK), or the
    // resonator drum's (RSN). A pad that changes its TYPE starts them from the new type's own.
    static constexpr int SOUND_BASE = KICK_BASE + NUM_VOICES * OldDrumVoice::NumKickParams;
    static constexpr int SOUND_CELLS = 8;
    static bool isSoundParam(int id) { return id >= SOUND_BASE && id < SOUND_BASE + NUM_VOICES * SOUND_CELLS; }

    static ParamSpec bankSpec(int voice, int param)
    {
        auto L = ParamKind::Linear; auto C = ParamKind::Choice;
        const int b = SOUND_BASE + voice * SOUND_CELLS;
        const float d = OscBank::paramDefault(param);
        switch (param)
        {
            case OscBank::ParModel:   return { "MODL", b + param, 0, OscBank::NumModels - 1, d, C, "", OscBank::modelNames(), OscBank::NumModels };
            case OscBank::ParPing:    return { "PING", b + param, 0, 1, d, L };
            case OscBank::ParColour:  return { "COLR", b + param, 0, 1, d, L };
            case OscBank::ParExcite:  return { "EXC",  b + param, 0, OscBank::MAX_EXCITE, d, L, "x", nullptr, 0, 0.05f };
            case OscBank::ParDamp:    return { "DAMP", b + param, 0, 1, d, L };
            case OscBank::ParBloom:   return { "BLOM", b + param, 0, 1, d, L };
            case OscBank::ParCouple:  return { "COUP", b + param, 0, 1, d, L };
            default:                  return { "LOFI", b + param, 0, 1, d, L };
        }
    }

    // The cymbal machine's page (TYPE CSYN, kept until the owner has decided: OldDrumVoice.h).
    static ParamSpec cymbalSpec(int voice, int param)
    {
        auto L = ParamKind::Linear; auto C = ParamKind::Choice;
        const int b = SOUND_BASE + voice * SOUND_CELLS;
        const float d = CymbalMachine::paramDefault(param);
        switch (param)
        {
            case CymbalMachine::ParModel:   return { "MODL", b + param, 0, CymbalMachine::NumModels - 1, d, C, "", CymbalMachine::modelNames(), CymbalMachine::NumModels };
            case CymbalMachine::ParPing:    return { "PING", b + param, 0, 1, d, L };
            case CymbalMachine::ParColour:  return { "COLR", b + param, 0, 1, d, L };
            case CymbalMachine::ParExcite:  return { "EXC",  b + param, 0, CymbalMachine::MAX_EXCITE, d, L, "x", nullptr, 0, 0.05f };
            case CymbalMachine::ParDamp:    return { "DAMP", b + param, 0, 1, d, L };
            case CymbalMachine::ParBloom:   return { "BLOM", b + param, 0, 1, d, L };
            case CymbalMachine::ParCouple:  return { "COUP", b + param, 0, 1, d, L };
            default:                        return { "LOFI", b + param, 0, 1, d, L };
        }
    }
    static ParamPage cymbalPage(int voice)
    {
        ParamPage p;
        p.name = "CYM";
        p.section = "CYM";
        for (int n = 0; n < SOUND_CELLS; ++n)
            p.params[(size_t) n] = cymbalSpec(voice, n);
        return p;
    }
    bool isCymbalMachine(int voice) const
    {
        return voice >= 0 && voice < NUM_VOICES && voices[voice].getType() == DrumSynthType::CymbalSynth;
    }

    // The resonator drum's page. SND is the sound: one of the resonator's ready sounds
    // (project/presets/ResonatorPresets.h: each a preset of the RSN engine, cell for cell). The
    // other seven are cells of the RSN engine by their own names, here as what the pad ADDS to
    // the sound's own value (0 = the sound as it is): where the body and the exciter are among
    // their points, the energy, and four latents.
    enum ResonatorCell { RsnSound = 0, RsnMorph, RsnExciter, RsnEnergy, RsnDamp, RsnWire, RsnMeet, RsnDirect };

    static const char* const* soundNames(int& count)
    {
        static const std::vector<const char*> names = []
        {
            std::vector<const char*> n;
            for (const auto& snd : resonatorSounds())
                n.push_back(snd.tag);
            return n;
        }();
        count = (int) names.size();
        return names.data();
    }

    static ParamSpec resonatorSpec(int voice, int param)
    {
        auto L = ParamKind::Linear; auto C = ParamKind::Choice;
        const int b = SOUND_BASE + voice * SOUND_CELLS;
        int sounds = 0;
        const char* const* names = soundNames(sounds);
        switch (param)
        {
            case RsnSound:   return { "SND",  b + param, 0, (float) (sounds - 1), 0, C, "", names, sounds };
            case RsnMorph:   return { "MRPH", b + param, -(float) (res::NumBodyPoints - 1), (float) (res::NumBodyPoints - 1), 0, L, "x", nullptr, 0, 0.05f };
            case RsnExciter: return { "XMPH", b + param, -(float) (res::NumExcitePoints - 1), (float) (res::NumExcitePoints - 1), 0, L, "x", nullptr, 0, 0.05f };
            case RsnEnergy:  return { "ENRG", b + param, -1, 1, 0, L };
            case RsnDamp:    return { "DAMP", b + param, -1, 1, 0, L };
            case RsnWire:    return { "WIRE", b + param, -1, 1, 0, L };
            case RsnMeet:    return { "MEET", b + param, -1, 1, 0, L };
            default:         return { "DIR",  b + param, -1, 1, 0, L };
        }
    }

    // The voice's cell behind a cell of the resonator drum's page (-1: SND).
    static int resonatorCellOf(int param)
    {
        using V = ResonatorVoice;
        static constexpr int cells[SOUND_CELLS] = { -1, V::Morph, V::ExciterMorph, V::Energy, V::BodyFirst + res::Damp,
                                                    V::MoreFirst + res::Wire, V::MoreFirst + res::Meet, V::MoreFirst + res::Direct };
        return cells[std::clamp(param, 0, SOUND_CELLS - 1)];
    }

    static ParamPage bankPage(int voice)
    {
        ParamPage p;
        p.name = "BANK";
        p.section = "BANK";
        for (int n = 0; n < SOUND_CELLS; ++n)
            p.params[(size_t) n] = bankSpec(voice, n);
        return p;
    }

    static ParamPage resonatorPage(int voice)
    {
        ParamPage p;
        p.name = "RSN";
        p.section = "RSN";
        for (int n = 0; n < SOUND_CELLS; ++n)
            p.params[(size_t) n] = resonatorSpec(voice, n);
        return p;
    }

    bool isBank(int voice) const
    {
        return voice >= 0 && voice < NUM_VOICES && voices[voice].getType() == DrumSynthType::OscBank;
    }
    bool isResonator(int voice) const
    {
        return voice >= 0 && voice < NUM_VOICES && voices[voice].getType() == DrumSynthType::Resonator;
    }
    // A pad whose TYPE is BANK, RSN or CSYN has a sound page.
    bool hasSoundPage(int voice) const { return isBank(voice) || isResonator(voice) || isCymbalMachine(voice); }
    ParamPage soundPage(int voice) const
    {
        return isBank(voice) ? bankPage(voice) : (isCymbalMachine(voice) ? cymbalPage(voice) : resonatorPage(voice));
    }
    // One cell of it (no page is made: a page has names, and names are allocated - this is what
    // setParameter reads, which a step lock calls on the audio thread).
    ParamSpec soundSpec(int voice, int param) const
    {
        return isBank(voice) ? bankSpec(voice, param) : (isCymbalMachine(voice) ? cymbalSpec(voice, param) : resonatorSpec(voice, param));
    }

    // The pages each pad has in getParamPages(): its DR page, its KICK page, its sound page.
    static constexpr int PAGES_PER_PAD = 3;

    static ParamPage kickPage(int voice)
    {
        ParamPage p;
        p.name = "KICK";
        p.section = "KICK";
        for (int n = 0; n < OldDrumVoice::NumKickParams; ++n)
            p.params[(size_t) n] = kickSpec(voice, n);
        return p;
    }

    bool isKickMachine(int voice) const
    {
        return voice >= 0 && voice < NUM_VOICES && voices[voice].getType() == DrumSynthType::KickSynth;
    }

    static const char* const* typeNames()
    {
        static const char* const names[NUM_DRUM_TYPES] = {
            "FMKK", "KICK", "MKIK", "CLAP", "SHKR", "SUB", "KSYN", "CYMB", "RSN", "BANK", "TOM", "SNAR", "FMSN", "CSYN" };
        return names;
    }

    // The eight parameters of one voice, in the order used everywhere (ids voice * 8 + n).
    static ParamPage voicePage(int voice)
    {
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto C = ParamKind::Choice;
        int b = voice * PARAMS_PER_VOICE;
        ParamPage p;
        p.name = "DR " + juce::String(voice + 1);
        p.section = voice < 8 ? "TOP" : "BOTTOM";   // the two pad rows
        p.params = {{
            { "TUNE", b + TUNE, 20, 12000, 100, G, "Hz" },
            { "DEC",  b + DECAY, 0.01f, 4, 0.3f, G, "s" },
            { "TONE", b + TONE, 0, 1, 0.5f, L },
            { "DRIV", b + DRIVE, 0, 1, 0, L },
            { "SNAP", b + SNAP, 0, 1, 0.5f, L },
            { "LEVL", b + LEVEL, 0, 1, 0.8f, L },
            { "PAN",  b + PAN, 0, 1, 0.5f, L },
            { "TYPE", b + TYPE, 0, NUM_DRUM_TYPES - 1, 0, C, "", typeNames(), NUM_DRUM_TYPES } }};
        return p;
    }

    // One page per voice (pad), in pad order, then the kit's two LFO pages (every voice has its
    // own pair of LFOs with these settings).
    ParamPages getParamPages() const override
    {
        ParamPages out;
        for (int v = 0; v < NUM_VOICES; ++v)
            out.push_back(voicePage(v));
        // Every pad's kick-machine page (used by the pads whose TYPE is KSYN; the values are kept
        // for the others). The two LFO pages stay last, as on every engine.
        for (int v = 0; v < NUM_VOICES; ++v)
            out.push_back(kickPage(v));
        // And every pad's sound page (the bank's cells on a BANK pad, else the resonator drum's).
        for (int v = 0; v < NUM_VOICES; ++v)
            out.push_back(soundPage(v));
        lfoSection.appendPages(out);
        lfoSection.insertModPages(out);
        return out;
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (EngineLfoSection::owns(id))
            return EngineLfoSection::name(id);
        int param = id % PARAMS_PER_VOICE;
        switch (param)
        {
            case TUNE:  return "TN";
            case DECAY: return "DC";
            case TONE:  return "TO";
            case DRIVE: return "DV";
            case SNAP:  return "SN";
            case LEVEL: return "LV";
            case PAN:   return "PN";
            case TYPE:  return "TY";
            default:    return {};
        }
    }

    int getParameterCount() const override { return NUM_VOICES * PARAMS_PER_VOICE; }

    // The pad whose voice the screens edit: set by DRUMS when a pad is pressed, read by PARAM for
    // its single "DR n" page. Message thread only; it is not part of the sound and is not saved.
    void setSelectedVoice(int voice) { selectedVoice = juce::jlimit(0, NUM_VOICES - 1, voice); }
    int getSelectedVoice() const { return selectedVoice; }

    OldDrumVoice& getVoice(int index) { return voices[index]; }
    const OldDrumVoice& getVoice(int index) const { return voices[index]; }

    int getChokeGroup(int voiceIndex) const
    {
        if (voiceIndex < 0 || voiceIndex >= NUM_VOICES) return -1;
        return chokeGroups[voiceIndex];
    }

    // --- Kit management ---

    // Load a kit (call from message thread only; audio thread reads atomically)
    void loadKit(const DrumKit& kit)
    {
        currentKit = kit;
        applyKitToVoices();
    }

    // Get current kit (read-only snapshot)
    const DrumKit& getCurrentKit() const { return currentKit; }

    // Get current kit name
    std::string getKitName() const { return currentKit.name; }

    // Set a single slot's synth type (mixed kit support)
    void setSlotType(int slotIndex, DrumSynthType type)
    {
        if (slotIndex < 0 || slotIndex >= NUM_VOICES) return;
        currentKit.slots[static_cast<size_t>(slotIndex)].synthType = type;
        int vi = mapping.voiceForPad(slotIndex);
        if (vi >= 0 && vi < NUM_VOICES)
            voices[vi].setType(type);
    }

    // Set a single slot's aftertouch destination
    void setSlotAftertouchDest(int slotIndex, DrumATDest dest)
    {
        if (slotIndex < 0 || slotIndex >= NUM_VOICES) return;
        currentKit.slots[static_cast<size_t>(slotIndex)].aftertouchDest = dest;
        int vi = mapping.voiceForPad(slotIndex);
        if (vi >= 0 && vi < NUM_VOICES)
            voices[vi].setAftertouchDest(static_cast<DrumVoiceATDest>(static_cast<int>(dest)));
    }

    // Set a single slot's aftertouch depth
    void setSlotAftertouchDepth(int slotIndex, float depth)
    {
        if (slotIndex < 0 || slotIndex >= NUM_VOICES) return;
        currentKit.slots[static_cast<size_t>(slotIndex)].aftertouchDepth = depth;
        int vi = mapping.voiceForPad(slotIndex);
        if (vi >= 0 && vi < NUM_VOICES)
            voices[vi].setAftertouchDepth(depth);
    }

    // --- Pad mapping ---

    PadMapping& getPadMapping() { return mapping; }
    const PadMapping& getPadMapping() const { return mapping; }

    void setPadToVoice(int pad, int voice)
    {
        mapping.setMapping(pad, voice);
    }

    int getVoiceForPad(int pad) const
    {
        return mapping.voiceForPad(pad);
    }

    static const char* drumTypeAbbrev(DrumSynthType t)
    {
        switch (t)
        {
            case DrumSynthType::FMKick:       return "KK";
            case DrumSynthType::AnalogKick:    return "AK";
            case DrumSynthType::ModCyclesKick: return "MK";
            case DrumSynthType::NoiseSnare:    return "SN";
            case DrumSynthType::FMSnare:       return "FS";
            case DrumSynthType::Clap:          return "CP";
            case DrumSynthType::ClosedHat:     return "CH";
            case DrumSynthType::OpenHat:       return "OH";
            case DrumSynthType::Cymbal:        return "CY";
            case DrumSynthType::Tom:           return "TM";
            case DrumSynthType::Rim:           return "RM";
            case DrumSynthType::Shaker:        return "SH";
            case DrumSynthType::Clave:         return "CL";
            case DrumSynthType::SubBass:       return "SB";
            case DrumSynthType::KickSynth:     return "KS";
            case DrumSynthType::CymbalSynth:   return "CS";
            case DrumSynthType::Resonator:     return "RS";
            case DrumSynthType::OscBank:       return "BK";
        }
        return "--";
    }

private:
    std::array<OldDrumVoice, NUM_VOICES> voices;
    EngineLfoSection lfoSection { lfoDests() };
    std::array<float, NUM_VOICES> voicePan;
    std::array<float, NUM_VOICES * PARAMS_PER_VOICE> paramValues {};
    std::array<float, NUM_VOICES * OldDrumVoice::NumKickParams> kickValues = [] {
        std::array<float, NUM_VOICES * OldDrumVoice::NumKickParams> a {};
        for (size_t i = 0; i < a.size(); ++i) a[i] = OldDrumVoice::kickDefault((int) (i % OldDrumVoice::NumKickParams));
        return a;
    }();
    std::array<bool, NUM_VOICES> padHeld {};        // between a pad's (a step's) note on and its note off
    std::array<float, NUM_VOICES * SOUND_CELLS> soundValues {};      // the sound pages (the resonator drum's cells start at 0)
    int selectedVoice = 8;      // the first pad of the bottom row, as on DRUMS
    float wheel = 0.0f;         // the mod strip as sent
    std::array<float, NUM_VOICES> voicePanAT;  // aftertouch pan modulation
    std::array<float, NUM_VOICES> padPressure {};  // each drum's pad pressure, 0..1 (the mod slots' PRES)
    std::array<float, NUM_VOICES> panGainL {}, panGainR {};    // the pan as it is heard (processBlock)
    std::array<bool, NUM_VOICES> panStruck {};
    float panStep = 0.005f;
    std::array<int, NUM_VOICES> chokeGroups;
    PadMapping mapping;
    DrumKit currentKit;
    double sr = 44100.0;

    int mapNoteToPad(int note) const { return padForNote(note); }

    void applyChokeGroups(int triggeringVoice)
    {
        int group = chokeGroups[triggeringVoice];
        if (group <= 0) return;

        for (int i = 0; i < NUM_VOICES; ++i)
        {
            if (i != triggeringVoice && chokeGroups[i] == group && voices[i].isActive())
                voices[i].choke();
        }
    }

    // Every cell of the RSN engine that belongs to a voice, with its default: what a pad's voice
    // is put back to before a sound is given to it.
    static const std::vector<std::pair<int, float>>& resonatorDefaults()
    {
        static const std::vector<std::pair<int, float>> all = []
        {
            std::vector<std::pair<int, float>> d;
            for (const auto& page : ResonatorEngine::ownPages())
                for (const auto& spec : page.params)
                    if (spec.isValid() && spec.id != ResonatorEngine::GrainSample)
                        d.push_back({ spec.id, spec.defaultVal });
            for (int stage = 0; stage < StageCurveParam::STAGES; ++stage)       // (the amp envelope's own curves)
                d.push_back({ StageCurveParam::id(0, stage), 0.0f });
            return d;
        }();
        return all;
    }

    // One cell of a sound to a pad's voice: the voice's own cells are kept by the drum voice (the
    // pad's page adds to them), the rest goes to the voice at once.
    void applySoundCell(int voice, int id, float value)
    {
        const int cell = id < ResonatorEngine::NumOwnParams ? ResonatorEngine::voiceCell(id) : -1;
        if (cell >= 0)
            voices[voice].setResonatorBase(cell, ResonatorEngine::cellSpec(id).clamp(value));
        else
            ResonatorEngine::applySound(voices[voice].getResonator(), id, value);
    }

    // SND: the whole sound.
    void applyKitSound(int voice, int index)
    {
        const auto& sounds = resonatorSounds();
        const auto& sound = sounds[(size_t) std::clamp(index, 0, (int) sounds.size() - 1)];
        if (voices[voice].isActive())
            soundSwapped();
        for (const auto& d : resonatorDefaults())
            applySoundCell(voice, d.first, d.second);
        for (const auto& kv : sound.params)
            applySoundCell(voice, kv.first, kv.second);
        voices[voice].setResonatorHost(sound.decScale, sound.pressEnergy, sound.pressDamp, sound.gated, ! sound.rings(),
                                       sound.driveWire, sound.dec);
    }

    void applyResonatorCell(int voice, int param, float value)
    {
        if (param == RsnSound)
            applyKitSound(voice, (int) value);
        else
            voices[voice].setResonatorOffset(resonatorCellOf(param), value);
    }

    // A pad's sound page from its type's own values.
    void resetSoundPage(int voice)
    {
        const bool onBank = isBank(voice);
        for (int n = 0; n < SOUND_CELLS; ++n)       // (cell by cell: a TYPE locked on a step comes here on the audio thread)
            setParameter(SOUND_BASE + voice * SOUND_CELLS + n, soundSpec(voice, n).defaultVal);
        if (onBank)     // (what setType made of a drum type that is a sound of the bank now)
            soundValues[(size_t) (voice * SOUND_CELLS + OscBank::ParModel)] = voices[voice].getBankParam(OscBank::ParModel);
    }

    // A pad as one of the resonator's ready sounds, at the pitch and length the sound suggests.
    void configureResonator(int index, const char* tag, float level, float pan, int choke = 0)
    {
        const int n = resonatorSoundIndex(tag);
        const auto& sound = resonatorSounds()[(size_t) n];
        configureVoice(index, { DrumSynthType::Resonator, sound.hz, sound.dec, 0.5f, 0.0f, 0.5f, level }, pan, choke);
        resetSoundPage(index);
        setParameter(SOUND_BASE + index * SOUND_CELLS + RsnSound, (float) n);
    }

    // A pad as a sound of the oscillator bank.
    void configureBank(int index, int model, const OldDrumKitVoiceConfig& cfg, float pan, int choke = 0)
    {
        configureVoice(index, cfg, pan, choke);
        resetSoundPage(index);
        setParameter(SOUND_BASE + index * SOUND_CELLS + OscBank::ParModel, (float) model);
    }

    void configureVoice(int index, const OldDrumKitVoiceConfig& cfg, float pan = 0.5f, int choke = 0)
    {
        voices[index].setType(cfg.type);
        voices[index].setTune(cfg.tune);
        voices[index].setDecay(cfg.decay);
        voices[index].setTone(cfg.tone);
        voices[index].setDrive(cfg.drive);
        voices[index].setSnap(cfg.snap);
        voices[index].setLevel(cfg.level);
        voicePan[index] = pan;
        chokeGroups[index] = choke;

        float* pv = &paramValues[(size_t) (index * PARAMS_PER_VOICE)];
        pv[TUNE] = cfg.tune; pv[DECAY] = cfg.decay; pv[TONE] = cfg.tone; pv[DRIVE] = cfg.drive;
        pv[SNAP] = cfg.snap; pv[LEVEL] = cfg.level; pv[PAN] = pan; pv[TYPE] = (float) (int) cfg.type;
    }

    // Apply the current kit configuration to all voices
    void applyKitToVoices()
    {
        for (int i = 0; i < NUM_VOICES; ++i)
        {
            const auto& slot = currentKit.slots[static_cast<size_t>(i)];
            int vi = mapping.voiceForPad(i);
            if (vi < 0 || vi >= NUM_VOICES) continue;

            // (A drum type that is not offered any more is a sound of the resonator drum.)
            const char* folded = CymbalPresets::retiredSound(slot.synthType);
            const float slotDecay = CymbalPresets::foldedDecay(slot.synthType, slot.params[1]);
            voices[vi].setType(folded != nullptr ? DrumSynthType::Resonator : slot.synthType);
            voices[vi].setTune(slot.params[0]);
            voices[vi].setDecay(slotDecay);
            voices[vi].setTone(slot.params[2]);
            voices[vi].setDrive(slot.params[3]);
            voices[vi].setSnap(slot.params[4]);
            voices[vi].setLevel(slot.params[5]);
            voicePan[vi] = slot.params[6];
            chokeGroups[vi] = slot.chokeGroup;

            float* pv = &paramValues[(size_t) (vi * PARAMS_PER_VOICE)];
            for (int k = 0; k < 7; ++k)
                pv[k] = slot.params[(size_t) k];
            pv[DECAY] = slotDecay;
            pv[TYPE] = (float) (int) voices[vi].getType();
            resetSoundPage(vi);
            if (folded != nullptr)
                setParameter(SOUND_BASE + vi * SOUND_CELLS + RsnSound, (float) resonatorSoundIndex(folded));

            // Apply aftertouch routing
            voices[vi].setAftertouchDest(
                static_cast<DrumVoiceATDest>(static_cast<int>(slot.aftertouchDest)));
            voices[vi].setAftertouchDepth(slot.aftertouchDepth);
        }
    }

    void loadDefaultKit()
    {
        voicePan.fill(0.5f);
        voicePanAT.fill(0.0f);
        chokeGroups.fill(0);

        // Top row pads 0-5: Sub bass notes (C1, D1, D#1, F1, G1, A#1). TONE 0 = a pure sine.
        configureVoice(0, { DrumSynthType::SubBass, 32.70f, 0.1f, 0.0f, 0.0f, 0.5f, 0.85f }, 0.4f);
        configureVoice(1, { DrumSynthType::SubBass, 36.71f, 0.1f, 0.0f, 0.0f, 0.5f, 0.85f }, 0.45f);
        configureVoice(2, { DrumSynthType::SubBass, 38.89f, 0.1f, 0.0f, 0.0f, 0.5f, 0.85f }, 0.45f);
        configureVoice(3, { DrumSynthType::SubBass, 43.65f, 0.1f, 0.0f, 0.0f, 0.5f, 0.85f }, 0.5f);
        configureVoice(4, { DrumSynthType::SubBass, 49.00f, 0.1f, 0.0f, 0.0f, 0.5f, 0.85f }, 0.55f);
        configureVoice(5, { DrumSynthType::SubBass, 58.27f, 0.1f, 0.0f, 0.0f, 0.5f, 0.85f }, 0.55f);

        // Top row pads 6-7: the cymbal machine's crash and ride (owner, 2026-11-14; before, two
        // of the older CYMB type at TUNE 7000 and 9000).
        configureResonator(6, "CRSH", 0.5f, 0.35f);
        configureResonator(7, "RIDE", 0.5f, 0.65f);

        // Bottom row pads 8-10: Three kicks
        // The lower left pad is the kick machine (its KICK page starts from OldDrumVoice::kickDefault).
        configureVoice(8,  { DrumSynthType::KickSynth,    48.0f,  0.5f, 0.7f, 0.2f, 0.7f, 0.9f }, 0.5f);
        configureVoice(9,  { DrumSynthType::AnalogKick,    52.0f,  0.6f, 0.5f, 0.0f, 0.5f, 0.9f }, 0.5f);
        configureVoice(10, { DrumSynthType::ModCyclesKick, 55.0f,  0.7f, 0.6f, 0.4f, 0.6f, 0.9f }, 0.5f);

        // Bottom row pad 11: a tom (the resonator drum)
        configureResonator(11, "TOM", 0.75f, 0.4f);

        // Bottom row pad 12: Snare
        configureResonator(12, "SNAR", 0.8f, 0.5f);

        // Bottom row pad 13: Clap (granular slow-clap)
        configureVoice(13, { DrumSynthType::Clap, 1200.0f, 0.3f, 0.6f, 0.7f, 0.5f, 1.0f }, 0.55f);

        // Bottom row pads 14-15: Hats (choke group 1)
        configureBank(14, OscBank::Hat,     { DrumSynthType::OscBank, 8000.0f, 0.04f, 0.5f, 0.0f, 0.5f, 0.4f }, 0.6f, CHOKE_GROUP_HAT);
        configureBank(15, OscBank::OpenHat, { DrumSynthType::OscBank, 8000.0f, 0.5f,  0.5f, 0.0f, 0.5f, 0.4f }, 0.6f, CHOKE_GROUP_HAT);

        // Build default kit from configured voices
        for (int i = 0; i < NUM_VOICES; ++i)
        {
            auto& slot = currentKit.slots[static_cast<size_t>(i)];
            slot.synthType = voices[i].getType();
            slot.chokeGroup = chokeGroups[i];
            // The kit description carries the real voice settings, so saving and reloading it
            // gives the same sound.
            for (int k = 0; k < 7; ++k)
                slot.params[(size_t) k] = paramValues[(size_t) (i * PARAMS_PER_VOICE + k)];
            // Squeezing a pad after the hit lengthens the sound.
            slot.aftertouchDest = DrumATDest::Decay;
            slot.aftertouchDepth = 1.0f;
            voices[i].setAftertouchDest(DrumVoiceATDest::Decay);
            voices[i].setAftertouchDepth(1.0f);
        }
        currentKit.name = "Default";
    }
};
