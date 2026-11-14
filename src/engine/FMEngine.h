#pragma once

#include "EngineBase.h"
#include "VoiceAllocator.h"
#include "FMVoice.h"
#include <vector>

class FMEngine : public EngineBase
{
public:
    enum Param
    {
        Op1Ratio = 0, Op1Level, Op1Detune, Op1Feedback,
        Op1Attack, Op1Decay, Op1Sustain, Op1Release,
        Op2Ratio = 10, Op2Level, Op2Detune, Op2Feedback,
        Op2Attack, Op2Decay, Op2Sustain, Op2Release,
        Op3Ratio = 20, Op3Level, Op3Detune, Op3Feedback,
        Op3Attack, Op3Decay, Op3Sustain, Op3Release,
        Op4Ratio = 30, Op4Level, Op4Detune, Op4Feedback,
        Op4Attack, Op4Decay, Op4Sustain, Op4Release,
        Op5Ratio = 40, Op5Level, Op5Detune, Op5Feedback,
        Op5Attack, Op5Decay, Op5Sustain, Op5Release,
        Op6Ratio = 50, Op6Level, Op6Detune, Op6Feedback,
        Op6Attack, Op6Decay, Op6Sustain, Op6Release,
        AlgorithmParam = 60,
        MasterLevel = 61,
        TuneParam = 62,         // semitones
        BendRangeParam = 63,    // semitones
        FilterBase = 100        // 100-115: the per-voice filter (VoiceFilterSection offsets)
    };

    FMEngine(int maxVoices = 8)
        : allocator(maxVoices)
    {
        voices.resize((size_t) maxVoices);

        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            voices[(size_t) voiceIdx].noteOn(v.note, v.velocity);
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            voices[(size_t) voiceIdx].noteOff();
        });

        // A new note takes a silent voice while there is one: release tails ring out.
        allocator.setVoiceSoundingQuery([this](int voiceIdx) { return voices[(size_t) voiceIdx].isActive(); });

        // Init patch: a two-operator sound (2 → 1) with the other operators silent, so adding an
        // operator is a matter of raising its level. Every page parameter is seeded so that
        // getParameter is always the true value (and later sets never allocate).
        for (auto& page : pages())
            for (auto& p : page.params)
                if (p.isValid())
                    setParameter(p.id, p.defaultVal);
        filterSection.applyAll(VoiceFilterSection::each(voices));
        lfoSection.applyAll(EngineLfoSection::each(voices));
    }

    static const ParamPages& pages()
    {
        static const char* const algos[] = { "6>1", "2+3", "3x2", "2x3", "1>3", "ADD", "2+4", "4+2" };
        static const ParamPages list = build(algos);
        return list;
    }

    // The LFO destinations: the original eight, then every continuous cell of the pages. The
    // operator cells carry the operator's number (LVL1, RAT1, DET1, FDB1, ATK1 ...).
    // Not offered: ALGO (a choice), TUNE and BEND (whole semitones; PTCH is the pitch), LEVL (the
    // level of the mixed voices; AMP is each voice's), and what the filter leaves out.
    static const LfoDestList& lfoDests(const ParamPages* enginePages = nullptr)
    {
        if (enginePages == nullptr)
            pages();                    // the list is made on the way (build())
        static const LfoDestList list = makeLfoDests(*enginePages);
        static const bool registered = (list.registerGroups(), true);
        juce::ignoreUnused(registered);
        return list;
    }

    static LfoDestList makeLfoDests(const ParamPages& enginePages)
    {
        static const char* const names[6][6] = {
            { "DET1", "DET2", "DET3", "DET4", "DET5", "DET6" }, { "FDB1", "FDB2", "FDB3", "FDB4", "FDB5", "FDB6" },
            { "ATK1", "ATK2", "ATK3", "ATK4", "ATK5", "ATK6" }, { "DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6" },
            { "SUS1", "SUS2", "SUS3", "SUS4", "SUS5", "SUS6" }, { "REL1", "REL2", "REL3", "REL4", "REL5", "REL6" } };
        LfoDestList l(FMVoice::lfoDestLabels(), FMVoice::NumLfoDests);
        l.group();
        for (int op = 0; op < 6; ++op) l.add(enginePages, op * 10 + 1);     // LVL1..6 (FMMIX)
        l.group();
        for (int op = 0; op < 6; ++op) l.add(enginePages, op * 10 + 0);     // RAT1..6 (RATIO)
        for (int op = 0; op < 6; ++op)
        {
            l.group();
            for (int cell = 2; cell <= 7; ++cell)
                l.add(enginePages, op * 10 + cell, names[cell - 2][op]);
        }
        VoiceFilterSection::addLfoDests(l, enginePages, FilterBase);
        l.addCross();
        // The mod strip until the STRIP page says otherwise: the modulation index up to three
        // times (from the strip INDX counts double: +100 = x 3) and a quarter-tone of vibrato.
        l.stripScale(FMVoice::LfoIndex, STRIP_INDEX_SCALE);
        l.stripRoute(0, FMVoice::LfoIndex, 1.0f);
        l.stripVibrato(0.25f);
        return l;
    }

    ParamPages getParamPages() const override
    {
        ParamPages out = pages();
        filterSection.updateModeCell(out);
        return out;
    }

    void modWheel(float value) override { wheel = juce::jlimit(0.0f, 1.0f, value); }
    void setTempo(float bpm) override { lfoSection.setTempo(bpm, EngineLfoSection::each(voices)); }

    void aftertouch(int note, float pressure) override { allocator.setAftertouch(note, pressure); }

    void pitchBend(float amount) override
    {
        bend = juce::jlimit(-1.0f, 1.0f, amount);
        updatePitch();
    }

    EngineType getType() const override { return EngineType::FM; }
    juce::String getName() const override { return "FM Synth"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr;
        for (auto& v : voices)
            v.setSampleRate(sr);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        int numSamples = buffer.getNumSamples();

        // The mod strip: the routes of the STRIP page (by default the modulation index and
        // vibrato), and the cutoff by the filter's WHL amount.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, sampleRate);
        lfoSection.stripBlock(wheelNow, numSamples, sampleRate, EngineLfoSection::each(voices),
                              [this](int key) { setParameter(key, getParameter(key)); });

        // Per-voice pressure: FM depth, volume and filter cutoff.
        for (int v = 0; v < (int) voices.size() && v < allocator.getVoiceCount(); ++v)
        {
            float pressure = allocator.getAftertouch(v);
            AftertouchRouter::ModValues m;
            if (pressure > 0.0f)
                m = (aftertouchRouter ? *aftertouchRouter : allocator.getAftertouchRouter()).computeModulation(pressure);
            voices[(size_t) v].setModSensitivity(juce::jlimit(0.0f, 4.0f, 1.0f + m.fmDepthMod + m.timbreMod));
            voices[(size_t) v].setGain(juce::jlimit(0.0f, 2.0f, 1.0f + m.volumeMod));
            voices[(size_t) v].getFilter().setExternalOctaves(filterSection.externalOctaves(wheelNow, m.cutoffMod));
        }

        for (int i = 0; i < numSamples; ++i)
        {
            // An LFO routed to PAN moves its own voice between the channels (balance law: the
            // centre is unchanged, so a sound without that routing is exactly as before).
            float sampleL = 0.0f, sampleR = 0.0f;
            for (auto& voice : voices)
            {
                if (! voice.isActive())
                    continue;
                // LFOs on page cells: this voice's own value of each (as of its last sample).
                if (voice.getLfos().keyed())
                    voice.getLfos().forEachKeyed([&voice](int key, float value) { applyVoiceParam(voice, key, value); });
                const float s = voice.process();
                const float pan = voice.getLfoPan();
                sampleL += pan > 0.0f ? s * (1.0f - pan) : s;
                sampleR += pan < 0.0f ? s * (1.0f + pan) : s;
            }

            sampleL *= masterLevel * HEADROOM;
            sampleR *= masterLevel * HEADROOM;

            if (right)
            {
                left[i] += sampleL;
                right[i] += sampleR;
            }
            else
            {
                left[i] += 0.5f * (sampleL + sampleR);
            }
        }

        lfoSection.endBlock(numSamples, EngineLfoSection::each(voices));
    }

    void releaseResources() override
    {
        for (auto& v : voices)
            v.reset();
    }

    void noteOn(int note, float velocity, int /*voiceIndex*/) override
    {
        allocator.noteOn(note, velocity);
    }

    void noteOff(int note, int /*voiceIndex*/) override
    {
        allocator.noteOff(note);
    }

    void allNotesOff() override
    {
        allocator.allNotesOff();
        for (auto& v : voices)
            v.reset();
    }

    // ---- Per-stage envelope curves (StageCurveParam): operators 1-6 are envelope slots 0-5 -----
    int getStageCurveParam(ParamId cellId) const override
    {
        const int op = cellId / 10, cell = cellId % 10;
        if (cellId >= 0 && op < 6 && (cell == 4 || cell == 5 || cell == 7))
            return StageCurveParam::id(op, cell == 4 ? StageCurveParam::Attack
                                                     : (cell == 5 ? StageCurveParam::Decay : StageCurveParam::Release));
        return filterSection.stageCurveParamFor(cellId);
    }

private:
    StageCurves stageCurves;

    void setStageCurve(ParamId id, float value)
    {
        stageCurves.set(id, value);
        const int op = StageCurveParam::envelopeOf(id);
        if (op >= 6)
            return;
        for (auto& voice : voices)
        {
            Envelope::Params p = voice.getOp(op).getEnvelope().getParams();
            stageCurves.applyTo(p, op);
            voice.getOp(op).getEnvelope().setParams(p);
        }
    }

public:
    void setParameter(ParamId id, float value) override
    {
        if (filterSection.owns(id))
        {
            filterSection.set(id, value, VoiceFilterSection::each(voices));
            lfoSection.baseChanged(id, filterSection.get(id), EngineLfoSection::each(voices));
            return;
        }
        if (lfoSection.owns(id))
        {
            // A cell an LFO has let go of goes back to its set value on every voice.
            const int released = lfoSection.set(id, value, EngineLfoSection::each(voices),
                                                [this](int key) { return getParameter(key); });
            if (released >= 0)
                setParameter(released, getParameter(released));
            return;
        }
        if (StageCurveParam::owns(id))
        {
            setStageCurve(id, value);
            return;
        }
        if (id == AlgorithmParam)
            value = (float) juce::jlimit(0, (int) FMVoice::Algorithm::Count - 1, (int) std::lround(value));
        params[id] = value;
        applyParameter(id, value);
        lfoSection.baseChanged(id, value, EngineLfoSection::each(voices));
    }

    float getParameter(ParamId id) const override
    {
        if (filterSection.owns(id))
            return filterSection.get(id);
        if (lfoSection.owns(id))
            return lfoSection.get(id);
        if (StageCurveParam::owns(id))
            return stageCurves.get(id);
        auto it = params.find(id);
        return it != params.end() ? it->second : 0.0f;
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (filterSection.owns(id))
            return VoiceFilterSection::name(id - FilterBase);
        if (EngineLfoSection::owns(id))
            return EngineLfoSection::name(id);
        int opIdx = id / 10;
        int paramIdx = id % 10;
        if (opIdx >= 0 && opIdx < 6)
        {
            juce::String opName = "Op" + juce::String(opIdx + 1) + " ";
            switch (paramIdx)
            {
                case 0: return opName + "Ratio";
                case 1: return opName + "Level";
                case 2: return opName + "Detune";
                case 3: return opName + "Feedback";
                case 4: return opName + "Attack";
                case 5: return opName + "Decay";
                case 6: return opName + "Sustain";
                case 7: return opName + "Release";
                default: break;
            }
        }
        if (id == AlgorithmParam) return "Algorithm";
        if (id == MasterLevel) return "Master Level";
        if (id == TuneParam) return "Tune";
        if (id == BendRangeParam) return "Bend Range";
        return "Param " + juce::String(id);
    }

    int getParameterCount() const override { return 64; }

    void setAlgorithm(FMVoice::Algorithm algo)
    {
        for (auto& v : voices)
            v.setAlgorithm(algo);
    }

    FMVoice& getVoice(int index) { return voices[(size_t) index]; }
    VoiceAllocator& getAllocator() { return allocator; }

private:
    void applyParameter(ParamId id, float value)
    {
        if (id == AlgorithmParam)
        {
            int algoIdx = juce::jlimit(0, (int) FMVoice::Algorithm::Count - 1, (int) value);
            setAlgorithm(static_cast<FMVoice::Algorithm>(algoIdx));
            return;
        }

        if (id == MasterLevel)
        {
            masterLevel = value;
            return;
        }

        if (id == TuneParam || id == BendRangeParam)
        {
            (id == TuneParam ? tune : bendRange) = value;
            updatePitch();
            return;
        }

        int opIdx = id / 10;
        int paramIdx = id % 10;
        if (opIdx < 0 || opIdx >= 6)
            return;

        for (auto& voice : voices)
        {
            auto& op = voice.getOp(opIdx);
            switch (paramIdx)
            {
                case 0: op.setRatio(value); break;
                case 1: op.setLevel(value); break;
                case 2: op.setDetune(value); break;
                case 3: op.setFeedback(value); break;
                case 4:
                {
                    Envelope::Params p = op.getEnvelope().getParams();
                    p.attack = value;
                    op.getEnvelope().setParams(p);
                    break;
                }
                case 5:
                {
                    Envelope::Params p = op.getEnvelope().getParams();
                    p.decay = value;
                    op.getEnvelope().setParams(p);
                    break;
                }
                case 6:
                {
                    Envelope::Params p = op.getEnvelope().getParams();
                    p.sustain = value;
                    op.getEnvelope().setParams(p);
                    break;
                }
                case 7:
                {
                    Envelope::Params p = op.getEnvelope().getParams();
                    p.release = value;
                    op.getEnvelope().setParams(p);
                    break;
                }
                default: break;
            }
        }
    }

    // One voice's own value of a page cell (an operator's or the filter's): what an LFO routed
    // to that cell sets, sample by sample. Envelope times act on the stage that is running.
    static void applyVoiceParam(FMVoice& voice, ParamId id, float value)
    {
        if (id >= FilterBase)
        {
            VoiceFilterSection::applyTo(voice.getFilter(), id - FilterBase, value);
            return;
        }
        if (id < 0 || id >= 60)
            return;
        auto& op = voice.getOp(id / 10);
        switch (id % 10)
        {
            case 0: op.setRatio(value); break;
            case 1: op.setLevel(value); break;
            case 2: op.setDetune(value); break;
            case 3: op.setFeedback(value); break;
            case 4: op.getEnvelope().setAttack(value); break;
            case 5: op.getEnvelope().setDecay(value); break;
            case 6: op.getEnvelope().setSustain(value); break;
            case 7: op.getEnvelope().setRelease(value); break;
            default: break;
        }
    }

    void updatePitch()
    {
        float mul = std::exp2((tune + bend * bendRange) / 12.0f);
        for (auto& v : voices)
            v.setPitchMul(mul);
    }

    // MIX (operator mixer on knobs 1-6), RATIO, then one page per operator.
    static ParamPages build(const char* const* algos)
    {
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;
        static const char* const lvl[] = { "LVL1", "LVL2", "LVL3", "LVL4", "LVL5", "LVL6" };
        static const char* const rat[] = { "RAT1", "RAT2", "RAT3", "RAT4", "RAT5", "RAT6" };
        static const char* const opNames[] = { "OP1", "OP2", "OP3", "OP4", "OP5", "OP6" };
        static const float defLevel[] = { 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f };

        ParamPages out;

        ParamPage mix; mix.name = "FMMIX"; mix.section = "FM";
        ParamPage ratio; ratio.name = "RATIO"; ratio.section = "FM";
        for (int op = 0; op < 6; ++op)
        {
            mix.params[(size_t) op] = { lvl[op], op * 10 + 1, 0, 1, defLevel[op], L };
            ratio.params[(size_t) op] = { rat[op], op * 10 + 0, 0.5f, 16, 1, L, "ratio" };
        }
        mix.params[6] = { "ALGO", AlgorithmParam, 0, 7, 0, C, "", algos, 8 };
        mix.params[7] = { "LEVL", MasterLevel, 0, 1, 0.8f, L };
        ratio.params[6] = { "TUNE", TuneParam, -24, 24, 0, I, "st" };
        ratio.params[7] = { "BEND", BendRangeParam, 0, 24, 2, I, "st" };
        out.push_back(mix);
        out.push_back(ratio);

        for (int op = 0; op < 6; ++op)
        {
            ParamPage p; p.name = opNames[op]; p.section = "OPS";
            int b = op * 10;
            p.params = {{
                { "RATI", b + 0, 0.5f, 16, 1, L, "ratio" },
                { "LEVL", b + 1, 0, 1, defLevel[op], L },
                { "DETN", b + 2, -50, 50, 0, L, "ct" },
                { "FDBK", b + 3, 0, 1, 0, L },
                { "ATK",  b + 4, 0.001f, 10, 0.01f, G, "s" },
                { "DEC",  b + 5, 0.001f, 10, 0.3f, G, "s" },
                { "SUS",  b + 6, 0, 1, 0.7f, L },
                { "REL",  b + 7, 0.001f, 10, 0.3f, G, "s" } }};
            out.push_back(p);
        }
        VoiceFilterSection::appendPages(out, FilterBase);
        EngineLfoSection(lfoDests(&out)).appendPages(out);
        return out;
    }

    static constexpr float HEADROOM = 0.7f;
    static constexpr float STRIP_INDEX_SCALE = 2.0f;    // a strip route to INDX at +100 triples the index

    double sampleRate = 44100.0;
    float masterLevel = 0.8f;
    float tune = 0.0f;
    float bend = 0.0f;
    float bendRange = 2.0f;
    float wheel = 0.0f, wheelNow = 0.0f;      // the strip as sent, and smoothed per block
    VoiceFilterSection filterSection { FilterBase };
    EngineLfoSection lfoSection { lfoDests() };
    VoiceAllocator allocator;
    std::vector<FMVoice> voices;
    ParamMap params;
};
