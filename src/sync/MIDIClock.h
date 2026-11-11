#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>

class MIDIClock
{
public:
    enum class Mode { Internal, External, Auto };

    struct ClockEvent
    {
        enum Type { Clock, Start, Stop, Continue, SongPosition };
        Type type = Clock;
        int position = 0;
    };

    MIDIClock() = default;

    void setMode(Mode m) { mode.store(m); }
    Mode getMode() const { return mode.load(); }

    void setTempo(double bpm)
    {
        bpm = juce::jlimit(20.0, 300.0, bpm);
        internalTempo.store(bpm);
    }

    double getTempo() const { return internalTempo.load(); }
    double getDetectedTempo() const { return detectedTempo.load(); }

    void prepareToPlay(double sr, int /*samplesPerBlock*/)
    {
        sampleRate = sr;
        reset();
    }

    void reset()
    {
        clockAccumulator = 0.0;
        clockTickCount = 0;
        currentBeat.store(0.0);
        running.store(false);

        extClockSampleCount = 0;
        extClockTickIndex = 0;
        for (auto& v : extIntervals)
            v = 0.0;
        extIntervalsFilled = 0;
        smoothedInterval = 0.0;
        lastExternalClockTime = 0;
        externalClockSeen.store(false);
        autoModeSending.store(true);

        outReadPos.store(0);
        outWritePos.store(0);
    }

    void processBlock(int numSamples, double sr)
    {
        sampleRate = sr;
        Mode currentMode = mode.load();

        if (currentMode == Mode::Auto)
        {
            if (externalClockSeen.load())
            {
                autoModeSending.store(false);
            }
            else
            {
                if (extClockSampleCount > sr)
                    autoModeSending.store(true);
            }
        }

        bool shouldSend = (currentMode == Mode::Internal)
                       || (currentMode == Mode::Auto && autoModeSending.load());

        if (shouldSend && running.load())
        {
            double bpm = internalTempo.load();
            double samplesPerTick = (sr * 60.0 / bpm) / 24.0;

            for (int i = 0; i < numSamples; ++i)
            {
                clockAccumulator += 1.0;
                if (clockAccumulator >= samplesPerTick)
                {
                    clockAccumulator -= samplesPerTick;
                    pushEvent({ ClockEvent::Clock, 0 });
                    clockTickCount++;
                    currentBeat.store(static_cast<double>(clockTickCount) / 24.0);
                }
            }
        }

        if (currentMode == Mode::External || (currentMode == Mode::Auto && !autoModeSending.load()))
        {
            extClockSampleCount += numSamples;
        }
    }

    void handleMidiMessage(const juce::MidiMessage& msg)
    {
        Mode currentMode = mode.load();

        if (msg.isMidiClock())
        {
            if (currentMode == Mode::External || currentMode == Mode::Auto)
                handleExternalClock();
            return;
        }

        if (msg.isMidiStart())
        {
            if (currentMode == Mode::External
                || (currentMode == Mode::Auto && !autoModeSending.load()))
            {
                clockTickCount = 0;
                currentBeat.store(0.0);
                running.store(true);
            }
            return;
        }

        if (msg.isMidiStop())
        {
            if (currentMode == Mode::External
                || (currentMode == Mode::Auto && !autoModeSending.load()))
            {
                running.store(false);
            }
            return;
        }

        if (msg.isMidiContinue())
        {
            if (currentMode == Mode::External
                || (currentMode == Mode::Auto && !autoModeSending.load()))
            {
                running.store(true);
            }
            return;
        }

        if (msg.isSongPositionPointer())
        {
            if (currentMode == Mode::External
                || (currentMode == Mode::Auto && !autoModeSending.load()))
            {
                int posBeats = msg.getSongPositionPointerMidiBeat();
                clockTickCount = posBeats * 6;
                currentBeat.store(static_cast<double>(clockTickCount) / 24.0);
            }
            return;
        }
    }

    // The entry for clock arriving on a MIDI input port. The controller's own ports are never an
    // external clock source, in any mode: the Launchkey can send MIDI clock by itself (its "MIDI Clock
    // Out" setting), and following it would make the controller the tempo master. Returns whether the
    // message was passed on.
    static bool isControllerPort(const juce::String& portName)
    {
        return portName.containsIgnoreCase("Launchkey");
    }

    bool handleMidiMessageFromPort(const juce::String& portName, const juce::MidiMessage& msg)
    {
        if (isControllerPort(portName))
            return false;
        handleMidiMessage(msg);
        return true;
    }

    void startTransport()
    {
        clockTickCount = 0;
        clockAccumulator = 0.0;
        currentBeat.store(0.0);
        running.store(true);

        Mode currentMode = mode.load();
        if (currentMode == Mode::Internal
            || (currentMode == Mode::Auto && autoModeSending.load()))
        {
            pushEvent({ ClockEvent::Start, 0 });
        }
    }

    void stopTransport()
    {
        running.store(false);

        Mode currentMode = mode.load();
        if (currentMode == Mode::Internal
            || (currentMode == Mode::Auto && autoModeSending.load()))
        {
            pushEvent({ ClockEvent::Stop, 0 });
        }
    }

    void continueTransport()
    {
        running.store(true);

        Mode currentMode = mode.load();
        if (currentMode == Mode::Internal
            || (currentMode == Mode::Auto && autoModeSending.load()))
        {
            pushEvent({ ClockEvent::Continue, 0 });
        }
    }

    void sendSongPosition()
    {
        Mode currentMode = mode.load();
        if (currentMode == Mode::Internal
            || (currentMode == Mode::Auto && autoModeSending.load()))
        {
            int posBeats = clockTickCount / 6;
            pushEvent({ ClockEvent::SongPosition, posBeats });
        }
    }

    void sendSongPosition(int positionInSixteenths)
    {
        Mode currentMode = mode.load();
        if (currentMode == Mode::Internal
            || (currentMode == Mode::Auto && autoModeSending.load()))
        {
            pushEvent({ ClockEvent::SongPosition, positionInSixteenths });
        }
    }

    bool popOutputEvent(ClockEvent& event)
    {
        int readIdx = outReadPos.load();
        int writeIdx = outWritePos.load();
        if (readIdx == writeIdx)
            return false;

        event = outBuffer[readIdx];
        outReadPos.store((readIdx + 1) % BUFFER_SIZE);
        return true;
    }

    int getPendingEventCount() const
    {
        int r = outReadPos.load();
        int w = outWritePos.load();
        return (w - r + BUFFER_SIZE) % BUFFER_SIZE;
    }

    bool isRunning() const { return running.load(); }
    double getCurrentBeat() const { return currentBeat.load(); }
    bool isExternalClockDetected() const { return externalClockSeen.load(); }
    bool isAutoModeSending() const { return autoModeSending.load(); }

private:
    void handleExternalClock()
    {
        externalClockSeen.store(true);

        if (running.load())
        {
            clockTickCount++;
            currentBeat.store(static_cast<double>(clockTickCount) / 24.0);
        }

        double interval = static_cast<double>(extClockSampleCount);
        extClockSampleCount = 0;

        if (interval <= 0.0)
            return;

        extIntervals[extClockTickIndex % TEMPO_AVERAGE_TICKS] = interval;
        extClockTickIndex++;
        if (extIntervalsFilled < TEMPO_AVERAGE_TICKS)
            extIntervalsFilled++;

        double sum = 0.0;
        for (int i = 0; i < extIntervalsFilled; ++i)
            sum += extIntervals[i];
        double avgInterval = sum / static_cast<double>(extIntervalsFilled);

        if (smoothedInterval <= 0.0)
            smoothedInterval = avgInterval;
        else
            smoothedInterval = smoothedInterval * JITTER_SMOOTH_COEFF
                             + avgInterval * (1.0 - JITTER_SMOOTH_COEFF);

        if (smoothedInterval > 0.0 && sampleRate > 0.0)
        {
            double bpm = (60.0 * sampleRate) / (smoothedInterval * 24.0);
            bpm = juce::jlimit(20.0, 300.0, bpm);
            detectedTempo.store(bpm);
        }
    }

    static constexpr int BUFFER_SIZE = 256;

    void pushEvent(const ClockEvent& evt)
    {
        int writeIdx = outWritePos.load();
        int nextWrite = (writeIdx + 1) % BUFFER_SIZE;
        if (nextWrite == outReadPos.load())
            return;
        outBuffer[writeIdx] = evt;
        outWritePos.store(nextWrite);
    }

    std::array<ClockEvent, BUFFER_SIZE> outBuffer {};
    std::atomic<int> outReadPos { 0 };
    std::atomic<int> outWritePos { 0 };

    static constexpr int TEMPO_AVERAGE_TICKS = 24;
    static constexpr double JITTER_SMOOTH_COEFF = 0.7;

    std::atomic<Mode> mode { Mode::Internal };
    std::atomic<double> internalTempo { 120.0 };
    std::atomic<double> detectedTempo { 0.0 };
    std::atomic<bool> running { false };
    std::atomic<double> currentBeat { 0.0 };
    std::atomic<bool> externalClockSeen { false };
    std::atomic<bool> autoModeSending { true };

    double sampleRate = 44100.0;
    double clockAccumulator = 0.0;
    int clockTickCount = 0;

    int extClockSampleCount = 0;
    int extClockTickIndex = 0;
    std::array<double, TEMPO_AVERAGE_TICKS> extIntervals {};
    int extIntervalsFilled = 0;
    double smoothedInterval = 0.0;
    double lastExternalClockTime = 0.0;
};
