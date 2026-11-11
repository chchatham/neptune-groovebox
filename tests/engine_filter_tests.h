#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <complex>
#include <functional>
#include <memory>
#include <set>
#include <vector>
#include "../src/engine/FMEngine.h"
#include "../src/engine/MacroOscEngine.h"
#include "../src/engine/WavetableEngine.h"
#include "../src/engine/WaveSeqEngine.h"
#include "../src/engine/VoiceFilterSection.h"

// The per-voice filter of the FM, macro-oscillator, wavetable and wave-sequencing engines,
// measured on rendered audio (through each engine's own processBlock).
class EngineFilterTests : public juce::UnitTest
{
public:
    EngineFilterTests() : UnitTest("Engine Filters") {}

    using F = VoiceFilterSection;
    static constexpr double SR = 44100.0;

    struct Rig
    {
        const char* name;
        int base;                                              // first filter parameter id
        std::function<std::unique_ptr<EngineBase>()> make;     // a steady, harmonically rich sound
        std::function<float(EngineBase&, int)> voiceCutoff;    // cutoff the voice's filter is running at
    };

    static std::vector<Rig> rigs()
    {
        std::vector<Rig> r;
        r.push_back({ "FM", FMEngine::FilterBase,
            [] {
                auto e = std::make_unique<FMEngine>();
                // Six carriers on spread-out harmonics: steady and bright.
                static const float ratios[6] = { 1.0f, 3.0f, 6.0f, 9.0f, 12.0f, 16.0f };
                e->setParameter(FMEngine::AlgorithmParam, 5.0f);
                for (int op = 0; op < 6; ++op)
                {
                    e->setParameter(op * 10 + 0, ratios[op]);
                    e->setParameter(op * 10 + 1, 1.0f);
                    e->setParameter(op * 10 + 6, 1.0f);
                }
                return std::unique_ptr<EngineBase>(std::move(e));
            },
            [](EngineBase& e, int v) { return static_cast<FMEngine&>(e).getVoice(v).getFilter().getCurrentCutoff(); } });
        r.push_back({ "Macro", MacroOscEngine::FilterBase,
            [] {
                auto e = std::make_unique<MacroOscEngine>();
                e->setParameter(MacroOscEngine::TimbreParam, 0.0f);    // a single saw
                e->setParameter(MacroOscEngine::AmpSustain, 1.0f);
                return std::unique_ptr<EngineBase>(std::move(e));
            },
            [](EngineBase& e, int v) { return static_cast<MacroOscEngine&>(e).getVoice(v).getFilter().getCurrentCutoff(); } });
        r.push_back({ "Wavetable", WavetableEngine::FilterBase,
            [] {
                auto e = std::make_unique<WavetableEngine>();
                e->setParameter(WavetableEngine::Position, 1.0f);      // the bright end of the saw table
                e->setParameter(WavetableEngine::AmpSustain, 1.0f);
                return std::unique_ptr<EngineBase>(std::move(e));
            },
            [](EngineBase& e, int v) { return static_cast<WavetableEngine&>(e).getVoice(v).getFilter().getCurrentCutoff(); } });
        r.push_back({ "WaveSeq", WaveSeqEngine::FilterBase,
            [] {
                auto e = std::make_unique<WaveSeqEngine>();
                // One step, a saw at the played pitch: a steady tone.
                e->setParameter(WaveSeqEngine::SeqLength, 1.0f);
                e->setParameter(WaveSeqEngine::TimingLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::PitchLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::ShapeLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::LevelLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::StepPitch, 0.0f);
                e->setParameter(WaveSeqEngine::StepShape, 1.0f);
                e->setParameter(WaveSeqEngine::StepLevel, 1.0f);
                e->setParameter(WaveSeqEngine::Crossfade, 0.0f);
                e->setParameter(WaveSeqEngine::AmpSustain, 1.0f);
                return std::unique_ptr<EngineBase>(std::move(e));
            },
            [](EngineBase& e, int v) { return static_cast<WaveSeqEngine&>(e).getVoice(v).getFilter().getCurrentCutoff(); } });
        return r;
    }

    struct NoteEvent { int note; float velocity; };

    // Renders `seconds` of the engine playing the notes (all started at time 0) through processBlock.
    static std::vector<float> render(EngineBase& e, std::vector<NoteEvent> notes, double seconds, float wheel = -1.0f)
    {
        const int block = 256;
        e.prepareToPlay(SR, block);
        if (wheel >= 0.0f)
            e.modWheel(wheel);
        for (auto& n : notes)
            e.noteOn(n.note, n.velocity);

        std::vector<float> out;
        const int total = (int) (seconds * SR);
        out.reserve((size_t) total + block);
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        while ((int) out.size() < total)
        {
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < block; ++i)
                out.push_back(buf.getSample(0, i));
        }
        return out;
    }

    static void fft(std::vector<std::complex<float>>& a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const float ang = -2.0f * juce::MathConstants<float>::pi / (float) len;
            const std::complex<float> wl(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<float> w(1.0f, 0.0f);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    auto u = a[i + k];
                    auto v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    // Energy between two frequencies in a Hann-windowed stretch of n samples (n a power of two).
    static float bandEnergy(const std::vector<float>& x, double startSeconds, int n, float loHz, float hiHz)
    {
        const size_t start = (size_t) (startSeconds * SR);
        std::vector<std::complex<float>> a((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * (float) i / (float) n);
            const size_t idx = start + (size_t) i;
            a[(size_t) i] = (idx < x.size() ? x[idx] : 0.0f) * w;
        }
        fft(a);
        const float binHz = (float) SR / (float) n;
        float e = 0.0f;
        for (int k = 1; k < n / 2; ++k)
        {
            const float f = (float) k * binHz;
            if (f >= loHz && f < hiHz)
                e += std::norm(a[(size_t) k]);
        }
        return e;
    }

    static float rms(const std::vector<float>& x, double fromSeconds = 0.0)
    {
        double s = 0.0;
        size_t n = 0;
        for (size_t i = (size_t) (fromSeconds * SR); i < x.size(); ++i, ++n)
            s += (double) x[i] * x[i];
        return n > 0 ? (float) std::sqrt(s / (double) n) : 0.0f;
    }

    static float rmsDifference(const std::vector<float>& a, const std::vector<float>& b)
    {
        double s = 0.0;
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i)
            s += (double) (a[i] - b[i]) * (a[i] - b[i]);
        return n > 0 ? (float) std::sqrt(s / (double) n) : 0.0f;
    }

    static bool allFinite(const std::vector<float>& x, float limit = 20.0f)
    {
        for (float v : x)
            if (! std::isfinite(v) || std::abs(v) > limit)
                return false;
        return true;
    }

    // Share of the energy above 2 kHz.
    static float brightness(const std::vector<float>& x, double startSeconds, int n = 4096)
    {
        const float total = bandEnergy(x, startSeconds, n, 20.0f, 22000.0f);
        return total > 0.0f ? bandEnergy(x, startSeconds, n, 2000.0f, 22000.0f) / total : 0.0f;
    }

    void runTest() override
    {
        beginTest("Pitch bend moves the pitch of every sound engine by its BEND range");
        {
            // Rising zero crossings per second of a held note = its pitch (the sound is made plain first).
            auto pitchOf = [](EngineType type, float bendAmount, bool bendWhileHeld)
            {
                auto e = AudioEngine::createEngine(type);
                e->prepareToPlay(44100.0, 64);
                for (auto& page : e->getParamPages())
                    for (auto& spec : page.params)
                    {
                        const juce::String label(spec.label);
                        if (label == "CUT") e->setParameter(spec.id, spec.maxVal);
                        if (label == "DRFT" || label == "SLOP" || label == "SOFT" || label == "WSHP" || label == "DET2") e->setParameter(spec.id, 0.0f);
                        if (label == "MIX") e->setParameter(spec.id, 0.0f);
                        if (label == "WAV1") e->setParameter(spec.id, 0.0f);       // sine
                        if (label == "SUS") e->setParameter(spec.id, 1.0f);
                    }
                if (!bendWhileHeld) e->pitchBend(bendAmount);
                e->noteOn(57, 1.0f, -1);                                            // 220 Hz
                juce::MidiBuffer midi;
                int crossings = 0;
                float last = 0.0f;
                const int blocks = 44100 / 64;
                for (int b = 0; b < blocks + 80; ++b)
                {
                    if (bendWhileHeld && b == 40) e->pitchBend(bendAmount);
                    juce::AudioBuffer<float> buf(2, 64);
                    buf.clear();
                    e->processBlock(buf, midi);
                    if (b < 80) continue;                                           // settle, then one second
                    for (int i = 0; i < 64; ++i)
                    {
                        const float v = buf.getSample(0, i);
                        if (last <= 0.0f && v > 0.0f) ++crossings;
                        last = v;
                    }
                }
                return (double) crossings;
            };
            struct Named { EngineType type; const char* name; };
            for (auto n : { Named { EngineType::UnifiedVoice, "synth" }, Named { EngineType::FM, "FM" }, Named { EngineType::MacroOsc, "macro" },
                            Named { EngineType::Wavetable, "wavetable" }, Named { EngineType::WaveSeq, "wave seq" } })
            {
                const double flat = pitchOf(n.type, 0.0f, false);
                const double up = pitchOf(n.type, 1.0f, false), upHeld = pitchOf(n.type, 1.0f, true), down = pitchOf(n.type, -1.0f, true);
                const juce::String info = juce::String(n.name) + ": " + juce::String(flat, 0) + " Hz flat, " + juce::String(up, 0) + " up before the note, "
                                        + juce::String(upHeld, 0) + " up while held, " + juce::String(down, 0) + " down";
                expect(flat > 20.0, info);
                // Default BEND is 2 semitones: x 1.122 up, x 0.891 down.
                expect(up > flat * 1.08 && up < flat * 1.17, info);
                expect(upHeld > flat * 1.08 && upHeld < flat * 1.17, info);
                expect(down < flat * 0.93 && down > flat * 0.85, info);
                logMessage(info);
            }

            // The sampler plays a drum loop by default, so its pitch cannot be counted: a bend up
            // plays the same sample faster (the same hits arrive earlier), which changes the output.
            auto samplerRender = [](float bendAmount)
            {
                auto e = AudioEngine::createEngine(EngineType::Sampler);
                e->prepareToPlay(44100.0, 64);
                e->pitchBend(bendAmount);
                e->noteOn(60, 1.0f, -1);
                juce::MidiBuffer midi;
                std::vector<float> out;
                for (int b = 0; b < 300; ++b)
                {
                    juce::AudioBuffer<float> buf(2, 64);
                    buf.clear();
                    e->processBlock(buf, midi);
                    out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 64);
                }
                return out;
            };
            const auto flat = samplerRender(0.0f), bent = samplerRender(1.0f);
            double diff = 0.0, level = 0.0;
            for (size_t i = 0; i < flat.size(); ++i) { diff += std::pow((double) flat[i] - bent[i], 2.0); level += (double) flat[i] * flat[i]; }
            expect(level > 0.0 && diff > 0.05 * level, "Sampler: a bend changes the playback speed");

            // Drums: the kit's kick (pad 10, a plain sine sweep) is higher with the strip up.
            auto kickCrossings = [](float bendAmount)
            {
                DrumEngine drums;
                drums.prepareToPlay(44100.0, 64);
                drums.pitchBend(bendAmount);
                drums.noteOn(9, 1.0f);
                juce::MidiBuffer midi;
                int crossings = 0;
                float last = 0.0f;
                for (int b = 0; b < 300; ++b)
                {
                    juce::AudioBuffer<float> buf(2, 64);
                    buf.clear();
                    drums.processBlock(buf, midi);
                    for (int i = 0; i < 64; ++i)
                    {
                        const float v = buf.getSample(0, i) + buf.getSample(1, i);
                        if (last <= 0.0f && v > 0.0f) ++crossings;
                        last = v;
                    }
                }
                return crossings;
            };
            const int kickFlat = kickCrossings(0.0f), kickUp = kickCrossings(1.0f), kickDown = kickCrossings(-1.0f);
            expect(kickUp > kickFlat && kickDown < kickFlat, "Drums bend: " + juce::String(kickDown) + " / " + juce::String(kickFlat) + " / " + juce::String(kickUp));
        }

        const auto all = rigs();
        const int offType = FilterSlot::indexFromModel(FilterModel::Bypass);

        beginTest("pages: eight working cells each, ids round-trip, filter ids clear of other ranges");
        for (auto& rig : all)
        {
            auto e = rig.make();
            const auto pages = e->getParamPages();
            std::set<int> ids;
            int filterCells = 0;
            bool sawFiltr = false, sawFenv = false;
            for (auto& page : pages)
            {
                expect(page.name.length() <= 5, juce::String(rig.name) + " page name " + page.name);
                sawFiltr = sawFiltr || page.name == "FILTR";
                sawFenv = sawFenv || page.name == "F ENV";
                for (auto& p : page.params)
                {
                    const juce::String where = juce::String(rig.name) + " " + page.name + " " + juce::String(p.label);
                    expect(p.isValid(), where + " is an empty cell");
                    if (! p.isValid())
                        continue;
                    const juce::String label(p.label);
                    expect(label.isNotEmpty() && label.length() <= 4 && label == label.toUpperCase(), where + " label");
                    expect(EngineParamRange::contains(p.id), where + " id in the engine ranges (0-149, 158-189)");
                    expect(p.maxVal > p.minVal, where + " range");
                    if (p.kind == ParamKind::Choice)
                        expect(p.choices != nullptr && p.numChoices == (int) p.maxVal + 1, where + " choices");

                    const bool isFilter = p.id >= rig.base && p.id < rig.base + (int) F::Count;
                    if (isFilter)
                    {
                        ++filterCells;
                        expect(ids.count(p.id) == 0, where + " filter id used twice");
                        expect(page.name == "FILTR" || page.name == "F ENV", where + " on a filter page");
                        expectWithinAbsoluteError(e->getParameter(p.id), p.defaultVal, 1.0e-6f, where + " default seeded");
                    }
                    else
                        expect(p.id < rig.base || p.id >= rig.base + (int) F::Count, where + " inside the filter ids");
                    ids.insert(p.id);

                    // Set / get round trip at both ends and back at the starting value.
                    const float before = e->getParameter(p.id);
                    for (float v : { p.minVal, p.maxVal, p.fromNorm(0.5f), before })
                    {
                        e->setParameter(p.id, v);
                        expectWithinAbsoluteError(e->getParameter(p.id), v, 1.0e-4f * juce::jmax(1.0f, std::abs(v)),
                                                  where + " round trip");
                    }
                }
            }
            expect(sawFiltr && sawFenv, juce::String(rig.name) + " has FILTR and F ENV pages");
            expectEquals(filterCells, (int) F::Count, juce::String(rig.name) + " filter cells");
            // Clear of the shared modulation ids (40-89 and 130-145) with room on both sides.
            expect(rig.base >= 100 && rig.base + (int) F::Count <= 120, juce::String(rig.name) + " filter id range");
        }

        beginTest("MODE follows TYPE");
        for (auto& rig : all)
        {
            auto e = rig.make();
            const int type = rig.base + F::Type, mode = rig.base + F::Mode;
            e->setParameter(type, (float) FilterSlot::indexFromModel(FilterModel::Ladder));
            e->setParameter(mode, 5.0f);
            expectEquals((int) e->getParameter(mode), 5);
            const ParamSpec* spec = nullptr;
            auto pages = e->getParamPages();
            for (auto& page : pages)
                if (auto* s = page.find(mode))
                    spec = s;
            expect(spec != nullptr && spec->numChoices == 6 && juce::String(spec->choices[5]) == "HP");

            e->setParameter(type, (float) FilterSlot::indexFromModel(FilterModel::SEM));
            expectEquals((int) e->getParameter(mode), 1, "mode clamped to the new type");
            pages = e->getParamPages();
            for (auto& page : pages)
                if (auto* s = page.find(mode))
                    expect(s->numChoices == 2 && (int) s->maxVal == 1);
            e->setParameter(mode, 4.0f);
            expectEquals((int) e->getParameter(mode), 1, "mode limited to the type's modes");
        }

        beginTest("defaults leave the sound as it was (filter off vs default)");
        for (auto& rig : all)
        {
            auto def = rig.make();
            auto off = rig.make();
            off->setParameter(rig.base + F::Type, (float) offType);
            const auto a = render(*def, { { 60, 0.8f } }, 0.5);
            const auto b = render(*off, { { 60, 0.8f } }, 0.5);
            const float ra = rms(a, 0.1), rb = rms(b, 0.1);
            expect(rb > 0.01f, juce::String(rig.name) + " makes sound");
            expectWithinAbsoluteError(ra / rb, 1.0f, 0.03f, juce::String(rig.name) + " default level");
            // The audible band is untouched.
            const float ea = bandEnergy(a, 0.1, 8192, 20.0f, 8000.0f), eb = bandEnergy(b, 0.1, 8192, 20.0f, 8000.0f);
            expectWithinAbsoluteError(ea / eb, 1.0f, 0.03f, juce::String(rig.name) + " default spectrum below 8 kHz");
        }

        beginTest("closing the cutoff removes the highs");
        for (auto& rig : all)
        {
            auto open = rig.make();
            auto closed = rig.make();
            closed->setParameter(rig.base + F::Cutoff, 300.0f);
            const auto a = render(*open, { { 60, 0.8f } }, 0.5);
            const auto b = render(*closed, { { 60, 0.8f } }, 0.5);
            const float hiOpen = bandEnergy(a, 0.1, 8192, 2000.0f, 20000.0f);
            const float hiClosed = bandEnergy(b, 0.1, 8192, 2000.0f, 20000.0f);
            expect(hiOpen > 0.0f && hiClosed < hiOpen * 0.01f,
                   juce::String(rig.name) + " highs " + juce::String(hiClosed / hiOpen, 5));
            // The fundamental (262 Hz) survives.
            expect(bandEnergy(b, 0.1, 8192, 200.0f, 330.0f) > bandEnergy(a, 0.1, 8192, 200.0f, 330.0f) * 0.2f,
                   juce::String(rig.name) + " fundamental kept");
            expect(allFinite(b));
        }

        beginTest("resonance raises the level around the cutoff");
        for (auto& rig : all)
        {
            auto flat = rig.make();
            auto peaky = rig.make();
            for (auto* e : { flat.get(), peaky.get() })
                e->setParameter(rig.base + F::Cutoff, 1570.0f);     // the sixth harmonic of note 60
            peaky->setParameter(rig.base + F::Resonance, 0.8f);
            const auto a = render(*flat, { { 60, 0.8f } }, 0.5);
            const auto b = render(*peaky, { { 60, 0.8f } }, 0.5);
            const float nearFlat = bandEnergy(a, 0.1, 8192, 1200.0f, 1900.0f);
            const float nearPeaky = bandEnergy(b, 0.1, 8192, 1200.0f, 1900.0f);
            expect(nearFlat > 0.0f && nearPeaky > nearFlat * 4.0f,
                   juce::String(rig.name) + " resonance gain " + juce::String(nearPeaky / nearFlat, 2));
            expect(allFinite(b));
        }

        beginTest("envelope amount sweeps the spectrum over the note");
        for (auto& rig : all)
        {
            auto makeSwept = [&](float amount)
            {
                auto e = rig.make();
                e->setParameter(rig.base + F::Cutoff, 200.0f);
                e->setParameter(rig.base + F::EnvAmount, amount);
                e->setParameter(rig.base + F::Attack, 0.001f);
                e->setParameter(rig.base + F::Decay, 0.3f);
                e->setParameter(rig.base + F::Sustain, 0.0f);
                return e;
            };
            auto still = makeSwept(0.0f);
            auto swept = makeSwept(1.0f);
            const auto a = render(*still, { { 60, 0.8f } }, 1.0);
            const auto b = render(*swept, { { 60, 0.8f } }, 1.0);
            const float early = brightness(b, 0.005, 2048), late = brightness(b, 0.7, 2048);
            expect(early > late * 20.0f + 0.01f, juce::String(rig.name) + " swept " + juce::String(early, 4) + " -> " + juce::String(late, 5));
            const float earlyStill = brightness(a, 0.005, 2048);
            expect(early > earlyStill * 10.0f, juce::String(rig.name) + " without amount the start stays dark");

            // Negative amount: starts closed below the cutoff and opens to it.
            auto inverted = rig.make();
            inverted->setParameter(rig.base + F::Cutoff, 4000.0f);
            inverted->setParameter(rig.base + F::EnvAmount, -1.0f);
            inverted->setParameter(rig.base + F::Attack, 0.001f);
            inverted->setParameter(rig.base + F::Decay, 0.3f);
            inverted->setParameter(rig.base + F::Sustain, 0.0f);
            const auto c = render(*inverted, { { 60, 0.8f } }, 1.0);
            expect(brightness(c, 0.7, 2048) > brightness(c, 0.005, 2048) * 5.0f, juce::String(rig.name) + " negative amount");
        }

        beginTest("key tracking: two notes at once are filtered independently (per voice)");
        for (auto& rig : all)
        {
            auto make = [&](float track)
            {
                auto e = rig.make();
                e->setParameter(rig.base + F::Cutoff, 400.0f);
                e->setParameter(rig.base + F::KeyTrack, track);
                return e;
            };
            const NoteEvent low { 36, 0.8f }, high { 84, 0.8f };

            // Both notes in one engine: each voice runs its own cutoff, two octaves either side.
            auto both = make(1.0f);
            const auto together = render(*both, { low, high }, 0.4);
            float cutLow = rig.voiceCutoff(*both, 0), cutHigh = rig.voiceCutoff(*both, 1);
            if (cutLow > cutHigh)
                std::swap(cutLow, cutHigh);
            expectWithinAbsoluteError(cutLow, 100.0f, 1.0f, juce::String(rig.name) + " low voice cutoff");
            expectWithinAbsoluteError(cutHigh, 1600.0f, 16.0f, juce::String(rig.name) + " high voice cutoff");

            // ... and the result is exactly the two notes filtered separately.
            auto onlyLow = make(1.0f);
            auto onlyHigh = make(1.0f);
            const auto lowAlone = render(*onlyLow, { low }, 0.4);
            const auto highAlone = render(*onlyHigh, { high }, 0.4);
            std::vector<float> sum(together.size());
            for (size_t i = 0; i < sum.size(); ++i)
                sum[i] = lowAlone[i] + highAlone[i];
            expect(rmsDifference(together, sum) < rms(together) * 0.01f,
                   juce::String(rig.name) + " chord = sum of separately filtered notes");

            // Tracking opens the high note and closes the low one, compared with no tracking.
            auto flatHigh = make(0.0f);
            auto flatLow = make(0.0f);
            const auto highFlat = render(*flatHigh, { high }, 0.4);
            const auto lowFlat = render(*flatLow, { low }, 0.4);
            const float highTracked = bandEnergy(highAlone, 0.1, 8192, 900.0f, 4000.0f);
            const float highUntracked = bandEnergy(highFlat, 0.1, 8192, 900.0f, 4000.0f);
            expect(highTracked > highUntracked * 4.0f,
                   juce::String(rig.name) + " high note opened " + juce::String(highTracked / highUntracked, 2));
            const float lowTracked = bandEnergy(lowAlone, 0.1, 8192, 300.0f, 4000.0f);
            const float lowUntracked = bandEnergy(lowFlat, 0.1, 8192, 300.0f, 4000.0f);
            expect(lowTracked < lowUntracked * 0.25f,
                   juce::String(rig.name) + " low note closed " + juce::String(lowTracked / lowUntracked, 3));
        }

        beginTest("velocity, envelope velocity and the mod strip move the cutoff");
        for (auto& rig : all)
        {
            auto dark = [&]
            {
                auto e = rig.make();
                e->setParameter(rig.base + F::Cutoff, 300.0f);
                return e;
            };
            const float base = brightness(render(*dark(), { { 60, 1.0f } }, 0.4), 0.1);

            auto vel = dark();
            vel->setParameter(rig.base + F::VelAmount, 1.0f);
            expect(brightness(render(*vel, { { 60, 1.0f } }, 0.4), 0.1) > base * 10.0f + 0.001f, juce::String(rig.name) + " VEL hard");
            auto velSoft = dark();
            velSoft->setParameter(rig.base + F::VelAmount, 1.0f);
            const float soft = brightness(render(*velSoft, { { 60, 0.2f } }, 0.4), 0.1);
            auto velHard = dark();
            velHard->setParameter(rig.base + F::VelAmount, 1.0f);
            expect(brightness(render(*velHard, { { 60, 1.0f } }, 0.4), 0.1) > soft * 3.0f, juce::String(rig.name) + " VEL soft vs hard");

            auto wheelUp = dark();
            wheelUp->setParameter(rig.base + F::WheelAmount, 1.0f);
            expect(brightness(render(*wheelUp, { { 60, 1.0f } }, 0.4, 1.0f), 0.1) > base * 10.0f + 0.001f, juce::String(rig.name) + " WHL up");
            auto wheelRest = dark();
            wheelRest->setParameter(rig.base + F::WheelAmount, 1.0f);
            expectWithinAbsoluteError(brightness(render(*wheelRest, { { 60, 1.0f } }, 0.4, 0.0f), 0.1), base, base * 0.05f + 1.0e-5f,
                                      juce::String(rig.name) + " WHL at rest");
            auto wheelDown = rig.make();
            wheelDown->setParameter(rig.base + F::Cutoff, 6000.0f);
            auto wheelDownRef = rig.make();
            wheelDownRef->setParameter(rig.base + F::Cutoff, 6000.0f);
            wheelDown->setParameter(rig.base + F::WheelAmount, -1.0f);
            expect(brightness(render(*wheelDown, { { 60, 1.0f } }, 0.4, 1.0f), 0.1)
                       < brightness(render(*wheelDownRef, { { 60, 1.0f } }, 0.4, 1.0f), 0.1) * 0.1f,
                   juce::String(rig.name) + " WHL negative");

            // EVEL: a soft note gets less envelope sweep.
            auto sweep = [&](float evel)
            {
                auto e = dark();
                e->setParameter(rig.base + F::EnvAmount, 0.6f);
                e->setParameter(rig.base + F::Sustain, 1.0f);
                e->setParameter(rig.base + F::EnvVelocity, evel);
                return brightness(render(*e, { { 60, 0.2f } }, 0.4), 0.1);
            };
            const float full = sweep(0.0f), scaled = sweep(1.0f);
            expect(full > scaled * 3.0f, juce::String(rig.name) + " EVEL " + juce::String(full, 4) + " vs " + juce::String(scaled, 5));
        }

        beginTest("envelope stages and curve are applied");
        for (auto& rig : all)
        {
            auto sweep = [&](int offset, float value, double at)
            {
                auto e = rig.make();
                e->setParameter(rig.base + F::Cutoff, 200.0f);
                e->setParameter(rig.base + F::EnvAmount, 1.0f);
                e->setParameter(rig.base + F::Attack, 0.001f);
                e->setParameter(rig.base + F::Decay, 0.4f);
                e->setParameter(rig.base + F::Sustain, 0.0f);
                e->setParameter(rig.base + offset, value);
                return brightness(render(*e, { { 60, 0.8f } }, 1.0), at, 2048);
            };
            // Slow attack: still dark just after the start.
            expect(sweep(F::Attack, 0.001f, 0.005) > sweep(F::Attack, 0.8f, 0.005) * 5.0f, juce::String(rig.name) + " ATK");
            // Long decay: still bright at 300 ms.
            expect(sweep(F::Decay, 3.0f, 0.3) > sweep(F::Decay, 0.1f, 0.3) * 5.0f, juce::String(rig.name) + " DEC");
            // Sustain: stays open.
            expect(sweep(F::Sustain, 1.0f, 0.8) > sweep(F::Sustain, 0.0f, 0.8) * 5.0f, juce::String(rig.name) + " SUS");
            // Curve bends the decay: mid-decay brightness differs clearly between the two extremes.
            const float curveA = sweep(F::Curve, -1.0f, 0.15), curveB = sweep(F::Curve, 1.0f, 0.15);
            expect(juce::jmax(curveA, curveB) > juce::jmin(curveA, curveB) * 1.5f,
                   juce::String(rig.name) + " CURV " + juce::String(curveA, 4) + " / " + juce::String(curveB, 4));

            // Release: after note-off the cutoff falls over the release time.
            auto released = [&](float rel)
            {
                auto e = rig.make();
                e->setParameter(rig.base + F::Cutoff, 200.0f);
                e->setParameter(rig.base + F::EnvAmount, 1.0f);
                e->setParameter(rig.base + F::Sustain, 1.0f);
                e->setParameter(rig.base + F::Release, rel);
                render(*e, { { 60, 0.8f } }, 0.2);
                e->noteOff(60);
                juce::AudioBuffer<float> buf(2, 256);
                juce::MidiBuffer midi;
                for (int i = 0; i < 8; ++i)      // about 46 ms into the release
                {
                    buf.clear();
                    e->processBlock(buf, midi);
                }
                return rig.voiceCutoff(*e, 0);
            };
            const float shortRel = released(0.01f), longRel = released(5.0f);
            expect(longRel > 10000.0f && shortRel < 250.0f,
                   juce::String(rig.name) + " REL " + juce::String(shortRel, 0) + " / " + juce::String(longRel, 0));
        }

        beginTest("every type and mode filters, stays bounded; drive and morph act");
        for (auto& rig : all)
        {
            auto bypass = rig.make();
            bypass->setParameter(rig.base + F::Type, (float) offType);
            const auto ref = render(*bypass, { { 60, 0.8f } }, 0.3);

            for (int type = 0; type < FilterSlot::NUM_MODELS; ++type)
            {
                const auto model = FilterSlot::modelFromIndex(type);
                if (model == FilterModel::Bypass)
                    continue;
                for (int mode = 0; mode < FilterSlot::modeCount(model); ++mode)
                {
                    auto e = rig.make();
                    e->setParameter(rig.base + F::Type, (float) type);
                    e->setParameter(rig.base + F::Mode, (float) mode);
                    e->setParameter(rig.base + F::Cutoff, 700.0f);
                    e->setParameter(rig.base + F::Resonance, 0.6f);
                    e->setParameter(rig.base + F::Drive, 0.5f);
                    const auto out = render(*e, { { 60, 0.8f } }, 0.3);
                    const juce::String where = juce::String(rig.name) + " " + FilterSlot::modelNames()[type] + " "
                                             + FilterSlot::modeNames(model)[mode];
                    expect(allFinite(out), where + " bounded");
                    expect(rms(out, 0.05) > 1.0e-4f, where + " passes signal");
                    expect(rmsDifference(out, ref) > rms(ref) * 0.1f, where + " changes the sound");
                }
            }

            // SVF high-pass removes the fundamental that the low-pass keeps.
            auto hp = rig.make();
            hp->setParameter(rig.base + F::Mode, 2.0f);
            hp->setParameter(rig.base + F::Cutoff, 3000.0f);
            const auto hpOut = render(*hp, { { 60, 0.8f } }, 0.3);
            expect(bandEnergy(hpOut, 0.1, 4096, 200.0f, 330.0f) < bandEnergy(ref, 0.1, 4096, 200.0f, 330.0f) * 0.01f,
                   juce::String(rig.name) + " HP mode");

            // Drive on the ladder changes the output.
            auto ladder = [&](float drive)
            {
                auto e = rig.make();
                e->setParameter(rig.base + F::Type, (float) FilterSlot::indexFromModel(FilterModel::Ladder));
                e->setParameter(rig.base + F::Cutoff, 2000.0f);
                e->setParameter(rig.base + F::Drive, drive);
                return render(*e, { { 60, 0.8f } }, 0.3);
            };
            const auto clean = ladder(0.0f), driven = ladder(1.0f);
            expect(rmsDifference(clean, driven) > rms(clean) * 0.02f, juce::String(rig.name) + " DRIV");

            // Morph on the SEM: low-pass at 0, high-pass at 1.
            auto sem = [&](float morph)
            {
                auto e = rig.make();
                e->setParameter(rig.base + F::Type, (float) FilterSlot::indexFromModel(FilterModel::SEM));
                e->setParameter(rig.base + F::Cutoff, 1500.0f);
                e->setParameter(rig.base + F::Morph, morph);
                return brightness(render(*e, { { 60, 0.8f } }, 0.3), 0.1);
            };
            expect(sem(1.0f) > sem(0.0f) * 3.0f, juce::String(rig.name) + " MORP");
        }

        beginTest("a cutoff jump is smoothed, not stepped");
        {
            VoiceFilter f;
            f.setSampleRate(SR);
            f.setCutoff(200.0f);
            f.noteOn(60, 1.0f, false);
            f.process(0.0f);
            expectWithinAbsoluteError(f.getCurrentCutoff(), 200.0f, 0.5f);
            f.setCutoff(8000.0f);
            f.process(0.0f);
            expect(f.getCurrentCutoff() < 400.0f, "one sample after the jump");
            for (int i = 0; i < 4410; ++i)
                f.process(0.0f);
            expectWithinAbsoluteError(f.getCurrentCutoff(), 8000.0f, 8.0f, "settled after 100 ms");
        }
    }
};
static EngineFilterTests EngineFilterTestsInstance;
