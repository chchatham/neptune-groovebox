#pragma once

#include "EffectChain.h"
#include <cstring>
#include <vector>

/**
 * SendBus — a send bus with its own effect chain and mix-back level. The chain is an insert chain
 * like a track's or the master's: the AudioEngine reaches it as AudioEngine::sendTrack(bus).
 *
 * A send is a return: what comes back is only the chain's wet sound, not a copy of what was sent.
 * An empty chain, a chain with every effect OFF, or a first effect at WET 0 gives silence. The
 * chain is set to send mode (EffectChain::setSendMode): the first effect that is ON is the
 * chain's entrance and gives mix*wet (not dry + mix*(wet-dry)), so the return rises smoothly from
 * silence as its WET leaves zero, and switching it ON / OFF is a fade from / to silence. The
 * effects behind it are an insert chain on that return: each mixes against the stage before it,
 * and one that is OFF passes the return on.
 *
 * Usage per audio block:
 *   1. clearBuffer()           — zero the input accumulator
 *   2. addInput() per track    — mix each track's signal into the bus at its send level
 *   3. process(keys, n)        — run the accumulated signal through the effect chain
 *   4. mixInto()               — add the processed result to the main output
 *
 * All buffers are pre-allocated in prepare(). No heap allocation on the audio thread.
 */
class SendBus
{
public:
    SendBus() { chain.setSendMode(true); }

    void prepare(double sampleRate, int blockSize)
    {
        currentBlockSize = blockSize;
        bufferL.resize((size_t) blockSize, 0.0f);
        bufferR.resize((size_t) blockSize, 0.0f);
        chain.prepare(sampleRate, blockSize);
    }

    /** Zero the input accumulator. Call at the start of each audio block. */
    void clearBuffer()
    {
        std::memset(bufferL.data(), 0, sizeof(float) * (size_t) currentBlockSize);
        std::memset(bufferR.data(), 0, sizeof(float) * (size_t) currentBlockSize);
    }

    /** Add a track's signal into the bus, scaled by the per-track send level. */
    void addInput(const float* left, const float* right, int numSamples, float level)
    {
        if (level <= 0.0f)
            return;

        int n = (numSamples < currentBlockSize) ? numSamples : currentBlockSize;
        for (int i = 0; i < n; ++i)
        {
            bufferL[(size_t) i] += left[i] * level;
            bufferR[(size_t) i] += right[i] * level;
        }
    }

    /** The same with the level moving in a straight line from one value to another across the
        block (a send level that jumps is a click in the effect). */
    void addInputRamped(const float* left, const float* right, int numSamples, float from, float to)
    {
        int n = (numSamples < currentBlockSize) ? numSamples : currentBlockSize;
        if (n <= 0)
            return;
        const float step = (to - from) / (float) n;
        float level = from;
        for (int i = 0; i < n; ++i)
        {
            level += step;
            bufferL[(size_t) i] += left[i] * level;
            bufferR[(size_t) i] += right[i] * level;
        }
    }

    /** Process the accumulated input through the effect chain (in send mode: the chain gives only
        the wet contribution, nothing of the dry input that was sent).
        keys: what a keyed effect on the bus may listen to (the tracks' sounds; a bus has no input
        of its own), as on any chain. */
    void process(const KeyFeed* keys = nullptr, int numSamples = -1)
    {
        // numSamples: the samples played in this block (a device may hand over fewer than the bus
        // was prepared for: the effects must then run that many, not the prepared length, or
        // their time runs fast); below 0: the prepared length.
        const int n = (numSamples >= 0 && numSamples < currentBlockSize) ? numSamples : currentBlockSize;
        chain.processStereo(bufferL.data(), bufferR.data(), n, keys);
    }

    /** Mix the processed result into the output buffer, scaled by sendLevel. */
    void mixInto(float* left, float* right, int numSamples)
    {
        if (sendLevel <= 0.0f)
            return;

        int n = (numSamples < currentBlockSize) ? numSamples : currentBlockSize;
        for (int i = 0; i < n; ++i)
        {
            left[i] += bufferL[(size_t) i] * sendLevel;
            right[i] += bufferR[(size_t) i] * sendLevel;
        }
    }

    void setSendLevel(float level) { sendLevel = level; }
    float getSendLevel() const { return sendLevel; }

    // What the bus holds now (after process(): what its chain made), for whoever looks it over
    // before it is mixed in.
    float* left() { return bufferL.data(); }
    float* right() { return bufferR.data(); }
    int frames() const { return currentBlockSize; }

    EffectChain& getChain() { return chain; }
    const EffectChain& getChain() const { return chain; }

private:
    EffectChain chain;
    float sendLevel = 0.0f;
    int currentBlockSize = 0;

    // Pre-allocated in prepare()
    std::vector<float> bufferL;
    std::vector<float> bufferR;
};
