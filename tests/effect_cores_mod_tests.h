#pragma once
#include <juce_core/juce_core.h>

#include "../src/engine/AudioEngine.h"
#include "../src/dsp/effects/ModDelay.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

// The modulation effects on their shared pieces (phase E1 of .ralph/spec-unify.md): the modulated
// delay (ModDelay.h) behind chorus, ensemble, flanger, vibrato and the rotary speaker; dsp/LFO.h as
// the LFO of every one of them and of tremolo, auto-pan, phaser, wah, ring modulator and MODFX; and
// the one interface that moves their cells from outside (modulateCell).
// And the one mechanism under all of it and under the delays, reverbs, dynamics and equalisers of
// phases E2 and E3: CellEffect (CellEffect.h), its clock, its mix and its modulation arithmetic.
// Levels are measured by RMS (a moving delay smears a narrow band).
// NEPTUNE_FX_TIME=1 prints the cost table instead of being quiet about it.
class EffectCoresModTests : public juce::UnitTest
{
public:
    EffectCoresModTests() : UnitTest("Effect Cores Mod") {}

    void runTest() override
    {
        lfoAdditions();
        coreDelay();
        coreVoicesFeedbackStereo();
        coreGlideAndShapes();
        typesAndShapes();
        whatEachGained();
        modulation();
        cellsUnderSound();
        anyInput();
        blockSizes();
        modFxBlock();
        dirtStages();
        review();
        oneMechanism();
        timing();
    }

private:
    using Buf = std::vector<float>;
    using Gen = std::function<float(int, int)>;     // (sample, side)
    struct Audio { Buf l, r; };

    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;
    static constexpr double TWO_PI = 6.283185307179586;

    // ---- signals ---------------------------------------------------------------------------------
    static Gen sine(double hz, float amp, double sr = SR)
    {
        return [=](int i, int) { return amp * (float) std::sin(TWO_PI * hz * (double) i / sr); };
    }
    // A low and a high partial (the rotary's two bands both get something).
    static Gen chord(double sr = SR)
    {
        return [=](int i, int) { return 0.3f * (float) std::sin(TWO_PI * 220.0 * i / sr) + 0.2f * (float) std::sin(TWO_PI * 1320.0 * i / sr); };
    }
    static float hashed(uint32_t x)
    {
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return (float) (x & 0xffffff) / 8388608.0f - 1.0f;
    }
    static Gen noise(float amp, uint32_t seed = 1, bool sameBothSides = false)
    {
        return [=](int i, int side) { return amp * hashed((uint32_t) i * 2u + (sameBothSides ? 0u : (uint32_t) side) + seed * 7919u); };
    }
    static Gen impulse() { return [](int i, int) { return i == 0 ? 1.0f : 0.0f; }; }
    static Gen leftOnly(Gen g) { return [g](int i, int side) { return side == 0 ? g(i, 0) : 0.0f; }; }

    using Process = std::function<void(float*, float*, int)>;
    static Audio renderWith(const Process& process, int n, const Gen& in, int block = BLOCK, const std::function<void(int)>& before = nullptr)
    {
        Audio a;
        a.l.resize((size_t) n);
        a.r.resize((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            a.l[(size_t) i] = in(i, 0);
            a.r[(size_t) i] = in(i, 1);
        }
        for (int done = 0, b = 0; done < n; done += block, ++b)
        {
            if (before) before(b);
            process(a.l.data() + done, a.r.data() + done, std::min(block, n - done));
        }
        return a;
    }
    // The core with a mix of its own, as an effect hands it one.
    struct MixedCore : moddelay::Core
    {
        float mix = 1.0f;
        void setMix(float m) { mix = m; }
    };
    static Audio render(EffectBase& fx, int n, const Gen& in, int block = BLOCK, const std::function<void(int)>& before = nullptr)
    {
        return renderWith([&fx](float* l, float* r, int count) { fx.processBlock(l, r, count); }, n, in, block, before);
    }
    static Audio render(moddelay::Core& core, int n, const Gen& in, int block = BLOCK, const std::function<void(int)>& before = nullptr)
    {
        return renderWith([&core](float* l, float* r, int count) { core.process(l, r, count); }, n, in, block, before);
    }
    static Audio render(MixedCore& core, int n, const Gen& in, int block = BLOCK, const std::function<void(int)>& before = nullptr)
    {
        return renderWith([&core](float* l, float* r, int count) { core.process(l, r, count, core.mix, core.mix); }, n, in, block, before);
    }

    static float rms(const Buf& b, int from = 0, int to = -1)
    {
        if (to < 0) to = (int) b.size();
        double sum = 0.0;
        for (int i = from; i < to; ++i)
            sum += (double) b[(size_t) i] * b[(size_t) i];
        return to > from ? (float) std::sqrt(sum / (to - from)) : 0.0f;
    }
    static float rmsDiff(const Buf& a, const Buf& b, int from = 0)
    {
        double sum = 0.0;
        const int n = (int) std::min(a.size(), b.size());
        for (int i = from; i < n; ++i)
            sum += (double) (a[(size_t) i] - b[(size_t) i]) * (a[(size_t) i] - b[(size_t) i]);
        return n > from ? (float) std::sqrt(sum / (n - from)) : 0.0f;
    }
    static float difference(const Audio& a, const Audio& b, int from = 0) { return std::max(rmsDiff(a.l, b.l, from), rmsDiff(a.r, b.r, from)); }
    static float maxDiff(const Audio& a, const Audio& b)
    {
        float m = 0.0f;
        for (size_t i = 0; i < std::min(a.l.size(), b.l.size()); ++i)
            m = std::max({ m, std::abs(a.l[i] - b.l[i]), std::abs(a.r[i] - b.r[i]) });
        return m;
    }
    static bool finite(const Audio& a, float bound = 1.0e30f)
    {
        for (auto* ch : { &a.l, &a.r })
            for (float v : *ch)
                if (! std::isfinite(v) || std::abs(v) > bound)
                    return false;
        return true;
    }
    static float amplitudeAt(const Buf& b, int from, int to, double hz, double sr = SR)
    {
        double re = 0.0, im = 0.0;
        for (int i = from; i < to; ++i)
        {
            const double ph = TWO_PI * hz * (double) i / sr;
            re += b[(size_t) i] * std::cos(ph);
            im += b[(size_t) i] * std::sin(ph);
        }
        return (float) (2.0 * std::sqrt(re * re + im * im) / (double) (to - from));
    }
    // The largest step from one sample to the next inside the windows that start at `edges` against
    // the largest outside them (the click audit's measure and its limits).
    struct Steps
    {
        float atEdges = 0.0f, elsewhere = 0.0f, peak = 0.0f;
        bool clicks() const { return atEdges > 1.5f * elsewhere + 0.03f * peak; }
        juce::String text() const { return juce::String(atEdges, 4) + " at the change, " + juce::String(elsewhere, 4) + " elsewhere, peak " + juce::String(peak, 3); }
    };
    static Steps steps(const Audio& a, const std::vector<int>& edges, int window, int from)
    {
        Steps res;
        for (auto* ch : { &a.l, &a.r })
            for (int i = std::max(1, from); i < (int) ch->size(); ++i)
            {
                const float step = std::abs((*ch)[(size_t) i] - (*ch)[(size_t) i - 1]);
                bool atEdge = false;
                for (int e : edges)
                    atEdge = atEdge || (i >= e - 4 && i < e + window);
                float& worst = atEdge ? res.atEdges : res.elsewhere;
                worst = std::max(worst, step);
                res.peak = std::max(res.peak, std::abs((*ch)[(size_t) i]));
            }
        return res;
    }

    // ---- the effects -----------------------------------------------------------------------------
    struct Kind
    {
        EffectType type;
        int number;                     // its stored number
        const char* name;
        std::vector<int> shapeCells;    // cells 0-3 the four, 4-7 the extras
        std::array<float, 8> lively;    // a setting in which every cell is heard
    };
    static const std::vector<Kind>& kinds()
    {
        static const std::vector<Kind> k = {
            { EffectType::ChorusEffect,   0,  "CHR",  { 4 },    { 2.0f, 0.8f, 0.5f, 3.0f,   0.0f, 0.3f, 0.5f, 0.007f } },
            { EffectType::EnsembleEffect, 11, "ENS",  { 4 },    { 1.5f, 0.8f, 0.8f, 0.5f,   1.0f, 0.3f, 3.0f, 0.005f } },
            { EffectType::FlangerEffect,  2,  "FLG",  { 4 },    { 2.0f, 0.7f, 0.5f, 0.5f,   0.0f, 0.5f, 0.0f, 0.003f } },
            { EffectType::VibratoEffect,  22, "VIB",  { 4 },    { 6.0f, 60.0f, 0.5f, 0.0f,  0.0f, 0.0f, 0.0f, 0.0f } },
            { EffectType::RotaryEffect,   13, "ROT",  { 4 },    { 1.0f, 7.0f, 1.5f, 0.5f,   0.0f, 0.5f, 800.0f, 0.0f } },
            { EffectType::TremoloEffect,  4,  "TRM",  { 2 },    { 6.0f, 0.8f, 0.0f, 0.25f,  0.0f, 0.0f, 0.0f, 0.0f } },
            { EffectType::AutoPanEffect,  12, "PAN",  { 2 },    { 6.0f, 0.8f, 0.0f, 0.0f,   0.0f, 0.0f, 0.0f, 0.0f } },
            { EffectType::PhaserEffect,   3,  "PHS",  { 4 },    { 2.0f, 0.8f, 0.5f, 4.0f,   0.0f, 0.5f, 0.0f, 0.0f } },
            { EffectType::WahEffect,      23, "WAH",  { 4 },    { 600.0f, 0.6f, 0.3f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f } },
            { EffectType::RingModEffect,  21, "RING", { 4, 5 }, { 300.0f, 3.0f, 0.5f, 0.8f, 0.0f, 0.0f, 0.0f, 0.0f } } };
        return k;
    }

    static void setCell(EffectBase& fx, int cell, float v)
    {
        if (cell < 4) fx.setParameter(cell, v);
        else if (cell < 8) fx.setExtra(cell - 4, v);
        else fx.setMix(v);
    }
    static float getCell(EffectBase& fx, int cell)
    {
        return cell < 4 ? fx.getParameter(cell) : cell < 8 ? fx.getExtra(cell - 4) : fx.getMix();
    }
    // An effect of cells as modulation sees it: everything goes in through EffectBase::modulateCell.
    struct Target
    {
        CellEffect* cells;
        ParamSpec cellSpec(int cell) const { return cells->cellSpec(cell); }
        bool modulates(int cell) const { return cells->moves(cell); }
        void modulate(int cell, float offset) { static_cast<EffectBase*> (cells)->modulateCell(cell, offset); }
        float modulation(int cell) const { return cells->getModulation(cell); }
        float cellValue(int cell) const { return cells->getMovedCell(cell); }
        void setUserShape(const float* levels) { cells->setUserShape(levels); }
        void clearModulation() { for (int cell = 0; cell < CellEffect::TARGETS; ++cell) modulate(cell, 0.0f); }
    };
    static CellEffect* cellsOf(EffectBase* fx) { return dynamic_cast<CellEffect*> (fx); }
    static Target target(EffectBase& fx) { return { cellsOf(&fx) }; }


    static std::unique_ptr<EffectBase> make(const Kind& k, double sr = SR, int block = BLOCK)
    {
        auto fx = AudioEngine::createEffect(k.type);
        fx->prepare(sr, block);
        for (int cell = 0; cell < 8; ++cell)
            if (target(*fx).cellSpec(cell).isValid())
                setCell(*fx, cell, k.lively[(size_t) cell]);
        fx->reset();
        return fx;
    }

    static MixedCore core(int voices, float centreSeconds, float swingSeconds, bool mono = false)
    {
        MixedCore c;
        c.setVoices(voices);
        c.setRange(0.0f, 0.03f);
        c.setDelay(0.0f, centreSeconds, swingSeconds);
        c.setRate(2.0f);
        c.prepare(SR, 0.035, mono);
        return c;
    }

    // ---- 1 ---------------------------------------------------------------------------------------
    void lfoAdditions()
    {
        beginTest("dsp/LFO.h: shapeAt is the same LFO read further on in its cycle; setSeed sets the random shapes apart");
        for (int s = 0; s < (int) LFO::Shape::COUNT; ++s)
        {
            const auto shape = (LFO::Shape) s;
            if (shape == LFO::Shape::Random || shape == LFO::Shape::SmoothRandom)
                continue;
            LFO a, b;
            for (auto* l : { &a, &b })
            {
                l->setSampleRate(1000.0);
                l->setRate(3.7f);
                l->setShape(shape);
            }
            b.setStartPhase(0.3f);
            float worst = 0.0f;
            for (int i = 0; i < 2000; ++i)
            {
                a.process();
                const float ahead = b.process();
                worst = std::max(worst, std::abs(a.shapeAt(0.3f) - ahead));
            }
            expect(worst < 1.0e-4f, juce::String(LFO::shapeLabels()[s]) + ": shapeAt(0.3) against an LFO started 0.3 on: " + juce::String(worst, 6));
        }
        {
            LFO a, b, c;
            for (auto* l : { &a, &b, &c })
            {
                l->setSampleRate(100.0);
                l->setRate(10.0f);
                l->setShape(LFO::Shape::Random);
            }
            b.setSeed(999u);
            float same = 0.0f, other = 0.0f;
            for (int i = 0; i < 400; ++i)
            {
                const float va = a.process();
                other = std::max(other, std::abs(va - b.process()));
                same = std::max(same, std::abs(va - c.process()));
            }
            expect(same == 0.0f, "the same seed draws the same values");
            expect(other > 0.2f, "another seed draws others");
        }

        beginTest("fxm::Lfo: the LFO at a control rate with straight lines between follows the shape; the second reader is ahead");
        {
            fxm::Lfo lfo;
            lfo.prepare(SR);
            lfo.setRate(5.0f);
            lfo.setOffset(0.25f);
            expectEquals((int) fxm::Lfo::INTERVAL, (int) CellEffect::CONTROL);
            float worstA = 0.0f, worstB = 0.0f, top = 0.0f;
            for (int i = 0; i < 48000; ++i)
            {
                float a, b;
                lfo.next(a, b);
                // (One step late: the line is drawn towards the value just taken.)
                if (i < 32) continue;       // (the first step holds its first value)
                const double t = TWO_PI * 5.0 * (double) (i + 1) / SR;
                worstA = std::max(worstA, std::abs(a - (float) std::sin(t)));
                worstB = std::max(worstB, std::abs(b - (float) std::cos(t)));
                top = std::max(top, a);
            }
            expect(worstA < 0.002f && worstB < 0.002f, "sine and its quarter-turn: " + juce::String(worstA, 5) + " " + juce::String(worstB, 5));
            expect(top > 0.999f);

            // A shape that jumps arrives over one step, not in one sample.
            fxm::Lfo square;
            square.prepare(SR);
            square.setRate(20.0f);
            square.setShape((int) LFO::Shape::Square);
            float biggest = 0.0f, last = square.next();
            for (int i = 0; i < 48000; ++i)
            {
                const float v = square.next();
                biggest = std::max(biggest, std::abs(v - last));
                last = v;
            }
            expectWithinAbsoluteError(biggest, 2.0f / 32.0f, 0.001f);
        }
    }

    // ---- 2 ---------------------------------------------------------------------------------------
    void coreDelay()
    {
        beginTest("Core: a whole number of samples is exact, a fraction is the Hermite read (good at 5 kHz)");
        {
            moddelay::Core c;
            c.setVoices(1);
            c.setRange(0.0f, 0.02f);
            c.setDelay(2.0f, 0.0f, 0.0f);
            c.prepare(SR, 0.03, false);
            const Audio dry = renderWith([](float*, float*, int) {}, 20000, noise(0.5f));
            const Audio out = render(c, 20000, noise(0.5f));
            bool same = true;
            for (size_t i = 2; i < out.l.size(); ++i)
                same = same && out.l[i] == dry.l[i - 2] && out.r[i] == dry.r[i - 2];
            expect(same, "two samples late, bit for bit");
        }
        {
            MixedCore c = core(1, 10.5f / (float) SR, 0.0f);
            const Audio out = render(c, 9600, sine(5000.0, 0.5f));
            float worst = 0.0f;
            for (int i = 4800; i < 9600; ++i)
                worst = std::max(worst, std::abs(out.l[(size_t) i] - 0.5f * (float) std::sin(TWO_PI * 5000.0 * ((double) i - 10.5) / SR)));
            logMessage("5 kHz read half way between samples: worst error " + juce::String(worst, 5) + " of 0.5 (a straight line: 0.013)");
            expect(worst < 0.005f, "Hermite read at 10.5 samples: " + juce::String(worst, 5));
        }

        beginTest("Core: mix is dry (1 - mix) + wet mix; one line takes the middle of the input to both sides");
        {
            MixedCore c = core(1, 0.005f, 0.0f);
            c.setMix(0.0f);
            const Audio dry = renderWith([](float*, float*, int) {}, 4800, noise(0.5f));
            expect(maxDiff(render(c, 4800, noise(0.5f)), dry) == 0.0f, "mix 0 is the input");

            MixedCore mono = core(1, 0.005f, 0.0f, true);
            const Audio out = render(mono, 9600, leftOnly(noise(0.5f)));
            expect(rms(out.r, 2400) > 0.1f, "the left input comes out on the right");
            expect(rmsDiff(out.l, out.r) < 1.0e-6f);
            MixedCore stereo = core(1, 0.005f, 0.0f);
            expect(rms(render(stereo, 9600, leftOnly(noise(0.5f))).r) == 0.0f, "two lines keep the sides apart");
        }
    }

    // ---- 3 ---------------------------------------------------------------------------------------
    void coreVoicesFeedbackStereo()
    {
        beginTest("Core: one to six voices; the level stays; each has its own place in the cycle and its own rate");
        for (int n = 1; n <= moddelay::MAX_VOICES; ++n)
        {
            MixedCore still = core(n, 0.005f, 0.0f);
            const Audio hit = render(still, 480, impulse());
            expectWithinAbsoluteError(hit.l[240], 1.0f, 1.0e-4f);     // every voice reads the same place: together they are the input

            MixedCore c = core(n, 0.008f, 0.004f);
            c.setSpread(0.1f, 1.0f, 0.0f);
            render(c, 17760, noise(0.2f));
            for (int v = 0; v < n; ++v)
            {
                const float d = c.tapDelay(v, 0) / (float) SR;
                expect(d >= 0.004f - 1.0e-4f && d <= 0.012f + 1.0e-4f, "voice " + juce::String(v) + " inside the swing");
                for (int w = 0; w < v; ++w)
                    expect(std::abs(c.tapDelay(v, 0) - c.tapDelay(w, 0)) > 1.0f,
                           juce::String(n) + " voices: " + juce::String(v) + " and " + juce::String(w) + " read different places");
            }
        }
        {
            // The voices are set apart in rate too: two taps that start a third of a cycle apart do not stay so.
            MixedCore c = core(3, 0.008f, 0.004f);
            c.setSpread(0.5f, 1.0f, 0.0f);
            render(c, 4800, noise(0.2f));
            const float early = c.tapDelay(1, 0) - c.tapDelay(0, 0);
            render(c, 36000, noise(0.2f));
            expect(std::abs((c.tapDelay(1, 0) - c.tapDelay(0, 0)) - early) > 20.0f, "the second voice runs at its own rate");
        }

        beginTest("Core: feedback of either polarity repeats the tap, and stays bounded at its top");
        for (float fb : { 0.5f, -0.5f })
        {
            MixedCore c = core(1, 0.005f, 0.0f);
            c.setFeedback(fb);
            const Audio out = render(c, 1200, impulse());
            expectWithinAbsoluteError(out.l[240], 1.0f, 1.0e-4f);
            expectWithinAbsoluteError(out.l[480], fb, 1.0e-3f);
            expectWithinAbsoluteError(out.l[720], fb * fb, 1.0e-3f);
        }
        for (int n : { 1, 6 })
        {
            MixedCore c = core(n, 0.005f, 0.003f);
            c.setFeedback(5.0f);        // held to the top
            const Audio loud = render(c, 96000, noise(1.0f));
            expect(finite(loud, 80.0f), "bounded with noise at full level");
            const Audio after = render(c, 48000 * 6, [](int, int) { return 0.0f; });
            expect(rms(after.l, 48000 * 5) < 1.0e-3f, "and it dies away: " + juce::String(rms(after.l, 48000 * 5), 6));
        }

        beginTest("Core: stereo sets the right side's LFO ahead; pan spread sets the voices apart; a rotor's level follows a quarter turn on");
        {
            MixedCore alike = core(2, 0.006f, 0.003f), apart = core(2, 0.006f, 0.003f);
            apart.setStereo(0.25f);
            const Audio a = render(alike, 48000, noise(0.3f, 1, true)), b = render(apart, 48000, noise(0.3f, 1, true));
            expect(rmsDiff(a.l, a.r) == 0.0f, "no offset: both sides alike");
            expect(rmsDiff(b.l, b.r) > 0.05f, "a quarter cycle: the sides differ");
            expect(rmsDiff(a.l, b.l) < 1.0e-6f, "the left side is the same either way");

            MixedCore wide = core(3, 0.006f, 0.003f, true);
            wide.setSpread(0.13f, 1.0f, 0.7f);
            const Audio w = render(wide, 48000, noise(0.3f, 1, true));
            expect(rmsDiff(w.l, w.r) > 0.05f, "voices panned apart");
            wide.setWidth(0.0f);
            wide.reset();
            const Audio narrow = render(wide, 48000, noise(0.3f, 1, true));
            expect(rmsDiff(narrow.l, narrow.r) < 1.0e-6f, "width 0: all in the middle");
        }
        {
            MixedCore rotor = core(1, 0.002f, 0.0f, true);
            rotor.setLevelFollow(0.3f);
            const Audio out = render(rotor, 48000, [](int, int) { return 0.5f; });
            float lo = 1.0f, hi = 0.0f, sum = 0.0f;
            for (int i = 4800; i < 48000; ++i)
            {
                lo = std::min(lo, out.l[(size_t) i]);
                hi = std::max(hi, out.l[(size_t) i]);
                sum = std::max(sum, std::abs(out.l[(size_t) i] + out.r[(size_t) i] - 0.7f));
            }
            expectWithinAbsoluteError(hi, 0.5f, 0.002f);
            expectWithinAbsoluteError(lo, 0.2f, 0.002f);
            expect(sum < 0.002f, "what one side loses the other has");
        }
    }

    // ---- 4 ---------------------------------------------------------------------------------------
    void coreGlideAndShapes()
    {
        beginTest("Core: a tap glides to a new place, never faster than MAX_SLEW; voices come and go without a click");
        {
            MixedCore c = core(1, 0.002f, 0.0f);
            float l = 0.0f, r = 0.0f, fastest = 0.0f;
            for (int i = 0; i < 480; ++i) c.process(&l, &r, 1);
            c.setDelay(0.0f, 0.015f, 0.0f);
            float last = c.tapDelay(0, 0);
            int arrived = -1;
            for (int i = 0; i < 9600; ++i)
            {
                l = r = 0.0f;
                c.process(&l, &r, 1);
                const float d = c.tapDelay(0, 0);
                fastest = std::max(fastest, std::abs(d - last));
                last = d;
                if (arrived < 0 && std::abs(d - 720.0f) < 0.5f) arrived = i;
            }
            expect(fastest <= 0.25f + 1.0e-4f, "fastest move " + juce::String(fastest, 4) + " samples a sample");
            expect(fastest > 0.2f, "and it does move at that speed for a jump this large");
            expect(arrived > 2400 && arrived < 3200, "13 ms at a quarter of a sample a sample: arrived after " + juce::String(arrived));

            // A square LFO at the top of the depth: the same limit.
            MixedCore sq = core(1, 0.008f, 0.006f);
            sq.setShape((int) LFO::Shape::Square);
            sq.setRate(5.0f);
            fastest = 0.0f;
            last = sq.tapDelay(0, 0);
            const Audio out = render(sq, 48000, sine(440.0, 0.5f), 1, [&](int b)
            {
                if (b > 1) fastest = std::max(fastest, std::abs(sq.tapDelay(0, 0) - last));     // (the tap is placed at the first sample)
                last = sq.tapDelay(0, 0);
            });
            expect(fastest <= 0.25f + 1.0e-4f && fastest > 0.2f);
            const Steps st = steps(out, {}, 0, 2400);
            const float plain = (float) (0.5 * TWO_PI * 440.0 / SR);
            expect(st.elsewhere < plain * 1.4f, "a bend of a quarter, no click: largest step " + juce::String(st.elsewhere, 4) + " against " + juce::String(plain, 4));
        }
        {
            MixedCore c = core(2, 0.006f, 0.002f);
            c.setMix(0.5f);
            const int at1 = 9856, at2 = 19392;
            const Audio out = render(c, 28800, sine(330.0, 0.5f), BLOCK, [&](int b)
            {
                if (b * BLOCK == at1) c.setVoices(6);
                if (b * BLOCK == at2) c.setVoices(1);
            });
            const Steps st = steps(out, { at1, at2 }, 960, 2400);
            expect(! st.clicks(), "voices 2 -> 6 -> 1: " + st.text());
        }

        beginTest("Core: every LFO shape moves the tap inside the swing, each its own way");
        {
            std::vector<Buf> paths;
            for (int s = 0; s < fxm::Lfo::NUM_SHAPES; ++s)
            {
                MixedCore c = core(1, 0.008f, 0.003f);
                c.setShape(s);
                c.setRate(8.0f);
                Buf path;
                float lo = 1.0e9f, hi = 0.0f;
                render(c, 96000, noise(0.1f), 16, [&](int b)
                {
                    if (b == 0) return;         // (the tap is placed at the first sample)
                    path.push_back(c.tapDelay(0, 0));
                    lo = std::min(lo, path.back());
                    hi = std::max(hi, path.back());
                });
                const juce::String name = LFO::shapeLabels()[s];
                const bool oneWay = LFO::isUnipolar((LFO::Shape) s);
                expect(hi <= 0.011f * SR + 0.5f && lo >= 0.005f * SR - 0.5f, name + " stays inside the swing");
                expect(hi > 0.0095f * SR, name + " reaches up: " + juce::String(hi / SR * 1000.0, 2) + " ms");
                if (oneWay) expect(lo > 0.008f * SR - 1.0f, name + " goes one way only");
                else        expect(lo < 0.0068f * SR, name + " reaches down: " + juce::String(lo / SR * 1000.0, 2) + " ms");
                for (size_t other = 0; other < paths.size(); ++other)
                    expect(rmsDiff(path, paths[other]) > 5.0f, name + " moves unlike " + LFO::shapeLabels()[other]);
                paths.push_back(path);
            }
        }

        beginTest("Core: through zero, the dry sound waits where the tap's centre is, so the two meet completely");
        {
            MixedCore c = core(1, 0.003f, 0.0f);
            c.setMix(0.5f);
            c.setThroughZero(true);
            const Audio dry = renderWith([](float*, float*, int) {}, 9600, noise(0.5f));
            const Audio out = render(c, 9600, noise(0.5f));
            float worst = 0.0f;
            for (size_t i = 144; i < out.l.size(); ++i)
                worst = std::max(worst, std::abs(out.l[i] - dry.l[i - 144]));
            expect(worst < 1.0e-5f, "tap and dry at the same place: the input, 3 ms late (" + juce::String(worst, 6) + ")");

            // Without it the same setting is a comb with its first notch at 1 / (2 x 3 ms).
            MixedCore comb = core(1, 0.003f, 0.0f);
            comb.setMix(0.5f);
            const double notch = 1.0 / 0.006;
            expect(rms(render(comb, 19200, sine(notch, 0.5f)).l, 9600) < 0.002f, "the comb's notch");
            MixedCore thru = core(1, 0.003f, 0.0f);
            thru.setMix(0.5f);
            thru.setThroughZero(true);
            expectWithinAbsoluteError(rms(render(thru, 19200, sine(notch, 0.5f)).l, 9600), 0.3536f, 0.002f);

            // Switched under sound it is crossed to.
            MixedCore live = core(1, 0.003f, 0.002f);
            live.setMix(0.5f);
            const Audio sw = render(live, 28800, sine(330.0, 0.5f), BLOCK, [&](int b)
            {
                if (b * BLOCK == 9856) live.setThroughZero(true);
                if (b * BLOCK == 19392) live.setThroughZero(false);
            });
            const Steps st = steps(sw, { 9856, 19392 }, 1920, 2400);
            expect(! st.clicks(), "through zero on and off: " + st.text());
        }
    }

    // ---- 5 ---------------------------------------------------------------------------------------
    void typesAndShapes()
    {
        beginTest("The ten effects keep their type numbers and names, and every one is a set of cells with a SHAPE");
        for (auto& k : kinds())
        {
            expectEquals((int) k.type, k.number, k.name);
            expectEquals(juce::String(AudioEngine::effectTypeName(k.type)), juce::String(k.name));
            auto fx = AudioEngine::createEffect(k.type);
            expect(cellsOf(fx.get()) != nullptr, juce::String(k.name) + " can be moved from outside");
            if (cellsOf(fx.get()) == nullptr) continue;
            for (int cell = 0; cell < 8; ++cell)
            {
                const ParamSpec spec = target(*fx).cellSpec(cell);
                if (! spec.isValid()) continue;
                expect(std::strlen(spec.label) >= 1 && std::strlen(spec.label) <= 4);
                expectWithinAbsoluteError(getCell(*fx, cell), spec.defaultVal, 1.0e-6f, juce::String(k.name) + " " + spec.label + " starts at its default");
                setCell(*fx, cell, spec.maxVal + 1000.0f);
                expectWithinAbsoluteError(getCell(*fx, cell), spec.maxVal, 1.0e-5f);
                setCell(*fx, cell, spec.minVal - 1000.0f);
                expectWithinAbsoluteError(getCell(*fx, cell), spec.minVal, 1.0e-5f);
                setCell(*fx, cell, std::numeric_limits<float>::quiet_NaN());
                expectWithinAbsoluteError(getCell(*fx, cell), spec.minVal, 1.0e-5f, "what is not a number is not taken");
            }
            for (int cell : k.shapeCells)
            {
                const ParamSpec spec = target(*fx).cellSpec(cell);
                expect(spec.kind == ParamKind::Choice && spec.numChoices == (int) LFO::Shape::COUNT && spec.choices == LFO::shapeLabels(),
                       juce::String(k.name) + ": cell " + juce::String(cell) + " offers every shape of dsp/LFO.h");
                expectEquals((int) spec.maxVal, (int) LFO::Shape::COUNT - 1);
            }
            // Tempo sync as before: the effects with a RATE among their four can be locked to the tempo.
            expect(fx->canSync() == (k.type != EffectType::RotaryEffect), juce::String(k.name) + " sync");
        }

        beginTest("Every shape of every effect is a different sound; USR plays the track's own shape when it is handed over");
        static const float levels[LFO::USER_POINTS] = { 1.0f, -1.0f, 0.6f, -0.2f, 1.0f, 1.0f, -1.0f, 0.1f };
        for (auto& k : kinds())
            for (int cell : k.shapeCells)
            {
                std::vector<Audio> heard;
                for (int s = 0; s < (int) LFO::Shape::COUNT; ++s)
                {
                    auto fx = make(k);
                    setCell(*fx, cell, (float) s);
                    fx->reset();
                    heard.push_back(render(*fx, 24000, chord()));
                    const juce::String what = juce::String(k.name) + " cell " + juce::String(cell) + " " + LFO::shapeLabels()[s];
                    expect(finite(heard.back(), 8.0f), what + " is bounded");
                    expectEquals((int) getCell(*fx, cell), s);
                    for (int other = 0; other < s; ++other)
                        expect(difference(heard[(size_t) s], heard[(size_t) other], 2400) > 2.0e-3f,
                               what + " sounds like " + LFO::shapeLabels()[other] + ": " + juce::String(difference(heard[(size_t) s], heard[(size_t) other], 2400), 5));
                }
                for (auto shape : { LFO::Shape::User, LFO::Shape::UserSmooth })
                {
                    auto fx = make(k);
                    setCell(*fx, cell, (float) (int) shape);
                    target(*fx).setUserShape(levels);
                    fx->reset();
                    expect(difference(render(*fx, 24000, chord()), heard[(size_t) (int) shape], 2400) > 2.0e-3f,
                           juce::String(k.name) + " cell " + juce::String(cell) + ": the custom shape is played");
                }
            }
    }

    // ---- 6 ---------------------------------------------------------------------------------------
    // Level of a sound in windows of `window` samples.
    static Buf levels(const Buf& b, int from, int window)
    {
        Buf out;
        for (int i = from; i + window <= (int) b.size(); i += window)
            out.push_back(rms(b, i, i + window));
        return out;
    }
    // How often a level pattern goes up through its middle, per second.
    static float cyclesPerSecond(const Buf& level, double windowsPerSecond)
    {
        const float lo = *std::min_element(level.begin(), level.end()), hi = *std::max_element(level.begin(), level.end());
        const float mid = 0.5f * (lo + hi), band = 0.15f * (hi - lo);
        int count = 0;
        bool below = level.front() < mid;
        for (float v : level)
        {
            if (below && v > mid + band) { ++count; below = false; }
            else if (! below && v < mid - band) below = true;
        }
        return (float) (count * windowsPerSecond / (double) level.size());
    }

    void whatEachGained()
    {
        auto sound = [](const Kind& k, const std::function<void(EffectBase&)>& set, const Gen& in, int n = 24000)
        {
            auto fx = make(k);
            set(*fx);
            fx->reset();
            return render(*fx, n, in);
        };
        auto kind = [](EffectType t) -> const Kind& { for (auto& k : kinds()) if (k.type == t) return k; return kinds().front(); };

        beginTest("Chorus: up to six voices, feedback of either polarity, the sides set apart, the centre delay");
        {
            const Kind& k = kind(EffectType::ChorusEffect);
            std::vector<Audio> byVoices;
            for (int n = 1; n <= 6; ++n)
            {
                byVoices.push_back(sound(k, [n](EffectBase& fx) { fx.setParameter(Chorus::Voices, (float) n); }, chord()));
                for (int other = 0; other + 1 < n; ++other)
                    expect(difference(byVoices.back(), byVoices[(size_t) other], 2400) > 2.0e-3f, juce::String(n) + " voices sound unlike " + juce::String(other + 1));
                expectWithinAbsoluteError(rms(byVoices.back().l, 2400), rms(byVoices.front().l, 2400), 0.08f, "about as loud with " + juce::String(n));
            }
            // Through-zero: at THRU ON the chorus's sweep crosses the delayed dry, deepening the comb.
            const Audio thruOff = sound(k, [](EffectBase& fx) { fx.setExtra(Chorus::Through, 0.0f); fx.setParameter(Chorus::Mix, 0.5f); }, chord());
            const Audio thruOn  = sound(k, [](EffectBase& fx) { fx.setExtra(Chorus::Through, 1.0f); fx.setParameter(Chorus::Mix, 0.5f); }, chord());
            expect(difference(thruOff, thruOn, 2400) > 0.01f, "THRU changes the sound");

            const Gen mono = noise(0.3f, 3, true);
            const Audio alike = sound(k, [](EffectBase& fx) { fx.setExtra(Chorus::Spread, 0.0f); }, mono);
            const Audio apart = sound(k, [](EffectBase& fx) { fx.setExtra(Chorus::Spread, 1.0f); }, mono);
            expect(rmsDiff(alike.l, alike.r) == 0.0f, "SPRD 0: the sides alike");
            expect(rmsDiff(apart.l, apart.r) > 0.02f, "SPRD up: a stereo chorus from a mono sound");

            for (float time : { 0.001f, 0.007f, 0.02f })
            {
                const Audio hit = sound(k, [time](EffectBase& fx)
                {
                    fx.setExtra(Chorus::Time, time);
                    fx.setParameter(Chorus::Depth, 0.0f);
                    fx.setExtra(Chorus::Through, 0.0f);
                    fx.setParameter(Chorus::Mix, 1.0f);
                }, impulse(), 2400);
                int where = 0;
                for (int i = 0; i < 2400; ++i)
                    if (std::abs(hit.l[(size_t) i]) > std::abs(hit.l[(size_t) where])) where = i;
                expectWithinAbsoluteError((float) where, time * (float) SR, 1.0f, "TIME " + juce::String(time * 1000.0f, 1) + " ms");
            }
        }

        beginTest("Ensemble: one to six voices, feedback, the middle of the input spread over both sides");
        {
            const Kind& k = kind(EffectType::EnsembleEffect);
            std::vector<Audio> byVoices;
            for (int n = 1; n <= 6; ++n)
            {
                byVoices.push_back(sound(k, [n](EffectBase& fx) { fx.setExtra(Ensemble::Voices, (float) n); }, chord()));
                for (int other = 0; other + 1 < n; ++other)
                    expect(difference(byVoices.back(), byVoices[(size_t) other], 2400) > 2.0e-3f, juce::String(n) + " voices sound unlike " + juce::String(other + 1));
            }
            // Through-zero is handled by the chain, not the core (the ensemble has a mono input
            // line, and the core disables through-zero for mono). Tested in "Through Zero".
            const Audio wide = sound(k, [](EffectBase& fx) { fx.setParameter(Ensemble::Mix, 1.0f); }, leftOnly(noise(0.3f)));
            expect(rms(wide.r, 2400) > 0.02f, "a sound on the left is spread to the right");
            const Audio narrow = sound(k, [](EffectBase& fx) { fx.setParameter(Ensemble::Mix, 1.0f); fx.setParameter(Ensemble::Width, 0.0f); }, noise(0.3f));
            expect(rmsDiff(narrow.l, narrow.r) < 1.0e-6f, "WDTH 0: the voices in the middle");
        }

        beginTest("Flanger: through zero, the sides set apart, the centre delay");
        {
            const Kind& k = kind(EffectType::FlangerEffect);
            auto still = [](bool thru) { return [thru](EffectBase& fx)
            {
                fx.setParameter(Flanger::Depth, 0.0f);
                fx.setParameter(Flanger::Feedback, 0.0f);
                fx.setExtra(Flanger::Through, thru ? 1.0f : 0.0f);
            }; };
            const double notch = 1.0 / 0.006;
            expect(rms(sound(k, still(false), sine(notch, 0.5f)).l, 12000) < 0.003f, "the comb's first notch at rest");
            expectWithinAbsoluteError(rms(sound(k, still(true), sine(notch, 0.5f)).l, 12000), 0.3536f, 0.003f);

            // Sweeping: with THRU the tap passes the dry sound, so the level of a low tone falls to
            // nothing twice a cycle (the tap half a period away) and comes back whole (at the crossing).
            const Audio swept = sound(k, [](EffectBase& fx)
            {
                fx.setParameter(Flanger::Feedback, 0.0f);
                fx.setParameter(Flanger::Depth, 1.0f);
                fx.setExtra(Flanger::Through, 1.0f);
                fx.setExtra(Flanger::Spread, 0.0f);
            }, sine(250.0, 0.5f), 48000);
            const Buf lv = levels(swept.l, 4800, 192);
            expect(*std::max_element(lv.begin(), lv.end()) > 0.34f && *std::min_element(lv.begin(), lv.end()) < 0.05f,
                   "through zero: whole at the crossing, gone half a period off");

            const Gen mono = noise(0.3f, 3, true);
            const Audio alike = sound(k, [](EffectBase& fx) { fx.setExtra(Flanger::Spread, 0.0f); }, mono);
            const Audio apart = sound(k, [](EffectBase& fx) { fx.setExtra(Flanger::Spread, 1.0f); }, mono);
            expect(rmsDiff(alike.l, alike.r) == 0.0f && rmsDiff(apart.l, apart.r) > 0.02f, "SPRD");
            const Audio shortOne = sound(k, [](EffectBase& fx) { fx.setExtra(Flanger::Time, 0.0005f); }, chord());
            const Audio longOne = sound(k, [](EffectBase& fx) { fx.setExtra(Flanger::Time, 0.01f); }, chord());
            expect(difference(shortOne, longOne, 2400) > 0.01f, "TIME");
        }

        beginTest("Vibrato and wah: the slot's WET is heard (it did nothing before)");
        for (auto type : { EffectType::VibratoEffect, EffectType::WahEffect })
        {
            const Kind& k = kind(type);
            const Audio dry = renderWith([](float*, float*, int) {}, 24000, chord());
            const Audio none = sound(k, [](EffectBase& fx) { fx.setMix(0.0f); }, chord());
            const Audio half = sound(k, [](EffectBase& fx) { fx.setMix(0.5f); }, chord());
            const Audio whole = sound(k, [](EffectBase& fx) { fx.setMix(1.0f); }, chord());
            expect(maxDiff(none, dry) < 1.0e-6f, juce::String(k.name) + " WET 0 is the input");
            expect(difference(half, whole, 2400) > 0.01f && difference(half, dry, 2400) > 0.01f, juce::String(k.name) + " WET 50 is between");
        }

        beginTest("Tremolo: the one-way shapes use the whole depth, SQR gates, STER sets the sides against each other");
        {
            const Kind& k = kind(EffectType::TremoloEffect);
            auto with = [](int shape, float stereo) { return [shape, stereo](EffectBase& fx)
            {
                fx.setParameter(Tremolo::Rate, 5.0f);
                fx.setParameter(Tremolo::Depth, 1.0f);
                fx.setParameter(Tremolo::Shape, (float) shape);
                fx.setParameter(Tremolo::StereoOffset, stereo);
            }; };
            const int cycle = 9600;     // 5 Hz
            const Audio decay = sound(k, with((int) LFO::Shape::ExpDown, 0.0f), sine(1000.0, 0.5f), 48000);
            // Each cycle starts whole and falls to nothing (a pluck).
            expectWithinAbsoluteError(rms(decay.l, 2 * cycle + 96, 2 * cycle + 288), 0.33f, 0.04f);
            expect(rms(decay.l, 3 * cycle - 960, 3 * cycle - 96) < 0.02f, "EXPD ends its cycle at nothing");
            const Audio swell = sound(k, with((int) LFO::Shape::ExpUp, 0.0f), sine(1000.0, 0.5f), 48000);
            expect(rms(swell.l, 2 * cycle + 96, 2 * cycle + 480) < 0.02f && rms(swell.l, 3 * cycle - 480, 3 * cycle - 96) > 0.25f, "EXPU swells");
            const Audio gate = sound(k, with((int) LFO::Shape::Square, 0.0f), sine(1000.0, 0.5f), 48000);
            expectWithinAbsoluteError(rms(gate.l, 2 * cycle + 960, 2 * cycle + 3840), 0.3536f, 0.01f);
            expect(rms(gate.l, 2 * cycle + cycle / 2 + 960, 3 * cycle - 960) < 0.005f, "SQR: on, then off");
            const Steps st = steps(gate, {}, 0, 2400);
            expect(st.elsewhere < 0.12f, "the gate's edge is rounded: largest step " + juce::String(st.elsewhere, 4));

            const Audio opposite = sound(k, with((int) LFO::Shape::Square, 0.5f), sine(1000.0, 0.5f), 48000);
            expect(rms(opposite.l, 2 * cycle + 960, 2 * cycle + 3840) > 0.34f && rms(opposite.r, 2 * cycle + 960, 2 * cycle + 3840) < 0.005f, "STER 50: one side on while the other is off");
        }

        beginTest("Auto-pan: PHAS says where the cycle stands; phaser: every stage count is its own sound, the sides can be set apart, WET starts at half");
        {
            const Kind& pan = kind(EffectType::AutoPanEffect);
            auto with = [](float phase) { return [phase](EffectBase& fx)
            {
                fx.setParameter(AutoPan::Rate, 1.0f);
                fx.setParameter(AutoPan::Depth, 1.0f);
                fx.setParameter(AutoPan::Shape, (float) (int) LFO::Shape::Square);
                fx.setParameter(AutoPan::Phase, phase);
            }; };
            const Audio a = sound(pan, with(0.0f), sine(1000.0, 0.5f), 19200), b = sound(pan, with(0.5f), sine(1000.0, 0.5f), 19200);
            expect(rms(a.r, 4800, 19200) > 0.34f && rms(a.l, 4800, 19200) < 0.005f, "PHAS 0: the first half of the cycle is on the right");
            expect(rms(b.l, 4800, 19200) > 0.34f && rms(b.r, 4800, 19200) < 0.005f, "PHAS 50: on the left");

            const Kind& ph = kind(EffectType::PhaserEffect);
            expectWithinAbsoluteError(AudioEngine::createEffect(EffectType::PhaserEffect)->getMix(), 0.5f, 1.0e-6f);
            std::vector<Audio> byStages;
            for (int n = 2; n <= 6; ++n)
            {
                byStages.push_back(sound(ph, [n](EffectBase& fx) { fx.setParameter(Phaser::Stages, (float) n); }, noise(0.3f, 5, true)));
                for (size_t other = 0; other + 1 < byStages.size(); ++other)
                    expect(difference(byStages.back(), byStages[other], 2400) > 0.01f, juce::String(n) + " stages sound unlike " + juce::String((int) other + 2));
            }
            const Audio alike = sound(ph, [](EffectBase& fx) { fx.setExtra(Phaser::Spread, 0.0f); }, noise(0.3f, 5, true));
            const Audio apart = sound(ph, [](EffectBase& fx) { fx.setExtra(Phaser::Spread, 1.0f); }, noise(0.3f, 5, true));
            expect(rmsDiff(alike.l, alike.r) == 0.0f && rmsDiff(apart.l, apart.r) > 0.02f, "SPRD");
        }

        beginTest("Ring modulator: the carrier is any LFO shape at audio rate, its steps rounded");
        {
            const Kind& k = kind(EffectType::RingModEffect);
            auto with = [](int wave, float hz) { return [wave, hz](EffectBase& fx)
            {
                fx.setParameter(RingMod::Freq, hz);
                fx.setParameter(RingMod::Depth, 0.0f);
                fx.setParameter(RingMod::Mix, 1.0f);
                fx.setExtra(RingMod::Wave, (float) wave);
            }; };
            const Audio sin300 = sound(k, with((int) LFO::Shape::Sine, 300.0f), sine(1000.0, 0.5f), 48000);
            expectWithinAbsoluteError(amplitudeAt(sin300.l, 0, 48000, 700.0), 0.25f, 0.01f);
            expect(amplitudeAt(sin300.l, 0, 48000, 100.0) < 0.005f && amplitudeAt(sin300.l, 0, 48000, 1000.0) < 0.005f);
            // A square carrier has the odd harmonics: side bands at 1000 -+ 300 and 1000 -+ 900.
            const Audio sq300 = sound(k, with((int) LFO::Shape::Square, 300.0f), sine(1000.0, 0.5f), 48000);
            expectWithinAbsoluteError(amplitudeAt(sq300.l, 0, 48000, 700.0), 0.25f * 4.0f / 3.14159f, 0.02f);
            expectWithinAbsoluteError(amplitudeAt(sq300.l, 0, 48000, 100.0), 0.25f * 4.0f / (3.0f * 3.14159f), 0.02f);
            // The carrier by itself (a steady input): a 5 kHz square's ninth harmonic would fold to 3 kHz at 0.14.
            const Audio alone = sound(k, with((int) LFO::Shape::Square, 5000.0f), [](int, int) { return 1.0f; }, 48000);
            const float folded = amplitudeAt(alone.l, 0, 48000, 3000.0);
            logMessage("5 kHz square carrier: folded ninth harmonic at 3 kHz " + juce::String(folded, 4) + " (unrounded 0.14)");
            expect(folded < 0.01f, "the square's steps are rounded: " + juce::String(folded, 4));
            expectWithinAbsoluteError(amplitudeAt(alone.l, 0, 48000, 5000.0), 4.0f / 3.14159f, 0.15f);
        }

        beginTest("Rotary: SPD is any speed between slow and fast, ACCL is how soon; the middle of the input, the sides against each other");
        {
            const Kind& k = kind(EffectType::RotaryEffect);
            auto hornRate = [&](float speed)
            {
                const Audio out = sound(k, [speed](EffectBase& fx) { fx.setParameter(RotarySpeaker::Speed, speed); fx.setMix(1.0f); }, sine(9000.0, 0.5f), 48000 * 6);
                return cyclesPerSecond(levels(out.l, 48000, 240), 200.0);
            };
            const float slow = hornRate(0.0f), mid = hornRate(0.5f), fast = hornRate(1.0f);
            logMessage("horn: " + juce::String(slow, 2) + " Hz slow, " + juce::String(mid, 2) + " Hz half way, " + juce::String(fast, 2) + " Hz fast");
            expectWithinAbsoluteError(slow, 0.8f, 0.2f);
            expectWithinAbsoluteError(fast, 7.0f, 0.4f);
            expectWithinAbsoluteError(mid, std::sqrt(0.8f * 7.0f), 0.4f);

            // Switched to fast under sound: more turns in the first second the higher ACCL is.
            auto turnsAfterSwitch = [&](float accel)
            {
                auto fx = make(k);
                fx->setParameter(RotarySpeaker::Speed, 0.0f);
                fx->setParameter(RotarySpeaker::Acceleration, accel);
                fx->setMix(1.0f);
                fx->reset();
                render(*fx, 24000, sine(9000.0, 0.5f));
                fx->setParameter(RotarySpeaker::Speed, 1.0f);
                const Audio out = render(*fx, 48000, sine(9000.0, 0.5f));
                return cyclesPerSecond(levels(out.l, 0, 240), 200.0);
            };
            const float lazy = turnsAfterSwitch(0.0f), quick = turnsAfterSwitch(1.0f);
            logMessage("turns in the first second after SPD goes up: ACCL 0 " + juce::String(lazy, 1) + ", ACCL 100 " + juce::String(quick, 1));
            expect(quick > 4.5f && lazy < 3.5f && lazy > 0.9f, "ACCL");

            const Audio one = sound(k, [](EffectBase& fx) { fx.setMix(1.0f); }, leftOnly(sine(3000.0, 0.5f)), 48000);
            expect(rms(one.r, 4800) > 0.05f, "one cabinet: a sound on the left comes out of both sides");
            const Buf l = levels(one.l, 4800, 240), r = levels(one.r, 4800, 240);
            double corr = 0.0, ml = 0.0, mr = 0.0;
            for (size_t i = 0; i < l.size(); ++i) { ml += l[i]; mr += r[i]; }
            ml /= (double) l.size(); mr /= (double) r.size();
            for (size_t i = 0; i < l.size(); ++i) corr += (l[i] - ml) * (r[i] - mr);
            expect(corr < 0.0, "as one side grows louder the other grows softer");
        }
    }

    // ---- 7 ---------------------------------------------------------------------------------------
    void modulation()
    {
        beginTest("modulateCell: an offset on a cell is the knob turned, without the knob moving; choices and counts do not move");
        for (auto& k : kinds())
            for (int cell = 0; cell < CellEffect::TARGETS; ++cell)
            {
                auto probe = make(k);
                const ParamSpec spec = target(*probe).cellSpec(cell);
                const juce::String what = juce::String(k.name) + " " + (spec.isValid() ? spec.label : "-");
                if (! spec.isValid())
                {
                    expect(! target(*probe).modulates(cell));
                    continue;
                }
                if (spec.isDiscrete())
                {
                    expect(! target(*probe).modulates(cell), what + " is a choice or a count");
                    const float before = target(*probe).cellValue(cell);
                    target(*probe).modulate(cell, 0.5f);
                    expect(target(*probe).cellValue(cell) == before && target(*probe).modulation(cell) == 0.0f);
                    continue;
                }
                expect(target(*probe).modulates(cell), what + " can be moved");
                const float low = spec.fromNorm(0.3f), high = spec.fromNorm(0.7f);

                auto atLow = make(k), atHigh = make(k), moved = make(k);
                setCell(*atLow, cell, low);
                setCell(*atHigh, cell, high);
                setCell(*moved, cell, low);
                target(*moved).modulate(cell, 0.4f);
                expectWithinAbsoluteError(target(*moved).modulation(cell), 0.4f, 1.0e-6f);
                expectWithinAbsoluteError(target(*moved).cellValue(cell), high, 1.0e-4f * std::max(1.0f, std::abs(high)), what + " plays knob + offset");
                expectWithinAbsoluteError(getCell(*moved, cell), low, 1.0e-5f * std::max(1.0f, std::abs(low)), what + ": the knob stays");
                for (auto* fx : { atLow.get(), atHigh.get(), moved.get() })
                    fx->reset();
                const Audio a = render(*atLow, 19200, chord()), b = render(*atHigh, 19200, chord()), c = render(*moved, 19200, chord());
                expect(maxDiff(b, c) < 2.0e-3f, what + ": the offset sounds like the knob turned (" + juce::String(maxDiff(b, c), 6) + ")");
                // (ACCL is heard only while the rotors change speed; DLAY, phase E6, only once it has
                // passed, and these renders are shorter than either value of it: "Effect Cores Sweep" holds it.)
                if (! (k.type == EffectType::RotaryEffect && cell == 3) && juce::String(spec.label) != "DLAY")
                    expect(difference(a, b) > 1.0e-3f, what + " is heard: " + juce::String(difference(a, b), 6));

                // Held to the range, to -1..1, and what is not a number is nothing.
                target(*moved).modulate(cell, 7.0f);
                expectWithinAbsoluteError(target(*moved).cellValue(cell), spec.maxVal, 1.0e-4f * std::max(1.0f, std::abs(spec.maxVal)));
                expectEquals(target(*moved).modulation(cell), 1.0f);
                target(*moved).modulate(cell, -7.0f);
                expectWithinAbsoluteError(target(*moved).cellValue(cell), spec.minVal, 1.0e-4f * std::max(1.0f, std::abs(spec.maxVal)));
                target(*moved).modulate(cell, std::numeric_limits<float>::quiet_NaN());
                expectEquals(target(*moved).modulation(cell), 0.0f);
                target(*moved).modulate(cell, 0.3f);
                target(*moved).clearModulation();
                expectWithinAbsoluteError(target(*moved).cellValue(cell), low, 1.0e-5f * std::max(1.0f, std::abs(low)), what + ": cleared");
            }

        beginTest("modulateCell: WET moved all the way down is the input; an offset moved every block makes no click");
        for (auto& k : kinds())
        {
            auto fx = make(k);
            fx->setMix(0.6f);
            target(*fx).modulate(CellEffect::WET, -1.0f);
            fx->reset();
            const Audio dry = renderWith([](float*, float*, int) {}, 9600, chord());
            expect(maxDiff(render(*fx, 9600, chord()), dry) < 1.0e-6f, juce::String(k.name) + ": WET offset -1");

            for (int cell = 0; cell < CellEffect::TARGETS; ++cell)
            {
                if (! target(*fx).modulates(cell)) continue;
                const juce::String what = juce::String(k.name) + " " + target(*fx).cellSpec(cell).label;
                // The steps of the sound at rest, at both ends and in the middle of the sweep.
                float still = 0.0f, peak = 0.0f;
                for (float offset : { -0.5f, 0.0f, 0.5f })
                {
                    auto ref = make(k);
                    if (cell == CellEffect::WET) ref->setMix(0.5f);
                    target(*ref).modulate(cell, offset);
                    ref->reset();
                    const Steps st = steps(render(*ref, 24000, chord()), {}, 0, 2400);
                    still = std::max(still, st.elsewhere);
                    peak = std::max(peak, st.peak);
                }
                auto live = make(k);
                if (cell == CellEffect::WET) live->setMix(0.5f);
                live->reset();
                const Audio out = render(*live, 48000, chord(), BLOCK, [&](int b)
                {
                    const float t = (float) (b % 150) / 150.0f;                 // a triangle five times a second
                    target(*live).modulate(cell, 0.5f * (t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t));
                });
                const Steps st = steps(out, {}, 0, 2400);
                expect(finite(out, 8.0f));
                expect(st.elsewhere <= 1.5f * still + 0.03f * peak, what + " swept: largest step " + juce::String(st.elsewhere, 4)
                                                                    + " against " + juce::String(still, 4) + " at rest");
            }
        }
    }

    // ---- 8 ---------------------------------------------------------------------------------------
    void cellsUnderSound()
    {
        beginTest("Every cell of every one of the ten jumped to its top, its bottom and back under a held sound: no click");
        const int first = 9856, second = 19584, third = 29248, total = 40800;
        for (auto& k : kinds())
            for (int cell = 0; cell < CellEffect::TARGETS; ++cell)
            {
                auto fx = make(k);
                const ParamSpec spec = target(*fx).cellSpec(cell);
                if (! spec.isValid()) continue;
                const float home = getCell(*fx, cell);
                const Audio out = render(*fx, total, chord(), BLOCK, [&](int b)
                {
                    if (b * BLOCK == first)  setCell(*fx, cell, spec.maxVal);
                    if (b * BLOCK == second) setCell(*fx, cell, spec.minVal);
                    if (b * BLOCK == third)  setCell(*fx, cell, home);
                });
                const Steps st = steps(out, { first, second, third }, 960, 2400);
                expect(finite(out, 8.0f));
                expect(! st.clicks(), juce::String(k.name) + " " + spec.label + ": " + st.text());
            }
    }

    // ---- 9 ---------------------------------------------------------------------------------------
    void anyInput()
    {
        beginTest("Any input (not a number, infinite, 1e30), 22.05 to 192 kHz, blocks of 1 to 4096, every cell at both ends: finite, bounded, and it recovers");
        const float bad[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f };
        struct Rate { double sr; int block; };
        for (auto& k : kinds())
            for (Rate rate : { Rate { 22050.0, 1 }, Rate { 48000.0, 64 }, Rate { 192000.0, 4096 }, Rate { 44100.0, 4096 }, Rate { 192000.0, 1 } })
                for (int end = 0; end < 3; ++end)        // lively, every cell at its bottom, at its top
                {
                    auto fx = make(k, rate.sr, rate.block);
                    if (end > 0)
                        for (int cell = 0; cell < 8; ++cell)
                        {
                            const ParamSpec spec = target(*fx).cellSpec(cell);
                            if (spec.isValid()) setCell(*fx, cell, end == 1 ? spec.minVal : spec.maxVal);
                        }
                    fx->setMix(end == 1 ? 0.5f : 1.0f);
                    fx->reset();
                    const juce::String what = juce::String(k.name) + " at " + juce::String(rate.sr, 0) + " / " + juce::String(rate.block) + ", setting " + juce::String(end);
                    const int n = (int) (rate.sr * (rate.block == 1 ? 0.05 : 0.2));
                    const Gen sound = chord(rate.sr);
                    const Audio out = render(*fx, n, [&](int i, int side)
                    {
                        if (i > n / 4 && i < n / 2 && (i % 97 == 0 || (i / 500) % 4 == 1))
                            return bad[(size_t) ((i / 7 + side) % 5)];
                        return sound(i, side);
                    }, rate.block);
                    expect(finite(out, 2000.0f), what + ": finite and bounded");
                    // Afterwards, with an ordinary sound, it plays ordinarily.
                    render(*fx, (int) (rate.sr * 0.3), [](int, int) { return 0.0f; }, rate.block);
                    const Audio after = render(*fx, n, sound, rate.block);
                    expect(finite(after, 30.0f), what + ": back to an ordinary level");
                }
    }

    // ---- 10 --------------------------------------------------------------------------------------
    void blockSizes()
    {
        beginTest("The same sound whatever the block size (1, 64, 4096)");
        for (auto& k : kinds())
        {
            std::vector<Audio> heard;
            for (int block : { 1, 64, 4096 })
            {
                auto fx = make(k);
                heard.push_back(render(*fx, 12000, chord(), block));
            }
            expect(maxDiff(heard[0], heard[1]) < 1.0e-5f && maxDiff(heard[1], heard[2]) < 1.0e-5f,
                   juce::String(k.name) + ": " + juce::String(maxDiff(heard[0], heard[1]), 7) + ", " + juce::String(maxDiff(heard[1], heard[2]), 7));
        }
    }

    // ---- 11 --------------------------------------------------------------------------------------
    void modFxBlock()
    {
        auto sound = [](ModFxBlock& m, int n = 24000)
        {
            return renderWith([&m](float* l, float* r, int count) { if (m.isActive()) m.process(l, r, count); }, n, chord());
        };
        auto blockWith = [](int cell, float depth)
        {
            auto m = std::make_unique<ModFxBlock>();
            m->prepare(SR, BLOCK);
            m->set(cell, depth);
            return m;
        };

        beginTest("MODFX: a ninth value, the shape of all four; its cells can be moved from outside like an effect's");
        expectEquals((int) ModFxBlock::ShapeCell, 8);
        expectEquals(ModFxBlock::shapeSpec().id, (int) TrackFxParam::MODFX_BASE + 8);
        expectEquals(ModFxBlock::shapeSpec().numChoices, (int) LFO::Shape::COUNT);
        for (int depthCell : { (int) ModFxBlock::ChorusDepth, (int) ModFxBlock::FlangerDepth, (int) ModFxBlock::PhaserDepth, (int) ModFxBlock::RotaryDepth })
        {
            auto plain = blockWith(depthCell, 0.8f), square = blockWith(depthCell, 0.8f);
            square->set(ModFxBlock::ShapeCell, (float) (int) LFO::Shape::Square);
            expectEquals((int) square->get(ModFxBlock::ShapeCell), (int) LFO::Shape::Square);
            expectEquals((int) plain->get(ModFxBlock::ShapeCell), 0);
            const Audio a = sound(*plain), b = sound(*square);
            expect(finite(b, 8.0f));
            expect(difference(a, b, 2400) > 2.0e-3f, "depth cell " + juce::String(depthCell) + ": SQR is heard");
            square->set(ModFxBlock::ShapeCell, 1000.0f);
            expectEquals((int) square->get(ModFxBlock::ShapeCell), (int) LFO::Shape::COUNT - 1);
        }
        for (int cell = 0; cell < ModFxBlock::NumCells; ++cell)
        {
            const ParamSpec spec = ModFxBlock::page().params[(size_t) cell];
            const int depthCell = cell | 1, rateCell = cell & ~1;
            const float low = spec.fromNorm(0.3f), high = spec.fromNorm(0.7f);
            auto make3 = [&](float v)
            {
                auto m = blockWith(depthCell, 0.6f);
                m->set(rateCell, ModFxBlock::page().params[(size_t) rateCell].fromNorm(0.5f));
                m->set(cell, v);
                return m;
            };
            auto atLow = make3(low), atHigh = make3(high), moved = make3(low);
            moved->modulateCell(cell, 0.4f);
            expectWithinAbsoluteError(moved->played(cell), high, 1.0e-4f, juce::String(spec.label) + " plays knob + offset");
            expectWithinAbsoluteError(moved->get(cell), low, 1.0e-5f);
            const Audio a = sound(*atLow), b = sound(*atHigh), c = sound(*moved);
            expect(maxDiff(b, c) < 2.0e-3f, juce::String(spec.label) + ": the offset sounds like the knob turned");
            expect(difference(a, b) > 1.0e-3f, juce::String(spec.label) + " is heard");
        }
        {
            // A depth at 0 with an offset on it: the block wakes.
            ModFxBlock m;
            m.prepare(SR, BLOCK);
            expect(! m.isActive());
            m.modulateCell(ModFxBlock::ChorusDepth, 0.5f);
            expect(m.isActive());
            m.modulateCell(ModFxBlock::ChorusDepth, 0.0f);
            expect(! m.isActive());
        }
    }

    // ---- 12: what the review found ------------------------------------------------------------------
    // The largest change of slope from one sample to the next (a jump in level is one too) in [from, to).
    static float bend(const Audio& a, int from, int to)
    {
        float m = 0.0f;
        for (auto* ch : { &a.l, &a.r })
            for (int i = std::max(2, from); i < std::min(to, (int) ch->size()); ++i)
                m = std::max(m, std::abs((*ch)[(size_t) i] - 2.0f * (*ch)[(size_t) i - 1] + (*ch)[(size_t) i - 2]));
        return m;
    }
    static Gen level(float v) { return [v](int, int) { return v; }; }

    void review()
    {
        beginTest("Core: a voice that joins the others comes up from nothing, also with the line full (feedback at its top)");
        for (bool mono : { false, true })
        {
            MixedCore c = core(1, 0.01f, 0.004f, mono);
            c.setFeedback(0.95f);
            const int at = 500 * BLOCK;
            const Audio out = render(c, at + 9600, level(0.5f), BLOCK, [&](int b) { if (b == 500) c.setVoices(6); });
            const Steps st = steps(out, { at }, 4800, at - 4800);
            expect(st.peak > 4.0f, "the line is full (held at FB_LIMIT): " + juce::String(st.peak));
            expect(st.atEdges < 0.05f, juce::String(mono ? "one line" : "two lines") + ": largest step as five voices join " + juce::String(st.atEdges, 4));
        }

        beginTest("Core: voices that start together drift apart by their rate spread alone; a mix set at once is glided");
        for (float spread : { 0.5f, 0.0f })
        {
            MixedCore c = core(3, 0.008f, 0.004f);
            c.setSpread(spread, 0.0f, 0.0f);            // no phase spread: only the rates set them apart
            c.reset();
            render(c, 9600, noise(0.2f));
            const float apart = std::abs(c.tapDelay(1, 0) - c.tapDelay(0, 0));
            if (spread > 0.0f) expect(apart > 20.0f, "half as fast again: " + juce::String(apart) + " samples apart after 0.2 s");
            else               expect(apart < 0.01f, "no spread: together, " + juce::String(apart));
        }
        {
            // (The mix is the effect's: CellEffect goes to a new one in a straight line, a full swing in 4 ms.)
            Vibrato fx;
            fx.prepare(SR, BLOCK);
            fx.setParameter(Vibrato::Depth, 0.0f);      // a clean delay of BASE_DELAY samples
            fx.setMix(0.0f);
            fx.reset();
            const int at = 200 * BLOCK;
            const Audio out = render(fx, at + 9600, sine(220.0, 0.5f), BLOCK, [&](int b) { if (b == 200) fx.setMix(1.0f); });
            const float before = bend(out, at - 4800, at), during = bend(out, at, at + 4800);
            expect(out.l[(size_t) at - 1] == sine(220.0, 0.5f)(at - 1, 0), "mix 0 is the input, to the bit");
            expectWithinAbsoluteError(out.l[(size_t) at + 9000], sine(220.0, 0.5f)(at + 9000 - Vibrato::BASE_DELAY, 0), 1.0e-3f, "the wet sound is the input two samples late");
            expect(std::abs(out.l[(size_t) at + 96] - sine(220.0, 0.5f)(at + 96, 0)) > 1.0e-4f
                   && std::abs(out.l[(size_t) at + 96] - sine(220.0, 0.5f)(at + 96 - Vibrato::BASE_DELAY, 0)) > 1.0e-4f, "half way after 2 ms");
            expect(during < 0.004f, "largest change of slope as the mix goes up " + juce::String(during, 5) + " (at rest " + juce::String(before, 5) + ")");
        }

        beginTest("Left in silence every one of the ten comes to rest at nothing, not at a number too small to be one");
        for (auto& k : kinds())
            for (int end = 0; end < 2; ++end)
            {
                auto fx = make(k);
                for (int cell = 0; cell < 8; ++cell)
                {
                    const ParamSpec spec = target(*fx).cellSpec(cell);
                    if (spec.isValid()) setCell(*fx, cell, end == 0 ? spec.minVal : spec.maxVal);
                }
                fx->setMix(0.5f);
                render(*fx, 24000, noise(0.5f), 512);
                const int n = 6 * 48000;
                const Audio rest = render(*fx, n, level(0.0f), 512);
                int small = 0;
                float last = 0.0f;
                for (auto* ch : { &rest.l, &rest.r })
                    for (int i = n - 48000; i < n; ++i)
                    {
                        const float v = std::abs((*ch)[(size_t) i]);
                        small += (v != 0.0f && v < std::numeric_limits<float>::min()) ? 1 : 0;
                        last = std::max(last, v);
                    }
                expectEquals(small, 0, juce::String(k.name) + " setting " + juce::String(end) + ": samples below the smallest normal number");
                expect(last < 1.0e-4f, juce::String(k.name) + " setting " + juce::String(end) + ": still sounding, " + juce::String(last));
            }

        beginTest("The same sound whatever the block size (1, 16, 64, 4096) while cells and WET are moved under it");
        for (auto& k : kinds())
        {
            std::vector<Audio> heard;
            for (int block : { 1, 16, 64, 4096 })
            {
                auto fx = make(k);
                auto move = [&](float norm, float wet)
                {
                    for (int cell = 0; cell < 8; ++cell)
                        if (target(*fx).modulates(cell))
                            setCell(*fx, cell, target(*fx).cellSpec(cell).fromNorm(norm));
                    fx->setMix(wet);
                };
                heard.push_back(render(*fx, 6 * 4096, chord(), block, [&](int b)
                {
                    if (b * block == 2 * 4096) move(0.85f, 0.6f);
                    if (b * block == 4 * 4096) move(0.2f, 1.0f);
                }));
            }
            for (size_t i = 1; i < heard.size(); ++i)
                expect(maxDiff(heard[0], heard[i]) < 1.0e-5f, juce::String(k.name) + ", block size " + juce::String((int) i) + ": " + juce::String(maxDiff(heard[0], heard[i]), 7));
        }

        beginTest("Tremolo and auto-pan: a DPTH set at once is rounded over a millisecond, a WET set at once glides");
        for (auto type : { EffectType::TremoloEffect, EffectType::AutoPanEffect })
            for (int way = 0; way < 2; ++way)
            {
                auto fx = AudioEngine::createEffect(type);
                fx->prepare(SR, BLOCK);
                fx->setParameter(0, 0.1f);                      // RATE: the LFO all but stands, in the middle of its swing
                fx->setParameter(1, way == 0 ? 0.0f : 1.0f);    // DPTH
                fx->reset();
                const int at = 100 * BLOCK;
                const Audio out = render(*fx, at + 4800, level(0.5f), BLOCK, [&](int b)
                {
                    if (b == 100) { if (way == 0) fx->setParameter(1, 1.0f); else fx->setMix(0.0f); }
                });
                const float moved = std::abs(out.l[(size_t) at + 2400] - out.l[(size_t) at - 1]);
                const float during = bend(out, at, at + 2400);
                const juce::String what = juce::String(AudioEngine::effectTypeName(type)) + (way == 0 ? " DPTH" : " WET");
                expect(moved > 0.15f, what + " moves the level by " + juce::String(moved, 3));
                // (In one straight line over a control step it would be the move / 32, about 0.007.)
                expect(during < (way == 0 ? 0.005f : 0.003f), what + ": largest change of slope " + juce::String(during, 5));
            }

        beginTest("Prepared for a rate no device has (0, not a number, 1e12), each of the ten still plays");
        for (auto& k : kinds())
            for (double sr : { 0.0, -48000.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1.0e12 })
            {
                auto fx = make(k, sr);
                const Audio out = render(*fx, 9600, chord());
                expect(finite(out, 8.0f), juce::String(k.name) + " prepared for " + juce::String(sr));
            }

        beginTest("Rotary: a turned XOVR arrives over a few milliseconds");
        {
            RotarySpeaker fx;
            fx.prepare(SR, BLOCK);
            fx.setExtra(RotarySpeaker::Crossover, 500.0f);
            fx.reset();
            const int at = 400 * BLOCK;
            const Audio out = render(fx, at + 24000, sine(110.0, 0.5f), BLOCK, [&](int b) { if (b == 400) fx.setExtra(RotarySpeaker::Crossover, 2000.0f); });
            const float before = bend(out, at - 12000, at), during = bend(out, at, at + 12000);
            expect(during < 0.009f, "largest change of slope " + juce::String(during, 5) + " (at rest " + juce::String(before, 5) + ")");
        }

        beginTest("MODFX: a depth taken to nothing at once lets the effect's mix finish its glide; the cells are read without building a page");
        for (int cell = 0; cell < ModFxBlock::NumCells; ++cell)
        {
            const ParamSpec a = ModFxBlock::spec(cell), b = ModFxBlock::page().params[(size_t) cell];
            expect(a.id == b.id && a.minVal == b.minVal && a.maxVal == b.maxVal && a.defaultVal == b.defaultVal && a.kind == b.kind
                   && juce::String(a.label) == juce::String(b.label));
        }
        for (int depthCell : { (int) ModFxBlock::ChorusDepth, (int) ModFxBlock::FlangerDepth, (int) ModFxBlock::PhaserDepth, (int) ModFxBlock::RotaryDepth })
            for (int way = 0; way < 2; ++way)       // an offset from outside, the knob (a step lock)
            {
                ModFxBlock m;
                m.prepare(SR, BLOCK);
                m.set(depthCell, 0.8f);
                const int at = 300 * BLOCK, n = at + 24000;
                const Gen in = sine(330.0, 0.5f);
                const Audio out = renderWith([&m](float* l, float* r, int count) { if (m.isActive()) m.process(l, r, count); }, n, in, BLOCK,
                                             [&](int b) { if (b == 300) { if (way == 0) m.modulateCell(depthCell, -1.0f); else m.set(depthCell, 0.0f); } });
                const Steps st = steps(out, { at }, 2400, 2400);
                expect(st.atEdges <= 1.3f * st.elsewhere, "depth cell " + juce::String(depthCell) + ", way " + juce::String(way) + ": " + st.text());
                expect(! m.isActive(), "and then it rests");
                expectWithinAbsoluteError(out.l[(size_t) n - 1], in(n - 1, 0), 1.0e-6f);
            }

        beginTest("In a chain, in series, side by side and mixed: finite, sounding, and with every WET down the sound itself");
        for (auto routing : { EffectChain::Routing::Series, EffectChain::Routing::Parallel, EffectChain::Routing::Mixed })
            for (size_t first = 0; first < kinds().size(); ++first)
            {
                EffectChain chain;
                chain.prepare(SR, BLOCK);
                chain.setRouting(routing);
                for (size_t i = 0; i < 3; ++i)
                {
                    const Kind& k = kinds()[(first + i) % kinds().size()];
                    auto fx = make(k);
                    fx->setMix(0.5f);
                    chain.addEffect(std::move(fx));
                }
                auto run = [&](int n) { return renderWith([&chain](float* l, float* r, int count) { chain.processStereo(l, r, count); }, n, chord()); };
                const Audio out = run(24000);
                const juce::String what = juce::String(kinds()[first].name) + " first, routing " + juce::String((int) routing);
                expect(finite(out, 8.0f), what);
                expect(rms(out.l, 2400) > 0.01f && rms(out.r, 2400) > 0.01f, what + ": sounding");
                if (routing == EffectChain::Routing::Series)
                {
                    for (int i = 0; i < 3; ++i)
                        chain.getEffect(i)->setMix(0.0f);
                    run(4800);
                    const Audio dry = renderWith([](float*, float*, int) {}, 9600, chord());
                    const Audio through = run(9600);
                    // (The ensemble and the rotary hold their input to +-64 and nothing else; the wah
                    // and the vibrato now obey WET.)
                    expect(maxDiff(through, dry) < 1.0e-5f, what + ": WET down, " + juce::String(maxDiff(through, dry), 6));
                }
            }

        beginTest("In the instrument: each of the ten as a track's insert (its cells reached by id) and as the send's effect");
        for (auto& k : kinds())
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.prepare(SR, BLOCK);
            audio.addInsertEffect(0, k.type);
            auto* fx = audio.getInsertChain(0) != nullptr ? audio.getInsertChain(0)->getEffect(0) : nullptr;
            expect(fx != nullptr && cellsOf(fx) != nullptr, juce::String(k.name) + ": a target");
            if (fx == nullptr) continue;
            for (int cell = 0; cell < 8; ++cell)
            {
                const ParamSpec spec = target(*fx).cellSpec(cell);
                if (! spec.isValid()) continue;
                const int id = cell < 4 ? AudioEngine::EFFECT_PARAM_BASE + cell : AudioEngine::INSERT_EXTRA_BASE + cell - 4;
                audio.setTrackParameter(0, id, spec.maxVal);
                expectWithinAbsoluteError(getCell(*fx, cell), spec.maxVal, 1.0e-4f * std::max(1.0f, std::abs(spec.maxVal)), juce::String(k.name) + " " + spec.label);
                expectWithinAbsoluteError(audio.getTrackParameter(0, id), spec.maxVal, 1.0e-4f * std::max(1.0f, std::abs(spec.maxVal)));
            }
            audio.setInsertEffect(AudioEngine::sendTrack(0), 0, k.type);
            audio.setTrackParameter(0, MixParam::SEND_BASE, 0.7f);
            audio.getEngine(0)->noteOn(57, 0.8f);
            const Audio out = renderWith([&audio](float* l, float* r, int count) { audio.renderBlock(l, r, count); }, 24000, level(0.0f));
            expect(finite(out, 8.0f), juce::String(k.name) + " in the instrument");
            expect(rms(out.l, 4800) > 1.0e-3f || rms(out.r, 4800) > 1.0e-3f, juce::String(k.name) + ": the track is heard");
        }
    }

    // ---- cost ------------------------------------------------------------------------------------
    // The settings the cost is taken at: what each effect was before this phase (its four cells as
    // its page gave them, three chorus voices, six phaser stages, the wah's and the ring
    // modulator's LFO running), half wet.
    static void oldSettings(EffectType type, EffectBase& fx)
    {
        for (int i = 0; i < 4; ++i)
        {
            const ParamSpec spec = fx.getParamSpec(i);
            if (spec.isValid())
                fx.setParameter(i, spec.defaultVal);
        }
        if (type == EffectType::ChorusEffect) fx.setParameter(3, 3.0f);
        if (type == EffectType::PhaserEffect) fx.setParameter(3, 6.0f);
        if (type == EffectType::WahEffect)    fx.setParameter(3, 2.0f);
        if (type == EffectType::RingModEffect) fx.setParameter(2, 0.5f);
        fx.setMix(0.5f);
    }

    // ---- 13: one mechanism (CellEffect.h) ---------------------------------------------------------
    // An effect of cells that only writes down what the base asks of it.
    struct Probe : CellEffect
    {
        struct Run { int at, n; float mixFrom, mixTo; };
        std::vector<int> configuredAt, controlledAt;
        std::vector<Run> runs;
        std::array<float, CELLS> last {};
        int clockNow = 0;
        bool rest = false;

        Probe() { seed(); prepare(SR, BLOCK); forget(); }
        void forget() { configuredAt.clear(); controlledAt.clear(); runs.clear(); clockNow = 0; }
        void touched() { touch(); }
        float clocked() { float l = 0.0f, r = 0.0f; const float m = clock(l, r); ++clockNow; return m; }

        const ParamSpec* cells() const override
        {
            static const char* const names[] = { "A", "B", "C" };
            static const ParamSpec c[CELLS] = {
                { "LIN", 0, -20.0f, 20.0f, 0.0f },
                { "LOG", 1, 20.0f, 20000.0f, 200.0f, ParamKind::Log, "Hz" },
                { "MIX", 2, 0, 1, 0.5f },
                { "INT", 3, 1, 6, 2, ParamKind::Int },
                { "LIST", 0, 0, 2, 0, ParamKind::Choice, "", names, 3 },
                {},
                { "TIME", 2, 0.01f, 1.0f, 0.1f, ParamKind::Log, "s" },
                {} };
            return c;
        }
        int mixCell() const override { return 2; }
        void configure(const float* v) override { std::copy(v, v + CELLS, last.begin()); configuredAt.push_back(clockNow); }
        void prepareCore() override {}
        void resetCore() override {}
        void controlCore() override { controlledAt.push_back(clockNow); }
        bool coasting() const override { return rest; }
        void runCore(float* left, float* right, int n, float mixFrom, float mixTo) override
        {
            runs.push_back({ clockNow, n, mixFrom, mixTo });
            // (The mix as a sound: silence in, the mix's own line out.)
            for (int i = 0; i < n; ++i)
                left[i] = right[i] = mixFrom + (mixTo - mixFrom) * (float) (i + 1) / (float) n;
            clockNow += n;
        }
    };

    static Buf through(Probe& fx, int total, int block, const std::function<void(int)>& before = nullptr)
    {
        Buf l((size_t) total, 0.0f), r((size_t) total, 0.0f);
        for (int done = 0; done < total; done += block)
        {
            if (before) before(done);
            fx.processBlock(l.data() + done, r.data() + done, std::min(block, total - done));
        }
        return l;
    }

    // DIRT as an insert against the block it was (DirtBlock: TrackFx.h's, the same text as
    // tests/old/OldTrackFx.h): a stage that is set to nothing is not run, so each stage alone,
    // and all of them, give the block's samples.
    void dirtStages()
    {
        beginTest("DIRT: the insert is the old block, each stage alone and all together (a stage set to nothing is not run: the shaper at DRIV 0 is not a wire)");
        {
            struct Case { const char* name; float driv, type, bits, rate, fdbk, tune, damp, mix; };
            const Case cases[] = {
                { "DRIV alone",          0.6f, 1, 0.0f, 0.0f, 0.0f, 220.0f, 0.3f, 1.0f },
                { "BITS alone",          0.0f, 0, 0.7f, 0.0f, 0.0f, 220.0f, 0.3f, 1.0f },
                { "RATE alone",          0.0f, 0, 0.0f, 0.5f, 0.0f, 220.0f, 0.3f, 1.0f },
                { "BITS and RATE",       0.0f, 0, 0.6f, 0.3f, 0.0f, 220.0f, 0.3f, 1.0f },
                { "FDBK alone",          0.0f, 0, 0.0f, 0.0f, 0.7f, 500.0f, 0.1f, 1.0f },
                { "all three",           0.6f, 2, 0.5f, 0.4f, 0.6f, 330.0f, 0.4f, 1.0f },
                { "all three, MIX 0.5",  0.6f, 2, 0.5f, 0.4f, 0.6f, 330.0f, 0.4f, 0.5f },
                { "BITS alone, MIX 0.5", 0.0f, 0, 0.7f, 0.0f, 0.0f, 220.0f, 0.3f, 0.5f } };
            const int n = 48000;
            const Audio in = renderWith([](float*, float*, int) {}, n, chord());
            for (const auto& c : cases)
            {
                const float cells[DirtBlock::NumCells] = { c.driv, c.type, c.bits, c.rate, c.fdbk, c.tune, c.damp, c.mix };
                DirtBlock block;
                block.prepare(SR, BLOCK);
                for (int cell = 0; cell < DirtBlock::NumCells; ++cell)
                    block.set(cell, cells[cell]);
                Audio old = in;
                for (int done = 0; done < n; done += BLOCK)
                    block.process(old.l.data() + done, old.r.data() + done, std::min(BLOCK, n - done));

                EffectChain chain;
                chain.prepare(SR, BLOCK);
                auto fx = AudioEngine::createEffect(EffectType::DirtInsertEffect);
                for (int cell = 0; cell < 4; ++cell)
                {
                    fx->setParameter(cell, cells[cell]);
                    fx->setExtra(cell, cells[4 + cell]);
                }
                chain.addEffect(std::move(fx));
                chain.reset();
                Audio now = in;
                for (int done = 0; done < n; done += BLOCK)
                    chain.processStereo(now.l.data() + done, now.r.data() + done, std::min(BLOCK, n - done));

                int differ = 0;
                double apart = 0.0, size = 0.0, moved = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    differ += (old.l[(size_t) i] != now.l[(size_t) i] || old.r[(size_t) i] != now.r[(size_t) i]) ? 1 : 0;
                    apart += (double) (old.l[(size_t) i] - now.l[(size_t) i]) * (old.l[(size_t) i] - now.l[(size_t) i]);
                    size += (double) old.l[(size_t) i] * old.l[(size_t) i];
                    moved += (double) (old.l[(size_t) i] - in.l[(size_t) i]) * (old.l[(size_t) i] - in.l[(size_t) i]);
                }
                const double rel = size > 0.0 ? std::sqrt(apart / size) : 0.0, acts = size > 0.0 ? std::sqrt(moved / size) : 0.0;
                logMessage("  DIRT " + juce::String(c.name) + ": " + juce::String(differ) + " of " + juce::String(n) + " samples differ from the block, apart by "
                           + juce::String(rel, 8) + " of its level (the stage moves the sound by " + juce::String(acts, 4) + ")");
                expect(differ == 0, juce::String("DIRT ") + c.name + ": " + juce::String(differ) + " samples differ from the old block, by " + juce::String(rel, 8));
                expect(acts > 0.001, juce::String("DIRT ") + c.name + " does something: " + juce::String(acts));
            }

            // What the shaper at DRIV 0 does to a sound (what "bits only" went through before the
            // stages were gated): not nothing.
            Waveshaper shaper;
            shaper.prepare(SR, BLOCK);
            shaper.setParameter(Waveshaper::Drive, 0.0f);
            shaper.setParameter(Waveshaper::Curve, 0.0f);
            shaper.setParameter(Waveshaper::Tone, 0.8f);
            shaper.setParameter(Waveshaper::Asymmetry, 0.5f);
            shaper.setParameter(Waveshaper::Mix, 1.0f);
            Audio shaped = in;
            shaper.processBlock(shaped.l.data(), shaped.r.data(), n);
            double apart = 0.0, size = 0.0;
            for (int i = 0; i < n; ++i)
            {
                apart += (double) (shaped.l[(size_t) i] - in.l[(size_t) i]) * (shaped.l[(size_t) i] - in.l[(size_t) i]);
                size += (double) in.l[(size_t) i] * in.l[(size_t) i];
            }
            const double colour = std::sqrt(apart / size);
            logMessage("  the shaper at DRIV 0 moves a chord by " + juce::String(colour, 4) + " of its level");
            expect(colour > 0.01, "the shaper at DRIV 0 is not a wire: " + juce::String(colour));
        }

        beginTest("RED: the steps of BITS are worked out when BITS is set; every setting gives the samples of the power worked out each sample");
        {
            const int n = 4800;
            const Audio in = renderWith([](float*, float*, int) {}, n, chord());
            for (float bits : { 0.0f, 0.25f, 0.5f, 0.9f, 1.0f })
            {
                RateReducer red;
                red.prepare(SR, BLOCK);
                red.setParameter(RateReducer::RateReduction, 0.0f);
                red.setParameter(RateReducer::PreFilter, 0.0f);
                red.setParameter(RateReducer::Mix, 1.0f);
                red.setParameter(RateReducer::BitDepth, bits);
                Audio out = in;
                red.processBlock(out.l.data(), out.r.data(), n);
                const float levels = std::pow(2.0f, 16.0f - bits * 15.0f), inv = 1.0f / levels;
                int differ = 0;
                for (int i = 0; i < n; ++i)
                    differ += out.l[(size_t) i] != std::floor(in.l[(size_t) i] * levels + 0.5f) * inv ? 1 : 0;
                expectEquals(differ, 0, "BITS " + juce::String(bits, 2));
            }
            // As it is made (BITS 0, sixteen bits), with nothing set.
            RateReducer fresh;
            fresh.prepare(SR, BLOCK);
            fresh.setParameter(RateReducer::RateReduction, 0.0f);
            fresh.setParameter(RateReducer::PreFilter, 0.0f);
            float l = 0.123456789f, r = l;
            fresh.process(l, r);
            expectEquals(l, std::floor(0.123456789f * 65536.0f + 0.5f) * (1.0f / 65536.0f));
        }
    }

    void oneMechanism()
    {
        beginTest("One mechanism: every effect built on a shared core is a CellEffect (since E7 the send delay, the send reverb and the hall too); the plain ones are not, and ignore modulateCell");
        {
            const std::vector<EffectType> ofCells = {
                EffectType::ChorusEffect, EffectType::FlangerEffect, EffectType::PhaserEffect, EffectType::TremoloEffect, EffectType::EnsembleEffect,
                EffectType::AutoPanEffect, EffectType::RotaryEffect, EffectType::RingModEffect, EffectType::VibratoEffect, EffectType::WahEffect,
                EffectType::FilterEffectType,       // (phase E6: a setting of the swept filter)
                EffectType::TapeDelayEffect, EffectType::LCRDelayEffect, EffectType::MultiTapEffect, EffectType::ReverseDelayEffect,
                EffectType::SpringReverbEffect, EffectType::ShimmerReverbEffect,
                EffectType::DelayEffect, EffectType::ReverbEffect, EffectType::HallReverbEffect,      // (E7)
                EffectType::CompressorEffect, EffectType::SidechainEffect, EffectType::LimiterEffect, EffectType::TransientEffect,
                EffectType::EQEffect, EffectType::GraphicEQEffect,
                EffectType::DirtInsertEffect, EffectType::TalkInsertEffect };
            int found = 0;
            for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
            {
                auto fx = AudioEngine::createEffect((EffectType) t);
                if (fx == nullptr) continue;
                const bool wanted = std::find(ofCells.begin(), ofCells.end(), (EffectType) t) != ofCells.end();
                const juce::String name = AudioEngine::effectTypeName((EffectType) t);
                CellEffect* cells = cellsOf(fx.get());
                expect((cells != nullptr) == wanted, name + (wanted ? " is an effect of cells" : " is not"));
                found += cells != nullptr ? 1 : 0;
                // Whatever it is: every target, every offset, and it still plays.
                fx->prepare(SR, BLOCK);
                for (int cell = -1; cell <= CellEffect::TARGETS; ++cell)
                    for (float offset : { 1.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
                        fx->modulateCell(cell, offset);
                fx->setUserShape(nullptr);
                expect(finite(render(*fx, 4800, chord()), 64.0f), name + " with every cell moved");
                if (cells == nullptr) continue;
                for (int cell = 0; cell < CellEffect::CELLS; ++cell)
                {
                    const ParamSpec spec = cells->cellSpec(cell);
                    expect(cells->moves(cell) == (spec.isValid() && ! spec.isDiscrete()), name + " cell " + juce::String(cell));
                    expectEquals(cells->getModulation(cell), 0.0f, name + ": what is not a number took the offset away");
                    // The page and the modulation see the same cell.
                    const ParamSpec page = cell < 4 ? fx->getParamSpec(cell) : fx->getExtraSpec(cell - 4);
                    expect(page.id == spec.id && page.minVal == spec.minVal && page.maxVal == spec.maxVal && page.kind == spec.kind);
                }
                expect(cells->moves(CellEffect::WET) && ! cells->moves(CellEffect::TARGETS) && ! cells->moves(-1));
            }
            expectEquals(found, (int) ofCells.size());
            expectEquals((int) CellEffect::WET, (int) EffectBase::WET_CELL);
        }

        beginTest("CellEffect::moved: a share of the range, by ratio on a logarithmic cell, held to the range; lists and counts stay; 0 is the value to the bit");
        {
            Probe fx;
            const ParamSpec lin = fx.cellSpec(0), log = fx.cellSpec(1), count = fx.cellSpec(3), list = fx.cellSpec(4);
            expectWithinAbsoluteError(CellEffect::moved(lin, -10.0f, 0.25f), 0.0f, 1.0e-5f);
            expectWithinAbsoluteError(CellEffect::moved(lin, 10.0f, 0.5f), 20.0f, 1.0e-5f, "held at the top");
            expectWithinAbsoluteError(CellEffect::moved(log, 200.0f, 1.0f / 3.0f), 2000.0f, 0.5f, "a third of three decades");
            expectWithinAbsoluteError(CellEffect::moved(log, 200.0f, -1.0f), 20.0f, 1.0e-3f, "held at the bottom");
            expect(CellEffect::moved(lin, 3.3333333f, 0.0f) == 3.3333333f && CellEffect::moved(log, 123.4567f, 0.0f) == 123.4567f);
            expectEquals(CellEffect::moved(count, 2.0f, 1.0f), 2.0f);
            expectEquals(CellEffect::moved(list, 1.0f, -1.0f), 1.0f);
            expectEquals(CellEffect::moved(lin, 1.0f, std::numeric_limits<float>::quiet_NaN()), 1.0f);
            expectEquals(CellEffect::moved(ParamSpec(), 1.0f, 0.5f), 1.0f, "no cell");
            expectEquals(CellEffect::offsetOf(3.0f), 1.0f);
            expectEquals(CellEffect::offsetOf(-3.0f), -1.0f);
            expectEquals(CellEffect::offsetOf(-std::numeric_limits<float>::infinity()), 0.0f);

            // What configure() is handed is the cells moved; the cells read as they were set.
            fx.setParameter(0, -10.0f);
            fx.modulateCell(0, 0.25f);
            fx.modulateCell(1, 1.0f / 3.0f);
            fx.modulateCell(3, 1.0f);
            fx.modulateCell(4, 1.0f);
            fx.modulateCell(5, 1.0f);       // (no cell)
            through(fx, 64, 64);
            expectWithinAbsoluteError(fx.last[0], 0.0f, 1.0e-5f);
            expectWithinAbsoluteError(fx.last[1], 2000.0f, 0.5f);
            expectEquals(fx.last[3], 2.0f);
            expectEquals(fx.last[4], 0.0f);
            expectEquals(fx.getParameter(0), -10.0f);
            expectEquals(fx.getModulation(3), 0.0f, "a count takes no offset");
            expectEquals(fx.getModulation(5), 0.0f);
            expectWithinAbsoluteError(fx.getMovedCell(1), 2000.0f, 0.5f);
        }

        beginTest("The clock: the core is stepped every CONTROL samples counted from reset(), whatever the block size; the cells go into it only when one is worth something new");
        for (int block : { 1, 7, 32, 64, 100, 4096 })
        {
            Probe fx;
            fx.reset();
            fx.forget();
            const int total = 4096, turnedAt = 1000;
            through(fx, total, block, [&](int done) { if (done <= turnedAt && turnedAt < done + block) fx.setParameter(0, 5.0f); });
            bool onGrid = fx.controlledAt.size() == (size_t) (total / CellEffect::CONTROL);
            for (size_t i = 0; i < fx.controlledAt.size(); ++i)
                onGrid = onGrid && fx.controlledAt[i] == (int) i * CellEffect::CONTROL;
            expect(onGrid, "block " + juce::String(block) + ": " + juce::String((int) fx.controlledAt.size()) + " steps");
            bool within = true;
            for (const auto& run : fx.runs)
                within = within && run.n >= 1 && run.at / CellEffect::CONTROL == (run.at + run.n - 1) / CellEffect::CONTROL;
            expect(within, "no run crosses a step");
            // One turn, one configure: at the first look after the block that brought it.
            const int blockStart = (turnedAt / block) * block;
            const int look = ((blockStart + CellEffect::CONTROL - 1) / CellEffect::CONTROL) * CellEffect::CONTROL;
            expect(fx.configuredAt.size() == 1 && fx.configuredAt[0] == look,
                   "block " + juce::String(block) + ": configured " + juce::String((int) fx.configuredAt.size()) + " times"
                   + (fx.configuredAt.empty() ? juce::String() : " at " + juce::String(fx.configuredAt[0])) + ", look at " + juce::String(look));
            expectEquals(fx.last[0], 5.0f);
        }
        {
            // reset() in the middle of a step: the count begins again with the next sample, where the
            // core's own counts (its LFOs) begin too.
            Probe fx;
            through(fx, 45, 45);
            fx.reset();
            fx.forget();
            fx.setParameter(0, 7.0f);
            through(fx, 96, 96);
            expect(fx.controlledAt.size() == 3 && fx.controlledAt[0] == 0 && fx.controlledAt[1] == CellEffect::CONTROL && fx.controlledAt[2] == 2 * CellEffect::CONTROL,
                   "the steps after a reset fall on its own count");
            expect(fx.configuredAt.size() == 1 && fx.configuredAt[0] == 0, "a cell set after a reset is in the core with the first sample");
        }
        {
            // The same value again, a value that is not a number, a cell that is none: nothing to configure.
            Probe fx;
            through(fx, 64, 64);
            fx.forget();
            fx.setParameter(0, 0.0f);
            fx.setParameter(1, std::numeric_limits<float>::quiet_NaN());
            fx.setExtra(1, 0.7f);
            fx.setParameter(9, 0.7f);
            fx.modulateCell(0, 0.0f);
            through(fx, 256, 64);
            expectEquals((int) fx.configuredAt.size(), 0);
            // touch(): something beside the cells changed. A new sample rate too.
            fx.touched();
            through(fx, 64, 64);
            expectEquals((int) fx.configuredAt.size(), 1);
            fx.prepare(96000.0, 64);
            expectEquals((int) fx.configuredAt.size(), 2);
            // Held to the range, a count rounded.
            fx.setParameter(0, 1.0e9f);
            fx.setParameter(3, 3.6f);
            expectEquals(fx.getParameter(0), 20.0f);
            expectEquals(fx.getParameter(3), 4.0f);
        }

        beginTest("The clock, one sample at a time (the master section) and at rest (a block in one run): the same steps and the same looks");
        {
            Probe byBlock, bySample;
            byBlock.reset(); bySample.reset();
            byBlock.forget(); bySample.forget();
            through(byBlock, 2048, 64, [&](int done) { if (done == 960) byBlock.setParameter(0, 5.0f); });
            for (int i = 0; i < 2048; ++i)
            {
                if (i == 960) bySample.setParameter(0, 5.0f);
                bySample.clocked();
            }
            expect(byBlock.controlledAt == bySample.controlledAt && byBlock.configuredAt == bySample.configuredAt && ! bySample.controlledAt.empty());
        }
        for (int block : { 7, 64, 100, 512 })
        {
            Probe stepped, resting;
            resting.rest = true;
            for (auto* fx : { &stepped, &resting }) { fx->reset(); fx->forget(); }
            const auto turn = [&](Probe& fx) { return [&fx, block](int done) { if (done / block == 5) fx.setParameter(0, 5.0f); if (done / block == 9) fx.setMix(1.0f); }; };
            const Buf a = through(stepped, 8192, block, turn(stepped)), b = through(resting, 8192, block, turn(resting));
            // (At rest: up to the first step in a block, then the rest of it; a few more while the mix is on its way.)
            const size_t blocks = (size_t) (8192 / block + 1);
            expect(stepped.runs.size() >= (size_t) (8192 / CellEffect::CONTROL) && resting.runs.size() <= 2 * blocks + 8
                   && (block < 64 || resting.runs.size() < stepped.runs.size()),
                   "block " + juce::String(block) + ": at rest " + juce::String((int) resting.runs.size()) + " runs against " + juce::String((int) stepped.runs.size()));
            // (Two looks find something new, the cell and the mix; with the shortest block one look finds both.)
            expect(resting.configuredAt == stepped.configuredAt && resting.configuredAt.size() == (block == 7 ? 1u : 2u),
                   "block " + juce::String(block) + ": the cell is seen at the same sample");
            expect(a == b, "block " + juce::String(block) + ": the mix takes the same line");
            // (After a run that ended between two steps the count goes on where it would be.)
            resting.rest = false;
            resting.forget();
            const int phase = 8192 % CellEffect::CONTROL;
            through(resting, 64, 64);
            expect(! resting.controlledAt.empty() && resting.controlledAt[0] == (phase == 0 ? 0 : CellEffect::CONTROL - phase));
        }

        beginTest("The mix: a straight line to where it is put, a full swing in MIX_SWING_SECONDS, whoever moved it (the cell, the slot's WET, modulation); "
                  "a small move is there within one look; after reset() it is where it is set; what is not a number is not taken");
        {
            const int swing = (int) std::lround(CellEffect::MIX_SWING_SECONDS * SR);
            for (int way = 0; way < 3; ++way)
            {
                Probe fx;
                fx.setMix(0.0f);
                fx.reset();
                Buf out = through(fx, 64, 64);
                expect(out[0] == 0.0f && out[63] == 0.0f, "after reset() the mix is where it is set");
                if (way == 0) fx.setParameter(2, 1.0f);
                if (way == 1) fx.setMix(1.0f);
                if (way == 2) fx.modulateCell(CellEffect::WET, 1.0f);
                out = through(fx, 1024, 64);
                expectWithinAbsoluteError(out[(size_t) swing / 2 - 1], 0.5f, 1.0e-3f, "way " + juce::String(way) + ": half way after half the time");
                expect(out[(size_t) swing - 2] < 1.0f && out[(size_t) swing - 1] == 1.0f, "way " + juce::String(way) + ": there, exactly, after " + juce::String(swing) + " samples");
                float worst = 0.0f;
                for (int i = 1; i < swing; ++i)
                    worst = std::max(worst, std::abs((out[(size_t) i] - out[(size_t) i - 1]) - 1.0f / (float) swing));
                expect(worst < 1.0e-5f, "way " + juce::String(way) + ": the same step every sample (" + juce::String(worst, 7) + ")");
                expect(out[1023] == 1.0f);
                expectEquals(fx.getMix(), way == 2 ? 0.0f : 1.0f, "an offset leaves the mix as it is set");
            }
            Probe fx;
            fx.reset();
            through(fx, 64, 64);
            fx.setMix(0.55f);
            const Buf out = through(fx, 64, 64);
            expect(out[0] > 0.5f && out[0] < 0.55f && out[(size_t) CellEffect::CONTROL - 1] == 0.55f, "a small move: there after one look");
            // Both offsets add to the mix, held to 0..1; the MIX cell and the slot's WET are one value.
            fx.modulateCell(2, 0.3f);
            fx.modulateCell(CellEffect::WET, 0.3f);
            expectEquals(fx.getMovedCell(CellEffect::WET), 1.0f);
            expectEquals(fx.getMovedCell(2), 1.0f);
            expectEquals(fx.getParameter(2), 0.55f);
            fx.setMix(std::numeric_limits<float>::quiet_NaN());
            expectEquals(fx.getMix(), 0.55f, "EffectBase::setMix: what is not a number is not taken");
            fx.setMix(std::numeric_limits<float>::infinity());
            expectEquals(fx.getMix(), 0.55f);
            Drive plain;
            plain.setMix(0.4f);
            plain.setMix(std::numeric_limits<float>::quiet_NaN());
            expectEquals(plain.getMix(), 0.4f, "nor by an effect that is not of cells");
        }

        beginTest("The slot's WET as modulateCell's ninth cell on the delays, reverbs and dynamics too: all the way down is the input");
        for (auto type : { EffectType::TapeDelayEffect, EffectType::LCRDelayEffect, EffectType::SpringReverbEffect, EffectType::ShimmerReverbEffect,
                           EffectType::CompressorEffect, EffectType::SidechainEffect, EffectType::TransientEffect, EffectType::WahEffect })
        {
            auto fx = AudioEngine::createEffect(type);
            fx->prepare(SR, BLOCK);
            fx->setMix(0.8f);
            if (type == EffectType::TransientEffect) fx->setParameter(0, 1.0f);
            const Audio dry = renderWith([](float*, float*, int) {}, 9600, chord());
            fx->reset();
            const Audio wet = render(*fx, 9600, chord());
            fx->modulateCell(CellEffect::WET, -1.0f);
            fx->reset();
            const Audio none = render(*fx, 9600, chord());
            const juce::String name = AudioEngine::effectTypeName(type);
            expect(maxDiff(none, dry) < 1.0e-6f, name + ": WET offset -1 is the input (" + juce::String(maxDiff(none, dry), 7) + ")");
            expect(difference(wet, dry) > 1.0e-3f, name + ": and without it the effect is heard");
            expectEquals(fx->getMix(), 0.8f);
        }

        beginTest("One sample at a time (as the master section runs its three) is the block, with a cell turned and the mix moved under sound");
        for (auto type : { EffectType::CompressorEffect, EffectType::LimiterEffect, EffectType::EQEffect, EffectType::TransientEffect,
                           EffectType::WahEffect, EffectType::ChorusEffect, EffectType::TapeDelayEffect })
        {
            auto byBlock = AudioEngine::createEffect(type), bySample = AudioEngine::createEffect(type);
            const juce::String name = AudioEngine::effectTypeName(type);
            const int total = 9600, turnedAt = 75 * BLOCK, mixedAt = 113 * BLOCK;
            const Gen in = [](int i, int side) { return (i % 2400 < 600 ? 1.6f : 0.2f) * (float) std::sin(TWO_PI * 330.0 * i / SR) * (side == 0 ? 1.0f : 0.8f); };
            const auto turn = [&](EffectBase& fx, int at)
            {
                // (A cell that is heard at once: the threshold, the ceiling, a band, the attack, the centre, the depth, the feedback.)
                if (at == turnedAt) fx.setParameter(type == EffectType::WahEffect || type == EffectType::CompressorEffect || type == EffectType::LimiterEffect
                                                    || type == EffectType::EQEffect || type == EffectType::TransientEffect ? 0 : 1,
                                                    type == EffectType::CompressorEffect ? -40.0f : type == EffectType::LimiterEffect ? -3.0f
                                                    : type == EffectType::EQEffect ? 12.0f : type == EffectType::WahEffect ? 1500.0f : 1.0f);
                if (at == mixedAt) fx.setMix(0.3f);
            };
            for (auto* fx : { byBlock.get(), bySample.get() }) { fx->prepare(SR, BLOCK); fx->reset(); }
            const Audio blocks = render(*byBlock, total, in, BLOCK, [&](int b) { turn(*byBlock, b * BLOCK); });
            Audio samples = renderWith([](float*, float*, int) {}, total, in);
            for (int i = 0; i < total; ++i)
            {
                turn(*bySample, i);
                bySample->process(samples.l[(size_t) i], samples.r[(size_t) i]);
            }
            // To the bit while the mix stands; while it is on its way the two add up the same line
            // in a different order (one step a sample, or a share of the stretch).
            float standing = 0.0f, moving = 0.0f;
            for (int i = 0; i < total; ++i)
            {
                const float d = std::max(std::abs(blocks.l[(size_t) i] - samples.l[(size_t) i]), std::abs(blocks.r[(size_t) i] - samples.r[(size_t) i]));
                (i < mixedAt ? standing : moving) = std::max(i < mixedAt ? standing : moving, d);
            }
            expect(standing == 0.0f && moving < 1.0e-5f, name + ": sample by sample differs from the block by " + juce::String(standing, 7) + ", " + juce::String(moving, 7));
            // And the turned cell is heard (it reached the core in both).
            auto plain = AudioEngine::createEffect(type);
            plain->prepare(SR, BLOCK);
            plain->reset();
            const Audio untouched = render(*plain, total, in);
            float before = 0.0f, after = 0.0f;
            for (int i = 0; i < total; ++i)
                (i < turnedAt ? before : after) = std::max(i < turnedAt ? before : after, std::abs(samples.l[(size_t) i] - untouched.l[(size_t) i]));
            expect(before == 0.0f && after > 1.0e-3f, name + ": the cell turned at " + juce::String(turnedAt) + " is heard from there on (" + juce::String(after, 5) + ")");
        }

        // (Review: the equalisers and the limiter kept what is not a number in their states for good,
        // the master section's limiter among them, and the compressors handed 1e30 on. The guard is
        // the base's now, before any core, by the block and by the sample.)
        beginTest("A sound that is not a number, infinite or 1e30 into every effect of cells, by the block and one sample at a time: "
                  "finite and held, and the next ordinary sound plays without a reset");
        {
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            float l[5] = { nan, inf, -1.0e30f, 0.25f, 64.0f }, r[5] = { -inf, 1.0e30f, nan, -0.5f, -64.0f };
            CellEffect::saneBlock(l, r, 5);
            expect(l[0] == 0.0f && l[1] == 64.0f && l[2] == -64.0f && l[3] == 0.25f && l[4] == 64.0f);
            expect(r[0] == -64.0f && r[1] == 64.0f && r[2] == 0.0f && r[3] == -0.5f && r[4] == -64.0f);

            const float bad[] = { nan, inf, -inf, 1.0e30f, -1.0e30f };
            const Gen sound = chord();
            const int n = 9600;
            const Gen dirty = [&](int i, int side)
            {
                return (i > n / 4 && i < n / 2 && (i % 97 == 0 || (i / 500) % 4 == 1)) ? bad[(size_t) ((i / 7 + side) % 5)] : sound(i, side);
            };
            int tried = 0;
            for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
                for (int bySample = 0; bySample < 2; ++bySample)
                {
                    auto fx = AudioEngine::createEffect((EffectType) t);
                    if (cellsOf(fx.get()) == nullptr) continue;
                    ++tried;
                    const juce::String name = juce::String(AudioEngine::effectTypeName((EffectType) t)) + (bySample ? " by the sample" : " by the block");
                    fx->prepare(SR, BLOCK);
                    fx->reset();
                    const Process run = [&](float* left, float* right, int count)
                    {
                        if (bySample) for (int i = 0; i < count; ++i) fx->process(left[i], right[i]);
                        else          fx->processBlock(left, right, count);
                    };
                    expect(finite(renderWith(run, n, dirty), 300.0f), name + ": finite and held");
                    renderWith(run, (int) SR, [](int, int) { return 0.0f; });
                    const Audio after = renderWith(run, n, sound);
                    expect(finite(after, 30.0f) && rms(after.l) > 1.0e-3f, name + ": plays on");
                }
            expectEquals(tried, 2 * 28);      // (22 + E7: the send delay, the send reverb and the hall + E6: the filter effect + DIRT + TALK inserts)
        }

        beginTest("Wah: RESO and SENS thrown under sound are glided to (they were jumped to)");
        for (int cell : { (int) Wah::Reso, (int) Wah::Sens })
        {
            Wah fx;
            fx.prepare(SR, BLOCK);
            fx.setParameter(Wah::Freq, 400.0f);
            fx.setParameter(Wah::Rate, 0.0f);
            fx.setParameter(Wah::Reso, cell == Wah::Reso ? 0.0f : 0.5f);
            fx.setParameter(Wah::Sens, cell == Wah::Sens ? 0.0f : 0.3f);
            fx.reset();
            const int first = 300 * BLOCK, second = 600 * BLOCK, total = 900 * BLOCK;
            const Audio out = render(fx, total, sine(400.0, 0.5f), BLOCK, [&](int b)
            {
                if (b * BLOCK == first)  fx.setParameter(cell, 1.0f);
                if (b * BLOCK == second) fx.setParameter(cell, 0.0f);
            });
            // The largest change of slope where it is thrown, against the sound at rest on either side.
            const float still = std::max(bend(out, first - 4800, first), bend(out, second - 4800, second));
            const float thrown = std::max(bend(out, first, first + 2400), bend(out, second, second + 2400));
            const juce::String what = cell == Wah::Reso ? "RESO" : "SENS";
            expect(std::abs(rms(out.l, second - 4800, second) - rms(out.l, first - 4800, first)) > 0.02f, what + " is heard");
            expect(thrown < 1.5f * still + 0.002f, what + ": largest change of slope " + juce::String(thrown, 5) + " thrown, " + juce::String(still, 5) + " at rest");
        }
    }

    // NEPTUNE_FX_TIME=1: microseconds per 512-sample stereo block at 48 kHz, the best of five runs.
    void timing()
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_FX_TIME", {}).isEmpty())
            return;
        beginTest("cost per block (printed, not judged)");
        const int block = 512, blocks = 1500;
        std::vector<float> noiseL((size_t) block * 8), noiseR(noiseL.size()), l((size_t) block), r((size_t) block);
        juce::Random rng(5);
        for (size_t i = 0; i < noiseL.size(); ++i)
        {
            noiseL[i] = rng.nextFloat() - 0.5f;
            noiseR[i] = rng.nextFloat() - 0.5f;
        }
        auto time = [&](const std::function<void(float*, float*, int)>& process)
        {
            double best = 1.0e9;
            for (int run = 0; run < 5; ++run)
            {
                double total = 0.0;
                for (int b = 0; b < blocks; ++b)
                {
                    std::copy_n(noiseL.begin() + (b % 8) * block, block, l.begin());
                    std::copy_n(noiseR.begin() + (b % 8) * block, block, r.begin());
                    const auto t0 = std::chrono::steady_clock::now();
                    process(l.data(), r.data(), block);
                    total += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                }
                best = std::min(best, total / blocks);
            }
            return best;
        };
        for (auto& k : kinds())
        {
            auto fx = AudioEngine::createEffect(k.type);
            fx->prepare(48000.0, block);
            oldSettings(k.type, *fx);
            const double us = time([&](float* a, float* b, int n) { fx->processBlock(a, b, n); });
            std::printf("FXTIME %-5s %8.2f us\n", k.name, us);
        }
        {
            ModFxBlock m;
            m.prepare(48000.0, block);
            for (int cell = 0; cell < ModFxBlock::NumCells; ++cell)
                m.set(cell, cell % 2 == 0 ? ModFxBlock::page().params[(size_t) cell].defaultVal : 0.7f);
            m.set(ModFxBlock::RotarySpeed, 1.0f);
            const double us = time([&](float* a, float* b, int n) { m.process(a, b, n); });
            std::printf("FXTIME %-5s %8.2f us\n", "MODFX", us);
        }
        // The core alone: one voice on one line (a rotor), one on two (a flanger), six on two.
        for (int kind = 0; kind < 3; ++kind)
        {
            moddelay::Core c;
            c.setVoices(kind == 2 ? 6 : 1);
            c.setDelay(0.0f, 0.005f, 0.002f);
            c.setRange(0.0f, 0.02f);
            c.setLevelFollow(kind == 0 ? 0.3f : 0.0f);
            c.prepare(48000.0, 0.03, kind == 0);
            const double us = time([&](float* a, float* b, int n) { c.process(a, b, n); });
            std::printf("FXTIME %-5s %8.2f us\n", kind == 0 ? "core1m" : kind == 1 ? "core1s" : "core6s", us);
        }
        std::fflush(stdout);
        expect(true);
    }
};

static EffectCoresModTests effectCoresModTests;
