#pragma once
// What the FM engine costs, before and after its operators moved onto the shared oscillator
// (dsp/PhaseOsc.h): NEPTUNE_PHASEOSC_BENCH=1 NEPTUNE_TEST_ONLY="Phase Osc" <test binary>.
// Eight voices held, 512-sample blocks, 48 kHz; the engine alone (no mixer, no effects); the best of
// five runs of ten seconds of sound each, as nanoseconds per sample and as a share of real time
// on one core. Only the engine's public interface is used (ids by number), so the same file
// times a tree from before the port: there the rows that need a WAVE cell are left out.
// Numbers that mean anything come from a Release build.
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include "../src/engine/AudioEngine.h"

namespace phaseOscBench
{
    inline double timeEngine(const std::function<void (EngineBase&)>& setUp)
    {
        const double sr = 48000.0;
        const int block = 512, blocks = (int) (10.0 * sr / block);
        double best = 1.0e30;
        float keep = 0.0f;
        for (int run = 0; run < 5; ++run)
        {
            std::shared_ptr<EngineBase> e = AudioEngine::createEngine(EngineType::FM);
            e->prepareToPlay(sr, block);
            setUp(*e);
            static const int notes[8] = { 36, 43, 48, 52, 55, 59, 62, 67 };
            for (int n : notes)
                e->noteOn(n, 0.8f);
            juce::AudioBuffer<float> buf(2, block);
            juce::MidiBuffer midi;
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                e->processBlock(buf, midi);
                keep += buf.getSample(0, block / 2);
            }
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        if (! std::isfinite(keep))
            std::printf("  (output not finite)\n");
        return best;
    }

    // NEPTUNE_PHASEOSC_BENCH_ROW=<part of a row's name>: only the rows that have it (for a profiler).
    inline void row(const char* name, const std::function<void (EngineBase&)>& setUp)
    {
        const juce::String only = juce::SystemStats::getEnvironmentVariable("NEPTUNE_PHASEOSC_BENCH_ROW", {});
        if (only.isNotEmpty() && ! juce::String(name).contains(only))
            return;
        const double seconds = timeEngine(setUp);
        std::printf("  %-58s %7.1f ns/sample  %7.1f us per 512-sample block  %6.2f %% of one core\n", name,
                    seconds * 1.0e9 / (10.0 * 48000.0), seconds * 1.0e6 * 512.0 / (10.0 * 48000.0), seconds * 10.0);
        std::fflush(stdout);
    }

    // Every operator in the sound: the chain 6 > 5 > 4 > 3 > 2 > 1, each at 0.8, feedback on 6,
    // envelopes that hold (so all ten seconds are the same work).
    inline void sixInAChain(EngineBase& e)
    {
        for (int op = 0; op < 6; ++op)
        {
            e.setParameter(op * 10 + 1, 0.8f);
            e.setParameter(op * 10 + 0, (float) (op + 1));
            e.setParameter(op * 10 + 6, 1.0f);
        }
        e.setParameter(53, 0.4f);
    }

    inline void run()
    {
        std::shared_ptr<EngineBase> probe = AudioEngine::createEngine(EngineType::FM);
        const bool waves = probe->getParameterName(8) == "Op1 Wave";
        std::printf("FM engine, 8 notes held, 512-sample blocks, 48 kHz (%s tree)\n", waves ? "PhaseOsc" : "before PhaseOsc");
        row("default sound (2 > 1, four operators silent), sines", [](EngineBase&) {});
        row("six operators in a chain, feedback, sines", sixInAChain);
        row("six carriers (ADD), sines", [](EngineBase& e) { sixInAChain(e); e.setParameter(60, 5.0f); });
        if (! waves)
            return;
        // What setting SMPL costs (message thread, once per sample and run): cutting the cycles
        // of the bank's default sample and of its longest.
        {
            auto& bank = SampleBank::get();
            int longest = bank.defaultIndex();
            for (int i = 0; i < bank.size(); ++i)
                if (bank.entry(i)->left.size() > bank.entry(longest)->left.size())
                    longest = i;
            for (int index : { bank.defaultIndex(), longest })
            {
                const auto* e = bank.entry(index);
                const auto t0 = std::chrono::steady_clock::now();
                const Wavetable t = phaseosc::cyclesFromSample(e->left.data(), nullptr, (int) e->left.size(), e->sampleRate);
                std::printf("  the cycles of %-5s (%6.2f s of sound, %d frames)            %7.2f ms, once\n", e->name.c_str(),
                            (double) e->left.size() / e->sampleRate, t.numFrames,
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1.0e3);
            }
        }
        // (WAVE: 0 SINE, 1 CYCL, 2 SAW ... ; 2 + 9 = VOWL)
        row("six carriers (ADD), all SAW tables, nothing modulated", [](EngineBase& e)
        {
            sixInAChain(e);
            e.setParameter(60, 5.0f);
            e.setParameter(53, 0.0f);
            for (int op = 0; op < 6; ++op) { e.setParameter(op * 10 + 8, 2.0f); e.setParameter(op * 10 + 9, 0.7f); }
        });
        row("six in a chain, all SAW tables (each modulated)", [](EngineBase& e)
        {
            sixInAChain(e);
            for (int op = 0; op < 6; ++op) { e.setParameter(op * 10 + 8, 2.0f); e.setParameter(op * 10 + 9, 0.7f); }
        });
        row("the same as a 4-wave sequence of VOWL, PENV, an LFO on POS1", [](EngineBase& e)
        {
            sixInAChain(e);
            for (int op = 0; op < 6; ++op) { e.setParameter(op * 10 + 8, 11.0f); e.setParameter(op * 10 + 9, 0.45f); }
            e.setParameter(67, 3.0f);
            e.setParameter(68, 0.5f);
            e.setParameter(69, 0.5f);
            for (auto& page : e.getParamPages())
                if (const auto* dest = page.find(165))
                    for (int i = 0; i < dest->numChoices; ++i)
                        if (juce::String(dest->choices[i]) == "POS1")
                        {
                            e.setParameter(158, 5.0f);
                            e.setParameter(159, 0.5f);
                            e.setParameter(165, (float) i);
                        }
        });
        row("default sound, one modulator a table (WAV2 = SAW)", [](EngineBase& e) { e.setParameter(18, 2.0f); e.setParameter(19, 0.7f); });
    }
}
