#pragma once

#include "../dsp/Envelope.h"
#include "../dsp/EnvelopeChain.h"
#include <algorithm>

// -----------------------------------------------------------------------------
// The amp envelope and its chain (the AMP page's ATK DEC SUS REL and CHAN STGS COMB): one
// voice's side (AmpEnvelope) and the engine's (AmpEnvelopeSection). The plumbing was copied
// in three engines (the macro oscillator, RSN, the wave sequencer); the macro oscillator is the
// first on this header, bit for bit what it was (tests/u4_macro_tests.h, "old against new").
//
//   CHAN  OFF: the amp envelope alone. SER / PAR: the chain's stages give the level, in series
//         or side by side (COMB: SUM or MAX); the amp envelope runs beside them all the same
//         (Policy::Beside: the voice ends when both have ended) or not at all (Policy::Instead)
//   STGS  how many stages; each is a copy of the amp envelope as the cells set it
//   ATK DEC SUS REL, the stage curves, ETIM and a note's EVAR: the amp envelope and every stage
//
// HOW AN ENGINE ADOPTS IT (done: MacroOscEngine.h. To come, each with its own old-against-new
// test: ResonatorVoice.h / ResonatorEngine.h and WaveSeqEngine.h, which still hold the copies)
//  1. Voice: `AmpEnvelope env;` in place of `Envelope ampEnv; EnvelopeChain envChain; bool
//     envChainEnabled;`. setSampleRate -> env.setSampleRate, noteOn / noteOff / reset the same,
//     the per-sample `env = ...; if (! ...isActive()) active = false;` block -> `env.process(active)`,
//     ETIM -> env.setTimeScale(factor), a slot's ATK .. REL on one voice -> AmpEnvelope::setCell.
//     getAmpEnvelope() -> env.envelope(), getEnvelopeChain() -> env.chain().
//  2. Engine: `AmpEnvelopeSection envSection { policy, maxStages };` The cells stay in the
//     engine's map (their ids are the engine's); setParameter hands their values on:
//     CHAN -> setMode, STGS -> setStages, COMB -> setCombine, ATK .. REL -> setCell, a stage
//     curve -> refresh after the amp envelope's params were changed. Each takes `each`, which
//     calls fn(AmpEnvelope&) for every voice (AmpEnvelopeSection::each(voices) for voices with
//     getAmpEnvelopeSection()), and the sample rate where stages are built.
//  What each of the other two does differently, so that adopting keeps its sound:
//   RSN              Policy::Beside, stages 1 .. 4 as here; CHAN has a fourth value RING (the
//                    envelope is the energy alone) that the engine keeps for itself: it calls
//                    setMode(0) for RING and branches before env.process() as it does now. Its
//                    rebuild skips the chain.reset() the macro makes first (the chain is replaced
//                    as a whole either way: no difference in sound).
//   wave sequencer   Policy::Instead: the chain OR the amp envelope runs (noteOn, noteOff and the
//                    level); its STGS is 0 .. 8 and adds stages of fixed params (0.01 / 0.1 / 0.7
//                    / 0.2 s) until the next change of an amp cell copies the amp envelope into
//                    them; CHAN on gives a chain with fewer than two stages two copies. Those three
//                    are the section's Policy::Instead branches; its "chainLevel = 0 at the note"
//                    (the source ENV) stays in the voice. Not adopted here: it needs its own test.
// No allocation: stages are built on the thread that sets the cells (never the audio thread);
// process() is the old per-sample code.
// -----------------------------------------------------------------------------

class AmpEnvelope
{
public:
    enum Cell { Attack = 0, Decay, Sustain, Release };
    enum class Policy { Beside, Instead };

    void setSampleRate(double sr)
    {
        amp.setSampleRate(sr);
        stages.setSampleRate(sr);
    }

    void noteOn()
    {
        if (policy == Policy::Instead && chained())
        {
            stages.noteOn();
            return;
        }
        amp.noteOn();
        if (policy == Policy::Beside && chainOn)
            stages.noteOn();
    }

    void noteOff()
    {
        if (policy == Policy::Instead && chained())
        {
            stages.noteOff();
            return;
        }
        amp.noteOff();
        if (policy == Policy::Beside && chainOn)
            stages.noteOff();
    }

    void reset()
    {
        amp.reset();
        stages.reset();
    }

    // One sample of the level. `sounding` is set false once the envelopes have ended (and left
    // as it is otherwise).
    float process(bool& sounding)
    {
        if (chained())
        {
            const float env = stages.process();
            if (policy == Policy::Beside)
            {
                amp.process();      // kept in step for the note-off
                if (! stages.isActive() && ! amp.isActive())
                    sounding = false;
            }
            else if (! stages.isActive())
                sounding = false;
            return env;
        }
        const float env = amp.process();
        if (! amp.isActive())
            sounding = false;
        return env;
    }

    // The level as it stands (the router's source ENV, the amp envelope's).
    float level() const { return amp.getCurrentLevel(); }

    // ETIM and a note's EVAR: every stage time, the amp envelope's and the chain's, times this.
    void setTimeScale(float scale)
    {
        amp.setTimeScale(scale);
        for (int s = 0; s < stages.getStageCount(); ++s)
            stages.getEnvelopeMut(s).setTimeScale(scale);
    }

    // One voice's own value of ATK .. REL (a slot's): the stage that is running takes it.
    void setCell(int cell, float value)
    {
        auto set = [cell, value](Envelope& e)
        {
            switch (cell)
            {
                case Attack:  e.setAttack(value); break;
                case Decay:   e.setDecay(value); break;
                case Sustain: e.setSustain(value); break;
                case Release: e.setRelease(value); break;
                default: break;
            }
        };
        set(amp);
        for (int s = 0; s < stages.getStageCount(); ++s)
            set(stages.getEnvelopeMut(s));
    }

    // The chain's stages as copies of the amp envelope's params.
    void copyToStages()
    {
        const auto p = amp.getParams();
        for (int s = 0; s < stages.getStageCount(); ++s)
            stages.getEnvelopeMut(s).setParams(p);
    }

    void setChained(bool on)       { chainOn = on; }
    bool isChained() const         { return chainOn; }
    // The stages give the level: CHAN is on and there is a stage.
    bool chained() const           { return chainOn && stages.getStageCount() > 0; }
    void setPolicy(Policy p)       { policy = p; }

    Envelope& envelope()           { return amp; }
    const Envelope& envelope() const { return amp; }
    EnvelopeChain& chain()         { return stages; }

private:
    Envelope amp;
    EnvelopeChain stages;
    bool chainOn = false;
    Policy policy = Policy::Beside;
};

class AmpEnvelopeSection
{
public:
    using Policy = AmpEnvelope::Policy;

    explicit AmpEnvelopeSection(Policy p = Policy::Beside, int maxStageCount = 4, int firstStages = 2)
        : policy(p), maxStages(maxStageCount), stageCount(firstStages) {}

    // The "every voice's envelope" argument, for voices that each have getAmpEnvelopeSection().
    template <typename Voices>
    static auto each(Voices& voices)
    {
        return [&voices](auto&& fn) { for (auto& v : voices) fn(v.getAmpEnvelopeSection()); };
    }

    // The policy goes to every voice (once, at construction).
    template <typename Each>
    void prepareVoices(const Each& each) const
    {
        each([this](AmpEnvelope& e) { e.setPolicy(policy); });
    }

    // CHAN: 0 off, 1 series, 2 parallel. A chain with no stages gets them, made from the amp envelope.
    template <typename Each>
    void setMode(int value, const Each& each, double sampleRate)
    {
        const int mode = std::clamp(value, 0, 2);
        if (mode == 0)
        {
            each([](AmpEnvelope& e) { e.setChained(false); });
            return;
        }
        const auto chainMode = mode == 1 ? EnvelopeChain::ChainMode::Series : EnvelopeChain::ChainMode::Parallel;
        bool empty = true, first = true;
        each([&](AmpEnvelope& e)
        {
            e.setChained(true);
            e.chain().setMode(chainMode);
            if (first)
                empty = e.chain().getStageCount() == 0;
            first = false;
        });
        if (empty)
            setStages(std::max(1, stageCount), each, sampleRate);
    }

    // STGS: the chain is made again with this many copies of the amp envelope.
    template <typename Each>
    void setStages(int value, const Each& each, double sampleRate)
    {
        const int stages = std::clamp(value, 1, maxStages);
        stageCount = stages;
        each([&](AmpEnvelope& e)
        {
            auto& chain = e.chain();
            chain.reset();
            EnvelopeChain fresh;
            fresh.setSampleRate(sampleRate);
            fresh.setMode(chain.getMode());
            fresh.setCombineMode(chain.getCombineMode());
            const auto p = e.envelope().getParams();
            for (int s = 0; s < stages; ++s)
                fresh.addEnvelope(p);
            chain = fresh;
        });
    }

    // COMB: 0 sum, 1 max.
    template <typename Each>
    void setCombine(int value, const Each& each)
    {
        const auto mode = value == 0 ? EnvelopeChain::CombineMode::Sum : EnvelopeChain::CombineMode::Max;
        each([mode](AmpEnvelope& e) { e.chain().setCombineMode(mode); });
    }

    // ATK .. REL as the cells set them: the amp envelope's params, then the stages'.
    template <typename Each>
    void setCell(int cell, float value, const Each& each)
    {
        each([cell, value](AmpEnvelope& e)
        {
            auto p = e.envelope().getParams();
            switch (cell)
            {
                case AmpEnvelope::Attack:  p.attack = value; break;
                case AmpEnvelope::Decay:   p.decay = value; break;
                case AmpEnvelope::Sustain: p.sustain = value; break;
                case AmpEnvelope::Release: p.release = value; break;
                default: break;
            }
            e.envelope().setParams(p);
        });
        refresh(each);
    }

    // The stages take the amp envelope's params again (after a stage curve changed them).
    template <typename Each>
    void refresh(const Each& each)
    {
        each([](AmpEnvelope& e) { e.copyToStages(); });
    }

    int stages() const { return stageCount; }

private:
    Policy policy;
    int maxStages;
    int stageCount;
};
