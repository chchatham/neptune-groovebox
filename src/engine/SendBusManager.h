#pragma once

#include "../dsp/effects/SendBus.h"
#include "../dsp/effects/EffectBase.h"
#include "../dsp/effects/EffectChain.h"
#include <vector>
#include <memory>
#include <string>
#include <array>
#include <cstring>
#include <algorithm>
#include <cassert>

/**
 * SendBusManager — manages an open-ended collection of send buses with
 * configurable effect chains, per-track send levels, and pre/post fader routing.
 *
 * Audio-thread safe: all buffers are pre-allocated in prepare(). Bus
 * add/remove/reorder must happen on the message thread only.
 *
 * Default configuration: 2 buses (delay + reverb) matching AudioEngine defaults.
 *
 * Each track can route to any bus with independent pre-fader or post-fader
 * sends. Pre-fader sends tap the signal before the track volume fader;
 * post-fader sends tap after volume is applied.
 */

/// Per-track send configuration for one bus
struct TrackSendConfig
{
    float level = 0.0f;        ///< Send level 0..1
    bool preFader = false;     ///< true = pre-fader send, false = post-fader (default)
};

/// Extended send bus with name, active flag, and pre-allocated fixed buffers
struct ManagedSendBus
{
    std::string name;
    float level = 1.0f;                        ///< Bus master level (return level)
    bool active = true;                         ///< Bypass entire bus when false
    EffectChain chain;                          ///< Configurable effect chain (series/parallel/mixed)

    static constexpr int MAX_BUFFER_SIZE = 4096;

    // Pre-allocated stereo buffers (no heap allocation on audio thread)
    std::array<float, MAX_BUFFER_SIZE> bufferL{};
    std::array<float, MAX_BUFFER_SIZE> bufferR{};

    int currentBlockSize = 0;

    void prepare(double sampleRate, int blockSize)
    {
        currentBlockSize = std::min(blockSize, MAX_BUFFER_SIZE);
        chain.prepare(sampleRate, blockSize);
    }

    void clearBuffer()
    {
        std::memset(bufferL.data(), 0, sizeof(float) * (size_t) currentBlockSize);
        std::memset(bufferR.data(), 0, sizeof(float) * (size_t) currentBlockSize);
    }

    /// Accumulate a track's signal into this bus at the given level
    void addInput(const float* left, const float* right, int numSamples, float sendLevel)
    {
        if (sendLevel <= 0.0f || !active) return;
        int n = std::min(numSamples, currentBlockSize);
        for (int i = 0; i < n; ++i)
        {
            bufferL[(size_t) i] += left[i] * sendLevel;
            bufferR[(size_t) i] += right[i] * sendLevel;
        }
    }

    /// Run accumulated signal through the effect chain
    void process()
    {
        if (!active) return;
        chain.processStereo(bufferL.data(), bufferR.data(), currentBlockSize);
    }

    /// Mix the processed bus output into the main buffer, scaled by bus return level
    void mixInto(float* left, float* right, int numSamples)
    {
        if (!active || level <= 0.0f) return;
        int n = std::min(numSamples, currentBlockSize);
        for (int i = 0; i < n; ++i)
        {
            left[i] += bufferL[(size_t) i] * level;
            right[i] += bufferR[(size_t) i] * level;
        }
    }

    /// Read-only access to the bus buffer (for sidechain sources)
    const float* getBufferL() const { return bufferL.data(); }
    const float* getBufferR() const { return bufferR.data(); }
};

class SendBusManager
{
public:
    static constexpr int DEFAULT_NUM_BUSES = 2;
    static constexpr int MAX_BUSES = 16;
    static constexpr int MAX_TRACKS = 64;

    SendBusManager()
    {
        // Default: bus 0 = delay, bus 1 = reverb (names only; effects added externally)
        addBus("Delay");
        addBus("Reverb");
    }

    // ----------------------------------------------------------------
    // Bus management (message thread only)
    // ----------------------------------------------------------------

    /// Add a new bus with a name. Returns bus index, or -1 if at max.
    int addBus(const std::string& name = "Send")
    {
        if ((int) buses.size() >= MAX_BUSES) return -1;
        buses.emplace_back();
        buses.back().name = name;
        if (currentSampleRate > 0.0)
            buses.back().prepare(currentSampleRate, currentBlockSize);
        return (int) buses.size() - 1;
    }

    /// Remove a bus by index. Shifts subsequent indices down.
    bool removeBus(int index)
    {
        if (index < 0 || index >= (int) buses.size()) return false;
        buses.erase(buses.begin() + index);
        // Shift send configs: remove column for this bus in all tracks
        for (auto& trackSends : trackSendConfigs)
        {
            if (index < (int) trackSends.size())
                trackSends.erase(trackSends.begin() + index);
        }
        return true;
    }

    /// Reorder a bus from one position to another
    bool reorderBus(int fromIndex, int toIndex)
    {
        if (fromIndex < 0 || fromIndex >= (int) buses.size()) return false;
        if (toIndex < 0 || toIndex >= (int) buses.size()) return false;
        if (fromIndex == toIndex) return true;

        auto bus = std::move(buses[(size_t) fromIndex]);
        buses.erase(buses.begin() + fromIndex);
        buses.insert(buses.begin() + toIndex, std::move(bus));

        // Reorder send configs to match
        for (auto& trackSends : trackSendConfigs)
        {
            if (fromIndex < (int) trackSends.size() && toIndex < (int) trackSends.size())
            {
                auto cfg = trackSends[(size_t) fromIndex];
                trackSends.erase(trackSends.begin() + fromIndex);
                trackSends.insert(trackSends.begin() + toIndex, cfg);
            }
        }
        return true;
    }

    int getBusCount() const { return (int) buses.size(); }

    ManagedSendBus& getBus(int index)
    {
        assert(index >= 0 && index < (int) buses.size());
        return buses[(size_t) index];
    }

    const ManagedSendBus& getBus(int index) const
    {
        assert(index >= 0 && index < (int) buses.size());
        return buses[(size_t) index];
    }

    // ----------------------------------------------------------------
    // Per-track send configuration
    // ----------------------------------------------------------------

    /// Set the send level from a track to a bus
    void setTrackSendLevel(int trackIndex, int busIndex, float level)
    {
        ensureTrackConfig(trackIndex);
        ensureBusConfig(trackIndex, busIndex);
        trackSendConfigs[(size_t) trackIndex][(size_t) busIndex].level =
            std::clamp(level, 0.0f, 1.0f);
    }

    /// Get the send level from a track to a bus
    float getTrackSendLevel(int trackIndex, int busIndex) const
    {
        if (trackIndex < 0 || trackIndex >= (int) trackSendConfigs.size()) return 0.0f;
        auto& sends = trackSendConfigs[(size_t) trackIndex];
        if (busIndex < 0 || busIndex >= (int) sends.size()) return 0.0f;
        return sends[(size_t) busIndex].level;
    }

    /// Set whether a track's send to a bus is pre-fader
    void setTrackSendPreFader(int trackIndex, int busIndex, bool pre)
    {
        ensureTrackConfig(trackIndex);
        ensureBusConfig(trackIndex, busIndex);
        trackSendConfigs[(size_t) trackIndex][(size_t) busIndex].preFader = pre;
    }

    /// Check if a track's send to a bus is pre-fader
    bool isTrackSendPreFader(int trackIndex, int busIndex) const
    {
        if (trackIndex < 0 || trackIndex >= (int) trackSendConfigs.size()) return false;
        auto& sends = trackSendConfigs[(size_t) trackIndex];
        if (busIndex < 0 || busIndex >= (int) sends.size()) return false;
        return sends[(size_t) busIndex].preFader;
    }

    // ----------------------------------------------------------------
    // Audio thread: prepare / process
    // ----------------------------------------------------------------

    void prepare(double sampleRate, int blockSize)
    {
        currentSampleRate = sampleRate;
        currentBlockSize = blockSize;
        for (auto& bus : buses)
            bus.prepare(sampleRate, blockSize);
    }

    /// Clear all bus buffers. Call at the start of each audio block.
    void clearAllBuffers()
    {
        for (auto& bus : buses)
            bus.clearBuffer();
    }

    /**
     * Route a track's signal to all buses it is connected to.
     *
     * @param trackIndex    Which track is sending
     * @param preFaderL     Pre-fader signal (before track volume)
     * @param preFaderR     Pre-fader signal (before track volume)
     * @param postFaderL    Post-fader signal (after track volume)
     * @param postFaderR    Post-fader signal (after track volume)
     * @param numSamples    Number of samples in the buffers
     */
    void routeTrackToSends(int trackIndex,
                           const float* preFaderL, const float* preFaderR,
                           const float* postFaderL, const float* postFaderR,
                           int numSamples)
    {
        if (trackIndex < 0 || trackIndex >= (int) trackSendConfigs.size()) return;

        auto& sends = trackSendConfigs[(size_t) trackIndex];
        int busCount = std::min((int) sends.size(), (int) buses.size());

        for (int b = 0; b < busCount; ++b)
        {
            auto& cfg = sends[(size_t) b];
            if (cfg.level <= 0.0f) continue;

            if (cfg.preFader)
                buses[(size_t) b].addInput(preFaderL, preFaderR, numSamples, cfg.level);
            else
                buses[(size_t) b].addInput(postFaderL, postFaderR, numSamples, cfg.level);
        }
    }

    /**
     * Simplified routing when pre/post distinction isn't needed.
     * Always uses the provided signal (typically post-fader).
     */
    void routeTrackToSends(int trackIndex,
                           const float* left, const float* right,
                           int numSamples)
    {
        if (trackIndex < 0 || trackIndex >= (int) trackSendConfigs.size()) return;

        auto& sends = trackSendConfigs[(size_t) trackIndex];
        int busCount = std::min((int) sends.size(), (int) buses.size());

        for (int b = 0; b < busCount; ++b)
        {
            auto& cfg = sends[(size_t) b];
            if (cfg.level <= 0.0f) continue;
            buses[(size_t) b].addInput(left, right, numSamples, cfg.level);
        }
    }

    /// Process all bus effect chains
    void processAllBuses()
    {
        for (auto& bus : buses)
            bus.process();
    }

    /// Mix all bus outputs into the main output
    void mixAllBusesInto(float* mainL, float* mainR, int numSamples)
    {
        for (auto& bus : buses)
            bus.mixInto(mainL, mainR, numSamples);
    }

    // ----------------------------------------------------------------
    // Effect chain management per bus (message thread only)
    // ----------------------------------------------------------------

    /// Add an effect to a bus's chain
    void addEffectToBus(int busIndex, std::unique_ptr<EffectBase> effect)
    {
        if (busIndex < 0 || busIndex >= (int) buses.size()) return;
        if (currentSampleRate > 0.0)
            effect->prepare(currentSampleRate, currentBlockSize);
        buses[(size_t) busIndex].chain.addEffect(std::move(effect));
    }

    /// Remove an effect from a bus's chain by slot index
    void removeEffectFromBus(int busIndex, int effectSlot)
    {
        if (busIndex < 0 || busIndex >= (int) buses.size()) return;
        buses[(size_t) busIndex].chain.removeEffect(effectSlot);
    }

    /// Set routing mode for a bus's effect chain
    void setBusChainRouting(int busIndex, EffectChain::Routing routing)
    {
        if (busIndex < 0 || busIndex >= (int) buses.size()) return;
        buses[(size_t) busIndex].chain.setRouting(routing);
    }

    /// Get a bus's effect chain for direct access
    EffectChain* getBusChain(int busIndex)
    {
        if (busIndex < 0 || busIndex >= (int) buses.size()) return nullptr;
        return &buses[(size_t) busIndex].chain;
    }

    const EffectChain* getBusChain(int busIndex) const
    {
        if (busIndex < 0 || busIndex >= (int) buses.size()) return nullptr;
        return &buses[(size_t) busIndex].chain;
    }

    // ----------------------------------------------------------------
    // Bus properties
    // ----------------------------------------------------------------

    void setBusName(int index, const std::string& name)
    {
        if (index >= 0 && index < (int) buses.size())
            buses[(size_t) index].name = name;
    }

    std::string getBusName(int index) const
    {
        if (index >= 0 && index < (int) buses.size())
            return buses[(size_t) index].name;
        return "";
    }

    void setBusLevel(int index, float level)
    {
        if (index >= 0 && index < (int) buses.size())
            buses[(size_t) index].level = std::clamp(level, 0.0f, 1.0f);
    }

    float getBusLevel(int index) const
    {
        if (index >= 0 && index < (int) buses.size())
            return buses[(size_t) index].level;
        return 0.0f;
    }

    void setBusActive(int index, bool active)
    {
        if (index >= 0 && index < (int) buses.size())
            buses[(size_t) index].active = active;
    }

    bool isBusActive(int index) const
    {
        if (index >= 0 && index < (int) buses.size())
            return buses[(size_t) index].active;
        return false;
    }

    /// Access a bus's buffer directly (for sidechain sources, metering, etc.)
    const float* getBusBufferL(int index) const
    {
        if (index >= 0 && index < (int) buses.size())
            return buses[(size_t) index].getBufferL();
        return nullptr;
    }

    const float* getBusBufferR(int index) const
    {
        if (index >= 0 && index < (int) buses.size())
            return buses[(size_t) index].getBufferR();
        return nullptr;
    }

private:
    void ensureTrackConfig(int trackIndex)
    {
        if (trackIndex < 0) return;
        if ((size_t) trackIndex >= trackSendConfigs.size())
            trackSendConfigs.resize((size_t) trackIndex + 1);
    }

    void ensureBusConfig(int trackIndex, int busIndex)
    {
        if (trackIndex < 0 || busIndex < 0) return;
        auto& sends = trackSendConfigs[(size_t) trackIndex];
        if ((size_t) busIndex >= sends.size())
            sends.resize((size_t) busIndex + 1);
    }

    std::vector<ManagedSendBus> buses;

    // Per-track send configurations: trackSendConfigs[trackIdx][busIdx]
    std::vector<std::vector<TrackSendConfig>> trackSendConfigs;

    double currentSampleRate = 0.0;
    int currentBlockSize = 0;
};
