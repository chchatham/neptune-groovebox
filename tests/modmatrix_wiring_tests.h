#pragma once

#include "../src/engine/UnifiedVoice.h"
#include "../src/dsp/ModMatrix.h"
#include <juce_core/juce_core.h>
#include <cmath>

class ModMatrixWiringTests : public juce::UnitTest
{
public:
    ModMatrixWiringTests() : UnitTest("ModMatrix Wiring") {}

    void runTest() override
    {
        beginTest("LFO modulating filter cutoff varies output over time");
        {
            // Set up a voice with LFO1 -> filter cutoff (param 10) mod slot
            UnifiedVoice voice;
            voice.setSampleRate(44100.0);
            voice.getAnalogCharacter().enabled = false; // disable drift for determinism
            voice.setFilterBaseCutoff(2000.0f);
            voice.setFilterResonance(0.5f);
            voice.getOsc(0).waveform = UnifiedVoice::OscWaveform::Saw;
            voice.getOsc(0).enabled = true;
            voice.getOsc(1).enabled = false;
            voice.getOsc(2).enabled = false;

            // Configure LFO1: slow enough to see variation across blocks
            auto& lfo = voice.getLFOBank().getLFO(0);
            lfo.setRate(2.0f);    // 2 Hz
            lfo.setDepth(1.0f);
            lfo.setShape(LFO::Shape::Sine);
            lfo.setTriggerMode(LFO::TriggerMode::KeySync);

            // Add mod slot: LFO1 -> filter cutoff, large depth
            ModSlot slot;
            slot.source = ModSource::LFO1;
            slot.destination = 10; // filter cutoff
            slot.depth = 5000.0f; // +/-5000 Hz modulation
            slot.bipolar = true;
            slot.active = true;
            voice.getModMatrix().addSlot(slot);

            // Set up envelope for sustained sound
            Envelope::Params ampParams;
            ampParams.attack = 0.001f;
            ampParams.decay = 0.01f;
            ampParams.sustain = 1.0f;
            ampParams.release = 0.1f;
            voice.getAmpEnvelope().setParams(ampParams);

            voice.setNote(60, 0.8f);
            voice.noteOn();

            // Collect RMS of blocks at different points in the LFO cycle
            const int blockSize = 512;
            const int numBlocks = 20;
            float rmsValues[20] = {};

            for (int b = 0; b < numBlocks; ++b)
            {
                float sumSq = 0.0f;
                for (int s = 0; s < blockSize; ++s)
                {
                    float sample = voice.process();
                    sumSq += sample * sample;
                }
                rmsValues[b] = std::sqrt(sumSq / blockSize);
            }

            // Verify that the RMS varies over time (not constant)
            float minRms = rmsValues[0], maxRms = rmsValues[0];
            for (int b = 1; b < numBlocks; ++b)
            {
                minRms = std::min(minRms, rmsValues[b]);
                maxRms = std::max(maxRms, rmsValues[b]);
            }

            // The LFO should cause measurable timbral variation
            float variation = maxRms - minRms;
            expect(variation > 0.001f,
                   "LFO->filter cutoff modulation should cause RMS variation over time, got " +
                   juce::String(variation));
        }

        beginTest("Envelope depth scaling: depth 0 vs depth 1.0");
        {
            // Two voices with same setup but different mod depth
            auto makeVoice = [](float modDepth) -> float
            {
                UnifiedVoice voice;
                voice.setSampleRate(44100.0);
                voice.getAnalogCharacter().enabled = false;
                voice.setFilterBaseCutoff(1000.0f);
                voice.getOsc(0).waveform = UnifiedVoice::OscWaveform::Saw;
                voice.getOsc(0).enabled = true;
                voice.getOsc(1).enabled = false;
                voice.getOsc(2).enabled = false;

                // Add an envelope to the chain
                Envelope::Params envParams;
                envParams.attack = 0.001f;
                envParams.decay = 0.05f;
                envParams.sustain = 0.5f;
                envParams.release = 0.1f;
                voice.getEnvelopeChain().addEnvelope(envParams);

                // Mod slot: Env1 -> filter cutoff
                ModSlot slot;
                slot.source = ModSource::Env1;
                slot.destination = 10;
                slot.depth = modDepth;
                slot.bipolar = false;
                slot.active = true;
                voice.getModMatrix().addSlot(slot);

                Envelope::Params ampParams;
                ampParams.attack = 0.001f;
                ampParams.decay = 0.01f;
                ampParams.sustain = 1.0f;
                ampParams.release = 0.5f;
                voice.getAmpEnvelope().setParams(ampParams);

                voice.setNote(60, 0.8f);
                voice.noteOn();

                float sumSq = 0.0f;
                for (int s = 0; s < 4096; ++s)
                {
                    float sample = voice.process();
                    sumSq += sample * sample;
                }
                return std::sqrt(sumSq / 4096.0f);
            };

            float rmsDepth0 = makeVoice(0.0f);
            float rmsDepth1 = makeVoice(8000.0f);

            // With depth 0, no modulation; with depth 8000, big filter sweep
            float diff = std::abs(rmsDepth1 - rmsDepth0);
            expect(diff > 0.001f,
                   "Different envelope depths should produce different RMS, diff=" +
                   juce::String(diff));
        }

        beginTest("Multiple sources summing to same destination");
        {
            UnifiedVoice voice;
            voice.setSampleRate(44100.0);
            voice.getAnalogCharacter().enabled = false;
            voice.setFilterBaseCutoff(1000.0f);
            voice.getOsc(0).waveform = UnifiedVoice::OscWaveform::Saw;
            voice.getOsc(0).enabled = true;
            voice.getOsc(1).enabled = false;
            voice.getOsc(2).enabled = false;

            // Activate LFO2
            voice.getLFOBank().addLFO();

            // LFO1: slow sine
            voice.getLFOBank().getLFO(0).setRate(1.0f);
            voice.getLFOBank().getLFO(0).setDepth(1.0f);
            voice.getLFOBank().getLFO(0).setShape(LFO::Shape::Sine);

            // LFO2: faster triangle
            voice.getLFOBank().getLFO(1).setRate(3.0f);
            voice.getLFOBank().getLFO(1).setDepth(1.0f);
            voice.getLFOBank().getLFO(1).setShape(LFO::Shape::Triangle);

            // Slot 1: LFO1 -> filter cutoff
            ModSlot slot1;
            slot1.source = ModSource::LFO1;
            slot1.destination = 10;
            slot1.depth = 3000.0f;
            slot1.bipolar = true;
            slot1.active = true;
            voice.getModMatrix().addSlot(slot1);

            // Slot 2: LFO2 -> filter cutoff (same dest)
            ModSlot slot2;
            slot2.source = ModSource::LFO2;
            slot2.destination = 10;
            slot2.depth = 2000.0f;
            slot2.bipolar = true;
            slot2.active = true;
            voice.getModMatrix().addSlot(slot2);

            Envelope::Params ampParams;
            ampParams.attack = 0.001f;
            ampParams.sustain = 1.0f;
            ampParams.release = 0.5f;
            voice.getAmpEnvelope().setParams(ampParams);

            voice.setNote(60, 0.8f);
            voice.noteOn();

            // Run and verify the mod matrix has both sources contributing
            // Process enough to get past the attack
            for (int s = 0; s < 256; ++s)
                voice.process();

            // Now check: getModulatedValue for dest 10 with base 0 should reflect
            // the sum of both LFO contributions
            float lfo1Val = voice.getModMatrix().getSourceValue(ModSource::LFO1);
            float lfo2Val = voice.getModMatrix().getSourceValue(ModSource::LFO2);
            float expectedMod = lfo1Val * 3000.0f + lfo2Val * 2000.0f;
            float actualMod = voice.getModMatrix().getModulatedValue(10, 0.0f);

            expectWithinAbsoluteError(actualMod, expectedMod, 0.01f,
                "Two mod slots targeting same dest should sum their contributions");
        }

        beginTest("Velocity as mod source: different velocities produce different modulation");
        {
            auto renderWithVelocity = [](float velocity) -> float
            {
                UnifiedVoice voice;
                voice.setSampleRate(44100.0);
                voice.getAnalogCharacter().enabled = false;
                voice.setFilterBaseCutoff(1000.0f);
                voice.getOsc(0).waveform = UnifiedVoice::OscWaveform::Saw;
                voice.getOsc(0).enabled = true;
                voice.getOsc(1).enabled = false;
                voice.getOsc(2).enabled = false;

                // Velocity -> filter cutoff
                ModSlot slot;
                slot.source = ModSource::Velocity;
                slot.destination = 10;
                slot.depth = 5000.0f;
                slot.bipolar = false;
                slot.active = true;
                voice.getModMatrix().addSlot(slot);

                Envelope::Params ampParams;
                ampParams.attack = 0.001f;
                ampParams.sustain = 1.0f;
                ampParams.release = 0.5f;
                voice.getAmpEnvelope().setParams(ampParams);

                voice.setNote(60, velocity);
                voice.noteOn();

                // Process enough samples past the attack
                for (int s = 0; s < 256; ++s)
                    voice.process();

                // Check the velocity source value
                return voice.getModMatrix().getSourceValue(ModSource::Velocity);
            };

            float velLow = renderWithVelocity(0.2f);
            float velHigh = renderWithVelocity(1.0f);

            expectWithinAbsoluteError(velLow, 0.2f, 0.01f,
                "Velocity source should reflect note velocity (low)");
            expectWithinAbsoluteError(velHigh, 1.0f, 0.01f,
                "Velocity source should reflect note velocity (high)");
            expect(velHigh > velLow,
                   "Higher velocity should produce higher mod source value");
        }

        beginTest("Mod slot active/inactive: inactive slot has no effect");
        {
            auto renderWithSlotActive = [](bool slotActive) -> float
            {
                UnifiedVoice voice;
                voice.setSampleRate(44100.0);
                voice.getAnalogCharacter().enabled = false;
                voice.setFilterBaseCutoff(2000.0f);
                voice.getOsc(0).waveform = UnifiedVoice::OscWaveform::Saw;
                voice.getOsc(0).enabled = true;
                voice.getOsc(1).enabled = false;
                voice.getOsc(2).enabled = false;

                // LFO at constant max (square wave at very low rate)
                voice.getLFOBank().getLFO(0).setRate(0.001f);
                voice.getLFOBank().getLFO(0).setDepth(1.0f);
                voice.getLFOBank().getLFO(0).setShape(LFO::Shape::Square);

                // Add slot then set active/inactive
                ModSlot slot;
                slot.source = ModSource::LFO1;
                slot.destination = 10;
                slot.depth = 10000.0f;
                slot.bipolar = true;
                voice.getModMatrix().addSlot(slot);
                // addSlot forces active=true, so override after
                voice.getModMatrix().getSlot(0).active = slotActive;

                Envelope::Params ampParams;
                ampParams.attack = 0.001f;
                ampParams.sustain = 1.0f;
                ampParams.release = 0.5f;
                voice.getAmpEnvelope().setParams(ampParams);

                voice.setNote(60, 0.8f);
                voice.noteOn();

                float sumSq = 0.0f;
                for (int s = 0; s < 2048; ++s)
                {
                    float sample = voice.process();
                    sumSq += sample * sample;
                }
                return std::sqrt(sumSq / 2048.0f);
            };

            float rmsActive = renderWithSlotActive(true);
            float rmsInactive = renderWithSlotActive(false);

            // Active slot causes a big filter cutoff shift; inactive does not
            float diff = std::abs(rmsActive - rmsInactive);
            expect(diff > 0.001f,
                   "Active vs inactive mod slot should produce different output, diff=" +
                   juce::String(diff));
        }

        beginTest("Per-voice independence: different notes get different KeyTrack values");
        {
            UnifiedVoice voiceLow, voiceHigh;

            auto setup = [](UnifiedVoice& voice)
            {
                voice.setSampleRate(44100.0);
                voice.getAnalogCharacter().enabled = false;
                voice.setFilterBaseCutoff(2000.0f);
                voice.getOsc(0).waveform = UnifiedVoice::OscWaveform::Saw;
                voice.getOsc(0).enabled = true;
                voice.getOsc(1).enabled = false;
                voice.getOsc(2).enabled = false;

                // KeyTrack -> filter cutoff
                ModSlot slot;
                slot.source = ModSource::KeyTrack;
                slot.destination = 10;
                slot.depth = 5000.0f;
                slot.bipolar = true;
                slot.active = true;
                voice.getModMatrix().addSlot(slot);

                Envelope::Params ampParams;
                ampParams.attack = 0.001f;
                ampParams.sustain = 1.0f;
                ampParams.release = 0.5f;
                voice.getAmpEnvelope().setParams(ampParams);
            };

            setup(voiceLow);
            setup(voiceHigh);

            // Note 36 (low) vs note 84 (high) — both offset from 60
            voiceLow.setNote(36, 0.8f);
            voiceLow.noteOn();

            voiceHigh.setNote(84, 0.8f);
            voiceHigh.noteOn();

            // Process to populate KeyTrack values
            for (int s = 0; s < 256; ++s)
            {
                voiceLow.process();
                voiceHigh.process();
            }

            float ktLow = voiceLow.getModMatrix().getSourceValue(ModSource::KeyTrack);
            float ktHigh = voiceHigh.getModMatrix().getSourceValue(ModSource::KeyTrack);

            // Note 36: (36-60)/60 = -0.4
            // Note 84: (84-60)/60 = 0.4
            expectWithinAbsoluteError(ktLow, -0.4f, 0.01f,
                "KeyTrack for note 36 should be -0.4");
            expectWithinAbsoluteError(ktHigh, 0.4f, 0.01f,
                "KeyTrack for note 84 should be 0.4");
            expect(ktHigh > ktLow,
                   "Higher note should produce higher KeyTrack value");
        }
    }
};

static ModMatrixWiringTests modMatrixWiringTests;
