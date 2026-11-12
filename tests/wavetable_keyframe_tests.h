#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "../src/dsp/WaveMorph.h"
#include "../src/dsp/WaveTables.h"
#include "../src/engine/WavetableEngine.h"
#include "../src/project/ProjectBridge.h"
#include "help_card_tests.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

// The wavetable engine's newer built-in tables, its KEYS table (key frames taken from samples, the
// frames between them computed by a chosen algorithm) and the CLSC read-out.
//
// Everything is measured on harmonics: a frame is one cycle, so wavemorph::analyse() gives the
// level of each of its harmonics exactly, and "share(h)" below is the part of a frame's energy
// that sits on harmonic h. Renders are notes whose cycles fit the window (A3 / A4 / A6 at 48 kHz).
class WavetableKeyframeTests : public juce::UnitTest
{
public:
    WavetableKeyframeTests() : UnitTest("Wavetable Keyframes") {}

    using P = WavetableEngine::Param;
    using Frame = wavemorph::Frame;
    using Spectrum = wavemorph::Spectrum;
    using Algo = wavemorph::Algo;
    using Keys = WavetableKeys;
    static constexpr double SR = 48000.0;
    static constexpr double PI = wavemorph::PI;

    // ---- Spectra ----------------------------------------------------------------------------------
    static Spectrum partial(int h, double amp = 1.0, double phase = 0.0)
    {
        Spectrum s;
        s.c[(size_t) h] = std::polar(amp, phase - 0.5 * PI);
        return s;
    }
    static double share(const Spectrum& s, int h)
    {
        const double e = s.energy();
        return e > 0.0 ? std::norm(s.c[(size_t) h]) / e : 0.0;
    }
    static double share(const Frame& f, int h) { return share(wavemorph::analyse(f), h); }
    static double centroid(const Spectrum& s)
    {
        double sum = 0.0;
        for (int h = 1; h <= wavemorph::H; ++h)
            sum += (double) h * std::norm(s.c[(size_t) h]);
        return sum / std::max(1.0e-30, s.energy());
    }
    static Spectrum randomSpectrum(uint32_t seed, int highest = 40)
    {
        Spectrum s;
        auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return (double) (seed >> 8) / 16777216.0; };
        for (int h = 1; h <= highest; ++h)
            s.c[(size_t) h] = std::polar((0.2 + next()) / (double) h, 2.0 * PI * next());
        return s;
    }
    static double distance(const Frame& a, const Frame& b)
    {
        double sq = 0.0;
        for (int n = 0; n < wavemorph::N; ++n)
            sq += ((double) a[(size_t) n] - b[(size_t) n]) * ((double) a[(size_t) n] - b[(size_t) n]);
        return std::sqrt(sq / (double) wavemorph::N);
    }
    static double tableDistance(const std::vector<Frame>& a, const std::vector<Frame>& b)
    {
        if (a.size() != b.size())
            return 1.0;
        double d = 0.0;
        for (size_t f = 0; f < a.size(); ++f)
            d = std::max(d, distance(a[f], b[f]));
        return d;
    }
    // A key as build() stores it with ALGN and NORM off: brought to a peak of 1.
    static Frame asKey(const Spectrum& s)
    {
        Frame f = wavemorph::synthesise(s);
        wavemorph::scale(f, 1.0f / wavemorph::peakOf(f));
        return f;
    }
    static Keys::Settings plain(Algo algo, int frames = 64)
    {
        Keys::Settings s;
        s.algo = algo;
        s.frames = frames;
        s.align = false;
        s.norm = Keys::NormOff;
        return s;
    }
    static const char* algoName(Algo a)
    {
        static const char* const names[] = { "LIN", "STEP", "CLSC", "PHAS", "TRNS", "VOC" };
        return names[(int) a];
    }

    // ---- Samples ----------------------------------------------------------------------------------
    // TONE-A: 196 Hz, one second: a pure fundamental that turns into fundamental 0.5 + third harmonic 1.
    // TONE-B: 311.13 Hz: a fundamental whose second harmonic grows from nothing to the same level.
    // NOISE:  half a second of noise (no pitch).
    struct Samples { int a = -1, b = -1, noise = -1; };
    static const Samples& samples()
    {
        static const Samples s = []
        {
            Samples out;
            const int n = 44100;
            std::vector<float> a((size_t) n), b((size_t) n), c((size_t) (n / 2));
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / 44100.0, k = (double) i / (double) n;
                a[(size_t) i] = (float) (0.5 * ((1.0 - 0.5 * k) * std::sin(2.0 * PI * 196.0 * t) + k * std::sin(2.0 * PI * 588.0 * t)));
                b[(size_t) i] = (float) (0.4 * (std::sin(2.0 * PI * 311.13 * t) + k * std::sin(2.0 * PI * 622.26 * t + 1.0)));
            }
            uint32_t rng = 0x1234567u;
            for (auto& v : c)
                v = 0.5f * dsp::xorshiftNoise(rng);
            out.a = SampleBank::get().addRecording(a.data(), nullptr, n, 44100.0, false);
            out.b = SampleBank::get().addRecording(b.data(), nullptr, n, 44100.0, false);
            out.noise = SampleBank::get().addRecording(c.data(), nullptr, n / 2, 44100.0, false);
            return out;
        }();
        return s;
    }

    // ---- Engine rig -------------------------------------------------------------------------------
    static void silentBlock(WavetableEngine& e, int n = 64)
    {
        juce::AudioBuffer<float> buf(1, n);
        juce::MidiBuffer midi;
        buf.clear();
        e.processBlock(buf, midi);
    }
    static std::vector<float> render(WavetableEngine& e, int n)
    {
        juce::AudioBuffer<float> buf(1, n);
        juce::MidiBuffer midi;
        buf.clear();
        e.processBlock(buf, midi);
        return std::vector<float>(buf.getReadPointer(0), buf.getReadPointer(0) + n);
    }
    static double magnitude(const std::vector<float>& x, double freq)
    {
        double c = 0.0, s = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double a = 2.0 * PI * freq * (double) i / SR;
            c += x[i] * std::cos(a);
            s += x[i] * std::sin(a);
        }
        return 2.0 * std::sqrt(c * c + s * s) / (double) x.size();
    }
    static double meanSquare(const std::vector<float>& x)
    {
        double sq = 0.0;
        for (float v : x)
            sq += (double) v * v;
        return sq / (double) x.size();
    }
    static std::unique_ptr<WavetableEngine> makeEngine(int voices = 2)
    {
        auto e = std::make_unique<WavetableEngine>(voices);
        e->prepareToPlay(SR, 64);
        e->setParameter(P::AmpAttack, 0.001f);
        e->setParameter(P::AmpSustain, 1.0f);
        return e;
    }
    // A held note after its attack: `n` samples.
    static std::vector<float> heldNote(WavetableEngine& e, int note, int n)
    {
        e.allNotesOff();
        e.noteOn(note, 1.0f, -1);
        render(e, 4800);
        auto out = render(e, n);
        e.allNotesOff();
        return out;
    }

    void runTest() override
    {
        const auto& smp = samples();

        beginTest("Fourier: a frame survives analysis and synthesis; the three mip builders agree");
        {
            const Spectrum s = randomSpectrum(7u, wavemorph::H);
            const Frame f = wavemorph::synthesise(s);
            const Frame g = wavemorph::synthesise(wavemorph::analyse(f));
            expect(distance(f, g) < 1.0e-6, "round trip " + juce::String(distance(f, g), 9));
            for (int h : { 1, 2, 50, 127 })
                expectWithinAbsoluteError(std::abs(wavemorph::analyse(f).c[(size_t) h]), std::abs(s.c[(size_t) h]), 1.0e-6);

            Wavetable a, b;
            a.frames.resize(1);
            a.frames[0] = { f, wavemorph::synthesise(randomSpectrum(8u, 90)) };
            b = a;
            a.buildMipMapsFast();
            b.buildMipMapsFFT();
            expectEquals(a.getMipLevelCount(), b.getMipLevelCount());
            expectEquals(b.numFrames, 2);
            double worst = 0.0;
            for (int level = 0; level < a.getMipLevelCount(); ++level)
                for (int fr = 0; fr < 2; ++fr)
                    worst = std::max(worst, distance(a.frames[(size_t) level][(size_t) fr], b.frames[(size_t) level][(size_t) fr]));
            logMessage("mip chains by DFT and by FFT differ by at most " + juce::String(worst, 9));
            expect(worst < 1.0e-4);
            expect(distance(b.frames[0][0], f) == 0.0, "level 0 is not touched");
        }

        beginTest("The table list: the old ten keep their numbers, KEYS is 10, fourteen new ones follow");
        {
            WavetableEngine e(1);
            expectEquals((int) WavetableEngine::USER_TABLE, 9);
            expectEquals((int) WavetableEngine::KEY_TABLE, 10);
            expectEquals((int) WavetableEngine::FIRST_MORE_TABLE, 11);
            expectEquals(e.getTableCount(), 25);
            expectEquals((int) wavetables::definitions().size(), (int) WavetableEngine::NUM_MORE_TABLES);
            expect(WavetableEngine::NUM_MORE_TABLES >= 12, "at least a dozen new tables");
            static const char* const old[] = { "SAW", "SQR", "TRI", "SIN", "PWM", "FORM", "HARM", "SPEC", "DIGI", "USER", "KEYS" };
            const auto pages = e.getParamPages();
            const auto* tabl = pages[0].find(P::TableSelect);
            expect(tabl != nullptr && tabl->numChoices == 25 && tabl->maxVal == 24.0f);
            std::set<juce::String> seen;
            for (int i = 0; i < 25; ++i)
            {
                const juce::String label = tabl->choices[i];
                if (i < 11)
                    expectEquals(label, juce::String(old[i]));
                expect(label.isNotEmpty() && label.length() <= 4 && label == label.toUpperCase(), label);
                expect(seen.insert(label).second, label + " twice");
            }
            // The first nine are the shared tables themselves (no copy), in their old order.
            for (int i = 0; i < 9; ++i)
                expect(&e.getTable(i) == &WavetableFactory::sharedFactoryTables()[(size_t) i]);
            expectEquals((int) WavetableFactory::sharedFactoryTables().size(), 9, "the first nine are a list of their own (the oscillator slots read builtInTables(): these, then the newer ones)");
            for (int i = 0; i < WavetableEngine::NUM_MORE_TABLES; ++i)
                expect(&e.getTable(WavetableEngine::FIRST_MORE_TABLE + i) == &WavetableFactory::sharedMoreTables()[(size_t) i]);
        }

        beginTest("Every new table: 64 frames, no DC, no frame silent, levels within 3 dB, the scan moves");
        {
            const auto& tables = WavetableFactory::sharedMoreTables();
            for (auto& wt : tables)
            {
                expectEquals(wt.numFrames, 64, wt.name);
                expectEquals(wt.getMipLevelCount(), 8, wt.name);
                float lowest = 10.0f, highest = 0.0f, peak = 0.0f, dc = 0.0f;
                double smallestStep = 10.0, largestStep = 0.0;
                bool finite = true;
                for (int f = 0; f < wt.numFrames; ++f)
                {
                    const auto& frame = wt.frames[0][(size_t) f];
                    const float rms = wavemorph::rmsOf(frame);
                    lowest = std::min(lowest, rms);
                    highest = std::max(highest, rms);
                    peak = std::max(peak, wavemorph::peakOf(frame));
                    double sum = 0.0;
                    for (float v : frame)
                    {
                        sum += v;
                        finite = finite && std::isfinite(v);
                    }
                    dc = std::max(dc, (float) std::abs(sum / wavemorph::N));
                    if (f > 0)
                    {
                        const double step = distance(frame, wt.frames[0][(size_t) f - 1]) / wavetables::FRAME_RMS;
                        smallestStep = std::min(smallestStep, step);
                        largestStep = std::max(largestStep, step);
                    }
                }
                const double ends = distance(wt.frames[0][0], wt.frames[0][63]) / wavetables::FRAME_RMS;
                logMessage(wt.name.paddedRight(' ', 5) + " rms " + juce::String(lowest, 3) + " .. " + juce::String(highest, 3)
                           + " (" + juce::String(20.0 * std::log10(lowest / highest), 1) + " dB)  peak " + juce::String(peak, 3)
                           + "  step " + juce::String(smallestStep, 3) + " .. " + juce::String(largestStep, 3)
                           + "  first to last " + juce::String(ends, 2));
                expect(finite, wt.name);
                expect(dc < 1.0e-5f, wt.name + " DC " + juce::String(dc, 8));
                expect(peak <= 1.0001f, wt.name + " peak");
                expect(highest <= wavetables::FRAME_RMS * 1.001f, wt.name + " loudest frame");
                expect(lowest >= wavetables::FRAME_RMS * 0.708f, wt.name + ": a frame more than 3 dB below the others");
                expect(smallestStep > 0.001, wt.name + ": two neighbouring frames are the same");
                expect(largestStep < 0.6, wt.name + ": a jump between neighbouring frames");
                expect(ends > 0.5, wt.name + ": the two ends of the table are alike");
                // Mip levels hold what they should: level L has nothing above harmonic 128 >> L.
                for (int level = 1; level < wt.getMipLevelCount(); ++level)
                {
                    const Spectrum s = wavemorph::analyse(wt.frames[(size_t) level][40]);
                    double above = 0.0;
                    for (int h = (128 >> level) + 1; h <= wavemorph::H; ++h)
                        above += std::norm(s.c[(size_t) h]);
                    expect(above < 1.0e-10, wt.name + " level " + juce::String(level));
                }
            }

            // No two tables are the same table.
            for (size_t a = 0; a < tables.size(); ++a)
                for (size_t b = a + 1; b < tables.size(); ++b)
                    expect(tableDistance(tables[a].frames[0], tables[b].frames[0]) > 0.1,
                           tables[a].name + " and " + tables[b].name + " are alike");
        }

        beginTest("Every new table plays: a note sounds at both ends of the table, differently, in bounds");
        {
            auto e = makeEngine(1);
            for (int t = WavetableEngine::FIRST_MORE_TABLE; t < WavetableEngine::NUM_TABLES; ++t)
            {
                e->setParameter(P::TableSelect, (float) t);
                std::vector<double> spectrum[2];
                for (int end = 0; end < 2; ++end)
                {
                    e->setParameter(P::Position, (float) end);
                    const auto x = heldNote(*e, 57, 2400);              // A3: 11 cycles
                    float peak = 0.0f;
                    bool finite = true;
                    for (float v : x)
                    {
                        peak = std::max(peak, std::abs(v));
                        finite = finite && std::isfinite(v);
                    }
                    expect(finite && peak < 1.0f, e->getTable(t).name + " peak " + juce::String(peak, 3));
                    expect(std::sqrt(meanSquare(x)) > 0.05, e->getTable(t).name + " is heard");
                    for (int h = 1; h <= 40; ++h)
                        spectrum[end].push_back(magnitude(x, 220.0 * h));
                }
                double diff = 0.0, sum = 0.0;
                for (size_t h = 0; h < 40; ++h)
                {
                    diff += std::abs(spectrum[0][h] - spectrum[1][h]);
                    sum += spectrum[0][h] + spectrum[1][h];
                }
                expect(diff / sum > 0.2, e->getTable(t).name + ": POS 0 and POS 100 sound alike (" + juce::String(diff / sum, 3) + ")");
            }
        }

        beginTest("Key frames sit on their frames exactly, with every algorithm");
        {
            const std::vector<Keys::Key> keys = { { randomSpectrum(1u), 0.0f }, { randomSpectrum(2u), 0.3f }, { randomSpectrum(3u), 1.0f } };
            for (int a = 0; a < (int) Algo::Count; ++a)
            {
                std::vector<int> at;
                const Wavetable wt = Keys::build(keys, plain((Algo) a), &at);
                expectEquals(wt.numFrames, 64);
                expect(at == std::vector<int> ({ 0, 19, 63 }), "keys on the frames nearest their places");
                for (size_t k = 0; k < keys.size(); ++k)
                {
                    const Frame want = asKey(keys[k].spectrum);
                    const Frame& got = wt.frames[0][(size_t) at[k]];
                    if ((Algo) a == Algo::Classic)
                    {
                        // 128 samples of 8 bits: every second sample, to half a step.
                        float worst = 0.0f;
                        for (int n = 0; n < wavemorph::N; n += 2)
                        {
                            worst = std::max(worst, std::abs(got[(size_t) n] - want[(size_t) n]));
                            expect(got[(size_t) n] == got[(size_t) n + 1]);
                            expectWithinAbsoluteError(got[(size_t) n] * 127.0f, std::round(got[(size_t) n] * 127.0f), 1.0e-3f);
                        }
                        expect(worst <= 0.5f / 127.0f + 1.0e-4f, "CLSC key within half a step: " + juce::String(worst, 5));
                    }
                    else
                        expect(distance(got, want) < 1.0e-6, juce::String(algoName((Algo) a)) + " key " + juce::String((int) k)
                                                                 + ": " + juce::String(distance(got, want), 9));
                }
                // And between two keys there is something that is neither (STEP: one of the two).
                const Frame& mid = wt.frames[0][40];
                const double toB = distance(mid, wt.frames[0][19]), toC = distance(mid, wt.frames[0][63]);
                if ((Algo) a == Algo::Step)
                    expect(toB < 1.0e-9 || toC < 1.0e-9, "STEP holds a key");
                else
                    expect(toB > 0.01 && toC > 0.01, juce::String(algoName((Algo) a)) + " computes the frames between");
            }
        }

        beginTest("Optimal transport: one partial glides from harmonic 2 to harmonic 8; a crossfade never leaves the two");
        {
            const Spectrum a = partial(2), b = partial(8);
            const Spectrum mid = wavemorph::morph(Algo::Transport, a, b, 0.5);
            const Spectrum fade = wavemorph::morph(Algo::Linear, a, b, 0.5);
            logMessage("half way, share of the energy on harmonics 2 / 4 / 8: transport "
                       + juce::String(share(mid, 2), 4) + " / " + juce::String(share(mid, 4), 4) + " / " + juce::String(share(mid, 8), 4)
                       + ", crossfade " + juce::String(share(fade, 2), 4) + " / " + juce::String(share(fade, 4), 4) + " / "
                       + juce::String(share(fade, 8), 4));
            expect(share(mid, 4) > 0.999, "ONE partial, at the harmonic an octave from each");
            expect(share(mid, 2) < 1.0e-6 && share(mid, 8) < 1.0e-6);
            expectWithinAbsoluteError(share(fade, 2), 0.5, 1.0e-9);
            expectWithinAbsoluteError(share(fade, 8), 0.5, 1.0e-9);
            expect(share(fade, 4) < 1.0e-12);
            expectWithinAbsoluteError(mid.energy(), 1.0, 1.0e-9, "the energy is kept");
            expectWithinAbsoluteError(fade.energy(), 0.5, 1.0e-9, "(a crossfade of two partials loses half of it)");

            // The whole table: the partial climbs steadily, never on more than two neighbouring harmonics.
            const Wavetable wt = Keys::build({ { a, 0.0f }, { b, 1.0f } }, plain(Algo::Transport));
            const Wavetable lin = Keys::build({ { a, 0.0f }, { b, 1.0f } }, plain(Algo::Linear));
            double last = 0.0;
            for (int f = 0; f < 64; ++f)
            {
                const Spectrum s = wavemorph::analyse(wt.frames[0][(size_t) f]);
                const double c = centroid(s);
                expect(c >= last - 1.0e-6, "the partial only climbs");
                last = c;
                const int lo = (int) std::floor(c + 1.0e-6);
                expect(share(s, lo) + (lo < 127 ? share(s, lo + 1) : 0.0) > 0.9999, "on two neighbouring harmonics at most, frame " + juce::String(f));
                const Spectrum l = wavemorph::analyse(lin.frames[0][(size_t) f]);
                expect(share(l, 2) + share(l, 8) > 0.9999, "the crossfade stays on 2 and 8");
            }
            expectWithinAbsoluteError(last, 8.0, 1.0e-6);

            // A formant (a bump of harmonics) moves as a whole.
            Spectrum low, high;
            for (int h = 1; h <= 60; ++h)
            {
                low.c[(size_t) h] = std::polar(std::exp(-0.5 * std::pow((h - 6.0) / 1.5, 2.0)), 0.3 * h);
                high.c[(size_t) h] = std::polar(std::exp(-0.5 * std::pow((h - 24.0) / 3.0, 2.0)), 0.1 * h);
            }
            const Spectrum moved = wavemorph::morph(Algo::Transport, low, high, 0.5);
            const Spectrum faded = wavemorph::morph(Algo::Linear, low, high, 0.5);
            double movedAt12 = 0.0, fadedAt12 = 0.0;
            for (int h = 9; h <= 16; ++h)
            {
                movedAt12 += share(moved, h);
                fadedAt12 += share(faded, h);
            }
            logMessage("a formant from harmonic 6 to 24, half way: energy on harmonics 9-16 with transport "
                       + juce::String(movedAt12, 3) + ", with a crossfade " + juce::String(fadedAt12, 3));
            expect(movedAt12 > 0.9 && fadedAt12 < 0.05, "the formant is half way (harmonic 12), not in two places");
        }

        beginTest("Phase: a harmonic in opposite phase in the two keys is silent half way in a crossfade, and at full level with PHAS");
        {
            const Spectrum a = partial(3, 1.0, 0.0), b = partial(3, 1.0, PI);
            const double fade = wavemorph::morph(Algo::Linear, a, b, 0.5).energy();
            const double phase = wavemorph::morph(Algo::Phase, a, b, 0.5).energy();
            const double moved = wavemorph::morph(Algo::Transport, a, b, 0.5).energy();
            const double voc = wavemorph::morph(Algo::Vocoder, a, b, 0.5).energy();
            logMessage("energy half way: LIN " + juce::String(fade, 6) + "  PHAS " + juce::String(phase, 6) + "  TRNS "
                       + juce::String(moved, 6) + "  VOC " + juce::String(voc, 6));
            expect(fade < 1.0e-20);
            expectWithinAbsoluteError(phase, 1.0, 1.0e-9);
            expectWithinAbsoluteError(moved, 1.0, 1.0e-9);
            expectWithinAbsoluteError(voc, 1.0, 1.0e-6);
        }

        beginTest("Vocoder: half way is the second key's envelope on the first key's harmonics");
        {
            // First key: every harmonic, falling 6 dB per octave (dark). Second key: odd harmonics
            // only, in a bump around harmonic 15 (bright, hollow).
            Spectrum saw, hollow;
            for (int h = 1; h <= 40; ++h)
            {
                saw.c[(size_t) h] = std::polar(1.0 / h, -0.5 * PI);
                if (h % 2 == 1)
                    hollow.c[(size_t) h] = std::polar(std::exp(-std::pow((h - 15.0) / 4.0, 2.0)), -0.5 * PI);
            }
            auto evenToOdd = [](const Spectrum& s)       // harmonics 14 and 16 against 13, 15, 17
            {
                return (s.magnitude(14) + s.magnitude(16)) / 2.0 / ((s.magnitude(13) + s.magnitude(15) + s.magnitude(17)) / 3.0);
            };
            const Spectrum voc = wavemorph::morph(Algo::Vocoder, saw, hollow, 0.5);
            const Spectrum fade = wavemorph::morph(Algo::Linear, saw, hollow, 0.5);
            logMessage("centroid (harmonic): first key " + juce::String(centroid(saw), 2) + ", second " + juce::String(centroid(hollow), 2)
                       + "; half way VOC " + juce::String(centroid(voc), 2) + ", LIN " + juce::String(centroid(fade), 2));
            logMessage("even harmonics against odd around 15: first key " + juce::String(evenToOdd(saw), 3) + ", second "
                       + juce::String(evenToOdd(hollow), 3) + "; half way VOC " + juce::String(evenToOdd(voc), 3) + ", LIN "
                       + juce::String(evenToOdd(fade), 3));
            // The envelope is the second key's ...
            expectWithinAbsoluteError(centroid(voc), centroid(hollow), 1.0);
            expect(centroid(fade) < centroid(hollow) - 3.0, "(a crossfade is still half the dark key)");
            const auto envVoc = wavemorph::envelopeOf(voc), envB = wavemorph::envelopeOf(hollow);
            for (int h = 11; h <= 19; ++h)       // the bump, down to 9 dB below its top
                expect(std::abs(20.0 * std::log10(envVoc[(size_t) h] / envB[(size_t) h])) < 1.5, "envelope at harmonic " + juce::String(h));
            // ... and the harmonics are the first key's: the even ones are there, as strong as the odd.
            expect(evenToOdd(voc) > 0.8 && evenToOdd(voc) < 1.25);
            expect(evenToOdd(fade) < 0.2 && evenToOdd(hollow) < 1.0e-6);
            // A quarter of the way the envelope is half way, the harmonics still the first key's.
            const Spectrum quarter = wavemorph::morph(Algo::Vocoder, saw, hollow, 0.25);
            expect(centroid(quarter) > centroid(saw) + 1.0 && centroid(quarter) < centroid(hollow) - 1.0);
            expect(evenToOdd(quarter) > 0.8);
            // Three quarters: the even harmonics are on their way out.
            const Spectrum late = wavemorph::morph(Algo::Vocoder, saw, hollow, 0.75);
            expect(evenToOdd(late) > 0.3 && evenToOdd(late) < 0.7);
        }

        beginTest("CLSC between keys: frames of 128 samples and 8 bits, crossfaded");
        {
            const Wavetable wt = Keys::build({ { randomSpectrum(4u), 0.0f }, { randomSpectrum(5u), 1.0f } }, plain(Algo::Classic));
            const Wavetable lin = Keys::build({ { randomSpectrum(4u), 0.0f }, { randomSpectrum(5u), 1.0f } }, plain(Algo::Linear));
            float worst = 0.0f;
            for (int f = 0; f < 64; ++f)
                for (int n = 0; n < wavemorph::N; n += 2)
                {
                    const float v = wt.frames[0][(size_t) f][(size_t) n];
                    expect(v == wt.frames[0][(size_t) f][(size_t) n + 1]);
                    expect(std::abs(v * 127.0f - std::round(v * 127.0f)) < 1.0e-3f);
                    worst = std::max(worst, std::abs(v - lin.frames[0][(size_t) f][(size_t) n]));
                }
            expect(worst <= 0.5f / 127.0f + 1.0e-4f, "each stored sample is the crossfade's, rounded: " + juce::String(worst, 5));
            expect(tableDistance(wt.frames[0], lin.frames[0]) > 1.0e-3, "and so not the same table");
        }

        beginTest("INTP = CLSC: on a high note the unfiltered read-out puts energy above the band-limited table's top harmonic");
        {
            auto e = makeEngine(1);
            e->setParameter(P::Position, 1.0f);                     // the bright end of the saw table
            const double f0 = 1760.0;                               // A6: 44 cycles in 1200 samples
            // The band-limited read-out of this note uses the mip level that keeps 4 harmonics.
            struct Result { double beyond = 0.0, inharmonic = 0.0; };
            auto measure = [&](int mode)
            {
                e->setParameter(P::InterpMode, (float) mode);
                const auto x = heldNote(*e, 93, 1200);
                const double total = meanSquare(x);
                double low = 0.0, harmonic = 0.0;
                for (int h = 1; h * f0 < SR / 2.0; ++h)
                {
                    const double m = magnitude(x, f0 * h);
                    harmonic += 0.5 * m * m;
                    if (h <= 4)
                        low += 0.5 * m * m;
                }
                for (float v : x)
                    expect(std::isfinite(v) && std::abs(v) < 1.0f);
                return Result { 1.0 - low / total, 1.0 - harmonic / total };
            };
            static const char* const names[] = { "LIN", "SPEC", "STEP", "VINT", "CLSC" };
            Result r[5];
            for (int mode = 0; mode < 5; ++mode)
            {
                r[mode] = measure(mode);
                logMessage(juce::String(names[mode]) + ": energy above harmonic 4 (7 kHz) " + juce::String(100.0 * r[mode].beyond, 2)
                           + " %, of it not on any harmonic of the note " + juce::String(100.0 * r[mode].inharmonic, 2) + " %");
            }
            expect(r[0].beyond < 0.01 && r[3].beyond < 0.01, "LIN and VINT stay band-limited");
            expect(r[4].beyond > 0.05, "CLSC has a top end");
            expect(r[4].inharmonic > 0.005, "part of which is aliasing (not harmonics of the note)");
            expect(r[4].inharmonic > 3.0 * r[2].inharmonic, "more than STEP, which reads the band-limited copy");

            // 64 places: positions inside one place are one sound, the next place is another.
            e->setParameter(P::TableSelect, 7.0f);
            auto at = [&](float pos) { e->setParameter(P::Position, pos); return heldNote(*e, 57, 2400); };
            const auto p0 = at(20.0f / 63.0f), p1 = at(20.3f / 63.0f), p2 = at(21.0f / 63.0f);
            double same = 0.0, other = 0.0;
            for (size_t i = 0; i < p0.size(); ++i)
            {
                same = std::max(same, (double) std::abs(p0[i] - p1[i]));
                other = std::max(other, (double) std::abs(p0[i] - p2[i]));
            }
            expect(same == 0.0 && other > 0.01, "the scan moves in 64 whole steps");
        }

        beginTest("A key is one cycle of its sample, at the pitch of the key whatever the pitch of the sample");
        {
            expect(smp.a >= 0 && smp.b >= 0 && smp.noise >= 0, "the sample bank has room for the test samples");
            const auto* a = SampleBank::get().entry(smp.a);
            const auto* b = SampleBank::get().entry(smp.b);
            const auto* noise = SampleBank::get().entry(smp.noise);
            if (a == nullptr || b == nullptr || noise == nullptr)
                return;
            Spectrum s;
            bool pitched = false;
            expect(Keys::extract(a->left.data(), nullptr, (int) a->left.size(), 44100.0, 0.02f, s, &pitched) && pitched);
            logMessage("196 Hz sample at 2 %: harmonic 1 holds " + juce::String(100.0 * share(s, 1), 2) + " % of the cycle");
            expect(share(s, 1) > 0.99);
            expect(Keys::extract(a->left.data(), nullptr, (int) a->left.size(), 44100.0, 0.9f, s, &pitched) && pitched);
            // There: fundamental 0.55, third harmonic 0.9 -> 27 % / 73 % of the energy.
            logMessage("196 Hz sample at 90 %: harmonic 1 " + juce::String(100.0 * share(s, 1), 1) + " %, harmonic 3 "
                       + juce::String(100.0 * share(s, 3), 1) + " %");
            expectWithinAbsoluteError(share(s, 1), 0.272, 0.03);
            expectWithinAbsoluteError(share(s, 3), 0.728, 0.03);
            expect(Keys::extract(b->left.data(), nullptr, (int) b->left.size(), 44100.0, 0.5f, s, &pitched) && pitched);
            // A different pitch (311 Hz): still harmonics 1 and 2 of the frame, 80 % / 20 %.
            logMessage("311 Hz sample at 50 %: harmonic 1 " + juce::String(100.0 * share(s, 1), 1) + " %, harmonic 2 "
                       + juce::String(100.0 * share(s, 2), 1) + " %");
            expectWithinAbsoluteError(share(s, 1), 0.8, 0.03);
            expectWithinAbsoluteError(share(s, 2), 0.2, 0.03);
            // Stereo is mixed; noise has no pitch and still gives a cycle.
            Spectrum stereo;
            expect(Keys::extract(b->left.data(), b->left.data(), (int) b->left.size(), 44100.0, 0.5f, stereo));
            expectWithinAbsoluteError(share(stereo, 2), share(s, 2), 1.0e-6);
            expect(Keys::extract(noise->left.data(), nullptr, (int) noise->left.size(), 44100.0, 0.5f, s, &pitched));
            expect(! pitched && s.energy() > 1.0e-4);
            for (int h = 1; h <= wavemorph::H; ++h)
                expect(std::isfinite(s.c[(size_t) h].real()) && std::isfinite(s.c[(size_t) h].imag()));
            // Nothing to take.
            std::vector<float> silence(4096, 0.0f);
            expect(! Keys::extract(silence.data(), nullptr, 4096, 44100.0, 0.5f, s));
            expect(! Keys::extract(nullptr, nullptr, 0, 44100.0, 0.5f, s));
            expect(! Keys::extract(silence.data(), nullptr, 10, 44100.0, 0.5f, s));
        }

        beginTest("The KEYS table from the cells: built when in use, heard, each key where it was put");
        {
            auto e = makeEngine(1);
            // Keys set while another table plays build nothing ...
            e->setParameter(P::KeySampleBase + 0, (float) (smp.a + 1));
            e->setParameter(P::KeyPositionBase + 0, 0.02f);
            e->setParameter(P::KeySampleBase + 1, (float) (smp.a + 1));
            e->setParameter(P::KeyPositionBase + 1, 0.9f);
            e->setParameter(P::KeySampleBase + 2, (float) (smp.b + 1));
            e->setParameter(P::KeyPositionBase + 2, 0.98f);
            expect(! e->keyTablePending(), "nothing is built for a table that is not playing");
            expectEquals(e->getTable(WavetableEngine::KEY_TABLE).numFrames, 1, "KEYS starts as a sine");
            expectEquals(e->getKeyCount(), 3);
            // ... choosing the table builds it, and the audio thread takes it at its next block.
            e->setParameter(P::TableSelect, (float) WavetableEngine::KEY_TABLE);
            expect(e->keyTablePending());
            silentBlock(*e);
            expect(! e->keyTablePending());
            const Wavetable& wt = e->getTable(WavetableEngine::KEY_TABLE);
            expectEquals(wt.numFrames, 64);
            expectEquals(wt.getMipLevelCount(), 8);
            expect(e->isKeyPitched(0) && e->isKeyPitched(1) && e->isKeyPitched(2) && ! e->isKeyPitched(3));
            // Keys 1, 2, 3 at places 0, 1/7, 2/7: frames 0, 32 (31.5 rounded) and 63.
            expect(share(wt.frames[0][0], 1) > 0.99, "first frame: the pure fundamental");
            expectWithinAbsoluteError(share(wt.frames[0][32], 3), 0.728, 0.03);
            expectWithinAbsoluteError(share(wt.frames[0][63], 2), 0.49, 0.03);

            // Heard: at POS 0 the note is a fundamental, in the middle it has the third harmonic.
            auto third = [&](float pos)
            {
                e->setParameter(P::Position, pos);
                const auto x = heldNote(*e, 69, 1200);
                return magnitude(x, 1320.0) / (magnitude(x, 440.0) + 1.0e-12);
            };
            const double atStart = third(0.0f), atMiddle = third(32.0f / 63.0f);
            logMessage("third harmonic against the fundamental, A4: POS 0 " + juce::String(atStart, 4) + ", at key 2 " + juce::String(atMiddle, 3));
            expect(atStart < 0.05, "(the sample has 2 % of it there)");
            expectWithinAbsoluteError(atMiddle, 0.9 / 0.55, 0.1);

            // PLC: key 3 moved to the front is the first frame; only order and spacing count.
            e->setParameter(P::KeyPlaceBase + 2, 0.0f);
            e->setParameter(P::KeyPlaceBase + 0, 0.5f);
            e->setParameter(P::KeyPlaceBase + 1, 0.9f);
            expect(e->keyTablePending(), "a key cell rebuilds the table that is playing");
            silentBlock(*e);
            expectWithinAbsoluteError(share(wt.frames[0][0], 2), 0.49, 0.03);
            expect(share(wt.frames[0][35], 1) > 0.99, "key 1 at 0.5 of 0 .. 0.9: frame 35");
            expectWithinAbsoluteError(share(wt.frames[0][63], 3), 0.728, 0.03);

            // The same value again builds nothing.
            e->setParameter(P::KeyPlaceBase + 1, 0.9f);
            e->setParameter(P::KeyAlgo, 0.0f);
            expect(! e->keyTablePending());

            // A key switched OFF leaves; with none left the table is a sine again.
            for (int k = 0; k < 3; ++k)
                e->setParameter(P::KeySampleBase + k, 0.0f);
            silentBlock(*e);
            expectEquals(e->getKeyCount(), 0);
            expectEquals(wt.numFrames, 1);
            expect(share(wt.frames[0][0], 1) > 0.9999);
            expect(std::sqrt(meanSquare(heldNote(*e, 69, 1200))) > 0.05, "and still sounds");
        }

        beginTest("The pages: KEYS, K SMP, K POS, K PLC before the filter; every cell changes the table; none can be locked");
        {
            auto e = makeEngine(1);
            const auto pages = e->getParamPages();
            expect(pages.size() == 13);
            expect(pages[4].name == "KEYS" && pages[5].name == "K SMP" && pages[6].name == "K POS" && pages[7].name == "K PLC");
            expect(pages[8].name == "FILTR");
            expect(pages[pages.size() - 3].name == "STRIP" && pages[pages.size() - 2].name == "LFO1" && pages.back().name == "LFO2");
            static const char* const labels[8] = { "ALGO", "KFRM", "BIAS", "WRAP", "ALGN", "NORM", "TILT", "HARM" };
            for (int c = 0; c < 8; ++c)
            {
                expectEquals(juce::String(pages[4].params[(size_t) c].label), juce::String(labels[c]));
                expectEquals(pages[4].params[(size_t) c].id, 33 + c);
                expectEquals(pages[5].params[(size_t) c].id, 41 + c);
                expectEquals(pages[6].params[(size_t) c].id, 49 + c);
                expectEquals(pages[7].params[(size_t) c].id, 57 + c);
                expectEquals(juce::String(pages[5].params[(size_t) c].label), "SMP" + juce::String(c + 1));
                expectEquals(juce::String(pages[6].params[(size_t) c].label), "POS" + juce::String(c + 1));
                expectEquals(juce::String(pages[7].params[(size_t) c].label), "PLC" + juce::String(c + 1));
            }
            const auto& smpCell = pages[5].params[0];
            expectEquals(smpCell.numChoices, SampleBank::get().size() + 1, "OFF and every sample of the bank");
            expect(smpCell.format(0.0f) == "OFF" && smpCell.format((float) (smp.a + 1)) == juce::String(SampleBank::get().entry(smp.a)->name));
            expect(pages[4].params[0].format(4.0f) == "TRNS" && pages[4].params[0].format(5.0f) == "VOC");
            expect(pages[0].find(P::InterpMode)->format(4.0f) == "CLSC" && pages[0].find(P::InterpMode)->numChoices == 5);

            for (int id = 33; id <= 64; ++id)
                expect(e->isRebuildParam(id), "id " + juce::String(id) + " rebuilds: no lock, no pressure range");
            expect(e->isRebuildParam(P::UserSample) && e->isRebuildParam(P::UserFrames));
            expect(! e->isRebuildParam(P::Position) && ! e->isRebuildParam(P::InterpMode) && ! e->isRebuildParam(65)
                   && ! e->isRebuildParam(P::TableSelect));

            // Eight keys in use, from three samples. Then every cell of the four pages, at the two
            // ends of its range: the table that comes out is not the one before.
            static const float positions[8] = { 0.02f, 0.3f, 0.2f, 0.6f, 0.4f, 0.9f, 0.7f, 0.98f };
            auto setUp = [&]
            {
                for (auto& page : e->getParamPages())
                    for (auto& p : page.params)
                        e->setParameter(p.id, p.defaultVal);
                for (int k = 0; k < 8; ++k)
                {
                    e->setParameter(P::KeySampleBase + k, (float) ((k % 3 == 1 ? smp.b : (k % 3 == 2 ? smp.noise : smp.a)) + 1));
                    e->setParameter(P::KeyPositionBase + k, positions[k]);
                }
                e->setParameter(P::TableSelect, (float) WavetableEngine::KEY_TABLE);
                silentBlock(*e);
            };
            setUp();
            const auto base = e->getTable(WavetableEngine::KEY_TABLE).frames[0];
            expectEquals((int) base.size(), 64);
            for (int page = 4; page <= 7; ++page)
                for (auto& p : pages[(size_t) page].params)
                {
                    double least = 10.0;
                    for (float v : { p.minVal, p.maxVal })
                    {
                        if (std::abs(v - e->getParameter(p.id)) < 1.0e-6f)
                            continue;
                        const float before = e->getParameter(p.id);
                        e->setParameter(p.id, v);
                        expect(e->keyTablePending(), juce::String(p.label) + " rebuilds");
                        silentBlock(*e);
                        const auto& frames = e->getTable(WavetableEngine::KEY_TABLE).frames[0];
                        least = std::min(least, tableDistance(base, frames));
                        for (auto& frame : frames)
                            for (float s : frame)
                                expect(std::isfinite(s) && std::abs(s) <= 1.0001f);
                        e->setParameter(p.id, before);
                        silentBlock(*e);
                    }
                    expect(least > 1.0e-3 && least < 10.0, pages[(size_t) page].name + " / " + p.label + " changes the table: "
                                                               + juce::String(least, 5));
                    expect(tableDistance(base, e->getTable(WavetableEngine::KEY_TABLE).frames[0]) == 0.0,
                           juce::String(p.label) + " put back gives the same table");
                }
        }

        beginTest("KFRM, WRAP, BIAS, NORM, HARM, TILT, ALGN do what they say");
        {
            const Spectrum a = partial(2), b = partial(8), c = partial(5);
            for (int option = 0; option < Keys::NUM_FRAME_OPTIONS; ++option)
            {
                auto s = plain(Algo::Linear, Keys::frameCount(option));
                std::vector<int> at;
                const Wavetable wt = Keys::build({ { a, 0.0f }, { c, 0.5f }, { b, 1.0f } }, s, &at);
                expectEquals(wt.numFrames, Keys::frameCount(option));
                expectEquals((int) at.size(), 3);
                expect(at[0] == 0 && at[2] == wt.numFrames - 1 && at[1] == juce::roundToInt(0.5f * (float) (wt.numFrames - 1)));
            }
            expectEquals(Keys::frameCount(0), 8);
            expectEquals(Keys::frameCount(5), 256);

            // Eight keys in eight frames, even at one place: one frame each, in order.
            {
                std::vector<Keys::Key> eight;
                for (int k = 0; k < 8; ++k)
                    eight.push_back({ partial(k + 1), 0.5f });
                std::vector<int> at;
                const Wavetable wt = Keys::build(eight, plain(Algo::Transport, 8), &at);
                expect(at == std::vector<int> ({ 0, 1, 2, 3, 4, 5, 6, 7 }));
                for (int k = 0; k < 8; ++k)
                    expect(share(wt.frames[0][(size_t) k], k + 1) > 0.9999);
                // With WRAP there is a ninth: the table grows to hold it.
                auto s = plain(Algo::Transport, 8);
                s.wrap = true;
                const Wavetable wrapped = Keys::build(eight, s, &at);
                expectEquals(wrapped.numFrames, 9);
                expect(share(wrapped.frames[0][8], 1) > 0.9999);
            }

            // WRAP: the last frame is the first key again, and the way back is one average step long.
            {
                auto s = plain(Algo::Linear);
                s.wrap = true;
                std::vector<int> at;
                const Wavetable wt = Keys::build({ { a, 0.0f }, { c, 0.5f }, { b, 1.0f } }, s, &at);
                expect(at == std::vector<int> ({ 0, 21, 42, 63 }));
                expect(distance(wt.frames[0][0], wt.frames[0][63]) < 1.0e-9);
                expect(share(wt.frames[0][42], 8) > 0.9999);
            }

            // BIAS: a quarter of the way from key to key (frame 16 of 0 .. 63).
            {
                double later[3];
                int i = 0;
                for (float bias : { -1.0f, 0.0f, 1.0f })
                {
                    auto s = plain(Algo::Linear);
                    s.bias = bias;
                    later[i++] = share(Keys::build({ { a, 0.0f }, { b, 1.0f } }, s).frames[0][16], 8);
                }
                logMessage("share of the later key a quarter of the way: BIAS -1 " + juce::String(later[0], 4) + ", 0 "
                           + juce::String(later[1], 4) + ", +1 " + juce::String(later[2], 4));
                expect(later[0] < 0.2 * later[1] && later[2] > 3.0 * later[1]);
                // ... and STEP changes key earlier or later.
                auto s = plain(Algo::Step);
                s.bias = 1.0f;
                expect(share(Keys::build({ { a, 0.0f }, { b, 1.0f } }, s).frames[0][16], 8) > 0.9999);
                s.bias = 0.0f;
                expect(share(Keys::build({ { a, 0.0f }, { b, 1.0f } }, s).frames[0][16], 2) > 0.9999);
            }

            // NORM: two keys that partly cancel. OFF dips, PEAK is full scale everywhere, RMS is level.
            {
                Spectrum x = randomSpectrum(11u), y = randomSpectrum(11u);
                for (int h = 1; h <= 40; h += 2)
                    y.c[(size_t) h] = -y.c[(size_t) h];
                float lowRms[3], highRms[3], lowPeak[3], highPeak[3];
                for (int norm = 0; norm < Keys::NumNorms; ++norm)
                {
                    auto s = plain(Algo::Linear);
                    s.norm = norm;
                    const Wavetable wt = Keys::build({ { x, 0.0f }, { y, 1.0f } }, s);
                    lowRms[norm] = lowPeak[norm] = 10.0f;
                    highRms[norm] = highPeak[norm] = 0.0f;
                    for (auto& frame : wt.frames[0])
                    {
                        lowRms[norm] = std::min(lowRms[norm], wavemorph::rmsOf(frame));
                        highRms[norm] = std::max(highRms[norm], wavemorph::rmsOf(frame));
                        lowPeak[norm] = std::min(lowPeak[norm], wavemorph::peakOf(frame));
                        highPeak[norm] = std::max(highPeak[norm], wavemorph::peakOf(frame));
                    }
                    logMessage(juce::String(norm == 0 ? "NORM OFF " : norm == 1 ? "NORM PEAK" : "NORM RMS ") + " rms "
                               + juce::String(lowRms[norm], 3) + " .. " + juce::String(highRms[norm], 3) + ", peak "
                               + juce::String(lowPeak[norm], 3) + " .. " + juce::String(highPeak[norm], 3));
                }
                expect(lowRms[0] < 0.75f * highRms[0], "as computed, the crossfade dips");
                expect(lowPeak[1] > 0.9999f && highPeak[1] < 1.0001f);
                expect(lowRms[2] > 0.999f * highRms[2] && highPeak[2] <= 1.0001f);
            }

            // HARM and TILT act on the keys; ALGN turns a key without changing a level.
            {
                const Spectrum x = randomSpectrum(21u, 60);
                auto s = plain(Algo::Linear);
                s.harmonics = 10;
                Spectrum got = wavemorph::analyse(Keys::build({ { x, 0.0f } }, s).frames[0][0]);
                double above = 0.0;
                for (int h = 11; h <= wavemorph::H; ++h)
                    above += std::norm(got.c[(size_t) h]);
                expect(above < 1.0e-12 && got.magnitude(10) > 1.0e-4);
                s = plain(Algo::Linear);
                s.tilt = -6.0f;
                got = wavemorph::analyse(Keys::build({ { x, 0.0f } }, s).frames[0][0]);
                // 6 dB per octave down: harmonic 8 against harmonic 1 loses 18 dB.
                expectWithinAbsoluteError(20.0 * std::log10((got.magnitude(8) / got.magnitude(1)) / (x.magnitude(8) / x.magnitude(1))), -18.0, 0.05);
                s = plain(Algo::Linear);
                s.align = true;
                got = wavemorph::analyse(Keys::build({ { x, 0.0f } }, s).frames[0][0]);
                expectWithinAbsoluteError(wavemorph::wrapPi(std::arg(got.c[1]) + 0.5 * PI), 0.0, 1.0e-6);
                const Spectrum raw = wavemorph::analyse(asKey(x));
                const double gain = got.magnitude(1) / raw.magnitude(1);     // (the peak, and so the scale, moves with the phases)
                for (int h = 1; h <= 60; ++h)
                    expectWithinAbsoluteError(got.magnitude(h), gain * raw.magnitude(h), 1.0e-6);
                // Two cuts of one wave at different phases: aligned, they are the same key.
                Spectrum turned = x;
                for (int h = 1; h <= 60; ++h)
                    turned.c[(size_t) h] *= std::polar(1.0, 1.3 * h);
                const Wavetable both = Keys::build({ { x, 0.0f }, { turned, 1.0f } }, s);
                expect(tableDistance({ both.frames[0][0] }, { both.frames[0][63] }) < 1.0e-5);
                expect(distance(both.frames[0][0], both.frames[0][31]) < 1.0e-5, "and nothing happens between them");
            }
        }

        beginTest("Bounded and finite: every algorithm, odd keys, every setting at its ends");
        {
            std::vector<std::vector<Keys::Key>> sets;
            sets.push_back({});
            sets.push_back({ { randomSpectrum(31u), 0.4f } });
            sets.push_back({ { Spectrum(), 0.0f }, { randomSpectrum(32u), 1.0f } });               // a silent key
            sets.push_back({ { partial(1), 0.0f }, { partial(127), 1.0f } });
            sets.push_back({ { randomSpectrum(33u, 127), 0.0f }, { randomSpectrum(34u, 3), 0.0f }, { randomSpectrum(35u, 127), 0.0f } });
            {
                std::vector<Keys::Key> eight;
                for (int k = 0; k < 8; ++k)
                    eight.push_back({ randomSpectrum(40u + (uint32_t) k, 20 + 15 * k), (float) ((k * 5) % 8) / 7.0f });
                sets.push_back(eight);
            }
            int built = 0;
            float worstPeak = 0.0f;
            for (auto& keys : sets)
                for (int a = 0; a < (int) Algo::Count; ++a)
                    for (int variant = 0; variant < 6; ++variant)
                    {
                        Keys::Settings s;
                        s.algo = (Algo) a;
                        s.frames = variant == 1 ? 8 : (variant == 2 ? 256 : 64);
                        s.bias = variant == 3 ? -1.0f : (variant == 4 ? 1.0f : 0.0f);
                        s.wrap = variant % 2 == 1;
                        s.align = variant != 5;
                        s.norm = variant % 3;
                        s.tilt = variant == 3 ? 6.0f : (variant == 4 ? -6.0f : 0.0f);
                        s.harmonics = variant == 5 ? 1 : 127;
                        const Wavetable wt = Keys::build(keys, s);
                        ++built;
                        expect(wt.numFrames >= 1 && wt.numFrames <= 256 && wt.getMipLevelCount() == 8);
                        for (auto& level : wt.frames)
                            for (auto& frame : level)
                                for (float v : frame)
                                {
                                    if (! std::isfinite(v))
                                        expect(false, "not finite");
                                    worstPeak = std::max(worstPeak, std::abs(v));
                                }
                        if (s.norm != Keys::NormOff)
                            for (auto& frame : wt.frames[0])
                                expect(wavemorph::peakOf(frame) <= 1.0001f);
                    }
            logMessage(juce::String(built) + " tables built; the largest sample in any of them (NORM OFF included) " + juce::String(worstPeak, 3));
            expect(worstPeak < 4.0f);
        }

        beginTest("Projects: the key frames come back, samples by name; a project without them loads as before");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                audio.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.setEngineType(2, EngineType::Wavetable);
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                seq.getPattern(p).getTrack(2).engineType = EngineType::Wavetable;
            auto set = [&](int id, float v) { audio.setTrackParameter(2, id, v); };
            set(P::KeySampleBase + 0, (float) (smp.a + 1));
            set(P::KeyPositionBase + 0, 0.02f);
            set(P::KeySampleBase + 3, (float) (smp.b + 1));
            set(P::KeyPositionBase + 3, 0.9f);
            set(P::KeySampleBase + 6, (float) (smp.a + 1));
            set(P::KeyPositionBase + 6, 0.9f);
            set(P::KeyPlaceBase + 6, 0.2f);
            set(P::KeyAlgo, (float) (int) Algo::Transport);
            set(P::KeyFrames, 4.0f);
            set(P::KeyBias, 0.25f);
            set(P::KeyWrap, 1.0f);
            set(P::KeyNorm, (float) Keys::NormPeak);
            set(P::KeyTilt, -1.5f);
            set(P::KeyHarmonics, 90.0f);
            set(P::InterpMode, (float) (int) WavetableInterpMode::Classic);
            set(P::TableSelect, (float) WavetableEngine::KEY_TABLE);

            const ProjectData data = ProjectBridge::capture({ seq, audio }, "keys");
            const auto& td = data.patterns[(size_t) data.activePattern].tracks[2];
            expectEquals(td.keySamples.size(), 8);
            expect(td.keySamples[0] == juce::String(SampleBank::get().entry(smp.a)->name)
                   && td.keySamples[3] == juce::String(SampleBank::get().entry(smp.b)->name) && td.keySamples[1].isEmpty());
            expect(data.patterns[(size_t) data.activePattern].tracks[0].keySamples.isEmpty(), "only wavetable tracks");
            const juce::String json = data.toJSON();
            expect(json.contains("keySamples"));

            auto load = [](const ProjectData& d, SequencerCore& s, AudioEngine& a) -> WavetableEngine*
            {
                ProjectBridge::apply(d, { s, a });
                auto* wt = dynamic_cast<WavetableEngine*> (a.getEngine(2));
                if (wt != nullptr)
                {
                    wt->prepareToPlay(SR, 64);
                    silentBlock(*wt);
                }
                return wt;
            };
            auto* first = dynamic_cast<WavetableEngine*> (audio.getEngine(2));
            first->prepareToPlay(SR, 64);
            silentBlock(*first);
            expectEquals(first->getTable(WavetableEngine::KEY_TABLE).numFrames, 128);

            {
                SequencerCore seq2;
                AudioEngine audio2;
                auto* wt = load(ProjectData::fromJSON(json), seq2, audio2);
                expect(wt != nullptr);
                if (wt != nullptr)
                {
                    for (int id = 33; id <= 64; ++id)
                        expectWithinAbsoluteError(wt->getParameter(id), first->getParameter(id), 1.0e-6f, "id " + juce::String(id));
                    expectEquals((int) wt->getParameter(P::TableSelect), (int) WavetableEngine::KEY_TABLE);
                    expectEquals((int) wt->getParameter(P::InterpMode), 4);
                    expect(tableDistance(wt->getTable(WavetableEngine::KEY_TABLE).frames[0],
                                         first->getTable(WavetableEngine::KEY_TABLE).frames[0]) == 0.0, "the same table");
                    expectEquals((int) seq2.getPattern().getTrack(2).soundParams[P::KeySampleBase + 3], smp.b + 1);
                }
            }
            {
                // The bank has changed since the project was saved (the stored numbers point at
                // other samples): the names decide.
                ProjectData moved = ProjectData::fromJSON(json);
                auto& params = moved.patterns[(size_t) moved.activePattern].tracks[2].soundParams;
                params[P::KeySampleBase + 0] = 1.0f;
                params[P::KeySampleBase + 3] = 2.0f;
                SequencerCore seq2;
                AudioEngine audio2;
                auto* wt = load(moved, seq2, audio2);
                expect(wt != nullptr);
                if (wt != nullptr)
                {
                    expectEquals((int) wt->getParameter(P::KeySampleBase + 0), smp.a + 1);
                    expectEquals((int) wt->getParameter(P::KeySampleBase + 3), smp.b + 1);
                    expectEquals((int) wt->getParameter(P::KeySampleBase + 1), 0);
                }
            }
            {
                // A project from before key frames: no "keySamples", none of the ids 33-64, a table
                // and an interpolation from the old lists. It loads; those cells are at their
                // defaults; TABL and INTP mean what they meant.
                ProjectData old = ProjectData::fromJSON(json);
                auto& track = old.patterns[(size_t) old.activePattern].tracks[2];
                track.keySamples.clear();
                for (int id = 33; id <= 64; ++id)
                    track.soundParams.erase(id);
                track.soundParams[P::TableSelect] = (float) WavetableEngine::USER_TABLE;
                track.soundParams[P::InterpMode] = 3.0f;
                track.soundParams[P::UserSample] = (float) smp.a;
                track.sampleName = juce::String(SampleBank::get().entry(smp.a)->name);
                const juce::String oldJson = old.toJSON();
                expect(! oldJson.contains("keySamples"));
                SequencerCore seq2;
                AudioEngine audio2;
                auto* wt = load(ProjectData::fromJSON(oldJson), seq2, audio2);
                expect(wt != nullptr);
                if (wt != nullptr)
                {
                    WavetableEngine fresh(1);
                    for (int id = 33; id <= 64; ++id)
                        expectWithinAbsoluteError(wt->getParameter(id), fresh.getParameter(id), 1.0e-6f, "default of id " + juce::String(id));
                    expectEquals(wt->getKeyCount(), 0);
                    expectEquals((int) wt->getParameter(P::TableSelect), 9);
                    expectEquals((int) wt->getParameter(P::InterpMode), 3);
                    expectEquals(wt->getTable(WavetableEngine::USER_TABLE).numFrames, 64, "the USER table as it was cut before");
                    expect(juce::String(WavetableEngine::tableLabels()[9]) == "USER");
                    expect(! wt->keyTablePending());
                }
            }
        }

        beginTest("From the controller: WAVE on a wavetable track is the key editor; PARAM has the pages");
        {
            using namespace helptest;
            Rig r;
            r.audio.setEngineType(0, EngineType::Wavetable, true);
            r.seq.getPattern().getTrack(0).engineType = EngineType::Wavetable;
            r.audio.prepare(SR, 64);
            auto* wt = dynamic_cast<WavetableEngine*> (r.audio.getEngine(0));
            expect(wt != nullptr);
            if (wt == nullptr)
                return;
            r.mgr.setActiveScreen(WAVE);
            auto* screen = dynamic_cast<WaveformScreen*> (r.mgr.getActiveScreen());
            expect(screen != nullptr);
            static const char* const knobs[8] = { "KY", "SM", "PO", "PL", "ZM", "SC", "AL", "TB" };
            for (int i = 0; i < 8; ++i)
                expectEquals(screen->getEncoderName(i), juce::String(knobs[i]));
            const auto offPicture = r.picture();
            expect(screen->getSubPage() == "K1 OFF");

            // SM, turned up to a sample: key 1 has it, and the table is KEYS.
            for (int i = 0; i <= smp.a; ++i)
                r.turn(1, 1);
            expectEquals((int) wt->getParameter(P::KeySampleBase), smp.a + 1);
            expectEquals((int) r.track(0).soundParams[P::KeySampleBase], smp.a + 1);
            expectEquals((int) wt->getParameter(P::TableSelect), (int) WavetableEngine::KEY_TABLE);
            expect(screen->getSubPage() == "K1 " + juce::String(SampleBank::get().entry(smp.a)->name));
            const auto withSample = r.picture();
            expect(withSample != offPicture, "the sample is drawn");

            // PO moves where the cycle is taken (the marker moves); PL where the key sits.
            const float before = wt->getParameter(P::KeyPositionBase);
            r.turn(2, 10);
            expectWithinAbsoluteError(wt->getParameter(P::KeyPositionBase), before + 10.0f / 250.0f, 1.0e-5f);
            expect(r.picture() != withSample);
            r.turn(3, 5);
            expectWithinAbsoluteError(wt->getParameter(P::KeyPlaceBase), 0.05f, 1.0e-5f);
            expectWithinAbsoluteError(r.track(0).soundParams[P::KeyPlaceBase], 0.05f, 1.0e-5f);

            // Pad 3 chooses key 3 (so does KY); it is OFF, and PO says so instead of doing nothing.
            r.pad(2);
            expectEquals(screen->getFocusKey(), 2);
            expect(screen->getSubPage() == "K3 OFF");
            const int said = TurnNotice::count();
            r.turn(2, 1);
            expectEquals(TurnNotice::count(), said + 1);
            expectWithinAbsoluteError(wt->getParameter(P::KeyPositionBase + 2), WavetableEngine::defaultKeyPosition(2), 1.0e-6f);
            r.turn(0, -1);
            expectEquals(screen->getFocusKey(), 1);
            r.turn(1, smp.b + 1);
            expectEquals((int) wt->getParameter(P::KeySampleBase + 1), smp.b + 1);
            std::array<Screen::PadColour, 16> colours;
            expect(screen->getPadColours(colours));
            expect(colours[1].r == 127 && colours[1].b == 127, "the chosen key is bright");
            expect(colours[0].r == 127 && colours[0].b == 40 && colours[2].r < 20, "keys in use lit, unused dim");

            // AL and TB.
            r.turn(6, 4);
            expectEquals((int) wt->getParameter(P::KeyAlgo), (int) Algo::Transport);
            r.turn(7, 1);
            expectEquals((int) wt->getParameter(P::TableSelect), (int) WavetableEngine::FIRST_MORE_TABLE);
            r.turn(7, -1);
            expectEquals((int) wt->getParameter(P::TableSelect), (int) WavetableEngine::KEY_TABLE);

            // The table that plays is the one with the two keys.
            std::vector<float> l(64), rr(64);
            r.audio.renderBlock(l.data(), rr.data(), 64);
            expectEquals(wt->getTable(WavetableEngine::KEY_TABLE).numFrames, 64);
            expectEquals(wt->getKeyCount(), 2);

            // REC's SM, while the table is KEYS, gives its sample to the chosen key.
            r.tap(BANK_DOWN);
            expectEquals(screen->getName(), juce::String("REC"));
            r.turn(6, 1);
            expectEquals((int) wt->getParameter(P::KeySampleBase + 1), smp.b + 2);
            expectEquals((int) wt->getParameter(P::TableSelect), (int) WavetableEngine::KEY_TABLE, "and the table stays KEYS");
            r.tap(BANK_DOWN);

            // On a sampler track the page is the sampler's again.
            r.audio.setEngineType(0, EngineType::Sampler, true);
            r.seq.getPattern().getTrack(0).engineType = EngineType::Sampler;
            expectEquals(screen->getEncoderName(0), juce::String("ST"));

            // PARAM: the four key pages are reached with the bank buttons, and a turn of a cell arrives.
            r.audio.setEngineType(0, EngineType::Wavetable, true);
            r.seq.getPattern().getTrack(0).engineType = EngineType::Wavetable;
            auto* engine = dynamic_cast<WavetableEngine*> (r.audio.getEngine(0));
            r.mgr.setActiveScreen(PARAM);
            auto find = [&](const juce::String& label)
            {
                for (int i = 0; i < 40; ++i)
                {
                    for (int e = 0; e < 8; ++e)
                        if (r.mgr.getActiveScreen()->getEncoderName(e) == label)
                            return e;
                    r.tap(BANK_DOWN);
                }
                return -1;
            };
            int knob = find("ALGO");
            expect(knob == 0, "the KEYS page");
            r.turn(knob, 5);
            expectEquals((int) engine->getParameter(P::KeyAlgo), (int) Algo::Vocoder);
            knob = find("SMP4");
            expect(knob == 3, "the K SMP page");
            r.turn(knob, 1);
            expectEquals((int) engine->getParameter(P::KeySampleBase + 3), 1);
            knob = find("POS8");
            expect(knob == 7, "the K POS page");
            r.turn(knob, -3);
            expectWithinAbsoluteError(engine->getParameter(P::KeyPositionBase + 7), WavetableEngine::defaultKeyPosition(7) - 0.03f, 1.0e-4f);
            knob = find("PLC2");
            expect(knob == 1, "the K PLC page");
            r.turn(knob, 2);
            expectWithinAbsoluteError(engine->getParameter(P::KeyPlaceBase + 1), WavetableEngine::defaultKeyPlace(1) + 0.02f, 1.0e-4f);
        }
    }
};
static WavetableKeyframeTests WavetableKeyframeTestsInstance;
