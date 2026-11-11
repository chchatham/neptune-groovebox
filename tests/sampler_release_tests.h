#pragma once
#include "../src/engine/AudioEngine.h"
#include "../src/engine/SamplerEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

// The sampler as a fresh track gives it (default sample, default mode), played from the keys with a
// long release: no clicks, and REL is heard. Everything is rendered the way the app renders it:
// through AudioEngine::renderBlock at 48 kHz in 64-sample blocks (track mixer, master EQ,
// compressor and limiter included), notes given to the engine as main.cpp gives key presses.
class SamplerReleaseTests : public juce::UnitTest
{
public:
    SamplerReleaseTests() : UnitTest("Sampler Release") {}

    using S = SamplerEngine;
    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;

    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        SamplerEngine* sampler = nullptr;
        std::vector<float> out;         // left channel of everything rendered
        std::vector<float> l = std::vector<float>((size_t) BLOCK), r = std::vector<float>((size_t) BLOCK);

        Rig()
        {
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::Sampler));
            audio.prepare(SR, BLOCK);
            sampler = dynamic_cast<SamplerEngine*>(audio.getEngine(0));
        }

        void set(int id, float value) { audio.setTrackParameter(0, id, value); }
        void on(int note, float velocity = 0.8f) { sampler->noteOn(note, velocity, -1); }
        void off(int note) { sampler->noteOff(note, -1); }

        void render(double seconds)
        {
            const int blocks = std::max(1, (int) std::lround(seconds * SR / BLOCK));
            for (int b = 0; b < blocks; ++b)
            {
                audio.renderBlock(l.data(), r.data(), BLOCK);
                out.insert(out.end(), l.begin(), l.end());
            }
        }
    };

    struct Clicks
    {
        int count = 0;
        float worst = 0.0f;         // the largest such jump
        int worstAt = -1;
    };

    // A click: a sample-to-sample jump that the signal around it does not explain - at least
    // `floor` in size and more than `ratio` times the largest step among the 32 samples before it
    // or among the 32 after it (a voice cut off, a hard stop, a wrap that does not join up). Steady
    // material, however bright, has steps of similar size on both sides.
    static Clicks findClicks(const std::vector<float>& x, float floor = 0.004f, float ratio = 5.0f)
    {
        Clicks c;
        const int n = (int) x.size(), w = 32;
        int last = -1000;
        for (int i = w + 1; i < n - w; ++i)
        {
            const float d = std::abs(x[(size_t) i] - x[(size_t) i - 1]);
            if (d < floor)
                continue;
            float before = 0.0f, after = 0.0f;
            for (int k = 1; k <= w; ++k)
            {
                before = std::max(before, std::abs(x[(size_t) (i - k)] - x[(size_t) (i - k - 1)]));
                after = std::max(after, std::abs(x[(size_t) (i + k)] - x[(size_t) (i + k - 1)]));
            }
            if (d > ratio * before || d > ratio * after)
            {
                if (i - last > 8)
                    ++c.count;
                last = i;
                if (d > c.worst) { c.worst = d; c.worstAt = i; }
            }
        }
        return c;
    }

    static float peak(const std::vector<float>& x, int from = 0, int to = -1)
    {
        float p = 0.0f;
        if (to < 0) to = (int) x.size();
        for (int i = std::max(0, from); i < std::min(to, (int) x.size()); ++i)
            p = std::max(p, std::abs(x[(size_t) i]));
        return p;
    }

    // Seconds from `from` until the sound has stayed below `level` for good.
    static double tailSeconds(const std::vector<float>& x, int from, float level)
    {
        int lastLoud = from;
        for (int i = from; i < (int) x.size(); ++i)
            if (std::abs(x[(size_t) i]) > level)
                lastLoud = i;
        return (double) (lastLoud - from) / SR;
    }

    bool allFinite(const std::vector<float>& x)
    {
        for (float v : x)
            if (! std::isfinite(v))
                return false;
        return true;
    }

    // Keys played one after the other, each held for `held` seconds, a new one every `every`.
    static void playPhrase(Rig& rig, int notes, double every, double held, float velocity = 0.8f)
    {
        static const int scale[10] = { 48, 51, 53, 55, 58, 60, 63, 65, 67, 70 };
        for (int i = 0; i < notes; ++i)
        {
            const int note = scale[(i * 3) % 10];
            rig.on(note, velocity);
            rig.render(held);
            rig.off(note);
            rig.render(every - held);
        }
    }

    // How long a C4 held for 0.3 s rings after the key is let go (to -60 dB of the note's peak).
    double releaseTail(float rel, int note = 60)
    {
        Rig rig;
        rig.set(S::Release, rel);
        rig.on(note);
        rig.render(0.3);
        const int offAt = (int) rig.out.size();
        const float heldPeak = peak(rig.out);
        rig.off(note);
        rig.render(8.0);
        expect(allFinite(rig.out));
        return tailSeconds(rig.out, offAt, heldPeak * 0.001f);
    }

    void runTest() override
    {
        beginTest("Default sound: REL lengthens the tail of a note");
        {
            const double shortTail = releaseTail(0.05f);
            const double mediumTail = releaseTail(1.0f);
            const double longTail = releaseTail(4.0f);
            logMessage("tail after key-up, REL 0.05 / 1 / 4 s: " + juce::String(shortTail, 3) + " / "
                       + juce::String(mediumTail, 3) + " / " + juce::String(longTail, 3) + " s");
            expect(shortTail < 0.1, "a short release stops the note at once");
            expect(mediumTail > 0.8 && mediumTail < 1.1, "REL 1 s rings for about a second");
            expect(longTail > 3.2 && longTail < 4.2, "REL 4 s rings for about four seconds");

            // The same across the keyboard: a high key is not cut short by the end of the sample.
            for (int note : { 36, 48, 72, 84 })
            {
                const double tail = releaseTail(4.0f, note);
                expect(tail > 3.2 && tail < 4.2, "REL 4 s on note " + juce::String(note) + ": " + juce::String(tail, 3) + " s");
            }
        }

        beginTest("Default sound: a held note sustains, without clicks where the sample repeats");
        {
            Rig rig;
            rig.on(60);
            rig.render(9.0);
            const float early = peak(rig.out, (int) (0.5 * SR), (int) (1.5 * SR));
            const float late = peak(rig.out, (int) (7.5 * SR), (int) (8.5 * SR));
            const auto c = findClicks(rig.out);
            logMessage("held 9 s: level early " + juce::String(early, 4) + ", late " + juce::String(late, 4)
                       + ", clicks " + juce::String(c.count) + " (worst jump " + juce::String(c.worst, 4) + ")");
            expect(early > 0.05f);
            expect(late > 0.5f * early, "the note is still sounding after 8 s");
            expectEquals(c.count, 0);
        }

        beginTest("Default sound, long release: a phrase of overlapping notes has no clicks");
        {
            for (float rel : { 0.05f, 2.0f, 5.0f })
            {
                Rig rig;
                rig.set(S::Release, rel);
                playPhrase(rig, 24, 0.2, 0.1);
                rig.render(6.0);
                const auto c = findClicks(rig.out);
                logMessage("REL " + juce::String(rel, 2) + " s, 24 notes: clicks " + juce::String(c.count)
                           + ", worst jump " + juce::String(c.worst, 4) + " at " + juce::String(c.worstAt)
                           + ", peak " + juce::String(peak(rig.out), 3));
                expect(allFinite(rig.out));
                expectEquals(c.count, 0, "REL " + juce::String(rel, 2));
                expect(peak(rig.out) <= 1.0f, "the master stays within full scale");
                expect(peak(rig.out, (int) rig.out.size() - (int) (0.25 * SR)) < 1.0e-4f, "and it ends in silence");
            }
        }

        beginTest("Long release: more notes than voices - the note that gives way is faded, not cut");
        {
            for (int vox : { 1, 2, 8 })
            {
                Rig rig;
                rig.set(S::Release, 5.0f);
                rig.set(S::Voices, (float) vox);
                playPhrase(rig, 30, 0.09, 0.05, 1.0f);
                rig.render(6.0);
                const auto c = findClicks(rig.out);
                logMessage("VOX " + juce::String(vox) + ", 30 fast notes, REL 5 s: clicks " + juce::String(c.count)
                           + ", worst jump " + juce::String(c.worst, 4));
                expectEquals(c.count, 0, "VOX " + juce::String(vox));
            }

            // The same key struck again and again while its last strike still rings.
            Rig rig;
            rig.set(S::Release, 5.0f);
            for (int i = 0; i < 20; ++i)
            {
                rig.on(60, 1.0f);
                rig.render(0.06);
                rig.off(60);
                rig.render(0.05);
            }
            rig.render(6.0);
            const auto c = findClicks(rig.out);
            logMessage("one key 20 times, REL 5 s: clicks " + juce::String(c.count) + ", worst jump " + juce::String(c.worst, 4));
            expectEquals(c.count, 0);
        }

        beginTest("VOX still limits how many notes sound");
        {
            // Middle C held, then a second key: how much of middle C's fundamental (261.5 Hz) is
            // left in the half second that starts 0.5 s after the second key.
            auto fundamental = [](int vox, bool second)
            {
                Rig rig;
                rig.set(S::Voices, (float) vox);
                rig.on(60);
                rig.render(0.2);
                if (second) rig.on(66);
                rig.render(1.0);
                double re = 0.0, im = 0.0;
                const int from = (int) (0.7 * SR), to = (int) (1.2 * SR);
                for (int i = from; i < to; ++i)
                {
                    const double ph = 2.0 * 3.141592653589793 * 261.5 * (double) i / SR;
                    re += rig.out[(size_t) i] * std::cos(ph);
                    im += rig.out[(size_t) i] * std::sin(ph);
                }
                return 2.0 * std::sqrt(re * re + im * im) / (double) (to - from);
            };
            const double alone = fundamental(8, false), mono = fundamental(1, true), poly = fundamental(8, true);
            logMessage("middle C's fundamental: alone " + juce::String(alone, 4) + ", after a second key with VOX 1 "
                       + juce::String(mono, 5) + ", with VOX 8 " + juce::String(poly, 4));
            expect(alone > 0.01);
            expect(mono < 0.05 * alone, "VOX 1: the first note has gone");
            expect(poly > 0.5 * alone, "VOX 8: both sound");
        }

        beginTest("A sample that stops before its sound has died away ends without a click");
        {
            // The factory kick cut off in the middle (END 20 %), and one slice of the factory beat:
            // the ways a note used to end in a step.
            for (int variant = 0; variant < 3; ++variant)
            {
                Rig rig;
                const int kick = SampleBank::get().indexOf("KICK"), beat = SampleBank::get().indexOf("BEAT");
                rig.set(S::SampleSelect, (float) (variant == 0 ? kick : beat));
                rig.set(S::Mode, (float) (variant == 2 ? S::OneShot : S::Forward));
                if (variant == 0)
                    rig.set(S::EndPoint, 0.2f);
                rig.set(S::Release, 5.0f);
                playPhrase(rig, 12, 0.3, 0.1, 1.0f);
                rig.render(3.0);
                // The strike itself is not what is measured here (the drum's own attack, which is
                // in the sample): the 15 ms after every key press are left out - the master
                // limiter's look-ahead of about 3 ms, then the first 12 ms of the note.
                const int period = (int) (0.3 * SR), strike = (int) (0.015 * SR);
                Clicks c;
                for (int n = 0; n < 12; ++n)
                {
                    const std::vector<float> rest(rig.out.begin() + n * period + strike,
                                                  n < 11 ? rig.out.begin() + (n + 1) * period : rig.out.end());
                    const auto part = findClicks(rest);
                    c.count += part.count;
                    c.worst = std::max(c.worst, part.worst);
                }
                logMessage(juce::String(variant == 0 ? "KICK to 20 %" : variant == 1 ? "BEAT slice, FWD" : "BEAT slice, 1SHT")
                           + ", REL 5 s, 12 notes: clicks " + juce::String(c.count) + ", worst jump " + juce::String(c.worst, 4));
                expect(peak(rig.out) > 0.05f);
                expectEquals(c.count, 0, "variant " + juce::String(variant));
            }
        }

        beginTest("MODE follows the sample until it is set by hand");
        {
            SamplerEngine e;
            auto& bank = SampleBank::get();
            const int tone = bank.indexOf("TONE"), kick = bank.indexOf("KICK"), beat = bank.indexOf("BEAT");
            expect(tone >= 0 && kick >= 0 && beat >= 0);
            expectEquals(e.getBankIndex(), tone, "a fresh sampler plays the sustaining tone");
            expectEquals((int) e.getParameter(S::SampleSelect), tone);
            expectEquals((int) e.getParameter(S::Mode), (int) S::Loop, "which loops");
            expectEquals(e.getSliceCount(), 0);

            e.setParameter(S::SampleSelect, (float) kick);
            expectEquals((int) e.getParameter(S::Mode), (int) S::Forward, "a drum hit plays once");
            e.setParameter(S::SampleSelect, (float) beat);
            expectEquals((int) e.getParameter(S::Mode), (int) S::Forward);
            expectEquals(e.getSliceCount(), 16);
            e.setParameter(S::SampleSelect, (float) tone);
            expectEquals((int) e.getParameter(S::Mode), (int) S::Loop);

            // A mode chosen by hand stays when the sample changes.
            e.setParameter(S::Mode, (float) S::Granular);
            e.setParameter(S::SampleSelect, (float) kick);
            expectEquals((int) e.getParameter(S::Mode), (int) S::Granular);
            e.setParameter(S::SampleSelect, (float) tone);
            expectEquals((int) e.getParameter(S::Mode), (int) S::Granular);
            e.setParameter(S::Mode, (float) S::Loop);
            e.setParameter(S::SampleSelect, (float) kick);
            expectEquals((int) e.getParameter(S::Mode), (int) S::Forward);
            e.setParameter(S::Mode, (float) S::Loop);
            e.setParameter(S::SampleSelect, (float) beat);
            expectEquals((int) e.getParameter(S::Mode), (int) S::Loop, "LOOP chosen for a hit is kept");

            // The page's defaults are the fresh engine's values.
            SamplerEngine fresh;
            for (auto& page : fresh.getParamPages())
                for (auto& p : page.params)
                    if (p.isValid() && (p.id == S::SampleSelect || p.id == S::Mode))
                        expectWithinAbsoluteError(fresh.getParameter(p.id), p.defaultVal, 1.0e-6f);
        }

        beginTest("The factory tone: tuned to the root key, joins up where it repeats");
        {
            const auto* e = SampleBank::get().entry(SampleBank::get().indexOf("TONE"));
            expect(e != nullptr);
            if (e != nullptr)
            {
                const auto& s = e->left;
                expect(s.size() > 40000);
                float p = 0.0f, maxStep = 0.0f;
                for (size_t i = 0; i < s.size(); ++i)
                {
                    p = std::max(p, std::abs(s[i]));
                    if (i > 0) maxStep = std::max(maxStep, std::abs(s[i] - s[i - 1]));
                }
                const float wrapStep = std::abs(s.front() - s.back());
                expect(p > 0.2f && p <= 0.6f, "peak " + juce::String(p, 3));
                expect(wrapStep <= maxStep * 1.01f, "last frame to first is a step like any other");

                // Pitch: the strongest of a few probe frequencies around middle C is 261.6 Hz.
                auto level = [&](double hz)
                {
                    double re = 0.0, im = 0.0;
                    for (size_t i = 0; i < s.size(); ++i)
                    {
                        const double ph = 2.0 * 3.141592653589793 * hz * (double) i / e->sampleRate;
                        re += s[i] * std::cos(ph);
                        im += s[i] * std::sin(ph);
                    }
                    return std::sqrt(re * re + im * im);
                };
                const double atC = level(261.63);
                expect(atC > 4.0 * level(246.94) && atC > 4.0 * level(277.18), "middle C, not its neighbours");
            }
        }
    }
};

static SamplerReleaseTests samplerReleaseTests;
