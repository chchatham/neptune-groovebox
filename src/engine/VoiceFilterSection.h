#pragma once

#include "ParamSpec.h"
#include "EngineLfoSection.h"
#include "../dsp/Envelope.h"
#include "../dsp/filters/FilterSlot.h"
#include <algorithm>
#include <array>
#include <cmath>

// -----------------------------------------------------------------------------
// The per-voice filter shared by the FM, macro-oscillator, wavetable and
// wave-sequencing engines: one FilterSlot (filter A only) and its own ADSR in
// every voice, plus the two parameter pages that drive them.
//
//   FILTR   CUT  RESO ENV  DRIV | TYPE MODE MORP KTRK
//   F ENV   ATK  DEC  SUS  REL  | CURV EVEL VEL  WHL
//
// The cutoff is moved in octaves by the envelope, key tracking, velocity, the
// mod strip and pad pressure, as in the unified voice. The defaults (clean
// low-pass, fully open, nothing routed to it) leave a sound as it was.
// -----------------------------------------------------------------------------

// One voice's filter. No allocation; everything here is safe on the audio thread.
class VoiceFilter
{
public:
    static constexpr float ENV_OCTAVES = 7.0f;       // envelope amount of 1 = this many octaves
    static constexpr float VEL_OCTAVES = 4.0f;
    static constexpr float WHEEL_OCTAVES = 5.0f;
    static constexpr float PRESSURE_OCTAVES = 4.0f;
    static constexpr float MAX_HZ = 20000.0f;
    static constexpr float MIN_HZ = 20.0f;

    VoiceFilter()
    {
        slot.setCutoff(0, baseTarget);
        slot.setResonance(0, 0.0f);
    }

    void setSampleRate(double sr)
    {
        sampleRate = (float) sr;
        maxHz = std::min(MAX_HZ, sampleRate * 0.49f);
        smoothCoeff = 1.0f - std::exp(-1.0f / (0.005f * sampleRate));   // 5 ms
        env.setSampleRate(sr);
        slot.setSampleRate(sr);
        lastHz = -1.0f;
    }

    // voiceWasSounding: a retriggered voice keeps its filter state (no click); an idle one starts clean.
    void noteOn(int note, float velocity, bool voiceWasSounding)
    {
        if (! voiceWasSounding)
        {
            slot.reset();
            env.reset();
            baseSmoothed = baseTarget;
        }
        currentNote = note;
        currentVelocity = velocity;
        updateNoteOctaves();
        env.noteOn();
    }

    void noteOff() { env.noteOff(); }

    void reset()
    {
        slot.reset();
        env.reset();
        baseSmoothed = baseTarget;
    }

    float process(float in)
    {
        if (bypassed)
            return in;

        const float e = env.process();

        if (baseSmoothed != baseTarget)
        {
            const float d = baseTarget - baseSmoothed;
            baseSmoothed += d * smoothCoeff;
            if (std::abs(d) < baseTarget * 1.0e-4f)
                baseSmoothed = baseTarget;
        }

        const float oct = e * envOctaves + noteOctaves + externalOctaves + lfoOctaves;
        float hz = baseSmoothed;
        if (oct != 0.0f)
            hz *= std::exp2(oct);
        hz = std::clamp(hz, MIN_HZ, maxHz);
        if (hz != lastHz)
        {
            slot.setCutoff(0, hz);
            lastHz = hz;
        }
        return slot.process(in);
    }

    // --- settings (the same values go to every voice of an engine) ---
    void setModel(FilterModel m)
    {
        slot.setModel(0, m);
        bypassed = (m == FilterModel::Bypass);
        lastHz = -1.0f;
    }
    void setMode(int m)             { slot.setMode(0, m); }
    void setCutoff(float hz)        { baseTarget = std::clamp(hz, MIN_HZ, MAX_HZ); }
    void setResonance(float r)
    {
        resonance = std::clamp(r, 0.0f, 1.0f);
        slot.setResonance(0, std::clamp(resonance + resonanceMod, 0.0f, 1.0f));
    }
    void setDrive(float d)          { slot.setDrive(0, std::clamp(d, 0.0f, 1.0f)); }
    void setMorph(float m)          { slot.setMorph(std::clamp(m, 0.0f, 1.0f)); }
    void setEnvAmount(float a)      { envAmount = std::clamp(a, -1.0f, 1.0f); updateNoteOctaves(); }
    void setEnvVelocity(float a)    { envVelocity = std::clamp(a, 0.0f, 1.0f); updateNoteOctaves(); }
    void setKeyTrack(float a)       { keyTrack = std::clamp(a, -1.0f, 1.0f); updateNoteOctaves(); }
    void setVelAmount(float a)      { velAmount = std::clamp(a, 0.0f, 1.0f); updateNoteOctaves(); }
    void setEnvParams(const Envelope::Params& p) { env.setParams(p); }

    // Mod strip and pad pressure, already in octaves; set once per block by the engine.
    void setExternalOctaves(float oct) { externalOctaves = oct; }

    // Modulation from the voice's LFOs (EngineLfoSection), set per sample by the voice while an LFO
    // is routed here and back to zero when none is: the cutoff in octaves, the resonance as an
    // offset from its set value. Zero leaves the filter exactly as it is without them.
    void setLfoOctaves(float oct) { lfoOctaves = oct; }
    void setResonanceMod(float offset)
    {
        if (offset == resonanceMod)
            return;
        resonanceMod = offset;
        slot.setResonance(0, std::clamp(resonance + resonanceMod, 0.0f, 1.0f));
    }

    FilterSlot& getSlot() { return slot; }
    Envelope& getEnvelope() { return env; }
    float getCurrentCutoff() const { return lastHz; }

private:
    void updateNoteOctaves()
    {
        noteOctaves = keyTrack * (float) (currentNote - 60) / 12.0f + velAmount * currentVelocity * VEL_OCTAVES;
        envOctaves = envAmount * (1.0f - envVelocity + envVelocity * currentVelocity) * ENV_OCTAVES;
    }

    FilterSlot slot;
    Envelope env;
    float sampleRate = 44100.0f;
    float maxHz = MAX_HZ;
    float smoothCoeff = 0.0045f;
    float baseTarget = MAX_HZ;
    float baseSmoothed = MAX_HZ;
    float lastHz = -1.0f;
    float envAmount = 0.0f, envVelocity = 0.0f, keyTrack = 0.0f, velAmount = 0.0f;
    float noteOctaves = 0.0f, envOctaves = 0.0f, externalOctaves = 0.0f, lfoOctaves = 0.0f;
    float resonance = 0.0f, resonanceMod = 0.0f;
    int currentNote = 60;
    float currentVelocity = 1.0f;
    bool bypassed = false;
};

// The engine side: parameter ids (base + offset), page descriptions, true values, and applying a
// change to every voice's filter.
class VoiceFilterSection
{
public:
    enum Offset
    {
        Type = 0,       // index into FilterSlot's display order
        Mode,           // mode within the type
        Cutoff,         // Hz
        Resonance,      // 0..1
        EnvAmount,      // -1..1 (x 7 octaves)
        KeyTrack,       // -1..1 (1 = cutoff follows the keyboard exactly, -1 = falls as the notes rise)
        Drive,          // 0..1
        VelAmount,      // velocity -> cutoff, 0..1 (x 4 octaves)
        Attack,         // filter envelope, seconds
        Decay,
        Sustain,        // 0..1
        Release,
        Curve,          // -1..1, all three envelope stages
        Morph,          // SEM: low-pass -> notch -> high-pass
        EnvVelocity,    // how much velocity scales the envelope amount, 0..1
        WheelAmount,    // mod strip -> cutoff, -1..1 (x 5 octaves)
        Count
    };

    explicit VoiceFilterSection(int baseParamId) : base(baseParamId)
    {
        const auto pages = makePages(base);
        for (auto& page : pages)
            for (auto& p : page.params)
                values[(size_t) (p.id - base)] = p.defaultVal;
    }

    int id(Offset o) const { return base + (int) o; }
    bool owns(int paramId) const { return (paramId >= base && paramId < base + (int) Count) || isStageCurve(paramId); }
    float get(int paramId) const
    {
        if (isStageCurve(paramId))
            return stageCurves.get(paramId);
        return owns(paramId) ? values[(size_t) (paramId - base)] : 0.0f;
    }

    // The filter envelope's own curve per stage (StageCurveParam slot FILTER_ENV; Shift + turn on
    // ATK / DEC / REL). They add to CURV.
    static bool isStageCurve(int paramId)
    {
        return StageCurveParam::owns(paramId) && StageCurveParam::envelopeOf(paramId) == StageCurveParam::FILTER_ENV;
    }
    // The stage-curve id behind the F ENV page's ATK, DEC or REL cell; -1 for any other id.
    static int stageCurveParamFor(int baseId, int cellId)
    {
        if (cellId == baseId + (int) Attack)  return StageCurveParam::id(StageCurveParam::FILTER_ENV, StageCurveParam::Attack);
        if (cellId == baseId + (int) Decay)   return StageCurveParam::id(StageCurveParam::FILTER_ENV, StageCurveParam::Decay);
        if (cellId == baseId + (int) Release) return StageCurveParam::id(StageCurveParam::FILTER_ENV, StageCurveParam::Release);
        return -1;
    }
    int stageCurveParamFor(int cellId) const { return stageCurveParamFor(base, cellId); }
    float wheelAmount() const { return values[WheelAmount]; }

    // Cutoff offset from the mod strip (0..1) and the pressure routing's cutoff amount, in octaves.
    float externalOctaves(float wheel, float pressureCutoffMod) const
    {
        return wheel * values[WheelAmount] * VoiceFilter::WHEEL_OCTAVES
             + pressureCutoffMod * VoiceFilter::PRESSURE_OCTAVES;
    }

    // The FILTR and F ENV pages for an engine whose filter ids start at baseId.
    static std::array<ParamPage, 2> makePages(int baseId)
    {
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto C = ParamKind::Choice;
        const int b = baseId;
        std::array<ParamPage, 2> out;
        out[0].name = "FILTR"; out[0].section = "FLTR";
        out[0].params = {{
            { "CUT",  b + Cutoff, 20, 20000, 20000, G, "Hz" },
            { "RESO", b + Resonance, 0, 1, 0, L },
            { "ENV",  b + EnvAmount, -1, 1, 0, L },
            { "DRIV", b + Drive, 0, 1, 0, L },
            { "TYPE", b + Type, 0, FilterSlot::NUM_MODELS - 1, 0, C, "", FilterSlot::modelNames(), FilterSlot::NUM_MODELS },
            { "MODE", b + Mode, 0, 3, 0, C, "", FilterSlot::modeNames(FilterModel::SVF), 4 },
            { "MORP", b + Morph, 0, 1, 0, L },
            { "KTRK", b + KeyTrack, -1, 1, 0, L } }};
        out[1].name = "F ENV"; out[1].section = "FLTR";
        out[1].params = {{
            { "ATK",  b + Attack, 0.001f, 10, 0.01f, G, "s" },
            { "DEC",  b + Decay, 0.001f, 10, 0.5f, G, "s" },
            { "SUS",  b + Sustain, 0, 1, 0, L },
            { "REL",  b + Release, 0.001f, 10, 0.3f, G, "s" },
            { "CURV", b + Curve, -1, 1, 0, L },
            { "EVEL", b + EnvVelocity, 0, 1, 0, L },
            { "VEL",  b + VelAmount, 0, 1, 0, L },
            { "WHL",  b + WheelAmount, -1, 1, 0, L } }};
        return out;
    }

    static void appendPages(ParamPages& pages, int baseId)
    {
        for (auto& p : makePages(baseId))
            pages.push_back(p);
    }

    // ---- LFO destinations (EngineLfoSection) ------------------------------------------------
    // CUT and RESO are in every engine's original list. These are the filter's other continuous
    // cells; the names say "filter" where the cell's own name is used elsewhere in an engine.
    // (Not offered: TYPE and MODE are choices, CURV would re-bend the envelope every sample, WHL
    // is the mod strip's own amount.)
    // morphLabel: for an engine whose own list already has a MORP (the macro oscillator).
    static void addLfoDests(LfoDestList& l, const ParamPages& pages, int baseId, const char* morphLabel = nullptr)
    {
        l.group();
        l.add(pages, baseId + EnvAmount);
        l.add(pages, baseId + Drive);
        l.add(pages, baseId + Morph, morphLabel);
        l.add(pages, baseId + KeyTrack);
        l.group();
        l.add(pages, baseId + Attack, "FATK");
        l.add(pages, baseId + Decay, "FDEC");
        l.add(pages, baseId + Sustain, "FSUS");
        l.add(pages, baseId + Release, "FREL");
        l.add(pages, baseId + EnvVelocity);
        l.add(pages, baseId + VelAmount, "FVEL");
    }

    // One voice's filter takes `value` for the cell at `offset` (the per-voice half of set(), for
    // the cells addLfoDests offers; range-checked by the caller or by the filter's own setters).
    static void applyTo(VoiceFilter& f, int offset, float value)
    {
        switch (offset)
        {
            case EnvAmount:   f.setEnvAmount(value); break;
            case Drive:       f.setDrive(value); break;
            case Morph:       f.setMorph(value); break;
            case KeyTrack:    f.setKeyTrack(value); break;
            case Attack:      f.getEnvelope().setAttack(value); break;
            case Decay:       f.getEnvelope().setDecay(value); break;
            case Sustain:     f.getEnvelope().setSustain(value); break;
            case Release:     f.getEnvelope().setRelease(value); break;
            case EnvVelocity: f.setEnvVelocity(value); break;
            case VelAmount:   f.setVelAmount(value); break;
            default: break;
        }
    }

    // The MODE cell offers the modes of the filter type currently chosen.
    void updateModeCell(ParamPages& pages) const
    {
        const auto model = currentModel();
        for (auto& page : pages)
            for (auto& p : page.params)
                if (p.id == base + (int) Mode)
                {
                    p.choices = FilterSlot::modeNames(model);
                    p.numChoices = FilterSlot::modeCount(model);
                    p.maxVal = (float) (p.numChoices - 1);
                }
    }

    FilterModel currentModel() const { return FilterSlot::modelFromIndex((int) values[Type]); }

    // Stores the (range-checked) value and applies it. forEachFilter(fn) must call fn(VoiceFilter&)
    // for every voice of the engine.
    template <typename ForEachFilter>
    void set(int paramId, float value, const ForEachFilter& forEachFilter)
    {
        if (! owns(paramId))
            return;
        const int o = isStageCurve(paramId) ? -1 : paramId - base;
        stageCurves.set(paramId, value);       // ignores every other id

        switch (o)
        {
            case Type:
            {
                value = (float) std::clamp((int) std::lround(value), 0, FilterSlot::NUM_MODELS - 1);
                values[Type] = value;
                const auto model = currentModel();
                // The stored mode always names a mode the new type has.
                values[Mode] = std::min(values[Mode], (float) (FilterSlot::modeCount(model) - 1));
                const int mode = (int) values[Mode];
                forEachFilter([model, mode](VoiceFilter& f) { f.setModel(model); f.setMode(mode); });
                return;
            }
            case Mode:
            {
                value = (float) std::clamp((int) std::lround(value), 0, FilterSlot::modeCount(currentModel()) - 1);
                values[Mode] = value;
                const int mode = (int) value;
                forEachFilter([mode](VoiceFilter& f) { f.setMode(mode); });
                return;
            }
            case Cutoff:
                value = std::clamp(value, VoiceFilter::MIN_HZ, VoiceFilter::MAX_HZ);
                values[(size_t) o] = value;
                forEachFilter([value](VoiceFilter& f) { f.setCutoff(value); });
                return;
            case Resonance:
                value = std::clamp(value, 0.0f, 1.0f);
                values[(size_t) o] = value;
                forEachFilter([value](VoiceFilter& f) { f.setResonance(value); });
                return;
            case EnvAmount:
                value = std::clamp(value, -1.0f, 1.0f);
                values[(size_t) o] = value;
                forEachFilter([value](VoiceFilter& f) { f.setEnvAmount(value); });
                return;
            case KeyTrack:
                value = std::clamp(value, -1.0f, 1.0f);
                values[(size_t) o] = value;
                forEachFilter([value](VoiceFilter& f) { f.setKeyTrack(value); });
                return;
            case Drive:
                value = std::clamp(value, 0.0f, 1.0f);
                values[(size_t) o] = value;
                forEachFilter([value](VoiceFilter& f) { f.setDrive(value); });
                return;
            case VelAmount:
                value = std::clamp(value, 0.0f, 1.0f);
                values[(size_t) o] = value;
                forEachFilter([value](VoiceFilter& f) { f.setVelAmount(value); });
                return;
            case Morph:
                value = std::clamp(value, 0.0f, 1.0f);
                values[(size_t) o] = value;
                forEachFilter([value](VoiceFilter& f) { f.setMorph(value); });
                return;
            case EnvVelocity:
                value = std::clamp(value, 0.0f, 1.0f);
                values[(size_t) o] = value;
                forEachFilter([value](VoiceFilter& f) { f.setEnvVelocity(value); });
                return;
            case WheelAmount:
                values[(size_t) o] = std::clamp(value, -1.0f, 1.0f);   // read once per block by the engine
                return;
            case Attack: case Decay: case Release:
                values[(size_t) o] = std::clamp(value, 0.001f, 10.0f);
                break;
            case Sustain:
                values[(size_t) o] = std::clamp(value, 0.0f, 1.0f);
                break;
            case Curve:
                values[(size_t) o] = std::clamp(value, -1.0f, 1.0f);
                break;
            case -1:        // a stage's own curve
                break;
            default:
                return;
        }

        // An envelope value changed.
        Envelope::Params p;
        p.attack = values[Attack];
        p.decay = values[Decay];
        p.sustain = values[Sustain];
        p.release = values[Release];
        stageCurves.applyTo(p, StageCurveParam::FILTER_ENV, values[Curve]);
        forEachFilter([&p](VoiceFilter& f) { f.setEnvParams(p); });
    }

    // Applies every stored value (used once, when the engine's voices exist).
    template <typename ForEachFilter>
    void applyAll(const ForEachFilter& forEachFilter)
    {
        // Type first: the mode is checked against it.
        set(base + (int) Type, values[Type], forEachFilter);
        for (int o = 1; o < (int) Count; ++o)
            set(base + o, values[(size_t) o], forEachFilter);
    }

    // The "every voice's filter" argument for set / applyAll, for a container of voices that each
    // have getFilter().
    template <typename Voices>
    static auto each(Voices& voices)
    {
        return [&voices](auto&& fn) { for (auto& v : voices) fn(v.getFilter()); };
    }

    static const char* name(int offset)
    {
        static const char* const names[Count] = {
            "Filter Type", "Filter Mode", "Cutoff", "Resonance", "Filter Env", "Key Track", "Filter Drive",
            "Vel > Cutoff", "F.Env Attack", "F.Env Decay", "F.Env Sustain", "F.Env Release", "F.Env Curve",
            "Filter Morph", "Vel > F.Env", "Wheel > Cutoff" };
        return (offset >= 0 && offset < (int) Count) ? names[offset] : "";
    }

private:
    int base;
    std::array<float, Count> values {};
    StageCurves stageCurves;
};
