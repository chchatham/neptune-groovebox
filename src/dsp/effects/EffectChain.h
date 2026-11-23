#pragma once

#include "EffectBase.h"
#include "CellEffect.h"
#include "ModDelay.h"
#include "../DSPUtils.h"
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstring>

// The sounds an effect may ask for beside its own, for one block: the track's input and the
// tracks' sounds (each after its inserts). Whoever runs an effect (a track's insert chain, the
// master chain, a PERF pad) fills one of these and hands it over; the effect says which sound it
// wants (EffectBase::keySource) and gets it through EffectBase::setInput. This is the one way
// another track's sound reaches an effect.
struct KeyFeed
{
    const float* inLeft = nullptr;              // the track's input (null = it has none)
    const float* inRight = nullptr;
    int inFrames = 0;
    const float* const* tapLeft = nullptr;      // the tracks' sounds, by track (null = none)
    const float* const* tapRight = nullptr;
    int taps = 0;
    int frames = 0;                             // samples in each tap
    int self = -1;                              // the track the effect is on: it does not key itself by number

    // `offset`, `count`: the stretch of the block the effect is about to be run on (count < 0: the rest).
    void hand(EffectBase& fx, int offset = 0, int count = -1) const
    {
        const int source = fx.keySource();
        if (source == EffectBase::KEY_SELF)
            return;
        const float* left = nullptr;
        const float* right = nullptr;
        int have = 0;
        if (source == EffectBase::KEY_IN)
        {
            left = inLeft; right = inRight; have = inFrames;
        }
        else if (source >= 0 && source != self && source < taps && tapLeft != nullptr && tapRight != nullptr)
        {
            left = tapLeft[source]; right = tapRight[source]; have = frames;
        }
        offset = std::max(0, offset);
        have = count < 0 ? have - offset : std::min(have - offset, count);
        if (left == nullptr || have <= 0)
            fx.setInput(nullptr, nullptr, 0);
        else
            fx.setInput(left + offset, (right != nullptr ? right : left) + offset, have);
    }
};

/**
 * EffectChain — ordered chain of effects with configurable routing.
 *
 * Routing modes:
 *   Series   — each effect feeds the next in sequence.
 *   Parallel — each effect processes a copy of the dry input; outputs are summed.
 *   Mixed    — first effect in series, then remaining effects in parallel (summed).
 *
 * The chain holds the one dry / wet mix: an effect that gives its wet sound alone
 * (EffectBase::givesWetOnly: every effect of cells, and AMP) is run at mix 1 and mixed here,
 * dry + mix (wet - dry), the mix going to what is asked for (EffectBase::wetWanted: the slot's
 * WET, a MIX cell, modulation) in a straight line, a full swing in MIX_SWING_SECONDS. So WET works
 * on every such effect, also on those with no mix of their own (EQ, GEQ, LMT, AMP), and a mix
 * that stands at 1 costs nothing. The few effects that still mix by themselves (DLY, RVB and HALL,
 * whose arithmetic is pinned; DRV, FLT, WSH, RED, VOC) are run as they were.
 *
 * An effect that asks for a delayed dry (EffectBase::dryDelaySamples: through-zero flanging,
 * chorus, ensemble, vibrato) is mixed with the dry sound delayed by that amount, fractionally
 * and glided (nothing jumped to), from the slot's DryLine: FLG, CHR, ENS and VIB each name the
 * delay they want (moddelay::Effect, THRU) and the chain does the rest. Every effect the app
 * makes sits in a chain, so this is the through-zero that is heard; the modulated delay's core
 * keeps a dry tap of its own for an effect run outside a chain (tests, TrackFx.h's blocks).
 * An effect whose sound is late (EffectBase::latencySamples: LMT, by its LOOK) is mixed with the
 * dry sound as late, so the two are in step (no comb): at WET 0 such a slot is that delay.
 *
 * A send's chain (send mode) returns nothing of what the bus was sent. That is a law of the
 * chain's entrance, not of every slot: the first slot that is on gives mix x wet and no dry;
 * the slots behind it mix against the stage before them exactly as in any chain, and one that
 * is off is a wire. With every slot off the chain returns silence.
 *
 * All internal buffers are pre-allocated in prepare(). No heap allocation on the audio thread.
 */
class EffectChain
{
public:
    enum class Routing { Series, Parallel, Mixed };
    static constexpr float MIX_SWING_SECONDS = CellEffect::MIX_SWING_SECONDS;

    EffectChain() = default;

    // Send mode: the chain is a return, never a copy of what it was given. Every effect runs at
    // mix 1 (pure wet, setWetOnly) and the chain mixes. The first slot that is on is the chain's
    // entrance: mix * wet, with no dry, and switched on or off it fades from or to silence. A
    // slot behind it mixes dry + mix * (wet - dry) against the stage before it and is a wire when
    // off, as in an insert chain. No slot on: silence.
    void setSendMode(bool s)
    {
        sendMode = s;
        for (auto& fx : effects)
            if (s) fx->setWetOnly(true);
    }
    bool isSendMode() const { return sendMode; }

    void prepare(double sampleRate, int blockSize)
    {
        currentSampleRate = sampleRate;
        currentBlockSize = blockSize;

        // Pre-allocate scratch buffers for parallel routing
        // Two scratch buffers per effect slot (L + R) plus accumulator
        scratchL.resize((size_t) blockSize);
        scratchR.resize((size_t) blockSize);
        accumL.resize((size_t) blockSize);
        accumR.resize((size_t) blockSize);

        for (auto& fx : effects)
            fx->prepare(sampleRate, blockSize);
        for (auto& m : mixes)
            m = FRESH;
        for (auto& line : lines)
        {
            line.clear();
            if (line.used())
                line.setRate(sampleRate);
        }
    }

    // keys: what the effects may listen to in this block (null: nothing is handed over; a keyed
    // effect then keys itself).
    void processStereo(float* left, float* right, int numSamples, const KeyFeed* keys = nullptr)
    {
        if (effects.empty())
            return;
        feed = keys;

        switch (routing)
        {
            case Routing::Series:
                processSeriesRouting(left, right, numSamples);
                break;
            case Routing::Parallel:
                processParallelRouting(left, right, numSamples);
                break;
            case Routing::Mixed:
                processMixedRouting(left, right, numSamples);
                break;
        }
        feed = nullptr;
    }

    // kind: what the effect is, as a number of whoever made it (the AudioEngine's EffectType: the
    // chain is the one place that knows what sits in each of its slots); NO_KIND = not said.
    static constexpr int NO_KIND = -1, SAME_KIND = -2;
    void addEffect(std::unique_ptr<EffectBase> fx, int kind = NO_KIND)
    {
        if (currentSampleRate > 0.0 && currentBlockSize > 0)
            fx->prepare(currentSampleRate, currentBlockSize);

        fx->setChained();
        if (sendMode) fx->setWetOnly(true);
        lines.emplace_back(*fx);
        effects.push_back(std::move(fx));
        mixes.push_back(FRESH);
        kinds.push_back(kind);
    }

    // Swap the effect in a slot, keeping its position in the chain (and, unless another is given,
    // what the slot is said to hold).
    void replaceEffect(int index, std::unique_ptr<EffectBase> fx, int kind = SAME_KIND)
    {
        if (index < 0 || index >= (int) effects.size() || !fx)
            return;
        if (currentSampleRate > 0.0 && currentBlockSize > 0)
            fx->prepare(currentSampleRate, currentBlockSize);
        fx->setChained();
        if (sendMode) fx->setWetOnly(true);
        lines[(size_t) index] = DryLine(*fx);
        effects[(size_t) index] = std::move(fx);
        mixes[(size_t) index] = FRESH;
        if (kind != SAME_KIND)
            kinds[(size_t) index] = kind;
    }

    void removeEffect(int index)
    {
        if (index >= 0 && index < (int) effects.size())
        {
            effects.erase(effects.begin() + index);
            mixes.erase(mixes.begin() + index);
            lines.erase(lines.begin() + index);
            kinds.erase(kinds.begin() + index);
        }
    }

    // What a slot was said to hold (NO_KIND: nothing was said, or there is no such slot).
    int getKind(int index) const { return index >= 0 && index < (int) kinds.size() ? kinds[(size_t) index] : NO_KIND; }

    void setRouting(Routing r) { routing = r; }
    Routing getRouting() const { return routing; }

    int getEffectCount() const { return (int) effects.size(); }

    EffectBase* getEffect(int index)
    {
        if (index >= 0 && index < (int) effects.size())
            return effects[(size_t) index].get();
        return nullptr;
    }

    const EffectBase* getEffect(int index) const
    {
        if (index >= 0 && index < (int) effects.size())
            return effects[(size_t) index].get();
        return nullptr;
    }

    void reset()
    {
        for (auto& fx : effects)
            fx->reset();
        for (auto& m : mixes)
            m = FRESH;          // (nothing sounds: the mix is where it is set, with no line to draw)
        for (auto& line : lines)
            line.clear();
    }

    // Where the chain's mix of a slot is now (for a test, or a screen that shows it move); below 0:
    // nothing has sounded through it since it was put in or emptied.
    float getMixNow(int index) const { return index >= 0 && index < (int) mixes.size() ? mixes[(size_t) index] : FRESH; }

private:
    static constexpr float FRESH = -1.0f;
    static constexpr int PIECE = 256;

    // The dry sound of a slot whose effect is late or asks for a delayed dry (through-zero),
    // kept so that it can be mixed as late or as delayed. The delay is fractional and glided:
    // nothing is jumped to. Only such a slot has the memory (made when the effect is put in,
    // never on the audio thread). While the slot's mix stands at 1 and it has no through-zero
    // dry delay, nothing is written to it; a mix that then moves waits until the line holds the
    // dry sound that far back (`held`).
    struct DryLine
    {
        static constexpr int SIZE = 4096;           // power of 2; enough for 20 ms at 192 kHz
        static constexpr int MASK = SIZE - 1;
        // The glide of the dry delay is the modulated delay's own (one pair of numbers: a dry
        // sound that followed its tap by another law would not cancel at the crossing).
        static constexpr float MAX_SLEW = moddelay::Core::MAX_SLEW;
        static constexpr float FOLLOW_SEC = moddelay::Core::FOLLOW_SECONDS;

        std::vector<float> left, right;
        int write = 0;
        int held = 0;            // samples in it since clear
        float current = 0.0f;    // the fractional delay being read (glided per sample)
        float follow = 1.0f;     // smoothing coefficient: 1 - exp(-1 / (FOLLOW_SEC * sampleRate))
        bool fractional = false;  // true: through-zero (fractional, glided); false: integer latency

        DryLine() = default;
        explicit DryLine(const EffectBase& fx)
        {
            if (fx.needsDryLine())
            {
                left.assign((size_t) SIZE, 0.0f);
                right.assign((size_t) SIZE, 0.0f);
                fractional = fx.dryDelaySamples() > 0.0f || fx.latencySamples() == 0;
            }
        }
        bool used() const { return ! left.empty(); }
        void clear()
        {
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            write = 0;
            held = 0;
            current = 0.0f;
        }
        void setRate(double sr)
        {
            follow = 1.0f - std::exp(-1.0f / (FOLLOW_SEC * (float) sr));
        }
        void put(float l, float r)
        {
            left[(size_t)(write & MASK)] = l;
            right[(size_t)(write & MASK)] = r;
            write = (write + 1) & MASK;
            if (held < SIZE) ++held;
        }
        // Nothing of the dry sound is kept from here on: the next use begins anew (it goes to
        // its delay at once and waits until the line holds the dry sound that far back).
        void rest()
        {
            held = 0;
            current = 0.0f;
        }
    };

    // The mix a slot is going to; 1 for an effect that mixes by itself.
    static float mixWanted(const EffectBase& fx)
    {
        if (! fx.givesWetOnly()) return 1.0f;
        const float m = fx.wetWanted();
        return m >= 0.0f && m <= 1.0f ? m : (m > 1.0f ? 1.0f : 0.0f);      // (what is not a number: dry)
    }

    float mixStep() const { return currentSampleRate > 0.0 ? (float) (1.0 / ((double) MIX_SWING_SECONDS * currentSampleRate)) : 1.0f; }

    // The slot's mix stands at 1 and stays there: the effect can be run in place, with no dry copy.
    bool mixStandsOpen(size_t slot) const
    {
        return mixWanted(*effects[slot]) >= 1.0f && (mixes[slot] < 0.0f || mixes[slot] >= 1.0f);
    }

    // One sample of a slot's mix. noDry: the entrance of a send's chain (mix * wet, nothing of
    // what the slot was given).
    static float mixed(bool noDry, float dry, float wet, float now)
    {
        return noDry ? now * wet : dry + now * (wet - dry);
    }

    // The wet sound of a slot made into dry + mix (wet - dry), in the wet buffers; the mix one
    // step further on its line with each sample. Nothing is done while it stands at 1.
    // noDry (the entrance of a send's chain): mix * wet, and no dry sound is kept for a late or
    // delayed mix, since none is mixed in.
    void mixSlot(size_t slot, const float* dryL, const float* dryR, float* wetL, float* wetR, int numSamples, bool noDry = false)
    {
        const float wanted = mixWanted(*effects[slot]);
        float now = mixes[slot] < 0.0f ? wanted : mixes[slot];
        if (noDry && lines[slot].used())
            lines[slot].rest();
        else if (lines[slot].used())
        {
            const float dryTarget = effects[slot]->dryDelaySamples();
            if (lines[slot].fractional)
            {
                // Through-zero path: fractional, glided dry delay.
                if (dryTarget <= 0.0f && lines[slot].current < 0.5f && now == wanted && now >= 1.0f)
                {
                    // No through-zero and mix is fully open: nothing to do.
                    lines[slot].rest();
                }
                else
                    now = mixDryDelay(slot, now, wanted, dryTarget, dryL, dryR, wetL, wetR, numSamples);
            }
            else
            {
                // Integer latency path (LMT).
                if (now == wanted && now >= 1.0f)
                    lines[slot].held = 0;
                else
                    now = mixLate(slot, now, wanted, dryL, dryR, wetL, wetR, numSamples);
            }
            mixes[slot] = now;
            return;
        }
        if (now == wanted)
        {
            mixes[slot] = now;
            if (now >= 1.0f) return;
            for (int i = 0; i < numSamples; ++i)
            {
                wetL[i] = mixed(noDry, dryL[i], wetL[i], now);
                wetR[i] = mixed(noDry, dryR[i], wetR[i], now);
            }
            return;
        }
        const float step = mixStep();
        for (int i = 0; i < numSamples; ++i)
        {
            now = dsp::towards(now, wanted, step);
            wetL[i] = mixed(noDry, dryL[i], wetL[i], now);
            wetR[i] = mixed(noDry, dryR[i], wetR[i], now);
        }
        mixes[slot] = now;
    }

    // One effect over a stretch of the block that begins `offset` samples in: it is handed what
    // it listens to for exactly that stretch, then run.
    void run(EffectBase& fx, float* left, float* right, int numSamples, int offset = 0)
    {
        if (feed != nullptr)
            feed->hand(fx, offset, numSamples);
        fx.processStereo(left, right, numSamples);
    }

    // A slot with all of its effect: in place, the mix at rest at 1.
    void runOpen(size_t slot, float* left, float* right, int numSamples, int offset = 0)
    {
        mixes[slot] = 1.0f;
        if (! lines[slot].fractional)
            lines[slot].held = 0;       // (integer latency: not written while the mix stands open)
        run(*effects[slot], left, right, numSamples, offset);
    }

    // A slot beside its dry sound: the effect runs on a copy (wetL / wetR), which is then the
    // slot's mix of what it was given (left / right, left as they are) and the effect's sound.
    // noDry: the entrance of a send's chain (mixSlot).
    void runMixed(size_t slot, const float* left, const float* right, float* wetL, float* wetR, int numSamples, int offset = 0, bool noDry = false)
    {
        runBeside(slot, left, right, left, right, wetL, wetR, numSamples, offset, noDry);
    }

    // The same with what the effect is given (inL / inR) and what its sound is mixed against
    // (dryL / dryR) apart: they differ only while the entrance of a send's chain fades (seriesStretch).
    void runBeside(size_t slot, const float* inL, const float* inR, const float* dryL, const float* dryR,
                   float* wetL, float* wetR, int numSamples, int offset, bool noDry)
    {
        std::memcpy(wetL, inL, sizeof(float) * (size_t) numSamples);
        std::memcpy(wetR, inR, sizeof(float) * (size_t) numSamples);
        run(*effects[slot], wetL, wetR, numSamples, offset);
        mixSlot(slot, dryL, dryR, wetL, wetR, numSamples, noDry);
    }

    // The same for an effect that is late: the dry sound goes through the slot's line and is mixed
    // as late as the effect's sound is. Returns the mix it arrived at.
    float mixLate(size_t slot, float now, float wanted, const float* dryL, const float* dryR, float* wetL, float* wetR, int numSamples)
    {
        auto& line = lines[slot];
        const int late = std::clamp(effects[slot]->latencySamples(), 0, DryLine::SIZE - 1);
        const float step = mixStep();
        for (int i = 0; i < numSamples; ++i)
        {
            line.left[(size_t) line.write] = dryL[i];       // (write is kept within the line)
            line.right[(size_t) line.write] = dryR[i];
            const int read = (line.write - late) & DryLine::MASK;
            line.write = (line.write + 1) & DryLine::MASK;
            if (line.held <= late)
                ++line.held;                // (the line does not reach that far back yet: the mix waits)
            else
                now = dsp::towards(now, wanted, step);
            const float l = line.left[(size_t) read], r = line.right[(size_t) read];
            wetL[i] = l + now * (wetL[i] - l);
            wetR[i] = r + now * (wetR[i] - r);
        }
        return now;
    }

    // Through-zero path: the dry sound goes through the slot's line and is read at a fractional
    // delay that glides to the target (nothing jumped to). Returns the mix it arrived at.
    float mixDryDelay(size_t slot, float now, float wanted, float target,
                      const float* dryL, const float* dryR, float* wetL, float* wetR, int numSamples)
    {
        auto& line = lines[slot];
        target = std::clamp(target, 0.0f, (float) (DryLine::SIZE - 4));
        const float step = mixStep();
        const float k = line.follow;
        const bool snap = line.held == 0;       // first time: snap to target, then wait for the line to fill
        float d = snap ? target : line.current;

        for (int i = 0; i < numSamples; ++i)
        {
            // Write dry into the line.
            line.put(dryL[i], dryR[i]);

            // Glide to the target delay (per sample, like ModDelay's dry tap).
            if (! snap)
            {
                const float delta = (target - d) * k;
                d += std::clamp(delta, -DryLine::MAX_SLEW, DryLine::MAX_SLEW);
            }

            // Not enough data yet: keep the mix where it is (only when the delay is meaningful).
            if (d >= 0.5f && line.held < (int)(d) + 3)
                continue;
            now = dsp::towards(now, wanted, step);

            if (d < 0.5f)
            {
                // No meaningful delay: use the dry input directly.
                wetL[i] = dryL[i] + now * (wetL[i] - dryL[i]);
                wetR[i] = dryR[i] + now * (wetR[i] - dryR[i]);
            }
            else
            {
                // Read from the line at the fractional delay (linear interpolation).
                const int whole = (int) d;
                const float frac = d - (float) whole;
                const int p0 = (line.write - 1 - whole) & DryLine::MASK;
                const int p1 = (line.write - 2 - whole) & DryLine::MASK;
                const float dl = line.left[(size_t) p0] * (1.0f - frac) + line.left[(size_t) p1] * frac;
                const float dr = line.right[(size_t) p0] * (1.0f - frac) + line.right[(size_t) p1] * frac;
                wetL[i] = dl + now * (wetL[i] - dl);
                wetR[i] = dr + now * (wetR[i] - dr);
            }
        }
        line.current = d;
        return now;
    }

    // Write dry input into a slot's line without reading (for effects whose mix stands at 1 but
    // whose through-zero line must be kept filled, so it has data when the mix moves).
    void fillDryLine(size_t slot, const float* left, const float* right, int numSamples)
    {
        auto& line = lines[slot];
        for (int i = 0; i < numSamples; ++i)
            line.put(left[i], right[i]);
    }

    void processSeriesRouting(float* left, float* right, int numSamples)
    {
        if ((int) scratchL.size() >= numSamples)
        {
            seriesStretch(left, right, numSamples, 0, scratchL.data(), scratchR.data(), accumL.data(), accumR.data());
            return;
        }
        // A chain that was not prepared for a block this long (a test, mostly): in pieces, with
        // the dry copy on the stack.
        float pieceL[PIECE], pieceR[PIECE], backL[PIECE], backR[PIECE];
        for (int done = 0; done < numSamples; done += PIECE)
            seriesStretch(left + done, right + done, std::min(PIECE, numSamples - done), done, pieceL, pieceR, backL, backR);
    }

    // What left / right hold on their way through a send's chain (an insert chain is Return
    // from its first slot):
    //   Raw     what the bus was sent, and no slot before was on: it feeds the next slot and is
    //           never returned;
    //   Return  what the slots before made: from here on the chain is an insert chain;
    //   Both    the entrance is fading in or out: left / right are what feeds the next slot (the
    //           return so far and the share of the raw sound the fade still lets through, so
    //           that nothing behind it is fed a cut), the return alone is kept beside (backL / backR).
    enum class Feed { Raw, Return, Both };

    float fadeStep() const { return currentSampleRate > 0.0 ? (float) (1.0 / (0.003 * currentSampleRate)) : 1.0f; }

    // dryRoom, back: room for numSamples each (the effect's copy; the return beside the feed).
    void seriesStretch(float* left, float* right, int numSamples, int offset, float* dryRoomL, float* dryRoomR, float* backL, float* backR)
    {
        const size_t bytes = sizeof(float) * (size_t) numSamples;
        float* wetL = dryRoomL;
        float* wetR = dryRoomR;
        Feed holds = sendMode ? Feed::Raw : Feed::Return;
        for (size_t slot = 0; slot < effects.size(); ++slot)
        {
            auto& fx = effects[slot];
            // Switched off or on under sound: the effect is faded out or in over 3 ms (a cut
            // from the effect's output to its input, or back, is a click).
            const float target = fx->isBypassed() ? 0.0f : 1.0f;
            float presence = fx->presence < 0.0f ? target : fx->presence;
            const bool atRest = presence == target;
            if (atRest && target == 0.0f)
            {
                // Off, and at rest: a wire (in a send's chain too: what is behind it is fed what
                // this slot was fed).
                fx->presence = target;
                fx->hold(false);
                continue;
            }

            if (holds == Feed::Return)
            {
                if (atRest && mixStandsOpen(slot))
                {
                    // At rest, and all of the effect: in place, as itself.
                    fx->presence = target;
                    fx->hold(true);
                    // A through-zero effect at mix 1 still needs its dry line filled, so the line
                    // has data when the mix later moves away from 1.
                    if (lines[slot].used() && lines[slot].fractional && fx->dryDelaySamples() > 0.0f)
                        fillDryLine(slot, left, right, numSamples);
                    runOpen(slot, left, right, numSamples, offset);
                    continue;
                }
                fx->hold(true);         // it runs on while it fades
                if (fx->givesWetOnly())
                    CellEffect::saneBlock(left, right, numSamples);     // (the dry sound that is mixed back is what the effect was given)
                runMixed(slot, left, right, wetL, wetR, numSamples, offset);
                if (atRest)
                {
                    fx->presence = target;      // (it has sounded: switched off now, it is faded, not cut)
                    std::memcpy(left, wetL, bytes);
                    std::memcpy(right, wetR, bytes);
                    continue;
                }
                const float step = fadeStep();
                for (int i = 0; i < numSamples; ++i)
                {
                    presence = dsp::towards(presence, target, step);
                    left[i] += presence * (wetL[i] - left[i]);
                    right[i] += presence * (wetR[i] - right[i]);
                }
                fx->presence = presence;
                continue;
            }

            if (holds == Feed::Raw)
            {
                // The entrance of a send's chain: mix * wet and nothing of what it was given.
                if (atRest && mixStandsOpen(slot))
                {
                    fx->presence = target;
                    fx->hold(true);
                    if (lines[slot].used())
                        lines[slot].rest();     // (no dry sound is mixed here: none is kept)
                    runOpen(slot, left, right, numSamples, offset);
                    holds = Feed::Return;
                    continue;
                }
                fx->hold(true);
                if (fx->givesWetOnly())
                    CellEffect::saneBlock(left, right, numSamples);
                runMixed(slot, left, right, wetL, wetR, numSamples, offset, true);
                if (atRest)
                {
                    fx->presence = target;
                    std::memcpy(left, wetL, bytes);
                    std::memcpy(right, wetR, bytes);
                    holds = Feed::Return;
                    continue;
                }
                // Switched on or off under sound: its return fades from or to silence.
                const float step = fadeStep();
                if (slot + 1 == effects.size())
                {
                    for (int i = 0; i < numSamples; ++i)
                    {
                        presence = dsp::towards(presence, target, step);
                        left[i] = presence * wetL[i];
                        right[i] = presence * wetR[i];
                    }
                    holds = Feed::Return;
                }
                else
                {
                    // Slots behind it: what feeds them goes over from the raw sound to this
                    // slot's return (or back) along the same fade.
                    for (int i = 0; i < numSamples; ++i)
                    {
                        presence = dsp::towards(presence, target, step);
                        backL[i] = presence * wetL[i];
                        backR[i] = presence * wetR[i];
                        left[i] = backL[i] + (1.0f - presence) * left[i];
                        right[i] = backR[i] + (1.0f - presence) * right[i];
                    }
                    holds = Feed::Both;
                }
                fx->presence = presence;
                continue;
            }

            // Feed::Both: the slot is fed left / right and mixes against the return alone.
            fx->hold(true);
            if (fx->givesWetOnly())
                CellEffect::saneBlock(left, right, numSamples);
            if (atRest && mixStandsOpen(slot))
            {
                if (lines[slot].used() && lines[slot].fractional && fx->dryDelaySamples() > 0.0f)
                    fillDryLine(slot, backL, backR, numSamples);
                std::memcpy(wetL, left, bytes);
                std::memcpy(wetR, right, bytes);
                runOpen(slot, wetL, wetR, numSamples, offset);
            }
            else
                runBeside(slot, left, right, backL, backR, wetL, wetR, numSamples, offset, false);
            if (atRest)
            {
                // All of this slot: what it made is the return, and all that feeds the next.
                fx->presence = target;
                std::memcpy(left, wetL, bytes);
                std::memcpy(right, wetR, bytes);
                holds = Feed::Return;
                continue;
            }
            const float step = fadeStep();
            for (int i = 0; i < numSamples; ++i)
            {
                presence = dsp::towards(presence, target, step);
                const float l = backL[i] + presence * (wetL[i] - backL[i]);
                const float r = backR[i] + presence * (wetR[i] - backR[i]);
                left[i] = l + (1.0f - presence) * (left[i] - backL[i]);
                right[i] = r + (1.0f - presence) * (right[i] - backR[i]);
                backL[i] = l;
                backR[i] = r;
            }
            fx->presence = presence;
        }

        if (holds == Feed::Raw)
        {
            // A send with no slot on returns nothing.
            std::memset(left, 0, bytes);
            std::memset(right, 0, bytes);
        }
        else if (holds == Feed::Both)
        {
            std::memcpy(left, backL, bytes);
            std::memcpy(right, backR, bytes);
        }
    }

    void processParallelRouting(float* left, float* right, int numSamples)
    {
        // Clear accumulator
        std::memset(accumL.data(), 0, sizeof(float) * (size_t) numSamples);
        std::memset(accumR.data(), 0, sizeof(float) * (size_t) numSamples);

        int activeCount = 0;

        for (size_t slot = 0; slot < effects.size(); ++slot)
        {
            auto& fx = effects[slot];
            fx->hold(! fx->isBypassed());
            if (fx->isBypassed())
                continue;

            if (fx->givesWetOnly() && ! mixStandsOpen(slot))
                CellEffect::saneBlock(left, right, numSamples);

            // (Side by side on a send, each slot is fed what the bus was sent: none returns it.)
            runMixed(slot, left, right, scratchL.data(), scratchR.data(), numSamples, 0, sendMode);

            // Sum into accumulator
            for (int i = 0; i < numSamples; ++i)
            {
                accumL[(size_t) i] += scratchL[(size_t) i];
                accumR[(size_t) i] += scratchR[(size_t) i];
            }

            ++activeCount;
        }

        if (activeCount > 0)
        {
            // Copy accumulated result back
            std::memcpy(left, accumL.data(), sizeof(float) * (size_t) numSamples);
            std::memcpy(right, accumR.data(), sizeof(float) * (size_t) numSamples);
        }
        else if (sendMode)
        {
            std::memset(left, 0, sizeof(float) * (size_t) numSamples);
            std::memset(right, 0, sizeof(float) * (size_t) numSamples);
        }
        // If all bypassed, input passes through unchanged (a send returns nothing)
    }

    void processMixedRouting(float* left, float* right, int numSamples)
    {
        if (effects.empty())
            return;

        // First effect processes in series (in place). On a send it is the entrance when it is
        // on; when it is off, each of the others is (they are then fed what the bus was sent).
        const bool entered = ! effects[0]->isBypassed();
        effects[0]->hold(entered);
        if (entered)
        {
            if (mixStandsOpen(0))
                runOpen(0, left, right, numSamples);
            else
            {
                CellEffect::saneBlock(left, right, numSamples);
                runMixed(0, left, right, scratchL.data(), scratchR.data(), numSamples, 0, sendMode);
                std::memcpy(left, scratchL.data(), sizeof(float) * (size_t) numSamples);
                std::memcpy(right, scratchR.data(), sizeof(float) * (size_t) numSamples);
            }
        }

        if (effects.size() == 1)
        {
            if (sendMode && ! entered)
            {
                std::memset(left, 0, sizeof(float) * (size_t) numSamples);
                std::memset(right, 0, sizeof(float) * (size_t) numSamples);
            }
            return;
        }

        // Remaining effects process in parallel from the series output
        std::memset(accumL.data(), 0, sizeof(float) * (size_t) numSamples);
        std::memset(accumR.data(), 0, sizeof(float) * (size_t) numSamples);

        int activeCount = 0;

        for (size_t idx = 1; idx < effects.size(); ++idx)
        {
            effects[idx]->hold(! effects[idx]->isBypassed());
            if (effects[idx]->isBypassed())
                continue;

            if (effects[idx]->givesWetOnly() && ! mixStandsOpen(idx))
                CellEffect::saneBlock(left, right, numSamples);
            runMixed(idx, left, right, scratchL.data(), scratchR.data(), numSamples, 0, sendMode && ! entered);

            for (int i = 0; i < numSamples; ++i)
            {
                accumL[(size_t) i] += scratchL[(size_t) i];
                accumR[(size_t) i] += scratchR[(size_t) i];
            }

            ++activeCount;
        }

        if (activeCount > 0)
        {
            std::memcpy(left, accumL.data(), sizeof(float) * (size_t) numSamples);
            std::memcpy(right, accumR.data(), sizeof(float) * (size_t) numSamples);
        }
        else if (sendMode && ! entered)
        {
            std::memset(left, 0, sizeof(float) * (size_t) numSamples);
            std::memset(right, 0, sizeof(float) * (size_t) numSamples);
        }
        // If all remaining are bypassed, series output passes through
    }

    std::vector<std::unique_ptr<EffectBase>> effects;
    std::vector<float> mixes;       // each slot's mix as it is now (audio thread); FRESH = nothing sounded yet
    std::vector<DryLine> lines;     // each slot's late dry sound (empty unless its effect is late)
    std::vector<int> kinds;         // what each slot holds, by its maker's number (addEffect)
    Routing routing = Routing::Series;
    bool sendMode = false;          // the chain gives only the wet sound (a send bus's return)
    const KeyFeed* feed = nullptr;  // what the effects may listen to, during processStereo only

    double currentSampleRate = 0.0;
    int currentBlockSize = 0;

    // Pre-allocated scratch buffers (sized in prepare())
    std::vector<float> scratchL, scratchR;
    std::vector<float> accumL, accumR;
};
