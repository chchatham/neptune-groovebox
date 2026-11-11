#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "../src/engine/WavetableEngine.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <vector>

// The wavetable engine's timed scan (SCAN page), the natural length of the USER table and the
// period-aligned frames.
//
// Most tests listen to the sound: the USER table is built from a "ramp" sample whose frame f holds
// the fundamental at level 1 - p and the second harmonic at level p (p = f / 63), so the share of
// the second harmonic in a 25 ms window of the output IS the scan position, whatever the level,
// the envelope or the note. Notes are A4 / A5 at 48 kHz so a window holds whole cycles.
class WavetableScanTests : public juce::UnitTest
{
public:
    WavetableScanTests() : UnitTest("Wavetable Scan") {}

    using P = WavetableEngine::Param;
    static constexpr double SR = 48000.0;
    static constexpr int WINDOW = 1200;                     // 25 ms
    static constexpr double WINDOW_SECONDS = WINDOW / SR;

    // ---- The MODE = OFF reference -------------------------------------------------------------
    // Three renders that only use controls the engine had before the SCAN page existed (a chord on
    // the saw table; a patch with every older position mover running; the USER table). The same
    // function was run against the engine before the scan was added: with NEPTUNE_WT_DUMP set to a
    // file path the raw floats are written there, so the comparison can be repeated bit for bit.
    static std::vector<float> referenceSample()
    {
        std::vector<float> s(16384);
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double t = (double) i / 44100.0, k = (double) i / (double) s.size();
            s[i] = (float) (0.6 * std::sin(6.283185307179586 * 196.0 * t) * (1.0 - k)
                          + 0.3 * std::sin(6.283185307179586 * 588.0 * t) * k
                          + 0.1 * std::sin(6.283185307179586 * 1372.0 * t + 3.0 * k));
        }
        return s;
    }

    static void renderInto(std::vector<float>& out, WavetableEngine& e, int numSamples, int block = 64)
    {
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        for (int done = 0; done < numSamples; done += block)
        {
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < block; ++i)
            {
                out.push_back(buf.getSample(0, i));
                out.push_back(buf.getSample(1, i));
            }
        }
    }

    static std::vector<float> renderOffReference(int bankIndex)
    {
        std::vector<float> out;
        {
            WavetableEngine e(4);
            e.prepareToPlay(48000.0, 64);
            e.setParameter(WavetableEngine::Position, 0.3f);
            e.noteOn(57, 0.9f, -1);
            e.noteOn(64, 0.6f, -1);
            renderInto(out, e, 9600);
            e.noteOff(57, -1);
            renderInto(out, e, 4800);
        }
        {
            WavetableEngine e(4);
            e.prepareToPlay(48000.0, 64);
            e.setParameter(WavetableEngine::TableSelect, 7.0f);
            e.setParameter(WavetableEngine::Position, 0.4f);
            e.setParameter(WavetableEngine::MorphSpeed, 2.0f);
            e.setParameter(WavetableEngine::MorphDepth, 0.6f);
            e.setParameter(WavetableEngine::EnvToPosition, 0.5f);
            e.setParameter(WavetableEngine::VelToPosition, 0.3f);
            e.setParameter(WavetableEngine::InterpMode, 3.0f);
            e.setParameter(WavetableEngine::UnisonSpread, 0.3f);
            e.setParameter(WavetableEngine::SubLevel, 0.4f);
            e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) WavetableVoice::LfoPosition);
            e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 0.4f);
            e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 3.0f);
            e.noteOn(50, 0.8f, -1);
            renderInto(out, e, 14400);
        }
        {
            WavetableEngine e(2);
            e.prepareToPlay(44100.0, 64);
            e.setParameter(WavetableEngine::UserSample, (float) bankIndex);
            e.setParameter(WavetableEngine::TableSelect, (float) WavetableEngine::USER_TABLE);
            e.setParameter(WavetableEngine::Position, 0.5f);
            e.setParameter(WavetableEngine::MorphSpeed, 1.0f);
            e.noteOn(48, 1.0f, -1);
            renderInto(out, e, 9600);
        }
        return out;
    }

    static int referenceBankIndex()
    {
        static int index = -1;
        if (index < 0)
        {
            auto s = referenceSample();
            index = SampleBank::get().addRecording(s.data(), nullptr, (int) s.size(), 44100.0, false);
        }
        return index;
    }

    static void dumpIfAsked(const std::vector<float>& ref)
    {
        const auto path = juce::SystemStats::getEnvironmentVariable("NEPTUNE_WT_DUMP", {});
        if (path.isEmpty())
            return;
        juce::File f(path);
        f.deleteFile();
        if (auto s = f.createOutputStream())
            s->write(ref.data(), ref.size() * sizeof(float));
    }

    // ---- Rig ----------------------------------------------------------------------------------
    static std::vector<float> rampSample()
    {
        std::vector<float> s(64 * 256);
        for (int f = 0; f < 64; ++f)
        {
            const double p = f / 63.0;
            for (int n = 0; n < 256; ++n)
                s[(size_t) (f * 256 + n)] = (float) ((1.0 - p) * std::sin(6.283185307179586 * n / 256.0)
                                                    + p * std::sin(6.283185307179586 * 2.0 * n / 256.0));
        }
        return s;
    }

    static void silentBlock(WavetableEngine& e, int n = 64)
    {
        juce::AudioBuffer<float> buf(1, n);
        juce::MidiBuffer midi;
        buf.clear();
        e.processBlock(buf, midi);
    }

    // An engine on the ramp table; the sample is given the rate that makes it naturalSeconds long.
    // Building an engine takes about a second without optimisation (its nine factory tables), so
    // the tests share one and put it back to its defaults: silent, every page cell at its default,
    // tempo 120, the default seed. Only one is in use at a time.
    WavetableEngine* makeEngine(double naturalSeconds = 2.0, double sampleRate = SR)
    {
        if (shared == nullptr)
            shared = std::make_unique<WavetableEngine>(4);
        auto* e = shared.get();
        e->allNotesOff();
        for (auto& page : e->getParamPages())
            for (auto& p : page.params)
                e->setParameter(p.id, p.defaultVal);
        e->setTempo(120.0f);
        e->setScanSeed(0x4E455054u);
        e->prepareToPlay(sampleRate, WINDOW);
        e->setParameter(P::AmpAttack, 0.001f);
        e->setParameter(P::AmpSustain, 1.0f);
        auto s = rampSample();
        e->buildUserTableFrom(s.data(), (int) s.size(), (double) s.size() / naturalSeconds);
        e->setParameter(P::TableSelect, (float) WavetableEngine::USER_TABLE);
        silentBlock(*e);            // the audio thread takes the table
        return e;
    }

    static double magnitude(const float* x, int n, double freq)
    {
        double c = 0.0, s = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double a = 6.283185307179586 * freq * i / SR;
            c += x[i] * std::cos(a);
            s += x[i] * std::sin(a);
        }
        return 2.0 * std::sqrt(c * c + s * s) / n;
    }

    static std::vector<float> renderWindow(WavetableEngine& e)
    {
        juce::AudioBuffer<float> buf(1, WINDOW);
        juce::MidiBuffer midi;
        buf.clear();
        e.processBlock(buf, midi);
        return std::vector<float>(buf.getReadPointer(0), buf.getReadPointer(0) + WINDOW);
    }

    // The scan position heard in each of `windows` consecutive 25 ms windows.
    static std::vector<float> track(WavetableEngine& e, int windows, double f0 = 440.0,
                                    const std::function<void(int)>& beforeWindow = {})
    {
        std::vector<float> out;
        for (int w = 0; w < windows; ++w)
        {
            if (beforeWindow)
                beforeWindow(w);
            auto x = renderWindow(e);
            const double a1 = magnitude(x.data(), WINDOW, f0), a2 = magnitude(x.data(), WINDOW, 2.0 * f0);
            out.push_back(a1 + a2 > 1.0e-9 ? (float) (a2 / (a1 + a2)) : -1.0f);
        }
        return out;
    }

    static double timeOf(int window) { return (window + 0.5) * WINDOW_SECONDS; }

    // Every window against expected(seconds at its middle); logs the worst one. The first window
    // is left out: the click of the note's attack is in it and blurs the measurement by about 0.01.
    void expectTrack(const std::vector<float>& t, const std::function<double(double)>& expected, float tol,
                     const juce::String& what)
    {
        float worst = 0.0f;
        int worstAt = 0;
        for (int w = 1; w < (int) t.size(); ++w)
        {
            const float d = std::abs(t[(size_t) w] - (float) expected(timeOf(w)));
            if (d > worst) { worst = d; worstAt = w; }
        }
        expect(worst <= tol, what + ": worst error " + juce::String(worst, 4) + " at " + juce::String(timeOf(worstAt), 3)
                             + " s (heard " + juce::String(t[(size_t) worstAt], 4) + ")");
    }

    std::unique_ptr<WavetableEngine> shared;

    // Scan positions of the sounding voices, lowest first.
    static std::vector<float> positions(WavetableEngine& e)
    {
        std::vector<float> out;
        for (int v = 0; v < 4; ++v)
            if (e.getVoice(v).isActive())
                out.push_back(e.getVoice(v).getEffectivePosition());
        std::sort(out.begin(), out.end());
        return out;
    }

    static double frac(double v) { return v - std::floor(v); }

    static double frameDistance(const Wavetable& wt)
    {
        double sum = 0.0;
        for (int f = 0; f + 1 < wt.numFrames; ++f)
        {
            double d = 0.0;
            for (int n = 0; n < Wavetable::kFrameSize; ++n)
            {
                const double v = wt.frames[0][(size_t) f + 1][(size_t) n] - wt.frames[0][(size_t) f][(size_t) n];
                d += v * v;
            }
            sum += std::sqrt(d / Wavetable::kFrameSize);
        }
        return sum / juce::jmax(1, wt.numFrames - 1);
    }

    // A pitched test tone (233.08 Hz: 189.2 samples per period at 44.1 kHz, so nothing lines up
    // with the pieces) that gets steadily brighter.
    static std::vector<float> pitchedSample(int length = 44100)
    {
        std::vector<float> s((size_t) length);
        for (int i = 0; i < length; ++i)
        {
            const double t = i / 44100.0, k = (double) i / length;
            double v = 0.0;
            for (int h = 1; h <= 6; ++h)
                v += (std::exp(-(h - 1) * (1.5 - 1.3 * k)) / h) * std::sin(6.283185307179586 * 233.08 * h * t + 0.7 * h);
            s[(size_t) i] = (float) (0.5 * v);
        }
        return s;
    }

    static std::vector<float> noiseSample(int length = 44100)
    {
        std::vector<float> s((size_t) length);
        uint32_t r = 12345u;
        for (auto& v : s)
        {
            r = r * 1664525u + 1013904223u;
            v = (float) (r >> 8) / 8388608.0f - 1.0f;
        }
        return s;
    }

    void runTest() override
    {
        beginTest("The measurement: the heard position is the POS knob (MODE = OFF)");
        {
            for (float pos : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                auto e = makeEngine();
                e->setParameter(P::Position, pos);
                e->noteOn(69, 0.9f, -1);
                auto t = track(*e, 8);
                expectTrack(t, [pos](double) { return (double) pos; }, 0.01f, "POS " + juce::String(pos));
            }
        }

        beginTest("RUN: once through in TIME, then the last frame is held");
        {
            auto e = makeEngine();
            e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
            e->setParameter(P::ScanTime, 1.0f);
            expectWithinAbsoluteError(e->getScanPassSeconds(), 1.0, 1.0e-9);
            e->noteOn(69, 0.9f, -1);
            auto t = track(*e, 60);
            expectTrack(t, [](double s) { return juce::jmin(1.0, s); }, 0.02f, "RUN 1 s");
            expectWithinAbsoluteError(t[19], 0.4875f, 0.02f, "half way at 0.5 s");
            for (int w = 41; w < 60; ++w)
                expectWithinAbsoluteError(t[(size_t) w], 1.0f, 0.005f, "held at the end");
        }

        beginTest("LOOP: wraps with a period of TIME");
        {
            auto e = makeEngine();
            e->setParameter(P::ScanMode, (float) WavetableScanMode::Loop);
            e->setParameter(P::ScanTime, 0.5f);
            e->noteOn(69, 0.9f, -1);
            auto t = track(*e, 80);
            expectTrack(t, [](double s) { return frac(s / 0.5); }, 0.03f, "LOOP 0.5 s");
            int wraps = 0;
            for (size_t w = 1; w < t.size(); ++w)
                if (t[w] < t[w - 1] - 0.5f)
                {
                    ++wraps;
                    expectEquals((int) w % 20, 0, "a wrap every 0.5 s");
                }
            expectEquals(wraps, 3);
        }

        beginTest("PING: to the end and back");
        {
            auto e = makeEngine();
            e->setParameter(P::ScanMode, (float) WavetableScanMode::Ping);
            e->setParameter(P::ScanTime, 0.5f);
            e->noteOn(69, 0.9f, -1);
            auto t = track(*e, 80);
            expectTrack(t, [](double s) { const double v = frac(s / 1.0) * 2.0; return v <= 1.0 ? v : 2.0 - v; }, 0.03f, "PING 0.5 s");
            expect(t[19] > 0.95f && t[20] > 0.95f, "turns around at the end after TIME");
            expect(t[39] < 0.05f && t[40] < 0.05f, "and at the start after 2 x TIME");
            expect(t[25] < t[21] && t[45] > t[41], "falling, then rising again");
        }

        beginTest("STEP: the set number of plateaus over the run, then hold");
        {
            for (int index = 0; index < WavetableEngine::NUM_SCAN_STEPS; ++index)
            {
                const int steps = WavetableEngine::scanStepCount(index);
                auto e = makeEngine();
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Step);
                e->setParameter(P::ScanTime, 3.2f);
                e->setParameter(P::ScanSteps, (float) index);
                e->noteOn(69, 0.9f, -1);
                auto t = track(*e, 160);           // 128 windows of run (a plateau is 128 / steps windows), 32 of hold
                expectTrack(t, [steps](double s)
                            {
                                const int k = juce::jmin(steps - 1, (int) std::floor(s / 3.2 * steps));
                                return (double) k / (steps - 1);
                            }, 0.006f, juce::String(steps) + " steps");
                std::set<int> levels;
                int changes = 0;
                for (size_t w = 1; w < t.size(); ++w)
                {
                    levels.insert((int) std::lround(t[w] * (float) (steps - 1)));
                    if (w > 1 && std::abs(t[w] - t[w - 1]) > 0.4f / (float) (steps - 1))     // (window 0 has the attack click)
                        ++changes;
                }
                expectEquals((int) levels.size(), steps, "plateaus");
                expectEquals(changes, steps - 1, "jumps between them");
            }
            expectEquals(WavetableEngine::scanStepCount(0), 4);
            expectEquals(WavetableEngine::scanStepCount(4), 64);
        }

        beginTest("STEP with the stepped interpolation: the sound only changes at the steps");
        {
            auto e = makeEngine();
            e->setParameter(P::ScanMode, (float) WavetableScanMode::Step);
            e->setParameter(P::InterpMode, 2.0f);
            e->setParameter(P::ScanTime, 0.8f);
            e->setParameter(P::ScanSteps, 0.0f);            // 4 plateaus of 0.2 s
            e->noteOn(69, 0.9f, -1);
            auto t = track(*e, 40);
            for (int w = 1; w < 40; ++w)
                expectWithinAbsoluteError(t[(size_t) w], (float) juce::jmin(3, w / 8) / 3.0f, 0.01f);
        }

        beginTest("RAND: holds a random frame for a step, reproducible with a seed");
        {
            auto run = [this](uint32_t seed, bool check)
            {
                auto e = makeEngine();
                if (seed != 0)
                    e->setScanSeed(seed);
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Rand);
                e->setParameter(P::ScanTime, 1.6f);
                e->setParameter(P::ScanSteps, 1.0f);        // 8 per pass: a new frame every 0.2 s
                e->noteOn(69, 0.9f, -1);
                auto t = track(*e, 128);
                if (check)
                {
                    std::set<int> frames;
                    for (int hold = 0; hold < 16; ++hold)
                    {
                        const float v = t[(size_t) hold * 8];
                        for (int w = 1; w < 8; ++w)
                            expectWithinAbsoluteError(t[(size_t) (hold * 8 + w)], v, 0.004f, "flat between the jumps");
                        expectWithinAbsoluteError(v * 63.0f, std::round(v * 63.0f), 0.2f, "a whole frame");
                        frames.insert((int) std::lround(v * 63.0f));
                    }
                    expect(frames.size() >= 8, "different frames: " + juce::String((int) frames.size()));
                    // The position the voice reports is the one heard.
                    expectWithinAbsoluteError(positions(*e).at(0), t.back(), 0.004f);
                }
                return t;
            };
            auto a = run(0, true), b = run(0, false), c = run(777u, false), d = run(777u, false);
            expect(a == b, "same seed (the default), same jumps");
            expect(c == d, "same seed, same jumps");
            expect(a != c, "another seed, other jumps");
        }

        beginTest("The time does not depend on the note; the pitch does");
        {
            std::vector<float> tracks[2];
            const int notes[2] = { 69, 81 };
            const double hz[2] = { 440.0, 880.0 };
            for (int i = 0; i < 2; ++i)
            {
                auto e = makeEngine();
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                e->setParameter(P::ScanTime, 1.0f);
                e->noteOn(notes[i], 0.9f, -1);
                // The first window also tells the pitch (position ~0: almost only the fundamental).
                auto x = renderWindow(*e);
                const double own = magnitude(x.data(), WINDOW, hz[i]);
                const double other = magnitude(x.data(), WINDOW, hz[1 - i]);
                expect(own > 0.1 && other < 0.1 * own, "note " + juce::String(notes[i]) + " sounds at its own pitch");
                tracks[i] = track(*e, 59, hz[i]);
                expectTrack(tracks[i], [](double s) { return juce::jmin(1.0, s + WINDOW_SECONDS); }, 0.02f,
                            "RUN 1 s at note " + juce::String(notes[i]));
            }
            float worst = 0.0f;
            for (size_t w = 0; w < tracks[0].size(); ++w)
                worst = juce::jmax(worst, std::abs(tracks[0][w] - tracks[1][w]));
            expect(worst < 0.01f, "the same run an octave apart, worst difference " + juce::String(worst, 4));
        }

        beginTest("The time does not depend on the sample rate");
        {
            for (double rate : { 44100.0, 96000.0 })
            {
                auto e = makeEngine(2.0, rate);
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                e->setParameter(P::ScanTime, 1.0f);
                e->noteOn(60, 0.9f, -1);
                juce::AudioBuffer<float> buf(1, (int) (rate / 100.0));
                juce::MidiBuffer midi;
                for (int block = 0; block < 50; ++block)        // 0.5 s
                {
                    buf.clear();
                    e->processBlock(buf, midi);
                }
                expectWithinAbsoluteError(positions(*e).at(0), 0.5f, 0.001f, juce::String(rate) + " Hz");
            }
        }

        beginTest("SYNC: LEN at the tempo, and a tempo change in the middle of a note");
        {
            for (float bpm : { 120.0f, 240.0f })
            {
                auto e = makeEngine();
                e->setTempo(bpm);
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                e->setParameter(P::ScanSync, 1.0f);
                e->setParameter(P::ScanLength, 4.0f);           // one bar = 4 beats
                e->setParameter(P::ScanTime, 7.0f);             // not used
                const double seconds = 240.0 / bpm;
                expectWithinAbsoluteError(e->getScanPassSeconds(), seconds, 1.0e-6);
                e->noteOn(69, 0.9f, -1);
                auto t = track(*e, 100);
                expectTrack(t, [seconds](double s) { return juce::jmin(1.0, s / seconds); }, 0.02f,
                            "1 bar at " + juce::String(bpm));
            }
            {
                auto e = makeEngine();
                e->setTempo(120.0f);
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                e->setParameter(P::ScanSync, 1.0f);
                e->setParameter(P::ScanLength, 4.0f);
                e->noteOn(69, 0.9f, -1);
                // 1 s at 120 (half the bar), then the other half at 240 takes 0.5 s.
                auto t = track(*e, 80, 440.0, [&e](int w) { if (w == 40) e->setTempo(240.0f); });
                expectTrack(t, [](double s) { return s < 1.0 ? s / 2.0 : juce::jmin(1.0, 0.5 + (s - 1.0)); }, 0.02f,
                            "tempo doubled after 1 s");
            }
            // Every length, in beats.
            auto e = makeEngine();
            e->setTempo(60.0f);
            e->setParameter(P::ScanSync, 1.0f);
            const double beats[] = { 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64 };
            for (int i = 0; i < WavetableEngine::NUM_SCAN_LENGTHS; ++i)
            {
                e->setParameter(P::ScanLength, (float) i);
                expectWithinAbsoluteError(e->getScanPassSeconds(), beats[i], 1.0e-6);
            }
        }

        beginTest("NAT: one pass lasts as long as the source sample; SPD scales it");
        {
            {
                auto e = makeEngine(2.0);
                expectWithinAbsoluteError(e->getUserSampleSeconds(), 2.0, 1.0e-9);
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                e->setParameter(P::ScanSync, 2.0f);
                e->setParameter(P::ScanTime, 9.0f);             // not used
                expectWithinAbsoluteError(e->getScanPassSeconds(), 2.0, 1.0e-9);
                e->noteOn(69, 0.9f, -1);
                auto t = track(*e, 100);
                expectTrack(t, [](double s) { return juce::jmin(1.0, s / 2.0); }, 0.02f, "natural 2 s");
            }
            struct Case { double natural; float speed; int note; double hz; };
            for (auto c : { Case { 2.0, 2.0f, 69, 440.0 }, Case { 2.0, 0.5f, 81, 880.0 }, Case { 0.8, 1.0f, 81, 880.0 },
                            Case { 0.8, 0.25f, 69, 440.0 }, Case { 2.0, 4.0f, 69, 440.0 } })
            {
                auto e = makeEngine(c.natural);
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                e->setParameter(P::ScanSync, 2.0f);
                e->setParameter(P::ScanSpeed, c.speed);
                const double seconds = c.natural / c.speed;
                expectWithinAbsoluteError(e->getScanPassSeconds(), seconds, 1.0e-6);
                e->noteOn(c.note, 0.9f, -1);
                auto t = track(*e, 100, c.hz);
                expectTrack(t, [seconds](double s) { return juce::jmin(1.0, s / seconds); }, 0.03f,
                            "natural " + juce::String(c.natural) + " s at x" + juce::String(c.speed));
            }
            {
                // From POS 0.5 only the second half of the recording is left: half the time, same speed.
                auto e = makeEngine(2.0);
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                e->setParameter(P::ScanSync, 2.0f);
                e->setParameter(P::Position, 0.5f);
                expectWithinAbsoluteError(e->getScanPassSeconds(), 1.0, 1.0e-6);
                e->noteOn(69, 0.9f, -1);
                auto t = track(*e, 60);
                expectTrack(t, [](double s) { return juce::jmin(1.0, 0.5 + s / 2.0); }, 0.02f, "natural from POS 0.5");
            }
            {
                // SPD also scales TIME and LEN; NAT on a factory table has no source and uses TIME.
                auto e = makeEngine(2.0);
                e->setParameter(P::ScanTime, 3.0f);
                e->setParameter(P::ScanSpeed, 2.0f);
                expectWithinAbsoluteError(e->getScanPassSeconds(), 1.5, 1.0e-6);
                e->setParameter(P::ScanSpeed, 1.0f);
                e->setParameter(P::ScanSync, 2.0f);
                e->setParameter(P::TableSelect, 0.0f);
                expectWithinAbsoluteError(e->getScanPassSeconds(), 3.0, 1.0e-6);
            }
            {
                // Through the sample bank: the entry's own length and rate.
                const int index = referenceBankIndex();
                if (index >= 0)
                {
                    WavetableEngine e(2);
                    e.prepareToPlay(SR, 64);
                    expectWithinAbsoluteError(e.getUserSampleSeconds(), 0.0, 1.0e-12, "nothing built yet");
                    e.setParameter(P::UserSample, (float) index);
                    silentBlock(e);
                    expectWithinAbsoluteError(e.getUserSampleSeconds(), 16384.0 / 44100.0, 1.0e-9);
                }
            }
        }

        beginTest("DIR and the start offset from POS");
        {
            struct Case { float pos; bool reverse; };
            for (auto c : { Case { 0.0f, true }, Case { 0.4f, false }, Case { 0.4f, true } })
            {
                auto e = makeEngine();
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                e->setParameter(P::ScanTime, 1.0f);
                e->setParameter(P::Position, c.pos);
                e->setParameter(P::ScanDirection, c.reverse ? 1.0f : 0.0f);
                e->noteOn(69, 0.9f, -1);
                auto t = track(*e, 60);
                const double pos = c.pos;
                const bool rev = c.reverse;
                expectTrack(t, [pos, rev](double s)
                            {
                                double run = juce::jmin(1.0, s);
                                if (rev) run = 1.0 - run;
                                return pos + run * (1.0 - pos);
                            }, 0.02f, juce::String("POS ") + juce::String(c.pos) + (c.reverse ? " REV" : " FWD"));
                expectWithinAbsoluteError(t.back(), c.reverse ? c.pos : 1.0f, 0.005f, "where it ends");
                expectEquals(e->getParameter(P::Position), c.pos, "the stored POS is not moved by the run");
            }
            // LOOP in reverse from POS 0.5: 1 -> 0.5, again and again.
            auto e = makeEngine();
            e->setParameter(P::ScanMode, (float) WavetableScanMode::Loop);
            e->setParameter(P::ScanTime, 0.5f);
            e->setParameter(P::Position, 0.5f);
            e->setParameter(P::ScanDirection, 1.0f);
            e->noteOn(69, 0.9f, -1);
            auto t = track(*e, 60);
            expectTrack(t, [](double s) { return 1.0 - 0.5 * frac(s / 0.5); }, 0.02f, "LOOP REV from 0.5");
        }

        beginTest("The run is the base; an LFO on POS, ENVP and VELP add to it and the sum is clamped");
        {
            {
                // Square LFO, 2 Hz, depth 0.2, restarted by the key: +-0.2 around the run.
                auto e = makeEngine();
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                e->setParameter(P::ScanTime, 2.0f);
                e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) WavetableVoice::LfoPosition);
                e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 0.2f);
                e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 2.0f);
                e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Shape), (float) LFO::Shape::Square);
                e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Trigger), 1.0f);     // KEY
                e->noteOn(69, 0.9f, -1);
                auto t = track(*e, 120);
                int above = 0, below = 0, checked = 0;
                for (int w = 0; w < 120; ++w)
                {
                    if (w % 10 == 0 || w % 10 == 9)
                        continue;                               // the LFO flips at these edges
                    const double run = juce::jmin(1.0, timeOf(w) / 2.0);
                    const float hi = (float) juce::jlimit(0.0, 1.0, run + 0.2), lo = (float) juce::jlimit(0.0, 1.0, run - 0.2);
                    const float v = t[(size_t) w];
                    const bool isHi = std::abs(v - hi) < 0.02f, isLo = std::abs(v - lo) < 0.02f;
                    expect(isHi || isLo, "run +- 0.2 at " + juce::String(timeOf(w), 3) + " s, heard " + juce::String(v, 3));
                    if (hi - lo > 0.1f) { above += isHi ? 1 : 0; below += isLo ? 1 : 0; }
                    ++checked;
                }
                expect(above > 30 && below > 30, "both halves of the LFO are heard");
                // Held at the end: the sum is clamped at 1, so only the lower half shows.
                expectWithinAbsoluteError(juce::jmax(t[102], t[112]), 1.0f, 0.005f);
                expectWithinAbsoluteError(juce::jmin(t[102], t[112]), 0.8f, 0.02f);
            }
            {
                // Velocity: VELP 0.4 at velocity 0.5 = +0.2.
                auto e = makeEngine();
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                e->setParameter(P::ScanTime, 1.0f);
                e->setParameter(P::VelToPosition, 0.4f);
                e->noteOn(69, 0.5f, -1);
                auto t = track(*e, 50);
                expectTrack(t, [](double s) { return juce::jmin(1.0, juce::jmin(1.0, s) + 0.2); }, 0.02f, "run + VELP");
            }
            {
                // MSPD under a run is clamped, not wrapped: the held last frame stays the last frame.
                auto e = makeEngine();
                e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                e->setParameter(P::ScanTime, 0.1f);
                e->setParameter(P::MorphSpeed, 4.0f);
                e->setParameter(P::MorphDepth, 0.4f);           // +-0.2
                e->noteOn(69, 0.9f, -1);
                auto t = track(*e, 44);
                float low = 1.0f, high = 0.0f;
                for (int w = 4; w < 44; ++w) { low = juce::jmin(low, t[(size_t) w]); high = juce::jmax(high, t[(size_t) w]); }
                expectWithinAbsoluteError(high, 1.0f, 0.005f);
                expect(low > 0.78f && low < 0.9f, "dips to about 0.8, never to the first frame: " + juce::String(low, 3));
            }
        }

        beginTest("Two notes started at different times run independently; a new note-on restarts its run");
        {
            auto e = makeEngine();
            e->setParameter(P::ScanMode, (float) WavetableScanMode::Run);
            e->setParameter(P::ScanTime, 1.0f);
            e->noteOn(60, 0.9f, -1);
            for (int w = 0; w < 10; ++w) renderWindow(*e);          // 0.25 s
            e->noteOn(67, 0.9f, -1);
            for (int w = 0; w < 10; ++w) renderWindow(*e);          // 0.25 s more
            auto at = positions(*e);
            expectEquals((int) at.size(), 2);
            expectWithinAbsoluteError(at.back(), 0.5f, 0.001f, "the first note is half way");
            expectWithinAbsoluteError(at.front(), 0.25f, 0.001f, "the second a quarter");
            for (int w = 0; w < 24; ++w) renderWindow(*e);          // 0.6 s more: 1.1 s and 0.85 s
            at = positions(*e);
            expectWithinAbsoluteError(at.back(), 1.0f, 0.001f, "the first has arrived");
            expectWithinAbsoluteError(at.front(), 0.85f, 0.001f, "the second has not");

            // Playing the first note again restarts only its own run.
            e->noteOff(60, -1);
            e->noteOn(60, 0.9f, -1);
            for (int w = 0; w < 4; ++w) renderWindow(*e);           // 0.1 s
            int restarted = 0, arrived = 0;
            for (int v = 0; v < 4; ++v)
            {
                if (! e->getVoice(v).isActive()) continue;
                const float p = e->getVoice(v).getEffectivePosition();
                if (std::abs(p - 0.1f) < 0.001f) ++restarted;
                if (std::abs(p - 0.95f) < 0.001f) ++arrived;
            }
            expectEquals(restarted, 1, "the new note is 0.1 s into its run");
            expect(arrived >= 1, "the other note carries on");
        }

        beginTest("MODE = OFF: the sound is what it was before the scan existed");
        {
            const int bank = referenceBankIndex();
            auto ref = renderOffReference(bank);
            dumpIfAsked(ref);
            // Values rendered by the engine before the SCAN page was added (the whole render was
            // also compared bit for bit when the page was written).
            const std::pair<int, float> before[] = { { 1000, -0.560398519f }, { 9001, 0.363033533f }, { 20000, 0.309159458f },
                                                     { 30001, -0.114391588f }, { 45000, 0.092445135f } };
            for (auto& b : before)
                expectWithinAbsoluteError(ref[(size_t) b.first], b.second, 2.0e-6f, "sample " + juce::String(b.first));

            // With MODE = OFF the other SCAN cells change nothing, bit for bit.
            auto render = [this](bool touched)
            {
                auto e = makeEngine();
                e->setParameter(P::Position, 0.3f);
                e->setParameter(P::MorphSpeed, 1.5f);
                e->setParameter(P::EnvToPosition, 0.3f);
                if (touched)
                {
                    e->setParameter(P::ScanTime, 0.07f);
                    e->setParameter(P::ScanSync, 1.0f);
                    e->setParameter(P::ScanLength, 0.0f);
                    e->setParameter(P::ScanSteps, 0.0f);
                    e->setParameter(P::ScanDirection, 1.0f);
                    e->setParameter(P::ScanSpeed, 4.0f);
                    e->setScanSeed(99u);
                }
                e->noteOn(57, 0.8f, -1);
                e->noteOn(64, 0.6f, -1);
                std::vector<float> out;
                renderInto(out, *e, 9600);
                return out;
            };
            expect(render(false) == render(true), "SCAN cells are inert while MODE is OFF");

            // And every mode gives finite sound in every interpolation mode.
            for (int mode = 1; mode < (int) WavetableScanMode::Count; ++mode)
                for (int interp = 0; interp < 4; ++interp)
                {
                    auto e = makeEngine();
                    e->setParameter(P::ScanMode, (float) mode);
                    e->setParameter(P::ScanTime, 0.05f);
                    e->setParameter(P::InterpMode, (float) interp);
                    e->noteOn(60, 1.0f, -1);
                    std::vector<float> out;
                    renderInto(out, *e, 9600);
                    double energy = 0.0;
                    bool finite = true;
                    for (float v : out) { finite = finite && std::isfinite(v); energy += v * v; }
                    expect(finite && energy > 1.0, "mode " + juce::String(mode) + " interp " + juce::String(interp));
                }
        }

        beginTest("Period-aligned frames: smoother than equal pieces on a pitched sample, same as them on noise");
        {
            auto tone = pitchedSample();
            const auto pieces = WavetableFactory::createFromSamples(tone.data(), (int) tone.size(), 64);
            for (int frames : { 64, 128, 256 })
            {
                int pitched = 0;
                const auto aligned = WavetableFactory::createFromSamplesEx(tone.data(), (int) tone.size(), 44100.0, frames, true, &pitched);
                const auto plain = WavetableFactory::createFromSamplesEx(tone.data(), (int) tone.size(), 44100.0, frames, false);
                expectEquals(aligned.numFrames, frames);
                expectEquals(pitched, frames, "every piece of the tone is pitched");
                const double dAligned = frameDistance(aligned), dPlain = frameDistance(plain);
                logMessage(juce::String(frames) + " frames: frame-to-frame distance " + juce::String(dPlain, 4)
                           + " (equal pieces) -> " + juce::String(dAligned, 4) + " (aligned)");
                expect(dAligned < 0.1 * dPlain, "at least ten times closer neighbours");
                expect(dAligned < 0.05, "neighbouring frames nearly the same waveform");

                // Each aligned frame is ONE cycle: its fundamental (harmonic 1) is the tone's.
                for (int f = 0; f < frames; f += frames / 8)
                {
                    double c1 = 0.0, s1 = 0.0, total = 0.0;
                    const auto& fr = aligned.frames[0][(size_t) f];
                    for (int n = 0; n < 256; ++n)
                    {
                        c1 += fr[(size_t) n] * std::cos(6.283185307179586 * n / 256.0);
                        s1 += fr[(size_t) n] * std::sin(6.283185307179586 * n / 256.0);
                        total += fr[(size_t) n] * fr[(size_t) n];
                    }
                    const double share = (c1 * c1 + s1 * s1) * 2.0 / 256.0 / total;
                    expect(share > 0.45, "frame " + juce::String(f) + ": fundamental share " + juce::String(share, 3));
                }
            }
            {
                // The classic cut at 64 frames is unchanged by the new builder (level 0 bit for bit;
                // the mip levels only by rounding).
                const auto plain = WavetableFactory::createFromSamplesEx(tone.data(), (int) tone.size(), 44100.0, 64, false);
                expect(plain.frames[0] == pieces.frames[0]);
                expectEquals(plain.getMipLevelCount(), pieces.getMipLevelCount());
                float worst = 0.0f;
                for (int level = 1; level < pieces.getMipLevelCount(); ++level)
                    for (int f = 0; f < 64; ++f)
                        for (int n = 0; n < 256; ++n)
                            worst = juce::jmax(worst, std::abs(plain.frames[(size_t) level][(size_t) f][(size_t) n]
                                                               - pieces.frames[(size_t) level][(size_t) f][(size_t) n]));
                expect(worst < 1.0e-3f, "fast mip maps = the original ones, worst difference " + juce::String(worst, 6));
            }
            {
                // The period itself.
                WavetableFactory::PeriodFinder finder(tone.data(), (int) tone.size(), 44100.0);
                float clarity = 0.0f;
                const float period = finder.periodAt(22050.0, &clarity);
                expectWithinAbsoluteError(period, 44100.0f / 233.08f, 0.1f, "period of the tone");
                expect(clarity > 0.95f);
            }
            {
                // Noise and silence have no period: the equal pieces are used.
                auto noise = noiseSample();
                int pitched = -1;
                const auto aligned = WavetableFactory::createFromSamplesEx(noise.data(), (int) noise.size(), 44100.0, 64, true, &pitched);
                const auto old = WavetableFactory::createFromSamples(noise.data(), (int) noise.size(), 64);
                expectEquals(pitched, 0, "noise is not pitched");
                expect(aligned.frames[0] == old.frames[0], "noise frames are the equal pieces");

                std::vector<float> silence(20000, 0.0f);
                WavetableFactory::createFromSamplesEx(silence.data(), (int) silence.size(), 44100.0, 64, true, &pitched);
                expectEquals(pitched, 0, "silence is not pitched");

                // Half tone, half noise: only the tone's half is aligned.
                std::vector<float> mixed(tone.begin(), tone.begin() + 22050);
                mixed.insert(mixed.end(), noise.begin(), noise.begin() + 22050);
                WavetableFactory::createFromSamplesEx(mixed.data(), (int) mixed.size(), 44100.0, 64, true, &pitched);
                expect(pitched >= 29 && pitched <= 33, "about half the frames: " + juce::String(pitched));

                // A sample too short to hold a period search still gives a table.
                int shortPitched = -1;
                const auto tiny = WavetableFactory::createFromSamplesEx(tone.data(), 100, 44100.0, 256, true, &shortPitched);
                expectEquals(tiny.numFrames, 256);
            }
        }

        beginTest("FRMS: the USER table is rebuilt on the message thread and swapped in by the audio thread");
        {
            auto tone = pitchedSample();
            const int bank = SampleBank::get().addRecording(tone.data(), nullptr, (int) tone.size(), 44100.0, false);
            if (bank >= 0)
            {
                WavetableEngine e(2);
                e.prepareToPlay(SR, WINDOW);
                e.setParameter(P::AmpSustain, 1.0f);
                e.setParameter(P::UserFrames, 5.0f);
                expect(! e.userTablePending(), "no sample chosen: nothing to rebuild");
                e.setParameter(P::UserFrames, 0.0f);

                e.setParameter(P::UserSample, (float) bank);
                e.setParameter(P::TableSelect, (float) WavetableEngine::USER_TABLE);
                e.setParameter(P::Position, 0.5f);
                expect(e.userTablePending());
                silentBlock(e);
                expect(! e.userTablePending());
                expectEquals(e.getTable(WavetableEngine::USER_TABLE).numFrames, 64);
                const auto classic = e.getTable(WavetableEngine::USER_TABLE).frames;
                const auto direct = WavetableFactory::createFromSamples(tone.data(), (int) tone.size(), 64);
                expect(classic == direct.frames, "FRMS 64 (the default) is the table as it always was");

                // Equal pieces hold 3.6 cycles of the tone, so the note does not sound at its pitch.
                e.noteOn(69, 0.9f, -1);
                renderWindow(e);
                auto x = renderWindow(e);
                double total = 0.0;
                for (float v : x) total += v * v;
                const double m = magnitude(x.data(), WINDOW, 440.0);
                const double shareClassic = m * m * 0.5 / (total / WINDOW);

                e.setParameter(P::UserFrames, 0.0f);
                expect(! e.userTablePending(), "same setting: no rebuild");

                const int counts[] = { 64, 128, 256, 64, 128, 256 };
                for (int option : { 1, 2, 3, 4, 5 })
                {
                    const int framesBefore = e.getTable(WavetableEngine::USER_TABLE).numFrames;
                    e.setParameter(P::UserFrames, (float) option);
                    expect(e.userTablePending(), "rebuilt, waiting for the audio thread");
                    expectEquals(e.getTable(WavetableEngine::USER_TABLE).numFrames, framesBefore,
                                 "the table in use is untouched until the audio thread swaps");
                    auto y = renderWindow(e);                      // the held note plays across the swap
                    expect(! e.userTablePending());
                    expectEquals(e.getTable(WavetableEngine::USER_TABLE).numFrames, counts[option]);
                    expectEquals(e.getTable(WavetableEngine::USER_TABLE).getMipLevelCount(), 8);
                    expectWithinAbsoluteError(e.getUserSampleSeconds(), 1.0, 1.0e-9);
                    bool finite = true;
                    for (float v : y) finite = finite && std::isfinite(v);
                    expect(finite);
                }

                // Aligned frames are single cycles: now the note sounds at its own pitch.
                renderWindow(e);
                x = renderWindow(e);
                total = 0.0;
                for (float v : x) total += v * v;
                const double m2 = magnitude(x.data(), WINDOW, 440.0);
                const double shareAligned = m2 * m2 * 0.5 / (total / WINDOW);
                logMessage("share of the sound at the note's fundamental: equal pieces " + juce::String(shareClassic, 3)
                           + ", aligned " + juce::String(shareAligned, 3));
                expect(shareClassic < 0.1 && shareAligned > 0.4, "aligned frames follow the key");

                // A scan across the aligned table is steadier than across the equal pieces: the
                // level of the note's fundamental does not flutter as frames go by.
                auto flutter = [&](int option)
                {
                    WavetableEngine s(1);
                    s.prepareToPlay(SR, WINDOW);
                    s.setParameter(P::AmpAttack, 0.001f);
                    s.setParameter(P::AmpSustain, 1.0f);
                    s.setParameter(P::UserFrames, (float) option);
                    s.setParameter(P::UserSample, (float) bank);
                    s.setParameter(P::TableSelect, (float) WavetableEngine::USER_TABLE);
                    s.setParameter(P::ScanMode, (float) WavetableScanMode::Run);
                    s.setParameter(P::ScanSync, 2.0f);              // natural length: 1 s
                    silentBlock(s);
                    s.noteOn(69, 0.9f, -1);
                    renderWindow(s);
                    double sum = 0.0, last = 0.0;
                    for (int w = 0; w < 36; ++w)
                    {
                        auto win = renderWindow(s);
                        double rms = 0.0;
                        for (float v : win) rms += v * v;
                        rms = std::sqrt(rms / WINDOW);
                        if (w > 0) sum += std::abs(rms - last);
                        last = rms;
                    }
                    return sum / 35.0;
                };
                const double fClassic = flutter(0), fAligned = flutter(3);
                logMessage("level flutter during a natural-length RUN: equal pieces " + juce::String(fClassic, 4)
                           + ", aligned " + juce::String(fAligned, 4));
                expect(fAligned < 0.5 * fClassic, "the scan through aligned frames is smoother");
            }
        }

        beginTest("Pages: SCAN after WAVE, 8 working cells each, ids round-trip");
        {
            WavetableEngine e;
            const auto pages = e.getParamPages();
            expect(pages.size() >= 4);
            expect(pages[0].name == "WAVE" && pages[1].name == "SCAN" && pages[2].name == "OSC" && pages[3].name == "AMP");
            const char* labels[8] = { "MODE", "TIME", "SYNC", "LEN", "STEP", "DIR", "SPD", "FRMS" };
            const int ids[8] = { 25, 26, 27, 28, 29, 30, 31, 32 };
            for (int c = 0; c < 8; ++c)
            {
                expect(juce::String(pages[1].params[(size_t) c].label) == labels[c]);
                expectEquals(pages[1].params[(size_t) c].id, ids[c]);
            }
            expectEquals((int) P::ScanMode, 25);
            expectEquals((int) P::UserFrames, 32);

            std::set<int> seen;
            for (auto& page : pages)
            {
                expect(page.name.length() <= 5);
                for (auto& p : page.params)
                {
                    expect(p.isValid(), page.name + ": an empty cell");
                    const juce::String label(p.label);
                    expect(label.isNotEmpty() && label.length() <= 4 && label == label.toUpperCase(), page.name + " / " + label);
                    expect(seen.insert(p.id).second, "id " + juce::String(p.id) + " is on two cells");
                    expect(EngineParamRange::contains(p.id));
                    expect(p.maxVal > p.minVal);
                    expectWithinAbsoluteError(e.getParameter(p.id), p.defaultVal, 1.0e-6f, label + " default");
                    if (p.kind == ParamKind::Choice)
                    {
                        expect(p.choices != nullptr && p.numChoices == (int) std::lround(p.maxVal - p.minVal) + 1, label + " choices");
                        for (int i = 0; i < p.numChoices; ++i)
                            expect(juce::String(p.choices[i]).length() <= 5 && juce::String(p.choices[i]).isNotEmpty());
                    }
                    if (p.kind == ParamKind::Log)
                        expect(p.minVal > 0.0f);
                    for (float n : { 1.0f, 0.0f, 0.37f })
                    {
                        float v = p.fromNorm(n);
                        if (p.isDiscrete()) v = std::round(v);
                        e.setParameter(p.id, v);
                        expectWithinAbsoluteError(e.getParameter(p.id), v, 1.0e-5f * juce::jmax(1.0f, std::abs(v)), label + " round trip");
                    }
                    e.setParameter(p.id, p.defaultVal);
                }
            }
            expectEquals(e.getParameter(P::ScanMode), 0.0f, "MODE is OFF by default");
            expect(e.getParameterName(P::ScanMode) == "Scan Mode" && e.getParameterName(P::UserFrames) == "User Frames");
            expect(e.getParameterName(P::Position) == "Position");

            // What the cells show.
            auto cell = [&pages](int c, float v) { return pages[1].params[(size_t) c].format(v); };
            expect(cell(0, 0) == "OFF" && cell(0, 1) == "RUN" && cell(0, 2) == "LOOP" && cell(0, 3) == "PING"
                   && cell(0, 4) == "STEP" && cell(0, 5) == "RAND");
            expect(cell(1, 0.05f) == "50m" && cell(1, 60.0f) == "60.0");
            expect(cell(2, 0) == "OFF" && cell(2, 1) == "ON" && cell(2, 2) == "NAT");
            expect(cell(3, 0) == "1/16" && cell(3, 4) == "1BAR" && cell(3, 8) == "16BR");
            expect(cell(4, 0) == "4" && cell(4, 4) == "64");
            expect(cell(5, 0) == "FWD" && cell(5, 1) == "REV");
            expect(cell(6, 1.0f) == "1.00" && cell(6, 0.25f) == "0.25" && cell(6, 4.0f) == "4.00");
            expect(cell(7, 0) == "64" && cell(7, 5) == "A256");
        }
        shared.reset();
    }
};
static WavetableScanTests WavetableScanTestsInstance;
