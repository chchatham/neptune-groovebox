#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <functional>
#include <vector>
#include "fast_measure.h"
#include "old/OldFMEngine.h"
#include "../src/engine/FMEngine.h"
#include "../src/project/PresetManager.h"

// -----------------------------------------------------------------------------
// Phase U4-FM: the FM engine on the modulation router.
//
// 1. OLD AGAINST NEW. tests/old/OldFMEngine.h is the engine as it was (its own LFO section, mod
//    slots, XIN cells, vibrato and pressure block). Every FM factory preset and every folded
//    feature is played on both with the same gesture and the two renders are compared:
//      MATCH  bit for bit
//      CLOSE  level within 1 dB, the level in 20 ms windows within 2 dB (windows within 30 dB of
//             the loudest), the spectrum in third-octave bands within 3 dB (bands within 40 dB of
//             the strongest); each side of the stereo pair by itself
//      LOST   anything else
//    NEPTUNE_U4_PRINT=1 prints the table; NEPTUNE_U4_ATLAS=<folder> writes an old / new pair of
//    WAV files for everything that is not MATCH.
// 2. What is new with the router: the table and the source list, OP1 .. OP6 as sources, the
//    operators' phases as destinations, ETIM / FTIM, the per-note randoms, per-voice pressure.
// 3. NEPTUNE_U4_BENCH=1 (a Release build of the test binary): old against new, % of one core.
// -----------------------------------------------------------------------------
class U4FmTests : public juce::UnitTest
{
public:
    U4FmTests() : UnitTest("U4 FM") {}

    using F = FMEngine;
    using O = OldFMEngine;
    using R = mod::Router;
    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;

    static juce::String env(const char* name) { return juce::SystemStats::getEnvironmentVariable(name, {}); }

    // ---- The gesture ---------------------------------------------------------------------------
    struct Gesture
    {
        float strip = 0.0f;                         // the mod strip, from the start
        float pressure = 0.0f;                      // on the first note's pad, from 0.1 s
        const AftertouchRouter* routes = nullptr;
        bool input = false;                         // a 330 Hz sine at 0.7 on the track's input
        int blocks = 1875;                          // 2.5 s
        int block = BLOCK;
        double rate = SR;                           // (the review: 44.1 and 96 kHz too)
    };

    // Three notes, overlapping, at three velocities; released; the tails ring out. Left, then right.
    template <typename Engine>
    static std::vector<float> play(Engine& e, const Gesture& g)
    {
        e.prepareToPlay(g.rate, g.block);
        if (g.routes != nullptr)
            e.setAftertouchRouter(g.routes);
        e.setModWheel(g.strip);
        std::vector<float> l, r, in((size_t) g.block);
        juce::AudioBuffer<float> buf(2, g.block);
        juce::MidiBuffer midi;
        long at = 0;
        const int per = (int) (g.rate / g.block);   // blocks a second
        for (int b = 0; b < g.blocks; ++b)
        {
            if (b == 0)             e.noteOn(48, 0.9f, -1);
            if (b == per / 10 && g.pressure > 0.0f) e.aftertouch(48, g.pressure);
            if (b == per / 4)       e.noteOn(60, 0.6f, -1);
            if (b == per / 2)       e.noteOn(67, 1.0f, -1);
            if (b == per)           e.noteOff(48, -1);
            if (b == per * 5 / 4)   { e.noteOff(60, -1); e.noteOff(67, -1); }
            if (g.input)
            {
                for (int i = 0; i < g.block; ++i)
                    in[(size_t) i] = 0.7f * (float) std::sin(6.283185307179586 * 330.0 * (double) (at + i) / g.rate);
                e.setAudioInput({ in.data(), in.data(), g.block, 0.5f });
            }
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < g.block; ++i)
            {
                l.push_back(buf.getSample(0, i));
                r.push_back(buf.getSample(1, i));
            }
            at += g.block;
        }
        e.setAudioInput({});
        e.setAftertouchRouter(nullptr);
        l.insert(l.end(), r.begin(), r.end());
        return l;
    }

    // ---- The comparison ------------------------------------------------------------------------
    struct Verdict
    {
        bool identical = false;
        double level = 0.0, envelope = 0.0, spectrum = 0.0, residual = 0.0;    // dB
        juce::String word;
    };

    static double rms(const float* x, size_t n)
    {
        double sq = 0.0;
        for (size_t i = 0; i < n; ++i)
            sq += (double) x[i] * x[i];
        return std::sqrt(sq / (double) std::max<size_t>(1, n));
    }
    static double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }

    static std::vector<double> bands(const float* x, size_t n, double rate = SR)
    {
        size_t size = 1;
        while (size < n) size <<= 1;
        std::vector<std::complex<double>> a(size);
        for (size_t i = 0; i < n; ++i)
            a[i] = x[i];
        fastmeasure::fft(a);
        std::vector<double> out;
        for (double lo = 44.5; lo < 18000.0; lo *= 1.2599)
        {
            const size_t from = (size_t) (lo * (double) size / rate), to = (size_t) (lo * 1.2599 * (double) size / rate);
            double sum = 0.0;
            for (size_t k = from; k < to && k < size / 2; ++k)
                sum += std::norm(a[k]);
            out.push_back(sum);
        }
        return out;
    }

    // One side (n samples) of old against new.
    static void compareSide(const float* a, const float* b, size_t n, Verdict& v, double rate = SR)
    {
        const double ra = rms(a, n), rb = rms(b, n);
        if (ra < 1.0e-9 && rb < 1.0e-9)
            return;
        v.level = std::max(v.level, std::abs(db(rb / std::max(ra, 1.0e-12))));
        const size_t window = (size_t) (0.02 * rate);
        double loudest = 0.0;
        for (size_t at = 0; at + window <= n; at += window)
            loudest = std::max(loudest, rms(a + at, window));
        for (size_t at = 0; at + window <= n; at += window)
        {
            const double wa = rms(a + at, window), wb = rms(b + at, window);
            if (wa > loudest * 0.0316)
                v.envelope = std::max(v.envelope, std::abs(db(wb / wa)));
        }
        const auto sa = bands(a, n, rate), sb = bands(b, n, rate);
        double strongest = 0.0;
        for (double s : sa) strongest = std::max(strongest, s);
        for (size_t k = 0; k < sa.size(); ++k)
            if (sa[k] > strongest * 1.0e-4)
                v.spectrum = std::max(v.spectrum, std::abs(10.0 * std::log10(std::max(sb[k], 1.0e-30) / sa[k])));
        std::vector<float> d(n);
        for (size_t i = 0; i < n; ++i)
            d[i] = b[i] - a[i];
        v.residual = std::max(v.residual, db(rms(d.data(), n) / std::max(ra, 1.0e-12)));
    }

    // mid: the two sides are compared as their sum (a sound whose place between them is drawn at
    // random per note).
    static Verdict compare(const std::vector<float>& oldSound, const std::vector<float>& newSound, bool mid = false, double rate = SR)
    {
        Verdict v;
        v.residual = -240.0;
        v.identical = oldSound.size() == newSound.size()
                      && std::memcmp(oldSound.data(), newSound.data(), oldSound.size() * sizeof(float)) == 0;
        const size_t n = oldSound.size() / 2;
        if (mid)
        {
            std::vector<float> a(n), b(n);
            for (size_t i = 0; i < n; ++i)
            {
                a[i] = 0.5f * (oldSound[i] + oldSound[n + i]);
                b[i] = 0.5f * (newSound[i] + newSound[n + i]);
            }
            compareSide(a.data(), b.data(), n, v, rate);
        }
        else
        {
            compareSide(oldSound.data(), newSound.data(), n, v, rate);
            compareSide(oldSound.data() + n, newSound.data() + n, n, v, rate);
        }
        v.word = v.identical ? "MATCH" : (v.level <= 1.0 && v.envelope <= 2.0 && v.spectrum <= 3.0 ? "CLOSE" : "LOST");
        return v;
    }

    static void writeWav(const juce::String& name, const std::vector<float>& sound)
    {
        const auto dir = env("NEPTUNE_U4_ATLAS");
        if (dir.isEmpty() || sound.empty())
            return;
        juce::File folder(dir);
        folder.createDirectory();
        juce::File file = folder.getChildFile(name + ".wav");
        file.deleteFile();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
        if (stream == nullptr)
            return;
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), SR, 2, 24, {}, 0));
        if (writer == nullptr)
            return;
        stream.release();
        const size_t n = sound.size() / 2;
        const float* channels[2] = { sound.data(), sound.data() + n };
        writer->writeFromFloatArrays(channels, 2, (int) n);
    }

    void report(const juce::String& name, const Verdict& v, const std::vector<float>& a, const std::vector<float>& b)
    {
        if (env("NEPTUNE_U4_PRINT").isNotEmpty())
            logMessage(juce::String("U4FM | ") + name.paddedRight(' ', 34) + " | " + v.word
                       + " | level " + juce::String(v.level, 2) + " dB | 20 ms " + juce::String(v.envelope, 2)
                       + " dB | bands " + juce::String(v.spectrum, 2) + " dB | difference " + juce::String(v.residual, 1) + " dB");
        if (! v.identical)
        {
            const auto file = name.replaceCharacters(" >/:,.()+", "_________");
            writeWav(file + "_old", a);
            writeWav(file + "_new", b);
        }
    }

    // ---- Cells, by number: the same on both engines --------------------------------------------
    static void lfo(EngineBase& e, int i, float hz, float depth, int dest, int shape = 0)
    {
        e.setParameter(158 + i * 8, hz);
        e.setParameter(159 + i * 8, depth);
        e.setParameter(160 + i * 8, (float) shape);
        e.setParameter(161 + i * 8, 1.0f);              // TRIG KEY: old and new start alike
        e.setParameter(165 + i * 8, (float) dest);
    }
    static void slot(EngineBase& e, int s, int src, int dest, float amount, int via = 0)
    {
        e.setParameter(700 + s * 4, (float) src);
        e.setParameter(701 + s * 4, (float) dest);
        e.setParameter(702 + s * 4, amount);
        e.setParameter(703 + s * 4, (float) via);
    }
    // A sound with something for every destination to act on: [2 > 1] and a third carrier, with
    // feedback, sustaining; the filter in play.
    static void rich(EngineBase& e)
    {
        e.setParameter(F::AlgorithmParam, 6.0f);            // [2 > 1], 3 .. 6 carriers
        e.setParameter(F::Op1Level, 0.8f);
        e.setParameter(F::Op2Level, 0.6f);  e.setParameter(F::Op2Ratio, 2.0f);  e.setParameter(F::Op2Feedback, 0.3f);
        e.setParameter(F::Op3Level, 0.3f);  e.setParameter(F::Op3Ratio, 3.0f);
        for (int op = 0; op < 3; ++op)
        {
            e.setParameter(op * 10 + 5, 0.6f);
            e.setParameter(op * 10 + 6, 0.6f);
        }
        e.setParameter(F::FilterBase + VoiceFilterSection::Cutoff, 2500.0f);
        e.setParameter(F::FilterBase + VoiceFilterSection::Resonance, 0.3f);
        e.setParameter(F::FilterBase + VoiceFilterSection::EnvAmount, 0.3f);
    }
    static void tables(EngineBase& e)
    {
        for (int op = 0; op < 2; ++op)
        {
            e.setParameter(op * 10 + F::OP_WAVE, (float) (phaseosc::FIRST_TABLE_WAVE + 9));
            e.setParameter(op * 10 + F::OP_POSITION, 0.5f);
        }
    }

    struct Feature
    {
        const char* name;
        std::function<void (EngineBase&)> setUp;
        Gesture gesture {};
        bool mid = false;
        std::function<void (EngineBase&, bool isNew)> more = nullptr;
    };

    // =============================================================================================
    void runTest() override
    {
        oldAgainstNew();
        tableTests();
        sourceTests();
        timeAndRandomTests();
        voiceTests();
        reviewTests();
        if (env("NEPTUNE_U4_BENCH").isNotEmpty())
            bench();
    }

    void oldAgainstNew()
    {
        beginTest("Old against new: every FM factory preset is the same sound, bit for bit (three notes, the tails; blocks of 64 and of 100)");
        {
            PresetManager pm;
            int found = 0, beyond = 0;
            for (int i = 0; i < pm.getPresetCount(); ++i)
            {
                const Preset& p = pm.getPreset(i);
                if (p.engineType != EngineType::FM || ! p.isFactory)
                    continue;
                // A preset that sets a cell of the router (a slot from an operator to the filter:
                // "FMFilter") plays what the old engine had no way of playing: nothing to compare.
                bool routed = false;
                for (auto& [id, value] : p.soundParams)
                    routed = routed || (mod::Router::ownsSlotBlock(id) && value != 0.0f);
                if (routed)
                {
                    ++beyond;
                    continue;
                }
                ++found;
                for (int block : { 64, 100 })
                {
                    O oldEngine;
                    F newEngine;
                    for (auto& [id, value] : p.soundParams)
                    {
                        oldEngine.setParameter(id, value);
                        newEngine.setParameter(id, value);
                    }
                    Gesture g;
                    g.block = block;
                    g.blocks = (int) (2.5 * SR / block);
                    const auto a = play(oldEngine, g), b = play(newEngine, g);
                    const Verdict v = compare(a, b);
                    report("preset " + p.name + (block == 64 ? "" : " (blocks of 100)"), v, a, b);
                    expect(rms(a.data(), a.size()) > 1.0e-3, p.name + " sounds");
                    expect(v.identical, p.name + " at blocks of " + juce::String(block) + ": " + v.word + ", difference " + juce::String(v.residual, 1) + " dB");
                }
            }
            expectEquals(found, 17, "the FM factory presets the old engine can play");   // (U6: FMWaveOp, FM Cycle, SprngKey beside the fourteen)
            expectEquals(beyond, 1, "and the one that routes an operator through the router (FMFilter)");
        }

        beginTest("Old against new: the default sound, and the rich test sound with nothing routed, with the track input connected and a pad pressed on no route: bit for bit");
        {
            AftertouchRouter none;
            none.clear();
            for (int variant = 0; variant < 4; ++variant)
            {
                O oldEngine;
                F newEngine;
                if (variant > 0) { rich(oldEngine); rich(newEngine); }
                Gesture g;
                g.input = variant == 2;
                if (variant == 3) { g.pressure = 0.7f; g.routes = &none; }
                const auto a = play(oldEngine, g), b = play(newEngine, g);
                const Verdict v = compare(a, b);
                report("plain " + juce::String(variant), v, a, b);
                expect(v.identical, "variant " + juce::String(variant) + ": " + juce::String(v.residual, 1) + " dB");
            }
        }

        beginTest("Old against new: each folded feature (LFO to each kind of destination, mod slots, XIN, the strip's routes and vibrato, pressure) is the same sound within the stated measures");
        {
            using D = AftertouchRouter::Destination;
            static AftertouchRouter volume, cutoff, depth;
            volume.clear(); volume.setRoute(0, { D::Volume, 0.8f, false });
            cutoff.clear(); cutoff.setRoute(0, { D::FilterCutoff, 0.5f, false });
            depth.clear();  depth.setRoute(0, { D::FMDepth, 1.0f, false });
            Gesture plain, stripUp, input, pressVolume, pressCutoff, pressDepth, pressStrip;
            stripUp.strip = 1.0f;
            input.input = true;
            pressVolume.pressure = 0.7f; pressVolume.routes = &volume;
            pressCutoff.pressure = 0.7f; pressCutoff.routes = &cutoff;
            pressDepth.pressure = 0.7f;  pressDepth.routes = &depth;
            pressStrip = pressVolume; pressStrip.strip = 0.8f;
            const int LVL2 = 10, RAT3 = 17, ATK1 = 23, FDB2 = 28, ENVF = 57, FDEC = 62, LRAT = 67, POS1 = 69;

            std::vector<Feature> features;
            features.push_back({ "LFO1 > PTCH", [](EngineBase& e) { lfo(e, 0, 5.0f, 0.3f, 1); } });
            features.push_back({ "LFO1 > AMP", [](EngineBase& e) { lfo(e, 0, 5.0f, 0.6f, 2); } });
            features.push_back({ "LFO1 > PAN", [](EngineBase& e) { lfo(e, 0, 3.0f, 0.8f, 3); } });
            features.push_back({ "LFO1 > CUT", [](EngineBase& e) { lfo(e, 0, 4.0f, 0.4f, 4); } });
            features.push_back({ "LFO1 > RESO", [](EngineBase& e) { lfo(e, 0, 4.0f, 0.8f, 5); } });
            features.push_back({ "LFO1 > INDX", [](EngineBase& e) { lfo(e, 0, 4.0f, 0.8f, 6); } });
            features.push_back({ "LFO1 > FDBK", [](EngineBase& e) { lfo(e, 0, 4.0f, 0.6f, 7); } });
            features.push_back({ "LFO1 > DETN", [](EngineBase& e) { lfo(e, 0, 2.0f, 0.8f, 8); } });
            features.push_back({ "LFO1 > LVL2 (operator level)", [=](EngineBase& e) { lfo(e, 0, 4.0f, 0.4f, LVL2); } });
            features.push_back({ "LFO1 > RAT3 (operator ratio)", [=](EngineBase& e) { lfo(e, 0, 3.0f, 0.05f, RAT3); } });
            features.push_back({ "LFO1 > ATK1 (operator attack)", [=](EngineBase& e) { lfo(e, 0, 1.0f, 0.8f, ATK1, 3); } });
            features.push_back({ "LFO1 > FDB2 (operator feedback)", [=](EngineBase& e) { lfo(e, 0, 4.0f, 0.5f, FDB2); } });
            features.push_back({ "LFO1 > ENV (filter env amount)", [=](EngineBase& e) { lfo(e, 0, 4.0f, 0.3f, ENVF); } });
            features.push_back({ "LFO1 > FDEC (filter decay)", [=](EngineBase& e) { lfo(e, 0, 1.0f, 0.8f, FDEC, 3); } });
            features.push_back({ "LFO1 > POS1 (table position)", [=](EngineBase& e) { tables(e); lfo(e, 0, 3.0f, 0.4f, POS1); } });
            features.push_back({ "LFO2 > LRAT, LFO1 > PTCH", [=](EngineBase& e) { lfo(e, 0, 5.0f, 0.3f, 1); lfo(e, 1, 0.5f, 0.8f, LRAT); } });
            features.push_back({ "LFO1 square > AMP", [](EngineBase& e) { lfo(e, 0, 6.0f, 0.5f, 2, 3); } });
            features.push_back({ "slot ENV > INDX", [](EngineBase& e) { slot(e, 0, 3, 6, 0.8f); } });
            features.push_back({ "slot VEL > CUT", [](EngineBase& e) { slot(e, 0, 5, 4, 0.6f); } });
            features.push_back({ "slot KEY > FDBK", [](EngineBase& e) { slot(e, 0, 8, 7, 0.9f); } });
            features.push_back({ "slot FENV > PTCH", [](EngineBase& e) { slot(e, 0, 4, 1, 0.4f); } });
            features.push_back({ "slot LFO2 > LVL2 via STRP", [=](EngineBase& e) { lfo(e, 1, 5.0f, 0.0f, 0); slot(e, 0, 2, LVL2, 0.5f, 7); }, stripUp });
            features.push_back({ "slot PRES > CUT", [](EngineBase& e) { slot(e, 0, 6, 4, 0.5f); }, pressStrip });
            features.push_back({ "slot IN > AMP", [](EngineBase& e) { slot(e, 0, 10, 2, -0.8f); }, input });
            features.push_back({ "slot RAND > PAN (sides summed)", [](EngineBase& e) { slot(e, 0, 9, 3, 0.9f); }, plain, true });
            features.push_back({ "slot LFO1 > L2RT, LFO2 > CUT", [](EngineBase& e) { lfo(e, 0, 0.7f, 0.0f, 0); lfo(e, 1, 4.0f, 0.4f, 4); },
                                 plain, false, [](EngineBase& e, bool isNew) { slot(e, 0, 1, isNew ? 86 : 77, 0.7f); } });
            features.push_back({ "XIN > OP1 at 50 (old XIN cells)", [](EngineBase& e) { e.setParameter(64, 1.0f); e.setParameter(65, 0.5f); }, input });
            features.push_back({ "XIN > ALL at 100 (old XIN cells)", [](EngineBase& e) { e.setParameter(64, 7.0f); e.setParameter(65, 1.0f); }, input });
            features.push_back({ "XIN > OP2 at 80, the XIN page", [](EngineBase&) {}, input, false, [](EngineBase& e, bool isNew)
            {
                if (isNew)  { e.setParameter(R::xinId(R::XinDest1), (float) (FMVoice::LfoPhase + 1)); e.setParameter(R::xinId(R::XinAmount1), 0.8f); }
                else        { e.setParameter(64, 2.0f); e.setParameter(65, 0.8f); }
            } });
            features.push_back({ "strip up, default routes (INDX x 3, vibrato)", [](EngineBase&) {}, stripUp });
            features.push_back({ "strip up, STRIP page: CUT -60, PAN +50, no vibrato", [](EngineBase& e)
            {
                e.setParameter(174, 4.0f); e.setParameter(175, -0.6f);
                e.setParameter(176, 3.0f); e.setParameter(177, 0.5f);
                e.setParameter(180, 0.0f);
            }, stripUp });
            features.push_back({ "strip up, vibrato only (1 st, 7 Hz)", [](EngineBase& e)
            {
                e.setParameter(175, 0.0f); e.setParameter(180, 1.0f); e.setParameter(181, 7.0f);
            }, stripUp });
            features.push_back({ "strip up, the filter's WHL", [](EngineBase& e)
            {
                e.setParameter(175, 0.0f); e.setParameter(180, 0.0f);
                e.setParameter(F::FilterBase + VoiceFilterSection::WheelAmount, 0.5f);
            }, stripUp });
            features.push_back({ "pressure > VOL", [](EngineBase&) {}, pressVolume });
            features.push_back({ "pressure > CUT", [](EngineBase&) {}, pressCutoff });
            features.push_back({ "pressure > FM depth", [](EngineBase&) {}, pressDepth });
            features.push_back({ "pressure > VOL with LFO1 > AMP", [](EngineBase& e) { lfo(e, 0, 5.0f, 0.5f, 2); }, pressVolume });
            features.push_back({ "pressure > FM depth with LFO1 > INDX", [](EngineBase& e) { lfo(e, 0, 4.0f, 0.5f, 6); }, pressDepth });
            // The old engine multiplied the pad's share of the level by the LFO's; the router adds
            // what is aimed at one destination. The product is one more slot: the LFO on the level
            // by way of the pad (amount = the LFO's depth x the route's).
            features.push_back({ "the same, + slot LFO1 > AMP via PRES", [](EngineBase& e) { lfo(e, 0, 5.0f, 0.5f, 2); }, pressVolume, false,
                                 [](EngineBase& e, bool isNew) { if (isNew) slot(e, 0, 1, 2, 0.4f, 6); } });

            // (The review's rows.) ENV is operator 1's envelope and no other's: operator 2's is short here.
            features.push_back({ "slot ENV > CUT, operator 2's envelope short", [](EngineBase& e)
            {
                e.setParameter(F::Op2Decay, 0.03f); e.setParameter(F::Op2Sustain, 0.0f);
                slot(e, 0, 3, 4, -0.7f);
            } });
            // An LFO locked to the tempo, with its fade-in (cells 162 SYNC, 163 DIV, 164 FADE).
            features.push_back({ "LFO1 at the tempo (96, DIV 5), FADE 0.3 s > AMP", [](EngineBase& e)
            {
                lfo(e, 0, 5.0f, 0.6f, 2);
                e.setParameter(162, 1.0f); e.setParameter(163, 5.0f); e.setParameter(164, 0.3f);
                e.setTempo(96.0f);
            } });
            features.push_back({ "LFO1 free (TRIG FREE) > CUT, three notes", [](EngineBase& e) { lfo(e, 0, 4.0f, 0.5f, 4); e.setParameter(161, 0.0f); } });

            // What is bit for bit today stays bit for bit (a row that slid from MATCH to CLOSE would
            // otherwise pass: the filter's WHL amount not reaching the voices did).
            const juce::StringArray same { "LFO1 > ATK1 (operator attack)", "slot VEL > CUT", "slot KEY > FDBK", "slot IN > AMP",
                                           "XIN > OP1 at 50 (old XIN cells)", "XIN > ALL at 100 (old XIN cells)",
                                           "XIN > OP2 at 80, the XIN page", "strip up, the filter's WHL" };

            // Knowingly not the old sound by the measures (each is named in the phase's report):
            //   RAND      the per-note random is the one seeded generator's now: other draws
            //   VOL + AMP pad pressure and an LFO on the level add where they multiplied
            const juce::StringArray moved { "slot RAND > PAN (sides summed)", "pressure > VOL with LFO1 > AMP" };
            int match = 0, close = 0, lost = 0;
            juce::String lostNames;
            for (auto& f : features)
            {
                O oldEngine;
                F newEngine;
                rich(oldEngine);    rich(newEngine);
                f.setUp(oldEngine); f.setUp(newEngine);
                if (f.more) { f.more(oldEngine, false); f.more(newEngine, true); }
                // (Against the plain sound: the feature must be heard, or the comparison says nothing.)
                O plainEngine;
                rich(plainEngine);
                const auto a = play(oldEngine, f.gesture), b = play(newEngine, f.gesture), dry = play(plainEngine, Gesture());
                const Verdict v = compare(a, b, f.mid), heard = compare(dry, a);
                report(f.name, v, a, b);
                expect(! heard.identical && heard.residual > -40.0, juce::String(f.name) + " is heard on the old engine (" + juce::String(heard.residual, 1) + " dB)");
                if (same.contains(f.name))
                    expect(v.identical, juce::String(f.name) + " is bit for bit (difference " + juce::String(v.residual, 1) + " dB)");
                match += v.word == "MATCH";
                close += v.word == "CLOSE";
                if (v.word == "LOST" && ! moved.contains(f.name))
                {
                    ++lost;
                    lostNames += juce::String(f.name) + " (level " + juce::String(v.level, 2) + ", 20 ms " + juce::String(v.envelope, 2)
                                 + ", bands " + juce::String(v.spectrum, 2) + "); ";
                }
            }
            logMessage("U4FM features: " + juce::String(match) + " MATCH, " + juce::String(close) + " CLOSE, " + juce::String(lost) + " LOST");
            expectEquals(lost, 0, "features outside the measures: " + lostNames);
        }
    }

    // =============================================================================================
    void tableTests()
    {
        beginTest("The table: the DEST list as it was (75 entries, the same names at the same places), then ETIM FTIM, PM1 .. PM6, PMAL; 84 in all, under DestTable::MAX");
        {
            const auto& t = F::lfoDests();
            const auto& before = O::lfoDests();
            expectEquals(before.size(), 75);
            for (int i = 0; i < before.size(); ++i)
                expectEquals(juce::String(t.labels()[i]), juce::String(before.labels()[i]), "place " + juce::String(i));
            for (int i = 1; i < before.size(); ++i)
                expectEquals(t[i].id, before[i].key, juce::String("the cell behind ") + before.labels()[i]);
            expectEquals(t.size(), 84);
            expectEquals(t.visible(), 84);
            expect(t.size() <= mod::DestTable::MAX);
            static const char* const added[] = { "ETIM", "FTIM", "PM1", "PM2", "PM3", "PM4", "PM5", "PM6", "PMAL" };
            for (int i = 0; i < 9; ++i)
                expectEquals(juce::String(t.labels()[75 + i]), juce::String(added[i]));
            expectEquals(t.ampTime(), (int) FMVoice::LfoEnvTime);
            expectEquals(t.filterTime(), (int) FMVoice::LfoFilterTime);
            expectEquals(t.indexOfLabel("POS1"), (int) FMVoice::LfoPosition);
            expectEquals(t.indexOfLabel("PM1"), (int) FMVoice::LfoPhase);
            expectEquals(t.indexOfLabel("PMAL"), (int) FMVoice::LfoPhaseAll);
            // Per sample: what an audio-rate source can reach.
            for (const char* name : { "PTCH", "AMP", "PAN", "CUT", "INDX", "FDBK", "POS1", "POS6", "PM1", "PM6", "PMAL" })
                expect(t[t.indexOfLabel(name)].perSample, juce::String(name) + " is taken per sample");
            for (const char* name : { "RESO", "DETN", "LVL1", "RAT1", "ATK1", "ETIM", "FTIM" })
                expect(! t[t.indexOfLabel(name)].perSample, juce::String(name) + " is taken once a sub-block");
            expectEquals(t.buffers(), 19);
            expectEquals(t[FMVoice::LfoIndex].stripScale, 2.0f);
            // A free slot's own destinations follow the visible list: nine places further on.
            expectEquals(juce::String(t.slotLabels()[84]), juce::String("L1RT"));
            expectEquals(t.slotChoices(), 84 + 4 + 16 + mod::TrackDest::COUNT);     // (and the track's effect cells: E4)
        }

        beginTest("The source list: as it was through IN, the router's, then OP1 .. OP6 at audio rate; the pages end XIN, SLT1, LSHP, STRIP, LFO1, LFO2; nothing of the engine's at 158-189 or 700-779");
        {
            F e;
            const auto& s = F::sources();
            static const char* const first[] = { "OFF", "LFO1", "LFO2", "ENV", "FENV", "VEL", "PRES", "STRP", "KEY", "RAND", "IN" };
            for (int i = 0; i < 11; ++i)
            {
                expectEquals(juce::String(s.labels()[i]), juce::String(first[i]));
                expectEquals(juce::String(ModSrc::labels()[i]), juce::String(first[i]), "the old list");
            }
            expectEquals(s.published(), 6);
            for (int k = 0; k < 6; ++k)
            {
                expectEquals(juce::String(s.labels()[s.placeOfPublished(k)]), "OP" + juce::String(k + 1));
                expect(s.audio(k) && s.bipolar(k));
                expectEquals(s.placeOfPublished(k), s.size() - 6 + k);
            }
            const auto pages = e.getParamPages();
            static const char* const last[] = { "F ENV", "XIN", "SLT1", "LSHP", "STRIP", "LFO1", "LFO2" };
            for (int i = 0; i < 7; ++i)
                expectEquals(pages[pages.size() - 7 + (size_t) i].name, juce::String(last[i]));
            expectEquals((int) pages.size(), 19);    // +1: VOICE page (VOX cell, empty cells are legal)
            for (auto& page : pages)
            {
                const bool routers = page.section == "XIN" || page.section == "SLOT" || page.section == "MOD";
                for (auto& cell : page.params)
                    if (cell.isValid())
                        expect(R::owns(cell.id) == routers, page.name + " " + cell.label + " (" + juce::String(cell.id) + ")");
            }
            // The strip's defaults: the index to three times, a quarter-tone of vibrato.
            expectEquals(e.getParameter(R::stripDestId(0)), (float) FMVoice::LfoIndex);
            expectEquals(e.getParameter(R::stripAmountId(0)), 1.0f);
            expectEquals(e.getParameter(R::STRIP_VIBRATO), 0.25f);
        }
    }

    // ---- New-engine renders --------------------------------------------------------------------
    // One note held: `seconds` of the left side (and the right after it when stereo is asked for).
    static std::vector<float> hold(F& e, double seconds, int note = 57, bool stereo = false, float velocity = 1.0f, int block = BLOCK)
    {
        e.prepareToPlay(SR, block);
        e.noteOn(note, velocity, -1);
        return run(e, seconds, stereo, block);
    }
    static std::vector<float> run(F& e, double seconds, bool stereo = false, int block = BLOCK)
    {
        std::vector<float> l, r;
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        for (int b = 0; b < (int) (seconds * SR / block); ++b)
        {
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < block; ++i)
            {
                l.push_back(buf.getSample(0, i));
                r.push_back(buf.getSample(1, i));
            }
        }
        if (stereo)
            l.insert(l.end(), r.begin(), r.end());
        return l;
    }
    // Steady sines: the additive algorithm, operators 1 and 2 at the given ratios and levels.
    static void steady(F& e, float level1, float ratio2, float level2, int algorithm = 5)
    {
        e.setParameter(F::AlgorithmParam, (float) algorithm);
        for (int op = 0; op < 6; ++op)
        {
            e.setParameter(op * 10 + 1, op == 0 ? level1 : (op == 1 ? level2 : 0.0f));
            e.setParameter(op * 10 + 4, 0.001f);
            e.setParameter(op * 10 + 6, 1.0f);
        }
        e.setParameter(F::Op2Ratio, ratio2);
    }
    static double tone(const std::vector<float>& x, double hz, size_t from = 9600, size_t n = 24000)
    {
        return std::sqrt(fastmeasure::goertzel(x, from, n, hz, SR));
    }
    int src(const F& e, int k) { juce::ignoreUnused(e); return F::sources().placeOfPublished(k); }

    void sourceTests()
    {
        const double hz = 220.0;        // note 57

        beginTest("OP2 > PM1 is the connection [2 > 1] of an algorithm, made by a slot: the same sidebands, and none without the slot");
        {
            // Operator 2 at 3.5 x the note and level 0.5. By the algorithm [2 > 1] (2+4), and in
            // ADD with a slot OP2 > PM1 at 100: the sidebands of operator 1 at 220 +- 770 Hz.
            F byAlgorithm, bySlot, neither;
            steady(byAlgorithm, 1.0f, 3.5f, 0.5f, 6);
            steady(bySlot, 1.0f, 3.5f, 0.5f);
            steady(neither, 1.0f, 3.5f, 0.5f);
            slot(bySlot, 0, src(bySlot, 1), FMVoice::LfoPhase, 1.0f);
            const auto a = hold(byAlgorithm, 1.0), b = hold(bySlot, 1.0), c = hold(neither, 1.0);
            for (double side : { hz * 4.5, hz * 2.5, hz * 8.0 })
            {
                const double want = tone(a, side) / tone(a, hz), got = tone(b, side) / tone(b, hz), none = tone(c, side) / tone(c, hz);
                expect(want > 0.01, "the algorithm makes a sideband at " + juce::String(side) + " Hz: " + juce::String(want, 4));
                expectWithinAbsoluteError(got / want, 1.0, 0.03, "sideband at " + juce::String(side) + " Hz: " + juce::String(got, 4) + " for " + juce::String(want, 4));
                expect(none < 0.01 * want, "no sideband without the slot: " + juce::String(none, 6));
            }
            // At half the amount the first sidebands are about half as strong (a small index).
            F half;
            steady(half, 1.0f, 3.5f, 0.2f);
            slot(half, 0, src(half, 1), FMVoice::LfoPhase, 0.5f);
            F full;
            steady(full, 1.0f, 3.5f, 0.2f);
            slot(full, 0, src(full, 1), FMVoice::LfoPhase, 1.0f);
            const auto h = hold(half, 1.0), f = hold(full, 1.0);
            expectWithinAbsoluteError((tone(h, hz * 4.5) / tone(h, hz)) / (tone(f, hz * 4.5) / tone(f, hz)), 0.5, 0.03);
        }

        beginTest("A slot from each operator reaches each operator's phase and no other; PMAL reaches all six");
        {
            // Carrier c alone is heard (ADD, the others at level 0 would give no source: the
            // modulator m is heard too, at 5 x the note). OPm > PMc puts sidebands on carrier c.
            for (int m = 0; m < 6; ++m)
                for (int c = 0; c < 6; ++c)
                {
                    if (m == c)
                        continue;
                    F e;
                    e.setParameter(F::AlgorithmParam, 5.0f);
                    for (int op = 0; op < 6; ++op)
                    {
                        e.setParameter(op * 10 + 1, op == m ? 0.5f : (op == c ? 1.0f : 0.0f));
                        e.setParameter(op * 10 + 0, op == m ? 5.0f : 1.0f);
                        e.setParameter(op * 10 + 6, 1.0f);
                    }
                    slot(e, 0, src(e, m), FMVoice::LfoPhase + c, 1.0f);
                    const auto x = hold(e, 0.5);
                    const double side = tone(x, hz * 6.0, 4800, 14400) / tone(x, hz, 4800, 14400);
                    expect(side > 0.15, "OP" + juce::String(m + 1) + " > PM" + juce::String(c + 1) + ": " + juce::String(side, 4));
                    // The same slot aimed at another operator's phase (a silent one) leaves the carrier clean.
                    int other = 0;
                    while (other == m || other == c) ++other;
                    F aside;
                    aside.setParameter(F::AlgorithmParam, 5.0f);
                    for (int op = 0; op < 6; ++op)
                    {
                        aside.setParameter(op * 10 + 1, op == m ? 0.5f : (op == c ? 1.0f : 0.0f));
                        aside.setParameter(op * 10 + 0, op == m ? 5.0f : 1.0f);
                        aside.setParameter(op * 10 + 6, 1.0f);
                    }
                    slot(aside, 0, src(aside, m), FMVoice::LfoPhase + other, 1.0f);
                    const auto y = hold(aside, 0.5);
                    expect(tone(y, hz * 6.0, 4800, 14400) / tone(y, hz, 4800, 14400) < 0.002, "aimed at PM" + juce::String(other + 1) + " it leaves operator " + juce::String(c + 1) + " alone");
                }
            // PMAL: three carriers at 1, 2 and 3 x the note, each gets the sidebands of operator 6 (7 x).
            F all;
            all.setParameter(F::AlgorithmParam, 5.0f);
            for (int op = 0; op < 6; ++op)
            {
                all.setParameter(op * 10 + 1, op < 3 ? 0.5f : (op == 5 ? 0.4f : 0.0f));
                all.setParameter(op * 10 + 0, op == 5 ? 7.0f : (float) (op + 1));
                all.setParameter(op * 10 + 6, 1.0f);
            }
            slot(all, 0, src(all, 5), FMVoice::LfoPhaseAll, 1.0f);
            const auto x = hold(all, 0.5);
            for (int carrier = 1; carrier <= 3; ++carrier)
                expect(tone(x, hz * (7.0 + carrier), 4800, 14400) / tone(x, hz * carrier, 4800, 14400) > 0.1, "PMAL on carrier " + juce::String(carrier));
        }

        beginTest("An operator on the filter's cutoff, on a table position, on the level and on the pan: each is heard, and is the operator's own frequency");
        {
            // OP2 > CUT: filter FM. A sine through a low-pass whose cutoff moves at 3 x the note
            // gains sidebands 3 x the note away.
            auto base = [](F& e)
            {
                steady(e, 1.0f, 3.0f, 0.0f);
                e.setParameter(F::AlgorithmParam, 6.0f);        // [2 > 1]: operator 2 is not in the mix
                e.setParameter(F::Op1Level, 0.0f);              // ... and its carrier is silent
                e.setParameter(F::Op3Level, 1.0f);              // operator 3 is the sound
                e.setParameter(F::Op2Level, 1.0f);
                e.setParameter(F::Op3Sustain, 1.0f);
            };
            {
                F plain, moved;
                for (F* e : { &plain, &moved })
                {
                    base(*e);
                    e->setParameter(F::FilterBase + VoiceFilterSection::Cutoff, 400.0f);
                }
                slot(moved, 0, src(moved, 1), FMVoice::LfoCutoff, 0.5f);
                const auto a = hold(plain, 1.0), b = hold(moved, 1.0);
                expect(tone(a, hz * 4.0) / tone(a, hz) < 1.0e-3, "a modulator of a silent carrier is not heard: " + juce::String(tone(a, hz * 4.0) / tone(a, hz), 6));
                expect(tone(b, hz * 4.0) / tone(b, hz) > 0.05, "filter FM by operator 2: " + juce::String(tone(b, hz * 4.0) / tone(b, hz), 4));
            }
            {
                // OP2 > POS3: operator 3 reads a table, scanned by operator 2.
                F plain, moved;
                for (F* e : { &plain, &moved })
                {
                    base(*e);
                    e->setParameter(20 + F::OP_WAVE, (float) (phaseosc::FIRST_TABLE_WAVE + 9));
                    e->setParameter(20 + F::OP_POSITION, 0.5f);
                }
                slot(moved, 0, src(moved, 1), FMVoice::LfoPosition + 2, 0.4f);
                const auto a = hold(plain, 1.0), b = hold(moved, 1.0);
                double changed = 0.0;
                for (int h = 1; h <= 12; ++h)
                    changed = std::max(changed, std::abs(tone(b, hz * h) - tone(a, hz * h)) / tone(a, hz));
                expect(changed > 0.05, "a table scanned at audio rate: " + juce::String(changed, 4));
                // The position goes back to the cell's when the slot goes.
                slot(moved, 0, 0, 0, 0.0f);
                run(moved, 0.3);
                run(plain, 0.3);
                const auto a2 = run(plain, 0.5), b2 = run(moved, 0.5);
                double left = 0.0;
                for (int h = 1; h <= 12; ++h)
                    left = std::max(left, std::abs(tone(b2, hz * h, 0, 24000) - tone(a2, hz * h, 0, 24000)) / tone(a2, hz, 0, 24000));
                expect(left < 1.0e-3, "let go: the position is the cell's again (" + juce::String(left, 6) + ")");
            }
            {
                // OP2 > AMP at 100: amplitude modulation, sidebands at +-3 x the note of 0.5.
                F moved;
                base(moved);
                slot(moved, 0, src(moved, 1), FMVoice::LfoAmp, 1.0f);
                const auto b = hold(moved, 1.0);
                expectWithinAbsoluteError(tone(b, hz * 4.0) / tone(b, hz), 0.5, 0.03);
            }
            {
                // OP2 > PAN: the two sides differ, and their sum is not what either is.
                F moved;
                base(moved);
                slot(moved, 0, src(moved, 1), FMVoice::LfoPan, 1.0f);
                const auto b = hold(moved, 1.0, 57, true);
                const size_t n = b.size() / 2;
                std::vector<float> l(b.begin(), b.begin() + (long) n), r(b.begin() + (long) n, b.end()), d(n);
                for (size_t i = 0; i < n; ++i) d[i] = l[i] - r[i];
                expect(rms(d.data() + 9600, 24000) > 0.1 * rms(l.data() + 9600, 24000), "the sides differ: " + juce::String(rms(d.data() + 9600, 24000), 4));
            }
        }

        beginTest("An operator's samples are kept only while a slot reads them: no slot, nothing handed over; the slot gone, nothing again; the plain sound comes back bit for bit");
        {
            F plain, tapped;
            steady(plain, 1.0f, 2.0f, 0.5f, 6);
            steady(tapped, 1.0f, 2.0f, 0.5f, 6);
            auto a = hold(plain, 0.1), b = hold(tapped, 0.1);
            for (int k = 0; k < 6; ++k)
                expect(! tapped.getVoice(0).getMod().publishing(k), "nothing is handed over while nothing reads it");
            slot(tapped, 0, src(tapped, 2), FMVoice::LfoPhase, 1.0f);       // OP3 (silent) > PM1: read, but nothing to hear
            b = run(tapped, 0.1);
            a = run(plain, 0.1);
            expect(tapped.getVoice(0).getMod().publishing(2) && ! tapped.getVoice(0).getMod().publishing(1), "only the operator a slot reads");
            expect(a == b, "a silent operator on a phase changes nothing");
            slot(tapped, 0, src(tapped, 1), FMVoice::LfoPhase, 1.0f);       // OP2 > PM1
            b = run(tapped, 0.1);
            a = run(plain, 0.1);
            expect(tapped.getVoice(0).getMod().publishing(1), "operator 2 is handed over");
            expect(a != b, "... and heard");
            slot(tapped, 0, 0, 0, 0.0f);
            run(tapped, 0.1);
            run(plain, 0.1);
            b = run(tapped, 0.1);
            a = run(plain, 0.1);
            for (int k = 0; k < 6; ++k)
                expect(! tapped.getVoice(0).getMod().publishing(k), "the slot gone: nothing is handed over");
            expect(a == b, "the slot gone: the plain sound, bit for bit");
        }

        beginTest("Loops between operators through the slots stay bounded at every amount, at notes 0 and 127, at 48 and 96 kHz");
        {
            for (double rate : { 48000.0, 96000.0 })
                for (int note : { 0, 60, 127 })
                    for (float amount : { 1.0f, -1.0f })
                    {
                        F e;
                        e.setParameter(F::AlgorithmParam, 5.0f);
                        for (int op = 0; op < 6; ++op)
                        {
                            e.setParameter(op * 10 + 1, 1.0f);
                            e.setParameter(op * 10 + 0, (float) (op + 1));
                            e.setParameter(op * 10 + 3, 1.0f);
                            e.setParameter(op * 10 + 6, 1.0f);
                        }
                        for (int k = 0; k < 6; ++k)
                            slot(e, k, src(e, k), FMVoice::LfoPhase + (k + 1) % 6, amount);        // a ring of six
                        slot(e, 6, src(e, 0), FMVoice::LfoPhaseAll, amount);
                        slot(e, 7, src(e, 3), FMVoice::LfoPitch, amount);
                        slot(e, 8, src(e, 4), FMVoice::LfoIndex, amount);
                        slot(e, 9, src(e, 5), FMVoice::LfoFeedback, amount);
                        slot(e, 10, src(e, 2), FMVoice::LfoAmp, amount);
                        slot(e, 11, src(e, 1), FMVoice::LfoCutoff, amount);
                        e.prepareToPlay(rate, 64);
                        e.noteOn(note, 1.0f, -1);
                        float peak = 0.0f;
                        bool finite = true;
                        juce::AudioBuffer<float> buf(2, 64);
                        juce::MidiBuffer midi;
                        for (int b = 0; b < 300; ++b)
                        {
                            buf.clear();
                            e.processBlock(buf, midi);
                            for (int i = 0; i < 64; ++i)
                            {
                                finite = finite && std::isfinite(buf.getSample(0, i)) && std::isfinite(buf.getSample(1, i));
                                peak = std::max(peak, std::abs(buf.getSample(0, i)));
                            }
                        }
                        expect(finite && peak < 40.0f && peak > 0.01f, "note " + juce::String(note) + " at " + juce::String(rate) + ": peak " + juce::String(peak, 3));
                    }
        }
    }

    // Seconds until the 5 ms level of a render has fallen to `fraction` of its greatest.
    static double fallTime(const std::vector<float>& x, double fraction)
    {
        const size_t window = 240;
        double top = 0.0;
        size_t topAt = 0;
        std::vector<double> levels;
        for (size_t at = 0; at + window <= x.size(); at += window)
            levels.push_back(rms(x.data() + at, window));
        for (size_t i = 0; i < levels.size(); ++i)
            if (levels[i] > top) { top = levels[i]; topAt = i; }
        for (size_t i = topAt; i < levels.size(); ++i)
            if (levels[i] < top * fraction)
                return (double) ((i - topAt) * window) / SR;
        return 1.0e9;
    }

    void timeAndRandomTests()
    {
        beginTest("ETIM is every operator's envelope: ONE > ETIM at -100 makes each operator's decay an eighth as long, at +100 eight times, and leaves the DEC cells alone");
        {
            for (int op = 0; op < 6; ++op)
            {
                double times[3] = {};
                for (int k = 0; k < 3; ++k)
                {
                    F e;
                    e.setParameter(F::AlgorithmParam, 5.0f);
                    for (int o = 0; o < 6; ++o)
                    {
                        e.setParameter(o * 10 + 1, o == op ? 1.0f : 0.0f);
                        e.setParameter(o * 10 + 4, 0.001f);
                        e.setParameter(o * 10 + 5, 0.4f);
                        e.setParameter(o * 10 + 6, 0.0f);
                    }
                    if (k > 0)
                        slot(e, 0, 16, FMVoice::LfoEnvTime, k == 1 ? -1.0f : 1.0f);     // ONE
                    times[k] = fallTime(hold(e, k == 2 ? 4.0 : 1.0), k == 2 ? 0.7 : 0.1);
                    expectEquals(e.getParameter(op * 10 + 5), 0.4f);
                    if (k > 0)
                        expectWithinAbsoluteError(e.getVoice(0).getOp(op).getEnvelope().getTimeScale(), k == 1 ? 0.125f : 8.0f, 1.0e-4f);
                }
                F slow;     // (the reference for +100 is measured to the same level)
                slow.setParameter(F::AlgorithmParam, 5.0f);
                for (int o = 0; o < 6; ++o)
                {
                    slow.setParameter(o * 10 + 1, o == op ? 1.0f : 0.0f);
                    slow.setParameter(o * 10 + 4, 0.001f);
                    slow.setParameter(o * 10 + 5, 0.4f);
                    slow.setParameter(o * 10 + 6, 0.0f);
                }
                const double plainTo70 = fallTime(hold(slow, 1.0), 0.7);
                expectWithinAbsoluteError(times[0] / times[1], 8.0, 1.2, "operator " + juce::String(op + 1) + " at -100: " + juce::String(times[0], 4) + " s to " + juce::String(times[1], 4) + " s");
                expectWithinAbsoluteError(times[2] / plainTo70, 8.0, 1.2, "operator " + juce::String(op + 1) + " at +100: " + juce::String(plainTo70, 4) + " s to " + juce::String(times[2], 4) + " s");
            }
            // Let go, the envelopes run at their own times again.
            F e;
            slot(e, 0, 16, FMVoice::LfoEnvTime, -1.0f);
            hold(e, 0.05);
            slot(e, 0, 0, 0, 0.0f);
            run(e, 0.05);
            for (int op = 0; op < 6; ++op)
                expectEquals(e.getVoice(0).getOp(op).getEnvelope().getTimeScale(), 1.0f);
        }

        beginTest("FTIM is the filter envelope's time and no operator's; VEL > ETIM makes hard notes short");
        {
            auto cutoffAfter = [](float amount, double seconds)
            {
                F e;
                e.setParameter(F::FilterBase + VoiceFilterSection::Cutoff, 300.0f);
                e.setParameter(F::FilterBase + VoiceFilterSection::EnvAmount, 0.6f);
                e.setParameter(F::FilterBase + VoiceFilterSection::Decay, 0.4f);
                e.setParameter(F::FilterBase + VoiceFilterSection::Sustain, 0.0f);
                if (amount != 0.0f)
                    slot(e, 0, 16, FMVoice::LfoFilterTime, amount);
                hold(e, seconds);
                for (int op = 0; op < 6; ++op)
                    if (e.getVoice(0).getOp(op).getEnvelope().getTimeScale() != 1.0f)
                        return -1.0f;
                return e.getVoice(0).getFilter().getCurrentCutoff();
            };
            const float plain = cutoffAfter(0.0f, 0.1), fast = cutoffAfter(-1.0f, 0.1), slow = cutoffAfter(1.0f, 0.1);
            expect(fast > 0.0f && slow > 0.0f, "no operator's envelope is touched");
            expect(fast < 0.6f * plain && slow > 1.5f * plain, "the cutoff 0.1 s into the note: " + juce::String(fast, 1) + " / " + juce::String(plain, 1) + " / " + juce::String(slow, 1) + " Hz");

            auto lengthAt = [](float velocity)
            {
                F e;
                e.setParameter(F::Op2Level, 0.0f);
                e.setParameter(F::Op1Decay, 0.4f);
                e.setParameter(F::Op1Sustain, 0.0f);
                slot(e, 0, 5, FMVoice::LfoEnvTime, -1.0f);      // VEL
                return fallTime(hold(e, 1.0, 57, false, velocity), 0.1);
            };
            const double soft = lengthAt(0.1f), hard = lengthAt(1.0f);
            expect(hard < 0.25 * soft, "a hard note is shorter: " + juce::String(hard, 4) + " s against " + juce::String(soft, 4) + " s");
        }

        beginTest("The randoms: RAND and RND2 .. RND4 are drawn per note, differ from note to note and from one another, and repeat on a new engine");
        {
            // RANDn > PAN at 100: the share of the left side is the value drawn.
            auto places = [](int source)
            {
                F e;
                e.setParameter(F::Op2Level, 0.0f);
                e.setParameter(F::Op1Sustain, 1.0f);
                slot(e, 0, source, FMVoice::LfoPan, 1.0f);
                std::vector<double> out;
                e.prepareToPlay(SR, BLOCK);
                for (int n = 0; n < 6; ++n)
                {
                    e.noteOn(57, 1.0f, -1);
                    const auto x = run(e, 0.1, true);
                    const size_t half = x.size() / 2;
                    const double l = rms(x.data() + half / 2, half / 2), r = rms(x.data() + half + half / 2, half / 2);
                    out.push_back((l - r) / std::max(l, r));       // = -pan
                    e.allNotesOff();
                    run(e, 0.02);
                }
                return out;
            };
            const int rnd2 = F::sources().indexOfLabel("RND2");
            expect(rnd2 > 16);
            const auto a = places(9), again = places(9), b = places(rnd2), c = places(rnd2 + 1), d = places(rnd2 + 2);
            double spread = 0.0, apart = 0.0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                expectWithinAbsoluteError(a[i], again[i], 1.0e-6, "a new engine draws the same");
                spread = std::max(spread, std::abs(a[i] - a[0]));
                apart = std::max({ apart, std::abs(a[i] - b[i]), std::abs(b[i] - c[i]), std::abs(c[i] - d[i]) });
                expect(std::abs(a[i]) <= 1.0001);
            }
            expect(spread > 0.2, "RAND differs from note to note: " + juce::String(spread, 3));
            expect(apart > 0.2, "the four randoms are not one value: " + juce::String(apart, 3));
            int same = 0;
            for (size_t i = 0; i < a.size(); ++i)
                same += std::abs(a[i] - b[i]) < 1.0e-3 && std::abs(b[i] - c[i]) < 1.0e-3;
            expectEquals(same, 0);
        }
    }

    void voiceTests()
    {
        beginTest("Pressure is each voice's own: a pad pressed on one note of two moves that note alone (route VOL), and FM DEPTH is the index");
        {
            using D = AftertouchRouter::Destination;
            AftertouchRouter routes;
            routes.clear();
            routes.setRoute(0, { D::Volume, 1.0f, false });
            auto levels = [&routes](float pressLow, float pressHigh)
            {
                F e;
                e.setParameter(F::Op2Level, 0.0f);
                e.setParameter(F::Op1Sustain, 1.0f);
                e.setAftertouchRouter(&routes);
                e.prepareToPlay(SR, BLOCK);
                e.noteOn(45, 1.0f, -1);        // 110 Hz
                e.noteOn(64, 1.0f, -1);        // 329.63 Hz
                e.aftertouch(45, pressLow);
                e.aftertouch(64, pressHigh);
                const auto x = run(e, 1.0);
                e.setAftertouchRouter(nullptr);
                return std::array<double, 2> { tone(x, 110.0), tone(x, 329.6276) };
            };
            const auto rest = levels(0.0f, 0.0f), low = levels(1.0f, 0.0f), high = levels(0.0f, 0.5f);
            expectWithinAbsoluteError(low[0] / rest[0], 2.0, 0.05, "the pressed note is twice as loud");
            expectWithinAbsoluteError(low[1] / rest[1], 1.0, 0.02, "the other is as it was");
            expectWithinAbsoluteError(high[1] / rest[1], 1.5, 0.05);
            expectWithinAbsoluteError(high[0] / rest[0], 1.0, 0.02);

            // FM DEPTH at 100, the pad fully pressed: the index doubles, which is operator 2 at twice its level.
            AftertouchRouter depth;
            depth.clear();
            depth.setRoute(0, { D::FMDepth, 1.0f, false });
            F pressed, louder;
            steady(pressed, 1.0f, 2.0f, 0.4f, 6);
            steady(louder, 1.0f, 2.0f, 0.8f, 6);
            pressed.setAftertouchRouter(&depth);
            pressed.prepareToPlay(SR, BLOCK);
            pressed.noteOn(57, 1.0f, -1);
            pressed.aftertouch(57, 1.0f);
            const auto a = run(pressed, 1.0), b = hold(louder, 1.0);
            pressed.setAftertouchRouter(nullptr);
            for (int h : { 1, 3, 5 })
                expectWithinAbsoluteError(tone(a, 220.0 * h) / tone(b, 220.0 * h), 1.0, 0.02, "partial " + juce::String(h));
        }

        beginTest("A cell a slot has moved goes back to its set value when the slot goes, on sounding and on silent voices; DETN and the phases are let go of");
        {
            F plain, moved;
            for (F* e : { &plain, &moved })
                steady(*e, 1.0f, 2.0f, 0.5f, 6);
            lfo(moved, 0, 6.0f, 0.5f, 10);                  // LFO1 > LVL2
            lfo(moved, 1, 3.0f, 0.8f, FMVoice::LfoDetune);  // LFO2 > DETN
            slot(moved, 0, 16, FMVoice::LfoPhase + 1, 0.7f);    // ONE > PM2
            slot(moved, 1, 16, 15, 0.2f);                       // ONE > RAT1
            auto a = hold(plain, 0.2), b = hold(moved, 0.2);
            expect(a != b);
            moved.setParameter(159, 0.0f);
            moved.setParameter(167, 0.0f);
            slot(moved, 0, 0, 0, 0.0f);
            slot(moved, 1, 0, 0, 0.0f);
            // A new note on each (the phases restart): the same sound again.
            for (F* e : { &plain, &moved })
            {
                e->allNotesOff();
                run(*e, 0.05);
                e->noteOn(57, 1.0f, -1);
            }
            a = run(plain, 0.2);
            b = run(moved, 0.2);
            expect(a == b, "everything was let go: the plain sound, bit for bit");
        }

        beginTest("A slot taken away while its voice is silent leaves nothing behind in the next note: a page cell (the voice at rest gives it back), and a table position beside another modulation");
        {
            // which: 0 = LFO1 > LVL2 and ONE > RAT1 (cells set once a sub-block), nothing else routed;
            //        1 = ONE > POS1 (per sample) while LFO2 > AMP stays, so the voice never takes the plain path.
            for (int which = 0; which < 2; ++which)
            {
                F plain, moved;
                for (F* e : { &plain, &moved })
                {
                    steady(*e, 1.0f, 2.0f, 0.5f, 6);
                    if (which == 1)
                    {
                        e->setParameter(F::OP_WAVE, (float) (phaseosc::FIRST_TABLE_WAVE + 9));
                        e->setParameter(F::OP_POSITION, 0.5f);
                        lfo(*e, 1, 5.0f, 0.3f, FMVoice::LfoAmp);
                    }
                }
                if (which == 0)
                {
                    lfo(moved, 0, 6.0f, 0.5f, 10);
                    slot(moved, 1, 16, 15, 0.2f);
                }
                else
                    slot(moved, 0, 16, FMVoice::LfoPosition, 0.4f);
                auto a = hold(plain, 0.2), b = hold(moved, 0.2);
                expect(a != b, "the slot is heard");
                for (F* e : { &plain, &moved })
                {
                    e->allNotesOff();
                    run(*e, 0.05);
                }
                moved.setParameter(159, 0.0f);
                slot(moved, 0, 0, 0, 0.0f);
                slot(moved, 1, 0, 0, 0.0f);
                for (F* e : { &plain, &moved })
                {
                    run(*e, 0.05);
                    e->noteOn(57, 1.0f, -1);
                }
                a = run(plain, 0.2);
                b = run(moved, 0.2);
                expect(a == b, juce::String(which == 0 ? "the cells" : "the position") + ": the next note is the sound without the slot, bit for bit");
            }
        }

        beginTest("The strip's default routes: at rest nothing moves (the plain path), up it is the index x 3 and a quarter-tone of vibrato");
        {
            F rest, up, indexOnly;
            for (F* e : { &rest, &up, &indexOnly })
                steady(*e, 1.0f, 2.0f, 0.2f, 6);
            indexOnly.setParameter(F::Op2Level, 0.6f);
            up.setParameter(R::STRIP_VIBRATO, 0.0f);
            up.setModWheel(1.0f);
            run(up, 0.3);       // (the smoothed strip arrives)
            const auto a = hold(rest, 0.5), b = hold(up, 0.5), c = hold(indexOnly, 0.5);
            expect(rms(a.data(), a.size()) > 0.01);
            for (int h : { 1, 3 })
                expectWithinAbsoluteError(tone(b, 220.0 * h, 4800, 14400) / tone(c, 220.0 * h, 4800, 14400), 1.0, 0.02, "the strip up is operator 2 at three times its level");
            expect(std::abs(tone(a, 660.0, 4800, 14400) / tone(c, 660.0, 4800, 14400) - 1.0) > 0.3, "(which is not the sound at rest)");
        }
    }


    // =============================================================================================
    // The review's tests (2026-11-22): what mutations of the engine showed nothing was watching.
    static const ParamSpec* cellOf(const ParamPages& pages, int id)
    {
        for (auto& page : pages)
            for (auto& c : page.params)
                if (c.id == id)
                    return &c;
        return nullptr;
    }

    void reviewTests()
    {
        const bool print = env("NEPTUNE_U4_PRINT").isNotEmpty();

        beginTest("Old against new at 44.1 and 96 kHz: the plain sound bit for bit, modulated sounds within the measures; a one-channel buffer bit for bit");
        {
            for (double rate : { 44100.0, 96000.0 })
            {
                Gesture g;
                g.rate = rate;
                g.blocks = (int) (1.5 * rate / g.block);
                for (int variant = 0; variant < 2; ++variant)
                {
                    O oldEngine;
                    F newEngine;
                    if (variant > 0) { rich(oldEngine); rich(newEngine); }
                    const auto a = play(oldEngine, g), b = play(newEngine, g);
                    expect(rms(a.data(), a.size()) > 1.0e-3);
                    expect(a == b, "plain sound " + juce::String(variant) + " at " + juce::String(rate) + " Hz");
                }
                AftertouchRouter volume;
                volume.clear();
                volume.setRoute(0, { AftertouchRouter::Destination::Volume, 0.8f, false });
                struct Row { const char* name; std::function<void (EngineBase&)> setUp; int kind; };
                const Row rows[] = {
                    { "LFO1 > PTCH", [](EngineBase& e) { lfo(e, 0, 5.0f, 0.3f, 1); }, 0 },
                    { "LFO1 > LVL2", [](EngineBase& e) { lfo(e, 0, 4.0f, 0.4f, 10); }, 0 },
                    { "LFO1 square > AMP", [](EngineBase& e) { lfo(e, 0, 6.0f, 0.5f, 2, 3); }, 0 },
                    { "strip up, default routes", [](EngineBase&) {}, 1 },
                    { "XIN > OP1 (old cells)", [](EngineBase& e) { e.setParameter(64, 1.0f); e.setParameter(65, 0.5f); }, 2 },
                    { "pressure > VOL", [](EngineBase&) {}, 3 } };
                for (auto& row : rows)
                {
                    O oldEngine;
                    F newEngine;
                    rich(oldEngine);        rich(newEngine);
                    row.setUp(oldEngine);   row.setUp(newEngine);
                    Gesture h = g;
                    h.strip = row.kind == 1 ? 1.0f : 0.0f;
                    h.input = row.kind == 2;
                    if (row.kind == 3) { h.pressure = 0.7f; h.routes = &volume; }
                    const auto a = play(oldEngine, h), b = play(newEngine, h);
                    const Verdict v = compare(a, b, false, rate);
                    report(juce::String(row.name) + " at " + juce::String(rate / 1000.0, 1) + " kHz", v, a, b);
                    expect(v.word != "LOST", juce::String(row.name) + " at " + juce::String(rate) + " Hz: level " + juce::String(v.level, 2)
                                             + ", 20 ms " + juce::String(v.envelope, 2) + ", bands " + juce::String(v.spectrum, 2));
                }
            }
            // One channel: the mean of the two sides, as it was.
            O oldEngine;
            F newEngine;
            rich(oldEngine);
            rich(newEngine);
            lfo(oldEngine, 0, 3.0f, 0.0f, 0);
            std::vector<float> a, b;
            for (int which = 0; which < 2; ++which)
            {
                EngineBase& e = which == 0 ? static_cast<EngineBase&> (oldEngine) : static_cast<EngineBase&> (newEngine);
                e.prepareToPlay(SR, BLOCK);
                e.noteOn(57, 0.9f, -1);
                juce::AudioBuffer<float> buf(1, BLOCK);
                juce::MidiBuffer midi;
                for (int k = 0; k < 200; ++k)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int i = 0; i < BLOCK; ++i)
                        (which == 0 ? a : b).push_back(buf.getSample(0, i));
                }
            }
            expect(rms(a.data(), a.size()) > 1.0e-3 && a == b, "a one-channel buffer");
        }

        beginTest("Stored cells keep their meaning: every LFO, STRIP and slot cell of the old engine is there at the same id, with the same range and default, and its choices are the old list at the same places (a slot's DEST: the first 75; its own twenty follow the table)");
        {
            O oldEngine;
            F newEngine;
            const auto before = oldEngine.getParamPages(), after = newEngine.getParamPages();
            int checked = 0;
            for (auto& page : before)
                for (auto& c : page.params)
                {
                    if (! c.isValid() || ! R::owns(c.id))
                        continue;
                    const ParamSpec* n = cellOf(after, c.id);
                    expect(n != nullptr, "cell " + juce::String(c.id) + " (" + c.label + ") is still there");
                    if (n == nullptr)
                        continue;
                    ++checked;
                    const juce::String what = juce::String(c.label) + " (" + juce::String(c.id) + ")";
                    expectEquals(juce::String(n->label), juce::String(c.label), what);
                    expect(n->kind == c.kind, what + ": kind");
                    expectEquals(n->minVal, c.minVal, what + ": least");
                    expectEquals(n->defaultVal, c.defaultVal, what + ": default");
                    expectEquals(newEngine.getParameter(c.id), oldEngine.getParameter(c.id), what + ": value on a new engine");
                    if (c.kind != ParamKind::Choice)
                    {
                        expectEquals(n->maxVal, c.maxVal, what + ": greatest");
                        continue;
                    }
                    expect(n->numChoices >= c.numChoices, what + ": the list only grew");
                    const bool slotDest = juce::String(c.label) == "DEST" && c.id >= 700;
                    const int alike = slotDest ? 75 : c.numChoices;
                    for (int i = 0; i < alike && i < n->numChoices; ++i)
                        expectEquals(juce::String(n->choices[i]), juce::String(c.choices[i]), what + " choice " + juce::String(i));
                    if (slotDest)
                    {
                        // L1RT L1DP L2RT L2DP AMT1 .. AM16: nine places further on, in the same order.
                        expectEquals(c.numChoices, 95);
                        expectEquals(n->numChoices, 104 + mod::TrackDest::COUNT);      // (and the track's effect cells: E4)
                        for (int i = 75; i < c.numChoices; ++i)
                            expectEquals(juce::String(n->choices[i + 9]), juce::String(c.choices[i]), what + " choice " + juce::String(i) + " moved by nine");
                    }
                }
            expect(checked >= 24 + 4 + 8, "the LFO, STRIP and slot cells were looked at: " + juce::String(checked));
        }

        beginTest("The old cells 64 / 65: the input as it is (FLT1 back to THRU), XDPT 50 until set, nothing read back, on no page; a XIN-page cell set after them wins (a stored sound is loaded in the order of its ids)");
        {
            F e;
            e.setParameter(R::xinId(R::XinFilter1), 1.0f);
            e.setParameter(64, 3.0f);
            expectEquals(e.getParameter(R::xinId(R::XinDest1)), (float) (FMVoice::LfoPhase + 2));
            expectEquals(e.getParameter(R::xinId(R::XinFilter1)), 0.0f);
            expectEquals(e.getParameter(R::xinId(R::XinAmount1)), 0.5f);
            e.setParameter(65, 0.8f);
            expectEquals(e.getParameter(R::xinId(R::XinAmount1)), 0.8f);
            expectEquals(e.getParameter(64), 0.0f);
            expectEquals(e.getParameter(65), 0.0f);
            for (auto& page : e.getParamPages())
                for (auto& c : page.params)
                    expect(c.id != 64 && c.id != 65);
            // A sound saved since the router holds the old cells (they stay in the track's map) and
            // the XIN page's: std::map order, so 64 and 65 first, then 182 ..
            ParamMap stored;
            stored[64] = 0.0f;
            stored[65] = 0.5f;
            stored[R::xinId(R::XinDest1)] = (float) FMVoice::LfoCutoff;
            stored[R::xinId(R::XinAmount1)] = -0.3f;
            stored[R::xinId(R::XinFilter1)] = 2.0f;
            F loaded;
            for (auto& [id, value] : stored)
                loaded.setParameter(id, value);
            expectEquals(loaded.getParameter(R::xinId(R::XinDest1)), (float) FMVoice::LfoCutoff);
            expectEquals(loaded.getParameter(R::xinId(R::XinAmount1)), -0.3f);
            expectEquals(loaded.getParameter(R::xinId(R::XinFilter1)), 2.0f);
            expect(64 < R::xinId(R::XinDest1) && 65 < R::xinId(R::XinAmount1));
        }

        beginTest("What a voice was left holding when its note ended is not in its next note: the cutoff and the pitch (per sample), alone and beside another modulation that stays");
        {
            for (int what = 0; what < 2; ++what)
                for (int beside = 0; beside < 2; ++beside)
                    for (int ending = 0; ending < 2; ++ending)
                    {
                        F plain, moved;
                        for (F* e : { &plain, &moved })
                        {
                            steady(*e, 1.0f, 2.0f, 0.5f, 6);
                            e->setParameter(F::FilterBase + VoiceFilterSection::Cutoff, 900.0f);
                            if (beside == 1)
                                lfo(*e, 1, 5.0f, 0.3f, FMVoice::LfoAmp);
                        }
                        lfo(moved, 0, 3.3f, 0.6f, what == 0 ? FMVoice::LfoCutoff : FMVoice::LfoPitch);
                        auto a = hold(plain, 0.23), b = hold(moved, 0.23);
                        expect(a != b, "the LFO is heard");
                        for (F* e : { &plain, &moved })
                        {
                            if (ending == 0) { e->allNotesOff(); run(*e, 0.05); }
                            else             { e->noteOff(57, -1); run(*e, 1.5); }
                            expect(! e->getVoice(0).isActive());
                        }
                        moved.setParameter(165, 0.0f);          // LFO1's DEST: OFF
                        for (F* e : { &plain, &moved })
                        {
                            run(*e, 0.05);
                            e->noteOn(57, 1.0f, -1);
                        }
                        a = run(plain, 0.2);
                        b = run(moved, 0.2);
                        expect(a == b, juce::String(what == 0 ? "the cutoff" : "the pitch") + (beside == 1 ? " beside an LFO on the level" : " alone")
                                       + (ending == 0 ? ", the note cut" : ", the note released") + ": the next note is the sound without it, bit for bit");
                    }
        }

        beginTest("The pad's routes PAN and VIBRATO act on this engine (they did nothing before the router)");
        {
            using D = AftertouchRouter::Destination;
            auto sides = [](D dest, float pressure)
            {
                AftertouchRouter routes;
                routes.clear();
                routes.setRoute(0, { dest, 1.0f, false });
                F e;
                steady(e, 1.0f, 2.0f, 0.0f);
                e.setAftertouchRouter(&routes);
                e.prepareToPlay(SR, BLOCK);
                e.noteOn(57, 1.0f, -1);
                e.aftertouch(57, pressure);
                const auto x = run(e, 0.6, true);
                e.setAftertouchRouter(nullptr);
                const size_t n = x.size() / 2;
                const std::vector<float> l(x.begin(), x.begin() + (long) n);
                return std::array<double, 3> { rms(x.data() + 9600, 14400), rms(x.data() + n + 9600, 14400), tone(l, 220.0, 9600, 14400) };
            };
            const auto rest = sides(D::PanSpread, 0.0f), pan = sides(D::PanSpread, 1.0f), vib = sides(D::Vibrato, 1.0f);
            expectWithinAbsoluteError(rest[0] / rest[1], 1.0, 1.0e-6);
            expect(pan[0] < 0.05 * pan[1] && pan[1] > 0.9 * rest[1], "the pad fully pressed, PAN at 100: the left side is gone (" + juce::String(pan[0], 4) + " / " + juce::String(pan[1], 4) + ")");
            expect(vib[2] < 0.9 * rest[2] && vib[0] > 0.9 * rest[0], "VIBRATO: the tone is no longer one line (" + juce::String(vib[2] / rest[2], 3) + " of it at 220 Hz)");
        }

        beginTest("A stolen voice starts its taps from silence: nothing of the note before is handed over; one voice, a loop through a slot, stays finite");
        {
            F e(1);
            steady(e, 1.0f, 3.5f, 0.8f);
            slot(e, 0, src(e, 1), FMVoice::LfoPhase, 1.0f);
            slot(e, 1, src(e, 0), FMVoice::LfoPhase + 1, 1.0f);
            const auto a = hold(e, 0.2);
            expect(e.getVoice(0).getMod().publishing(1) && e.getVoice(0).getMod().publishing(0));
            e.noteOn(64, 1.0f, -1);                         // the one voice is taken
            for (int k = 0; k < 6; ++k)
                expect(! e.getVoice(0).getMod().publishing(k), "the new note has nothing of the old one's operator " + juce::String(k + 1));
            const auto b = run(e, 0.2);
            expect(e.getVoice(0).getMod().publishing(1));
            bool finite = true;
            for (float x : b) finite = finite && std::isfinite(x) && std::abs(x) < 8.0f;
            expect(finite && rms(b.data(), b.size()) > 0.01);

            // A slot that reads an operator again under a sounding note hears nothing of the time
            // it read it before.
            {
                F twice, once;
                for (F* f : { &twice, &once })
                    steady(*f, 1.0f, 3.5f, 0.8f);
                // (Host blocks of 5 samples: every sub-block is shorter than the 16 the taps reach back.)
                slot(twice, 0, src(twice, 1), FMVoice::LfoPhase, 1.0f);
                hold(twice, 0.1, 57, false, 1.0f, 5);
                hold(once, 0.1, 57, false, 1.0f, 5);
                slot(twice, 0, 0, 0, 0.0f);
                run(twice, 0.3, false, 5);
                run(once, 0.3, false, 5);
                slot(twice, 0, src(twice, 1), FMVoice::LfoPhase, 1.0f);
                slot(once, 0, src(once, 1), FMVoice::LfoPhase, 1.0f);
                const auto x = run(twice, 0.1, false, 5), y = run(once, 0.1, false, 5);
                expect(x == y, "read again: as if read for the first time");
            }
            // How late: one sub-block, 16 samples at 48 kHz, at any host block size. Operator 2 at
            // the note's own frequency on operator 1's phase: sin(wt + I sin(w (t - d))) has a
            // constant part of -(I / 2) sin(w d) to first order; a model with the same delay gives
            // it exactly (note 66: 16 samples are an eighth of its period).
            for (int block : { 64, 100 })
            {
                F late;
                steady(late, 1.0f, 1.0f, 0.5f);         // ADD: operators 1 and 2 at the note, at 1 and 0.5
                slot(late, 0, src(late, 1), FMVoice::LfoPhase, 1.0f);
                const auto heard = hold(late, 1.0, 66, false, 1.0f, block);
                const double f = 440.0 * std::pow(2.0, (66 - 69) / 12.0), w = 6.283185307179586 * f / SR;
                auto constantPart = [f](const std::vector<float>& x)
                {
                    double sum = 0.0;
                    for (size_t i = 9600; i < 9600 + 24000; ++i)
                        sum += x[i];
                    return (sum / 24000.0) / tone(x, f);
                };
                auto model = [&](int delay)
                {
                    std::vector<float> x(heard.size());
                    for (size_t n = 0; n < x.size(); ++n)
                        x[n] = (float) (std::sin(w * (double) n + 0.5 * std::sin(w * ((double) n - delay))) + 0.5 * std::sin(w * (double) n));
                    return constantPart(x);
                };
                const double got = constantPart(heard), want = model(16), sooner = model(0), later = model(32);
                if (print)
                    logMessage("U4FM review | OP2 > PM1 at blocks of " + juce::String(block) + ", the constant part: " + juce::String(got, 5) + "; a model 16 samples late "
                               + juce::String(want, 5) + ", 0 late " + juce::String(sooner, 5) + ", 32 late " + juce::String(later, 5));
                expect(std::abs(later - want) > 0.2 * std::abs(want) && std::abs(sooner - want) > 0.2 * std::abs(want), "(the measure tells the delays apart)");
                expectWithinAbsoluteError(got / want, 1.0, 0.03, "blocks of " + juce::String(block) + ": the operator is heard 16 samples late");
            }


            // A note after a note that was cut is the note on an engine that never played the
            // first, bit for bit: the operators' last samples of the first are not in its slots.
            F again(1), first(1);
            for (F* f : { &again, &first })
            {
                steady(*f, 1.0f, 3.5f, 0.8f);
                slot(*f, 0, src(*f, 1), FMVoice::LfoPhase, 1.0f);
                f->prepareToPlay(SR, BLOCK);
            }
            again.noteOn(57, 1.0f, -1);
            run(again, 0.2);
            run(first, 0.2);
            again.allNotesOff();
            run(again, 0.1);
            run(first, 0.1);
            again.noteOn(64, 1.0f, -1);
            first.noteOn(64, 1.0f, -1);
            const auto x = run(again, 0.1), y = run(first, 0.1);
            expect(rms(x.data(), x.size()) > 0.01 && x == y, "the second note is a first note");
        }

        beginTest("A table scanned by an operator is scanned at the operator's speed (the position the router gives is not slowed by the operator's own 3 ms glide), and OPn > PMn at host blocks that are not whole sub-blocks");
        {
            const double hz = 220.0;
            // Operator 3 reads a table and is scanned by operator 2 at 3 x the note.
            F plain, moved;
            for (F* e : { &plain, &moved })
            {
                steady(*e, 1.0f, 3.0f, 0.0f);
                e->setParameter(F::AlgorithmParam, 6.0f);
                e->setParameter(F::Op1Level, 0.0f);
                e->setParameter(F::Op3Level, 1.0f);
                e->setParameter(F::Op2Level, 1.0f);
                e->setParameter(F::Op3Sustain, 1.0f);
                e->setParameter(20 + F::OP_WAVE, (float) (phaseosc::FIRST_TABLE_WAVE + 9));
                e->setParameter(20 + F::OP_POSITION, 0.5f);
            }
            slot(moved, 0, src(moved, 1), FMVoice::LfoPosition + 2, 0.4f);
            const auto a = hold(plain, 1.0), b = hold(moved, 1.0);
            double changed = 0.0;
            for (int h = 1; h <= 12; ++h)
                changed = std::max(changed, std::abs(tone(b, hz * h) - tone(a, hz * h)) / tone(a, hz));
            expect(changed > 0.05, "the table is scanned: " + juce::String(changed, 4));
            // The operator itself: a position the router gives is read at once (the router has led
            // it), where a set POS is moved to over 3 ms.
            {
                phaseosc::Wave wave;
                wave.table = phaseosc::waveTable(phaseosc::FIRST_TABLE_WAVE + 9, nullptr);
                expect(wave.hasPosition());
                FMOperator jumped, there, set;
                for (FMOperator* op : { &jumped, &there, &set })
                {
                    op->setSampleRate(SR);
                    op->setWave(wave);
                    op->getEnvelope().setParams({ 0.001f, 0.1f, 1.0f, 0.1f });
                    op->noteOn(57, 1.0f);
                }
                bool sameAtOnce = true, setIsLed = false, differsBefore = false;
                for (int i = 0; i < 400; ++i)
                {
                    jumped.drivePosition(i < 200 ? 0.0f : 1.0f);
                    there.drivePosition(1.0f);
                    if (i == 200) set.setPosition(1.0f);
                    const float x = jumped.process(), y = there.process(), z = set.process();
                    if (i < 200)  differsBefore = differsBefore || x != y;
                    else          sameAtOnce = sameAtOnce && x == y;
                    if (i >= 200 && i < 230) setIsLed = setIsLed || z != y;
                }
                expect(differsBefore, "the table differs along its positions");
                expect(sameAtOnce, "a driven position is where the operator reads, from the same sample");
                expect(setIsLed, "(a set position is moved to)");
            }

            // OP2 > PM1 against the algorithm's own [2 > 1], at several host block sizes.
            double strayAtWhole = 0.0;
            for (int block : { 64, 16, 100, 37, 1 })
            {
                F byAlgorithm, bySlot;
                steady(byAlgorithm, 1.0f, 3.5f, 0.5f, 6);
                steady(bySlot, 1.0f, 3.5f, 0.5f);
                slot(bySlot, 0, src(bySlot, 1), FMVoice::LfoPhase, 1.0f);
                const auto x = hold(byAlgorithm, 1.0, 57, false, 1.0f, block), y = hold(bySlot, 1.0, 57, false, 1.0f, block);
                double worst = 0.0;
                for (double side : { hz * 4.5, hz * 2.5, hz * 8.0 })
                    worst = std::max(worst, std::abs((tone(y, side) / tone(y, hz)) / (tone(x, side) / tone(x, hz)) - 1.0));
                // What is not a partial of the sound (they are all multiples of 110 Hz): between two of them.
                double stray = 0.0;
                for (double f : { 165.0, 495.0, 1265.0, 2035.0, 3465.0 })
                    stray = std::max(stray, tone(y, f) / tone(y, hz));
                if (print)
                    logMessage("U4FM review | OP2 > PM1 at blocks of " + juce::String(block) + ": sidebands off by " + juce::String(100.0 * worst, 2)
                               + " %, strays " + juce::String(db(stray), 1) + " dB under the carrier");
                if (block == 64)
                    strayAtWhole = stray;
                expect(worst < 0.03, "blocks of " + juce::String(block) + ": the sidebands are the algorithm's within 3 % (" + juce::String(100.0 * worst, 2) + " %)");
                expect(stray < 2.0 * strayAtWhole + 1.0e-6, "blocks of " + juce::String(block) + ": nothing between the partials (" + juce::String(db(stray), 1)
                                                            + " dB under the carrier, " + juce::String(db(strayAtWhole), 1) + " at blocks of 64)");
            }
        }

        if (print)
        {
            // RAND > PAN was reported LOST "because the draws differ": the same row, new against new
            // with other seeds, and old against old (two engines).
            auto setUp = [](EngineBase& e) { rich(e); slot(e, 0, 9, 3, 0.9f); };
            F n0, n1;
            O o0, o1;
            setUp(n0); setUp(n1); setUp(o0); setUp(o1);
            for (int v = 0; v < 8; ++v)
                n1.getVoice(v).getMod().seed(7, v);
            const auto a = play(n0, Gesture()), b = play(n1, Gesture()), c = play(o0, Gesture()), d = play(o1, Gesture());
            const Verdict nn = compare(a, b, true), oo = compare(c, d, true), on = compare(c, a, true);
            for (auto& [name, v] : { std::pair<const char*, Verdict> { "new seed 0 / new seed 7", nn }, { "old / old (another engine)", oo }, { "old / new", on } })
                logMessage(juce::String("U4FM review | RAND > PAN, ") + name + ": " + v.word + ", level " + juce::String(v.level, 2) + " dB, 20 ms "
                           + juce::String(v.envelope, 2) + " dB, bands " + juce::String(v.spectrum, 2) + " dB");
        }
    }

    // =============================================================================================
    // What the engine costs, old against new: eight voices held, 512-sample blocks, 48 kHz, the
    // share of real time on one core, the best of five runs, old and new alternately.
    struct BenchCase
    {
        const char* name;
        std::function<void (EngineBase&)> setUp;
        bool input = false;
        float strip = 0.0f;
        bool pressure = false;
        bool newOnly = false;
    };

    template <typename Engine>
    static double cost(const BenchCase& c)
    {
        constexpr int SIZE = 512, BLOCKS = 940;        // 10 s
        Engine e(8);
        e.prepareToPlay(SR, SIZE);
        rich(e);
        c.setUp(e);
        e.setModWheel(c.strip);
        static const int notes[8] = { 48, 52, 55, 59, 62, 65, 69, 72 };
        for (int n : notes)
            e.noteOn(n, 0.8f, -1);
        if (c.pressure)
            for (int n : notes)
                e.aftertouch(n, 0.6f);
        juce::AudioBuffer<float> buf(2, SIZE);
        juce::MidiBuffer midi;
        std::vector<float> in((size_t) SIZE);
        double phase = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < BLOCKS; ++b)
        {
            if (c.input)
            {
                for (auto& s : in)
                {
                    s = 0.7f * (float) std::sin(phase);
                    phase += 2.0 * 3.141592653589793 * 330.0 / SR;
                }
                e.setAudioInput({ in.data(), in.data(), SIZE, 0.5f });
            }
            buf.clear();
            e.processBlock(buf, midi);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return 100.0 * seconds / ((double) SIZE * BLOCKS / SR);
    }

    void bench()
    {
        beginTest("Cost: old against new");
        std::vector<BenchCase> cases;
        cases.push_back({ "idle (nothing routed)", [](EngineBase&) {} });
        cases.push_back({ "idle, the default sound", [](EngineBase& e) { e.setParameter(F::AlgorithmParam, 0.0f); e.setParameter(F::Op3Level, 0.0f); } });
        cases.push_back({ "input connected, nothing routed", [](EngineBase&) {}, true });
        cases.push_back({ "pads pressed, default route (VOL)", [](EngineBase&) {}, false, 0.0f, true });
        cases.push_back({ "strip up, default routes", [](EngineBase&) {}, false, 1.0f });
        cases.push_back({ "LFO1 > PTCH", [](EngineBase& e) { lfo(e, 0, 5.0f, 0.3f, 1); } });
        cases.push_back({ "LFO1 > INDX, LFO2 > CUT", [](EngineBase& e) { lfo(e, 0, 5.0f, 0.3f, 6); lfo(e, 1, 0.7f, 0.4f, 4); } });
        cases.push_back({ "LFO1 > LVL2 (a page cell)", [](EngineBase& e) { lfo(e, 0, 5.0f, 0.3f, 10); } });
        cases.push_back({ "four slots (ENV, PRES, LFO via STRP, RAND)", [](EngineBase& e)
        {
            slot(e, 0, 3, 6, 0.5f); slot(e, 1, 6, 4, 0.4f); slot(e, 2, 1, 10, 0.6f, 7); slot(e, 3, 9, 3, 0.8f);
        }, false, 0.8f, true });
        cases.push_back({ "XIN > OP1 (old cells 64 / 65)", [](EngineBase& e) { e.setParameter(64, 1.0f); e.setParameter(65, 0.8f); }, true });
        cases.push_back({ "XIN > ALL", [](EngineBase& e) { e.setParameter(64, 7.0f); e.setParameter(65, 0.8f); }, true });
        cases.push_back({ "OP2 > PM1 (a slot)", [](EngineBase& e) { slot(e, 0, F::sources().placeOfPublished(1), FMVoice::LfoPhase, 0.8f); }, false, 0.0f, false, true });
        cases.push_back({ "OP3 > CUT, OP2 > PM3", [](EngineBase& e)
        {
            slot(e, 0, F::sources().placeOfPublished(2), FMVoice::LfoCutoff, 0.4f);
            slot(e, 1, F::sources().placeOfPublished(1), FMVoice::LfoPhase + 2, 0.8f);
        }, false, 0.0f, false, true });
        cases.push_back({ "ONE > ETIM, RND2 > PAN", [](EngineBase& e)
        {
            slot(e, 0, 16, FMVoice::LfoEnvTime, 0.3f);
            slot(e, 1, F::sources().indexOfLabel("RND2"), FMVoice::LfoPan, 0.8f);
        }, false, 0.0f, false, true });
        for (auto& c : cases)
        {
            double oldBest = 1.0e9, newBest = 1.0e9;
            for (int runIndex = 0; runIndex < 5; ++runIndex)
            {
                if (! c.newOnly)
                    oldBest = std::min(oldBest, cost<O>(c));
                newBest = std::min(newBest, cost<F>(c));
            }
            logMessage(juce::String("U4FM bench | ") + juce::String(c.name).paddedRight(' ', 44) + " | old "
                       + (c.newOnly ? juce::String("  -  ") : juce::String(oldBest, 3)) + " % | new " + juce::String(newBest, 3) + " %"
                       + (c.newOnly ? juce::String() : " | " + juce::String(100.0 * (newBest / oldBest - 1.0), 1) + " %"));
        }
        expect(true);
    }
};

static U4FmTests u4FmTests;
