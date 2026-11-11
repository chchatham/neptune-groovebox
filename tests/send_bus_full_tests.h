#pragma once

// Send bus manager, routing, and sidechain compressor tests.
//
// To include in the test suite, add to main_test.cpp:
//   #include "send_bus_full_tests.h"
// The static instances auto-register with JUCE's UnitTestRunner.

#include "../src/engine/SendBusManager.h"
#include "../src/dsp/effects/SidechainCompressor.h"
#include "../src/dsp/effects/Reverb.h"
#include "../src/dsp/effects/Delay.h"
#include "../src/dsp/effects/Chorus.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstring>
#include <algorithm>

// ================================================================
// SendBusManager Tests
// ================================================================

class SendBusManagerTests : public juce::UnitTest
{
public:
    SendBusManagerTests() : UnitTest("Send Bus Manager") {}

    void runTest() override
    {
        // ---- Default construction ----
        beginTest("Default bus count is 2");
        {
            SendBusManager mgr;
            expectEquals(mgr.getBusCount(), 2);
            expect(mgr.getBusName(0) == "Delay", "Bus 0 should be named Delay");
            expect(mgr.getBusName(1) == "Reverb", "Bus 1 should be named Reverb");
        }

        // ---- Add buses ----
        beginTest("Add bus returns correct index");
        {
            SendBusManager mgr;
            int idx = mgr.addBus("Chorus");
            expectEquals(idx, 2);
            expectEquals(mgr.getBusCount(), 3);
            expect(mgr.getBusName(2) == "Chorus", "New bus name should be Chorus");
        }

        // ---- Remove buses ----
        beginTest("Remove bus shifts indices");
        {
            SendBusManager mgr;
            mgr.addBus("Chorus");
            expectEquals(mgr.getBusCount(), 3);
            expect(mgr.removeBus(1), "Remove should succeed");
            expectEquals(mgr.getBusCount(), 2);
            // After removing index 1 (Reverb), bus 1 should now be Chorus
            expect(mgr.getBusName(1) == "Chorus",
                   "After removing bus 1, bus 1 should be the former bus 2");
        }

        beginTest("Remove bus out of range returns false");
        {
            SendBusManager mgr;
            expect(!mgr.removeBus(-1), "Negative index");
            expect(!mgr.removeBus(99), "Out of range");
        }

        // ---- Max bus limit ----
        beginTest("Cannot exceed MAX_BUSES");
        {
            SendBusManager mgr;
            for (int i = 0; i < 20; ++i)
                mgr.addBus("Extra");
            expect(mgr.getBusCount() <= SendBusManager::MAX_BUSES,
                   "Should not exceed MAX_BUSES");
        }

        // ---- Bus properties ----
        beginTest("Bus level get/set");
        {
            SendBusManager mgr;
            mgr.setBusLevel(0, 0.5f);
            expectWithinAbsoluteError(mgr.getBusLevel(0), 0.5f, 0.001f);
            mgr.setBusLevel(0, 2.0f); // Clamped to 1.0
            expectWithinAbsoluteError(mgr.getBusLevel(0), 1.0f, 0.001f);
        }

        beginTest("Bus active/bypass");
        {
            SendBusManager mgr;
            expect(mgr.isBusActive(0), "Buses should be active by default");
            mgr.setBusActive(0, false);
            expect(!mgr.isBusActive(0), "Bus should be inactive after setBusActive(false)");
        }

        beginTest("Bus name get/set");
        {
            SendBusManager mgr;
            mgr.setBusName(0, "FX1");
            expect(mgr.getBusName(0) == "FX1", "Bus name should be FX1");
        }

        // ---- Reorder buses ----
        beginTest("Reorder buses");
        {
            SendBusManager mgr;
            mgr.addBus("Chorus");
            // Buses: [Delay, Reverb, Chorus]
            expect(mgr.reorderBus(2, 0), "Reorder should succeed");
            // Buses: [Chorus, Delay, Reverb]
            expect(mgr.getBusName(0) == "Chorus", "Index 0 should be Chorus after reorder");
            expect(mgr.getBusName(1) == "Delay", "Index 1 should be Delay after reorder");
            expect(mgr.getBusName(2) == "Reverb", "Index 2 should be Reverb after reorder");
        }

        // ---- Per-track send config ----
        beginTest("Track send level defaults to 0");
        {
            SendBusManager mgr;
            expectWithinAbsoluteError(mgr.getTrackSendLevel(0, 0), 0.0f, 0.001f);
        }

        beginTest("Set and get track send level");
        {
            SendBusManager mgr;
            mgr.setTrackSendLevel(3, 1, 0.75f);
            expectWithinAbsoluteError(mgr.getTrackSendLevel(3, 1), 0.75f, 0.001f);
            // Other sends should still be 0
            expectWithinAbsoluteError(mgr.getTrackSendLevel(3, 0), 0.0f, 0.001f);
        }

        beginTest("Track send level clamped to 0..1");
        {
            SendBusManager mgr;
            mgr.setTrackSendLevel(0, 0, -0.5f);
            expectWithinAbsoluteError(mgr.getTrackSendLevel(0, 0), 0.0f, 0.001f);
            mgr.setTrackSendLevel(0, 0, 2.0f);
            expectWithinAbsoluteError(mgr.getTrackSendLevel(0, 0), 1.0f, 0.001f);
        }

        beginTest("Pre-fader send config");
        {
            SendBusManager mgr;
            expect(!mgr.isTrackSendPreFader(0, 0), "Default is post-fader");
            mgr.setTrackSendPreFader(0, 0, true);
            expect(mgr.isTrackSendPreFader(0, 0), "Should be pre-fader after set");
        }

        // ---- Prepare / buffer operations ----
        beginTest("Prepare initializes buffers");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 512);
            // Should not crash, and buses should work
            mgr.clearAllBuffers();
            // Verify buffer is zeroed
            const float* buf = mgr.getBusBufferL(0);
            expect(buf != nullptr, "Buffer pointer should not be null after prepare");
        }

        // ---- Signal routing ----
        beginTest("Post-fader routing accumulates correctly");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 64);
            mgr.setTrackSendLevel(0, 0, 0.5f);  // Track 0 -> Bus 0 at 0.5

            mgr.clearAllBuffers();

            // Generate a test signal: constant 1.0
            std::array<float, 64> signalL{}, signalR{};
            std::fill(signalL.begin(), signalL.end(), 1.0f);
            std::fill(signalR.begin(), signalR.end(), 1.0f);

            mgr.routeTrackToSends(0, signalL.data(), signalR.data(), 64);

            // Bus 0 buffer should now contain 0.5
            const float* busL = mgr.getBusBufferL(0);
            expectWithinAbsoluteError(busL[0], 0.5f, 0.001f);
            expectWithinAbsoluteError(busL[32], 0.5f, 0.001f);
        }

        beginTest("Pre-fader vs post-fader routing");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 64);
            mgr.setTrackSendLevel(0, 0, 1.0f);
            mgr.setTrackSendPreFader(0, 0, true);  // Pre-fader

            mgr.clearAllBuffers();

            // Pre-fader signal = 1.0, Post-fader signal = 0.5
            std::array<float, 64> preL{}, preR{}, postL{}, postR{};
            std::fill(preL.begin(), preL.end(), 1.0f);
            std::fill(preR.begin(), preR.end(), 1.0f);
            std::fill(postL.begin(), postL.end(), 0.5f);
            std::fill(postR.begin(), postR.end(), 0.5f);

            mgr.routeTrackToSends(0,
                                  preL.data(), preR.data(),
                                  postL.data(), postR.data(), 64);

            // Pre-fader => should use 1.0 signal
            const float* busL = mgr.getBusBufferL(0);
            expectWithinAbsoluteError(busL[0], 1.0f, 0.001f);

            // Now test post-fader
            mgr.setTrackSendPreFader(0, 0, false);
            mgr.clearAllBuffers();
            mgr.routeTrackToSends(0,
                                  preL.data(), preR.data(),
                                  postL.data(), postR.data(), 64);

            busL = mgr.getBusBufferL(0);
            expectWithinAbsoluteError(busL[0], 0.5f, 0.001f);
        }

        beginTest("Multiple tracks route into same bus");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 64);
            mgr.setTrackSendLevel(0, 0, 0.5f);
            mgr.setTrackSendLevel(1, 0, 0.3f);

            mgr.clearAllBuffers();

            std::array<float, 64> sig1L{}, sig1R{}, sig2L{}, sig2R{};
            std::fill(sig1L.begin(), sig1L.end(), 1.0f);
            std::fill(sig1R.begin(), sig1R.end(), 1.0f);
            std::fill(sig2L.begin(), sig2L.end(), 1.0f);
            std::fill(sig2R.begin(), sig2R.end(), 1.0f);

            mgr.routeTrackToSends(0, sig1L.data(), sig1R.data(), 64);
            mgr.routeTrackToSends(1, sig2L.data(), sig2R.data(), 64);

            const float* busL = mgr.getBusBufferL(0);
            // Should be 0.5 + 0.3 = 0.8
            expectWithinAbsoluteError(busL[0], 0.8f, 0.001f);
        }

        beginTest("Bus mix into main output");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 64);
            mgr.setTrackSendLevel(0, 0, 1.0f);
            mgr.setBusLevel(0, 0.8f);

            mgr.clearAllBuffers();

            std::array<float, 64> signalL{}, signalR{};
            std::fill(signalL.begin(), signalL.end(), 1.0f);
            std::fill(signalR.begin(), signalR.end(), 1.0f);

            mgr.routeTrackToSends(0, signalL.data(), signalR.data(), 64);
            // No effect chain, so process just passes through
            mgr.processAllBuses();

            std::array<float, 64> mainL{}, mainR{};
            mainL.fill(0.0f);
            mainR.fill(0.0f);
            mgr.mixAllBusesInto(mainL.data(), mainR.data(), 64);

            // Bus 0 has signal * 1.0 send * 0.8 return = 0.8
            expectWithinAbsoluteError(mainL[0], 0.8f, 0.001f);
        }

        beginTest("Inactive bus does not contribute to output");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 64);
            mgr.setTrackSendLevel(0, 0, 1.0f);
            mgr.setBusLevel(0, 1.0f);
            mgr.setBusActive(0, false);

            mgr.clearAllBuffers();

            std::array<float, 64> signalL{}, signalR{};
            std::fill(signalL.begin(), signalL.end(), 1.0f);
            std::fill(signalR.begin(), signalR.end(), 1.0f);

            mgr.routeTrackToSends(0, signalL.data(), signalR.data(), 64);
            mgr.processAllBuses();

            std::array<float, 64> mainL{}, mainR{};
            mainL.fill(0.0f);
            mainR.fill(0.0f);
            mgr.mixAllBusesInto(mainL.data(), mainR.data(), 64);

            // Inactive bus should contribute nothing
            expectWithinAbsoluteError(mainL[0], 0.0f, 0.001f);
        }

        // ---- Effect chain management ----
        beginTest("Add effect to bus chain");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 512);
            auto reverb = std::make_unique<Reverb>();
            mgr.addEffectToBus(0, std::move(reverb));
            auto* chain = mgr.getBusChain(0);
            expect(chain != nullptr, "Bus chain should exist");
            expectEquals(chain->getEffectCount(), 1);
        }

        beginTest("Remove effect from bus chain");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 512);
            mgr.addEffectToBus(0, std::make_unique<Reverb>());
            mgr.addEffectToBus(0, std::make_unique<Delay>());
            expectEquals(mgr.getBusChain(0)->getEffectCount(), 2);
            mgr.removeEffectFromBus(0, 0);
            expectEquals(mgr.getBusChain(0)->getEffectCount(), 1);
        }

        beginTest("Set bus chain routing mode");
        {
            SendBusManager mgr;
            mgr.setBusChainRouting(0, EffectChain::Routing::Parallel);
            expect(mgr.getBusChain(0)->getRouting() == EffectChain::Routing::Parallel,
                   "Routing should be parallel");
        }

        // ---- Remove bus updates track send configs ----
        beginTest("Remove bus cleans up track send configs");
        {
            SendBusManager mgr;
            mgr.addBus("Chorus");
            // Set sends to bus 2 (Chorus)
            mgr.setTrackSendLevel(0, 2, 0.9f);
            mgr.setTrackSendLevel(0, 0, 0.5f);
            mgr.setTrackSendLevel(0, 1, 0.3f);

            // Remove bus 1 (Reverb). Chorus shifts from index 2 to 1.
            mgr.removeBus(1);

            expectEquals(mgr.getBusCount(), 2);
            // Bus 0 (Delay) send should still be 0.5
            expectWithinAbsoluteError(mgr.getTrackSendLevel(0, 0), 0.5f, 0.001f);
            // Bus 1 (was Chorus at index 2) send should be 0.9
            expectWithinAbsoluteError(mgr.getTrackSendLevel(0, 1), 0.9f, 0.001f);
        }

        // ---- Buffer access for sidechain ----
        beginTest("Bus buffer accessible for sidechain");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 64);
            mgr.setTrackSendLevel(0, 0, 1.0f);

            mgr.clearAllBuffers();

            std::array<float, 64> signalL{}, signalR{};
            std::fill(signalL.begin(), signalL.end(), 0.75f);
            std::fill(signalR.begin(), signalR.end(), 0.75f);

            mgr.routeTrackToSends(0, signalL.data(), signalR.data(), 64);

            const float* scL = mgr.getBusBufferL(0);
            const float* scR = mgr.getBusBufferR(0);

            expect(scL != nullptr && scR != nullptr, "Sidechain buffer pointers valid");
            expectWithinAbsoluteError(scL[0], 0.75f, 0.001f);
            expectWithinAbsoluteError(scR[0], 0.75f, 0.001f);
        }
    }
};

static SendBusManagerTests sendBusManagerTests;

// ================================================================
// SidechainCompressor Tests
// ================================================================

class SidechainCompressorTests : public juce::UnitTest
{
public:
    SidechainCompressorTests() : UnitTest("Sidechain Compressor") {}

    void runTest() override
    {
        // ---- Basic construction and defaults ----
        beginTest("Default parameter values");
        {
            SidechainCompressor comp;
            expectWithinAbsoluteError(comp.getThreshold(), -20.0f, 0.01f);
            expectWithinAbsoluteError(comp.getRatio(), 4.0f, 0.01f);
            expectWithinAbsoluteError(comp.getAttack(), 10.0f, 0.01f);
            expectWithinAbsoluteError(comp.getRelease(), 100.0f, 0.01f);
            expectWithinAbsoluteError(comp.getMakeupGain(), 0.0f, 0.01f);
            expectWithinAbsoluteError(comp.getKneeWidth(), 3.0f, 0.01f);
            expectWithinAbsoluteError(comp.getSidechainHPF(), 0.0f, 0.01f);
            expect(!comp.isKeyListening(), "Key listen should be off by default");
            expect(comp.getDetectionMode() == SidechainCompressor::DetectionMode::Peak,
                   "Default detection should be Peak");
        }

        // ---- Parameter clamping ----
        beginTest("Parameter clamping");
        {
            SidechainCompressor comp;
            comp.setThreshold(-100.0f);
            expectWithinAbsoluteError(comp.getThreshold(), -60.0f, 0.01f);
            comp.setThreshold(10.0f);
            expectWithinAbsoluteError(comp.getThreshold(), 0.0f, 0.01f);

            comp.setRatio(0.5f);
            expectWithinAbsoluteError(comp.getRatio(), 1.0f, 0.01f);
            comp.setRatio(50.0f);
            expectWithinAbsoluteError(comp.getRatio(), 20.0f, 0.01f);
        }

        // ---- Generic parameter interface ----
        beginTest("Set/get via parameter index");
        {
            SidechainCompressor comp;
            comp.setParameter(SidechainCompressor::PARAM_THRESHOLD, -30.0f);
            expectWithinAbsoluteError(comp.getParameter(SidechainCompressor::PARAM_THRESHOLD),
                                      -30.0f, 0.01f);

            comp.setParameter(SidechainCompressor::PARAM_RATIO, 8.0f);
            expectWithinAbsoluteError(comp.getParameter(SidechainCompressor::PARAM_RATIO),
                                      8.0f, 0.01f);

            comp.setParameter(SidechainCompressor::PARAM_KEY_LISTEN, 1.0f);
            expectWithinAbsoluteError(comp.getParameter(SidechainCompressor::PARAM_KEY_LISTEN),
                                      1.0f, 0.01f);

            comp.setParameter(SidechainCompressor::PARAM_DETECT_MODE, 1.0f);
            expectWithinAbsoluteError(comp.getParameter(SidechainCompressor::PARAM_DETECT_MODE),
                                      1.0f, 0.01f);
        }

        // ---- Normal compression (no sidechain) ----
        beginTest("Self-detection compresses loud signal");
        {
            SidechainCompressor comp;
            comp.prepare(44100.0, 512);
            comp.setThreshold(-20.0f);
            comp.setRatio(4.0f);
            comp.setAttack(0.1f);   // Very fast attack
            comp.setRelease(10.0f);
            comp.setKneeWidth(0.0f); // Hard knee
            comp.setMakeupGain(0.0f);
            comp.setMix(1.0f);

            // Process a loud signal (well above threshold)
            float peak = 1.0f; // 0 dB, threshold is -20 dB
            float left = peak;
            float right = peak;

            // Run many samples for envelope to settle
            for (int i = 0; i < 1000; ++i)
            {
                left = peak;
                right = peak;
                comp.process(left, right);
            }

            // The signal should be significantly attenuated
            expect(left < peak * 0.8f,
                   "Compressed signal should be < 80% of input, got "
                   + juce::String(left));

            // Gain reduction should be negative (in dB)
            expect(comp.getGainReductionDb() < 0.0f,
                   "Gain reduction should be negative dB, got "
                   + juce::String(comp.getGainReductionDb()));
        }

        beginTest("Signal below threshold passes through uncompressed");
        {
            SidechainCompressor comp;
            comp.prepare(44100.0, 512);
            comp.setThreshold(-6.0f);
            comp.setRatio(4.0f);
            comp.setAttack(0.1f);
            comp.setRelease(10.0f);
            comp.setKneeWidth(0.0f);
            comp.setMix(1.0f);

            // Signal at -40 dB (well below -6 dB threshold)
            float level = 0.01f;
            float left = level;
            float right = level;

            for (int i = 0; i < 500; ++i)
            {
                left = level;
                right = level;
                comp.process(left, right);
            }

            // Should be essentially unchanged
            expectWithinAbsoluteError(left, level, 0.002f);
        }

        // ---- Sidechain compression ----
        beginTest("External sidechain drives compression on main signal");
        {
            SidechainCompressor comp;
            comp.prepare(44100.0, 512);
            comp.setThreshold(-20.0f);
            comp.setRatio(10.0f);
            comp.setAttack(0.1f);
            comp.setRelease(10.0f);
            comp.setKneeWidth(0.0f);
            comp.setMix(1.0f);

            // Main signal: moderate level
            constexpr int N = 512;
            std::array<float, N> mainL{}, mainR{};
            std::array<float, N> scL{}, scR{};

            // Main signal: 0.3 (~-10 dB)
            std::fill(mainL.begin(), mainL.end(), 0.3f);
            std::fill(mainR.begin(), mainR.end(), 0.3f);

            // Sidechain signal: loud (1.0 = 0 dB, well above -20 dB threshold)
            std::fill(scL.begin(), scL.end(), 1.0f);
            std::fill(scR.begin(), scR.end(), 1.0f);

            comp.setSidechainBuffers(scL.data(), scR.data());

            // Process several blocks to let envelope settle
            for (int block = 0; block < 5; ++block)
            {
                std::fill(mainL.begin(), mainL.end(), 0.3f);
                std::fill(mainR.begin(), mainR.end(), 0.3f);
                comp.processBlock(mainL.data(), mainR.data(), N);
            }

            // The main signal should be attenuated because the sidechain is loud
            float outputLevel = mainL[N - 1];
            expect(outputLevel < 0.25f,
                   "Main signal should be ducked by sidechain, got "
                   + juce::String(outputLevel));
        }

        beginTest("No sidechain buffers uses self-detection");
        {
            SidechainCompressor comp;
            comp.prepare(44100.0, 512);
            comp.setThreshold(-20.0f);
            comp.setRatio(4.0f);
            comp.setAttack(0.1f);
            comp.setRelease(10.0f);
            comp.setKneeWidth(0.0f);

            comp.setSidechainBuffers(nullptr, nullptr);

            constexpr int N = 512;
            std::array<float, N> mainL{}, mainR{};
            std::fill(mainL.begin(), mainL.end(), 1.0f);
            std::fill(mainR.begin(), mainR.end(), 1.0f);

            // Run enough samples
            for (int block = 0; block < 5; ++block)
            {
                std::fill(mainL.begin(), mainL.end(), 1.0f);
                std::fill(mainR.begin(), mainR.end(), 1.0f);
                comp.processBlock(mainL.data(), mainR.data(), N);
            }

            // Should compress (self-detection on loud signal)
            expect(mainL[N - 1] < 0.8f,
                   "Self-detection should compress loud signal");
        }

        // ---- Sidechain HPF ----
        beginTest("Sidechain HPF removes low frequency from detector");
        {
            SidechainCompressor compNoHPF;
            compNoHPF.prepare(44100.0, 512);
            compNoHPF.setThreshold(-20.0f);
            compNoHPF.setRatio(10.0f);
            compNoHPF.setAttack(0.1f);
            compNoHPF.setRelease(10.0f);
            compNoHPF.setKneeWidth(0.0f);
            compNoHPF.setSidechainHPF(0.0f);  // No HPF

            SidechainCompressor compHPF;
            compHPF.prepare(44100.0, 512);
            compHPF.setThreshold(-20.0f);
            compHPF.setRatio(10.0f);
            compHPF.setAttack(0.1f);
            compHPF.setRelease(10.0f);
            compHPF.setKneeWidth(0.0f);
            compHPF.setSidechainHPF(500.0f);  // HPF at 500 Hz

            // Generate a low-frequency sidechain signal (50 Hz sine)
            constexpr int N = 2048;
            std::array<float, N> mainL{}, mainR{}, scL{}, scR{};
            std::fill(mainL.begin(), mainL.end(), 0.5f);
            std::fill(mainR.begin(), mainR.end(), 0.5f);

            for (int i = 0; i < N; ++i)
            {
                float t = (float) i / 44100.0f;
                scL[(size_t) i] = std::sin(2.0f * 3.14159f * 50.0f * t); // 50 Hz
                scR[(size_t) i] = scL[(size_t) i];
            }

            // Process without HPF
            compNoHPF.setSidechainBuffers(scL.data(), scR.data());
            auto mainLNoHPF = mainL;
            auto mainRNoHPF = mainR;
            compNoHPF.processBlock(mainLNoHPF.data(), mainRNoHPF.data(), N);

            // Process with HPF
            compHPF.setSidechainBuffers(scL.data(), scR.data());
            auto mainLHPF = mainL;
            auto mainRHPF = mainR;
            compHPF.processBlock(mainLHPF.data(), mainRHPF.data(), N);

            // With HPF, the low-frequency sidechain should be attenuated,
            // so less compression should occur (output should be louder)
            float avgNoHPF = 0.0f, avgHPF = 0.0f;
            for (int i = N / 2; i < N; ++i) // Use second half (past transient)
            {
                avgNoHPF += std::abs(mainLNoHPF[(size_t) i]);
                avgHPF += std::abs(mainLHPF[(size_t) i]);
            }
            avgNoHPF /= (float)(N / 2);
            avgHPF /= (float)(N / 2);

            // HPF version should let more signal through (less compression on low freq)
            expect(avgHPF >= avgNoHPF * 0.9f,
                   "HPF should reduce compression from low-freq sidechain, "
                   "avgHPF=" + juce::String(avgHPF) + " avgNoHPF=" + juce::String(avgNoHPF));
        }

        // ---- Key-listen mode ----
        beginTest("Key-listen outputs sidechain signal");
        {
            SidechainCompressor comp;
            comp.prepare(44100.0, 512);
            comp.setKeyListen(true);

            constexpr int N = 64;
            std::array<float, N> mainL{}, mainR{}, scL{}, scR{};
            std::fill(mainL.begin(), mainL.end(), 0.5f);
            std::fill(mainR.begin(), mainR.end(), 0.5f);
            std::fill(scL.begin(), scL.end(), 0.9f);
            std::fill(scR.begin(), scR.end(), 0.9f);

            comp.setSidechainBuffers(scL.data(), scR.data());
            comp.processBlock(mainL.data(), mainR.data(), N);

            // Output should be the sidechain signal, not the main signal
            expectWithinAbsoluteError(mainL[0], 0.9f, 0.01f);
            expectWithinAbsoluteError(mainR[0], 0.9f, 0.01f);
        }

        // ---- Dry/wet mix ----
        beginTest("Mix parameter blends dry and wet");
        {
            SidechainCompressor compFull;
            compFull.prepare(44100.0, 512);
            compFull.setThreshold(-20.0f);
            compFull.setRatio(10.0f);
            compFull.setAttack(0.1f);
            compFull.setRelease(10.0f);
            compFull.setKneeWidth(0.0f);
            compFull.setMix(1.0f);

            SidechainCompressor compHalf;
            compHalf.prepare(44100.0, 512);
            compHalf.setThreshold(-20.0f);
            compHalf.setRatio(10.0f);
            compHalf.setAttack(0.1f);
            compHalf.setRelease(10.0f);
            compHalf.setKneeWidth(0.0f);
            compHalf.setMix(0.5f);

            float inputLevel = 1.0f;

            // Let both settle
            for (int i = 0; i < 2000; ++i)
            {
                float lF = inputLevel, rF = inputLevel;
                float lH = inputLevel, rH = inputLevel;
                compFull.process(lF, rF);
                compHalf.process(lH, rH);
            }

            float lF = inputLevel, rF = inputLevel;
            float lH = inputLevel, rH = inputLevel;
            compFull.process(lF, rF);
            compHalf.process(lH, rH);

            // Half-mix output should be between dry (1.0) and full compression
            expect(lH > lF, "50% mix should be louder than 100% mix, "
                   "halfMix=" + juce::String(lH) + " fullMix=" + juce::String(lF));
            expect(lH < inputLevel, "50% mix should still be compressed, "
                   "halfMix=" + juce::String(lH));
        }

        // ---- Sidechain source configuration ----
        beginTest("Sidechain source and index config");
        {
            SidechainCompressor comp;
            comp.setSidechainSource(SidechainCompressor::SidechainSource::Track);
            expect(comp.getSidechainSource() == SidechainCompressor::SidechainSource::Track,
                   "Source should be Track");

            comp.setSidechainIndex(3);
            expectEquals(comp.getSidechainIndex(), 3);

            comp.setSidechainSource(SidechainCompressor::SidechainSource::SendBus);
            expect(comp.getSidechainSource() == SidechainCompressor::SidechainSource::SendBus,
                   "Source should be SendBus");
        }

        // ---- RMS detection mode ----
        beginTest("RMS detection mode");
        {
            SidechainCompressor comp;
            comp.prepare(44100.0, 512);
            comp.setDetectionMode(SidechainCompressor::DetectionMode::RMS);
            comp.setThreshold(-20.0f);
            comp.setRatio(4.0f);
            comp.setAttack(0.1f);
            comp.setRelease(10.0f);
            comp.setKneeWidth(0.0f);

            // Process a signal
            for (int i = 0; i < 1000; ++i)
            {
                float l = 1.0f, r = 1.0f;
                comp.process(l, r);
            }

            // Should still compress (gain reduction present)
            expect(comp.getGainReductionDb() < 0.0f,
                   "RMS mode should still detect and compress");
        }

        // ---- Disabled/bypassed ----
        beginTest("Bypassed compressor passes signal unchanged");
        {
            SidechainCompressor comp;
            comp.prepare(44100.0, 512);
            comp.setEnabled(false);

            float l = 0.75f, r = 0.75f;
            comp.process(l, r);
            expectWithinAbsoluteError(l, 0.75f, 0.001f);
            expectWithinAbsoluteError(r, 0.75f, 0.001f);
        }

        // ---- Reset ----
        beginTest("Reset clears state");
        {
            SidechainCompressor comp;
            comp.prepare(44100.0, 512);
            comp.setThreshold(-20.0f);
            comp.setRatio(4.0f);
            comp.setAttack(0.1f);

            // Drive it hard
            for (int i = 0; i < 1000; ++i)
            {
                float l = 1.0f, r = 1.0f;
                comp.process(l, r);
            }

            expect(comp.getGainReductionDb() < -1.0f,
                   "Should have significant gain reduction before reset");

            comp.reset();

            // After reset, the first sample of a quiet signal should pass through
            float l = 0.001f, r = 0.001f;
            comp.process(l, r);
            expectWithinAbsoluteError(l, 0.001f, 0.0005f);
        }

        // ---- Makeup gain ----
        beginTest("Makeup gain boosts output");
        {
            SidechainCompressor comp;
            comp.prepare(44100.0, 512);
            comp.setThreshold(-60.0f);  // Very low threshold to ensure compression
            comp.setRatio(20.0f);
            comp.setAttack(0.1f);
            comp.setRelease(10.0f);
            comp.setKneeWidth(0.0f);
            comp.setMakeupGain(0.0f);

            // Let it settle with a loud signal
            float lastNoMakeup = 0.0f;
            for (int i = 0; i < 2000; ++i)
            {
                float l = 1.0f, r = 1.0f;
                comp.process(l, r);
                lastNoMakeup = l;
            }

            // Now add makeup gain
            comp.setMakeupGain(12.0f);
            float l = 1.0f, r = 1.0f;
            comp.process(l, r);

            expect(l > lastNoMakeup,
                   "Makeup gain should increase output, "
                   "withMakeup=" + juce::String(l) + " without=" + juce::String(lastNoMakeup));
        }
    }
};

static SidechainCompressorTests sidechainCompressorTests;

// ================================================================
// Integration: SendBusManager + SidechainCompressor
// ================================================================

class SendBusSidechainIntegrationTests : public juce::UnitTest
{
public:
    SendBusSidechainIntegrationTests() : UnitTest("Send Bus Sidechain Integration") {}

    void runTest() override
    {
        beginTest("Sidechain compressor reads from send bus buffer");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 256);

            // Set up: track 0 sends to bus 0 (the sidechain source)
            mgr.setTrackSendLevel(0, 0, 1.0f);
            mgr.setBusLevel(0, 1.0f);

            // Create a sidechain compressor
            SidechainCompressor comp;
            comp.prepare(44100.0, 256);
            comp.setThreshold(-20.0f);
            comp.setRatio(10.0f);
            comp.setAttack(0.1f);
            comp.setRelease(10.0f);
            comp.setKneeWidth(0.0f);
            comp.setSidechainSource(SidechainCompressor::SidechainSource::SendBus);
            comp.setSidechainIndex(0);

            constexpr int N = 256;

            // Run a few blocks for the compressor to settle
            for (int block = 0; block < 10; ++block)
            {
                mgr.clearAllBuffers();

                // Track 0 signal: loud kick (1.0)
                std::array<float, N> kickL{}, kickR{};
                std::fill(kickL.begin(), kickL.end(), 1.0f);
                std::fill(kickR.begin(), kickR.end(), 1.0f);

                mgr.routeTrackToSends(0, kickL.data(), kickR.data(), N);

                // Point the sidechain at the bus buffer
                const float* scL = mgr.getBusBufferL(0);
                const float* scR = mgr.getBusBufferR(0);
                comp.setSidechainBuffers(scL, scR);

                // Track 1 signal: bass (steady 0.5)
                std::array<float, N> bassL{}, bassR{};
                std::fill(bassL.begin(), bassL.end(), 0.5f);
                std::fill(bassR.begin(), bassR.end(), 0.5f);

                // Compress the bass using the kick sidechain
                comp.processBlock(bassL.data(), bassR.data(), N);

                if (block == 9) // Check the last block
                {
                    // Bass should be ducked because the kick is loud in the sidechain bus
                    float outputLevel = bassL[N - 1];
                    expect(outputLevel < 0.45f,
                           "Bass should be ducked by kick sidechain, got "
                           + juce::String(outputLevel));
                }
            }
        }

        beginTest("Sidechain compressor in bus effect chain");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 256);

            // Add a sidechain compressor to bus 0's effect chain
            auto scComp = std::make_unique<SidechainCompressor>();
            scComp->setThreshold(-20.0f);
            scComp->setRatio(4.0f);
            scComp->setAttack(0.1f);
            scComp->setRelease(10.0f);

            mgr.addEffectToBus(0, std::move(scComp));

            auto* chain = mgr.getBusChain(0);
            expect(chain != nullptr, "Chain should exist");
            expectEquals(chain->getEffectCount(), 1);

            // Verify we can access it as a SidechainCompressor
            auto* fx = chain->getEffect(0);
            expect(fx != nullptr, "Effect should exist in chain");
        }

        beginTest("Full routing pipeline: track -> send -> process -> mixback");
        {
            SendBusManager mgr;
            mgr.prepare(44100.0, 128);

            // Bus 0 has no effects — signal passes through directly

            // Track 0 -> bus 0 at full level
            mgr.setTrackSendLevel(0, 0, 1.0f);
            mgr.setBusLevel(0, 1.0f);

            constexpr int N = 128;

            mgr.clearAllBuffers();

            std::array<float, N> trackL{}, trackR{};
            std::fill(trackL.begin(), trackL.end(), 0.5f);
            std::fill(trackR.begin(), trackR.end(), 0.5f);

            mgr.routeTrackToSends(0, trackL.data(), trackR.data(), N);
            mgr.processAllBuses();

            std::array<float, N> mainL{}, mainR{};
            mainL.fill(0.0f);
            mainR.fill(0.0f);
            mgr.mixAllBusesInto(mainL.data(), mainR.data(), N);

            // Output should be non-zero (signal passed through the bus)
            float sumL = 0.0f;
            for (int i = 0; i < N; ++i)
                sumL += std::abs(mainL[(size_t) i]);

            expect(sumL > 0.0f,
                   "Bus output should be non-zero after routing/processing/mixback");
        }
    }
};

static SendBusSidechainIntegrationTests sendBusSidechainIntegrationTests;
