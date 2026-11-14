#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../engine/AudioEngine.h"
#include "../sequencer/SequencerCore.h"
#include "../sync/MIDIClock.h"
#include "../arpeggiator/Arpeggiator.h"
#include "../project/FactoryPresets.h"
#include <memory>
#include <vector>

// Neptune Groovebox as an AU / VST3 instrument plugin.
//
// Ableton (or any DAW) loads this as a virtual instrument. The DAW provides:
//   - Audio output (stereo)
//   - MIDI input (keys and clock)
//   - Transport / tempo information
//
// The plugin creates its own AudioEngine, SequencerCore and MIDIClock,
// mirroring what main.cpp does for the standalone app. The Launchkey controller
// is optional: if connected, a future editor could drive it.

class NeptunePluginProcessor : public juce::AudioProcessor
{
public:
    NeptunePluginProcessor()
        : AudioProcessor(BusesProperties()
              .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
        sequencer = std::make_unique<SequencerCore>();
        audioEngine = std::make_unique<AudioEngine>();
        audioEngine->setSequencer(sequencer.get());

        midiClock = std::make_unique<MIDIClock>();
        midiClock->setTempo(sequencer->getTempo());
        audioEngine->setMIDIClock(midiClock.get());

        for (int t = 0; t < AudioEngine::MAX_TRACKS; ++t)
        {
            arps.push_back(std::make_unique<Arpeggiator>());
            auto* arp = arps.back().get();
            arp->setRandomSeed(0x1234567u + (uint32_t) t * 7919u);
            arp->onNoteOn = [this, t](int pitch, float velocity)
            {
                if (auto* engine = audioEngine->getEngine(t))
                    engine->noteOn(pitch, velocity, -1);
            };
            arp->onNoteOff = [this, t](int pitch)
            {
                if (auto* engine = audioEngine->getEngine(t))
                    engine->noteOff(pitch, -1);
            };
            audioEngine->setTrackArpeggiator(t, arp);
        }

        setupDefaultTracks();
    }

    ~NeptunePluginProcessor() override = default;

    const juce::String getName() const override { return "Neptune Groovebox"; }

    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }

    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    void getStateInformation(juce::MemoryBlock& /*destData*/) override {}
    void setStateInformation(const void* /*data*/, int /*sizeInBytes*/) override {}

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        audioEngine->prepare(sampleRate, samplesPerBlock);
    }

    void releaseResources() override
    {
        audioEngine->audioDeviceStopped();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ScopedNoDenormals noDenormals;

        if (auto* playHead = getPlayHead())
        {
            auto pos = playHead->getPosition();
            if (pos.hasValue())
            {
                if (auto bpm = pos->getBpm())
                {
                    if (*bpm > 0.0)
                    {
                        float tempo = static_cast<float>(*bpm);
                        if (std::abs(tempo - sequencer->getTempo()) > 0.01f)
                        {
                            sequencer->setTempo(tempo);
                            midiClock->setTempo(tempo);
                        }
                    }
                }

                auto clockMode = midiClock->getMode();
                if (clockMode == MIDIClock::Mode::External || clockMode == MIDIClock::Mode::Auto)
                {
                    bool playing = pos->getIsPlaying();
                    if (playing && !sequencer->isPlaying())
                        sequencer->start();
                    else if (!playing && sequencer->isPlaying())
                        sequencer->stop();
                }
            }
        }

        for (const auto metadata : midiMessages)
        {
            auto msg = metadata.getMessage();
            if (msg.isMidiClock() || msg.isMidiStart() || msg.isMidiStop()
                || msg.isMidiContinue() || msg.isSongPositionPointer())
            {
                midiClock->handleMidiMessage(msg);
            }
            else if (msg.isNoteOn())
            {
                int track = sequencer->getSelectedTrack();
                if (auto* engine = audioEngine->getEngine(track))
                    engine->noteOn(msg.getNoteNumber(), msg.getFloatVelocity(), -1);
            }
            else if (msg.isNoteOff())
            {
                int track = sequencer->getSelectedTrack();
                if (auto* engine = audioEngine->getEngine(track))
                    engine->noteOff(msg.getNoteNumber(), -1);
            }
        }

        const int numSamples = buffer.getNumSamples();
        buffer.clear();

        audioEngine->renderBlock(buffer.getWritePointer(0),
                                 buffer.getWritePointer(1),
                                 numSamples);
    }

    AudioEngine* getAudioEngine() { return audioEngine.get(); }
    SequencerCore* getSequencer() { return sequencer.get(); }

private:
    void setupDefaultTracks()
    {
        constexpr int NUM_DEFAULT_TRACKS = 8;
        for (int t = 0; t < NUM_DEFAULT_TRACKS; ++t)
        {
            auto engine = std::make_shared<UnifiedVoiceEngine>();
            audioEngine->addEngine(t, engine);
        }
    }

    std::unique_ptr<SequencerCore> sequencer;
    std::unique_ptr<AudioEngine> audioEngine;
    std::unique_ptr<MIDIClock> midiClock;
    std::vector<std::unique_ptr<Arpeggiator>> arps;
};
