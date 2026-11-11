#pragma once

#include "EffectChain.h"
#include <cstring>
#include <vector>

/**
 * SendBus — a send bus with its own effect chain and mix-back level.
 *
 * Usage per audio block:
 *   1. clearBuffer()           — zero the input accumulator
 *   2. addInput() per track    — mix each track's signal into the bus at its send level
 *   3. process()               — run the accumulated signal through the effect chain
 *   4. mixInto()               — add the processed result to the main output
 *
 * All buffers are pre-allocated in prepare(). No heap allocation on the audio thread.
 */
class SendBus
{
public:
    SendBus() = default;

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

    /** Process the accumulated input through the effect chain. */
    void process()
    {
        chain.processStereo(bufferL.data(), bufferR.data(), currentBlockSize);
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
