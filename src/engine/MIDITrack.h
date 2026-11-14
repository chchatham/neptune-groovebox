#pragma once

#include "EngineBase.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>

class MIDITrackEngine : public EngineBase
{
public:
    MIDITrackEngine()
    {
        // Known starting values for the paged controllers; nothing is sent until one is turned.
        for (auto& page : pages())
            for (auto& p : page.params)
                if (p.isValid())
                    params[p.id] = p.defaultVal;
    }

    EngineType getType() const override { return EngineType::MIDITrack; }
    juce::String getName() const override { return "MIDI Track"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiOut) override
    {
        juce::ignoreUnused(buffer);

        int readIdx = outReadPos.load();
        int writeIdx = outWritePos.load();

        while (readIdx != writeIdx)
        {
            auto& evt = outBuffer[readIdx];
            if (evt.isNoteOn)
                midiOut.addEvent(juce::MidiMessage::noteOn(midiChannel, evt.note, (juce::uint8) evt.value), 0);
            else if (evt.isNoteOff)
                midiOut.addEvent(juce::MidiMessage::noteOff(midiChannel, evt.note), 0);
            else if (evt.kind == Program)
                midiOut.addEvent(juce::MidiMessage::programChange(midiChannel, evt.value), 0);
            else if (evt.kind == Bend)
                midiOut.addEvent(juce::MidiMessage::pitchWheel(midiChannel, evt.value), 0);
            else if (evt.kind == PolyPressure)
                midiOut.addEvent(juce::MidiMessage::aftertouchChange(midiChannel, evt.note, evt.value), 0);
            else
                midiOut.addEvent(juce::MidiMessage::controllerEvent(midiChannel, evt.note, evt.value), 0);

            readIdx = (readIdx + 1) % BUFFER_SIZE;
        }
        outReadPos.store(readIdx);
    }

    void releaseResources() override {}

    void noteOn(int note, float velocity, int /*voiceIndex*/) override
    {
        MIDIEvent evt;
        evt.isNoteOn = true;
        evt.note = note;
        evt.value = juce::jlimit(0, 127, (int)(velocity * 127.0f));
        pushEvent(evt);
    }

    void noteOff(int note, int /*voiceIndex*/) override
    {
        MIDIEvent evt;
        evt.isNoteOff = true;
        evt.note = note;
        pushEvent(evt);
    }

    void allNotesOff() override
    {
        for (int n = 0; n < 128; ++n)
        {
            MIDIEvent evt;
            evt.isNoteOff = true;
            evt.note = n;
            pushEvent(evt);
        }
    }

    static constexpr int CHANNEL_PARAM = 128;   // 1..16
    static constexpr int PROGRAM_PARAM = 129;   // 0..127, sent as a program change when turned

    // MIDI (channel, program and the standard performance controllers), SOUND (the standard sound
    // controllers most synths answer to) and CC (general-purpose controllers 16-23).
    static const ParamPages& pages()
    {
        auto I = ParamKind::Int; auto L = ParamKind::Linear;
        static const ParamPages list = {
            { "MIDI", "MIDI", {{
                { "CHAN", CHANNEL_PARAM, 1, 16, 1, I },
                { "PROG", PROGRAM_PARAM, 0, 127, 0, I },
                { "VOL",  7, 0, 1, 100.0f / 127.0f, L, "cc" },
                { "PAN",  10, 0, 1, 64.0f / 127.0f, L, "cc" },
                { "MOD",  1, 0, 1, 0, L, "cc" },
                { "EXPR", 11, 0, 1, 1, L, "cc" },
                { "CUT",  74, 0, 1, 64.0f / 127.0f, L, "cc" },
                { "RESO", 71, 0, 1, 64.0f / 127.0f, L, "cc" } }} },
            { "SOUND", "MIDI", {{
                { "ATK",  73, 0, 1, 64.0f / 127.0f, L, "cc" },
                { "DEC",  75, 0, 1, 64.0f / 127.0f, L, "cc" },
                { "REL",  72, 0, 1, 64.0f / 127.0f, L, "cc" },
                { "VRAT", 76, 0, 1, 64.0f / 127.0f, L, "cc" },
                { "VDPT", 77, 0, 1, 64.0f / 127.0f, L, "cc" },
                { "VDLY", 78, 0, 1, 64.0f / 127.0f, L, "cc" },
                { "RVB",  91, 0, 1, 0, L, "cc" },
                { "CHOR", 93, 0, 1, 0, L, "cc" } }} },
            { "CC", "MIDI", {{
                { "CC16", 16, 0, 1, 0, L, "cc" }, { "CC17", 17, 0, 1, 0, L, "cc" },
                { "CC18", 18, 0, 1, 0, L, "cc" }, { "CC19", 19, 0, 1, 0, L, "cc" },
                { "CC20", 20, 0, 1, 0, L, "cc" }, { "CC21", 21, 0, 1, 0, L, "cc" },
                { "CC22", 22, 0, 1, 0, L, "cc" }, { "CC23", 23, 0, 1, 0, L, "cc" } }} },
        };
        return list;
    }

    ParamPages getParamPages() const override { return pages(); }

    void pitchBend(float amount) override
    {
        MIDIEvent evt;
        evt.kind = Bend;
        evt.value = juce::jlimit(0, 16383, 8192 + (int) (juce::jlimit(-1.0f, 1.0f, amount) * 8191.0f));
        pushEvent(evt);
    }

    void modWheel(float value) override { setParameter(1, value); }

    void aftertouch(int note, float pressure) override
    {
        MIDIEvent evt;
        evt.kind = PolyPressure;
        evt.note = juce::jlimit(0, 127, note);
        evt.value = juce::jlimit(0, 127, (int) (pressure * 127.0f));
        pushEvent(evt);
    }

    void setParameter(ParamId id, float value) override
    {
        params[id] = value;

        if (id == CHANNEL_PARAM)
        {
            setMidiChannel((int) std::lround(value));
            return;
        }
        if (id == PROGRAM_PARAM)
        {
            MIDIEvent evt;
            evt.kind = Program;
            evt.value = juce::jlimit(0, 127, (int) std::lround(value));
            pushEvent(evt);
            return;
        }

        if (id >= 0 && id < 128)
        {
            MIDIEvent evt;
            evt.note = id;
            evt.value = juce::jlimit(0, 127, (int)(juce::jlimit(0.0f, 1.0f, value) * 127.0f));
            pushEvent(evt);
        }
    }

    float getParameter(ParamId id) const override
    {
        auto it = params.find(id);
        return it != params.end() ? it->second : 0.0f;
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (id == CHANNEL_PARAM) return "MIDI Channel";
        if (id == PROGRAM_PARAM) return "Program";
        return "CC " + juce::String(id);
    }

    int getParameterCount() const override { return 130; }

    void setMidiChannel(int ch) { midiChannel = juce::jlimit(1, 16, ch); }
    int getMidiChannel() const { return midiChannel; }

    int getPendingEventCount() const
    {
        int r = outReadPos.load();
        int w = outWritePos.load();
        return (w - r + BUFFER_SIZE) % BUFFER_SIZE;
    }

private:
    struct MIDIEvent
    {
        bool isNoteOn = false;
        bool isNoteOff = false;
        int note = 0;
        int value = 0;
        int kind = 0;   // Controller unless a note flag is set
    };
    enum EventKind { Controller = 0, Program, Bend, PolyPressure };

    static constexpr int BUFFER_SIZE = 256;
    std::array<MIDIEvent, BUFFER_SIZE> outBuffer {};
    std::atomic<int> outReadPos { 0 };
    std::atomic<int> outWritePos { 0 };

    void pushEvent(const MIDIEvent& evt)
    {
        int writeIdx = outWritePos.load();
        int nextWrite = (writeIdx + 1) % BUFFER_SIZE;
        if (nextWrite == outReadPos.load())
            return;
        outBuffer[writeIdx] = evt;
        outWritePos.store(nextWrite);
    }

    int midiChannel = 1;
    double sampleRate = 44100.0;
    ParamMap params;
};
