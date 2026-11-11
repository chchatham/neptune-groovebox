#pragma once

#include "MIDIClock.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>

// ---------------------------------------------------------------------------
// AbletonLink — wrapper interface for Ableton Link integration.
//
// The actual Link library (https://github.com/Ableton/link) is a submodule
// that must be added to the build. This header defines the integration layer
// that wraps the Link API and bridges it to our MIDIClock / SequencerCore.
//
// Tempo priority: Link > External MIDI > Internal
// ---------------------------------------------------------------------------

class AbletonLink
{
public:
    // Status snapshot taken from the Link session — safe to read from any thread
    struct SessionState
    {
        double tempo = 120.0;
        double beat = 0.0;
        double phase = 0.0;       // beat phase within quantum
        int    numPeers = 0;
        bool   isPlaying = false;
        bool   isConnected = false;
    };

    // Callback fired when Link tempo changes (called from Link's thread)
    using TempoCallback = std::function<void(double newTempo)>;
    using PeersCallback = std::function<void(int numPeers)>;
    using StartStopCallback = std::function<void(bool isPlaying)>;

    AbletonLink() = default;
    ~AbletonLink() { disable(); }

    // --- Enable / Disable ---------------------------------------------------

    void enable()
    {
        if (enabled.load())
            return;

        enabled.store(true);

        // In the real implementation this would be:
        //   link = std::make_unique<ableton::Link>(internalTempo.load());
        //   link->enable(true);
        //   link->enableStartStopSync(true);
        //   link->setTempoCallback([this](double bpm) { onLinkTempoChanged(bpm); });
        //   link->setNumPeersCallback([this](std::size_t n) { onPeersChanged(n); });
        //   link->setStartStopCallback([this](bool p) { onStartStopChanged(p); });
    }

    void disable()
    {
        if (!enabled.load())
            return;

        enabled.store(false);
        numPeers.store(0);
        // link->enable(false);
        // link.reset();
    }

    bool isEnabled() const { return enabled.load(); }

    // --- Configuration ------------------------------------------------------

    void setQuantum(double q) { quantum.store(q); }
    double getQuantum() const { return quantum.load(); }

    // --- Tempo exchange (thread-safe) ---------------------------------------

    void setTempo(double bpm)
    {
        bpm = juce::jlimit(20.0, 300.0, bpm);
        internalTempo.store(bpm);

        // In the real implementation:
        //   auto sessionState = link->captureAudioSessionState();
        //   sessionState.setTempo(bpm, link->clock().micros());
        //   link->commitAudioSessionState(sessionState);
    }

    double getTempo() const
    {
        return linkTempo.load();
    }

    // --- Session state queries (thread-safe) --------------------------------

    int getNumPeers() const { return numPeers.load(); }
    bool isConnected() const { return numPeers.load() > 0; }

    SessionState getSessionState() const
    {
        SessionState s;
        s.tempo = linkTempo.load();
        s.beat = currentBeat.load();
        s.phase = currentPhase.load();
        s.numPeers = numPeers.load();
        s.isPlaying = isPlaying.load();
        s.isConnected = isConnected();
        return s;
    }

    // --- Transport (start/stop sync) ----------------------------------------

    void requestStart()
    {
        if (!enabled.load())
            return;

        isPlaying.store(true);

        // In the real implementation:
        //   auto sessionState = link->captureAudioSessionState();
        //   sessionState.setIsPlaying(true, link->clock().micros());
        //   link->commitAudioSessionState(sessionState);
    }

    void requestStop()
    {
        if (!enabled.load())
            return;

        isPlaying.store(false);

        // In the real implementation:
        //   auto sessionState = link->captureAudioSessionState();
        //   sessionState.setIsPlaying(false, link->clock().micros());
        //   link->commitAudioSessionState(sessionState);
    }

    // --- Audio callback integration -----------------------------------------
    // Call from the audio thread at the start of each buffer.
    // Captures the Link session state and updates atomics for UI reading.

    void captureForAudioThread(int numSamples, double sampleRate)
    {
        if (!enabled.load())
            return;

        juce::ignoreUnused(numSamples, sampleRate);

        // In the real implementation:
        //   auto sessionState = link->captureAudioSessionState();
        //   auto hostTime = link->clock().micros() + std::chrono::microseconds(
        //       llround(numSamples / sampleRate * 1.0e6));
        //
        //   linkTempo.store(sessionState.tempo());
        //   currentBeat.store(sessionState.beatAtTime(hostTime, quantum.load()));
        //   currentPhase.store(sessionState.phaseAtTime(hostTime, quantum.load()));
        //
        //   bool nowPlaying = sessionState.isPlaying();
        //   bool wasPlaying = isPlaying.load();
        //   isPlaying.store(nowPlaying);
        //
        //   if (nowPlaying && !wasPlaying && startStopCallback)
        //       startStopCallback(true);
        //   if (!nowPlaying && wasPlaying && startStopCallback)
        //       startStopCallback(false);
    }

    // --- Beat phase query for quantized launch ------------------------------
    // Returns the beat position within the current quantum (0.0 to quantum).
    // Use this for pattern-launch quantization: launch on the next quantum boundary.

    double getBeatPhase() const { return currentPhase.load(); }
    double getCurrentBeat() const { return currentBeat.load(); }

    bool isAtQuantumBoundary(double tolerance = 0.05) const
    {
        double phase = currentPhase.load();
        double q = quantum.load();
        return phase < tolerance || (q - phase) < tolerance;
    }

    // --- Callback registration (main thread) --------------------------------

    void setTempoCallback(TempoCallback cb) { tempoCallback = std::move(cb); }
    void setPeersCallback(PeersCallback cb) { peersCallback = std::move(cb); }
    void setStartStopCallback(StartStopCallback cb) { startStopCallback = std::move(cb); }

    // --- Integration with MIDIClock -----------------------------------------
    // When Link is enabled and has peers, it takes priority over internal/external.
    // Call this from the MIDIClock's processBlock to apply Link tempo.

    bool shouldOverrideTempo() const
    {
        return enabled.load() && numPeers.load() > 0;
    }

    void applyToMidiClock(MIDIClock& midiClock) const
    {
        if (shouldOverrideTempo())
        {
            midiClock.setTempo(linkTempo.load());
        }
    }

private:
    // --- Link callbacks (called from Link's internal thread) ----------------

    void onLinkTempoChanged(double newTempo)
    {
        linkTempo.store(newTempo);
        if (tempoCallback)
            tempoCallback(newTempo);
    }

    void onPeersChanged(int n)
    {
        numPeers.store(n);
        if (peersCallback)
            peersCallback(n);
    }

    void onStartStopChanged(bool playing)
    {
        isPlaying.store(playing);
        if (startStopCallback)
            startStopCallback(playing);
    }

    // --- State (all atomic for thread-safe access) --------------------------

    std::atomic<bool>   enabled { false };
    std::atomic<double> internalTempo { 120.0 };
    std::atomic<double> linkTempo { 120.0 };
    std::atomic<double> currentBeat { 0.0 };
    std::atomic<double> currentPhase { 0.0 };
    std::atomic<double> quantum { 4.0 };   // beats per quantum (4 = 1 bar in 4/4)
    std::atomic<int>    numPeers { 0 };
    std::atomic<bool>   isPlaying { false };

    // Callbacks (set from main thread, called from Link thread)
    TempoCallback       tempoCallback;
    PeersCallback       peersCallback;
    StartStopCallback   startStopCallback;

    // In the real implementation:
    // std::unique_ptr<ableton::Link> link;
};
