#pragma once

#include "FMOperator.h"
#include "VoiceFilterSection.h"
#include "ModRouter.h"
#include <algorithm>
#include <array>
#include <cmath>

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

    // The destinations the voice reads by name: indices of the engine's table (FMEngine::lfoDests),
    // in the order of the DEST cells. The first nine are the list as it always was; the engine's
    // page cells follow (LVL1 .. from 9, RAT1 .. from 15, each operator's DETn FDBn ATKn DECn SUSn
    // RELn from 21, the filter's from 57, LRAT LDPT at 67), then what the voice takes per sample
    // again: the operators' positions, and after them the envelopes' times and the operators'
    // phases.
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // per sample: a frequency ratio (+-12 semitones, amount squared)
        LfoAmp,         // per sample: a gain, 0..4 (an audio-rate slot may take it to -4)
        LfoPan,         // per sample: +-1
        LfoCutoff,      // per sample: octaves (+-4 at full depth)
        LfoResonance,   // +-0.5
        LfoIndex,       // per sample: the modulation index, every modulator's effect x 0..4 (1 = as set)
        LfoFeedback,    // per sample: +-1 on every operator's feedback (0..1)
        LfoDetune,      // cents: operators 1, 3, 5 up and 2, 4, 6 down (50 at full depth)
        NumLfoDests,
        LfoPosition = 69,   // POS1 .. POS6, per sample: an operator's place in its table, 0..1
        LfoEnvTime = 75,    // ETIM: every operator's envelope time, a factor
        LfoFilterTime,      // FTIM: the filter envelope's time, a factor
        LfoPhase,           // PM1 .. PM6, per sample: radians added to one operator's phase
        LfoPhaseAll = LfoPhase + NUM_OPS,   // PMAL: ... to every operator's
        LfoCount
    };

    // The voice's own sources, in the order the engine publishes them (FMEngine::sources):
    // OP1 .. OP6, each operator's output (under its envelope, level and velocity), at audio rate.
    enum Published { PubOp = 0 };

    static constexpr float LFO_DETUNE_CENTS = 50.0f;

    void setSampleRate(double sr)
    {
        for (auto& op : ops)
            op.setSampleRate(sr);
        filter.setSampleRate(sr);
        mods.prepare(sr);
        declick.setSampleRate(sr);
        levelStep = (float) (1.0 / (0.004 * sr));
    }

    void noteOn(const mod::Router& router, int note, float velocity)
    {
        // (Whatever is routed is read before the note's first sample: mod::Voice::tick.)
        // (The taps OP1 .. OP6 begin from silence: nothing of the note before is handed over.)
        mods.noteOn(router, note, velocity);
        noteOn(note, velocity);
    }

    // The voice by itself, with no router (a test's voice): the plain sound.
    void noteOn(int note, float velocity)
    {
        if (active)
            declick.arm();      // struck again while sounding: the phase restart must not jump
        filter.noteOn(note, velocity, active);
        for (auto& op : ops)
            op.noteOn(note, velocity);
        active = true;
        struck = true;
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
    mod::Voice& getMod() { return mods; }

    void setPitchMul(float m) { for (auto& op : ops) op.setPitchMul(m); }
    void setLevel(float l) { level = l; }               // the engine's LEVL

    // One sub-block of n samples, added to left / right. pressure: this voice's pad;
    // setCell(voice, dest, value) sets a page cell of this voice (a control-rate destination of
    // the router: an operator's level, ratio, envelope ..., a filter cell).
    template <typename SetCell>
    void render(const mod::Router& router, float* left, float* right, int n, float pressure, const SetCell& setCell)
    {
        // ENV is operator 1's envelope (a carrier in every algorithm). (OP1 .. OP6, the voice's own
        // sources, reach the slots of this sub-block in tick(): what each tapped operator gave
        // one whole sub-block ago, sample for sample, kept by the router - mod::Voice::publishAudio.)
        const bool moved = mods.tick(router, n, ops[0].getEnvelope().getCurrentLevel(), filter.getEnvelope().getCurrentLevel(), pressure);

        // They are kept only while a slot reads them.
        unsigned taps = 0;
        for (int k = 0; k < NUM_OPS; ++k)
            if (router.wants(PubOp + k))
                taps |= 1u << k;
        if (taps != tapped)
            tap(taps);

        if (! moved && taps == 0)
        {
            // Nothing moves this voice: the operators as they are without any modulation.
            if ((held | heldPlaces) != 0)
                letGo();
            for (int i = 0; i < n && active; ++i)
            {
                const float s = processSample(1.0f);
                left[i] += s;
                right[i] += s;
            }
            return;
        }

        if (moved)
            mods.forEachControl([&](int dest, float value) { setControl(dest, value, setCell); });
        const float* pitch = moved ? mods.buffer(LfoPitch) : nullptr;
        const float* amp = moved ? mods.buffer(LfoAmp) : nullptr;
        const float* pan = moved ? mods.buffer(LfoPan) : nullptr;
        const float* cutoff = moved ? mods.buffer(LfoCutoff) : nullptr;
        const float* index = moved ? mods.buffer(LfoIndex) : nullptr;
        const float* feedback = moved ? mods.buffer(LfoFeedback) : nullptr;
        const float* phaseAll = moved ? mods.buffer(LfoPhaseAll) : nullptr;
        const float* phase[NUM_OPS];
        const float* place[NUM_OPS];
        bool anyPhase = phaseAll != nullptr;
        unsigned placed = 0;
        for (int k = 0; k < NUM_OPS; ++k)
        {
            phase[k] = moved ? mods.buffer(LfoPhase + k) : nullptr;
            place[k] = moved ? mods.buffer(LfoPosition + k) : nullptr;
            anyPhase = anyPhase || phase[k] != nullptr;
            if (place[k] != nullptr)
                placed |= 1u << k;
        }

        // What the operators are given per sample: the pitch (with DETN's share, worked out once
        // a sub-block), the index, the feedback and the phases.
        const bool toOps = pitch || index || feedback || anyPhase || detuneCents != 0.0f;
        float up = 1.0f, down = 1.0f;
        if (detuneCents != 0.0f)
        {
            up = std::exp2(detuneCents / 1200.0f);
            down = 1.0f / up;
        }
        // What is no longer moved goes back to the state without it.
        if (! toOps && (held & HeldOps))
            for (auto& op : ops)
            {
                op.clearLfo();
                op.setExternal(0.0f);
            }
        if (cutoff == nullptr && (held & HeldCutoff))
            filter.setLfoOctaves(0.0f);
        for (int k = 0; k < NUM_OPS; ++k)
            if ((heldPlaces & ~placed) & (1u << k))
                ops[(size_t) k].releasePosition();
        held = (toOps ? (unsigned) HeldOps : 0u) | (cutoff != nullptr ? (unsigned) HeldCutoff : 0u);
        heldPlaces = placed;

        for (int i = 0; i < n && active; ++i)
        {
            if (toOps)
            {
                const float ratio = pitch ? pitch[i] : 1.0f;
                const float factor = index ? index[i] : 1.0f;
                const float offset = feedback ? feedback[i] : 0.0f;
                const float all = phaseAll ? phaseAll[i] : 0.0f;
                for (int k = 0; k < NUM_OPS; ++k)
                {
                    ops[(size_t) k].setLfo(ratio * ((k & 1) == 0 ? up : down), factor, offset);
                    ops[(size_t) k].setExternal(phase[k] ? phase[k][i] + all : all);
                }
            }
            if (placed != 0)
                for (int k = 0; k < NUM_OPS; ++k)
                    if (place[k])
                        ops[(size_t) k].drivePosition(place[k][i]);
            if (cutoff)
                filter.setLfoOctaves(cutoff[i]);
            const float s = processSample(amp ? amp[i] : 1.0f);
            if (taps != 0)
                for (int k = 0; k < NUM_OPS; ++k)
                    if (taps & (1u << k))
                        opOut[(size_t) k][(size_t) i] = ops[(size_t) k].lastOutput();
            // PAN moves its own voice between the channels (balance law: the centre is unchanged).
            const float p = pan ? pan[i] : 0.0f;
            left[i] += p > 0.0f ? s * (1.0f - p) : s;
            right[i] += p < 0.0f ? s * (1.0f + p) : s;
        }
        // What the tapped operators made in this sub-block, for the slots of the next. (A voice
        // that ended within it is not heard again before a note-on, which starts from silence.)
        if (taps != 0)
            for (int k = 0; k < NUM_OPS; ++k)
                if (taps & (1u << k))
                    mods.publishAudio(PubOp + k, opOut[(size_t) k].data(), n);
    }

    // A control-rate destination of the router: the resonance, DETN, the envelopes' times, or one
    // of this voice's page cells. (A destination that is let go arrives here once with its set
    // value.)
    template <typename SetCell>
    void setControl(int dest, float value, const SetCell& setCell)
    {
        if (dest == LfoResonance)
            filter.setResonanceMod(value);
        else if (dest == LfoDetune)
            detuneCents = value;
        else if (dest == LfoEnvTime)
            setEnvTimeScale(value);
        else if (dest == LfoFilterTime)
            filter.setEnvTimeScale(value);
        else
            setCell(*this, dest, value);
    }

    // ETIM: every stage time of every operator's envelope times this. Under a sounding note the
    // running stage goes on from where it is (Envelope::setTimeScale).
    void setEnvTimeScale(float scale)
    {
        for (auto& op : ops)
            op.getEnvelope().setTimeScale(scale);
    }

    // The voice by itself: one sample of the plain sound.
    float process() { return processSample(1.0f); }

    // One sample. amp: the gain the router has for the level (1 = none).
    float processSample(float amp)
    {
        if (!active)
            return 0.0f;

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

        // The LEVL cell is moved to over a few milliseconds under a sounding note (it arrives
        // between blocks: a jump would be a click); the first sample of a note takes it as it
        // stands. (What the router gives for the level it has led itself.)
        if (struck)
        {
            struck = false;
            levelNow = level;
        }
        else if (levelNow != level)
            levelNow = dsp::towards(levelNow, level, levelStep);
        return filter.process(declick.process(output)) * amp * levelNow;
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
    // Which of the sources OP1 .. OP6 are kept: one that is no longer read is taken back from the
    // router (one that a slot begins to read starts from silence there).
    void tap(unsigned taps)
    {
        for (int k = 0; k < NUM_OPS; ++k)
            if ((tapped & (1u << k)) && ! (taps & (1u << k)))
                mods.publishAudio(PubOp + k, nullptr, 0);
        tapped = taps;
    }

    // The operators and the filter with nothing of the router's on them (the plain path's entry).
    void letGo()
    {
        if (held & HeldOps)
            for (auto& op : ops)
            {
                op.clearLfo();
                op.setExternal(0.0f);
            }
        if (held & HeldCutoff)
            filter.setLfoOctaves(0.0f);
        for (int k = 0; k < NUM_OPS; ++k)
            if (heldPlaces & (1u << k))
                ops[(size_t) k].releasePosition();
        held = 0;
        heldPlaces = 0;
    }

    std::array<FMOperator, NUM_OPS> ops;
    VoiceFilter filter;      // the whole voice (all carriers) through one filter
    dsp::Declick declick;    // on the operators' sum, before the filter (whose state smears a jump)
    Algorithm algorithm = Algorithm::Algo1;
    bool active = false;
    float level = 1.0f;
    float levelNow = 1.0f, levelStep = 0.005f;
    bool struck = true;

    // This voice's side of the modulation router
    mod::Voice mods;
    enum { HeldOps = 1, HeldCutoff = 2 };
    unsigned held = 0;          // the operators / the filter hold something the router gave them
    unsigned heldPlaces = 0;    // bit k: operator k + 1 reads a position of the router's
    float detuneCents = 0.0f;   // DETN as the router has it
    // The sources OP1 .. OP6 while a slot reads them: what each tapped operator made in the
    // sub-block, handed to the router after it (which keeps them a sub-block)
    unsigned tapped = 0;
    std::array<std::array<float, mod::Router::MAX_CHUNK>, NUM_OPS> opOut {};
};
