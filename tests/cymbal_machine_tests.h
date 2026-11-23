#pragma once

#include "fast_measure.h"
#include "legacy/LegacyHat.h"
#include "../src/engine/DrumEngine.h"
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include "../src/project/CymbalPresets.h"
#include "../src/project/FactoryPresets.h"
#include "../src/project/ProjectBridge.h"
#include "../src/ui/screens/DrumPadScreen.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include <juce_core/juce_core.h>
#include <complex>
#include <limits>
#include <vector>

// What became of the cymbal machine in phase U2b (.ralph/spec-unify.md), and the measures the
// sound tests share (namespace cymbaltest):
//   - its oscillator bank (METL, RING) and the drum machine's hats are ONE oscillator bank, drum
//     type BANK (src/engine/OscBank.h): tested here, against the code they came from;
//   - its bodies are sounds of the resonator drum (drum type RSN): tests/resonator_drum_tests.h,
//     which also has the atlas, old against new;
//   - the machine itself (drum type CSYN) is still in the product until the owner has decided
//     about the three sounds the resonator drum does not reach within the atlas's bounds: here it
//     is checked that it is what it was.
namespace cymbaltest
{
    // (In fast_measure.cpp, compiled with optimisation: the transform was most of this suite's time.)
    inline void fft(std::vector<std::complex<double>>& a) { fastmeasure::fft(a); }

    // Power spectrum of n samples from `start` (Hann window); bin i is i * sr / n Hz.
    inline std::vector<double> spectrum(const std::vector<float>& x, size_t start, size_t n)
    {
        std::vector<std::complex<double>> a(n);
        for (size_t i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * (double) i / (double) n);
            a[i] = (start + i < x.size() ? (double) x[start + i] : 0.0) * w;
        }
        fft(a);
        std::vector<double> p(n / 2);
        for (size_t i = 0; i < n / 2; ++i) p[i] = std::norm(a[i]);
        return p;
    }

    inline double centroid(const std::vector<float>& x, double seconds, double sr, size_t n = 2048)
    {
        const auto p = spectrum(x, (size_t) (seconds * sr), n);
        double num = 0.0, den = 0.0;
        for (size_t i = 1; i < p.size(); ++i)
        {
            num += (double) i * sr / (double) n * p[i];
            den += p[i];
        }
        return den > 1.0e-18 ? num / den : 0.0;
    }

    // The same over a stretch of time: the mean of the centroids of windows 50 ms apart. (The wash
    // is noise in narrow bands: one window of it lies anywhere within 15 % of this.)
    inline double brightness(const std::vector<float>& x, double from, double to, double sr, size_t n = 2048)
    {
        double sum = 0.0;
        int count = 0;
        for (double t = from; t < to; t += 0.05, ++count)
            sum += centroid(x, t, sr, n);
        return sum / (double) std::max(1, count);
    }

    inline double rms(const std::vector<float>& x, double from, double to, double sr)
    {
        const size_t a = (size_t) (from * sr), b = std::min(x.size(), (size_t) (to * sr));
        double s = 0.0;
        for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
        return std::sqrt(s / (double) std::max<size_t>(1, b - a));
    }

    // Bands: below 1 kHz, 1-4 kHz, 4-10 kHz, above 10 kHz, everything.
    struct Bands
    {
        double t60[5] = {};         // seconds until the band has fallen 60 dB below its loudest moment for good
        double loudest[5] = {};     // when that moment was
        double level[5] = {};       // its power then
    };

    inline Bands bands(const std::vector<float>& x, double sr, size_t hop = 512)
    {
        const size_t n = 1024;
        std::vector<double> e[5];
        for (size_t s = 0; s + n <= x.size(); s += hop)
        {
            const auto p = spectrum(x, s, n);
            double b[5] = {};
            for (size_t i = 1; i < p.size(); ++i)
            {
                const double f = (double) i * sr / (double) n;
                b[f < 1000.0 ? 0 : f < 4000.0 ? 1 : f < 10000.0 ? 2 : 3] += p[i];
                b[4] += p[i];
            }
            for (int k = 0; k < 5; ++k) e[k].push_back(b[k]);
        }
        Bands out;
        for (int k = 0; k < 5; ++k)
        {
            double mx = 0.0;
            size_t at = 0;
            for (size_t i = 0; i < e[k].size(); ++i)
                if (e[k][i] > mx) { mx = e[k][i]; at = i; }
            size_t last = at;
            for (size_t i = at; i < e[k].size(); ++i)
                if (e[k][i] > mx * 1.0e-6) last = i;
            out.t60[k] = (double) ((last + 1) * hop) / sr;
            out.loudest[k] = (double) (at * hop) / sr;
            out.level[k] = mx;
        }
        return out;
    }

    // The partials that stand out between 300 Hz and 12 kHz in 32768 samples from `seconds`: local
    // maxima within 40 dB of the strongest and 6 dB above the lowest point nearby on either side.
    inline std::vector<double> partials(const std::vector<float>& x, double sr, double seconds, std::vector<double>* powers = nullptr)
    {
        const size_t n = 32768;
        const auto p = spectrum(x, (size_t) (seconds * sr), n);
        const size_t lo = (size_t) (300.0 * (double) n / sr), hi = std::min(p.size() - 9, (size_t) (12000.0 * (double) n / sr));
        double mx = 0.0;
        for (size_t i = lo; i < hi; ++i) mx = std::max(mx, p[i]);
        std::vector<double> at;
        for (size_t i = lo + 8; i < hi; ++i)
        {
            if (p[i] < mx * 1.0e-4) continue;
            bool top = true;
            for (int d = -3; d <= 3; ++d) if (d != 0 && p[(size_t) ((int) i + d)] > p[i]) top = false;
            if (! top) continue;
            double l = p[i], r = p[i];
            for (size_t d = 1; d <= 8; ++d) { l = std::min(l, p[i - d]); r = std::min(r, p[i + d]); }
            if (p[i] > 4.0 * std::max(l, r))
            {
                // (the place between bins, from the three around the top)
                const double a = std::log(p[i - 1] + 1.0e-30), b = std::log(p[i] + 1.0e-30), c = std::log(p[i + 1] + 1.0e-30);
                const double shift = 0.5 * (a - c) / (a - 2.0 * b + c);
                at.push_back(((double) i + shift) * sr / (double) n);
                if (powers != nullptr) powers->push_back(p[i]);
            }
        }
        return at;
    }

    // How strongly the waveform repeats itself at a lag of 0.4 .. 12 ms, 0.1 .. 0.4 s after the
    // stroke: 1 = a periodic tone (a buzzer), near 0 = noise.
    inline double repeat(const std::vector<float>& x, double sr)
    {
        const size_t from = (size_t) (0.1 * sr), n = (size_t) (0.3 * sr), maxLag = (size_t) (0.012 * sr);
        if (from + n + maxLag >= x.size()) return 0.0;
        double e = 0.0;
        for (size_t i = 0; i < n; ++i) e += (double) x[from + i] * x[from + i];
        if (e < 1.0e-12) return 0.0;
        double best = 0.0;
        for (size_t lag = (size_t) (0.0004 * sr); lag < maxLag; ++lag)
        {
            double c = 0.0, e2 = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                c += (double) x[from + i] * x[from + i + lag];
                e2 += (double) x[from + i + lag] * x[from + i + lag];
            }
            best = std::max(best, c / std::sqrt(e * e2));
        }
        return best;
    }

    inline double dB(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }

    inline double largestStep(const std::vector<float>& x, size_t from, size_t to)
    {
        double m = 0.0;
        for (size_t i = std::max<size_t>(1, from); i < to && i < x.size(); ++i)
            m = std::max(m, (double) std::abs(x[i] - x[i - 1]));
        return m;
    }
}

class CymbalMachineTests : public juce::UnitTest
{
public:
    CymbalMachineTests() : UnitTest("Cymbal Machine") {}

    using D = DrumEngine;
    using B = OscBank;
    using M = CymbalMachine;

    static bool bounded(const std::vector<float>& x, float limit)
    {
        for (float s : x)
            if (! std::isfinite(s) || std::abs(s) > limit) return false;
        return true;
    }

    // A pad of the kit as a sound of the bank, played: struck at 0, let go after `held` seconds.
    struct Hit
    {
        int model = B::Hat;
        float tune = 8000.0f, decay = 0.3f, tone = 0.5f, snap = 0.5f, velocity = 1.0f;
        float page[8] = { 0.0f, 0.5f, 0.5f, 0.0f, 0.4f, 0.0f, 0.0f, 0.0f };
        double held = 10.0;
        float pressure = 0.0f;
        std::vector<double> strokes { 0.0 };
    };

    static std::vector<float> play(const Hit& h, double seconds, double sr = 48000.0)
    {
        DrumVoice v;
        v.setSampleRate(sr);
        v.setType(DrumSynthType::OscBank);
        v.setTune(h.tune); v.setDecay(h.decay); v.setTone(h.tone); v.setDrive(0.0f); v.setSnap(h.snap); v.setLevel(1.0f);
        for (int i = 1; i < 8; ++i) v.setBankParam(i, h.page[i]);
        v.setBankParam(B::ParModel, (float) h.model);
        std::vector<float> out((size_t) (seconds * sr));
        size_t next = 0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            if (next < h.strokes.size() && i >= (size_t) (h.strokes[next] * sr))
            {
                v.noteOn(h.velocity);
                if (h.pressure > 0.0f) v.setPadPressure(h.pressure);
                ++next;
            }
            if (i == (size_t) (h.held * sr))
            {
                v.noteOff();
                v.setPadPressure(0.0f);
            }
            out[i] = v.process();
        }
        return out;
    }

    void runTest() override
    {
        using namespace cymbaltest;
        const double sr = 48000.0;

        beginTest("One bank, four sounds: HAT and OHAT are the hats as the drum voice made them and METL and RING the cymbal "
                  "machine's two, to a millionth of full scale (the same arithmetic; a product is taken in another order)");
        {
            expectEquals((int) B::NumModels, 4);
            const char* names[4] = { "HAT", "OHAT", "METL", "RING" };
            for (int i = 0; i < 4; ++i)
                expectEquals(juce::String(B::modelNames()[i]), juce::String(names[i]));
            // The hats, against the code moved to tests/legacy/LegacyHat.h.
            for (int open = 0; open < 2; ++open)
                for (float tune : { 8000.0f, 5200.0f, 11000.0f })
                    for (float tone : { 0.2f, 0.5f, 0.9f })
                    {
                        LegacyHat old;
                        old.setSampleRate(sr);
                        old.open = open != 0; old.tune = tune; old.decay = open ? 0.5f : 0.04f; old.tone = tone; old.level = 1.0f;
                        old.noteOn(0.8f);
                        Hit h;
                        h.model = open ? B::OpenHat : B::Hat; h.tune = tune; h.decay = old.decay; h.tone = tone; h.velocity = 0.8f;
                        const auto now = play(h, 0.7);
                        double worst = 0.0, loudest = 0.0;
                        for (float s : now)
                        {
                            const float was = old.process();
                            worst = std::max(worst, (double) std::abs(was - s));
                            loudest = std::max(loudest, (double) std::abs(was));
                        }
                        expect(loudest > 0.05 && worst < 1.0e-6, juce::String(open ? "OHAT" : "HAT") + " TUNE " + juce::String(tune, 0) + " TONE "
                               + juce::String(tone, 1) + ": largest difference " + juce::String(worst, 9));
                    }
            // METL and RING, against the machine they came from (still in the product).
            for (int model = 0; model < 2; ++model)
                for (float snap : { 0.2f, 0.5f, 0.9f })
                {
                    M old;
                    old.setSampleRate(sr);
                    old.setParam(M::ParModel, (float) model);
                    old.setParam(M::ParBloom, 0.0f);
                    M::Strike hit;
                    hit.velocity = 0.8f; hit.tune = 7000.0f; hit.decay = 1.0f; hit.tone = 0.5f; hit.snap = snap;
                    old.strike(hit);
                    Hit h;
                    h.model = B::Metal + model; h.tune = 7000.0f; h.decay = 1.0f; h.snap = snap; h.velocity = 0.8f;
                    const auto now = play(h, 1.0);
                    double worst = 0.0, loudest = 0.0;
                    for (float s : now)
                    {
                        const float was = old.process(7000.0f);
                        worst = std::max(worst, (double) std::abs(was - s));
                        loudest = std::max(loudest, (double) std::abs(was));
                    }
                    expect(loudest > 0.05 && worst < 1.0e-6, juce::String(names[2 + model]) + " SNAP " + juce::String(snap, 1)
                           + ": largest difference " + juce::String(worst, 9));
                }
        }

        beginTest("Every cell of the BANK page is heard on every sound of it, and the pad's own cells");
        {
            const char* cells[8] = { "MODL", "PING", "COLR", "EXC", "DAMP", "BLOM", "COUP", "LOFI" };
            const float moved[8] = { 0.0f, 0.1f, 0.9f, 1.0f, 0.95f, 0.9f, 1.0f, 0.8f };
            for (int model = 0; model < B::NumModels; ++model)
            {
                Hit base;
                base.model = model;
                base.decay = 0.5f;
                const auto plain = play(base, 0.6);
                expect(rms(plain, 0.0, 0.1, sr) > 0.01, juce::String(B::modelNames()[model]) + " sounds");
                for (int cell = 1; cell < 8; ++cell)
                {
                    Hit h = base;
                    h.page[cell] = moved[cell];
                    if (cell == B::ParExcite) h.pressure = 1.0f;        // EXC: what the pad's pressure feeds
                    const auto x = play(h, 0.6);
                    const float d = fastmeasure::difference(plain, x, 1024);
                    expect(d > 0.02f && bounded(x, 3.0f), juce::String(B::modelNames()[model]) + " " + cells[cell] + ": " + juce::String(d, 3));
                }
                Hit tuned = base, toned = base, longer = base;
                tuned.tune = 5000.0f; toned.tone = 0.9f; longer.decay = 1.0f;
                expect(fastmeasure::difference(plain, play(tuned, 0.6), 1024) > 0.02f, "TUNE");
                expect(fastmeasure::difference(plain, play(toned, 0.6), 1024) > 0.02f, "TONE");
                const double from = model == B::Hat ? 0.1 : 0.4;        // (where the shorter one is nearly over)
                expect(rms(play(longer, 0.6), from, from + 0.1, sr) > 1.5 * rms(plain, from, from + 0.1, sr), juce::String(B::modelNames()[model]) + ": DEC");
            }
            // COUP: from the sum to the products (and RING the other way): METL at 1 is RING's kind of sound.
            Hit metal;
            metal.model = B::Metal;
            metal.page[B::ParCouple] = 1.0f;
            const auto products = partials(play(metal, 1.0), sr, 0.02);
            metal.page[B::ParCouple] = 0.0f;
            const auto sums = partials(play(metal, 1.0), sr, 0.02);
            expect(products.size() > sums.size(), "more partials multiplied (" + juce::String((int) products.size()) + ") than summed ("
                   + juce::String((int) sums.size()) + ")");
        }

        beginTest("The open hat is open for as long as it is held; a hat is the same hit every time; a cymbal's second stroke "
                  "adds; a hand ends either; in the kit the closed hat cuts the open one");
        {
            Hit open;
            open.model = B::OpenHat;
            open.decay = 0.5f;
            const auto held = play(open, 0.6);
            open.held = 0.05;
            const auto tapped = play(open, 0.6);
            expect(rms(held, 0.2, 0.3, sr) > 0.01 && rms(tapped, 0.2, 0.3, sr) < 0.01 * rms(held, 0.2, 0.3, sr), "let go, it closes");
            Hit twice;
            twice.strokes = { 0.0, 0.25 };
            twice.decay = 0.04f;
            const auto hats = play(twice, 0.5);
            bool same = true;
            for (int i = 0; i < 4800; ++i) same = same && hats[(size_t) i] == hats[(size_t) (12000 + i)];
            expect(same, "the same hat every time");
            Hit cymbal;
            cymbal.model = B::Metal;
            cymbal.decay = 1.5f;
            const auto one = play(cymbal, 1.0);
            cymbal.strokes = { 0.0, 0.3 };
            cymbal.velocity = 0.6f;
            const auto two = play(cymbal, 1.0);
            expect(largestStep(two, (size_t) (0.3 * sr) - 2, (size_t) (0.3 * sr) + 2) < 0.3, "the second stroke cuts nothing off");
            juce::ignoreUnused(one);

            DrumEngine kit;
            kit.prepareToPlay(sr, 64);
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            auto run = [&](double seconds)
            {
                float peak = 0.0f;
                for (int b = 0; b < (int) (seconds * sr) / 64; ++b)
                {
                    buf.clear();
                    kit.processBlock(buf, midi);
                    peak = std::max(peak, buf.getMagnitude(0, 64));
                }
                return peak;
            };
            expect(kit.isBank(14) && kit.isBank(15) && kit.getChokeGroup(14) == D::CHOKE_GROUP_HAT, "the starting kit's hats are the bank's");
            expectEquals((int) kit.getParameter(D::SOUND_BASE + 15 * 8 + B::ParModel), (int) B::OpenHat);
            kit.noteOn(15, 0.9f);
            expect(run(0.1) > 0.01f && kit.getVoice(15).isActive(), "the open hat rings while it is held");
            kit.noteOn(14, 0.9f);
            run(0.1);
            expect(! kit.getVoice(15).isActive(), "the closed hat cuts it");
            // A cymbal under the hand.
            kit.setParameter(D::SOUND_BASE + 15 * 8 + B::ParModel, (float) B::Metal);
            kit.setParameter(15 * 8 + D::DECAY, 2.0f);
            kit.noteOn(15, 0.9f);
            const float ringing = run(0.2);
            kit.getVoice(15).choke();
            run(0.1);
            expect(run(0.1) < 0.01f * ringing, "a hand on the cymbal");
        }

        beginTest("Bounded for anything: cells, strokes and pitches that are no numbers, 22.05 to 192 kHz; it ends, and at rest "
                  "it is exactly silent");
        {
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            for (double rate : { 22050.0, 44100.0, 96000.0, 192000.0 })
                for (int model = 0; model < B::NumModels; ++model)
                {
                    B bank;
                    bank.setSampleRate(rate);
                    bank.setParam(B::ParModel, (float) model);
                    B::Strike hit;
                    bool ok = true;
                    const float odd[6] = { nan, inf, -inf, 1.0e30f, -1.0e30f, 0.0f };
                    for (int round = 0; round < 6; ++round)
                    {
                        for (int cell = 1; cell < 8; ++cell)
                            bank.setParam(cell, odd[(round + cell) % 6]);
                        hit.velocity = odd[round]; hit.tune = odd[(round + 1) % 6]; hit.decay = odd[(round + 2) % 6];
                        hit.tone = odd[(round + 3) % 6]; hit.snap = odd[(round + 4) % 6];
                        bank.strike(hit);
                        bank.setPressure(odd[(round + 5) % 6]);
                        bank.setDecayStretch(odd[round]);
                        for (int i = 0; i < 2000; ++i)
                        {
                            const float y = bank.process(odd[(round + i / 500) % 6]);
                            ok = ok && std::isfinite(y) && std::abs(y) <= 12.0f;
                        }
                    }
                    expect(ok, juce::String(B::modelNames()[model]) + " at " + juce::String(rate, 0));
                    // A clean hit ends.
                    B clean;
                    clean.setSampleRate(rate);
                    clean.setParam(B::ParModel, (float) model);
                    B::Strike plain;
                    plain.decay = 0.2f;
                    clean.strike(plain);
                    clean.release();
                    float last = 1.0f;
                    for (int i = 0; i < (int) (3.0 * rate); ++i) last = clean.process(8000.0f);
                    expect(! clean.isActive() && last == 0.0f, juce::String(B::modelNames()[model]) + " ends in exact silence at " + juce::String(rate, 0));
                }
        }

        beginTest("[FOLD] CSYN is folded to the resonator drum: setting TYPE to CymbalSynth gives a resonator at "
                  "sound RIDE; the kits use the new sounds only");
        {
            DrumEngine kit;
            kit.prepareToPlay(sr, 64);
            const int padIndex = 6;
            kit.setParameter(padIndex * 8 + D::TYPE, (float) (int) DrumSynthType::CymbalSynth);
            // [FOLD] After the fold, CSYN is a resonator drum; the page is RSN.
            expect(kit.isResonator(padIndex), "CSYN folds to resonator");
            expectEquals(kit.soundPage(padIndex).name, juce::String("RSN"), "sound page is RSN");
            // The TYPE cell holds what it offers: RSN (it held the stored 13, past its ten choices,
            // and read "BANK" over a resonator drum).
            expectWithinAbsoluteError(kit.getParameter(padIndex * 8 + D::TYPE),
                                      (float) (int) DrumSynthType::Resonator, 0.01f, "the cell is RSN");
            // The sound loaded is RIDE (what foldedSound gives for CSYN).
            const int rideIndex = resonatorSoundIndex("RIDE");
            expectEquals((int) kit.getParameter(D::SOUND_BASE + padIndex * 8 + D::RsnSound), rideIndex, "sound is RIDE");
            // It sounds (a resonator drum at the ride sound).
            kit.setParameter(padIndex * 8 + D::LEVEL, 1.0f);
            kit.setParameter(padIndex * 8 + D::PAN, 0.0f);
            kit.setSlotAftertouchDest(padIndex, DrumATDest::Off);
            kit.noteOn(padIndex, 1.0f);
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            double loudest = 0.0;
            for (int b = 0; b < 100; ++b)
            {
                buf.clear();
                kit.processBlock(buf, midi);
                for (int i = 0; i < 64; ++i)
                    loudest = std::max(loudest, (double) std::abs(buf.getSample(0, i)));
            }
            expect(loudest > 0.01, "the folded CSYN pad sounds");

            const auto presets = FactoryPresets::getAll();
            int kits = 0;
            for (const auto& preset : presets)
            {
                if (preset.engineType != EngineType::Drum)
                    continue;
                ++kits;
                for (int padOf = 0; padOf < 16; ++padOf)
                {
                    const auto type = preset.soundParams.find(padOf * 8 + D::TYPE);
                    if (type == preset.soundParams.end())
                        continue;
                    const int t = (int) type->second;
                    expect(t >= 0 && t < D::NUM_DRUM_TYPES, preset.name + ": a type TYPE offers");
                    expect(t != (int) DrumSynthType::Tom && t != (int) DrumSynthType::NoiseSnare && t != (int) DrumSynthType::FMSnare
                           && t != (int) DrumSynthType::CymbalSynth, preset.name + " pad " + juce::String(padOf + 1) + " is made of the new sounds");
                }
            }
            expect(kits >= 10);
            auto find = [&](const char* name) -> const Preset*
            {
                for (auto& p : presets)
                    if (p.name == name) return &p;
                return nullptr;
            };
            const Preset* works = find(CymbalPresets::worksName);
            expect(works != nullptr);
            if (works != nullptr)
            {
                int resonators = 0, banks = 0;
                for (int padOf = 0; padOf < 16; ++padOf)
                {
                    const auto type = works->soundParams.find(padOf * 8 + D::TYPE);
                    if (type == works->soundParams.end()) continue;
                    resonators += (int) type->second == (int) DrumSynthType::Resonator ? 1 : 0;
                    banks += (int) type->second == (int) DrumSynthType::OscBank ? 1 : 0;
                }
                expectEquals(resonators, 8, "Cymbal Works: eight sounds of the resonator drum");
                expectEquals(banks, 2, "and the bank's METL and RING");
            }
        }
    }
};

static CymbalMachineTests cymbalMachineTests;
