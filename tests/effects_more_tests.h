#pragma once
#include <juce_core/juce_core.h>

#include "../src/dsp/effects/RingMod.h"
#include "../src/dsp/effects/Vibrato.h"
#include "../src/dsp/effects/Wah.h"
#include "../src/dsp/effects/AmpSim.h"
#include "../src/dsp/effects/GraphicEQ.h"
#include "../src/dsp/effects/TapeDelay.h"
#include "../src/dsp/effects/LCRDelay.h"
#include "../src/dsp/effects/MultiTapInsert.h"
#include "../src/dsp/effects/SidechainInsert.h"

#include <cctype>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

// Ring modulator, vibrato, wah, amp, graphic EQ, tape delay, L/C/R delay and the insert-slot
// adapters for the multi-tap delay and the sidechain compressor. Every check renders audio through
// the effect (fresh input each block) and measures the result.
class EffectsMoreTests : public juce::UnitTest
{
public:
    EffectsMoreTests() : UnitTest("Effects More") {}

    void runTest() override
    {
        testContract();
        testRingMod();
        testVibrato();
        testWah();
        testAmp();
        testGraphicEQ();
        testTapeDelay();
        testLCRDelay();
        testMultiTapInsert();
        testSidechainInsert();
    }

private:
    using Buf = std::vector<float>;
    using Gen = std::function<float(int)>;

    static constexpr int BLOCK = 256;

    // ---- signal helpers ------------------------------------------------------------------------

    static Gen sine(double hz, double sr, float amp)
    {
        return [=](int i) { return amp * (float) std::sin(6.283185307179586 * hz * (double) i / sr); };
    }

    static Gen silence() { return [](int) { return 0.0f; }; }

    static Gen impulse(float amp) { return [=](int i) { return i == 0 ? amp : 0.0f; }; }

    static Gen noise(float amp, uint32_t seed)
    {
        auto state = std::make_shared<uint32_t>(seed);
        return [=](int) { return amp * dsp::xorshiftNoise(*state); };
    }

    // Appends n samples to outL / outR. The generators get the absolute sample index, so a second
    // call continues the signal. Input is generated fresh for every block.
    static void render(EffectBase& fx, int n, const Gen& genL, const Gen& genR, Buf& outL, Buf& outR,
                       Buf* dryL = nullptr)
    {
        float l[BLOCK], r[BLOCK];
        int done = 0;
        while (done < n)
        {
            int count = std::min(BLOCK, n - done);
            int base = (int) outL.size();
            for (int i = 0; i < count; ++i)
            {
                l[i] = genL(base + i);
                r[i] = genR(base + i);
                if (dryL) dryL->push_back(l[i]);
            }
            fx.processStereo(l, r, count);
            outL.insert(outL.end(), l, l + count);
            outR.insert(outR.end(), r, r + count);
            done += count;
        }
    }

    static bool allFinite(const Buf& b)
    {
        for (float v : b)
            if (!std::isfinite(v)) return false;
        return true;
    }

    static float peak(const Buf& b, int start, int end)
    {
        float p = 0.0f;
        start = std::max(0, start);
        end = std::min(end, (int) b.size());
        for (int i = start; i < end; ++i)
            p = std::max(p, std::abs(b[(size_t) i]));
        return p;
    }

    static int peakIndex(const Buf& b, int start, int end)
    {
        int idx = start;
        float p = -1.0f;
        for (int i = std::max(0, start); i < std::min(end, (int) b.size()); ++i)
            if (std::abs(b[(size_t) i]) > p) { p = std::abs(b[(size_t) i]); idx = i; }
        return idx;
    }

    static float rms(const Buf& b, int start, int end)
    {
        double sum = 0.0;
        start = std::max(0, start);
        end = std::min(end, (int) b.size());
        for (int i = start; i < end; ++i)
            sum += (double) b[(size_t) i] * b[(size_t) i];
        return end > start ? (float) std::sqrt(sum / (double) (end - start)) : 0.0f;
    }

    // Amplitude of the component at hz (single DFT bin over [start, end)).
    static float amplitudeAt(const Buf& b, int start, int end, double hz, double sr)
    {
        double re = 0.0, im = 0.0;
        for (int i = start; i < end; ++i)
        {
            double ph = 6.283185307179586 * hz * (double) i / sr;
            re += b[(size_t) i] * std::cos(ph);
            im += b[(size_t) i] * std::sin(ph);
        }
        return (float) (2.0 * std::sqrt(re * re + im * im) / (double) (end - start));
    }

    // Longest / shortest period between upward zero crossings in [start, end): 1 for a steady tone.
    static float periodSpread(const Buf& b, int start, int end)
    {
        double last = -1.0, minP = 1.0e9, maxP = 0.0;
        for (int i = start + 1; i < end; ++i)
        {
            float a = b[(size_t) i - 1], c = b[(size_t) i];
            if (a < 0.0f && c >= 0.0f)
            {
                double t = (double) (i - 1) + (double) (-a) / (double) (c - a);
                if (last >= 0.0)
                {
                    minP = std::min(minP, t - last);
                    maxP = std::max(maxP, t - last);
                }
                last = t;
            }
        }
        return maxP > 0.0 ? (float) (maxP / minP) : 0.0f;
    }

    static float toDb(float ratio) { return 20.0f * std::log10(std::max(ratio, 1.0e-9f)); }

    // Steady-state gain of the effect for a sine (left channel).
    float sineGain(EffectBase& fx, double hz, double sr, float amp, double seconds = 0.5)
    {
        fx.prepare(sr, BLOCK);
        Buf l, r;
        int n = (int) (seconds * sr);
        render(fx, n, sine(hz, sr, amp), sine(hz, sr, amp), l, r);
        return amplitudeAt(l, n / 2, n, hz, sr) / amp;
    }

    // ---- the four-parameter contract, silence, extremes ----------------------------------------

    struct Entry
    {
        juce::String name;
        std::function<std::unique_ptr<EffectBase>()> make;
        int mixParam;   // index of the dry / wet parameter, -1 if the effect has none
    };

    static std::vector<Entry> allEffects()
    {
        return {
            { "RingMod",        [] { return std::unique_ptr<EffectBase>(new RingMod()); }, 3 },
            { "Vibrato",        [] { return std::unique_ptr<EffectBase>(new Vibrato()); }, -1 },
            { "Wah",            [] { return std::unique_ptr<EffectBase>(new Wah()); }, -1 },
            { "AmpSim",         [] { return std::unique_ptr<EffectBase>(new AmpSim()); }, -1 },
            { "GraphicEQ",      [] { return std::unique_ptr<EffectBase>(new GraphicEQ()); }, -1 },
            { "TapeDelay",      [] { return std::unique_ptr<EffectBase>(new TapeDelay()); }, 3 },
            { "LCRDelay",       [] { return std::unique_ptr<EffectBase>(new LCRDelay()); }, 3 },
            { "MultiTapInsert", [] { return std::unique_ptr<EffectBase>(new MultiTapInsert()); }, 3 },
            { "SidechainInsert",[] { return std::unique_ptr<EffectBase>(new SidechainInsert()); }, -1 },
        };
    }

    void testContract()
    {
        for (auto& entry : allEffects())
        {
            const auto& name = entry.name;

            beginTest(name + ": four described parameters, defaults seeded, values round-trip");
            {
                auto fx = entry.make();
                for (int p = 0; p < 4; ++p)
                {
                    ParamSpec spec = fx->getParamSpec(p);
                    expect(spec.isValid(), name + " param " + juce::String(p) + " has a spec");
                    expectEquals(spec.id, p);
                    size_t len = std::strlen(spec.label);
                    expect(len >= 1 && len <= 4, name + " label fits four characters");
                    for (size_t c = 0; c < len; ++c)
                        expect(! std::islower((unsigned char) spec.label[c]), name + " label is upper case");
                    expect(spec.maxVal > spec.minVal);
                    expect(spec.defaultVal >= spec.minVal && spec.defaultVal <= spec.maxVal);
                    if (spec.kind == ParamKind::Log)
                        expect(spec.minVal > 0.0f, name + " log parameter has a positive minimum");
                    if (spec.kind == ParamKind::Choice)
                        expect(spec.choices != nullptr && spec.numChoices == (int) spec.maxVal + 1);

                    expectWithinAbsoluteError(fx->getParameter(p), spec.defaultVal, 1.0e-5f);
                    fx->setParameter(p, spec.maxVal);
                    expectWithinAbsoluteError(fx->getParameter(p), spec.maxVal, 1.0e-4f);
                    fx->setParameter(p, spec.minVal);
                    expectWithinAbsoluteError(fx->getParameter(p), spec.minVal, 1.0e-4f);
                    fx->setParameter(p, spec.maxVal + 1000.0f);
                    expectWithinAbsoluteError(fx->getParameter(p), spec.maxVal, 1.0e-4f);
                    fx->setParameter(p, spec.minVal - 1000.0f);
                    expectWithinAbsoluteError(fx->getParameter(p), spec.minVal, 1.0e-4f);
                }
                expect(! fx->getParamSpec(4).isValid());
            }

            beginTest(name + ": usable straight after construction, bypass passes the input");
            {
                auto fx = entry.make();   // no prepare() on purpose
                Buf l, r;
                render(*fx, 2000, noise(0.5f, 11), noise(0.5f, 12), l, r);
                expect(allFinite(l) && allFinite(r));

                auto off = entry.make();
                off->prepare(48000.0, BLOCK);
                off->setEnabled(false);
                Buf ol, orr, dry;
                render(*off, 2000, noise(0.5f, 11), noise(0.5f, 12), ol, orr, &dry);
                expect(ol == dry, name + " disabled = untouched");
            }

            for (double sr : { 44100.0, 48000.0, 96000.0 })
            {
                const juce::String at = " at " + juce::String((int) sr);

                beginTest(name + ": silence in, silence out" + at);
                for (int setting = 0; setting < 3; ++setting)   // defaults, all minimum, all maximum
                {
                    auto fx = entry.make();
                    applyExtreme(*fx, setting);
                    fx->prepare(sr, BLOCK);
                    Buf l, r;
                    render(*fx, (int) sr, silence(), silence(), l, r);
                    expect(allFinite(l) && allFinite(r));
                    expect(peak(l, 0, (int) l.size()) < 1.0e-9f && peak(r, 0, (int) r.size()) < 1.0e-9f,
                           name + " makes no sound or DC from silence (setting " + juce::String(setting) + ")");
                }

                beginTest(name + ": finite and bounded at extreme settings" + at);
                for (int setting = 1; setting < 3; ++setting)
                {
                    auto fx = entry.make();
                    applyExtreme(*fx, setting);
                    fx->prepare(sr, BLOCK);
                    Buf l, r;
                    render(*fx, (int) sr, noise(1.0f, 101), noise(1.0f, 202), l, r);
                    // then a full-scale square wave at 30 Hz and silence, parameters swept while it runs
                    int n0 = (int) l.size();
                    for (int step = 0; step < 20; ++step)
                    {
                        for (int p = 0; p < 4; ++p)
                        {
                            ParamSpec spec = fx->getParamSpec(p);
                            fx->setParameter(p, (step + p) % 2 == 0 ? spec.minVal : spec.maxVal);
                        }
                        render(*fx, (int) (sr * 0.05),
                               [=](int i) { return ((i - n0) * 60 / (int) sr) % 2 == 0 ? 1.0f : -1.0f; },
                               [=](int i) { return ((i - n0) * 60 / (int) sr) % 2 == 0 ? -1.0f : 1.0f; }, l, r);
                    }
                    applyExtreme(*fx, setting);
                    render(*fx, (int) (sr * 2.0), silence(), silence(), l, r);

                    expect(allFinite(l) && allFinite(r), name + " stays finite (setting " + juce::String(setting) + ")");
                    float p = std::max(peak(l, 0, (int) l.size()), peak(r, 0, (int) r.size()));
                    expect(p < 40.0f, name + " stays bounded, peak " + juce::String(p));
                }
            }

            if (entry.mixParam >= 0)
            {
                beginTest(name + ": mix 0 is the dry signal");
                auto fx = entry.make();
                fx->setParameter(entry.mixParam, 0.0f);
                fx->prepare(48000.0, BLOCK);
                Buf l, r, dry;
                render(*fx, 24000, noise(0.7f, 5), noise(0.7f, 6), l, r, &dry);
                float worst = 0.0f;
                for (size_t i = 0; i < l.size(); ++i)
                    worst = std::max(worst, std::abs(l[i] - dry[i]));
                expect(worst < 1.0e-6f, name + " mix 0 differs from dry by " + juce::String(worst));

                fx->setParameter(entry.mixParam, 1.0f);
                Buf wl, wr, dry2;
                render(*fx, 24000, noise(0.7f, 5), noise(0.7f, 6), wl, wr, &dry2);
                expect(wl != dry2, name + " mix 1 is not the dry signal");
            }
        }
    }

    // 0 = leave defaults, 1 = every parameter at its minimum, 2 = every parameter at its maximum.
    static void applyExtreme(EffectBase& fx, int setting)
    {
        if (setting == 0) return;
        for (int p = 0; p < 4; ++p)
        {
            ParamSpec spec = fx.getParamSpec(p);
            fx.setParameter(p, setting == 1 ? spec.minVal : spec.maxVal);
        }
    }

    // ---- ring modulator ------------------------------------------------------------------------

    void testRingMod()
    {
        const double sr = 48000.0;
        const int n = 48000;

        beginTest("RingMod: a 1 kHz tone times a 300 Hz carrier gives 700 Hz and 1300 Hz, no 1 kHz");
        {
            RingMod fx;
            fx.setParameter(RingMod::Freq, 300.0f);
            fx.setParameter(RingMod::Depth, 0.0f);
            fx.setParameter(RingMod::Mix, 1.0f);
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, n, sine(1000.0, sr, 0.5f), sine(1000.0, sr, 0.5f), l, r);
            expectWithinAbsoluteError(amplitudeAt(l, 0, n, 700.0, sr), 0.25f, 0.01f);
            expectWithinAbsoluteError(amplitudeAt(l, 0, n, 1300.0, sr), 0.25f, 0.01f);
            expect(amplitudeAt(l, 0, n, 1000.0, sr) < 0.005f, "the input frequency is suppressed");
            expectWithinAbsoluteError(amplitudeAt(r, 0, n, 700.0, sr), 0.25f, 0.01f);
        }

        beginTest("RingMod: the carrier frequency sets where the sidebands are");
        {
            RingMod fx;
            fx.setParameter(RingMod::Freq, 450.0f);
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, n, sine(1000.0, sr, 0.5f), sine(1000.0, sr, 0.5f), l, r);
            expectWithinAbsoluteError(amplitudeAt(l, 0, n, 550.0, sr), 0.25f, 0.01f);
            expectWithinAbsoluteError(amplitudeAt(l, 0, n, 1450.0, sr), 0.25f, 0.01f);
            expect(amplitudeAt(l, 0, n, 700.0, sr) < 0.005f);
        }

        beginTest("RingMod: half mix keeps half of the input next to the sidebands");
        {
            RingMod fx;
            fx.setParameter(RingMod::Freq, 300.0f);
            fx.setParameter(RingMod::Mix, 0.5f);
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, n, sine(1000.0, sr, 0.5f), sine(1000.0, sr, 0.5f), l, r);
            expectWithinAbsoluteError(amplitudeAt(l, 0, n, 1000.0, sr), 0.25f, 0.01f);
            expectWithinAbsoluteError(amplitudeAt(l, 0, n, 700.0, sr), 0.125f, 0.01f);
        }

        beginTest("RingMod: the LFO moves the carrier, smearing the sidebands");
        {
            RingMod fx;
            fx.setParameter(RingMod::Freq, 300.0f);
            fx.setParameter(RingMod::Rate, 3.0f);
            fx.setParameter(RingMod::Depth, 1.0f);
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, n, sine(1000.0, sr, 0.5f), sine(1000.0, sr, 0.5f), l, r);
            expect(amplitudeAt(l, 0, n, 700.0, sr) < 0.1f, "the fixed 700 Hz sideband is gone");
            expectWithinAbsoluteError(rms(l, 0, n), 0.25f, 0.03f);   // energy is still there
        }
    }

    // ---- vibrato -------------------------------------------------------------------------------

    void testVibrato()
    {
        const double sr = 48000.0;

        beginTest("Vibrato: depth 0 is a clean delay of BASE_DELAY samples");
        {
            Vibrato fx;
            fx.setParameter(Vibrato::Depth, 0.0f);
            fx.prepare(sr, BLOCK);
            Buf l, r, dry;
            render(fx, 20000, noise(0.5f, 3), noise(0.5f, 4), l, r, &dry);
            bool same = true;
            for (size_t i = Vibrato::BASE_DELAY; i < l.size(); ++i)
                same = same && (l[i] == dry[i - Vibrato::BASE_DELAY]);
            expect(same, "output is the input, two samples late");
        }

        beginTest("Vibrato: depth sets the pitch swing in cents, at any rate");
        for (float rate : { 2.0f, 5.0f, 9.0f })
        {
            for (float cents : { 0.0f, 30.0f, 100.0f })
            {
                Vibrato fx;
                fx.setParameter(Vibrato::Rate, rate);
                fx.setParameter(Vibrato::Depth, cents);
                fx.prepare(sr, BLOCK);
                Buf l, r;
                render(fx, 96000, sine(440.0, sr, 0.5f), sine(440.0, sr, 0.5f), l, r);
                float spreadCents = 1200.0f * std::log2(periodSpread(l, 24000, 96000));
                // highest to lowest pitch is twice the depth
                expectWithinAbsoluteError(spreadCents, 2.0f * cents, 2.0f + 0.2f * cents,
                    "rate " + juce::String(rate) + " depth " + juce::String(cents) + ": " + juce::String(spreadCents));
                expectWithinAbsoluteError(rms(l, 24000, 96000), 0.3536f, 0.01f);   // level unchanged
            }
        }

        beginTest("Vibrato: spread puts the right channel's modulation out of phase with the left");
        {
            auto difference = [&](float spread)
            {
                Vibrato fx;
                fx.setParameter(Vibrato::Depth, 60.0f);
                fx.setParameter(Vibrato::Spread, spread);
                fx.prepare(sr, BLOCK);
                Buf l, r;
                render(fx, 48000, sine(440.0, sr, 0.5f), sine(440.0, sr, 0.5f), l, r);
                Buf d(l.size());
                for (size_t i = 0; i < l.size(); ++i) d[i] = l[i] - r[i];
                return rms(d, 12000, 48000);
            };
            expect(difference(0.0f) < 1.0e-6f, "no spread = identical channels");
            expect(difference(1.0f) > 0.05f, "full spread = channels differ");
        }

        beginTest("Vibrato: fade delays the vibrato after a note starts");
        {
            Vibrato fx;
            fx.setParameter(Vibrato::Depth, 100.0f);
            fx.setParameter(Vibrato::Fade, 1.0f);
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, 24000, silence(), silence(), l, r);
            const int onset = (int) l.size();
            auto tone = [=](int i) { return 0.5f * (float) std::sin(6.283185307179586 * 440.0 * (i - onset) / sr); };
            render(fx, 96000, tone, tone, l, r);
            float early = 1200.0f * std::log2(periodSpread(l, onset + 480, onset + 7200));    // first 150 ms
            float late = 1200.0f * std::log2(periodSpread(l, onset + 60000, onset + 96000));  // after the fade
            expect(early < 40.0f, "almost no vibrato at the start: " + juce::String(early));
            expect(late > 170.0f, "full vibrato after the fade: " + juce::String(late));

            Vibrato immediate;
            immediate.setParameter(Vibrato::Depth, 100.0f);
            immediate.setParameter(Vibrato::Rate, 9.0f);
            immediate.prepare(sr, BLOCK);
            Buf il, ir;
            render(immediate, 24000, silence(), silence(), il, ir);
            render(immediate, 24000, tone, tone, il, ir);
            expect(1200.0f * std::log2(periodSpread(il, onset + 480, onset + 14400)) > 150.0f,
                   "fade 0 = vibrato from the first cycle");
        }
    }

    // ---- wah -----------------------------------------------------------------------------------

    void testWah()
    {
        const double sr = 48000.0;

        beginTest("Wah: with envelope and LFO off, the peak sits at FREQ with gain sqrt(Q)");
        for (float centre : { 300.0f, 500.0f, 1500.0f })
        {
            Wah fx;
            fx.setParameter(Wah::Freq, centre);
            fx.setParameter(Wah::Reso, 0.5f);
            fx.setParameter(Wah::Sens, 0.0f);
            fx.setParameter(Wah::Rate, 0.0f);

            float atCentre = sineGain(fx, centre, sr, 0.1f);
            float expected = std::sqrt(std::pow(12.0f, 0.5f));
            expectWithinAbsoluteError(atCentre, expected, 0.1f);
            for (float ratio : { 0.25f, 0.5f, 2.0f, 4.0f })
                expect(sineGain(fx, centre * ratio, sr, 0.1f) < atCentre * 0.6f,
                       "less gain " + juce::String(ratio) + "x away from " + juce::String(centre));
        }

        beginTest("Wah: resonance narrows and raises the peak");
        {
            auto gains = [&](float reso)
            {
                Wah fx;
                fx.setParameter(Wah::Freq, 600.0f);
                fx.setParameter(Wah::Reso, reso);
                fx.setParameter(Wah::Sens, 0.0f);
                return std::pair<float, float>(sineGain(fx, 600.0, sr, 0.1f), sineGain(fx, 1200.0, sr, 0.1f));
            };
            auto low = gains(0.0f), high = gains(1.0f);
            expect(high.first > low.first * 2.5f, "more gain at the centre");
            expect(high.second / high.first < 0.5f * low.second / low.first, "narrower");
        }

        beginTest("Wah: a louder input pushes the peak up (envelope), by an amount set by SENS");
        {
            auto run = [&](float sens, float amp, float probeHz, float& centreOut)
            {
                Wah fx;
                fx.setParameter(Wah::Freq, 300.0f);
                fx.setParameter(Wah::Sens, sens);
                float g = sineGain(fx, probeHz, sr, amp);
                centreOut = fx.getCurrentFrequency();
                return g;
            };
            float cQuiet, cLoud, cHalf, cNone;
            float quietAt300 = run(1.0f, 0.002f, 300.0f, cQuiet);
            float loudAt300 = run(1.0f, 0.5f, 300.0f, cLoud);
            expect(quietAt300 > 1.5f, "quiet: the filter rests at FREQ");
            expect(loudAt300 < quietAt300 * 0.25f, "loud: the filter has left 300 Hz");
            expect(cQuiet < 330.0f);
            expect(cLoud > 3000.0f, "loud input opens it by octaves: " + juce::String(cLoud));

            run(0.5f, 0.5f, 300.0f, cHalf);
            run(0.0f, 0.5f, 300.0f, cNone);
            expect(cHalf > 900.0f && cHalf < cLoud * 0.6f, "half sensitivity, half the octaves");
            expectWithinAbsoluteError(cNone, 300.0f, 1.0f);

            float loudUpHigh = run(1.0f, 0.5f, 4500.0f, cLoud);
            expect(loudUpHigh > 1.0f, "and the loud signal's peak is now up there");
        }

        beginTest("Wah: the LFO sweeps the peak at RATE");
        {
            auto sweep = [&](float rate, float& minHz, float& maxHz)
            {
                Wah fx;
                fx.setParameter(Wah::Freq, 500.0f);
                fx.setParameter(Wah::Sens, 0.0f);
                fx.setParameter(Wah::Rate, rate);
                fx.prepare(sr, BLOCK);
                Buf l, r;
                minHz = 1.0e9f; maxHz = 0.0f;
                // level of a 500 Hz tone in 20 ms windows over two seconds
                Buf levels;
                for (int w = 0; w < 100; ++w)
                {
                    int start = (int) l.size();
                    render(fx, 960, sine(500.0, sr, 0.1f), sine(500.0, sr, 0.1f), l, r);
                    if (w >= 5)
                    {
                        levels.push_back(rms(l, start, start + 960));
                        minHz = std::min(minHz, fx.getCurrentFrequency());
                        maxHz = std::max(maxHz, fx.getCurrentFrequency());
                    }
                }
                return levels;
            };

            float lo, hi;
            Buf still = sweep(0.0f, lo, hi);
            float stillMin = *std::min_element(still.begin(), still.end());
            float stillMax = *std::max_element(still.begin(), still.end());
            expect(stillMax / stillMin < 1.05f, "no LFO: steady level");
            expectWithinAbsoluteError(lo, 500.0f, 1.0f);
            expectWithinAbsoluteError(hi, 500.0f, 1.0f);

            Buf moving = sweep(2.0f, lo, hi);
            float movMin = *std::min_element(moving.begin(), moving.end());
            float movMax = *std::max_element(moving.begin(), moving.end());
            expect(movMax / movMin > 2.0f, "LFO: the tone's level rises and falls as the peak passes");
            expect(lo < 280.0f && hi > 900.0f, "about an octave each way: " + juce::String(lo) + " - " + juce::String(hi));

            // the level pattern repeats at the LFO rate: the peak passes the tone twice per cycle,
            // so windows half a second (one LFO cycle) apart match
            float diffCycle = 0.0f, diffQuarter = 0.0f;
            for (size_t i = 0; i + 25 < moving.size(); ++i)
            {
                diffCycle += std::abs(moving[i] - moving[i + 25]);
                diffQuarter += std::abs(moving[i] - moving[i + 6]);
            }
            expect(diffCycle < diffQuarter * 0.3f, "periodic at 2 Hz");
        }
    }

    // ---- amp -----------------------------------------------------------------------------------

    void testAmp()
    {
        const double sr = 48000.0;
        const int n = 48000;

        auto harmonics = [&](float gain, float amp, float& h1, float& h2, float& h3, float& pk)
        {
            AmpSim fx;
            fx.setParameter(AmpSim::Gain, gain);
            fx.setParameter(AmpSim::Cab, 10000.0f);
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, n, sine(220.0, sr, amp), sine(220.0, sr, amp), l, r);
            h1 = amplitudeAt(l, n / 2, n, 220.0, sr);
            h2 = amplitudeAt(l, n / 2, n, 440.0, sr);
            h3 = amplitudeAt(l, n / 2, n, 660.0, sr);
            pk = peak(l, 0, n);
        };

        beginTest("Amp: gain adds harmonics (even ones too) and the output stays bounded");
        {
            float c1, c2, c3, cp, m1, m2, m3, mp, d1, d2, d3, dp;
            harmonics(0.0f, 0.05f, c1, c2, c3, cp);
            harmonics(0.4f, 0.3f, m1, m2, m3, mp);
            harmonics(0.9f, 0.3f, d1, d2, d3, dp);
            expect(c3 / c1 < 0.01f, "clean at low gain and level: " + juce::String(c3 / c1));
            expectWithinAbsoluteError(c1, 0.025f, 0.006f);   // clean: the fixed -6 dB trim
            expect(m3 / m1 > 0.03f, "third harmonic at medium gain: " + juce::String(m3 / m1));
            expect(m2 / m1 > 0.02f, "second harmonic from the asymmetric stage: " + juce::String(m2 / m1));
            expect(d3 / d1 > 0.15f, "strong third harmonic when driven hard: " + juce::String(d3 / d1));
            expect(d3 / d1 > m3 / m1, "more gain, more distortion");
            expect(dp < 1.5f && dp > 0.3f, "driven output is limited by the clipping stages: " + juce::String(dp));

            // 10 dB more input at full gain barely changes the output level
            float e1, e2, e3, ep;
            harmonics(1.0f, 1.0f, e1, e2, e3, ep);
            expect(ep < 1.5f && ep > 0.3f, "full gain, full-scale input: " + juce::String(ep));
            expect(e1 < d1 * 1.2f);
        }

        beginTest("Amp: no DC on the output even though the first stage is asymmetric");
        {
            AmpSim fx;
            fx.setParameter(AmpSim::Gain, 0.8f);
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, n, sine(220.0, sr, 0.5f), sine(220.0, sr, 0.5f), l, r);
            double mean = 0.0;
            for (int i = n / 2; i < n; ++i) mean += l[(size_t) i];
            expect(std::abs(mean / (n / 2)) < 0.003, "mean " + juce::String(mean / (n / 2)));
        }

        beginTest("Amp: TONE tilts bass against treble");
        {
            auto gainAt = [&](float tone, double hz)
            {
                AmpSim fx;
                fx.setParameter(AmpSim::Gain, 0.0f);
                fx.setParameter(AmpSim::Cab, 10000.0f);
                fx.setParameter(AmpSim::Tone, tone);
                return sineGain(fx, hz, sr, 0.02f);
            };
            float trebleDb = toDb(gainAt(1.0f, 6000.0) / gainAt(-1.0f, 6000.0));
            float bassDb = toDb(gainAt(1.0f, 150.0) / gainAt(-1.0f, 150.0));
            expect(trebleDb > 12.0f, "bright has more treble: " + juce::String(trebleDb));
            expect(bassDb < -8.0f, "and less bass: " + juce::String(bassDb));
        }

        beginTest("Amp: CAB sets the speaker roll-off, and the cabinet removes sub-bass");
        {
            auto gainAt = [&](float cab, double hz)
            {
                AmpSim fx;
                fx.setParameter(AmpSim::Gain, 0.0f);
                fx.setParameter(AmpSim::Cab, cab);
                return sineGain(fx, hz, sr, 0.02f);
            };
            expect(toDb(gainAt(1500.0f, 6000.0) / gainAt(10000.0f, 6000.0)) < -30.0f, "4-pole roll-off above CAB");
            expectWithinAbsoluteError(toDb(gainAt(3000.0f, 3000.0) / gainAt(10000.0f, 3000.0)), -3.0f, 1.0f);
            expect(toDb(gainAt(1500.0f, 500.0) / gainAt(10000.0f, 500.0)) > -1.0f, "passband untouched");
            expect(toDb(gainAt(4500.0f, 25.0) / gainAt(4500.0f, 500.0)) < -20.0f, "25 Hz is well down");
        }

        beginTest("Amp: LEVL is the output level in dB");
        {
            auto level = [&](float db)
            {
                AmpSim fx;
                fx.setParameter(AmpSim::Level, db);
                return sineGain(fx, 440.0, sr, 0.2f);
            };
            float ref = level(0.0f);
            expectWithinAbsoluteError(toDb(level(6.0f) / ref), 6.0f, 0.1f);
            expectWithinAbsoluteError(toDb(level(-24.0f) / ref), -24.0f, 0.1f);
        }
    }

    // ---- graphic EQ ----------------------------------------------------------------------------

    void testGraphicEQ()
    {
        beginTest("GraphicEQ: all bands at 0 dB is unity");
        {
            GraphicEQ fx;
            fx.prepare(48000.0, BLOCK);
            Buf l, r, dry;
            render(fx, 24000, noise(0.8f, 9), noise(0.8f, 10), l, r, &dry);
            float worst = 0.0f;
            for (size_t i = 0; i < l.size(); ++i)
                worst = std::max(worst, std::abs(l[i] - dry[i]));
            expect(worst < 1.0e-4f, "flat differs from the input by " + juce::String(worst));
        }

        // Where each band is measured (shelves well inside their shelf) and a frequency it must
        // leave alone.
        const double probe[4] = { 30.0, 500.0, 2000.0, 15000.0 };
        const double away[4] = { 3000.0, 6000.0, 150.0, 300.0 };

        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            beginTest("GraphicEQ: each band changes its own frequency by the set dB at " + juce::String((int) sr));
            for (int band = 0; band < 4; ++band)
            {
                for (float db : { 12.0f, 6.0f, -6.0f, -12.0f })
                {
                    GraphicEQ fx;
                    fx.setParameter(band, db);
                    float got = toDb(sineGain(fx, probe[band], sr, 0.1f));
                    expectWithinAbsoluteError(got, db, 1.0f,
                        "band " + juce::String(band) + " set " + juce::String(db) + " got " + juce::String(got));
                    float other = toDb(sineGain(fx, away[band], sr, 0.1f, 0.25));
                    expectWithinAbsoluteError(other, 0.0f, 1.0f,
                        "band " + juce::String(band) + " leaves " + juce::String(away[band]) + " Hz alone: " + juce::String(other));
                }
            }
        }

        beginTest("GraphicEQ: turning a band while audio runs follows smoothly to the new gain");
        {
            const double sr = 48000.0;
            GraphicEQ fx;
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, 12000, sine(500.0, sr, 0.1f), sine(500.0, sr, 0.1f), l, r);
            fx.setParameter(GraphicEQ::LowMid, 12.0f);
            render(fx, 24000, sine(500.0, sr, 0.1f), sine(500.0, sr, 0.1f), l, r);
            expectWithinAbsoluteError(toDb(amplitudeAt(l, 24000, 36000, 500.0, sr) / 0.1f), 12.0f, 0.3f);
            float biggestStep = 0.0f;
            for (size_t i = 12001; i < l.size(); ++i)
                biggestStep = std::max(biggestStep, std::abs(l[i] - l[i - 1]));
            // a 0.4-amplitude 500 Hz sine moves at most 0.026 per sample: no jump beyond that
            expect(biggestStep < 0.035f, "no click: " + juce::String(biggestStep));
        }
    }

    // ---- tape delay ----------------------------------------------------------------------------

    void testTapeDelay()
    {
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            beginTest("TapeDelay: echoes at TIME and its multiples, each quieter, at " + juce::String((int) sr));
            for (float time : { 0.1f, 0.25f, 0.6f })
            {
                TapeDelay fx;
                fx.setParameter(TapeDelay::Time, time);
                fx.setParameter(TapeDelay::Feedback, 0.5f);
                fx.setParameter(TapeDelay::Age, 0.0f);
                fx.setParameter(TapeDelay::Mix, 1.0f);
                fx.prepare(sr, BLOCK);
                Buf l, r;
                int d = (int) std::lround(time * sr);
                render(fx, d * 3 + d / 2, impulse(0.8f), impulse(0.4f), l, r);

                expect(peak(l, 0, d - 4) < 1.0e-5f, "nothing before the first echo");
                int first = peakIndex(l, 0, d + d / 2);
                int second = peakIndex(l, d + d / 2, 2 * d + d / 2);
                int third = peakIndex(l, 2 * d + d / 2, 3 * d + d / 2);
                expect(std::abs(first - d) <= 3, "first echo at " + juce::String(first) + " expected " + juce::String(d));
                expect(std::abs(second - 2 * d) <= 6, "second echo at " + juce::String(second));
                expect(std::abs(third - 3 * d) <= 9, "third echo at " + juce::String(third));
                float a1 = std::abs(l[(size_t) first]), a2 = std::abs(l[(size_t) second]), a3 = std::abs(l[(size_t) third]);
                expect(a1 > 0.1f);
                expect(a2 < a1 * 0.6f && a2 > a1 * 0.05f, "second is quieter: " + juce::String(a2 / a1));
                expect(a3 < a2 * 0.75f && a3 > 0.0f, "third quieter again: " + juce::String(a3 / a2));

                // each channel echoes its own input
                int firstR = peakIndex(r, 0, d + d / 2);
                expect(std::abs(firstR - d) <= 3);
                expectWithinAbsoluteError(std::abs(r[(size_t) firstR]) / a1, 0.5f, 0.1f);
            }
        }

        const double sr = 48000.0;

        beginTest("TapeDelay: feedback sets how long the repeats last; none = one echo");
        {
            auto tail = [&](float fb)
            {
                TapeDelay fx;
                fx.setParameter(TapeDelay::Time, 0.2f);
                fx.setParameter(TapeDelay::Feedback, fb);
                fx.setParameter(TapeDelay::Age, 0.0f);
                fx.setParameter(TapeDelay::Mix, 1.0f);
                fx.prepare(sr, BLOCK);
                Buf l, r;
                // a 20 ms tone burst
                auto burst = [=](int i) { return i < 960 ? 0.5f * (float) std::sin(6.283185307179586 * 1000.0 * i / sr) : 0.0f; };
                render(fx, 48000 * 4, burst, burst, l, r);
                return std::array<float, 3> { rms(l, 9600, 11520), rms(l, 19200, 21120), rms(l, 172800, 174720) };
            };
            auto none = tail(0.0f), half = tail(0.5f), most = tail(0.9f);
            expect(none[0] > 0.1f, "the echo itself");
            expect(none[1] < none[0] * 1.0e-3f, "no second echo without feedback");
            expect(half[1] > half[0] * 0.3f && half[1] < half[0] * 0.7f, "about 0.55 per repeat at half feedback");
            expect(half[2] < half[0] * 1.0e-3f, "half feedback has died away after 3.6 s");
            expect(most[2] > half[2] * 100.0f && most[2] < most[0], "more feedback lasts longer but still decays");
        }

        beginTest("TapeDelay: maximum feedback self-oscillates at a bounded level");
        for (float ageValue : { 0.0f, 0.5f, 1.0f })
        {
            TapeDelay fx;
            fx.setParameter(TapeDelay::Time, 0.15f);
            fx.setParameter(TapeDelay::Feedback, 1.0f);
            fx.setParameter(TapeDelay::Age, ageValue);
            fx.setParameter(TapeDelay::Mix, 1.0f);
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, 48000, noise(1.0f, 21), noise(1.0f, 22), l, r);
            render(fx, 48000 * 10, silence(), silence(), l, r);
            expect(allFinite(l) && allFinite(r));
            float late = peak(l, 48000 * 9, 48000 * 11);
            expect(late < 1.5f, "bounded: " + juce::String(late));
            expect(late > 0.02f, "still sounding ten seconds on: " + juce::String(late));
        }

        beginTest("TapeDelay: AGE adds wow and flutter (pitch wobble), dulls and saturates the repeats");
        {
            auto echoOf = [&](float ageValue, double hz, float amp, float& spread)
            {
                TapeDelay fx;
                fx.setParameter(TapeDelay::Time, 0.3f);
                fx.setParameter(TapeDelay::Feedback, 0.0f);
                fx.setParameter(TapeDelay::Age, ageValue);
                fx.setParameter(TapeDelay::Mix, 1.0f);
                fx.prepare(sr, BLOCK);
                Buf l, r;
                render(fx, 48000 * 3, sine(hz, sr, amp), sine(hz, sr, amp), l, r);
                spread = 1200.0f * std::log2(periodSpread(l, 24000, 48000 * 3));
                return rms(l, 24000, 48000 * 3) / (amp * 0.7071f);
            };
            float cleanSpread, oldSpread, s;
            float cleanMid = echoOf(0.0f, 1000.0, 0.1f, cleanSpread);
            float oldMid = echoOf(1.0f, 1000.0, 0.1f, oldSpread);
            expect(cleanSpread < 1.0f, "new tape: steady pitch (" + juce::String(cleanSpread) + " cents)");
            expect(oldSpread > 10.0f && oldSpread < 80.0f, "old tape: wobbling pitch (" + juce::String(oldSpread) + " cents)");
            expect(cleanMid > 0.9f && cleanMid < 1.05f, "new tape repeats mid frequencies at full level");

            float cleanHigh = echoOf(0.0f, 6000.0, 0.1f, s);
            float oldHigh = echoOf(1.0f, 6000.0, 0.1f, s);
            expect(oldHigh < cleanHigh * 0.6f, "old tape is duller");
            float cleanLow = echoOf(0.0f, 60.0, 0.1f, s);
            float oldLow = echoOf(1.0f, 60.0, 0.1f, s);
            expect(oldLow < cleanLow * 0.6f, "and thinner");

            float cleanLoud = echoOf(0.0f, 1000.0, 0.9f, s);
            float oldLoud = echoOf(1.0f, 1000.0, 0.9f, s);
            expect(oldLoud < cleanLoud * 0.7f && oldLoud < oldMid * 0.6f, "and compresses loud signals harder");
        }

        beginTest("TapeDelay: changing TIME while running glides without a jump");
        {
            TapeDelay fx;
            fx.setParameter(TapeDelay::Time, 0.2f);
            fx.setParameter(TapeDelay::Feedback, 0.0f);
            fx.setParameter(TapeDelay::Age, 0.0f);
            fx.setParameter(TapeDelay::Mix, 1.0f);
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, 24000, sine(200.0, sr, 0.3f), sine(200.0, sr, 0.3f), l, r);
            fx.setParameter(TapeDelay::Time, 0.25f);
            render(fx, 48000, sine(200.0, sr, 0.3f), sine(200.0, sr, 0.3f), l, r);
            float biggestStep = 0.0f;
            for (size_t i = 12000; i < l.size(); ++i)
                biggestStep = std::max(biggestStep, std::abs(l[i] - l[i - 1]));
            // a 0.3-amplitude 200 Hz sine moves 0.008 per sample; the glide may double its pitch
            expect(biggestStep < 0.02f, "no click: " + juce::String(biggestStep));
            expectWithinAbsoluteError(rms(l, 60000, 72000), 0.3f * 0.7071f, 0.02f);
        }
    }

    // ---- L/C/R delay ---------------------------------------------------------------------------

    void testLCRDelay()
    {
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            beginTest("LCRDelay: left, centre and right taps at their times and sides, at " + juce::String((int) sr));
            for (float spread : { 1.0f, 0.5f })
            {
                LCRDelay fx;
                fx.setParameter(LCRDelay::Time, 0.2f);
                fx.setParameter(LCRDelay::Feedback, 0.0f);
                fx.setParameter(LCRDelay::Spread, spread);
                fx.setParameter(LCRDelay::Mix, 1.0f);
                fx.prepare(sr, BLOCK);
                Buf l, r;
                render(fx, (int) (0.4 * sr), impulse(1.0f), impulse(1.0f), l, r);

                int tl = (int) std::lround(fx.getLeftTime() * sr);
                int tc = (int) std::lround(fx.getCentreTime() * sr);
                int tr = (int) std::lround(fx.getRightTime() * sr);
                expectEquals(tc, (int) std::lround(0.2 * sr));
                expectEquals(tl, (int) std::lround(0.2 * (1.0 - 0.5 * spread) * sr));
                expectEquals(tr, (int) std::lround(0.2 * (1.0 + 0.5 * spread) * sr));

                const int w = 3;
                expect(peak(l, tl - w, tl + w) > 0.9f, "left tap on the left");
                expect(peak(r, tl - w, tl + w) < 1.0e-4f, "left tap not on the right");
                expectWithinAbsoluteError(peak(l, tc - w, tc + w), 0.707f, 0.05f);
                expectWithinAbsoluteError(peak(r, tc - w, tc + w), 0.707f, 0.05f);
                expect(peak(r, tr - w, tr + w) > 0.9f, "right tap on the right");
                expect(peak(l, tr - w, tr + w) < 1.0e-4f, "right tap not on the left");

                // nothing anywhere else
                Buf rest = l;
                for (size_t i = 0; i < rest.size(); ++i)
                {
                    rest[i] = std::max(std::abs(l[i]), std::abs(r[i]));
                    for (int t : { tl, tc, tr })
                        if (std::abs((int) i - t) <= w) rest[i] = 0.0f;
                }
                expect(peak(rest, 0, (int) rest.size()) < 1.0e-3f, "silent between the taps");
            }
        }

        const double sr = 48000.0;

        beginTest("LCRDelay: spread 0 puts all three taps together");
        {
            LCRDelay fx;
            fx.setParameter(LCRDelay::Time, 0.1f);
            fx.setParameter(LCRDelay::Feedback, 0.0f);
            fx.setParameter(LCRDelay::Spread, 0.0f);
            fx.setParameter(LCRDelay::Mix, 1.0f);
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, 9600, impulse(1.0f), impulse(1.0f), l, r);
            expectWithinAbsoluteError(peak(l, 4797, 4803), 1.707f, 0.05f);
            expectWithinAbsoluteError(peak(r, 4797, 4803), 1.707f, 0.05f);
            expect(peak(l, 0, 4790) < 1.0e-4f && peak(l, 4810, 9600) < 1.0e-3f);
        }

        beginTest("LCRDelay: feedback repeats the whole pattern every TIME and decays");
        {
            auto levels = [&](float fb)
            {
                LCRDelay fx;
                fx.setParameter(LCRDelay::Time, 0.2f);
                fx.setParameter(LCRDelay::Feedback, fb);
                fx.setParameter(LCRDelay::Spread, 1.0f);
                fx.setParameter(LCRDelay::Mix, 1.0f);
                fx.prepare(sr, BLOCK);
                Buf l, r;
                auto burst = [=](int i) { return i < 960 ? 0.5f * (float) std::sin(6.283185307179586 * 800.0 * i / sr) : 0.0f; };
                render(fx, 48000 * 8, burst, burst, l, r);
                expect(allFinite(l) && allFinite(r));
                // left tap windows: first pass (0.1 s), second pass (0.3 s), third (0.5 s), and late
                return std::array<float, 4> { rms(l, 4800, 5760), rms(l, 14400, 15360), rms(l, 24000, 24960),
                                              rms(l, 48000 * 7, 48000 * 8) };
            };
            auto none = levels(0.0f), half = levels(0.5f), most = levels(0.95f);
            expect(none[0] > 0.2f);
            expect(none[1] < 1.0e-4f, "no repeats without feedback");
            expectWithinAbsoluteError(half[1] / half[0], 0.5f, 0.08f);
            expectWithinAbsoluteError(half[2] / half[1], 0.5f, 0.08f);
            expect(half[3] < 1.0e-6f, "half feedback is gone after seven seconds");
            expect(most[1] > half[1] * 1.5f, "more feedback, louder repeats");
            expect(most[3] < most[1] * 0.5f, "maximum feedback still decays");
        }
    }

    // ---- multi-tap adapter ---------------------------------------------------------------------

    void testMultiTapInsert()
    {
        const double sr = 48000.0;

        auto impulseResponse = [&](int taps, float time, float fb, Buf& l, Buf& r, double seconds)
        {
            auto fx = std::make_unique<MultiTapInsert>();
            fx->setParameter(MultiTapInsert::Time, time);
            fx->setParameter(MultiTapInsert::Taps, (float) taps);
            fx->setParameter(MultiTapInsert::Feedback, fb);
            fx->setParameter(MultiTapInsert::Mix, 1.0f);
            fx->prepare(sr, BLOCK);
            render(*fx, (int) (seconds * sr), impulse(1.0f), impulse(1.0f), l, r);
        };

        beginTest("MultiTapInsert: TAPS evenly spaced echoes up to TIME, alternating sides, getting quieter");
        for (int taps : { 1, 2, 4, 8 })
        {
            Buf l, r;
            impulseResponse(taps, 0.4f, 0.0f, l, r, 0.5);
            Buf both(l.size());
            for (size_t i = 0; i < l.size(); ++i)
                both[i] = std::abs(l[i]) + std::abs(r[i]);

            float previous = 10.0f;
            for (int i = 0; i < taps; ++i)
            {
                int t = (int) std::lround(0.4 * sr * (i + 1) / taps);
                float pl = peak(l, t - 3, t + 3), pr = peak(r, t - 3, t + 3);
                expect(pl + pr > 0.3f, juce::String(taps) + " taps: echo " + juce::String(i + 1) + " present");
                if (taps > 1)
                    expect(i % 2 == 0 ? pl > pr * 3.0f : pr > pl * 3.0f, "alternating sides");
                else
                    expectWithinAbsoluteError(pl, pr, 1.0e-4f);
                expect(pl + pr <= previous + 1.0e-4f, "not louder than the one before");
                previous = pl + pr;
                for (int k = t - 3; k < t + 3; ++k)
                    both[(size_t) k] = 0.0f;
            }
            expect(peak(both, 0, (int) both.size()) < 1.0e-3f, juce::String(taps) + " taps: nothing between the echoes");
        }

        beginTest("MultiTapInsert: feedback regenerates the pattern and always decays");
        {
            auto energy = [&](float fb, int startMs, int endMs)
            {
                auto fx = std::make_unique<MultiTapInsert>();
                fx->setParameter(MultiTapInsert::Time, 0.2f);
                fx->setParameter(MultiTapInsert::Taps, 8.0f);
                fx->setParameter(MultiTapInsert::Feedback, fb);
                fx->setParameter(MultiTapInsert::Mix, 1.0f);
                fx->prepare(sr, BLOCK);
                Buf l, r;
                render(*fx, 12000, noise(0.5f, 31), noise(0.5f, 32), l, r);
                render(*fx, 48000 * 6, silence(), silence(), l, r);
                expect(allFinite(l) && allFinite(r));
                return rms(l, startMs * 48, endMs * 48);
            };
            expect(energy(0.0f, 600, 1000) < 1.0e-5f, "no feedback: silent once the taps have passed");
            expect(energy(0.6f, 600, 1000) > 1.0e-3f, "feedback: still echoing");
            float early = energy(0.9f, 600, 1000), late = energy(0.9f, 5000, 6000);
            expect(late < early * 0.1f, "maximum feedback decays: " + juce::String(late / early));
        }

        beginTest("MultiTapInsert: turning TIME glides instead of jumping");
        {
            auto fx = std::make_unique<MultiTapInsert>();
            fx->setParameter(MultiTapInsert::Time, 0.2f);
            fx->setParameter(MultiTapInsert::Taps, 1.0f);
            fx->setParameter(MultiTapInsert::Feedback, 0.0f);
            fx->setParameter(MultiTapInsert::Mix, 1.0f);
            fx->prepare(sr, BLOCK);
            Buf l, r;
            render(*fx, 24000, sine(200.0, sr, 0.3f), sine(200.0, sr, 0.3f), l, r);
            fx->setParameter(MultiTapInsert::Time, 0.23f);
            render(*fx, 48000, sine(200.0, sr, 0.3f), sine(200.0, sr, 0.3f), l, r);
            float biggestStep = 0.0f;
            for (size_t i = 12000; i < l.size(); ++i)
                biggestStep = std::max(biggestStep, std::abs(l[i] - l[i - 1]));
            expect(biggestStep < 0.02f, "no click: " + juce::String(biggestStep));
        }
    }

    // ---- sidechain adapter ---------------------------------------------------------------------

    void testSidechainInsert()
    {
        const double sr = 48000.0;

        // Renders a quiet 300 Hz tone through the effect; when keyAmp > 0 a loud key signal is
        // handed over as the sidechain for every block.
        auto run = [&](SidechainInsert& fx, int n, float keyAmp, Buf& out, bool giveKey)
        {
            float l[BLOCK], r[BLOCK], kl[BLOCK], kr[BLOCK];
            uint32_t seed = 77;
            int done = 0;
            while (done < n)
            {
                int count = std::min(BLOCK, n - done);
                for (int i = 0; i < count; ++i)
                {
                    int idx = (int) out.size() + i;
                    l[i] = r[i] = 0.05f * (float) std::sin(6.283185307179586 * 300.0 * idx / sr);
                    kl[i] = kr[i] = keyAmp * dsp::xorshiftNoise(seed);
                }
                if (giveKey)
                    fx.setSidechainBuffers(kl, kr);
                fx.processStereo(l, r, count);
                out.insert(out.end(), l, l + count);
                done += count;
            }
        };

        const float dryRms = 0.05f * 0.7071f;

        beginTest("SidechainInsert: a quiet track is left alone on its own, ducked by a loud source track");
        {
            SidechainInsert fx;
            fx.setParameter(SidechainInsert::Threshold, -20.0f);
            fx.setParameter(SidechainInsert::Ratio, 10.0f);
            fx.prepare(sr, BLOCK);
            expectEquals(fx.getSidechainTrack(), -1);

            Buf self;
            run(fx, 24000, 0.9f, self, false);
            expectWithinAbsoluteError(rms(self, 12000, 24000), dryRms, dryRms * 0.02f);

            // SELF ignores a buffer it is handed
            Buf selfWithKey;
            run(fx, 24000, 0.9f, selfWithKey, true);
            expectWithinAbsoluteError(rms(selfWithKey, 12000, 24000), dryRms, dryRms * 0.02f);

            fx.setParameter(SidechainInsert::Source, 2.0f);
            expectEquals(fx.getSidechainTrack(), 1);
            Buf ducked;
            run(fx, 24000, 0.9f, ducked, true);
            float duckedRms = rms(ducked, 12000, 24000);
            expect(duckedRms < dryRms * 0.35f, "ducked by " + juce::String(toDb(duckedRms / dryRms)) + " dB");
            expect(fx.getGainReductionDb() < -8.0f);

            // a quiet source does not duck
            fx.reset();
            Buf quietKey;
            run(fx, 24000, 0.01f, quietKey, true);
            expectWithinAbsoluteError(rms(quietKey, 12000, 24000), dryRms, dryRms * 0.02f);
        }

        beginTest("SidechainInsert: the handed-over buffer is used for one block only, then the gain recovers");
        {
            SidechainInsert fx;
            fx.setParameter(SidechainInsert::Source, 1.0f);
            fx.setParameter(SidechainInsert::Threshold, -20.0f);
            fx.setParameter(SidechainInsert::Ratio, 10.0f);
            fx.setParameter(SidechainInsert::Release, 50.0f);
            fx.prepare(sr, BLOCK);
            Buf out;
            run(fx, 24000, 0.9f, out, true);
            expect(rms(out, 12000, 24000) < dryRms * 0.35f);
            run(fx, 48000, 0.9f, out, false);   // key no longer supplied
            expectWithinAbsoluteError(rms(out, 60000, 72000), dryRms, dryRms * 0.02f);
        }

        beginTest("SidechainInsert: threshold, ratio and release shape the ducking");
        {
            auto duckDb = [&](float thr, float ratioValue)
            {
                SidechainInsert fx;
                fx.setParameter(SidechainInsert::Source, 1.0f);
                fx.setParameter(SidechainInsert::Threshold, thr);
                fx.setParameter(SidechainInsert::Ratio, ratioValue);
                fx.prepare(sr, BLOCK);
                Buf out;
                run(fx, 24000, 0.9f, out, true);
                return toDb(rms(out, 12000, 24000) / dryRms);
            };
            expect(duckDb(-40.0f, 10.0f) < duckDb(-20.0f, 10.0f) - 6.0f, "lower threshold ducks more");
            expect(duckDb(-20.0f, 10.0f) < duckDb(-20.0f, 2.0f) - 3.0f, "higher ratio ducks more");
            expectWithinAbsoluteError(duckDb(-20.0f, 1.0f), 0.0f, 0.2f);

            auto recoveredAfter100ms = [&](float releaseMs)
            {
                SidechainInsert fx;
                fx.setParameter(SidechainInsert::Source, 1.0f);
                fx.setParameter(SidechainInsert::Threshold, -30.0f);
                fx.setParameter(SidechainInsert::Ratio, 20.0f);
                fx.setParameter(SidechainInsert::Release, releaseMs);
                fx.prepare(sr, BLOCK);
                Buf out;
                run(fx, 24000, 0.9f, out, true);
                run(fx, 9600, 0.0f, out, true);   // the source goes silent
                return rms(out, 24000 + 4800, 24000 + 5760) / dryRms;
            };
            expect(recoveredAfter100ms(20.0f) > 0.9f, "short release: back up within 100 ms");
            expect(recoveredAfter100ms(1000.0f) < 0.5f, "long release: still ducked after 100 ms");
        }

        beginTest("SidechainInsert: SELF compresses the track's own loud signal");
        {
            SidechainInsert fx;
            fx.setParameter(SidechainInsert::Threshold, -30.0f);
            fx.setParameter(SidechainInsert::Ratio, 10.0f);
            fx.prepare(sr, BLOCK);
            Buf l, r;
            render(fx, 24000, sine(300.0, sr, 0.8f), sine(300.0, sr, 0.8f), l, r);
            expect(rms(l, 12000, 24000) < 0.8f * 0.7071f * 0.25f);
        }
    }
};
static EffectsMoreTests EffectsMoreTestsInstance;
