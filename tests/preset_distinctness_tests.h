#pragma once

// Distinctness: every factory preset the PRESET screen offers, rendered (three notes each) and
// described by what the ear goes by - how its level runs over the note, where its energy lies
// by octave, and the same two things for its first tenth of a second, so that a tick of a few
// milliseconds (a clave, a rim shot) is seen as well as a pad. NO TWO PRESETS OF A CATEGORY MAY
// BE THE SAME SOUND: the suite fails on a pair closer than MIN_DISTANCE and prints the nearest
// pairs.
//
// The distance of two presets is the largest difference of any one feature, in dB, over the
// features that either of them has above the floor (presetaudition::featureDistance).
//
// One rule beside the threshold, with its reason: the INIT presets of two DIFFERENT engines are
// not compared. "Init FM" and "Init WT" are each the bare default of their engine - what the
// player picks to start from that engine with nothing set - and two engines that both start
// from one plain wave are close by that alone (0.6 dB on 2026-11-22). They are not two versions
// of a sound: loading one or the other puts another machine on the track. Within one engine
// INIT presets are compared like any others.

#include "../src/project/FactoryPresets.h"
#include "../src/project/presets/FusionPresets.h"
#include "../src/project/presets/ResonatorPresets.h"
#include "../src/project/PresetManager.h"
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include "preset_audition_tests.h"
#include <juce_core/juce_core.h>
#include <vector>
#include <map>
#include <cmath>

class PresetDistinctnessTests : public juce::UnitTest
{
public:
    PresetDistinctnessTests() : UnitTest("Preset Distinctness") {}

    static constexpr float MIN_DISTANCE = 1.0f;     // dB: closer than this is the same sound twice (the suite fails)
    static constexpr float CLOSE = 2.0f;            // dB: printed as close

    struct PresetFeatures
    {
        juce::String name;
        juce::String category;
        EngineType engine;
        std::vector<float> features;
    };

    // One note of 0.6 s and 0.3 s of its release: the level in twelve windows of 75 ms and the
    // energy in ten octave bands (as presetaudition::renderFeatures has them), then the level in
    // eight windows of 10 ms from the note's start and the ten bands of its first 100 ms.
    static std::vector<float> featuresOf(EngineBase& e, int note)
    {
        const double sr = 48000.0;
        const int block = 64;
        std::vector<float> mono;
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        auto run = [&](double seconds)
        {
            for (int b = 0; b < (int) (seconds * sr) / block; ++b)
            {
                buf.clear();
                e.processBlock(buf, midi);
                for (int i = 0; i < block; ++i)
                    mono.push_back(0.5f * (buf.getSample(0, i) + buf.getSample(1, i)));
            }
        };
        e.noteOn(note, 0.8f);
        run(0.6);
        e.noteOff(note);
        run(0.3);
        e.allNotesOff();
        run(0.3);
        mono.resize((size_t) (0.9 * sr));

        std::vector<float> f;
        auto db = [](double power) { return (float) (10.0 * std::log10(power + 1.0e-7)); };
        auto levels = [&](size_t from, size_t length, int windows)
        {
            const size_t win = length / (size_t) windows;
            for (int w = 0; w < windows; ++w)
            {
                double sum = 0.0;
                for (size_t i = from + (size_t) w * win; i < from + (size_t) (w + 1) * win; ++i)
                    sum += (double) mono[i] * mono[i];
                f.push_back(db(sum / (double) win));
            }
        };
        auto bands = [&](size_t length)
        {
            double total[10] = {}, lp[10] = {};
            for (size_t n = 0; n < length; ++n)
            {
                const double x = mono[n];
                double prev = x;
                for (int b = 9; b >= 0; --b)
                {
                    const double fc = 31.25 * std::pow(2.0, b);
                    const double a = 1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi * fc / sr);
                    lp[b] += a * (x - lp[b]);
                    const double band = prev - lp[b];
                    total[b] += band * band;
                    prev = lp[b];
                }
            }
            for (double t : total)
                f.push_back(db(t / (double) length));
        };
        levels(0, mono.size(), 12);
        bands(mono.size());
        levels(0, (size_t) (0.08 * sr), 8);
        bands((size_t) (0.1 * sr));
        return f;
    }

    static PresetFeatures measurePreset(const Preset& preset)
    {
        static constexpr int NOTES[] = { 48, 57, 69 };
        PresetFeatures pf;
        pf.name = preset.name;
        pf.category = preset.category;
        pf.engine = preset.engineType;
        int notes = 0;
        for (int note : NOTES)
        {
            SequencerCore seq;
            auto audio = presetaudition::freshAudio(seq, preset);
            auto* engine = audio->getEngine(0);
            if (engine == nullptr)
                continue;
            const auto f = featuresOf(*engine, note);
            if (pf.features.empty())
                pf.features.assign(f.size(), 0.0f);
            for (size_t i = 0; i < f.size(); ++i)
                pf.features[i] += f[i];
            ++notes;
        }
        for (auto& f : pf.features)
            f /= (float) std::max(1, notes);
        return pf;
    }

    static float distance(const PresetFeatures& a, const PresetFeatures& b)
    {
        return presetaudition::featureDistance(a.features, b.features);
    }

    // Not compared (see the top of the file): the bare defaults of two different engines.
    static bool exempt(const PresetFeatures& a, const PresetFeatures& b)
    {
        return a.category == "INIT" && b.category == "INIT" && a.engine != b.engine;
    }

    void runTest() override
    {
        beginTest("No two factory presets of a category are the same sound");
        PresetManager pm;
        std::vector<PresetFeatures> measured;
        for (int i = 0; i < pm.getPresetCount(); ++i)
            if (pm.getPreset(i).isFactory)
            {
                auto pf = measurePreset(pm.getPreset(i));
                if (! pf.features.empty())
                    measured.push_back(std::move(pf));
            }
        logMessage("Rendered " + juce::String((int) measured.size()) + " factory presets");
        expect(measured.size() >= 190, "every factory preset");

        // A preset that gives nothing at all cannot be told from another that gives nothing.
        for (auto& m : measured)
        {
            float top = -200.0f;
            for (float f : m.features) top = std::max(top, f);
            expect(top > -55.0f, m.name + ": nothing above the floor in any feature (silent on the keys?)");
        }

        std::map<juce::String, std::vector<size_t>> byCategory;
        for (size_t i = 0; i < measured.size(); ++i)
            byCategory[measured[i].category].push_back(i);

        struct Pair { size_t a, b; float dist; };
        std::vector<Pair> pairs;
        for (auto& [category, indices] : byCategory)
        {
            Pair nearest { 0, 0, 1.0e9f };
            for (size_t i = 0; i < indices.size(); ++i)
                for (size_t j = i + 1; j < indices.size(); ++j)
                {
                    const auto& a = measured[indices[i]];
                    const auto& b = measured[indices[j]];
                    if (exempt(a, b))
                        continue;
                    const float d = distance(a, b);
                    pairs.push_back({ indices[i], indices[j], d });
                    if (d < nearest.dist)
                        nearest = { indices[i], indices[j], d };
                    expect(d >= MIN_DISTANCE, category + ": " + a.name + " (" + engineName(a.engine) + ") and " + b.name + " (" + engineName(b.engine)
                                              + ") are " + juce::String(d, 2) + " dB apart: the same sound twice");
                }
            if (nearest.dist < 1.0e8f)
                logMessage(category.paddedRight(' ', 6) + " nearest pair: " + measured[nearest.a].name + " / " + measured[nearest.b].name
                           + "  " + juce::String(nearest.dist, 1) + " dB" + (nearest.dist < CLOSE ? "  (close)" : ""));
        }

        beginTest("The nearest pairs of all");
        std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.dist < y.dist; });
        for (size_t k = 0; k < std::min<size_t>(pairs.size(), 12); ++k)
            logMessage(juce::String((int) k + 1) + ". " + measured[pairs[k].a].name + " / " + measured[pairs[k].b].name
                       + " [" + measured[pairs[k].a].category + "]  " + juce::String(pairs[k].dist, 1) + " dB");
        // The pairs the rule at the top leaves out, so that they are seen.
        for (size_t i = 0; i < measured.size(); ++i)
            for (size_t j = i + 1; j < measured.size(); ++j)
                if (exempt(measured[i], measured[j]) && distance(measured[i], measured[j]) < CLOSE)
                    logMessage("not compared (two engines' bare defaults): " + measured[i].name + " / " + measured[j].name + "  "
                               + juce::String(distance(measured[i], measured[j]), 1) + " dB");
        expect(true);
    }

private:
    static juce::String engineName(EngineType t)
    {
        switch (t)
        {
            case EngineType::UnifiedVoice: return "UV";
            case EngineType::FM: return "FM";
            case EngineType::MacroOsc: return "MO";
            case EngineType::Wavetable: return "WT";
            case EngineType::WaveSeq: return "WS";
            case EngineType::Sampler: return "SM";
            case EngineType::Drum: return "DR";
            case EngineType::Resonator: return "RSN";
            default: return "?";
        }
    }
};

static PresetDistinctnessTests presetDistinctnessTests;
