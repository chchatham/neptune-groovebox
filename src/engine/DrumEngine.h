#pragma once

#include "EngineBase.h"
#include "DrumVoice.h"
#include "../project/DrumKit.h"
#include <array>
#include <atomic>

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
        loadDefaultKit();
        lfoSection.applyAll(EngineLfoSection::each(voices));
    }

    EngineType getType() const override { return EngineType::Drum; }
    juce::String getName() const override { return "Drum"; }

    void prepareToPlay(double sampleRate, int /*samplesPerBlock*/) override
    {
        sr = sampleRate;
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
                float left = sample * std::sqrt(1.0f - pan);
                float right = sample * std::sqrt(pan);
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
        voices[vi].noteOn(velocity);
    }

    void noteOff(int note, int /*voiceIndex*/ = -1) override
    {
        int pad = mapNoteToPad(note);
        if (pad < 0) return;

        int vi = mapping.voiceForPad(pad);
        if (vi < 0 || vi >= NUM_VOICES) return;

        voices[vi].noteOff();
        voices[vi].clearAftertouch();
        voicePanAT[vi] = 0.0f;
    }

    void allNotesOff() override
    {
        for (int i = 0; i < NUM_VOICES; ++i)
        {
            voices[i].reset();
            voicePanAT[i] = 0.0f;
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

        // The kick-machine page of each pad (ids 400-527).
        if (isKickParam(id))
        {
            const int voice = (id - KICK_BASE) / DrumVoice::NumKickParams, param = (id - KICK_BASE) % DrumVoice::NumKickParams;
            value = kickSpec(voice, param).clamp(value);
            voices[voice].setKickParam(param, value);
            kickValues[(size_t) (id - KICK_BASE)] = value;
            return;
        }

        int voiceIdx = id / PARAMS_PER_VOICE;
        int param = id % PARAMS_PER_VOICE;
        if (voiceIdx < 0 || voiceIdx >= NUM_VOICES) return;

        auto& v = voices[voiceIdx];
        switch (param)
        {
            case TUNE:  value = std::max(1.0f, value); v.setTune(value); break;
            case DECAY: value = std::max(0.001f, value); v.setDecay(value); break;
            case TONE:  value = std::clamp(value, 0.0f, 1.0f); v.setTone(value); break;
            case DRIVE: value = std::clamp(value, 0.0f, 1.0f); v.setDrive(value); break;
            case SNAP:  value = std::clamp(value, 0.0f, 1.0f); v.setSnap(value); break;
            case LEVEL: value = std::clamp(value, 0.0f, 1.0f); v.setLevel(value); break;
            case PAN:   value = std::clamp(value, 0.0f, 1.0f); voicePan[voiceIdx] = value; break;
            case TYPE:
                value = (float) std::clamp((int) std::lround(value), 0, NUM_DRUM_TYPES - 1);
                v.setType(static_cast<DrumSynthType>((int) value));
                break;
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

    static constexpr int NUM_DRUM_TYPES = 15;

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

    // The LFO destinations: the original seven, then the kick machine's eight cells (each drum
    // moves its own; they are heard on the pads whose TYPE is KSYN) and the other LFO.
    // Not offered: TYPE (a choice); TUNE and LEVL are the original PTCH and AMP.
    static const LfoDestList& lfoDests()
    {
        static const LfoDestList list = []
        {
            LfoDestList l(DrumVoice::lfoDestLabels(), DrumVoice::NumLfoDests);
            l.group();
            for (int n = 0; n < DrumVoice::NumKickParams; ++n)
                l.add(kickSpec(0, n), nullptr, n);
            l.addCross();
            // The mod strip until the STRIP page says otherwise (see modWheel). DEC moves by
            // octaves of time: 2 ^ (2 x 0.7925) = 3.
            l.stripRoute(0, DrumVoice::LfoDecay, 0.7925f);
            l.stripRoute(1, DrumVoice::LfoDrive, 0.5f);
            return l;
        }();
        static const bool registered = (list.registerGroups(), true);
        juce::ignoreUnused(registered);
        return list;
    }

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
        static const char* const names[NUM_DRUM_TYPES] = {
            "FMKK", "KICK", "MKIK", "SNAR", "FMSN", "CLAP", "HAT", "OHAT", "CYMB", "TOM", "RIM", "SHKR", "CLAV", "SUB", "KSYN" };
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
        lfoSection.appendPages(out);
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
        }
        return "--";
    }

private:
    std::array<DrumVoice, NUM_VOICES> voices;
    EngineLfoSection lfoSection { lfoDests() };
    std::array<float, NUM_VOICES> voicePan;
    std::array<float, NUM_VOICES * PARAMS_PER_VOICE> paramValues {};
    std::array<float, NUM_VOICES * DrumVoice::NumKickParams> kickValues = [] {
        std::array<float, NUM_VOICES * DrumVoice::NumKickParams> a {};
        for (size_t i = 0; i < a.size(); ++i) a[i] = DrumVoice::kickDefault((int) (i % DrumVoice::NumKickParams));
        return a;
    }();
    int selectedVoice = 8;      // the first pad of the bottom row, as on DRUMS
    float wheel = 0.0f;         // the mod strip as sent
    std::array<float, NUM_VOICES> voicePanAT;  // aftertouch pan modulation
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
    }

    // Apply the current kit configuration to all voices
    void applyKitToVoices()
    {
        for (int i = 0; i < NUM_VOICES; ++i)
        {
            const auto& slot = currentKit.slots[static_cast<size_t>(i)];
            int vi = mapping.voiceForPad(i);
            if (vi < 0 || vi >= NUM_VOICES) continue;

            voices[vi].setType(slot.synthType);
            voices[vi].setTune(slot.params[0]);
            voices[vi].setDecay(slot.params[1]);
            voices[vi].setTone(slot.params[2]);
            voices[vi].setDrive(slot.params[3]);
            voices[vi].setSnap(slot.params[4]);
            voices[vi].setLevel(slot.params[5]);
            voicePan[vi] = slot.params[6];
            chokeGroups[vi] = slot.chokeGroup;

            float* pv = &paramValues[(size_t) (vi * PARAMS_PER_VOICE)];
            for (int k = 0; k < 7; ++k)
                pv[k] = slot.params[(size_t) k];
            pv[TYPE] = (float) (int) slot.synthType;

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

        // Top row pads 6-7: Cymbals
        configureVoice(6, { DrumSynthType::Cymbal, 7000.0f, 1.0f, 0.6f, 0.0f, 0.5f, 0.3f }, 0.35f);
        configureVoice(7, { DrumSynthType::Cymbal, 9000.0f, 1.5f, 0.7f, 0.0f, 0.5f, 0.3f }, 0.65f);

        // Bottom row pads 8-10: Three kicks
        // The lower left pad is the kick machine (its KICK page starts from DrumVoice::kickDefault).
        configureVoice(8,  { DrumSynthType::KickSynth,    48.0f,  0.5f, 0.7f, 0.2f, 0.7f, 0.9f }, 0.5f);
        configureVoice(9,  { DrumSynthType::AnalogKick,    52.0f,  0.6f, 0.5f, 0.0f, 0.5f, 0.9f }, 0.5f);
        configureVoice(10, { DrumSynthType::ModCyclesKick, 55.0f,  0.7f, 0.6f, 0.4f, 0.6f, 0.9f }, 0.5f);

        // Bottom row pad 11: Tom
        configureVoice(11, { DrumSynthType::Tom, 100.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.75f }, 0.4f);

        // Bottom row pad 12: Snare
        configureVoice(12, { DrumSynthType::NoiseSnare, 200.0f, 0.2f, 0.5f, 0.3f, 0.6f, 0.8f }, 0.5f);

        // Bottom row pad 13: Clap (granular slow-clap)
        configureVoice(13, { DrumSynthType::Clap, 1200.0f, 0.3f, 0.6f, 0.7f, 0.5f, 1.0f }, 0.55f);

        // Bottom row pads 14-15: Hats (choke group 1)
        configureVoice(14, { DrumSynthType::ClosedHat, 8000.0f, 0.04f, 0.5f, 0.0f, 0.5f, 0.4f }, 0.6f, CHOKE_GROUP_HAT);
        configureVoice(15, { DrumSynthType::OpenHat,   8000.0f, 0.5f,  0.5f, 0.0f, 0.5f, 0.4f }, 0.6f, CHOKE_GROUP_HAT);

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
