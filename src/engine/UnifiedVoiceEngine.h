#pragma once

#include "EngineBase.h"
#include "ArchitecturePreset.h"
#include "VoiceAllocator.h"
#include "UnifiedVoice.h"
#include <array>
#include <vector>

// The parameter ids (namespace UnifiedParam) are in UnifiedVoice.h.
class UnifiedVoiceEngine : public EngineBase
{
public:
    static constexpr int MAX_VOICES = 16;

    UnifiedVoiceEngine(int maxVoices = 8)
        : allocator(juce::jlimit(1, MAX_VOICES, maxVoices))
    {
        // The voice pool never changes size, so the audio thread never sees a reallocation; the
        // allocator decides how many of them are in use.
        voices.resize((size_t) MAX_VOICES);
        UnifiedVoice::sharedTables();        // built here, on the message thread, not at the first note
        installAllocatorCallbacks();

        initialVoiceCount = juce::jlimit(1, MAX_VOICES, maxVoices);
        loadInitPatch();
    }

    EngineType getType() const override { return EngineType::UnifiedVoice; }
    juce::String getName() const override { return "Unified Voice"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr;
        for (auto& v : voices)
            v.setSampleRate(sr);
        // Glide coefficients depend on the sample rate.
        applyParameter(UnifiedParam::GLIDE, flatParams[UnifiedParam::GLIDE]);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        int numSamples = buffer.getNumSamples();

        modWheelNow = smoothWheel(modWheelNow, modWheelValue, numSamples, sampleRate);
        updatePressure();

        const float gain = HEADROOM * outputLevel * unisonGain;

        for (int i = 0; i < numSamples; ++i)
        {
            // An LFO routed to PAN moves its own voice between the channels (balance law: the
            // centre is unchanged, so a patch without that routing sounds exactly as before).
            float sampleL = 0.0f, sampleR = 0.0f;
            for (auto& voice : voices)
            {
                if (!voice.isActive())
                    continue;
                const float s = voice.process();
                const float pan = voice.getPanMod();
                sampleL += pan > 0.0f ? s * (1.0f - pan) : s;
                sampleR += pan < 0.0f ? s * (1.0f + pan) : s;
            }

            if (right)
            {
                left[i] += sampleL * gain;
                right[i] += sampleR * gain;
            }
            else
            {
                left[i] += 0.5f * (sampleL + sampleR) * gain;
            }
        }

        // Voices that are not sounding still keep their free-running LFOs in time, so FREE and
        // HOLD behave like one LFO that never stops.
        for (auto& voice : voices)
            if (!voice.isActive())
                voice.advanceIdleLfos(numSamples);
    }

    void releaseResources() override
    {
        for (auto& v : voices)
            v.reset();
    }

    void noteOn(int note, float velocity, int /*voiceIndex*/) override
    {
        if (isMono())
        {
            monoNoteOn(note, velocity);
            return;
        }
        allocator.noteOn(note, velocity);
    }

    void noteOff(int note, int /*voiceIndex*/) override
    {
        if (isMono())
        {
            monoNoteOff(note);
            return;
        }
        allocator.noteOff(note);
    }

    void allNotesOff() override
    {
        monoStackCount = 0;
        monoNote = -1;
        monoPressure = 0.0f;
        allocator.allNotesOff();
        for (auto& v : voices)
            v.reset();
    }

    void aftertouch(int note, float pressure) override
    {
        if (isMono())
        {
            if (note == monoNote)
                monoPressure = pressure;
            return;
        }
        allocator.setAftertouch(note, pressure);
    }

    // -1..1 from the pitch strip; the range is the BEND parameter.
    void pitchBend(float amount) override
    {
        bendAmount = juce::jlimit(-1.0f, 1.0f, amount);
        float semis = bendAmount * flatParams[UnifiedParam::BEND_RANGE];
        for (auto& v : voices)
            v.setPitchBend(semis);
    }

    // 0..1 from the mod strip. It goes to every LFO whose WHL amount is set (MOD2 page); while no
    // LFO in use listens to it, it is vibrato (up to half a semitone), so the strip always does
    // something.
    void modWheel(float value) override
    {
        modWheelValue = juce::jlimit(0.0f, 1.0f, value);
    }

    // ---- Per-stage envelope curves (StageCurveParam) --------------------------------------------
    // Envelope slots: 0 amp, 1 filter envelope, 2-5 the extra envelopes 1-4 (MENV). On the amp and
    // filter envelopes they add to the CURV cell.
    static int stageCurveParamFor(ParamId cellId)
    {
        using namespace UnifiedParam;
        switch (cellId)
        {
            case AMP_ATTACK:   return StageCurveParam::id(0, StageCurveParam::Attack);
            case AMP_DECAY:    return StageCurveParam::id(0, StageCurveParam::Decay);
            case AMP_RELEASE:  return StageCurveParam::id(0, StageCurveParam::Release);
            case FENV_ATTACK:  return StageCurveParam::id(1, StageCurveParam::Attack);
            case FENV_DECAY:   return StageCurveParam::id(1, StageCurveParam::Decay);
            case FENV_RELEASE: return StageCurveParam::id(1, StageCurveParam::Release);
            default: break;
        }
        if (cellId >= ModParam::ENV_BASE && cellId < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
        {
            const int env = (cellId - ModParam::ENV_BASE) / ModParam::ENV_STRIDE;
            const int cell = (cellId - ModParam::ENV_BASE) % ModParam::ENV_STRIDE;
            if (cell == ModParam::ENV_ATK) return StageCurveParam::id(2 + env, StageCurveParam::Attack);
            if (cell == ModParam::ENV_DEC) return StageCurveParam::id(2 + env, StageCurveParam::Decay);
            if (cell == ModParam::ENV_REL) return StageCurveParam::id(2 + env, StageCurveParam::Release);
        }
        return -1;
    }
    int getStageCurveParam(ParamId cellId) const override { return stageCurveParamFor(cellId); }

private:
    StageCurves stageCurves;

    // The curves of extra envelope `env` (0-3), when it is running.
    void applyExtraEnvCurves(int env)
    {
        for (auto& v : voices)
        {
            auto& chain = v.getEnvelopeChain();
            if (env >= chain.getStageCount())
                continue;
            auto p = chain.getEnvelopeMut(env).getParams();
            stageCurves.applyTo(p, 2 + env);
            chain.getEnvelopeMut(env).setParams(p);
        }
    }

    void setStageCurve(ParamId id, float value)
    {
        stageCurves.set(id, value);
        const int slot = StageCurveParam::envelopeOf(id);
        if (slot < 2)                               applyEnvelope(slot == 1);
        else if (slot < 2 + ModParam::MAX_ENVS)     applyExtraEnvCurves(slot - 2);
    }

public:
    void setParameter(ParamId id, float value) override
    {
        if (StageCurveParam::owns(id))
        {
            setStageCurve(id, value);
            return;
        }
        if (!EngineParamRange::contains(id) || id >= UnifiedParam::MAX_ID)
            return;
        if (id == UnifiedParam::MODE_A || id == UnifiedParam::MODE_B)
        {
            auto model = FilterSlot::modelFromIndex(
                (int) flatParams[id == UnifiedParam::MODE_A ? UnifiedParam::MODEL_A : UnifiedParam::MODEL_B]);
            value = (float) juce::jlimit(0, FilterSlot::modeCount(model) - 1, (int) std::lround(value));
        }
        else if (const ParamSpec* spec = findSpec(id))
            value = spec->isDiscrete() ? std::round(spec->clamp(value)) : spec->clamp(value);
        flatParams[(size_t) id] = value;
        applyParameter(id, value);

        // Page cells an LFO or the extra envelopes are routed to: the voices move them around
        // their set value, so they are told when it changes (and when an oscillator's kind, and
        // with it the range of its SRC cells, does).
        if (id == UnifiedParam::ARCH || id == UnifiedParam::WAVE1 || id == UnifiedParam::WAVE2 || id == UnifiedParam::WAVE3)
            refreshRoutes();
        else if (isRouted(id))
            for (auto& v : voices)
                v.setParamRouteBase(id, flatParams[(size_t) id]);
    }

    float getParameter(ParamId id) const override
    {
        if (id >= 0 && id < UnifiedParam::MAX_ID)
            return flatParams[(size_t) id];
        return stageCurves.get(id);     // 0 for anything else
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (const ParamSpec* spec = findSpec(id))
            return spec->label;
        return "Param " + juce::String(id);
    }

    int getParameterCount() const override { return UnifiedParam::MAX_ID; }

    bool supportsModulation() const override { return true; }
    int getMasterParamId() const override { return UnifiedParam::ARCH; }

    // ---- Pages -------------------------------------------------------------------------------

    ParamPages getParamPages() const override
    {
        ParamPages pages = basePages();
        // The third oscillator is a sub in the mono architecture and a full oscillator otherwise.
        const bool subStyle = (currentPresetId == ArchitecturePresetId::MonoVA && archIndex() == 1);
        pages[1].name = subStyle ? "SUB" : "OSC3";
        pages[1].params[2].label = subStyle ? "SUB" : "LVL3";

        // SRC: two cells for each oscillator that plays something other than an analog wave.
        for (auto& page : pages)
            if (page.name == "SRC")
                for (int osc = 0; osc < 3; ++osc)
                {
                    const int kind = (int) std::lround(flatParams[osc == 0 ? UnifiedParam::WAVE1 : (osc == 1 ? UnifiedParam::WAVE2 : UnifiedParam::WAVE3)]);
                    page.params[(size_t) (osc * 2)] = srcSpec(osc, kind, false);
                    page.params[(size_t) (osc * 2 + 1)] = srcSpec(osc, kind, true);
                }

        // The MODE cells offer the modes of the filter type currently chosen.
        for (auto& page : pages)
            for (auto& p : page.params)
                if (p.id == UnifiedParam::MODE_A || p.id == UnifiedParam::MODE_B)
                {
                    auto model = FilterSlot::modelFromIndex(
                        (int) flatParams[p.id == UnifiedParam::MODE_A ? UnifiedParam::MODEL_A : UnifiedParam::MODEL_B]);
                    p.choices = FilterSlot::modeNames(model);
                    p.numChoices = FilterSlot::modeCount(model);
                    p.maxVal = (float) (p.numChoices - 1);
                }
        return pages;
    }

    // The SRC cell of oscillator `osc` (0..2) for WAV choice `kind`; invalid for the analog waves.
    // Labels carry the oscillator's number: 1POS 1TBL, 2IDX 2RAT, 3FRM 3WID, 1TLT 1ODD.
    static ParamSpec srcSpec(int osc, int kind, bool second)
    {
        static const char* const labels[4][2][3] = {
            { { "1POS", "2POS", "3POS" }, { "1TBL", "2TBL", "3TBL" } },
            { { "1IDX", "2IDX", "3IDX" }, { "1RAT", "2RAT", "3RAT" } },
            { { "1FRM", "2FRM", "3FRM" }, { "1WID", "2WID", "3WID" } },
            { { "1TLT", "2TLT", "3TLT" }, { "1ODD", "2ODD", "3ODD" } } };
        // nTBL: the wavetable engine's built-in set, by its names (the first nine, then the newer ones).
        const int numTables = WavetableFactory::builtInCount();
        const int k = kind - (int) UnifiedVoice::OscWaveform::Wavetable;
        if (k < 0 || k > 3 || osc < 0 || osc > 2)
            return {};
        const int id = second ? UnifiedParam::srcB(osc) : UnifiedParam::srcA(osc);
        const char* label = labels[k][second ? 1 : 0][osc];
        const float d = srcDefault(kind, second);
        if (second && k == 0) return { label, id, 0, (float) (numTables - 1), d, ParamKind::Choice, "", WavetableFactory::builtInLabels(), numTables };
        if (second && k == 1) return { label, id, 1, 12, d, ParamKind::Int };
        return { label, id, 0, 1, d, ParamKind::Linear };
    }
    static float srcDefault(int kind, bool second)
    {
        switch ((UnifiedVoice::OscWaveform) kind)
        {
            case UnifiedVoice::OscWaveform::Wavetable: return second ? 0.0f : 0.0f;      // SAW table, first frame
            case UnifiedVoice::OscWaveform::FM:        return second ? 2.0f : 0.3f;
            case UnifiedVoice::OscWaveform::Formant:   return second ? 0.5f : 0.3f;
            case UnifiedVoice::OscWaveform::Harmonic:  return second ? 0.0f : 0.4f;
            default:                                   return 0.0f;
        }
    }

    static const ParamPages& basePages()
    {
        using namespace UnifiedParam;
        static const char* const waves[] = { "SIN", "SAW", "SQR", "TRI", "PLS", "NOIS", "WT", "FM", "FORM", "HARM" };
        static const char* const routes[] = { "SNGL", "SER", "PAR" };
        static const char* const modes[] = { "MONO", "DUO", "POLY", "UNI" };
        static const char* const archs[] = { "INIT", "MONO", "HYBR", "POLY" };
        static const char* const stacks[] = { "OFF", "2", "4", "8" };
        static const char* const glideModes[] = { "ALL", "LEGA" };

        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;

        static const ParamPages pages = {
            { "OSC", "SRC", {{
                { "WAV1", WAVE1, 0, 9, 1, C, "", waves, 10 },
                { "WAV2", WAVE2, 0, 9, 1, C, "", waves, 10 },
                { "MIX",  MIX, 0, 1, 0.5f, L },
                { "PW",   PULSE_WIDTH, 0.05f, 0.95f, 0.5f, L },
                { "TUN2", TUNE2, -24, 24, 0, I, "st" },
                { "DET2", DETUNE2, -50, 50, 8, L, "ct" },
                { "XMOD", CROSS_MOD, 0, 1, 0, L },
                { "DRIV", MIX_DRIVE, 0, 1, 0, L } }} },
            { "OSC3", "SRC", {{
                { "WAV3", WAVE3, 0, 9, 0, C, "", waves, 10 },
                { "TUN3", TUNE3, -24, 24, -12, I, "st" },
                { "LVL3", SUB, 0, 1, 0, L },
                { "NOIS", NOISE, 0, 1, 0, L },
                { "NCOL", NOISE_COLOR, 0, 1, 0, L },
                { "TUNE", TUNE, -24, 24, 0, I, "st" },
                { "FINE", FINE, -100, 100, 0, L, "ct" },
                { "BEND", BEND_RANGE, 0, 24, 2, I, "st" } }} },
            // Filled in getParamPages(): two cells per oscillator that is a wavetable, FM, formant
            // or harmonic oscillator (empty for the analog waves).
            { "SRC", "SRC", {{ {}, {}, {}, {}, {}, {}, {}, {} }} },
            { "VOICE", "SRC", {{
                { "ARCH", ARCH, 0, 3, 0, C, "", archs, 4 },
                { "MODE", VOICE_MODE, 0, 3, 2, C, "", modes, 4 },
                { "VOX",  VOICE_COUNT, 1, MAX_VOICES, 8, I },
                { "LEGA", LEGATO, 0, 1, 0, C, "", ParamChoices::offOn, 2 },
                { "STCK", STACK, 0, 3, 0, C, "", stacks, 4 },
                { "SPRD", SPREAD, 0, 1, 0.1f, L },
                { "GLID", GLIDE, 0, 2, 0, L, "s" },
                { "GLMD", GLIDE_MODE, 0, 1, 0, C, "", glideModes, 2 } }} },
            // Everything that makes the voices imperfect, in one place: slow drift (amount, rate),
            // and what is drawn afresh for every note (pitch, start phase, waveshape, pitch
            // overshoot, cutoff, envelope times).
            { "ANALG", "SRC", {{
                { "DRFT", DRIFT, 0, 1, 0.15f, L },
                { "DRAT", DRIFT_RATE, 0.05f, 5, 0.3f, G, "Hz" },
                { "SLOP", SLOP, 0, 50, 2, L, "ct" },
                { "PHAS", RANDOM_PHASE, 0, 1, 1, C, "", ParamChoices::offOn, 2 },
                { "WSHP", WAVE_VAR, 0, 1, 0.1f, L },
                { "SOFT", SOFT_START, 0, 50, 8, L, "ct" },
                { "FVAR", FILTER_VAR, 0, 1, 0, L },
                { "EVAR", ENV_VAR, 0, 1, 0, L } }} },
            { "FILTR", "FLTR", {{
                { "CUT",  CUTOFF, 20, 20000, 8000, G, "Hz" },
                { "RESO", RESONANCE, 0, 1, 0.2f, L },
                { "ENV",  FILTER_ENV, -1, 1, 0, L },
                { "DRIV", FILTER_DRIVE, 0, 1, 0, L },
                { "TYPE", MODEL_A, 0, FilterSlot::NUM_MODELS - 1, 0, C, "", FilterSlot::modelNames(), FilterSlot::NUM_MODELS },
                { "MODE", MODE_A, 0, 5, 0, C, "", FilterSlot::modeNames(FilterModel::SVF), 4 },
                { "MORP", MORPH, 0, 1, 0, L },
                { "FFM",  FILTER_FM, 0, 1, 0, L } }} },
            { "FLT B", "FLTR", {{
                { "TYPB", MODEL_B, 0, FilterSlot::NUM_MODELS - 1, FilterSlot::NUM_MODELS - 1, C, "", FilterSlot::modelNames(), FilterSlot::NUM_MODELS },
                { "MODB", MODE_B, 0, 5, 0, C, "", FilterSlot::modeNames(FilterModel::Bypass), 1 },
                { "OFSB", FILTER_B_OFFSET, -48, 48, 0, I, "st" },
                { "RESB", RESONANCE_B, 0, 1, 0.2f, L },
                { "ROUT", ROUTING, 0, 2, 0, C, "", routes, 3 },
                { "BAL",  BALANCE, 0, 1, 0.5f, L },
                { "DRVB", DRIVE_B, 0, 1, 0, L },
                { "TRKB", B_TRACK, 0, 1, 1, L } }} },
            { "F ENV", "FLTR", {{
                { "ATK",  FENV_ATTACK, 0.001f, 10, 0.01f, G, "s" },
                { "DEC",  FENV_DECAY, 0.001f, 10, 0.5f, G, "s" },
                { "SUS",  FENV_SUSTAIN, 0, 1, 0, L },
                { "REL",  FENV_RELEASE, 0.001f, 10, 0.3f, G, "s" },
                { "CURV", FENV_CURVE, -1, 1, 0, L },
                { "KEY",  KEYTRACK, -1, 1, 0, L },
                { "VEL",  FILTER_VEL, 0, 1, 0, L },
                { "PMOD", POLY_MOD, -1, 1, 0, L } }} },
            { "AMP", "AMP", {{
                { "ATK",  AMP_ATTACK, 0.001f, 10, 0.01f, G, "s" },
                { "DEC",  AMP_DECAY, 0.001f, 10, 0.3f, G, "s" },
                { "SUS",  AMP_SUSTAIN, 0, 1, 0.7f, L },
                { "REL",  AMP_RELEASE, 0.001f, 10, 0.3f, G, "s" },
                { "CURV", AMP_CURVE, -1, 1, 0, L },
                { "VEL",  VEL_SENS, 0, 1, 0.7f, L },
                { "LEVL", LEVEL, 0, 1, 0.8f, L },
                {} }} },
        };
        return pages;
    }

    VoiceAllocator& getAllocator() { return allocator; }
    UnifiedVoice& getVoice(int index) { return voices[(size_t) juce::jlimit(0, MAX_VOICES - 1, index)]; }

    // ---- Architecture presets ----------------------------------------------------------------

    void applyArchitecturePreset(ArchitecturePresetId presetId)
    {
        const auto& preset = getArchitecturePreset(presetId);
        currentPresetId = presetId;
        flatParams[UnifiedParam::ARCH] = (float) ((int) presetId + 1);

        using namespace UnifiedParam;

        set(VOICE_MODE, (float) (int) preset.voiceMode);
        set(VOICE_COUNT, (float) preset.voiceCount);
        set(LEGATO, preset.legato ? 1.0f : 0.0f);
        set(GLIDE, preset.glideTime);
        set(GLIDE_MODE, preset.legato ? 1.0f : 0.0f);
        set(STACK, preset.unisonEnabled ? (preset.unisonCount >= 4 ? 2.0f : 1.0f) : 0.0f);
        set(SPREAD, preset.unisonEnabled ? preset.unisonSpread : 0.1f);
        set(DRIFT, preset.analogEnabled ? preset.driftAmount : 0.0f);

        set(WAVE1, (float) (int) preset.oscWaveforms[0]);
        set(WAVE2, (float) (int) preset.oscWaveforms[1]);
        set(WAVE3, (float) (int) preset.oscWaveforms[2]);
        const float l1 = preset.oscEnabled[0] ? preset.oscLevels[0] : 0.0f;
        const float l2 = preset.oscEnabled[1] ? preset.oscLevels[1] : 0.0f;
        set(MIX, (l1 + l2) > 0.0f ? l2 / (l1 + l2) : 0.0f);
        set(SUB, preset.oscEnabled[2] ? preset.oscLevels[2] : 0.0f);
        const float semis2 = std::round(preset.oscDetune[1]);
        set(TUNE2, semis2);
        set(DETUNE2, (preset.oscDetune[1] - semis2) * 100.0f);
        set(TUNE3, std::round(preset.oscDetune[2]));
        set(NOISE, preset.noiseEnabled ? preset.noiseLevel : 0.0f);
        set(PULSE_WIDTH, 0.5f);
        set(CROSS_MOD, 0.0f);
        set(MIX_DRIVE, 0.0f);
        set(TUNE, 0.0f);
        set(FINE, 0.0f);

        set(MODEL_A, (float) FilterSlot::indexFromModel(preset.filterModel));
        set(MODEL_B, (float) FilterSlot::indexFromModel(preset.filter2Model));
        set(MODE_A, 0.0f);
        set(MODE_B, 0.0f);
        set(BALANCE, 0.5f);
        set(FILTER_FM, 0.0f);
        set(DRIVE_B, 0.0f);
        set(POLY_MOD, 0.0f);
        set(B_TRACK, 1.0f);
        set(ROUTING, (float) (int) preset.filterRouting);
        set(CUTOFF, preset.filterCutoff);
        set(RESONANCE, juce::jmin(preset.filterResonance, 1.0f));
        set(RESONANCE_B, juce::jmin(preset.filterResonance, 1.0f));
        set(FILTER_DRIVE, preset.filterDrive);
        // Presets give the envelope amount in Hz above the cutoff; the parameter is in octaves.
        float octaves = std::log2(juce::jmax(20.0f, preset.filterCutoff + preset.filterEnvAmount)
                                  / juce::jmax(20.0f, preset.filterCutoff));
        set(FILTER_ENV, octaves / UnifiedVoice::FILTER_ENV_OCTAVES);
        set(KEYTRACK, preset.voiceMode == VoiceMode::Poly ? 0.5f : 0.3f);
        set(FILTER_VEL, 0.0f);
        set(MORPH, 0.0f);
        set(FILTER_B_OFFSET, 0.0f);

        set(AMP_ATTACK, preset.ampEnv.attack);
        set(AMP_DECAY, preset.ampEnv.decay);
        set(AMP_SUSTAIN, preset.ampEnv.sustain);
        set(AMP_RELEASE, preset.ampEnv.release);
        set(FENV_ATTACK, preset.filterEnv.attack);
        set(FENV_DECAY, preset.filterEnv.decay);
        set(FENV_SUSTAIN, preset.filterEnv.sustain);
        set(FENV_RELEASE, preset.filterEnv.release);

        // The architecture's mod strip route (LFO cells on MOD / MOD2).
        for (auto& [id, value] : preset.wheelRoute)
            set(id, value);
    }

    ArchitecturePresetId getCurrentPresetId() const { return currentPresetId; }

    const ArchitecturePreset& getCurrentPreset() const
    {
        return getArchitecturePreset(currentPresetId);
    }

private:
    // One full-level voice peaks about where a drum hit does (measured on the device: 0.7 left the
    // synth clearly quieter than the kit).
    static constexpr float HEADROOM = 1.0f;
    static constexpr int MONO_STACK_SIZE = 16;

    void set(ParamId id, float value) { setParameter(id, value); }

    int archIndex() const { return (int) flatParams[UnifiedParam::ARCH]; }

    static const ParamSpec* findSpec(int id)
    {
        for (auto& page : basePages())
            if (auto* p = page.find(id))
                return p;
        return nullptr;
    }

    void installAllocatorCallbacks()
    {
        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            if (voiceIdx >= 0 && voiceIdx < (int) voices.size())
            {
                voices[(size_t) voiceIdx].setNote(v.note, v.velocity);
                voices[(size_t) voiceIdx].noteOn();
            }
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            if (voiceIdx >= 0 && voiceIdx < (int) voices.size())
                voices[(size_t) voiceIdx].noteOff();
        });
    }

    // Every page parameter at its default, with the voice count the engine was built with.
    void loadInitPatch()
    {
        for (auto& page : basePages())
            for (auto& p : page.params)
                if (p.isValid() && p.id != UnifiedParam::ARCH)
                    setParameter(p.id, p.id == UnifiedParam::VOICE_COUNT ? (float) initialVoiceCount : p.defaultVal);

        flatParams[UnifiedParam::ARCH] = 0.0f;

        // Modulation defaults: one LFO, no extra envelopes, nothing routed.
        flatParams[ModParam::LFO_COUNT] = 1.0f;
        flatParams[ModParam::ENV_COUNT] = 0.0f;
        for (int l = 0; l < ModParam::MAX_LFOS; ++l)
        {
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_RATE), 2.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_DEPTH), 0.3f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_SHAPE), 0.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_TRIG), 0.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_SYNC), 0.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_DEST), 0.0f);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_DIV), (float) LFO::DEFAULT_DIVISION);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_PHASE), 0.0f);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_DELAY), 0.0f);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_FADE), 0.0f);
            for (int off : { ModParam::LFO_WHEEL, ModParam::LFO_VEL, ModParam::LFO_KTRK, ModParam::LFO_PRES })
                setParameter(ModParam::lfoExt2ParamId(l, off), 0.0f);
        }
        for (int e = 0; e < ModParam::MAX_ENVS; ++e)
        {
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_ATK)] = 0.01f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_DEC)] = 0.1f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_SUS)] = 0.7f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_REL)] = 0.3f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_DEPTH)] = 1.0f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_TARGET)] = 3.0f;
        }
        applyModParameter(ModParam::ENV_COUNT, 0.0f);
        applyModParameter(ModParam::LFO_COUNT, 1.0f);
    }

    bool isMono() const { return allocator.getMode() == VoiceMode::Mono; }

    // ---- Mono: last-note priority with a held-note stack, optional legato -----------------------

    void monoNoteOn(int note, float velocity)
    {
        const bool wasHeld = monoStackCount > 0 && voices[0].isGateOpen();
        monoRemove(note);
        if (monoStackCount < MONO_STACK_SIZE)
            monoStack[(size_t) monoStackCount++] = note;

        monoVelocity = velocity;
        monoPressure = 0.0f;
        monoNote = note;

        if (legato && wasHeld)
        {
            voices[0].setNoteLegato(note);
        }
        else
        {
            voices[0].setNote(note, velocity);
            voices[0].noteOn();
        }
    }

    void monoNoteOff(int note)
    {
        monoRemove(note);
        if (note != monoNote)
            return;

        if (monoStackCount > 0)
        {
            // Fall back to the most recent key still held.
            monoNote = monoStack[(size_t) (monoStackCount - 1)];
            if (legato)
            {
                voices[0].setNoteLegato(monoNote);
            }
            else
            {
                voices[0].setNote(monoNote, monoVelocity);
                voices[0].noteOn();
            }
        }
        else
        {
            monoNote = -1;
            voices[0].noteOff();
        }
    }

    void monoRemove(int note)
    {
        int w = 0;
        for (int r = 0; r < monoStackCount; ++r)
            if (monoStack[(size_t) r] != note)
                monoStack[(size_t) w++] = monoStack[(size_t) r];
        monoStackCount = w;
    }

    // True while an LFO in use has a WHL amount: the strip then does what those say and nothing else.
    bool wheelIsRouted() const
    {
        const int lfos = juce::jlimit(1, ModParam::MAX_LFOS, (int) flatParams[ModParam::LFO_COUNT]);
        for (int l = 0; l < lfos; ++l)
            if (flatParams[(size_t) ModParam::lfoExt2ParamId(l, ModParam::LFO_WHEEL)] != 0.0f)
                return true;
        return false;
    }

    // Per-voice pressure through the shared router, plus the mod wheel: to the LFOs' WHL amounts,
    // or as vibrato on every voice while none is set.
    void updatePressure()
    {
        const int count = juce::jmin(allocator.getVoiceCount(), (int) voices.size());
        const float wheelVibrato = wheelIsRouted() ? 0.0f : modWheelNow * 0.5f;
        for (int i = 0; i < (int) voices.size(); ++i)
        {
            float pressure = 0.0f;
            if (isMono())
                pressure = (i == 0) ? monoPressure : 0.0f;
            else if (i < count)
                pressure = allocator.getAftertouch(i);

            AftertouchRouter::ModValues m;
            if (pressure > 0.0f)
                m = (aftertouchRouter != nullptr ? *aftertouchRouter : allocator.getAftertouchRouter())
                        .computeModulation(pressure);

            voices[(size_t) i].setPressureMods(m.volumeMod, m.cutoffMod, m.vibratoMod + wheelVibrato,
                                               m.timbreMod, m.wavePosMod, m.fmDepthMod);
            // The LFOs' own WHL / PRES amounts take the raw wheel and this voice's raw pressure.
            voices[(size_t) i].setLfoPerformance(modWheelNow, pressure);
        }
    }

    void applyOscLevels()
    {
        const float mix = flatParams[UnifiedParam::MIX];
        const float sub = flatParams[UnifiedParam::SUB];
        const float xmod = flatParams[UnifiedParam::CROSS_MOD];
        for (auto& v : voices)
        {
            v.getOsc(0).level = 1.0f - mix;
            v.getOsc(0).enabled = mix < 0.999f;
            v.getOsc(1).level = mix;
            // Still needed as the cross-mod / filter-FM source when it is out of the mix.
            v.getOsc(1).enabled = mix > 0.001f || xmod > 0.0f || flatParams[UnifiedParam::FILTER_FM] > 0.0f
                                  || lfoNeedsOsc2();
            v.getOsc(2).level = sub;
            v.getOsc(2).enabled = sub > 0.001f;
        }
    }

    // An LFO routed to the cross-mod or filter-FM amount needs oscillator 2 running.
    bool lfoNeedsOsc2() const
    {
        const int count = juce::jlimit(1, ModParam::MAX_LFOS, (int) flatParams[ModParam::LFO_COUNT]);
        for (int l = 0; l < count; ++l)
            if (UnifiedVoice::lfoDestNeedsOsc2(voices[0].getLfoDest(l)))
                return true;
        return UnifiedVoice::lfoDestNeedsOsc2(voices[0].getEnvDest());
    }

    // ---- Page cells as LFO / envelope destinations (UnifiedVoice::PARAM_DESTS) ------------------

    bool isRouted(int id) const
    {
        for (int r : routeId)
            if (r == id && id >= 0)
                return true;
        return false;
    }

    // A slot's table cell: full depth moves it as far as it did when the list was nine tables long
    // (UnifiedVoice::TABLE_ROUTE_SPAN). 0 for every other cell: its whole range.
    float tableRouteSpan(int id) const
    {
        for (int osc = 0; osc < 3; ++osc)
            if (id == UnifiedParam::srcB(osc))
            {
                const int kind = (int) std::lround(flatParams[osc == 0 ? UnifiedParam::WAVE1 : (osc == 1 ? UnifiedParam::WAVE2 : UnifiedParam::WAVE3)]);
                return kind == (int) UnifiedVoice::OscWaveform::Wavetable ? UnifiedVoice::TABLE_ROUTE_SPAN : 0.0f;
            }
        return 0.0f;
    }

    // The range and scale of a cell an LFO can be sent to, from its ParamSpec.
    void destRange(int id, float& lo, float& hi, bool& log) const
    {
        lo = 0.0f; hi = 1.0f; log = false;
        ParamSpec src;
        const ParamSpec* spec = findSpec(id);
        for (int osc = 0; osc < 3 && spec == nullptr; ++osc)
            if (id == UnifiedParam::srcA(osc) || id == UnifiedParam::srcB(osc))
            {
                const int kind = (int) std::lround(flatParams[osc == 0 ? UnifiedParam::WAVE1 : (osc == 1 ? UnifiedParam::WAVE2 : UnifiedParam::WAVE3)]);
                src = srcSpec(osc, kind, id == UnifiedParam::srcB(osc));
                if (! src.isValid())
                    return;             // an analog wave: the cells do nothing
                spec = &src;
            }
        if (spec != nullptr)
        {
            lo = spec->minVal;
            hi = spec->maxVal;
            log = spec->kind == ParamKind::Log;
            return;
        }
        // The extra envelopes' cells are built by the screen: times 1 ms .. 10 s, the rest 0..1.
        if (id >= ModParam::ENV_BASE && id < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
        {
            const int cell = (id - ModParam::ENV_BASE) % ModParam::ENV_STRIDE;
            if (cell == ModParam::ENV_ATK || cell == ModParam::ENV_DEC || cell == ModParam::ENV_REL)
            {
                lo = 0.001f; hi = 10.0f; log = true;
            }
        }
    }

    // Source 0-3 = the LFOs (their DEST), 4 = the extra envelopes: tells every voice which page
    // cell the source moves, and puts a cell it has let go of back to its set value.
    void updateRoute(int source)
    {
        const int dest = source < ModParam::MAX_LFOS
            ? (int) std::lround(flatParams[(size_t) ModParam::lfoParamId(source, ModParam::LFO_DEST)])
            : envDestIndex;
        const int id = UnifiedVoice::paramOfDest(dest);
        const int before = routeId[(size_t) source];
        routeId[(size_t) source] = id;
        if (id >= 0)
        {
            float lo, hi; bool log;
            destRange(id, lo, hi, log);
            for (auto& v : voices)
                v.setParamRoute(source, id, flatParams[(size_t) id], lo, hi, log, tableRouteSpan(id));
        }
        else
        {
            for (auto& v : voices)
                v.setParamRoute(source, -1);
        }
        if (before >= 0 && before != id && ! isRouted(before))
            applyParameter(before, flatParams[(size_t) before]);
    }

    void refreshRoutes()
    {
        for (int s = 0; s < UnifiedVoice::NUM_MOD_SOURCES; ++s)
            if (routeId[(size_t) s] >= 0 || s < ModParam::MAX_LFOS)
                updateRoute(s);
    }

    void applyUnison()
    {
        const bool unison = allocator.getMode() == VoiceMode::Unison;
        const int n = allocator.getVoiceCount();
        const float spread = flatParams[UnifiedParam::SPREAD];
        unisonGain = (unison && n > 1) ? 1.0f / std::sqrt((float) n) : 1.0f;
        for (int i = 0; i < (int) voices.size(); ++i)
        {
            float detune = 0.0f;
            if (unison && n > 1 && i < n)
                detune = (((float) i / (float) (n - 1)) * 2.0f - 1.0f) * spread * 0.5f;
            voices[(size_t) i].setUnisonDetune(detune);
        }
    }

    void applyEnvelope(bool filter)
    {
        using namespace UnifiedParam;
        Envelope::Params p;
        p.attack = flatParams[filter ? FENV_ATTACK : AMP_ATTACK];
        p.decay = flatParams[filter ? FENV_DECAY : AMP_DECAY];
        p.sustain = flatParams[filter ? FENV_SUSTAIN : AMP_SUSTAIN];
        p.release = flatParams[filter ? FENV_RELEASE : AMP_RELEASE];
        stageCurves.applyTo(p, filter ? 1 : 0, flatParams[filter ? FENV_CURVE : AMP_CURVE]);
        for (auto& v : voices)
        {
            if (filter) v.setFilterEnvParams(p);
            else        v.setAmpEnvParams(p);
        }
    }

    void applyParameter(ParamId id, float value)
    {
        using namespace UnifiedParam;
        const int iv = (int) std::lround(value);

        switch (id)
        {
            case WAVE1: case WAVE2: case WAVE3:
            {
                int osc = (id == WAVE1) ? 0 : (id == WAVE2 ? 1 : 2);
                const int kind = juce::jlimit(0, UnifiedVoice::NUM_WAVEFORMS - 1, iv);
                auto w = static_cast<UnifiedVoice::OscWaveform>(kind);
                // A different kind of oscillator starts from that kind's own two settings.
                if (voices[0].getOsc(osc).waveform != w && !UnifiedVoice::isAnalog(w))
                {
                    flatParams[(size_t) srcA(osc)] = srcDefault(kind, false);
                    flatParams[(size_t) srcB(osc)] = srcDefault(kind, true);
                }
                for (auto& v : voices)
                {
                    v.getOsc(osc).waveform = w;
                    v.getOsc(osc).a = flatParams[(size_t) srcA(osc)];
                    v.getOsc(osc).b = flatParams[(size_t) srcB(osc)];
                }
                break;
            }
            // The cells that are simply one value in every voice: UnifiedVoice::setParam (which is
            // also what an LFO routed to the cell calls, for its own voice).
            case SRC_A1: case SRC_A2: case SRC_A3: case SRC_B1: case SRC_B2: case SRC_B3:
            case TUNE3: case MIX_DRIVE: case NOISE_COLOR: case GLIDE: case DRIFT: case DRIFT_RATE:
            case FILTER_ENV: case FILTER_DRIVE: case MORPH: case RESONANCE_B: case BALANCE: case DRIVE_B:
            case B_TRACK: case KEYTRACK: case FILTER_VEL: case POLY_MOD: case VEL_SENS:
                for (auto& v : voices) v.setParam(id, value);
                break;
            case MIX: case SUB: case CROSS_MOD:
                applyOscLevels();
                if (id == CROSS_MOD)
                    for (auto& v : voices) v.setCrossMod(value);
                break;
            case TUNE: case FINE:
                for (auto& v : voices) v.setTune(flatParams[TUNE] + flatParams[FINE] / 100.0f);
                break;
            case TUNE2: case DETUNE2:
                for (auto& v : voices)
                {
                    v.setParam(TUNE2, flatParams[TUNE2]);
                    v.setParam(DETUNE2, flatParams[DETUNE2]);
                }
                break;
            case NOISE:        for (auto& v : voices) v.setNoiseLevel(value); break;
            case PULSE_WIDTH:  for (auto& v : voices) v.setPulseWidth(value); break;
            case GLIDE_MODE:
                for (auto& v : voices)
                    v.setGlideMode(iv > 0 ? UnifiedVoice::GlideMode::Legato : UnifiedVoice::GlideMode::Always);
                break;
            case BEND_RANGE:   pitchBend(bendAmount); break;

            case CUTOFF:       for (auto& v : voices) v.setFilterBaseCutoff(value); break;
            case RESONANCE:    for (auto& v : voices) v.setFilterResonanceA(value); break;
            case FILTER_B_OFFSET: for (auto& v : voices) v.setFilterBOffset(value); break;
            case FILTER_FM:
                for (auto& v : voices) v.setFilterFM(value);
                applyOscLevels();      // oscillator 2 must run while it modulates the filter
                break;
            case MODEL_A: case MODEL_B:
            {
                const int slot = (id == MODEL_A) ? 0 : 1;
                const auto model = FilterSlot::modelFromIndex(iv);
                // Keep the stored mode inside the new type's range so the cell and the sound agree.
                const int modeId = (slot == 0) ? MODE_A : MODE_B;
                flatParams[(size_t) modeId] = (float) juce::jmin((int) flatParams[(size_t) modeId],
                                                                 FilterSlot::modeCount(model) - 1);
                for (auto& v : voices)
                {
                    v.setFilterModel(slot, model);
                    v.setFilterMode(slot, (int) flatParams[(size_t) modeId]);
                }
                break;
            }
            case MODE_A: case MODE_B:
                for (auto& v : voices) v.setFilterMode(id == MODE_A ? 0 : 1, iv);
                break;
            case ROUTING:
                for (auto& v : voices) v.setFilterRouting(static_cast<FilterRouting>(juce::jlimit(0, 2, iv)));
                break;

            case AMP_ATTACK: case AMP_DECAY: case AMP_SUSTAIN: case AMP_RELEASE: case AMP_CURVE:
                applyEnvelope(false);
                break;
            case FENV_ATTACK: case FENV_DECAY: case FENV_SUSTAIN: case FENV_RELEASE: case FENV_CURVE:
                applyEnvelope(true);
                break;
            case LEVEL:    outputLevel = value; break;

            case ARCH:
                if (iv <= 0)
                {
                    loadInitPatch();
                }
                else
                {
                    applyArchitecturePreset(static_cast<ArchitecturePresetId>(
                        juce::jlimit(0, (int) ArchitecturePresetId::Count - 1, iv - 1)));
                }
                break;
            case VOICE_MODE:
            {
                auto m = static_cast<VoiceMode>(juce::jlimit(0, 3, iv));
                if (m != allocator.getMode())
                {
                    allNotesOff();
                    allocator.setMode(m);
                }
                applyUnison();
                break;
            }
            case VOICE_COUNT:
            {
                int n = juce::jlimit(1, MAX_VOICES, iv);
                if (n != allocator.getVoiceCount())
                {
                    allNotesOff();
                    allocator.setVoiceCount(n);
                }
                applyUnison();
                break;
            }
            case LEGATO: legato = iv > 0; break;
            case STACK:  for (auto& v : voices) v.setWaveStackMode(iv); break;
            case SPREAD:
                for (auto& v : voices) v.setWaveStackDetune(value);
                applyUnison();
                break;
            case SLOP:         for (auto& v : voices) v.getAnalogCharacter().slopCents = value; break;
            case RANDOM_PHASE: for (auto& v : voices) v.getAnalogCharacter().randomPhase = iv > 0; break;
            // A component tolerance over the first part of the cell (0.3 % at the default, as it
            // always was), growing into a difference that is heard from note to note (15 % at the top:
            // up to 0.03 the whole cell could not be heard at all).
            case WAVE_VAR:     for (auto& v : voices) v.getAnalogCharacter().waveshapeVar = value * (0.03f + 0.12f * value * value); break;
            case SOFT_START:   for (auto& v : voices) v.getAnalogCharacter().softStartCents = value; break;
            case FILTER_VAR:   for (auto& v : voices) v.getAnalogCharacter().filterVariance = value; break;
            case ENV_VAR:      for (auto& v : voices) v.getAnalogCharacter().envVariance = value; break;
            default:
                applyModParameter(id, value);
                break;
        }
    }

    void applyModParameter(ParamId id, float value)
    {
        // LFO params: 40 + lfoIdx*6 + offset
        if (id >= ModParam::LFO_BASE
            && id < ModParam::LFO_BASE + ModParam::MAX_LFOS * ModParam::LFO_STRIDE)
        {
            int offset = id - ModParam::LFO_BASE;
            int lfoIdx = offset / ModParam::LFO_STRIDE;
            int param = offset % ModParam::LFO_STRIDE;

            for (auto& v : voices)
            {
                auto& lfo = v.getLFOBank().getLFO(lfoIdx);
                switch (param)
                {
                    case ModParam::LFO_RATE:  lfo.setRate(value); break;
                    case ModParam::LFO_DEPTH: lfo.setDepth(value); break;
                    case ModParam::LFO_SHAPE:
                        lfo.setShape(static_cast<LFO::Shape>(juce::jlimit(0, (int) LFO::Shape::COUNT - 1, (int) std::lround(value))));
                        break;
                    case ModParam::LFO_TRIG:
                        lfo.setTriggerMode(static_cast<LFO::TriggerMode>(
                            juce::jlimit(0, (int) LFO::TriggerMode::COUNT - 1, (int) std::lround(value))));
                        break;
                    case ModParam::LFO_SYNC:
                        lfo.setTempo(tempoBpm);
                        lfo.setTempoSync(value > 0.5f);
                        break;
                    case ModParam::LFO_DEST:
                        v.setLfoDest(lfoIdx, static_cast<UnifiedVoice::LfoDest>(
                            juce::jlimit(0, (int) UnifiedVoice::LfoDest::Count - 1, (int) std::lround(value))));
                        break;
                    default: break;
                }
            }
            if (param == ModParam::LFO_DEST)
            {
                flatParams[(size_t) id] = (float) juce::jlimit(0, (int) UnifiedVoice::LfoDest::Count - 1, (int) std::lround(value));
                updateRoute(lfoIdx);    // a page cell as the destination (or no longer)
                applyOscLevels();       // oscillator 2 may now be needed (or no longer)
            }
            return;
        }

        // More LFO params: 130 + lfoIdx*4 + offset
        if (id >= ModParam::LFO_EXT_BASE && id < ModParam::LFO_EXT_END)
        {
            int offset = id - ModParam::LFO_EXT_BASE;
            int lfoIdx = offset / ModParam::LFO_EXT_STRIDE;
            int param = offset % ModParam::LFO_EXT_STRIDE;

            for (auto& v : voices)
            {
                auto& lfo = v.getLFOBank().getLFO(lfoIdx);
                switch (param)
                {
                    case ModParam::LFO_DIV:
                        lfo.setTempo(tempoBpm);
                        lfo.setSyncDivision((int) std::lround(value));
                        break;
                    case ModParam::LFO_PHASE: lfo.setStartPhase(juce::jlimit(0.0f, 1.0f, value)); break;
                    case ModParam::LFO_DELAY: lfo.setDelayTime(value); break;
                    case ModParam::LFO_FADE:  lfo.setFadeTime(value); break;
                    default: break;
                }
            }
            return;
        }

        // Extra ENV params: 64 + envIdx*6 + offset
        if (id >= ModParam::ENV_BASE
            && id < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
        {
            int offset = id - ModParam::ENV_BASE;
            int envIdx = offset / ModParam::ENV_STRIDE;
            int param = offset % ModParam::ENV_STRIDE;

            for (auto& v : voices)
            {
                auto& chain = v.getEnvelopeChain();
                // Depth and target belong to the whole chain; the stage settings need the stage.
                if (param == ModParam::ENV_DEPTH) { chain.setDepth(value); continue; }
                if (param == ModParam::ENV_TARGET)
                {
                    // 0-3: the envelopes' own targets. Beyond: one of the LFOs' destinations.
                    const int t = juce::jlimit(0, UnifiedVoice::NUM_ENV_DESTS - 1, (int) std::lround(value));
                    chain.setTarget(t < UnifiedVoice::NUM_ENV_TARGETS ? static_cast<EnvelopeChain::Target>(t)
                                                                      : EnvelopeChain::Target::Custom);
                    v.setEnvDest(UnifiedVoice::lfoDestOfEnvDest(t));
                    continue;
                }
                if (envIdx >= chain.getStageCount())
                    continue;
                auto& env = chain.getEnvelopeMut(envIdx);
                auto p = env.getParams();
                switch (param)
                {
                    case ModParam::ENV_ATK:    p.attack = value; env.setParams(p); break;
                    case ModParam::ENV_DEC:    p.decay = value; env.setParams(p); break;
                    case ModParam::ENV_SUS:    p.sustain = value; env.setParams(p); break;
                    case ModParam::ENV_REL:    p.release = value; env.setParams(p); break;
                    default: break;
                }
            }
            if (param == ModParam::ENV_TARGET)
            {
                envDestIndex = (int) voices[0].getEnvDest();
                updateRoute(UnifiedVoice::ENV_SOURCE);
                applyOscLevels();
            }
            return;
        }

        // LFO performance controls: 174 + lfoIdx*4 + offset
        if (id >= ModParam::LFO_EXT2_BASE && id < ModParam::LFO_EXT2_END)
        {
            int offset = id - ModParam::LFO_EXT2_BASE;
            int lfoIdx = offset / ModParam::LFO_EXT2_STRIDE;
            int param = offset % ModParam::LFO_EXT2_STRIDE;
            const float amount = juce::jlimit(-1.0f, 1.0f, value);

            for (auto& v : voices)
            {
                switch (param)
                {
                    case ModParam::LFO_WHEEL: v.setLfoWheelAmount(lfoIdx, amount); break;
                    case ModParam::LFO_VEL:   v.setLfoVelocityAmount(lfoIdx, amount); break;
                    case ModParam::LFO_KTRK:  v.getLFOBank().getLFO(lfoIdx).setKeyTrack(amount); break;
                    case ModParam::LFO_PRES:  v.setLfoPressureAmount(lfoIdx, amount); break;
                    default: break;
                }
            }
            return;
        }

        // LFO count change
        if (id == ModParam::LFO_COUNT)
        {
            int count = juce::jlimit(1, ModParam::MAX_LFOS, (int)value);
            for (auto& v : voices)
            {
                auto& bank = v.getLFOBank();
                while (bank.getActiveCount() < count)
                    bank.addLFO();
                while (bank.getActiveCount() > count)
                    bank.removeLFO();
            }
            applyOscLevels();
            return;
        }

        // ENV count change
        if (id == ModParam::ENV_COUNT)
        {
            int count = juce::jlimit(0, ModParam::MAX_ENVS, (int)value);
            for (auto& v : voices)
            {
                auto& chain = v.getEnvelopeChain();
                while (chain.getStageCount() < count)
                {
                    Envelope::Params p;
                    chain.addEnvelope(p);
                }
                while (chain.getStageCount() > count)
                    chain.removeLastEnvelope();
            }
            // New stages take the stored settings.
            for (int e = 0; e < count; ++e)
            {
                for (int p = ModParam::ENV_ATK; p <= ModParam::ENV_TARGET; ++p)
                    applyModParameter(ModParam::envParamId(e, p), flatParams[(size_t) ModParam::envParamId(e, p)]);
                applyExtraEnvCurves(e);
            }
            return;
        }
    }

public:
    void setTempo(float bpm) override
    {
        tempoBpm = bpm;
        for (auto& v : voices)
            v.getLFOBank().setTempo(bpm);
    }

private:
    double sampleRate = 44100.0;
    float outputLevel = 0.8f;
    float unisonGain = 1.0f;
    float tempoBpm = 120.0f;
    float bendAmount = 0.0f;
    float modWheelValue = 0.0f;
    float modWheelNow = 0.0f;       // smoothed per block
    bool legato = false;
    int initialVoiceCount = 8;

    VoiceAllocator allocator;
    std::vector<UnifiedVoice> voices;
    std::array<float, UnifiedParam::MAX_ID> flatParams {};
    ArchitecturePresetId currentPresetId = ArchitecturePresetId::MonoVA;
    // The page cell each modulation source (LFO 1-4, the extra envelopes) is routed to, -1 = none.
    std::array<int, UnifiedVoice::NUM_MOD_SOURCES> routeId { { -1, -1, -1, -1, -1 } };
    int envDestIndex = 0;       // the extra envelopes' destination as a UnifiedVoice::LfoDest index

    std::array<int, MONO_STACK_SIZE> monoStack {};
    int monoStackCount = 0;
    int monoNote = -1;
    float monoVelocity = 1.0f;
    float monoPressure = 0.0f;
};
