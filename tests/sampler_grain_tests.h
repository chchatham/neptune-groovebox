#pragma once
#include "../src/engine/SamplerEngine.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

// The sampler's granular play mode (MODE = GRAN, GRAIN page), the playheads it reports and the SMPL
// cell following the sample bank. Everything is rendered through processBlock at 48 kHz from known
// samples (sine, sweep, ramp, constant, noise), so pitch, position and envelope can be measured.
class SamplerGrainTests : public juce::UnitTest
{
public:
    SamplerGrainTests() : UnitTest("Sampler Grain") {}

    static constexpr double SR = 48000.0;
    static constexpr int N = 96000;           // test samples are 2 s long
    static constexpr float CENTRE = 0.70710678f;   // the sampler's pan law: centre = cos(pi / 4) per side

    struct Out
    {
        std::vector<float> l, r;
        float rms(size_t from = 0, size_t to = (size_t) -1) const
        {
            to = std::min(to, l.size());
            double s = 0.0;
            for (size_t i = from; i < to; ++i) s += (double) l[i] * l[i];
            return to > from ? (float) std::sqrt(s / (double) (to - from)) : 0.0f;
        }
        float peak() const
        {
            float p = 0.0f;
            for (size_t i = 0; i < l.size(); ++i) p = std::max({ p, std::abs(l[i]), std::abs(r[i]) });
            return p;
        }
    };

    // --- test samples ---
    static std::vector<float> sine(double hz)
    {
        std::vector<float> s((size_t) N);
        for (int i = 0; i < N; ++i) s[(size_t) i] = 0.5f * (float) std::sin(2.0 * 3.141592653589793 * hz * i / SR);
        return s;
    }
    // Linear sweep: the frequency at sample position p (0..1) is 200 + 2000 p Hz.
    static std::vector<float> sweep()
    {
        std::vector<float> s((size_t) N);
        double ph = 0.0;
        for (int i = 0; i < N; ++i)
        {
            ph += 2.0 * 3.141592653589793 * (200.0 + 2000.0 * (double) i / N) / SR;
            s[(size_t) i] = 0.5f * (float) std::sin(ph);
        }
        return s;
    }
    // The value says where it was read: 0.2 at the start, 1.0 at the end.
    static std::vector<float> ramp()
    {
        std::vector<float> s((size_t) N);
        for (int i = 0; i < N; ++i) s[(size_t) i] = 0.2f + 0.8f * (float) i / (float) N;
        return s;
    }
    static float rampPosition(float outputPeak) { return (outputPeak / CENTRE - 0.2f) / 0.8f; }
    static std::vector<float> constant() { return std::vector<float>((size_t) N, 1.0f); }
    static std::vector<float> noise()
    {
        std::vector<float> s((size_t) N);
        uint32_t seed = 777u;
        for (auto& x : s) x = 0.5f * dsp::xorshiftNoise(seed);
        return s;
    }

    // --- engines and rendering ---
    static std::unique_ptr<SamplerEngine> engine(const std::vector<float>& sample, int mode = SamplerEngine::Granular,
                                                 double sampleRate = SR)
    {
        auto e = std::make_unique<SamplerEngine>();
        e->prepareToPlay(sampleRate, 64);
        e->loadSample(sample.data(), (int) sample.size(), 1, SR);
        e->setParameter(SamplerEngine::Mode, (float) mode);
        e->setParameter(SamplerEngine::VelSens, 0.0f);
        e->setParameter(SamplerEngine::Volume, 1.0f);
        return e;
    }

    static Out render(SamplerEngine& e, double seconds, double sampleRate = SR)
    {
        Out o;
        juce::AudioBuffer<float> buf(2, 64);
        juce::MidiBuffer midi;
        const int blocks = (int) std::lround(seconds * sampleRate / 64.0);
        o.l.reserve((size_t) blocks * 64);
        o.r.reserve((size_t) blocks * 64);
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < 64; ++i)
            {
                o.l.push_back(buf.getSample(0, i));
                o.r.push_back(buf.getSample(1, i));
            }
        }
        return o;
    }

    // --- measurements ---
    static double goertzel(const std::vector<float>& x, size_t from, size_t n, double hz)
    {
        const double w = 2.0 * 3.141592653589793 * hz / SR, c = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            const double win = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * (double) i / (double) n);
            const double s0 = win * x[from + i] + c * s1 - s2;
            s2 = s1; s1 = s0;
        }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    }

    struct Spectrum { double peakHz = 0.0, centroid = 0.0, width = 0.0; };
    static Spectrum spectrum(const std::vector<float>& x, double fromSec, double lenSec, double lo, double hi, double step = 4.0)
    {
        const size_t from = (size_t) (fromSec * SR), n = (size_t) (lenSec * SR);
        Spectrum s;
        double best = -1.0, sum = 0.0, sumF = 0.0, sumFF = 0.0;
        for (double f = lo; f <= hi; f += step)
        {
            const double p = goertzel(x, from, n, f);
            if (p > best) { best = p; s.peakHz = f; }
            sum += p; sumF += p * f; sumFF += p * f * f;
        }
        if (sum > 0.0)
        {
            s.centroid = sumF / sum;
            s.width = std::sqrt(std::max(0.0, sumFF / sum - s.centroid * s.centroid));
        }
        return s;
    }

    // Stretches of sound separated by silence = single grains, when they do not overlap.
    struct Run { size_t start = 0, length = 0; float peak = 0.0f; size_t peakAt = 0; };
    static std::vector<Run> runs(const std::vector<float>& x, float threshold = 1.0e-6f)
    {
        std::vector<Run> out;
        bool in = false;
        Run r;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const float a = std::abs(x[i]);
            if (a > threshold)
            {
                if (! in) { in = true; r = Run(); r.start = i; }
                ++r.length;
                if (a > r.peak) { r.peak = a; r.peakAt = i - r.start; }
            }
            else if (in)
            {
                in = false;
                out.push_back(r);
            }
        }
        return out;   // a run still open at the end is not counted
    }

    static uint64_t hashOf(const Out& o)
    {
        uint64_t h = 1469598103934665603ull;
        for (auto* v : { &o.l, &o.r })
            for (float f : *v)
            {
                uint32_t bits;
                std::memcpy(&bits, &f, 4);
                h = (h ^ bits) * 1099511628211ull;
            }
        return h;
    }

    // A fixed performance in one of the six classic modes. variant 0: plain; 1: start / end / loop
    // points, slices, filter, crusher, drive, random start, pan, two LFOs; 2: the bank's sample.
    // grainKnobs: every GRAIN cell and the grain seed away from the defaults.
    static uint64_t classicHash(int mode, int variant, bool grainKnobs)
    {
        SamplerEngine e;
        e.prepareToPlay(48000.0, 64);
        if (variant != 2)
        {
            auto s = sweep();
            uint32_t seed = 12345u;
            for (auto& x : s) x += 0.1f * dsp::xorshiftNoise(seed);
            e.loadSample(s.data(), 24000, 1, 48000.0);
        }
        e.setParameter(SamplerEngine::Mode, (float) mode);
        if (grainKnobs)
        {
            e.setGrainSeed(42u);
            e.setParameter(SamplerEngine::GrainSize, 0.3f);
            e.setParameter(SamplerEngine::GrainDensity, 150.0f);
            e.setParameter(SamplerEngine::GrainScan, -1.5f);
            e.setParameter(SamplerEngine::GrainSpray, 0.7f);
            e.setParameter(SamplerEngine::GrainPitchJit, 0.6f);
            e.setParameter(SamplerEngine::GrainSpread, 1.0f);
            e.setParameter(SamplerEngine::GrainShape, 2.0f);
            e.setParameter(SamplerEngine::GrainReverse, 0.5f);
        }
        if (variant >= 1)
        {
            e.setParameter(SamplerEngine::StartPoint, 0.1f);
            e.setParameter(SamplerEngine::EndPoint, 0.8f);
            e.setParameter(SamplerEngine::LoopStart, 0.2f);
            e.setParameter(SamplerEngine::FilterCutoff, 0.6f);
            e.setParameter(SamplerEngine::FilterEnv, 0.3f);
            e.setParameter(SamplerEngine::DriveAmt, 0.3f);
            e.setParameter(SamplerEngine::BitReduce, 0.3f);
            e.setParameter(SamplerEngine::RateReduce, 0.1f);
            e.setParameter(SamplerEngine::StartRandom, 0.2f);
            e.setParameter(SamplerEngine::Pan, 0.3f);
            e.setParameter(SamplerEngine::SliceCount, 4.0f);
            e.setParameter(SamplerEngine::SliceSelect, 0.3f);
            e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) SamplerEngine::LfoPitch);
            e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 0.4f);
            e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Dest), (float) SamplerEngine::LfoCutoff);
            e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Depth), -0.5f);
            e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Rate), 7.0f);
        }
        Out o;
        juce::AudioBuffer<float> buf(2, 64);
        juce::MidiBuffer midi;
        for (int block = 0; block < 400; ++block)
        {
            if (block == 0) e.noteOn(60, 0.9f, -1);
            if (block == 40) e.noteOn(67, 0.5f, -1);
            if (block == 90) e.pitchBend(0.5f);
            if (block == 200) e.noteOff(60, -1);
            if (block == 260) e.noteOff(67, -1);
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < 64; ++i)
            {
                o.l.push_back(buf.getSample(0, i));
                o.r.push_back(buf.getSample(1, i));
            }
        }
        return hashOf(o);
    }

    static void setLfo(SamplerEngine& e, int dest, float depth, float rateHz)
    {
        e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) dest);
        e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), depth);
        e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), rateHz);
    }

    void runTest() override
    {
        using S = SamplerEngine;

        beginTest("GRAIN page: second page, eight working cells, MODE has GRAN, ids are stable");
        {
            S e;
            auto pages = e.getParamPages();
            expect(juce::String(pages[0].name) == "SAMPL" && juce::String(pages[1].name) == "GRAIN");
            expect(juce::String(pages[1].section) == "SRC");
            const char* labels[8] = { "SIZE", "DENS", "SCAN", "SPRY", "PJIT", "SPRD", "SHAP", "RVRS" };
            for (int i = 0; i < 8; ++i)
            {
                const auto& p = pages[1].params[(size_t) i];
                expect(p.isValid() && juce::String(p.label) == labels[i], labels[i]);
                expectEquals(p.id, 32 + i);
                expect(juce::String(p.label).length() <= 4 && juce::String(p.label) == juce::String(p.label).toUpperCase());
                expectEquals(e.getParameter(p.id), p.defaultVal);          // defaults seeded
                const float v = p.fromNorm(0.3f);
                e.setParameter(p.id, v);
                expectEquals(e.getParameter(p.id), v);
                expect(e.getParameterName(p.id).isNotEmpty());
            }
            const auto& mode = pages[0].params[1];
            expectEquals((int) mode.maxVal, 6);
            expectEquals(mode.numChoices, 7);
            expect(juce::String(mode.choices[6]) == "GRAN" && juce::String(mode.choices[5]) == "PING");
            expectEquals((int) S::Granular, 6);
            // SCAN lands exactly on 0 (freeze) from its default with whole encoder steps.
            const auto& scan = pages[1].params[2];
            expectEquals(scan.stepped(1.0f, -20), 0.0f);
            const auto& shape = pages[1].params[6];
            expectEquals(shape.numChoices, 4);
            for (int i = 0; i < 4; ++i)
                expect(juce::String(shape.choices[i]).length() <= 4);
        }

        beginTest("LFO destinations: old indices unchanged, GPOS SIZE DENS appended");
        {
            const char* want[] = { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "STRT", "DRIV", "GPOS", "SIZE", "DENS" };
            expectEquals((int) S::NumLfoDests, 12);      // SPD (stretch speed) was appended after these
            for (int i = 0; i < 11; ++i)
                expect(juce::String(S::lfoDestLabels()[i]) == want[i], want[i]);
            S e;
            auto pages = e.getParamPages();
            const auto& dest = pages.back().params[7];
            expect(dest.numChoices > 12);                // the page cells follow (LFO Destinations tests)
            expectEquals(juce::String(dest.choices[11]), juce::String("SPD"));
            e.setParameter(dest.id, 10.0f);
            expectEquals(e.getParameter(dest.id), 10.0f);
        }

        beginTest("Classic modes are untouched by the GRAIN cells (bit-identical)");
        {
            for (int variant = 0; variant < 3; ++variant)
                for (int mode = 0; mode < 6; ++mode)
                    expect(classicHash(mode, variant, false) == classicHash(mode, variant, true),
                           "mode " + juce::String(mode) + " variant " + juce::String(variant));
        }

        beginTest("GRAN sounds, stays bounded, and the defaults reproduce the sample at unity");
        {
            auto e = engine(sweep());
            e->noteOn(60, 1.0f, -1);
            auto o = render(*e, 1.0);
            bool finite = true;
            for (float x : o.l) finite = finite && std::isfinite(x);
            expect(finite);
            expect(o.rms() > 0.1f, "audible");
            expect(o.peak() < 1.5f, "bounded: " + juce::String(o.peak()));

            // SCAN 1 at the original pitch with SIZE x DENS = 4 Hann grains: the grains line up and
            // sum to the sample itself.
            auto fwd = engine(sweep(), S::Forward);
            fwd->noteOn(60, 1.0f, -1);
            auto ref = render(*fwd, 1.0);
            double err = 0.0, pow = 0.0;
            for (size_t i = 9600; i < 43200; ++i)
            {
                err += std::pow((double) o.l[i] - ref.l[i], 2.0);
                pow += (double) ref.l[i] * ref.l[i];
            }
            expect(err < 0.001 * pow, "default GRAN = the sample: error " + juce::String(err / pow));
        }

        beginTest("Pitch follows the note, PTCH and bend, not SCAN");
        {
            auto freq = [this](int note, float scan, float ptch, float bend)
            {
                auto e = engine(sine(440.0));
                e->setParameter(S::GrainScan, scan);
                e->setParameter(S::GrainSpray, 0.5f);
                e->setParameter(S::PitchSemi, ptch);
                e->pitchBend(bend);
                e->noteOn(note, 1.0f, -1);
                auto o = render(*e, 1.0);
                expect(o.rms() > 0.05f);
                return spectrum(o.l, 0.25, 0.5, 200.0, 2000.0, 2.0).peakHz;
            };
            expectWithinAbsoluteError(freq(60, 1.0f, 0, 0), 440.0, 6.0);
            expectWithinAbsoluteError(freq(60, 0.25f, 0, 0), 440.0, 6.0);
            expectWithinAbsoluteError(freq(60, 2.0f, 0, 0), 440.0, 6.0);
            expectWithinAbsoluteError(freq(60, -1.0f, 0, 0), 440.0, 6.0);
            expectWithinAbsoluteError(freq(60, 0.0f, 0, 0), 440.0, 6.0);
            expectWithinAbsoluteError(freq(72, 0.25f, 0, 0), 880.0, 10.0);
            expectWithinAbsoluteError(freq(72, 2.0f, 0, 0), 880.0, 10.0);
            expectWithinAbsoluteError(freq(60, 0.5f, 7, 0), 440.0 * std::pow(2.0, 7.0 / 12.0), 8.0);
            expectWithinAbsoluteError(freq(60, 0.5f, 0, 1.0f), 440.0 * std::pow(2.0, 2.0 / 12.0), 8.0);   // BEND 2 st
        }

        beginTest("SCAN 0 freezes: a held note stays on one moment of the sample, STRT moves it");
        {
            auto frozen = [this](float start, double& early, double& late)
            {
                auto e = engine(sweep());
                e->setParameter(S::GrainScan, 0.0f);
                e->setParameter(S::StartPoint, start);
                e->noteOn(60, 1.0f, -1);
                auto o = render(*e, 3.0);
                early = spectrum(o.l, 0.2, 0.5, 200.0, 2400.0).peakHz;
                late = spectrum(o.l, 2.4, 0.5, 200.0, 2400.0).peakHz;
                expect(o.rms(120000) > 0.05f, "still sounding after 2.5 s");
                expectWithinAbsoluteError(e->getPlayheadNormalized(), start, 0.001f);
            };
            // Grains of 100 ms read from the position on: the sweep is 200 + 2000 p there, + 50 Hz mid-grain.
            double a0, a1, b0, b1;
            frozen(0.25f, a0, a1);
            frozen(0.75f, b0, b1);
            expectWithinAbsoluteError(a0, 750.0, 60.0);
            expectWithinAbsoluteError(a1, a0, 8.0);
            expectWithinAbsoluteError(b0, 1750.0, 60.0);
            expectWithinAbsoluteError(b1, b0, 8.0);
        }

        beginTest("SCAN travels at its speed, in both directions, and wraps inside STRT..END");
        {
            // Half speed: after 1 s the position is 0.5 s into the 2 s sample.
            auto e = engine(sweep());
            e->setParameter(S::GrainScan, 0.5f);
            e->noteOn(72, 1.0f, -1);                       // the note does not matter
            auto o = render(*e, 1.0);
            expectWithinAbsoluteError(e->getPlayheadNormalized(), 0.25f, 0.001f);
            auto o2 = render(*e, 1.0);
            expectWithinAbsoluteError(e->getPlayheadNormalized(), 0.5f, 0.001f);
            // What is heard moves with it: the sweep (an octave up for note 72) around each position.
            // Between 0.5 and 0.75 s the scan is at p = 0.125..0.1875: 2 x (200 + 2000 p), + a grain's worth.
            const double f1 = spectrum(o.l, 0.5, 0.25, 300.0, 4800.0, 8.0).peakHz;
            const double f2 = spectrum(o2.l, 0.5, 0.25, 300.0, 4800.0, 8.0).peakHz;
            expectWithinAbsoluteError(f1, 2.0 * (200.0 + 2000.0 * 0.156) + 200.0, 250.0);
            expectWithinAbsoluteError(f2 - f1, 2.0 * 2000.0 * 0.25, 200.0);

            // The same at another output sample rate (48 kHz sample, 96 kHz engine).
            auto hi = engine(sweep(), S::Granular, 96000.0);
            hi->setParameter(S::GrainScan, 0.5f);
            hi->noteOn(60, 1.0f, -1);
            render(*hi, 1.0, 96000.0);
            expectWithinAbsoluteError(hi->getPlayheadNormalized(), 0.25f, 0.001f);

            // Wrap: region 0.25..0.5 (24000 frames). Twice the speed for 0.4 s = 38400 frames = one
            // lap + 14400 -> 0.4. Backwards for 0.1 s from STRT: straight to the END side, 0.45.
            auto w = engine(sweep());
            w->setParameter(S::StartPoint, 0.25f);
            w->setParameter(S::EndPoint, 0.5f);
            w->setParameter(S::GrainScan, 2.0f);
            w->noteOn(60, 1.0f, -1);
            bool inside = true;
            Out all;
            for (int i = 0; i < 300; ++i)
            {
                auto part = render(*w, 64.0 / SR);
                all.l.insert(all.l.end(), part.l.begin(), part.l.end());
                const float p = w->getPlayheadNormalized();
                inside = inside && p >= 0.25f && p < 0.5f;
            }
            expect(inside, "scan position stays inside the region");
            expectWithinAbsoluteError(w->getPlayheadNormalized(), 0.4f, 0.001f);
            // Only the region is heard: 700..1200 Hz (+ a grain's length past END), never the rest of the sweep.
            for (double t = 0.05; t < 0.35; t += 0.05)
            {
                const double f = spectrum(all.l, t, 0.05, 200.0, 2400.0, 10.0).peakHz;
                expect(f > 640.0 && f < 1340.0, "inside the region (+ a grain) at " + juce::String(t) + ": " + juce::String(f));
            }

            auto back = engine(sweep());
            back->setParameter(S::StartPoint, 0.25f);
            back->setParameter(S::EndPoint, 0.5f);
            back->setParameter(S::GrainScan, -1.0f);
            back->noteOn(60, 1.0f, -1);
            auto ob = render(*back, 0.1);
            expectWithinAbsoluteError(back->getPlayheadNormalized(), 0.45f, 0.001f);
            expect(ob.rms() > 0.05f);
        }

        beginTest("SIZE sets the grain length and DENS the grains per second");
        {
            auto measure = [this](float size, float dens, double sampleRate, int& count, double& meanLen)
            {
                auto e = engine(constant(), S::Granular, sampleRate);
                e->setParameter(S::GrainScan, 0.0f);
                e->setParameter(S::GrainSize, size);
                e->setParameter(S::GrainDensity, dens);
                e->noteOn(60, 1.0f, -1);
                auto o = render(*e, 1.95, sampleRate);
                auto rs = runs(o.l);
                count = (int) rs.size();
                meanLen = 0.0;
                for (auto& r : rs) meanLen += (double) r.length / sampleRate / (double) std::max(1, count);
                float peak = 0.0f;
                for (size_t i = 1; i < rs.size(); ++i) peak = std::max(peak, rs[i].peak);
                expectWithinAbsoluteError(peak, CENTRE, 0.01f, "a lone grain plays at full level");
            };
            int c; double len;
            measure(0.02f, 4.0f, SR, c, len);
            expectEquals(c, 8);                                   // 0, 0.25 ... 1.75 s
            expectWithinAbsoluteError(len, 0.02, 0.001);
            measure(0.02f, 10.0f, SR, c, len);
            expectEquals(c, 20);
            measure(0.1f, 4.0f, SR, c, len);
            expectEquals(c, 8);
            expectWithinAbsoluteError(len, 0.1, 0.003);
            measure(0.005f, 40.0f, SR, c, len);
            expectEquals(c, 78);
            expectWithinAbsoluteError(len, 0.005, 0.0005);
            measure(0.05f, 4.0f, 96000.0, c, len);                // seconds, whatever the sample rate
            expectEquals(c, 8);
            expectWithinAbsoluteError(len, 0.05, 0.002);
        }

        beginTest("SHAP changes the grain window");
        {
            // A constant sample: the output of a lone grain is its window.
            auto grain = [this](int shape, float& peakAt, float& atEighth)
            {
                auto e = engine(constant());
                e->setParameter(S::GrainScan, 0.0f);
                e->setParameter(S::GrainSize, 0.1f);
                e->setParameter(S::GrainDensity, 2.0f);
                e->setParameter(S::GrainShape, (float) shape);
                e->noteOn(60, 1.0f, -1);
                auto o = render(*e, 0.9);
                // The second grain starts at 0.5 s and is 4800 samples long.
                size_t best = 0;
                for (size_t i = 0; i < 4800; ++i)
                    if (o.l[24000 + i] > o.l[24000 + best]) best = i;
                peakAt = (float) best / 4800.0f;
                atEighth = o.l[24000 + 600] / CENTRE;
                expectWithinAbsoluteError(o.l[24000 + best] / CENTRE, 1.0f, 0.02f);
                expect(std::abs(o.l[23990]) < 1.0e-6f && std::abs(o.l[24000 + 4810]) < 1.0e-6f, "starts and ends at zero");
            };
            float at, v;
            grain(S::WindowHann, at, v);
            expectWithinAbsoluteError(at, 0.5f, 0.01f);
            expectWithinAbsoluteError(v, 0.1464f, 0.01f);
            grain(S::WindowTriangle, at, v);
            expectWithinAbsoluteError(at, 0.5f, 0.01f);
            expectWithinAbsoluteError(v, 0.25f, 0.01f);
            grain(S::WindowPerc, at, v);
            expectWithinAbsoluteError(at, 0.02f, 0.005f);
            expect(v > 0.4f && v < 0.7f, "decaying: " + juce::String(v));
            grain(S::WindowSwell, at, v);
            expectWithinAbsoluteError(at, 0.98f, 0.005f);
            expect(v < 0.02f, "still rising slowly: " + juce::String(v));
        }

        beginTest("SPRY widens the set of positions read (and scatters the timing)");
        {
            // Ramp sample: the peak of a short triangular grain is the sample value at its middle.
            auto spreadOf = [this](float spray, float& centre, std::vector<size_t>& starts)
            {
                auto e = engine(ramp());
                e->setParameter(S::GrainScan, 0.0f);
                e->setParameter(S::StartPoint, 0.5f);
                e->setParameter(S::GrainSize, 0.005f);
                e->setParameter(S::GrainDensity, 10.0f);
                e->setParameter(S::GrainShape, (float) S::WindowTriangle);
                e->setParameter(S::GrainSpray, spray);
                e->noteOn(60, 1.0f, -1);
                auto o = render(*e, 6.0);
                float lo = 2.0f, hi = -1.0f, sum = 0.0f;
                int n = 0;
                starts.clear();
                for (auto& r : runs(o.l))
                {
                    if (r.start < 4800) continue;          // after the attack
                    const float p = rampPosition(r.peak);
                    lo = std::min(lo, p); hi = std::max(hi, p); sum += p; ++n;
                    starts.push_back(r.start);
                }
                expect(n > 40, "grains found: " + juce::String(n));
                centre = sum / (float) std::max(1, n);
                return hi - lo;
            };
            float c0, c1, c2;
            std::vector<size_t> s0, s1, s2;
            const float w0 = spreadOf(0.0f, c0, s0);
            const float w1 = spreadOf(0.2f, c1, s1);
            const float w2 = spreadOf(1.0f, c2, s2);
            expect(w0 < 0.001f, "no spray: one position (" + juce::String(w0) + ")");
            expectWithinAbsoluteError(c0, 0.5f, 0.005f);
            // +-250 ms of a 2 s sample at full = +-0.125; 20 % = +-0.025.
            expect(w1 > 0.03f && w1 <= 0.051f, "20 %: " + juce::String(w1));
            expect(w2 > 0.2f && w2 <= 0.251f, "100 %: " + juce::String(w2));
            expectWithinAbsoluteError(c2, 0.5f, 0.04f);
            // Timing: regular without spray, irregular with it, the same number of grains on average.
            auto jitter = [](const std::vector<size_t>& s)
            {
                size_t lo = (size_t) -1, hi = 0;
                for (size_t i = 1; i < s.size(); ++i) { lo = std::min(lo, s[i] - s[i - 1]); hi = std::max(hi, s[i] - s[i - 1]); }
                return (int) (hi - lo);
            };
            expect(jitter(s0) <= 2, "quasi-synchronous: " + juce::String(jitter(s0)));
            expect(jitter(s2) > 2000, "asynchronous: " + juce::String(jitter(s2)));
            expect(std::abs((int) s2.size() - (int) s0.size()) < 12, "same mean density");
        }

        beginTest("PJIT widens the spectrum; small amounts are a detune");
        {
            auto width = [this](float pjit)
            {
                auto e = engine(sine(1000.0));
                e->setParameter(S::GrainScan, 0.0f);
                e->setParameter(S::GrainPitchJit, pjit);
                e->noteOn(60, 1.0f, -1);
                auto o = render(*e, 2.0);
                expect(o.rms() > 0.03f);
                return spectrum(o.l, 0.5, 1.4, 400.0, 2200.0, 5.0).width;
            };
            const double w0 = width(0.0f), w10 = width(0.1f), w50 = width(0.5f), w100 = width(1.0f);
            logMessage("PJIT widths (Hz): " + juce::String(w0) + " " + juce::String(w10) + " " + juce::String(w50) + " " + juce::String(w100));
            expect(w10 < w0 + 12.0, "10 % = +-12 cents: within a few Hz of 1 kHz");
            expect(w50 > w0 + 50.0, "50 % = +-3 semitones");      // uniform over 841..1189 Hz: about 100 Hz
            expect(w100 > w50 * 1.8, "100 % = +-12 semitones");
        }

        beginTest("SPRD places grains in the stereo field; 0 keeps left and right identical");
        {
            auto run = [this](float spread, float spray, double& correlation, double& energyRatio)
            {
                auto e = engine(sweep());
                e->setParameter(S::GrainSpread, spread);
                e->setParameter(S::GrainSpray, spray);
                e->setParameter(S::GrainSize, 0.03f);
                e->noteOn(60, 1.0f, -1);
                auto o = render(*e, 1.5);
                double ll = 0, rr = 0, lr = 0;
                bool same = true;
                for (size_t i = 0; i < o.l.size(); ++i)
                {
                    ll += (double) o.l[i] * o.l[i]; rr += (double) o.r[i] * o.r[i]; lr += (double) o.l[i] * o.r[i];
                    same = same && o.l[i] == o.r[i];
                }
                correlation = lr / std::sqrt(ll * rr);
                energyRatio = ll / rr;
                return same;
            };
            double corr, ratio;
            expect(run(0.0f, 0.3f, corr, ratio), "SPRD 0: left = right exactly");
            expect(! run(1.0f, 0.3f, corr, ratio));
            expect(corr < 0.8, "full spread decorrelates: " + juce::String(corr));
            expect(ratio > 0.6 && ratio < 1.6, "balanced on average: " + juce::String(ratio));
            double corrHalf;
            run(0.3f, 0.3f, corrHalf, ratio);
            expect(corrHalf > corr && corrHalf < 0.9999, "30 % is narrower: " + juce::String(corrHalf));

            // PAN still applies after the grains.
            auto e = engine(sweep());
            e->setParameter(S::Pan, 1.0f);
            e->noteOn(60, 1.0f, -1);
            auto o = render(*e, 0.3);
            float left = 0.0f;
            for (float x : o.l) left = std::max(left, std::abs(x));
            expect(left < 1.0e-4f && o.peak() > 0.1f, "PAN right");
        }

        beginTest("RVRS plays grains backwards with the given probability");
        {
            // Ramp sample, symmetric window, note 84 (4 x: a 100 ms grain covers 0.2 of the ramp):
            // a forward grain is louder in its second half, a reversed one in its first.
            auto count = [this](float reverse, int& fwd, int& rev, float& fwdEarly, float& revLate)
            {
                auto e = engine(ramp());
                e->setParameter(S::GrainScan, 0.0f);
                e->setParameter(S::GrainSize, 0.1f);
                e->setParameter(S::GrainDensity, 5.0f);
                e->setParameter(S::GrainReverse, reverse);
                e->noteOn(84, 1.0f, -1);
                auto o = render(*e, 8.1);
                fwd = rev = 0;
                for (auto& r : runs(o.l))
                {
                    if (r.start < 4800) continue;
                    const float early = o.l[r.start + 1200], late = o.l[r.start + 3600];
                    if (late > early * 1.2f) { ++fwd; fwdEarly = early; }
                    else if (early > late * 1.2f) { ++rev; revLate = late; }
                }
            };
            int f, r; float fe = 0, rl = 0, fe2 = 0, rl2 = 0;
            count(0.0f, f, r, fe, rl);
            expect(f == 40 && r == 0, "RVRS 0: all forwards (" + juce::String(f) + " / " + juce::String(r) + ")");
            count(1.0f, f, r, fe2, rl2);
            expect(f == 0 && r == 40, "RVRS 1: all backwards (" + juce::String(f) + " / " + juce::String(r) + ")");
            // The same stretch of the sample, mirrored in time.
            expectWithinAbsoluteError(rl2, fe, 0.005f);
            count(0.5f, f, r, fe, rl);
            expect(f + r == 40 && f >= 10 && r >= 10, "RVRS 50 %: " + juce::String(f) + " / " + juce::String(r));
        }

        beginTest("Loudness holds across SIZE and DENS (1 / sqrt(overlap))");
        {
            auto level = [this](float size, float dens)
            {
                auto e = engine(noise());
                e->setParameter(S::GrainSize, size);
                e->setParameter(S::GrainDensity, dens);
                e->setParameter(S::GrainSpray, 0.5f);
                e->noteOn(60, 1.0f, -1);
                auto o = render(*e, 1.6);
                expect(o.peak() < 2.0f, "bounded");
                return o.rms(28800);
            };
            const float settings[][2] = { { 0.02f, 50.0f }, { 0.02f, 100.0f }, { 0.1f, 40.0f }, { 0.05f, 200.0f },
                                          { 0.3f, 100.0f }, { 0.5f, 200.0f }, { 0.5f, 2.0f }, { 0.005f, 200.0f } };
            float lo = 10.0f, hi = 0.0f;
            juce::String all;
            for (auto& s : settings)
            {
                const float l = level(s[0], s[1]);
                lo = std::min(lo, l); hi = std::max(hi, l);
                all << juce::String(l, 3) << " ";
            }
            // The sample played forward gives 0.289 x 0.707 (pan law) = 0.204; Hann grains of noise that
            // do not line up: x sqrt(3/8), and x sqrt(2/3) for interpolating noise between samples = 0.102.
            expect(hi / lo < 1.3f, "overlap 1..100: within 2 dB (" + juce::String(hi / lo) + ": " + all + ")");
            expect(lo > 0.085f && hi < 0.125f, "the predicted level: " + all);
            // Sparse grains are simply fewer.
            expect(level(0.02f, 5.0f) < lo);
        }

        beginTest("A full grain pool skips new grains");
        {
            auto e = engine(noise());
            e->setParameter(S::GrainSize, 0.5f);
            e->setParameter(S::GrainDensity, 200.0f);
            e->setParameter(S::GrainSpray, 0.5f);
            e->noteOn(60, 1.0f, -1);
            int most = 0;
            float peak = 0.0f;
            for (int i = 0; i < 100; ++i)
            {
                auto o = render(*e, 0.01);
                most = std::max(most, e->getActiveGrainCount());
                peak = std::max(peak, o.peak());
            }
            expectEquals(most, (int) S::MAX_GRAINS);
            expect(peak < 1.0f, "peak " + juce::String(peak));
            // Eight voices: every one has its own pool (a slot is free for a moment when a grain ends).
            for (int n = 0; n < 7; ++n) e->noteOn(61 + n, 1.0f, -1);
            render(*e, 0.6);
            expect(e->getActiveGrainCount() > 8 * 40 && e->getActiveGrainCount() <= 8 * (int) S::MAX_GRAINS,
                   "grains: " + juce::String(e->getActiveGrainCount()));
        }

        beginTest("The random cloud is deterministic for a seed");
        {
            auto cloud = [this](uint32_t seed)
            {
                auto e = engine(sweep());
                e->setGrainSeed(seed);
                e->setParameter(S::GrainSpray, 0.6f);
                e->setParameter(S::GrainPitchJit, 0.4f);
                e->setParameter(S::GrainSpread, 0.8f);
                e->setParameter(S::GrainReverse, 0.5f);
                e->noteOn(60, 1.0f, -1);
                return hashOf(render(*e, 0.5));
            };
            expect(cloud(1u) == cloud(1u));
            expect(cloud(1u) != cloud(2u));
        }

        beginTest("Everything after the grains still works: envelope, level, velocity, filter, crusher, drive");
        {
            auto make = [this]
            {
                auto e = engine(sweep());
                e->setParameter(S::GrainScan, 0.0f);
                e->setParameter(S::StartPoint, 0.9f);          // about 2 kHz
                return e;
            };
            auto plain = make();
            plain->noteOn(60, 1.0f, -1);
            auto ref = render(*plain, 0.5);

            auto quiet = make();
            quiet->setParameter(S::Volume, 0.25f);
            quiet->noteOn(60, 1.0f, -1);
            expectWithinAbsoluteError(render(*quiet, 0.5).rms() / ref.rms(), 0.25f, 0.001f);

            auto vel = make();
            vel->setParameter(S::VelSens, 1.0f);
            vel->noteOn(60, 0.5f, -1);
            expectWithinAbsoluteError(render(*vel, 0.5).rms() / ref.rms(), 0.5f, 0.001f);

            auto dark = make();
            dark->setParameter(S::FilterCutoff, 0.3f);          // about 160 Hz low-pass
            dark->noteOn(60, 1.0f, -1);
            expect(render(*dark, 0.5).rms() < 0.1f * ref.rms(), "voice filter");

            auto crushed = make();
            crushed->setParameter(S::BitReduce, 1.0f);
            crushed->noteOn(60, 1.0f, -1);
            expect(hashOf(render(*crushed, 0.5)) != hashOf(ref), "BITS");

            auto driven = make();
            driven->setParameter(S::DriveAmt, 1.0f);
            driven->noteOn(60, 1.0f, -1);
            expect(render(*driven, 0.5).rms() > 1.3f * ref.rms(), "DRIV");

            auto slow = make();
            slow->setParameter(S::Attack, 0.4f);
            slow->noteOn(60, 1.0f, -1);
            auto att = render(*slow, 0.5);
            expect(att.rms(0, 4800) < 0.3f * ref.rms(0, 4800) && att.rms(19200) > 0.7f * ref.rms(19200), "ATK");

            // Release: the note fades and the voice (and its grains) stops.
            plain->noteOff(60, -1);
            auto tail = render(*plain, 0.3);                    // REL 50 ms
            expect(tail.rms(0, 480) > 0.05f && tail.rms(4800) == 0.0f, "REL");
            expectEquals(plain->getActiveGrainCount(), 0);
            expect(plain->getPlayheadNormalized() < 0.0f);
        }

        beginTest("Polyphony: each voice has its own grains and pitch");
        {
            auto e = engine(sine(440.0));
            e->setParameter(S::GrainScan, 0.0f);
            e->noteOn(60, 1.0f, -1);
            e->noteOn(67, 1.0f, -1);
            auto o = render(*e, 1.0);
            const size_t from = 12000, n = 24000;
            const double p440 = goertzel(o.l, from, n, 440.0), p659 = goertzel(o.l, from, n, 659.26),
                         p550 = goertzel(o.l, from, n, 550.0);
            expect(p440 > 100.0 * p550 && p659 > 100.0 * p550, "both notes, nothing between");
        }

        beginTest("getPlayheads: voices in the classic modes; scan positions then grains in GRAN");
        {
            float pos[128];
            bool grain[128];

            auto classic = engine(sweep(), S::Loop);
            expectEquals(classic->getPlayheads(pos, 128, grain), 0);
            classic->noteOn(60, 1.0f, -1);
            classic->noteOn(72, 1.0f, -1);
            render(*classic, 0.25);
            expectEquals(classic->getPlayheads(pos, 128, grain), 2);
            expect(! grain[0] && ! grain[1]);
            expectWithinAbsoluteError(pos[0], 0.125f, 0.001f);       // 0.25 s of 2 s
            expectWithinAbsoluteError(pos[1], 0.25f, 0.001f);        // an octave up
            expectWithinAbsoluteError(pos[0], classic->getPlayheadNormalized(), 1.0e-6f);
            expectEquals(classic->getPlayheads(pos, 1, grain), 1);
            expectEquals(classic->getPlayheads(pos, 0, grain), 0);
            expectEquals(classic->getPlayheads(pos, 128, nullptr), 2);
            classic->allNotesOff();
            render(*classic, 0.01);
            expectEquals(classic->getPlayheads(pos, 128, grain), 0);

            auto e = engine(sweep());
            e->setParameter(S::GrainScan, 0.5f);
            e->setParameter(S::StartPoint, 0.2f);
            e->noteOn(60, 1.0f, -1);
            render(*e, 0.4);
            int n = e->getPlayheads(pos, 128, grain);
            expectEquals(n, 1 + 4);                                  // SIZE x DENS = 4 grains
            expect(! grain[0]);
            expectWithinAbsoluteError(pos[0], 0.2f + 0.1f, 0.001f);  // 0.4 s at half speed = 0.2 s = 0.1
            for (int i = 1; i < n; ++i)
            {
                expect(grain[i]);
                // Started at an earlier scan position (up to 100 ms ago = 0.025), read forward up to 100 ms (0.05).
                expect(pos[i] >= 0.0f && pos[i] <= 1.0f && pos[i] > pos[0] - 0.026f && pos[i] < pos[0] + 0.051f,
                       "grain near the scan position: " + juce::String(pos[i]));
            }
            // Two voices: both scan positions come first.
            e->noteOn(64, 1.0f, -1);
            render(*e, 0.2);
            n = e->getPlayheads(pos, 128, grain);
            expectEquals(n, 2 + 8);
            expect(! grain[0] && ! grain[1] && grain[2] && grain[9]);
            expectWithinAbsoluteError(pos[0], 0.35f, 0.001f);
            expectWithinAbsoluteError(pos[1], 0.25f, 0.001f);
            expectEquals(e->getPlayheads(pos, 3, grain), 3);

            // Many grains: thinned to the cap, all valid.
            e->setParameter(S::GrainSize, 0.5f);
            e->setParameter(S::GrainDensity, 200.0f);
            e->setParameter(S::GrainSpray, 1.0f);
            for (int k = 0; k < 6; ++k) e->noteOn(70 + k, 1.0f, -1);
            render(*e, 0.6);
            n = e->getPlayheads(pos, 128, grain);
            expect(n > 8 + 32 && n <= 8 + S::MAX_PLAYHEAD_GRAINS, "count " + juce::String(n));
            bool ok = true;
            for (int i = 0; i < n; ++i) ok = ok && pos[i] >= 0.0f && pos[i] <= 1.0f && grain[i] == (i >= 8);
            expect(ok);

            e->allNotesOff();
            render(*e, 0.01);
            expectEquals(e->getPlayheads(pos, 128, grain), 0);
        }

        beginTest("SMPL follows the sample bank as it grows");
        {
            auto& bank = SampleBank::get();
            S e;
            auto smplOf = [](const S& s)
            {
                for (auto& page : s.getParamPages())
                    for (auto& p : page.params)
                        if (p.id == S::SampleSelect) return p;
                return ParamSpec();
            };
            const int before = bank.size();
            auto p = smplOf(e);
            expectEquals(p.numChoices, before);
            expectEquals((int) p.maxVal, before - 1);

            if (before < SampleBank::MAX_ENTRIES)
            {
                std::vector<float> rec(4800, 0.25f);
                const int index = bank.addRecording(rec.data(), nullptr, (int) rec.size(), 48000.0, false);
                expectEquals(index, before);
                p = smplOf(e);                                       // an engine made before the recording
                expectEquals(p.numChoices, before + 1);
                expectEquals((int) p.maxVal, before);
                expect(p.choices == bank.names() && p.choices[before] != nullptr);
                expect(juce::String(p.format((float) before)).isNotEmpty());

                e.setParameter(S::SampleSelect, (float) before);
                expectEquals(e.getParameter(S::SampleSelect), (float) before);
                expectEquals(e.getBankIndex(), before);
                expectEquals(e.getSampleLength(), 4800);
                e.setParameter(S::SampleSelect, 1000.0f);            // clamped to what exists now
                expectEquals(e.getParameter(S::SampleSelect), (float) (bank.size() - 1));
            }
        }

        beginTest("Changing the sample under sounding grains: silence, no stuck grains, no bad reads");
        {
            auto e = engine(sweep());
            e->setParameter(S::GrainSpray, 1.0f);
            e->setParameter(S::GrainReverse, 0.5f);
            e->setParameter(S::StartPoint, 0.9f);
            e->noteOn(60, 1.0f, -1);
            e->noteOn(64, 1.0f, -1);
            render(*e, 0.3);
            expect(e->getActiveGrainCount() > 0);

            std::vector<float> tiny(100, 0.5f);                      // far shorter than where the grains were
            e->loadSample(tiny.data(), (int) tiny.size(), 1, SR);
            expectEquals(e->getActiveGrainCount(), 0);
            float pos[16];
            expectEquals(e->getPlayheads(pos, 16), 0);
            auto after = render(*e, 0.1);
            expect(after.peak() == 0.0f, "silent until the next note");

            e->noteOn(60, 1.0f, -1);                                 // region of 10 samples, grains of 100 ms
            auto next = render(*e, 0.3);
            bool finite = true;
            for (float x : next.l) finite = finite && std::isfinite(x);
            expect(finite && next.peak() > 0.1f && next.peak() < 2.0f, "plays the new sample: " + juce::String(next.peak()));

            e->setParameter(S::SampleSelect, 0.0f);                  // a bank sample, same path
            expectEquals(e->getActiveGrainCount(), 0);
            expect(render(*e, 0.05).peak() == 0.0f);

            // The same while another thread renders: samples of very different lengths swapped under
            // grains that are sounding, with notes started from the rendering thread.
            auto live = engine(sweep());
            live->setParameter(S::GrainSpray, 1.0f);
            live->setParameter(S::GrainSize, 0.2f);
            live->setParameter(S::GrainDensity, 200.0f);
            live->setParameter(S::GrainReverse, 0.5f);
            std::atomic<bool> stop { false };
            std::atomic<bool> bad { false }, loud { false }, badHead { false };
            std::thread audio([&]
            {
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                int block = 0;
                float pos2[80];
                while (! stop.load())
                {
                    if (block++ % 8 == 0) live->noteOn(48 + block % 30, 1.0f, -1);
                    buf.clear();
                    live->processBlock(buf, midi);
                    for (int i = 0; i < 64; ++i)
                    {
                        if (! std::isfinite(buf.getSample(0, i))) bad.store(true);
                        else if (std::abs(buf.getSample(0, i)) > 32.0f) loud.store(true);
                    }
                    const int n = live->getPlayheads(pos2, 80);
                    for (int i = 0; i < n; ++i)
                        if (! (pos2[i] >= 0.0f && pos2[i] <= 1.0f)) badHead.store(true);
                }
            });
            auto big = sweep();
            std::vector<float> small(256, 0.5f);
            for (int i = 0; i < 150; ++i)
            {
                if (i % 2 == 0) live->loadSample(small.data(), 50 + i, 1, SR);
                else            live->loadSample(big.data(), N - i * 100, 1, SR);
                float pos3[80];
                live->getPlayheads(pos3, 80);
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
            stop.store(true);
            audio.join();
            expect(! bad.load(), "finite throughout");
            expect(! loud.load(), "bounded throughout");
            expect(! badHead.load(), "playheads stay in 0..1");
        }

        beginTest("LFO destinations GPOS, SIZE and DENS");
        {
            // GPOS: the place grains are read from swings around the frozen scan position.
            auto positions = [this](float depth, float& lo, float& hi)
            {
                auto e = engine(ramp());
                e->setParameter(S::GrainScan, 0.0f);
                e->setParameter(S::StartPoint, 0.5f);
                e->setParameter(S::GrainSize, 0.005f);
                e->setParameter(S::GrainDensity, 20.0f);
                e->setParameter(S::GrainShape, (float) S::WindowTriangle);
                setLfo(*e, S::LfoGrainPos, depth, 1.0f);
                e->noteOn(60, 1.0f, -1);
                auto o = render(*e, 3.0);
                lo = 2.0f; hi = -1.0f;
                for (auto& r : runs(o.l))
                {
                    if (r.start < 4800) continue;
                    lo = std::min(lo, rampPosition(r.peak));
                    hi = std::max(hi, rampPosition(r.peak));
                }
                expectWithinAbsoluteError(e->getPlayheadNormalized(), 0.5f, 0.001f, "the scan position itself stays");
            };
            float lo, hi;
            positions(0.0f, lo, hi);
            expect(hi - lo < 0.001f);
            positions(0.3f, lo, hi);                                  // +-0.3 of STRT..END, which is half the sample
            expectWithinAbsoluteError(lo, 0.35f, 0.01f);
            expectWithinAbsoluteError(hi, 0.65f, 0.01f);
            positions(1.0f, lo, hi);                                  // +-0.5: the whole sample, clamped to it
            expect(lo >= 0.0f && lo < 0.02f && hi > 0.98f && hi <= 1.0f, juce::String(lo) + " .. " + juce::String(hi));

            // SIZE and DENS: lone Hann grains of a constant sample.
            auto grains = [this](int dest, float depth, int& count, double& shortest, double& longest)
            {
                auto e = engine(constant());
                e->setParameter(S::GrainScan, 0.0f);
                e->setParameter(S::GrainSize, 0.02f);
                e->setParameter(S::GrainDensity, 8.0f);
                setLfo(*e, dest, depth, 0.5f);
                e->noteOn(60, 1.0f, -1);
                auto o = render(*e, 4.0);
                auto rs = runs(o.l);
                count = (int) rs.size();
                shortest = 1.0e9; longest = 0.0;
                for (auto& r : rs)
                {
                    shortest = std::min(shortest, (double) r.length / SR);
                    longest = std::max(longest, (double) r.length / SR);
                }
            };
            int c0, c; double s0, l0, s, l;
            grains(S::LfoOff, 0.0f, c0, s0, l0);
            expectEquals(c0, 32);
            expectWithinAbsoluteError(l0, 0.02, 0.001);
            expectWithinAbsoluteError(s0, 0.02, 0.001);

            grains(S::LfoGrainSize, 0.25f, c, s, l);                  // x 100^(+-0.25) = 6.3 .. 63 ms
            expect(c >= 31 && c <= 32, "SIZE leaves the density alone: " + juce::String(c));
            expectWithinAbsoluteError(l, 0.0632, 0.004);
            expectWithinAbsoluteError(s, 0.00632, 0.001);

            grains(S::LfoGrainDens, 0.25f, c, s, l);                  // x 200^(+-0.25) = 2.1 .. 30 grains/s
            expect(c > c0 + 8, "DENS: more grains over a cycle (mean of the exponential): " + juce::String(c));
            expectWithinAbsoluteError(l, 0.02, 0.001);
            int cNeg;
            grains(S::LfoGrainDens, -0.25f, cNeg, s, l);
            expect(std::abs(cNeg - c) <= 4, "over whole cycles the sign does not matter: " + juce::String(cNeg));
        }
    }
};
static SamplerGrainTests samplerGrainTestsInstance;
