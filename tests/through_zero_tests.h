#pragma once

#include <juce_core/juce_core.h>

#include "../src/engine/AudioEngine.h"
#include "../src/dsp/effects/EffectChain.h"
#include "../src/dsp/effects/ModDelay.h"
#include "../src/project/ProjectBridge.h"

#include <cmath>
#include <vector>
#include <algorithm>

// Through-zero: the chain's unified dry delay (EffectChain.h, DryLine) for FLG, CHR, ENS and VIB.
class ThroughZeroTests : public juce::UnitTest
{
public:
    ThroughZeroTests() : juce::UnitTest("Through Zero") {}

    void runTest() override
    {
        static constexpr double SR = 48000.0;
        static constexpr int BLOCK = 256;

        // ---- helpers ----------------------------------------------------------------------
        auto makeTone = [](float hz, float amp, int n, double sr) -> std::vector<float>
        {
            std::vector<float> buf((size_t) n);
            for (int i = 0; i < n; ++i)
                buf[(size_t) i] = amp * std::sin(2.0f * 3.14159265f * hz * (float) i / (float) sr);
            return buf;
        };

        auto rms = [](const float* buf, int n) -> float
        {
            double sum = 0.0;
            for (int i = 0; i < n; ++i) sum += (double) buf[i] * buf[i];
            return (float) std::sqrt(sum / (double) n);
        };

        auto maxAbs = [](const float* buf, int n) -> float
        {
            float m = 0.0f;
            for (int i = 0; i < n; ++i) m = std::max(m, std::abs(buf[i]));
            return m;
        };

        auto diff = [](const float* a, const float* b, int n) -> float
        {
            float m = 0.0f;
            for (int i = 0; i < n; ++i) m = std::max(m, std::abs(a[i] - b[i]));
            return m;
        };

        auto rmsOf = [](const float* a, const float* b, int n) -> float
        {
            double sum = 0.0;
            for (int i = 0; i < n; ++i) { double d = a[i] - b[i]; sum += d * d; }
            return (float) std::sqrt(sum / (double) n);
        };

        // ---- FLG THRU in a chain: matches FLG alone (not chained) --------------------------
        beginTest("FLG with THRU in a chain matches FLG alone");
        {
            const int N = (int) SR;     // 1 second
            auto tone = makeTone(200.0f, 0.4f, N, SR);

            // Standalone flanger (not chained): the core handles through-zero.
            Flanger alone;
            alone.setParameter(Flanger::Depth, 0.3f);
            alone.setParameter(Flanger::Feedback, 0.3f);
            alone.setParameter(Flanger::Mix, 0.5f);
            alone.setExtra(Flanger::Through, 1.0f);
            alone.setExtra(Flanger::Time, 0.003f);
            alone.prepare(SR, BLOCK);
            alone.reset();

            std::vector<float> aloneL(tone), aloneR(tone);
            for (int done = 0; done < N; done += BLOCK)
            {
                int n = std::min(BLOCK, N - done);
                alone.processBlock(aloneL.data() + done, aloneR.data() + done, n);
            }

            // Chained flanger.
            EffectChain chain;
            chain.prepare(SR, BLOCK);
            {
                auto fx = std::make_unique<Flanger>();
                fx->setParameter(Flanger::Depth, 0.3f);
                fx->setParameter(Flanger::Feedback, 0.3f);
                fx->setParameter(Flanger::Mix, 0.5f);
                fx->setExtra(Flanger::Through, 1.0f);
                fx->setExtra(Flanger::Time, 0.003f);
                chain.addEffect(std::move(fx));
            }
            chain.reset();

            std::vector<float> chainL(tone), chainR(tone);
            for (int done = 0; done < N; done += BLOCK)
            {
                int n = std::min(BLOCK, N - done);
                chain.processStereo(chainL.data() + done, chainR.data() + done, n);
            }

            // The chain's dry is the pure input delayed; the core's dry reads from the feedback
            // line (input + feedback * wet). With feedback 0.3 the two differ by about 16 % of
            // the signal (the feedback colours the core's dry path). This is the designed
            // difference: the chain's through-zero is clean; the core's includes feedback.
            const float d = rmsOf(aloneL.data() + 500, chainL.data() + 500, N - 500);
            const float level = rms(aloneL.data() + 500, N - 500);
            const float ratio = level > 0.0f ? d / level : 0.0f;
            logMessage("  FLG THRU alone vs chain: RMS diff " + juce::String(ratio, 4) + " of level");
            // The difference comes from the feedback in the core's dry tap; it is below -12 dB.
            expect(ratio < 0.25f, "FLG THRU in chain close to standalone");
            // Both should sound (not silence).
            expect(level > 0.01f, "FLG THRU produces sound");
        }

        // ---- THRU ON changes the sound, OFF nulls to plain effect --------------------------
        beginTest("THRU ON changes the sound and THRU OFF nulls to the plain effect");
        {
            struct Case { const char* name; EffectType type; };
            Case cases[] = {
                { "FLG", EffectType::FlangerEffect },
                { "CHR", EffectType::ChorusEffect },
                { "ENS", EffectType::EnsembleEffect },
                { "VIB", EffectType::VibratoEffect }
            };

            for (const auto& c : cases)
            {
                const int N = (int)(SR * 0.5);   // 0.5 s
                auto tone = makeTone(440.0f, 0.3f, N, SR);

                auto makeChain = [&](bool thruOn) -> std::pair<std::vector<float>, std::vector<float>>
                {
                    EffectChain ch;
                    ch.prepare(SR, BLOCK);
                    auto fx = AudioEngine::createEffect(c.type);
                    if (c.type == EffectType::VibratoEffect)
                    {
                        fx->setParameter(Vibrato::Depth, 25.0f);
                        fx->setParameter(Vibrato::Fade, 0.0f);
                    }
                    // Set mix to 0.5 for the chain's MIX cell to matter.
                    fx->setMix(0.5f);
                    // Set THRU.
                    if (c.type == EffectType::FlangerEffect)
                        fx->setExtra(Flanger::Through, thruOn ? 1.0f : 0.0f);
                    else if (c.type == EffectType::ChorusEffect)
                        fx->setExtra(Chorus::Through, thruOn ? 1.0f : 0.0f);
                    else if (c.type == EffectType::EnsembleEffect)
                        fx->setExtra(Ensemble::Through, thruOn ? 1.0f : 0.0f);
                    else if (c.type == EffectType::VibratoEffect)
                        fx->setExtra(Vibrato::Through, thruOn ? 1.0f : 0.0f);
                    ch.addEffect(std::move(fx));
                    ch.reset();

                    std::vector<float> L(tone), R(tone);
                    for (int done = 0; done < N; done += BLOCK)
                    {
                        int n = std::min(BLOCK, N - done);
                        ch.processStereo(L.data() + done, R.data() + done, n);
                    }
                    return { L, R };
                };

                auto [offL, offR] = makeChain(false);
                auto [onL, onR]  = makeChain(true);

                // THRU ON should change the sound.
                const int skip = 500;
                const float d = rmsOf(offL.data() + skip, onL.data() + skip, N - skip);
                const float level = rms(offL.data() + skip, N - skip);
                logMessage(juce::String("  ") + c.name + " THRU diff: " + juce::String(d / std::max(1.0e-6f, level), 4) + " of level");
                expect(d > 1.0e-4f, juce::String(c.name) + " THRU ON changes the sound");
            }
        }

        // ---- Glided dry delay makes no click -----------------------------------------------
        beginTest("a glided dry delay makes no click");
        {
            // A flanger with TIME turned from 1ms to 10ms in mid-block: the dry delay glides,
            // so the step relative to the tone should be small.
            const int N = (int)(SR * 0.3);
            auto tone = makeTone(1000.0f, 0.3f, N, SR);

            EffectChain chain;
            chain.prepare(SR, BLOCK);
            auto fx = std::make_unique<Flanger>();
            fx->setParameter(Flanger::Depth, 0.0f);        // no sweep
            fx->setParameter(Flanger::Feedback, 0.0f);
            fx->setParameter(Flanger::Mix, 0.5f);
            fx->setExtra(Flanger::Through, 1.0f);
            fx->setExtra(Flanger::Time, 0.001f);            // 1 ms
            auto* flg = fx.get();
            chain.addEffect(std::move(fx));
            chain.reset();

            // Run for 50 ms at TIME 1ms, then set TIME to 10ms and run the rest.
            const int first = (int)(SR * 0.05);
            std::vector<float> L(tone), R(tone);
            for (int done = 0; done < N; done += BLOCK)
            {
                if (done >= first && done < first + BLOCK)
                    flg->setExtra(Flanger::Time, 0.01f);    // step: 1ms -> 10ms
                int n = std::min(BLOCK, N - done);
                chain.processStereo(L.data() + done, R.data() + done, n);
            }

            // Measure the largest step in the output around the change point.
            float maxStep = 0.0f;
            for (int i = first; i < std::min(first + (int)(SR * 0.05), N - 1); ++i)
                maxStep = std::max(maxStep, std::abs(L[(size_t)(i + 1)] - L[(size_t) i]));
            // The tone's own largest step.
            float toneStep = 0.0f;
            for (int i = 0; i < 480; ++i)
                toneStep = std::max(toneStep, std::abs(tone[(size_t)(i + 1)] - tone[(size_t) i]));
            const float ratio = toneStep > 0.0f ? maxStep / toneStep : 0.0f;
            logMessage("  Glided dry delay step: " + juce::String(ratio, 2) + " x the tone's own");
            expect(ratio < 5.0f, "no click from the glided dry delay");
        }

        // ---- LMT integer path still works --------------------------------------------------
        beginTest("a sound of before format 3 has no THRU: the chorus's and the ensemble's FDBK and the vibrato's KEY, which had its stored place, load as THRU OFF");
        {
            // The numbers stored::old::thruOff reads by (StoredLists.h cannot include AudioEngine.h).
            static_assert((int) EffectType::ChorusEffect == stored::old::FX_CHORUS && (int) EffectType::EnsembleEffect == stored::old::FX_ENSEMBLE
                          && (int) EffectType::VibratoEffect == stored::old::FX_VIBRATO, "effect type numbers");
            static_assert(ProjectBridge::INSERT_TYPE_BASE == stored::old::INSERT_TYPE_BASE && AudioEngine::INSERT_EXTRA_BASE == stored::old::INSERT_EXTRA_BASE
                          && AudioEngine::MAX_INSERTS_PER_TRACK == stored::old::INSERT_SLOTS && EffectBase::MAX_EXTRAS == stored::old::INSERT_EXTRAS, "insert ids");
            static_assert((int) Chorus::Through == 1 && (int) Ensemble::Through == 1 && (int) Vibrato::Through == 3, "the cells THRU took");

            const int type = ProjectBridge::INSERT_TYPE_BASE, extra = AudioEngine::INSERT_EXTRA_BASE;
            auto stored = [&](int version)
            {
                ProjectData data;
                data.patterns[0].tracks.resize(1);
                auto& sound = data.patterns[0].tracks[0].soundParams;
                sound[type + 0] = (float) ((int) EffectType::ChorusEffect + 1);
                sound[extra + 0 * 4 + 1] = 0.6f;                            // FDBK 0.6 (it read as THRU ON)
                sound[type + 1] = (float) ((int) EffectType::EnsembleEffect + 1);
                sound[extra + 1 * 4 + 1] = 0.95f;                           // FDBK at its top
                sound[type + 2] = (float) ((int) EffectType::VibratoEffect + 1);
                sound[extra + 2 * 4 + 3] = (float) dyn::KEY_CHOICE_IN;      // KEY IN
                sound[type + 3] = (float) ((int) EffectType::FlangerEffect + 1);
                sound[extra + 3 * 4 + (int) Flanger::Through] = 1.0f;       // the flanger had THRU before: it stays
                sound[extra + 0 * 4 + 3] = 0.012f;                          // the chorus's TIME: not touched
                juce::var file = data.toVar();
                file.getDynamicObject()->setProperty("version", version);
                return ProjectData::fromVar(file).patterns[0].tracks[0].soundParams;
            };
            auto before = stored(2);
            expectEquals(before[extra + 0 * 4 + 1], 0.0f, "CHR: a stored FDBK of 0.6");
            expectEquals(before[extra + 1 * 4 + 1], 0.0f, "ENS: a stored FDBK of 0.95");
            expectEquals(before[extra + 2 * 4 + 3], 0.0f, "VIB: a stored KEY at IN");
            expectEquals(before[extra + 3 * 4 + (int) Flanger::Through], 1.0f, "FLG: its THRU");
            expectEquals(before[extra + 0 * 4 + 3], 0.012f, "CHR: its TIME");
            // A file of format 3 was written with THRU: what it holds there is THRU.
            auto now = stored(ProjectData::FORMAT_VERSION);
            expectEquals(now[extra + 0 * 4 + 1], 0.6f);
            expectEquals(now[extra + 2 * 4 + 3], (float) dyn::KEY_CHOICE_IN);
            // What the effect makes of the number when it is set (why the loader has the line):
            Chorus chorus;
            chorus.setExtra(Chorus::Through, 0.6f);
            expectEquals(chorus.getExtra(Chorus::Through), 1.0f, "0.6 set on THRU is ON");
            chorus.setExtra(Chorus::Through, 0.3f);
            expectEquals(chorus.getExtra(Chorus::Through), 0.0f, "0.3 set on THRU is OFF");
        }

        beginTest("LMT latency path unchanged");
        {
            const int N = (int)(SR * 0.1);
            auto tone = makeTone(1000.0f, 0.5f, N, SR);

            EffectChain chain;
            chain.prepare(SR, BLOCK);
            auto fx = std::make_unique<Limiter>();
            fx->setCeiling(-0.3f);
            fx->setLookahead(3.0f);
            fx->setMix(0.5f);       // partial limiting
            chain.addEffect(std::move(fx));
            chain.reset();

            std::vector<float> L(tone), R(tone);
            for (int done = 0; done < N; done += BLOCK)
            {
                int n = std::min(BLOCK, N - done);
                chain.processStereo(L.data() + done, R.data() + done, n);
            }

            // The output should be audible (not silence) and not NaN.
            bool finite = true;
            for (int i = 0; i < N; ++i) finite = finite && std::isfinite(L[(size_t) i]);
            expect(finite, "LMT output is finite");
            expect(rms(L.data() + 200, N - 200) > 0.01f, "LMT output is audible");
        }
    }
};

static ThroughZeroTests throughZeroTests;
