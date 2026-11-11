#pragma once

#include "FMOperator.h"
#include "VoiceFilterSection.h"
#include "EngineLfoSection.h"
#include <array>
#include <functional>

class FMVoice
{
public:
    static constexpr int NUM_OPS = 6;

    enum class Algorithm
    {
        Algo1,  // 6→5→4→3→2→1 (serial chain)
        Algo2,  // [3→2]→1, [6→5→4]→1 (two chains into carrier)
        Algo3,  // [2→1], [4→3], [6→5] (three stacks of 2)
        Algo4,  // [3→2→1], [6→5→4] (two stacks of 3)
        Algo5,  // 6→{1,2,3}, [5→4] (one modulator on three carriers + a 2-stack)
        Algo6,  // all carriers (additive)
        Algo7,  // [2→1], 3, 4, 5, 6 (one stack + 4 carriers)
        Algo8,  // [4→3→2→1], [6→5] (stack of 4 + stack of 2)
        Count
    };

    // LFO destinations (EngineLfoSection), in the order of the DEST cell: the original ones. The
    // list goes on with the engine's page cells (FMEngine::lfoDests), which the engine sets on
    // this voice's operators and filter sample by sample.
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // +-12 semitones, depth squared
        LfoAmp,         // gain 0..2
        LfoPan,         // +-1
        LfoCutoff,      // +-4 octaves
        LfoResonance,   // +-0.5
        LfoIndex,       // modulation index: every modulator's effect x 0..2
        LfoFeedback,    // +-1 on every operator's feedback (0..1)
        LfoDetune,      // operators 1, 3, 5 up and 2, 4, 6 down by up to 50 cents
        NumLfoDests
    };

    static const char* const* lfoDestLabels()
    {
        static const char* const names[NumLfoDests] = {
            "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "INDX", "FDBK", "DETN" };
        return names;
    }

    static constexpr float LFO_DETUNE_CENTS = 50.0f;

    void setSampleRate(double sr)
    {
        for (auto& op : ops)
            op.setSampleRate(sr);
        filter.setSampleRate(sr);
        lfos.setSampleRate(sr);
        declick.setSampleRate(sr);
    }

    void noteOn(int note, float velocity)
    {
        if (active)
            declick.arm();      // struck again while sounding: the phase restart must not jump
        // An LFO on a page cell is read before the first sample (the engine sets the cell then).
        if (lfos.keyed()) lfos.noteOnAndSample(); else lfos.noteOn();
        filter.noteOn(note, velocity, active);
        for (auto& op : ops)
            op.noteOn(note, velocity);
        active = true;
    }

    void noteOff()
    {
        for (auto& op : ops)
            op.noteOff();
        filter.noteOff();
    }

    bool isActive() const { return active; }

    void setAlgorithm(Algorithm a) { algorithm = a; }
    Algorithm getAlgorithm() const { return algorithm; }

    FMOperator& getOp(int index) { return ops[(size_t) index]; }
    VoiceFilter& getFilter() { return filter; }
    VoiceLfos& getLfos() { return lfos; }

    // -1..1 from LFOs routed to the pan (as of the last processed sample); the engine places the voice.
    float getLfoPan() const { return lfoPan; }

    void setPitchMul(float m) { for (auto& op : ops) op.setPitchMul(m); }
    void setModSensitivity(float s) { for (auto& op : ops) op.setModSensitivity(s); }
    void setGain(float g) { gain = g; }

    float process()
    {
        if (!active)
            return 0.0f;

        if (lfos.anyOn())
            applyLfos();
        else if (lfoApplied)
            clearLfos();

        float output = 0.0f;

        switch (algorithm)
        {
            case Algorithm::Algo1: // 6→5→4→3→2→1
            {
                float o6 = ops[5].process();
                float o5 = ops[4].process(o6);
                float o4 = ops[3].process(o5);
                float o3 = ops[2].process(o4);
                float o2 = ops[1].process(o3);
                output = ops[0].process(o2);
                break;
            }
            case Algorithm::Algo2: // [3→2]→1, [6→5→4]→1
            {
                float o3 = ops[2].process();
                float o2 = ops[1].process(o3);
                float o6 = ops[5].process();
                float o5 = ops[4].process(o6);
                float o4 = ops[3].process(o5);
                output = ops[0].process(o2 + o4);
                break;
            }
            case Algorithm::Algo3: // [2→1], [4→3], [6→5]
            {
                float o2 = ops[1].process();
                float o4 = ops[3].process();
                float o6 = ops[5].process();
                output = ops[0].process(o2) + ops[2].process(o4) + ops[4].process(o6);
                break;
            }
            case Algorithm::Algo4: // [3→2→1], [6→5→4]
            {
                float o3 = ops[2].process();
                float o2 = ops[1].process(o3);
                float o6 = ops[5].process();
                float o5 = ops[4].process(o6);
                output = ops[0].process(o2) + ops[3].process(o5);
                break;
            }
            case Algorithm::Algo5: // 6 modulates carriers 1, 2 and 3; [5→4] is a fourth voice
            {
                float o6 = ops[5].process();
                float o5 = ops[4].process();
                output = ops[0].process(o6) + ops[1].process(o6) + ops[2].process(o6) + ops[3].process(o5);
                break;
            }
            case Algorithm::Algo6: // all additive
            {
                for (auto& op : ops)
                    output += op.process();
                break;
            }
            case Algorithm::Algo7: // [2→1], 3, 4, 5, 6
            {
                float o2 = ops[1].process();
                output = ops[0].process(o2);
                for (int i = 2; i < NUM_OPS; ++i)
                    output += ops[i].process();
                break;
            }
            case Algorithm::Algo8: // [4→3→2→1], [6→5]
            {
                float o4 = ops[3].process();
                float o3 = ops[2].process(o4);
                float o2 = ops[1].process(o3);
                float o6 = ops[5].process();
                output = ops[0].process(o2) + ops[4].process(o6);
                break;
            }
            default:
                break;
        }

        bool anyActive = false;
        for (auto& op : ops)
        {
            if (op.isActive())
            {
                anyActive = true;
                break;
            }
        }
        if (!anyActive)
            active = false;

        return filter.process(declick.process(output)) * gain * lfoGain;
    }

    void reset()
    {
        declick.reset();
        for (auto& op : ops)
            op.reset();
        filter.reset();
        active = false;
    }

private:
    // One sample of the LFOs, sent to wherever they are routed.
    void applyLfos()
    {
        lfos.tick();
        lfoApplied = true;

        const float semis = lfos.pitchSemitones(LfoPitch);
        const float cents = lfos.amount(LfoDetune) * LFO_DETUNE_CENTS;
        const float index = std::clamp(1.0f + lfos.amount(LfoIndex), 0.0f, 4.0f);   // LFOs reach x 2, the strip x 3
        const float feedback = lfos.amount(LfoFeedback);
        float up = 1.0f, down = 1.0f;
        if (semis != 0.0f || cents != 0.0f)
        {
            up = std::exp2((semis * 100.0f + cents) / 1200.0f);
            down = cents != 0.0f ? std::exp2((semis * 100.0f - cents) / 1200.0f) : up;
        }
        for (int i = 0; i < NUM_OPS; ++i)
            ops[(size_t) i].setLfo((i & 1) == 0 ? up : down, index, feedback);

        filter.setLfoOctaves(lfos.amount(LfoCutoff) * VoiceLfos::CUTOFF_OCTAVES);
        filter.setResonanceMod(lfos.amount(LfoResonance) * VoiceLfos::RESONANCE_RANGE);
        lfoGain = std::clamp(1.0f + lfos.amount(LfoAmp), 0.0f, 2.0f);
        lfoPan = std::clamp(lfos.amount(LfoPan), -1.0f, 1.0f);
    }

    void clearLfos()
    {
        for (auto& op : ops)
            op.clearLfo();
        filter.setLfoOctaves(0.0f);
        filter.setResonanceMod(0.0f);
        lfoGain = 1.0f;
        lfoPan = 0.0f;
        lfoApplied = false;
    }

    std::array<FMOperator, NUM_OPS> ops;
    VoiceLfos lfos;
    bool lfoApplied = false;
    float lfoGain = 1.0f;
    float lfoPan = 0.0f;
    VoiceFilter filter;      // the whole voice (all carriers) through one filter
    dsp::Declick declick;    // on the operators' sum, before the filter (whose state smears a jump)
    Algorithm algorithm = Algorithm::Algo1;
    bool active = false;
    float gain = 1.0f;
};
