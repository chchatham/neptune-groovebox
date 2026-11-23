#pragma once

#include "EngineBase.h"
#include "DrumVoice.h"
#include "ResonatorEngine.h"
#include "../project/DrumKit.h"
#include "../project/CymbalPresets.h"
#include <array>
#include <atomic>

// [U4-DRUM] The kit is on the modulation router (ModRouter.h, "drum kit" under PER ENGINE): ONE
// router for the kit (the LFO1 / LFO2 / STRIP / XIN pages and the sixteen slots are the kit's), each
// drum its own mod::Voice (its own LFOs, randoms and sources) and its own set values of the cells
// that are a drum's own (DrumVoice::modBase through mod::Voice::useBase: the kick machine's eight,
// the resonator drum's 24; 1 for the factors PTCH DEC AMP ETIM FTIM). A hit is struck at the drum's
// next sub-block, after the router has been read: the strip and every LFO are read at the strike
// through the note-on's snap. The drums' oscillators are PhaseOsc: each pad a WAVE (ids 128-143,
// pages WAV1 / WAV2), the router's PM and WPOS on it. With nothing routed a kit renders the samples
// it rendered before ("U4 Drum" compares it with the kit as it was, tests/old/OldDrumEngine.h).
// A pad's own LFO and two slots (PadMod, ids 4000+) are NOT slots of that router: a small
// evaluator of their own (computePadMod), which takes the router's sources, VIA rule and law.
struct DrumKitVoiceConfig
{
    DrumSynthType type;
    float tune;
    float decay;
    float tone;
    float drive;
    float snap;
    float level;
};

class DrumEngine : public EngineBase
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

    DrumEngine()
    {
        resonatorSounds();      // built now, never on the audio thread
        resonatorDefaults();
        // One router for the kit; each drum its own LFOs and its own set values of the cells that
        // are a drum's own (the kick machine's, the resonator drum's). The per-note randoms (RAND,
        // RND2 ..) repeat from run to run: the kit has no ANLG page, so the seed is fixed (0, the
        // drum's number). (The kit draws nothing else per hit: spread::Drum is set aside for it and
        // unused.)
        for (int v = 0; v < NUM_VOICES; ++v)
        {
            voices[(size_t) v].initModBase(lfoDests());
            voices[(size_t) v].getMod().seed(0, v);
        }
        initPadMods();
        loadDefaultKit();
        // The kit's pressure route (pressure slot 1) to one of a drum's seven own destinations
        // is played by the drums themselves (DrumVoice::applyAftertouch): the router leaves it out.
        router.setOwnPressure(ownPressureDests(), false, 1);
    }

    EngineType getType() const override { return EngineType::Drum; }
    juce::String getName() const override { return "Drum"; }

    void prepareToPlay(double sampleRate, int /*samplesPerBlock*/) override
    {
        sr = sampleRate;
        panStep = (float) (1.0 / (0.004 * std::max(sampleRate, 1.0)));
        router.prepare(sampleRate);
        for (auto& v : voices)
            v.setSampleRate(sampleRate);
        for (auto& pm : padMod)
            pm.lfo.setSampleRate(sampleRate);
    }

    void setTempo(float bpm) override { router.setTempo(bpm); currentTempo = bpm; }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        int numSamples = buffer.getNumSamples();
        int numChannels = buffer.getNumChannels();

        // Once a block: the kit's router reads its cells, the strip (as it is: a drum reads it at
        // the strike, through the note-on's snap) and the track input. (The kit's pressure route
        // to a drum's own destination is each drum's DrumVoiceATDest: applyKitPressure.)
        router.beginBlock(audioInput.left, audioInput.right, audioInput.connected() ? audioInput.frames : 0,
                          audioInput.envelope, wheel, sr);

        // Sub-blocks (the router's control rate), the drums one after the other inside each.
        for (int offset = 0; offset < numSamples;)
        {
            const int n = router.beginChunk(offset, numSamples);
            // Tick per-pad LFOs (they run even for idle voices: FREE mode keeps time).
            for (int v = 0; v < NUM_VOICES; ++v)
                if (padMod[(size_t) v].active)
                    tickPadLfo(v, n);
            std::fill(mixL.begin(), mixL.begin() + n, 0.0f);
            std::fill(mixR.begin(), mixR.begin() + n, 0.0f);
            for (int v = 0; v < NUM_VOICES; ++v)
            {
                auto& voice = voices[(size_t) v];
                if (! voice.isActive())
                {
                    // Its LFOs keep time; a cell it was left holding goes back to its set value.
                    if (voice.getMod().idle(routerFor(v), n))
                        voice.getMod().restore(routerFor(v), [&voice](int dest, float value) { voice.setControl(dest, value); });
                    voice.padModOffsets.active = false;
                    continue;
                }
                // The pad's own modulation for this sub-block, before the voice renders.
                if (padMod[(size_t) v].active)
                    computePadMod(v, voice.padModOffsets);
                else
                    voice.padModOffsets.active = false;
                bool panMoved = false;
                const int sounding = voice.render(routerFor(v), n, voiceOut.data(), voicePanMod.data(), panMoved, padPressure[(size_t) v]);
                for (int i = 0; i < sounding; ++i)
                {
                    float pan = voicePan[v] + voicePanAT[v];
                    if (panMoved)
                        pan += voicePanMod[(size_t) i];
                    pan = std::clamp(pan, 0.0f, 1.0f);
                    // Constant-power pan (-3 dB at centre). The earlier linear law put a centred voice
                    // 6 dB down on each side, which left the whole kit far below the synth tracks.
                    // The two gains are moved to over at most 4 ms where the pan is set under a
                    // ringing drum (a step lock, pad pressure, the router); a hit takes them as
                    // they stand.
                    const float gainL = std::sqrt(1.0f - pan), gainR = std::sqrt(pan);
                    panGainL[v] = panStruck[v] ? gainL : dsp::towards(panGainL[v], gainL, panStep);
                    panGainR[v] = panStruck[v] ? gainR : dsp::towards(panGainR[v], gainR, panStep);
                    panStruck[v] = false;
                    // (Two statements: one expression would let the compiler fuse the product
                    // into the sum, which rounds once where the kit always rounded twice.)
                    const float sample = voiceOut[(size_t) i];
                    const float left = sample * panGainL[v];
                    const float right = sample * panGainR[v];
                    mixL[(size_t) i] += left;
                    mixR[(size_t) i] += right;
                }
            }
            for (int i = 0; i < n; ++i)
            {
                if (numChannels >= 1) buffer.addSample(0, offset + i, mixL[(size_t) i]);
                if (numChannels >= 2) buffer.addSample(1, offset + i, mixR[(size_t) i]);
            }
            offset += n;
        }
    }

    void releaseResources() override {}

    void noteOn(int note, float velocity, int /*voiceIndex*/ = -1) override
    {
        int pad = mapNoteToPad(note);
        if (pad < 0) return;

        int vi = mapping.voiceForPad(pad);
        if (vi < 0 || vi >= NUM_VOICES) return;

        applyChokeGroups(vi);
        // For the router KEY is the drum's place in the kit: -1 at the first voice, +1 at the last.
        // The strike itself is at the drum's next sub-block, where the router has been read.
        voices[vi].trigger(routerFor(vi), vi * 8, velocity);
        panStruck[(size_t) vi] = true;
        padHeld[(size_t) vi] = true;
        // Per-pad LFO: retrigger on a note-on if its trigger mode calls for it (KEY / ONE / HALF / HOLD).
        {
            auto& pm = padMod[(size_t) vi];
            if (pm.active && ! mod::Router::triggeredByInput(pm.trigger))
                pm.lfo.trigger();
        }
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
    // harder (DRIV +50). Not smoothed here: a drum reads its decay at the strike, which is at its
    // next sub-block, after the router has been given the strip as it is (beginBlock).
    void modWheel(float value) override
    {
        wheel = std::clamp(value, 0.0f, 1.0f);
    }

    void setParameter(ParamId id, float value) override
    {
        // The router's pages (LFO1, LFO2, STRIP, XIN, the slots: ids 158-189, 700-772) belong to
        // the whole kit.
        if (mod::Router::owns(id))
        {
            router.set(id, value);
            // The kit's pressure route (AT ROUTE's one column on a drum track) reaches every drum.
            if (PressureSlotParam::contains(id) && id < PressureSlotParam::id(1, 0))
                applyKitPressure();
            return;
        }

        // Per-pad modulation (ids 4000-4255, PadModParam).
        if (isPadModParam(id))
        {
            setPadModParam(id, value);
            return;
        }

        // (What is no number is not taken: a clamp lets it through, and one pad's level then
        // silences the whole kit and everything after it.)
        if (! std::isfinite(value))
            return;

        // Each pad's WAVE (ids 128-143).
        if (isWaveParam(id))
        {
            auto& v = voices[(size_t) (id - WAVE_BASE)];
            int wave = std::clamp((int) std::lround(value), 0, phaseosc::waveCount() - 1);
            if (wave == phaseosc::WAVE_CYCLES)
                wave = phaseosc::WAVE_SINE;     // (not offered: what it played; see waveNames)
            if (wave != v.getWave() && v.isActive() && v.readsWave())
                soundSwapped();     // another wave takes over a hit that is ringing
            v.setWave(wave);
            return;
        }

        // The kick-machine page of each pad (ids 400-527).
        if (isKickParam(id))
        {
            const int voice = (id - KICK_BASE) / DrumVoice::NumKickParams, param = (id - KICK_BASE) % DrumVoice::NumKickParams;
            value = kickSpec(voice, param).clamp(value);
            voices[voice].setKickParam(param, value);
            kickValues[(size_t) (id - KICK_BASE)] = value;
            return;
        }

        // The sound page of each pad (ids 528-655): the bank's cells on a BANK pad, the kick
        // machine's second page (KCK2) on a kick, the resonator drum's on any other (heard on the
        // pads whose TYPE is RSN).
        if (isSoundParam(id))
        {
            const int voice = (id - SOUND_BASE) / SOUND_CELLS, param = (id - SOUND_BASE) % SOUND_CELLS;
            setSoundCell(voice, param, value);
            if (param == SOUND_CELLS - 1)
                restoreSavedKick(voice);
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
                // (FMKK, KICK and MKIK are the kick machine at a point: turned to one of them, the
                // pad's KICK and KCK2 pages start from that point; its DR cells stay as they are.)
                // [FOLD] What comes here may be any of the eighteen stored types (a file's name, a
                // preset's or a step lock's number: nothing between a stored value and this line
                // holds it to the cell's ten). What it is now is CymbalPresets::typeNow, the rule
                // a kit goes by too: the voice never plays a retired type's old code, and the cell
                // holds what it offers (RSN for a stored SNAR, BANK for a stored hat).
                const auto type = static_cast<DrumSynthType>(std::clamp((int) std::lround(value), 0, NUM_STORED_TYPES - 1));
                const auto now = CymbalPresets::typeNow(type);
                const auto was = static_cast<DrumSynthType>(typeGiven[(size_t) voiceIdx]);
                const bool changes = was != type || ! sameVoiceType(v.getType(), type);
                if (v.isActive() && changes)
                    soundSwapped();     // another drum takes over a hit that is ringing
                v.setType(now.voice);
                typeGiven[(size_t) voiceIdx] = (int) type;
                value = (float) (int) now.cell;
                paramValues[(size_t) id] = value;
                // (The pad's sound page belongs to the type: it starts from that type's own.)
                if (changes)
                {
                    resetSoundPage(voiceIdx);
                    if (DrumVoice::isKickPoint(type))
                        applyKickPoint(voiceIdx, DrumVoice::kickPointOf(type), false);
                    else if (type == DrumSynthType::KickSynth && DrumVoice::isKickPoint(was))
                        applyKickPoint(voiceIdx, DrumVoice::kickPointOf(type), false);     // (from a point: its own cells; else they stay)
                    // [FOLD] A type that is now a resonator drum sound: load that sound. A hat:
                    // the bank at that hat.
                    else if (now.sound != nullptr)
                        setParameter(SOUND_BASE + voiceIdx * SOUND_CELLS + RsnSound, (float) resonatorSoundIndex(now.sound));
                    else if (now.model >= 0)
                        setSoundCell(voiceIdx, OscBank::ParModel, (float) now.model);
                }
                break;
            }
        }
        paramValues[(size_t) id] = value;
    }

    // The true current value of every voice parameter.
    float getParameter(ParamId id) const override
    {
        if (mod::Router::owns(id))
            return router.get(id);
        if (isPadModParam(id))
            return getPadModParam(id);
        if (isWaveParam(id))
            return (float) voices[(size_t) (id - WAVE_BASE)].getWave();
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

    // What TYPE offers (DrumVoice.h says what became of the rest), and how many types a stored
    // number may be (CymbalPresets::typeNow says what each is now).
    static constexpr int NUM_DRUM_TYPES = DrumVoice::NUM_OFFERED;
    static constexpr int NUM_STORED_TYPES = (int) DrumSynthType::OpenHat + 1;

    // SND is a whole sound set at once (some forty cells of the voice): no work for a step lock.
    bool isRebuildParam(ParamId id) const override
    {
        return isSoundParam(id) && (id - SOUND_BASE) % SOUND_CELLS == 0 && isResonator((id - SOUND_BASE) / SOUND_CELLS);
    }

    // ---- The kick machine's page (type KSYN): eight more cells per pad, ids 400 + pad * 8 + n ------
    static constexpr int KICK_BASE = EngineParamRange::THIRD_BASE;
    static bool isKickParam(int id) { return id >= KICK_BASE && id < KICK_BASE + NUM_VOICES * DrumVoice::NumKickParams; }

    static ParamSpec kickSpec(int voice, int param)
    {
        auto L = ParamKind::Linear; auto G = ParamKind::Log;
        const int b = KICK_BASE + voice * DrumVoice::NumKickParams;
        const float d = DrumVoice::kickDefault(param);
        switch (param)
        {
            case DrumVoice::KickSweep:      return { "SWEP", b + param, 0, 72, d, L, "st" };
            case DrumVoice::KickSweepTime:  return { "SWPT", b + param, 0.002f, 1, d, G, "s" };
            case DrumVoice::KickSweepCurve: return { "SCRV", b + param, -1, 1, d, L };
            case DrumVoice::KickShape:      return { "SHAP", b + param, 0, 1, d, L };
            case DrumVoice::KickKnock:      return { "KNCK", b + param, 0, 1, d, L };
            case DrumVoice::KickKnockTone:  return { "KTON", b + param, 0, 1, d, L };
            case DrumVoice::KickHold:       return { "HOLD", b + param, 0, 0.3f, d, L, "s", nullptr, 0, 0.005f };
            default:                        return { "ACRV", b + param, -1, 1, d, L };
        }
    }

    // What the kit's router can move (DrumVoice::ModDest says what each is): the original seven,
    // then the kick machine's eight cells (each drum moves its own; heard on the pads whose TYPE
    // is KSYN), LRAT / LDPT, the resonator drum's 24 (each drum its own; heard on the pads whose
    // TYPE is RSN), then, new with the router, PM and WPOS on the drum's oscillator and the two
    // envelope times. A DEST cell stores an index: what was in the list keeps its place.
    // Per sample (so the track input reaches them at audio rate): PTCH, TONE, DRIV, AMP, PAN, PM,
    // WPOS. Read at the strike: DEC, SNAP and the kick machine's cells but SWEP, SHAP and KTON.
    // Not offered: TYPE, WAVE, SND (choices); TUNE and LEVL are PTCH and AMP.
    static const mod::DestTable& lfoDests()
    {
        static const mod::DestTable table = []
        {
            using V = DrumVoice;
            mod::DestTable t;
            t.group();
            t.input("PTCH", 1.0f, 1.0f, 1.0f / 1024.0f, 1024.0f, mod::Pitch);     // a ratio: one octave x the amount squared
            t.input("DEC", 1.0f, 2.0f, 1.0f / 256.0f, 256.0f, mod::Exp);         // a factor: +-2 octaves of time
            t.input("TONE", 0.0f, 1.0f, -1.0f, 1.0f, mod::PerSample);            // added to the cell, then held to 0..1
            t.input("DRIV", 0.0f, 1.0f, -1.0f, 1.0f, mod::PerSample);
            t.input("SNAP", 0.0f, 1.0f, -1.0f, 1.0f);
            t.input("AMP", 1.0f, 1.0f, 0.0f, 2.0f, mod::PerSample).audioFloor(-2.0f);  // (below 0: ring modulation)
            t.input("PAN", 0.0f, 0.5f, -0.5f, 0.5f, mod::PerSample);
            jassert(t.size() == V::ModKickFirst);
            t.group();
            for (int n = 0; n < DrumVoice::NumKickParams; ++n)
                t.add(kickSpec(0, n));
            t.addCross();
            jassert(t.indexOfLabel("LRAT") == V::ModLfoRate && t.indexOfLabel("LDPT") == V::ModLfoDepth);
            // [U2b] The resonator drum's cells (DrumVoice::rsnDestCells: the same order).
            {
                using R = ResonatorEngine;
                const auto& cells = V::rsnDestCells();
                int d = 0;
                t.group();
                for (int id : { (int) R::MorphParam, (int) R::ExciterMorphParam, (int) R::EnergyParam })
                {
                    jassert(R::voiceCell(id) == cells[(size_t) d]);
                    t.add(R::cellSpec(id));
                    ++d;
                }
                t.group();
                for (int i = 0; i < res::NumBody; ++i, ++d)
                    t.add(R::cellSpec(R::BodyBase + i));
                t.group();
                for (int i = 0; i < res::NumMore; ++i, ++d)
                    t.add(R::cellSpec(R::MoreBase + i));
                t.group();
                for (int i = 0; i < res::NumExcite; ++i, ++d)
                    t.add(R::cellSpec(R::ExciteBase + i));
                juce::ignoreUnused(cells, d);
            }
            jassert(t.size() == V::ModPhase);
            // New with the router (after what was there before: a stored DEST keeps its meaning).
            t.group();
            t.input("PM", 0.0f, 6.2831853f, -V::PHASE_LIMIT, V::PHASE_LIMIT, mod::PerSample | mod::Phase).period(6.2831853f);    // radians: a cycle either way at full depth (a phase: U4-1)
            t.input("WPOS", 0.0f, 1.0f, 0.0f, 1.0f, mod::PerSample);
            t.group();
            t.envTimes();
            jassert(t.ampTime() == V::ModEnvTime && t.filterTime() == V::ModFilterTime);
            jassert(t.size() == V::NumModDests);
            // The mod strip until the STRIP page says otherwise (see modWheel). DEC moves by
            // octaves of time: 2 ^ (2 x 0.7925) = 3.
            t.stripRoute(0, V::ModDecay, 0.7925f);
            t.stripRoute(1, V::ModDrive, 0.5f);
            // Pad pressure until AT ROUTE says otherwise: squeezing a pad lengthens its sound.
            t.pressureRoute(0, V::ModDecay, 1.0f);
            t.finish(2);
            return t;
        }();
        static const bool registered = (table.registerGroups(), true);
        juce::ignoreUnused(registered);
        return table;
    }

    // ---- Each pad's WAVE: the wave its oscillator reads, ids 128 + pad -------------------------
    // On two pages of the kit, WAV1 (pads 1-8, the top row) and WAV2 (pads 9-16), before the
    // router's pages. (The pad's own pages are full; a pad's DR page could take it in format 3.)
    // What a W cell offers is phaseosc::waveLabels() without CYCL: a drum has no sample whose
    // cycles it could read (there is no cell to choose one with: a pad's pages are full), and the
    // choice played the sine. The list keeps its place (the stored indices do not move): the label
    // at WAVE_CYCLES is empty, which a knob passes over (ParamSpec::turned), and a sound stored
    // with it loads as SINE, which is what it played.
    static constexpr int WAVE_BASE = 128;
    static bool isWaveParam(int id) { return id >= WAVE_BASE && id < WAVE_BASE + NUM_VOICES; }
    static const char* const* waveNames()
    {
        static const std::vector<const char*> names = []
        {
            std::vector<const char*> n(phaseosc::waveLabels(), phaseosc::waveLabels() + phaseosc::waveCount());
            n[(size_t) phaseosc::WAVE_CYCLES] = "";
            return n;
        }();
        return names.data();
    }
    static ParamSpec waveSpec(int voice)
    {
        static const char* const labels[NUM_VOICES] = { "W1", "W2", "W3", "W4", "W5", "W6", "W7", "W8",
                                                         "W9", "W10", "W11", "W12", "W13", "W14", "W15", "W16" };
        const int v = std::clamp(voice, 0, NUM_VOICES - 1);
        return { labels[v], WAVE_BASE + v, 0, (float) (phaseosc::waveCount() - 1), 0, ParamKind::Choice, "",
                 waveNames(), phaseosc::waveCount() };
    }
    static ParamPage wavePage(int row)
    {
        ParamPage p;
        p.name = row == 0 ? "WAV1" : "WAV2";
        p.section = "WAVE";
        for (int n = 0; n < 8; ++n)
            p.params[(size_t) n] = waveSpec(row * 8 + n);
        return p;
    }

    // ---- Per-pad modulation: LFO and slots, ids 4000 + pad * 16 + cell (PadModParam) ----------------
    // Each pad has a per-pad LFO page (PLFO, 8 cells) and a per-pad slot page (PSLT, 8 cells = 2
    // slots of 4 each). The pages appear on PARAM and DRUMS with the pad's other pages.
    static bool isPadModParam(int id) { return PadModParam::contains(id); }

    // What a pad's DEST cells offer: the kit's table, entry for entry (a DEST stores the index),
    // without what a pad's own modulation cannot move (DrumVoice::padDestActs): those places are
    // kept with an empty label, which a knob passes over (ParamSpec::turned), and the cells of the
    // track's effects after the table are not there at all. A stored value that is none of these
    // did nothing and is taken as OFF (setPadModParam).
    static int padDestCount() { return lfoDests().visible(); }
    static const char* const* padDestLabels()
    {
        static const std::vector<const char*> names = []
        {
            const auto& table = lfoDests();
            std::vector<const char*> n(table.labels(), table.labels() + table.visible());
            for (int d = 1; d < (int) n.size(); ++d)
                if (! DrumVoice::padDestActs(d))
                    n[(size_t) d] = "";
            return n;
        }();
        return names.data();
    }

    // The per-pad LFO page (8 cells): RATE DPTH SHPE DEST TRIG SYNC DIV FADE.
    ParamPage padLfoPage(int voice) const
    {
        auto C = ParamKind::Choice; auto L = ParamKind::Linear; auto G = ParamKind::Log;
        const int b = PadModParam::id(voice, 0);
        const int numDests = padDestCount();
        const int numShapes = (int) LFO::Shape::COUNT;
        ParamPage p;
        p.name = "PLFO";
        p.section = "PAD MOD";
        p.params = {{
            { "RATE", b + PadModParam::LFO_RATE, mod::Router::MIN_RATE, mod::Router::MAX_RATE, 2.0f, G, "Hz" },
            { "DPTH", b + PadModParam::LFO_DEPTH, -1.0f, 1.0f, 0.0f, L },
            { "SHPE", b + PadModParam::LFO_SHAPE, 0, (float) (numShapes - 1), 0, C, "", LFO::shapeLabels(), numShapes },
            { "DEST", b + PadModParam::LFO_DEST, 0, (float) (numDests - 1), 0, C, "", padDestLabels(), numDests },
            { "TRIG", b + PadModParam::LFO_TRIGGER, 0, (float) (mod::Router::NUM_TRIGGERS - 1), 0, C, "", mod::Router::triggerLabels(), mod::Router::NUM_TRIGGERS },
            { "SYNC", b + PadModParam::LFO_SYNC, 0, 1, 0, C, "", ParamChoices::offOn, 2 },
            { "DIV",  b + PadModParam::LFO_DIV, 0, (float) (LFO::NUM_DIVISIONS - 1), (float) LFO::DEFAULT_DIVISION, C, "", LFO::divisionLabels(), LFO::NUM_DIVISIONS },
            { "FADE", b + PadModParam::LFO_FADE, 0, mod::Router::MAX_FADE, 0, L, "s" }
        }};
        return p;
    }

    // The per-pad slot page (8 cells): SRC1 DST1 AMT1 VIA1 SRC2 DST2 AMT2 VIA2.
    ParamPage padSlotPage(int voice) const
    {
        auto C = ParamKind::Choice; auto L = ParamKind::Linear;
        const int b = PadModParam::id(voice, 0);
        const int numDests = padDestCount();
        const int numSources = router.numSources();
        const char* const* srcLabels = router.sources().labels();
        const char* const* dstLabels = padDestLabels();
        ParamPage p;
        p.name = "PSLT";
        p.section = "PAD MOD";
        p.params = {{
            { "SRC1", b + PadModParam::SLOT1_SRC, 0, (float) (numSources - 1), 0, C, "", srcLabels, numSources },
            { "DST1", b + PadModParam::SLOT1_DEST, 0, (float) (numDests - 1), 0, C, "", dstLabels, numDests },
            { "AMT1", b + PadModParam::SLOT1_AMT, -1.0f, 1.0f, 0.0f, L },
            { "VIA1", b + PadModParam::SLOT1_VIA, 0, (float) (numSources - 1), 0, C, "", srcLabels, numSources },
            { "SRC2", b + PadModParam::SLOT2_SRC, 0, (float) (numSources - 1), 0, C, "", srcLabels, numSources },
            { "DST2", b + PadModParam::SLOT2_DEST, 0, (float) (numDests - 1), 0, C, "", dstLabels, numDests },
            { "AMT2", b + PadModParam::SLOT2_AMT, -1.0f, 1.0f, 0.0f, L },
            { "VIA2", b + PadModParam::SLOT2_VIA, 0, (float) (numSources - 1), 0, C, "", srcLabels, numSources }
        }};
        return p;
    }

    // A cell of PLFO or PSLT that cannot act as the pad stands: its destination is OFF. What the
    // screens say when it is turned ("SET DEST"); nullptr = it acts.
    const char* padModIdleReason(int id) const
    {
        if (! PadModParam::contains(id)) return nullptr;
        const int pad = PadModParam::padOf(id), cell = PadModParam::cellOf(id);
        auto off = [&](int destCell) { return (int) std::lround(getPadModParam(PadModParam::id(pad, destCell))) == 0; };
        if (cell == PadModParam::LFO_RATE || cell == PadModParam::LFO_DEPTH || cell == PadModParam::LFO_FADE)
            return off(PadModParam::LFO_DEST) ? "SET DEST" : nullptr;
        if (cell == PadModParam::SLOT1_AMT) return off(PadModParam::SLOT1_DEST) ? "SET DST1" : nullptr;
        if (cell == PadModParam::SLOT2_AMT) return off(PadModParam::SLOT2_DEST) ? "SET DST2" : nullptr;
        return nullptr;
    }

    // A pad's own knob pages, in the order DRUMS and PARAM show them: its DR page, what its type
    // adds (a kick: KICK and KCK2; a resonator drum: RSN; an oscillator bank: BANK), then its
    // own modulation, PLFO and PSLT. Every page has eight cells.
    std::vector<ParamPage> padPages(int voice) const
    {
        std::vector<ParamPage> out;
        out.push_back(voicePage(voice));
        if (isKickMachine(voice)) out.push_back(kickPage(voice));
        if (hasSoundPage(voice))  out.push_back(soundPage(voice));
        out.push_back(padLfoPage(voice));
        out.push_back(padSlotPage(voice));
        return out;
    }

    // ---- The sound page of a pad: eight more cells, ids 528 + pad * 8 + n -------------------------
    // What they are depends on the pad's TYPE: the oscillator bank's cells (BANK), or the
    // resonator drum's (RSN). A pad that changes its TYPE starts them from the new type's own.
    static constexpr int SOUND_BASE = KICK_BASE + NUM_VOICES * DrumVoice::NumKickParams;
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
    // The kick machine's second page, KCK2 (on its pads' sound page ids; DrumVoice::Kick2Param says
    // what each does). Every cell at 0 is the kick machine as it was.
    static const char* const* kickKindNames()
    {
        static const char* const names[DrumVoice::NumKickKinds] = { "TICK", "FM" };
        return names;
    }
    static const char* const* kickPathNames()
    {
        static const char* const names[DrumVoice::NumKickPaths] = { "KSYN", "DRUM" };
        return names;
    }
    static ParamSpec kick2Spec(int voice, int param)
    {
        auto L = ParamKind::Linear; auto C = ParamKind::Choice;
        const int b = SOUND_BASE + voice * SOUND_CELLS;
        switch (param)
        {
            case DrumVoice::KickKind:      return { "KIND", b + param, 0, DrumVoice::NumKickKinds - 1, 0, C, "", kickKindNames(), DrumVoice::NumKickKinds };
            case DrumVoice::KickLin:       return { "LIN",  b + param, 0, 1, 0, L };
            case DrumVoice::KickRes:       return { "RES",  b + param, 0, 1, 0, L };
            case DrumVoice::KickFiltSweep: return { "LPSW", b + param, 0, DrumVoice::KICK_SWEEP_OPEN, 0, L, "x" };
            case DrumVoice::KickFiltTrack: return { "LPKT", b + param, 0, 1, 0, L };
            case DrumVoice::KickSat:       return { "SAT",  b + param, 0, 1, 0, L };
            case DrumVoice::KickEdge:      return { "EDGE", b + param, -1, 1, 0, L };
            default:                       return { "PATH", b + param, 0, DrumVoice::NumKickPaths - 1, 0, C, "", kickPathNames(), DrumVoice::NumKickPaths };
        }
    }
    static ParamPage kick2Page(int voice)
    {
        ParamPage p;
        p.name = "KCK2";
        p.section = "KICK";
        for (int n = 0; n < SOUND_CELLS; ++n)
            p.params[(size_t) n] = kick2Spec(voice, n);
        return p;
    }

    // A pad whose TYPE is BANK or RSN has a sound page; a kick (KSYN and its points) has KCK2 there.
    bool hasSoundPage(int voice) const { return isBank(voice) || isResonator(voice) || isKickMachine(voice); }
    ParamPage soundPage(int voice) const
    {
        return isBank(voice) ? bankPage(voice) : (isKickMachine(voice) ? kick2Page(voice) : resonatorPage(voice));
    }
    // One cell of it (no page is made: a page has names, and names are allocated - this is what
    // setParameter reads, which a step lock calls on the audio thread).
    ParamSpec soundSpec(int voice, int param) const
    {
        return isBank(voice) ? bankSpec(voice, param) : (isKickMachine(voice) ? kick2Spec(voice, param) : resonatorSpec(voice, param));
    }

    // The pages each pad has in getParamPages(): DR, KICK, SOUND, PLFO, PSLT.
    static constexpr int PAGES_PER_PAD = 5;

    static ParamPage kickPage(int voice)
    {
        ParamPage p;
        p.name = "KICK";
        p.section = "KICK";
        for (int n = 0; n < DrumVoice::NumKickParams; ++n)
            p.params[(size_t) n] = kickSpec(voice, n);
        return p;
    }

    bool isKickMachine(int voice) const
    {
        return voice >= 0 && voice < NUM_VOICES && voices[voice].getType() == DrumSynthType::KickSynth;
    }

    static const char* const* typeNames()
    {
        // [FOLD] TOM, SNAR, FMSN and CSYN are no longer offered; the first ten are what the TYPE
        // cell shows (stored indices keep their meaning).
        static const char* const names[NUM_DRUM_TYPES] = {
            "FMKK", "KICK", "MKIK", "CLAP", "SHKR", "SUB", "KSYN", "CYMB", "RSN", "BANK" };
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

    mod::Router* modRouter() override { return &router; }
    const mod::Router* modRouter() const override { return &router; }

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
        // Per-pad modulation: a per-pad LFO (PLFO) and two per-pad slots (PSLT) for each pad.
        for (int v = 0; v < NUM_VOICES; ++v)
            out.push_back(padLfoPage(v));
        for (int v = 0; v < NUM_VOICES; ++v)
            out.push_back(padSlotPage(v));
        // Each pad's WAVE, then the router's pages (XIN, SLT1 .., LSHP, STRIP, LFO1, LFO2: last).
        out.push_back(wavePage(0));
        out.push_back(wavePage(1));
        router.appendPages(out);
        return out;
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (mod::Router::owns(id))
            return mod::Router::name(id);
        if (isWaveParam(id))
            return "WV";
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

    DrumVoice& getVoice(int index) { return voices[index]; }
    const DrumVoice& getVoice(int index) const { return voices[index]; }

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

    // ---- The kit's pressure route ---------------------------------------------------------------
    // Pressure slot 1 of the kit's router (PressureSlotParam: AT ROUTE shows a drum track that one
    // column) goes to every pad. Aimed at one of a drum's seven own destinations it is the drum's
    // DrumVoiceATDest, with the drum's own law and a depth of 0..1 (BIPOLAR means nothing there);
    // aimed at anything else of the kit's table (a kick-page cell, VIB, an effect's cell) it is a
    // slot of the kit's router like an LFO's.
    static mod::Mask ownPressureDests()
    {
        using V = DrumVoice;
        mod::Mask m;
        for (int d : { (int) V::ModPitch, (int) V::ModDecay, (int) V::ModTone, (int) V::ModDrive, (int) V::ModSnap, (int) V::ModAmp, (int) V::ModPan })
            m.set(d);
        return m;
    }
    static DrumATDest kitPressureDest(int dest)
    {
        using V = DrumVoice;
        switch (dest)
        {
            case V::ModPitch:   return DrumATDest::Pitch;
            case V::ModDecay:   return DrumATDest::Decay;
            case V::ModTone:    return DrumATDest::Filter;
            case V::ModAmp:     return DrumATDest::Level;
            case V::ModDrive:   return DrumATDest::Drive;
            case V::ModSnap:    return DrumATDest::Snap;
            case V::ModPan:     return DrumATDest::Pan;
            default:            return DrumATDest::Off;
        }
    }
    void applyKitPressure()
    {
        const auto slot = router.pressureSlot(0);
        const DrumATDest dest = router.pressureIsOwn(0) ? kitPressureDest(slot.dest) : DrumATDest::Off;
        for (int s = 0; s < NUM_VOICES; ++s)
        {
            setSlotAftertouchDest(s, dest);
            setSlotAftertouchDepth(s, std::clamp(slot.amount, 0.0f, 1.0f));
        }
    }
    // What an old pressure route (AftertouchRouter::Destination) is on a drum kit, as a slot's DEST.
    int pressureDestOf(AftertouchRouter::Destination d) const override
    {
        using D = AftertouchRouter::Destination;
        using V = DrumVoice;
        switch (d)
        {
            case D::DrumPitch:      return V::ModPitch;
            case D::DrumDecay:      return V::ModDecay;
            case D::FilterCutoff:   return V::ModTone;
            case D::Volume:         return V::ModAmp;
            case D::DrumDrive:      return V::ModDrive;
            case D::DrumSnap:       return V::ModSnap;
            case D::PanSpread:      return V::ModPan;
            default:                return 0;
        }
    }
    // (A kit that was handed no routes: squeezing a pad lengthens the sound.)
    AftertouchRouter legacyPressureRoutes() const override
    {
        AftertouchRouter kit;
        kit.clear();
        kit.setRoute(0, { AftertouchRouter::Destination::DrumDecay, 1.0f, false });
        return kit;
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
            case DrumSynthType::NoiseSnare:    return "RS";   // [FOLD] folded to resonator drum
            case DrumSynthType::FMSnare:       return "RS";   // [FOLD]
            case DrumSynthType::Clap:          return "CP";
            case DrumSynthType::ClosedHat:     return "CH";
            case DrumSynthType::OpenHat:       return "OH";
            case DrumSynthType::Cymbal:        return "CY";
            case DrumSynthType::Tom:           return "RS";   // [FOLD]
            case DrumSynthType::Rim:           return "RM";
            case DrumSynthType::Shaker:        return "SH";
            case DrumSynthType::Clave:         return "CL";
            case DrumSynthType::SubBass:       return "SB";
            case DrumSynthType::KickSynth:     return "KS";
            case DrumSynthType::CymbalSynth:   return "RS";   // [FOLD]
            case DrumSynthType::Resonator:     return "RS";
            case DrumSynthType::OscBank:       return "BK";
        }
        return "--";
    }

private:
    std::array<DrumVoice, NUM_VOICES> voices;
    // The kit's modulation router: one for the whole kit (its LFO, STRIP, XIN and slot cells),
    // each drum its own mod::Voice. Each pad also has its own LFO and two free slots (PadMod,
    // PadModParam): a brushed snare can carry its own stirring LFO while the kit's LFOs move the
    // whole kit. The per-pad modulation is applied additively on top of the kit-wide modulation,
    // at control rate (once per sub-block), in processBlock.
    mod::Router router { lfoDests(), 2 };

    // ---- Per-pad modulation state (U5) ----------------------------------------------------------
    struct PadSlot
    {
        int src = 0, dest = 0, via = 0;
        float amount = 0.0f;
    };
    struct PadMod
    {
        LFO lfo;
        float rate = 2.0f, depth = 0.0f, fade = 0.0f;
        int shape = 0, dest = 0, trigger = 0, division = LFO::DEFAULT_DIVISION;
        bool sync = false;
        PadSlot slots[2] {};
        float lfoValue = 0.0f;         // the LFO's output this sub-block (-1..1 with fade)
        bool active = false;            // anything to apply?
    };
    std::array<PadMod, NUM_VOICES> padMod {};

    void initPadMods()
    {
        for (int v = 0; v < NUM_VOICES; ++v)
        {
            padMod[(size_t) v].lfo.setSampleRate(sr);
            padMod[(size_t) v].lfo.setShape((LFO::Shape) 0);
            padMod[(size_t) v].lfo.setRate(2.0f);
        }
    }

    void setPadModParam(int id, float value)
    {
        if (! PadModParam::contains(id)) return;
        if (! std::isfinite(value)) value = 0.0f;
        const int pad = PadModParam::padOf(id);
        const int cell = PadModParam::cellOf(id);
        if (pad < 0 || pad >= NUM_VOICES) return;
        auto& pm = padMod[(size_t) pad];
        const int numSrcs = router.numSources();
        // (A DEST the pad cannot move - LRAT, LDPT, PM, WPOS, a cell of the track's effects - never
        // did anything: it is OFF, and the cell never holds what it does not offer.)
        const auto destOf = [](float v)
        {
            const int d = (int) std::lround(std::clamp(v, 0.0f, 100000.0f));
            return DrumVoice::padDestActs(d) ? d : 0;
        };
        switch (cell)
        {
            case PadModParam::LFO_RATE:    pm.rate = std::clamp(value, mod::Router::MIN_RATE, mod::Router::MAX_RATE); pm.lfo.setRate(pm.rate); break;
            case PadModParam::LFO_DEPTH:   pm.depth = std::clamp(value, -1.0f, 1.0f); break;
            case PadModParam::LFO_SHAPE:   pm.shape = std::clamp((int) std::lround(value), 0, (int) LFO::Shape::COUNT - 1); pm.lfo.setShape((LFO::Shape) pm.shape); break;
            case PadModParam::LFO_DEST:    pm.dest = destOf(value); break;
            case PadModParam::LFO_TRIGGER: pm.trigger = std::clamp((int) std::lround(value), 0, mod::Router::NUM_TRIGGERS - 1); pm.lfo.setTriggerMode((LFO::TriggerMode) mod::Router::lfoMode(pm.trigger)); break;
            case PadModParam::LFO_SYNC:    pm.sync = value >= 0.5f; pm.lfo.setTempoSync(pm.sync); break;
            case PadModParam::LFO_DIV:     pm.division = std::clamp((int) std::lround(value), 0, LFO::NUM_DIVISIONS - 1); pm.lfo.setSyncDivision(pm.division); break;
            case PadModParam::LFO_FADE:    pm.fade = std::clamp(value, 0.0f, mod::Router::MAX_FADE); pm.lfo.setFadeTime(pm.fade); break;
            default:
            {
                const int slot = PadModParam::slotOf(cell);
                const int sc = PadModParam::slotCell(cell);
                if (slot < 0 || slot > 1) break;
                auto& s = pm.slots[slot];
                switch (sc)
                {
                    case 0: s.src = std::clamp((int) std::lround(value), 0, numSrcs - 1); break;
                    case 1: s.dest = destOf(value); break;
                    case 2: s.amount = std::clamp(value, -1.0f, 1.0f); break;
                    case 3: s.via = std::clamp((int) std::lround(value), 0, numSrcs - 1); break;
                }
                break;
            }
        }
        resolvePadMod(pad);
    }

    float getPadModParam(int id) const
    {
        if (! PadModParam::contains(id)) return 0.0f;
        const int pad = PadModParam::padOf(id);
        const int cell = PadModParam::cellOf(id);
        if (pad < 0 || pad >= NUM_VOICES) return 0.0f;
        const auto& pm = padMod[(size_t) pad];
        switch (cell)
        {
            case PadModParam::LFO_RATE:    return pm.rate;
            case PadModParam::LFO_DEPTH:   return pm.depth;
            case PadModParam::LFO_SHAPE:   return (float) pm.shape;
            case PadModParam::LFO_DEST:    return (float) pm.dest;
            case PadModParam::LFO_TRIGGER: return (float) pm.trigger;
            case PadModParam::LFO_SYNC:    return pm.sync ? 1.0f : 0.0f;
            case PadModParam::LFO_DIV:     return (float) pm.division;
            case PadModParam::LFO_FADE:    return pm.fade;
            default:
            {
                const int slot = PadModParam::slotOf(cell);
                const int sc = PadModParam::slotCell(cell);
                if (slot < 0 || slot > 1) return 0.0f;
                const auto& s = pm.slots[slot];
                switch (sc)
                {
                    case 0: return (float) s.src;
                    case 1: return (float) s.dest;
                    case 2: return s.amount;
                    case 3: return (float) s.via;
                }
                return 0.0f;
            }
        }
    }

    // Whether a per-pad mod has anything to do (a nonzero depth on the LFO with a destination,
    // or a slot with a nonzero amount and a destination).
    void resolvePadMod(int pad)
    {
        auto& pm = padMod[(size_t) pad];
        pm.active = (pm.depth != 0.0f && pm.dest > 0) || (pm.slots[0].amount != 0.0f && pm.slots[0].dest > 0)
                 || (pm.slots[1].amount != 0.0f && pm.slots[1].dest > 0);
    }

    // Tick one pad's LFO for n samples (called from processBlock's sub-block loop). The LFO
    // runs even when the voice is idle (FREE mode keeps time). Returns the LFO value with fade.
    void tickPadLfo(int pad, int n)
    {
        auto& pm = padMod[(size_t) pad];
        if (pm.sync)
            pm.lfo.setTempo(currentTempo);
        pm.lfo.setDepth(1.0f);
        pm.lfo.setEnabled(true);
        pm.lfoValue = pm.lfo.processBlock(n);    // shape * fade * 1 = the LFO's value -1..1
    }

    // A pad slot's source: the drum's own side of the kit's router as it stands (mod::Voice).
    // srcIndex is a place in the source table (what a SRC or VIA cell stores).
    // NOTE: the router moves a source only while one of ITS slots reads it. VEL, KEY and the
    // randoms are taken at the hit and ONE is always there; ENV, PRES, STRP, the kit's LFOs and
    // the input's sources stand still for a pad's slot unless a slot of the kit reads the same
    // source. That goes when a pad's slots are router slots.
    float padSourceValue(int pad, int srcIndex) const
    {
        const int source = router.sources().source(srcIndex);
        if (source == mod::Src::One)
            return 1.0f;            // (the router writes it into a voice only once a slot of its own is worked out)
        return voices[(size_t) pad].getMod().source(source);
    }

    // The pad's own LFO and two slots for this sub-block: each destination's share (-1..1 for
    // one of them at full depth) into the voice's padModOffsets, which its render adds to what
    // the kit's router made of the destination. A slot is SRC x AMT, x VIA by the router's rule
    // (mod::Voice::viaOf: a two-sided source shifted up, a one-sided one as it is, so it can
    // close the slot).
    void computePadMod(int pad, DrumVoice::PadModOffsets& out) const
    {
        const auto& pm = padMod[(size_t) pad];
        std::fill(std::begin(out.v), std::end(out.v), 0.0f);
        out.active = false;
        if (pm.depth != 0.0f && pm.dest > 0)
        {
            out.v[pm.dest] += pm.depth * pm.lfoValue;
            out.active = true;
        }
        for (const auto& s : pm.slots)
        {
            if (s.amount == 0.0f || s.dest <= 0)
                continue;
            float value = padSourceValue(pad, s.src);
            if (s.via > 0)
                value *= voices[(size_t) pad].getMod().viaOf(router, router.sources().source(s.via));
            out.v[s.dest] += s.amount * value;
            out.active = true;
        }
    }
    // (One router for the kit: every drum is handed the same one.)
    const mod::Router& routerFor(int /*voice*/) const { return router; }
    std::array<float, mod::Router::MAX_CHUNK> mixL {}, mixR {}, voiceOut {}, voicePanMod {};
    std::array<float, NUM_VOICES> voicePan;
    std::array<float, NUM_VOICES * PARAMS_PER_VOICE> paramValues {};
    // The drum type each pad was last given, as it was stored (0..17): the TYPE cell itself holds
    // what it offers (CymbalPresets::typeNow), so a stored SNAR given twice is known for the same.
    std::array<int, NUM_VOICES> typeGiven {};
    std::array<float, NUM_VOICES * DrumVoice::NumKickParams> kickValues = [] {
        std::array<float, NUM_VOICES * DrumVoice::NumKickParams> a {};
        for (size_t i = 0; i < a.size(); ++i) a[i] = DrumVoice::kickDefault((int) (i % DrumVoice::NumKickParams));
        return a;
    }();
    std::array<bool, NUM_VOICES> padHeld {};        // between a pad's (a step's) note on and its note off
    std::array<std::array<float, 7>, NUM_VOICES> kickWritten {};     // the default kit's old kicks as written (its DrumKit)
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
    float currentTempo = 120.0f;

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
                                       sound.driveWire, sound.dec, sound.lofi, sound.driveLofi);
    }

    void applyResonatorCell(int voice, int param, float value)
    {
        if (param == RsnSound)
            applyKitSound(voice, (int) value);
        else
            voices[voice].setResonatorOffset(resonatorCellOf(param), value);
    }

    // One cell of a pad's sound page, as its type reads it.
    void setSoundCell(int voice, int param, float value)
    {
        const bool onBank = isBank(voice);
        const ParamSpec spec = soundSpec(voice, param);
        value = spec.clamp(value);
        if (spec.isDiscrete())
            value = (float) std::lround(value);
        soundValues[(size_t) (voice * SOUND_CELLS + param)] = value;
        if (onBank)
            voices[voice].setBankParam(param, value);
        else if (isKickMachine(voice))
            voices[voice].setKick2Param(param, value);
        else if (isResonator(voice))
            applyResonatorCell(voice, param, value);
    }

    // Two cell values of a TYPE that are the same drum of the voice (the kicks are all the kick machine,
    // the folded types are all the resonator drum).
    static bool sameVoiceType(DrumSynthType voiceType, DrumSynthType type)
    {
        type = CymbalPresets::typeNow(type).cell;       // (a retired type: the resonator drum; a hat: the bank)
        if (DrumVoice::isKickPoint(type)) type = DrumSynthType::KickSynth;
        return voiceType == type;
    }

    // A kick point to a pad: its KICK and KCK2 pages (and, withCells, its DR cells but TUNE and PAN).
    void applyKickPoint(int voice, const DrumVoice::KickPoint& p, bool withCells)
    {
        for (int n = 0; n < DrumVoice::NumKickParams; ++n)
        {
            const float v = kickSpec(voice, n).clamp(p.kick[(size_t) n]);
            voices[voice].setKickParam(n, v);
            kickValues[(size_t) (voice * DrumVoice::NumKickParams + n)] = v;
        }
        for (int n = 0; n < DrumVoice::NumKick2Params; ++n)
            setSoundCell(voice, n, p.kick2[(size_t) n]);
        if (withCells)
        {
            const int b = voice * PARAMS_PER_VOICE;
            setParameter(b + DECAY, p.decay);
            setParameter(b + TONE, p.tone);
            setParameter(b + DRIVE, p.drive);
            setParameter(b + SNAP, p.snap);
            setParameter(b + LEVEL, p.level);
        }
    }

    // A kick saved before the kick machine had a second page: a pad of TYPE FMKK, KICK or MKIK whose
    // KICK page holds the kick machine's own values (what such a pad saved: they did nothing on it)
    // and whose KCK2 page holds 0 (what it saved there). Looked at when the last cell of the page
    // comes, which a saved sound sets after its TYPE and its KICK page (ids in order): the pad is
    // made the kick it was, from its DR cells as they were written for the old type.
    // (A pad turned by hand to exactly that state - every KICK cell at the kick machine's own value,
    // every KCK2 cell at 0, on one of the three old TYPEs - is taken for one too.)
    void restoreSavedKick(int voice)
    {
        const auto type = static_cast<DrumSynthType>((int) paramValues[(size_t) (voice * PARAMS_PER_VOICE + TYPE)]);
        if (! DrumVoice::isKickPoint(type) || ! isKickMachine(voice))
            return;
        for (int n = 0; n < DrumVoice::NumKickParams; ++n)
            if (kickValues[(size_t) (voice * DrumVoice::NumKickParams + n)] != DrumVoice::kickDefault(n))
                return;
        for (int n = 0; n < SOUND_CELLS; ++n)
            if (soundValues[(size_t) (voice * SOUND_CELLS + n)] != 0.0f)
                return;
        const float* pv = &paramValues[(size_t) (voice * PARAMS_PER_VOICE)];
        applyKickPoint(voice, DrumVoice::kickPoint(type, pv[TUNE], pv[DECAY], pv[TONE], pv[DRIVE], pv[SNAP], pv[LEVEL]), true);
    }

    // A pad's sound page from its type's own values.
    void resetSoundPage(int voice)
    {
        const bool onBank = isBank(voice);
        for (int n = 0; n < SOUND_CELLS; ++n)       // (cell by cell: a TYPE locked on a step comes here on the audio thread)
            setSoundCell(voice, n, soundSpec(voice, n).defaultVal);
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
    void configureBank(int index, int model, const DrumKitVoiceConfig& cfg, float pan, int choke = 0)
    {
        configureVoice(index, cfg, pan, choke);
        resetSoundPage(index);
        setParameter(SOUND_BASE + index * SOUND_CELLS + OscBank::ParModel, (float) model);
    }

    void configureVoice(int index, const DrumKitVoiceConfig& cfg, float pan = 0.5f, int choke = 0)
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
        typeGiven[(size_t) index] = (int) cfg.type;
        jassert((int) cfg.type < NUM_DRUM_TYPES);       // (the built-in kit is written with offered types)
    }

    // A pad as one of the old kicks, written with that type's cells: the kick machine at that kick
    // (DrumVoice::kickPoint). TYPE keeps the old kick's number.
    void configureKick(int index, const DrumKitVoiceConfig& cfg, float pan)
    {
        const auto p = DrumVoice::kickPoint(cfg.type, cfg.tune, cfg.decay, cfg.tone, cfg.drive, cfg.snap, cfg.level);
        configureVoice(index, { cfg.type, p.tune, p.decay, p.tone, p.drive, p.snap, p.level }, pan);
        resetSoundPage(index);
        applyKickPoint(index, p, false);
        kickWritten[(size_t) index] = { cfg.tune, cfg.decay, cfg.tone, cfg.drive, cfg.snap, cfg.level, pan };
    }

    // Apply the current kit configuration to all voices
    void applyKitToVoices()
    {
        for (int i = 0; i < NUM_VOICES; ++i)
        {
            const auto& slot = currentKit.slots[static_cast<size_t>(i)];
            int vi = mapping.voiceForPad(i);
            if (vi < 0 || vi >= NUM_VOICES) continue;

            // (A drum type that is not offered any more is a sound of the resonator drum: the
            // rule the TYPE cell goes by, CymbalPresets::typeNow.)
            const auto now = CymbalPresets::typeNow(slot.synthType);
            const char* folded = now.sound;
            const float slotDecay = CymbalPresets::foldedDecay(slot.synthType, slot.params[1]);
            voices[vi].setType(now.voice);
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
            pv[TYPE] = (float) (int) now.cell;
            // (What the pad was last given, as the TYPE cell compares it: as it always was here.)
            typeGiven[(size_t) vi] = folded != nullptr ? (int) voices[vi].getType() : (int) slot.synthType;
            resetSoundPage(vi);
            if (folded != nullptr)
                setParameter(SOUND_BASE + vi * SOUND_CELLS + RsnSound, (float) resonatorSoundIndex(folded));
            else if (now.model >= 0)
                setSoundCell(vi, OscBank::ParModel, (float) now.model);
            // (A kick written as one of the old kick types: the kick machine at that kick.)
            if (DrumVoice::isKickPoint(slot.synthType))
                applyKickPoint(vi, DrumVoice::kickPoint(slot.synthType, slot.params[0], slot.params[1], slot.params[2],
                                                        slot.params[3], slot.params[4], slot.params[5]), true);

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
        // The lower left pad is the kick machine (its KICK page starts from DrumVoice::kickDefault).
        // (Pads 9 and 10 were KICK and MKIK: the kick machine at those kicks, written as they were.)
        configureVoice(8,  { DrumSynthType::KickSynth,    48.0f,  0.5f, 0.7f, 0.2f, 0.7f, 0.9f }, 0.5f);
        configureKick(9,  { DrumSynthType::AnalogKick,    52.0f,  0.6f, 0.5f, 0.0f, 0.5f, 0.9f }, 0.5f);
        configureKick(10, { DrumSynthType::ModCyclesKick, 55.0f,  0.7f, 0.6f, 0.4f, 0.6f, 0.9f }, 0.5f);

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
            slot.synthType = static_cast<DrumSynthType>((int) paramValues[(size_t) (i * PARAMS_PER_VOICE + TYPE)]);
            slot.chokeGroup = chokeGroups[i];
            // The kit description carries the real voice settings, so saving and reloading it
            // gives the same sound (a kick point: the cells it was written with, which loadKit
            // turns into the kick machine's again).
            for (int k = 0; k < 7; ++k)
                slot.params[(size_t) k] = paramValues[(size_t) (i * PARAMS_PER_VOICE + k)];
            if (DrumVoice::isKickPoint(slot.synthType))
                for (int k = 0; k < 7; ++k)
                    slot.params[(size_t) k] = kickWritten[(size_t) i][(size_t) k];
            // Squeezing a pad after the hit lengthens the sound.
            slot.aftertouchDest = DrumATDest::Decay;
            slot.aftertouchDepth = 1.0f;
            voices[i].setAftertouchDest(DrumVoiceATDest::Decay);
            voices[i].setAftertouchDepth(1.0f);
        }
        currentKit.name = "Default";
    }
};
