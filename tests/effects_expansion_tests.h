#pragma once

#include "../src/dsp/effects/HallReverb.h"
#include "../src/dsp/effects/SpringReverb.h"
#include "../src/dsp/effects/ShimmerReverb.h"
#include "../src/dsp/effects/MultiTapDelay.h"
#include "../src/dsp/effects/ReverseDelay.h"
#include "../src/dsp/effects/Ensemble.h"
#include "../src/dsp/effects/RotarySpeaker.h"
#include "../src/dsp/effects/AutoPan.h"
#include "../src/dsp/effects/Waveshaper.h"
#include "../src/dsp/effects/RateReducer.h"
#include "../src/dsp/effects/TransientShaper.h"

// -------------------------------------------------------------------
// Helper: fill buffer with deterministic noise using xorshift
// -------------------------------------------------------------------
static void fillNoise(float* buf, int n, uint32_t seed)
{
    uint32_t state = seed;
    for (int i = 0; i < n; ++i)
        buf[i] = dsp::xorshiftNoise(state);
}

static bool buffersEqual(const float* a, const float* b, int n)
{
    for (int i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

static bool bufferIsFinite(const float* buf, int n)
{
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(buf[i])) return false;
    return true;
}

// -------------------------------------------------------------------
// Effects Expansion Tests
// -------------------------------------------------------------------
class EffectsExpansionTests : public juce::UnitTest
{
public:
    EffectsExpansionTests() : juce::UnitTest("Effects Expansion") {}

    void runTest() override
    {
        testHallReverb();
        testSpringReverb();
        testShimmerReverb();
        testMultiTapDelay();
        testReverseDelay();
        testEnsemble();
        testRotarySpeaker();
        testAutoPan();
        testWaveshaper();
        testRateReducer();
        testTransientShaper();
    }

private:
    static constexpr int BLOCK_SIZE = 512;
    static constexpr double SAMPLE_RATE = 44100.0;

    // ---------------------------------------------------------------
    // Generic test pattern used for every effect
    // ---------------------------------------------------------------
    template <typename EffectT>
    void runStandardTests(EffectT& fx, const juce::String& name)
    {
        // --- Silence test: no crash ---
        beginTest(name + " silence processing");
        fx.prepare(SAMPLE_RATE, BLOCK_SIZE);

        float silL[BLOCK_SIZE] = {};
        float silR[BLOCK_SIZE] = {};
        fx.processStereo(silL, silR, BLOCK_SIZE);
        expect(bufferIsFinite(silL, BLOCK_SIZE), name + " left output is finite after silence");
        expect(bufferIsFinite(silR, BLOCK_SIZE), name + " right output is finite after silence");

        // --- Noise test: output differs from input when enabled ---
        beginTest(name + " noise processing");
        fx.prepare(SAMPLE_RATE, BLOCK_SIZE);
        fx.setEnabled(true);

        float noiseL[BLOCK_SIZE], noiseR[BLOCK_SIZE];
        float origL[BLOCK_SIZE], origR[BLOCK_SIZE];
        fillNoise(noiseL, BLOCK_SIZE, 12345);
        fillNoise(noiseR, BLOCK_SIZE, 67890);
        std::copy(noiseL, noiseL + BLOCK_SIZE, origL);
        std::copy(noiseR, noiseR + BLOCK_SIZE, origR);

        fx.processStereo(noiseL, noiseR, BLOCK_SIZE);
        expect(bufferIsFinite(noiseL, BLOCK_SIZE), name + " left output is finite after noise");
        expect(bufferIsFinite(noiseR, BLOCK_SIZE), name + " right output is finite after noise");

        bool changed = !buffersEqual(noiseL, origL, BLOCK_SIZE) ||
                       !buffersEqual(noiseR, origR, BLOCK_SIZE);
        expect(changed, name + " output differs from input when enabled");

        // --- Reset test: no crash ---
        beginTest(name + " reset");
        fx.reset();
        // Process again after reset
        float postResetL[BLOCK_SIZE] = {};
        float postResetR[BLOCK_SIZE] = {};
        fx.processStereo(postResetL, postResetR, BLOCK_SIZE);
        expect(bufferIsFinite(postResetL, BLOCK_SIZE), name + " output finite after reset");
    }

    // ---------------------------------------------------------------
    // Individual effect tests
    // ---------------------------------------------------------------

    void testHallReverb()
    {
        HallReverb fx;
        runStandardTests(fx, "HallReverb");

        beginTest("HallReverb parameter round-trip");
        fx.setParameter(HallReverb::DecayTime, 5.0f);
        expectWithinAbsoluteError(fx.getParameter(HallReverb::DecayTime), 5.0f, 0.001f);
        fx.setParameter(HallReverb::Size, 1.5f);
        expectWithinAbsoluteError(fx.getParameter(HallReverb::Size), 1.5f, 0.001f);
        fx.setParameter(HallReverb::Damping, 0.7f);
        expectWithinAbsoluteError(fx.getParameter(HallReverb::Damping), 0.7f, 0.001f);
        fx.setParameter(HallReverb::PreDelay, 50.0f);
        expectWithinAbsoluteError(fx.getParameter(HallReverb::PreDelay), 50.0f, 0.001f);
        fx.setParameter(HallReverb::Modulation, 0.8f);
        expectWithinAbsoluteError(fx.getParameter(HallReverb::Modulation), 0.8f, 0.001f);
        fx.setParameter(HallReverb::Mix, 0.6f);
        expectWithinAbsoluteError(fx.getParameter(HallReverb::Mix), 0.6f, 0.001f);
    }

    void testSpringReverb()
    {
        SpringReverb fx;
        runStandardTests(fx, "SpringReverb");

        beginTest("SpringReverb parameter round-trip");
        fx.setParameter(SpringReverb::Decay, 3.0f);
        expectWithinAbsoluteError(fx.getParameter(SpringReverb::Decay), 3.0f, 0.001f);
        fx.setParameter(SpringReverb::Dispersion, 0.8f);
        expectWithinAbsoluteError(fx.getParameter(SpringReverb::Dispersion), 0.8f, 0.001f);
        fx.setParameter(SpringReverb::Tension, 0.6f);
        expectWithinAbsoluteError(fx.getParameter(SpringReverb::Tension), 0.6f, 0.001f);
        fx.setParameter(SpringReverb::Damping, 0.4f);
        expectWithinAbsoluteError(fx.getParameter(SpringReverb::Damping), 0.4f, 0.001f);
        fx.setParameter(SpringReverb::Mix, 0.5f);
        expectWithinAbsoluteError(fx.getParameter(SpringReverb::Mix), 0.5f, 0.001f);
    }

    void testShimmerReverb()
    {
        ShimmerReverb fx;
        runStandardTests(fx, "ShimmerReverb");

        beginTest("ShimmerReverb parameter round-trip");
        fx.setParameter(ShimmerReverb::Decay, 4.0f);
        expectWithinAbsoluteError(fx.getParameter(ShimmerReverb::Decay), 4.0f, 0.001f);
        fx.setParameter(ShimmerReverb::Shimmer, 0.7f);
        expectWithinAbsoluteError(fx.getParameter(ShimmerReverb::Shimmer), 0.7f, 0.001f);
        fx.setParameter(ShimmerReverb::ShiftMode, 1.0f);
        expectWithinAbsoluteError(fx.getParameter(ShimmerReverb::ShiftMode), 1.0f, 0.001f);
        fx.setParameter(ShimmerReverb::Damping, 0.5f);
        expectWithinAbsoluteError(fx.getParameter(ShimmerReverb::Damping), 0.5f, 0.001f);
        fx.setParameter(ShimmerReverb::Size, 1.2f);
        expectWithinAbsoluteError(fx.getParameter(ShimmerReverb::Size), 1.2f, 0.001f);
        fx.setParameter(ShimmerReverb::Mix, 0.3f);
        expectWithinAbsoluteError(fx.getParameter(ShimmerReverb::Mix), 0.3f, 0.001f);
    }

    void testMultiTapDelay()
    {
        MultiTapDelay fx;
        runStandardTests(fx, "MultiTapDelay");

        beginTest("MultiTapDelay parameter round-trip");
        // Test tap parameters
        fx.setParameter(MultiTapDelay::tapTime(0), 0.5f);
        expectWithinAbsoluteError(fx.getParameter(MultiTapDelay::tapTime(0)), 0.5f, 0.001f);
        fx.setParameter(MultiTapDelay::tapLevel(0), 0.8f);
        expectWithinAbsoluteError(fx.getParameter(MultiTapDelay::tapLevel(0)), 0.8f, 0.001f);
        fx.setParameter(MultiTapDelay::tapPan(0), 0.3f);
        expectWithinAbsoluteError(fx.getParameter(MultiTapDelay::tapPan(0)), 0.3f, 0.001f);
        fx.setParameter(MultiTapDelay::tapFeedback(0), 0.6f);
        expectWithinAbsoluteError(fx.getParameter(MultiTapDelay::tapFeedback(0)), 0.6f, 0.001f);
        // Test global parameters
        fx.setParameter(MultiTapDelay::NumTaps, 6.0f);
        expectWithinAbsoluteError(fx.getParameter(MultiTapDelay::NumTaps), 6.0f, 0.001f);
        fx.setParameter(MultiTapDelay::MixParam, 0.4f);
        expectWithinAbsoluteError(fx.getParameter(MultiTapDelay::MixParam), 0.4f, 0.001f);
    }

    void testReverseDelay()
    {
        ReverseDelay fx;
        runStandardTests(fx, "ReverseDelay");

        beginTest("ReverseDelay parameter round-trip");
        fx.setParameter(ReverseDelay::Time, 1.0f);
        expectWithinAbsoluteError(fx.getParameter(ReverseDelay::Time), 1.0f, 0.001f);
        fx.setParameter(ReverseDelay::Feedback, 0.5f);
        expectWithinAbsoluteError(fx.getParameter(ReverseDelay::Feedback), 0.5f, 0.001f);
        fx.setParameter(ReverseDelay::HighCut, 6000.0f);
        expectWithinAbsoluteError(fx.getParameter(ReverseDelay::HighCut), 6000.0f, 0.1f);
        fx.setParameter(ReverseDelay::Mix, 0.7f);
        expectWithinAbsoluteError(fx.getParameter(ReverseDelay::Mix), 0.7f, 0.001f);
    }

    void testEnsemble()
    {
        Ensemble fx;
        runStandardTests(fx, "Ensemble");

        beginTest("Ensemble parameter round-trip");
        fx.setParameter(Ensemble::Rate, 1.5f);
        expectWithinAbsoluteError(fx.getParameter(Ensemble::Rate), 1.5f, 0.001f);
        fx.setParameter(Ensemble::Depth, 0.8f);
        expectWithinAbsoluteError(fx.getParameter(Ensemble::Depth), 0.8f, 0.001f);
        fx.setParameter(Ensemble::Width, 0.9f);
        expectWithinAbsoluteError(fx.getParameter(Ensemble::Width), 0.9f, 0.001f);
        fx.setParameter(Ensemble::Mix, 0.6f);
        expectWithinAbsoluteError(fx.getParameter(Ensemble::Mix), 0.6f, 0.001f);
    }

    void testRotarySpeaker()
    {
        RotarySpeaker fx;
        runStandardTests(fx, "RotarySpeaker");

        beginTest("RotarySpeaker parameter round-trip");
        fx.setParameter(RotarySpeaker::Speed, 1.0f);
        expectWithinAbsoluteError(fx.getParameter(RotarySpeaker::Speed), 1.0f, 0.001f);
        fx.setParameter(RotarySpeaker::HornRate, 8.0f);
        expectWithinAbsoluteError(fx.getParameter(RotarySpeaker::HornRate), 8.0f, 0.001f);
        fx.setParameter(RotarySpeaker::DrumRate, 1.5f);
        expectWithinAbsoluteError(fx.getParameter(RotarySpeaker::DrumRate), 1.5f, 0.001f);
        fx.setParameter(RotarySpeaker::Acceleration, 0.7f);
        expectWithinAbsoluteError(fx.getParameter(RotarySpeaker::Acceleration), 0.7f, 0.001f);
        fx.setParameter(RotarySpeaker::CrossoverFreq, 1000.0f);
        expectWithinAbsoluteError(fx.getParameter(RotarySpeaker::CrossoverFreq), 1000.0f, 0.1f);
        fx.setParameter(RotarySpeaker::Mix, 0.5f);
        expectWithinAbsoluteError(fx.getParameter(RotarySpeaker::Mix), 0.5f, 0.001f);
    }

    void testAutoPan()
    {
        AutoPan fx;
        runStandardTests(fx, "AutoPan");

        beginTest("AutoPan parameter round-trip");
        fx.setParameter(AutoPan::Rate, 5.0f);
        expectWithinAbsoluteError(fx.getParameter(AutoPan::Rate), 5.0f, 0.001f);
        fx.setParameter(AutoPan::Depth, 0.9f);
        expectWithinAbsoluteError(fx.getParameter(AutoPan::Depth), 0.9f, 0.001f);
        fx.setParameter(AutoPan::Shape, 1.0f);
        expectWithinAbsoluteError(fx.getParameter(AutoPan::Shape), 1.0f, 0.001f);
        fx.setParameter(AutoPan::Phase, 0.25f);
        expectWithinAbsoluteError(fx.getParameter(AutoPan::Phase), 0.25f, 0.001f);
        fx.setParameter(AutoPan::Mix, 0.8f);
        expectWithinAbsoluteError(fx.getParameter(AutoPan::Mix), 0.8f, 0.001f);
    }

    void testWaveshaper()
    {
        Waveshaper fx;
        runStandardTests(fx, "Waveshaper");

        beginTest("Waveshaper parameter round-trip");
        fx.setParameter(Waveshaper::Drive, 0.7f);
        expectWithinAbsoluteError(fx.getParameter(Waveshaper::Drive), 0.7f, 0.001f);
        fx.setParameter(Waveshaper::Curve, 2.0f);
        expectWithinAbsoluteError(fx.getParameter(Waveshaper::Curve), 2.0f, 0.001f);
        fx.setParameter(Waveshaper::Tone, 0.4f);
        expectWithinAbsoluteError(fx.getParameter(Waveshaper::Tone), 0.4f, 0.001f);
        fx.setParameter(Waveshaper::Asymmetry, 0.6f);
        expectWithinAbsoluteError(fx.getParameter(Waveshaper::Asymmetry), 0.6f, 0.001f);
        fx.setParameter(Waveshaper::Mix, 0.5f);
        expectWithinAbsoluteError(fx.getParameter(Waveshaper::Mix), 0.5f, 0.001f);
    }

    void testRateReducer()
    {
        RateReducer fx;
        runStandardTests(fx, "RateReducer");

        beginTest("RateReducer parameter round-trip");
        fx.setParameter(RateReducer::RateReduction, 0.5f);
        expectWithinAbsoluteError(fx.getParameter(RateReducer::RateReduction), 0.5f, 0.001f);
        fx.setParameter(RateReducer::BitDepth, 0.3f);
        expectWithinAbsoluteError(fx.getParameter(RateReducer::BitDepth), 0.3f, 0.001f);
        fx.setParameter(RateReducer::PreFilter, 0.8f);
        expectWithinAbsoluteError(fx.getParameter(RateReducer::PreFilter), 0.8f, 0.001f);
        fx.setParameter(RateReducer::Mix, 0.6f);
        expectWithinAbsoluteError(fx.getParameter(RateReducer::Mix), 0.6f, 0.001f);
    }

    void testTransientShaper()
    {
        TransientShaper fx;
        runStandardTests(fx, "TransientShaper");

        beginTest("TransientShaper parameter round-trip");
        fx.setParameter(TransientShaper::Attack, 0.8f);
        expectWithinAbsoluteError(fx.getParameter(TransientShaper::Attack), 0.8f, 0.001f);
        fx.setParameter(TransientShaper::Sustain, -0.5f);
        expectWithinAbsoluteError(fx.getParameter(TransientShaper::Sustain), -0.5f, 0.001f);
        fx.setParameter(TransientShaper::Speed, 0.7f);
        expectWithinAbsoluteError(fx.getParameter(TransientShaper::Speed), 0.7f, 0.001f);
        fx.setParameter(TransientShaper::Output, 3.0f);
        expectWithinAbsoluteError(fx.getParameter(TransientShaper::Output), 3.0f, 0.001f);
        fx.setParameter(TransientShaper::Mix, 0.4f);
        expectWithinAbsoluteError(fx.getParameter(TransientShaper::Mix), 0.4f, 0.001f);
    }
};

static EffectsExpansionTests effectsExpansionTests;
