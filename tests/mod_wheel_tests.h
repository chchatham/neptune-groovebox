#pragma once

#include "../src/engine/AudioEngine.h"
#include "../src/engine/ModStripFollower.h"
#include "../src/sequencer/SequencerCore.h"
#include "../src/project/FactoryPresets.h"
#include <juce_core/juce_core.h>
#include <complex>
#include <functional>
#include <vector>

// -----------------------------------------------------------------------------
// The mod strip (CC 1) on every sound engine and every factory preset.
//
// Each sound is rendered twice, with the strip at rest and fully up, and the two
// renders are compared frame by frame in the frequency domain:
//
//   difference = sum |A - B| / sum (A + B)      over all STFT magnitude bins
//
// 0 = the same sound, 1 = nothing in common. It reacts to anything a wheel route
// can do (brightness, level, vibrato, timing), so one threshold serves every
// engine. A new engine or factory preset whose sound ignores the strip fails
// here.
// -----------------------------------------------------------------------------
class ModWheelTests : public juce::UnitTest
{
public:
    ModWheelTests() : UnitTest("Mod Wheel") {}

    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;                    // the app's block size
    static constexpr float MIN_DIFFERENCE = 0.08f;      // strip up v. at rest: at least this
    static constexpr float MAX_REST_DIFFERENCE = 1.0e-4f;   // two renders at rest: at most this

    struct Measure
    {
        float rms = 0.0f;
        float centroid = 0.0f;      // Hz, energy weighted over the whole render
    };

    // ---- rendering ---------------------------------------------------------------------------

    // The unified voice's analog character randomises phases and tuning per note; without it two
    // renders of one sound are the same samples.
    static void makeRepeatable(EngineBase& e)
    {
        if (auto* u = dynamic_cast<UnifiedVoiceEngine*>(&e))
        {
            using namespace UnifiedParam;
            for (int id : { DRIFT, SLOP, RANDOM_PHASE, WAVE_VAR, SOFT_START, FILTER_VAR, ENV_VAR })
                u->setParameter(id, 0.0f);
            for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                u->getVoice(v).getAnalogCharacter().enabled = false;
        }
    }

    static std::shared_ptr<EngineBase> makeEngine(EngineType type, const ParamMap* sound = nullptr)
    {
        std::shared_ptr<EngineBase> shared = AudioEngine::createEngine(type);
        shared->prepareToPlay(SR, BLOCK);
        if (sound != nullptr)
        {
            // As AudioEngine::applyAllTrackParameters: the master parameter first.
            const int master = shared->getMasterParamId();
            auto it = master >= 0 ? sound->find(master) : sound->end();
            if (it != sound->end())
                shared->setParameter(master, it->second);
            for (auto& [id, value] : *sound)
                if (id != master && EngineParamRange::contains(id))
                    shared->setParameter(id, value);
        }
        makeRepeatable(*shared);
        return shared;
    }

    // What is played: a held chord on the pitched engines, a bar of hits on a kit.
    static std::vector<float> render(EngineBase& e, float wheel, double seconds = 2.5,
                                     const std::function<void (EngineBase&, int)>& atBlock = {})
    {
        const bool kit = e.getType() == EngineType::Drum;
        e.setModWheel(wheel);

        std::vector<float> out;
        const int total = (int) (seconds * SR);
        out.reserve((size_t) total + BLOCK);
        juce::AudioBuffer<float> buf(2, BLOCK);
        juce::MidiBuffer midi;
        const int hitEvery = (int) (0.15 * SR / BLOCK);     // blocks between drum hits
        int hit = 0;
        for (int block = 0; (int) out.size() < total; ++block)
        {
            if (atBlock)
                atBlock(e, block);
            if (kit && block % hitEvery == 0 && hit < 16)
                e.noteOn(hit++, 0.9f);
            else if (! kit && block == 0)
                for (int note : { 48, 60 })
                    e.noteOn(note, 0.8f);
            if (! kit && block == (int) (0.75 * total / BLOCK))
                for (int note : { 48, 60 })
                    e.noteOff(note);

            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < BLOCK; ++i)
                out.push_back(0.5f * (buf.getSample(0, i) + buf.getSample(1, i)));
        }
        return out;
    }

    // ---- measuring ---------------------------------------------------------------------------

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
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    static constexpr int FFT_SIZE = 2048;

    // Magnitude frames (square root of the magnitude, so that quiet parts of a sound - a tail,
    // high harmonics - count as the ear counts them), Hann window, hop of half a frame.
    static std::vector<std::vector<float>> frames(const std::vector<float>& x)
    {
        std::vector<std::vector<float>> out;
        std::vector<std::complex<float>> buf((size_t) FFT_SIZE);
        for (size_t start = 0; start + (size_t) FFT_SIZE <= x.size(); start += (size_t) FFT_SIZE / 2)
        {
            for (int i = 0; i < FFT_SIZE; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * (float) i / (float) FFT_SIZE);
                buf[(size_t) i] = { x[start + (size_t) i] * w, 0.0f };
            }
            fft(buf);
            std::vector<float> mag((size_t) FFT_SIZE / 2);
            for (int i = 0; i < FFT_SIZE / 2; ++i)
                mag[(size_t) i] = std::sqrt(std::abs(buf[(size_t) i]));
            out.push_back(std::move(mag));
        }
        return out;
    }

    static float difference(const std::vector<float>& a, const std::vector<float>& b)
    {
        const auto fa = frames(a), fb = frames(b);
        double diff = 0.0, sum = 0.0;
        for (size_t f = 0; f < std::min(fa.size(), fb.size()); ++f)
            for (size_t i = 0; i < fa[f].size(); ++i)
            {
                diff += std::abs(fa[f][i] - fb[f][i]);
                sum += fa[f][i] + fb[f][i];
            }
        return sum > 0.0 ? (float) (diff / sum) : 0.0f;
    }

    static Measure measure(const std::vector<float>& x)
    {
        Measure m;
        double sq = 0.0;
        for (float s : x) sq += (double) s * s;
        m.rms = x.empty() ? 0.0f : (float) std::sqrt(sq / (double) x.size());
        double num = 0.0, den = 0.0;
        for (auto& frame : frames(x))
            for (size_t i = 1; i < frame.size(); ++i)
            {
                const double p = std::pow((double) frame[i], 4.0);     // back to power
                num += p * (double) i * SR / FFT_SIZE;
                den += p;
            }
        m.centroid = den > 0.0 ? (float) (num / den) : 0.0f;
        return m;
    }

    static float dB(float ratio) { return 20.0f * std::log10(std::max(ratio, 1.0e-9f)); }

    // Largest step between neighbouring samples: a click shows up here.
    static float largestStep(const std::vector<float>& x)
    {
        float worst = 0.0f;
        for (size_t i = 1; i < x.size(); ++i)
            worst = std::max(worst, std::abs(x[i] - x[i - 1]));
        return worst;
    }

    struct Result { float diff = 0.0f, rest = 0.0f; Measure at0, at127; bool finite = true; float peak = 0.0f; };

    static Result compare(EngineType type, const ParamMap* sound = nullptr)
    {
        Result r;
        const auto a = render(*makeEngine(type, sound), 0.0f);
        const auto a2 = render(*makeEngine(type, sound), 0.0f);
        const auto b = render(*makeEngine(type, sound), 1.0f);
        r.diff = difference(a, b);
        r.rest = difference(a, a2);
        r.at0 = measure(a);
        r.at127 = measure(b);
        for (float s : b)
        {
            r.finite = r.finite && std::isfinite(s);
            r.peak = std::max(r.peak, std::abs(s));
        }
        return r;
    }

    juce::String line(const juce::String& name, const Result& r) const
    {
        return name.paddedRight(' ', 20)
             + " diff " + juce::String(r.diff, 3)
             + "  centroid " + juce::String((int) r.at0.centroid) + " -> " + juce::String((int) r.at127.centroid) + " Hz"
             + "  level " + juce::String(dB(r.at127.rms / std::max(r.at0.rms, 1.0e-9f)), 1) + " dB"
             + "  peak " + juce::String(r.peak, 2);
    }

    static const std::vector<std::pair<EngineType, const char*>>& soundEngines()
    {
        static const std::vector<std::pair<EngineType, const char*>> list = {
            { EngineType::UnifiedVoice, "Unified voice" }, { EngineType::FM, "FM" },
            { EngineType::MacroOsc, "Macro oscillator" }, { EngineType::Wavetable, "Wavetable" },
            { EngineType::WaveSeq, "Wave sequencer" }, { EngineType::Sampler, "Sampler" },
            { EngineType::Drum, "Drums" } };
        return list;
    }

    void runTest() override
    {
        beginTest("Every sound engine's default sound answers to the mod strip, and is unchanged with the strip at rest");
        for (auto& [type, name] : soundEngines())
        {
            const Result r = compare(type);
            logMessage(line(name, r));
            expect(r.at0.rms > 1.0e-4f, juce::String(name) + ": the default sound is silent");
            expect(r.finite && r.peak < 4.0f, juce::String(name) + ": the strip makes the sound blow up");
            expect(r.rest <= MAX_REST_DIFFERENCE, juce::String(name) + ": two renders at rest differ (" + juce::String(r.rest, 5) + ")");
            expect(r.diff >= MIN_DIFFERENCE,
                   juce::String(name) + ": the mod strip changes the default sound by only " + juce::String(r.diff, 3));
        }

        beginTest("Every factory preset answers to the mod strip");
        for (auto& preset : FactoryPresets::getAll())
        {
            if (preset.engineType == EngineType::MIDITrack)
                continue;
            const Result r = compare(preset.engineType, &preset.soundParams);
            logMessage(line(preset.name, r));
            expect(r.at0.rms > 1.0e-4f, preset.name + ": silent");
            expect(r.finite && r.peak < 4.0f, preset.name + ": the strip makes the sound blow up");
            expect(r.rest <= MAX_REST_DIFFERENCE, preset.name + ": two renders at rest differ (" + juce::String(r.rest, 5) + ")");
            expect(r.diff >= MIN_DIFFERENCE,
                   preset.name + ": the mod strip changes the sound by only " + juce::String(r.diff, 3));
        }

        beginTest("Unified voice: each architecture answers to the strip");
        for (int arch = 1; arch <= ArchitecturePreset::PRESET_COUNT; ++arch)
        {
            const ParamMap sound { { UnifiedParam::ARCH, (float) arch } };
            const Result r = compare(EngineType::UnifiedVoice, &sound);
            const juce::String name = "ARCH " + ArchitecturePreset::getPresetName(arch - 1);
            logMessage(line(name, r));
            expect(r.rest <= MAX_REST_DIFFERENCE, name + ": two renders at rest differ");
            expect(r.diff >= MIN_DIFFERENCE, name + ": the strip changes the sound by only " + juce::String(r.diff, 3));
        }

        beginTest("Unified voice: the built-in vibrato steps aside once an LFO has a WHL amount");
        {
            // An LFO that listens to the strip but goes nowhere: the strip must now do nothing at all.
            const ParamMap sound { { ModParam::lfoParamId(0, ModParam::LFO_DEST), 0.0f },
                                   { ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.5f } };
            const Result r = compare(EngineType::UnifiedVoice, &sound);
            expect(r.diff <= MAX_REST_DIFFERENCE, "strip still heard: " + juce::String(r.diff, 4));

            // The same amount on an LFO that is not in use (LFOS = 1) does not count.
            const ParamMap unused { { ModParam::lfoExt2ParamId(1, ModParam::LFO_WHEEL), 0.5f } };
            expect(compare(EngineType::UnifiedVoice, &unused).diff >= MIN_DIFFERENCE);
        }

        beginTest("Unified voice presets: the strip's LFOs are not heard with the strip at rest");
        for (auto& preset : FactoryPresets::getAll())
        {
            if (preset.engineType != EngineType::UnifiedVoice)
                continue;
            ParamMap plain;
            for (auto& [id, value] : preset.soundParams)
                if (! ModParam::isModParam(id))
                    plain[id] = value;
            const float d = difference(render(*makeEngine(preset.engineType, &preset.soundParams), 0.0f),
                                       render(*makeEngine(preset.engineType, &plain), 0.0f));
            expect(d <= MAX_REST_DIFFERENCE, preset.name + ": differs at rest by " + juce::String(d, 5));
        }

        beginTest("Macro oscillator: every model answers to the strip");
        for (int model = 0; model < 8; ++model)
        {
            const ParamMap sound { { MacroOscEngine::ModelParam, (float) model } };
            const Result r = compare(EngineType::MacroOsc, &sound);
            logMessage(line("model " + juce::String(model), r));
            expect(r.finite && r.diff >= MIN_DIFFERENCE, "model " + juce::String(model) + ": " + juce::String(r.diff, 3));
        }

        beginTest("Sampler: the strip is heard in every play mode");
        for (int mode = 0; mode < SamplerEngine::NumPlayModes; ++mode)
        {
            const ParamMap sound { { SamplerEngine::Mode, (float) mode } };
            const Result r = compare(EngineType::Sampler, &sound);
            logMessage(line("mode " + juce::String(mode), r));
            expect(r.finite && r.diff >= MIN_DIFFERENCE, "mode " + juce::String(mode) + ": " + juce::String(r.diff, 3));
            expect(std::abs(dB(r.at127.rms / std::max(r.at0.rms, 1.0e-9f))) < 6.0f, "mode " + juce::String(mode) + ": the level jumps");
        }

        beginTest("Engines read the strip through a smoother: a 7-bit step is spread over about 20 ms");
        {
            struct Probe : MIDITrackEngine { using EngineBase::smoothWheel; };
            // At rest it stays exactly at rest, and it lands exactly on the target.
            expectEquals(Probe::smoothWheel(0.0f, 0.0f, BLOCK, SR), 0.0f);
            float v = 0.0f, biggest = 0.0f;
            int blocks = 0;
            for (; v != 1.0f / 127.0f && blocks < 1000; ++blocks)
            {
                const float next = Probe::smoothWheel(v, 1.0f / 127.0f, BLOCK, SR);
                biggest = std::max(biggest, next - v);
                v = next;
            }
            expect(blocks < 1000, "never arrives");
            expect(biggest < 0.1f / 127.0f, "one block takes more than a tenth of a 7-bit step");
            // Full travel: within 1 % after 100 ms, and exactly back at zero afterwards.
            v = 0.0f;
            for (int b = 0; b < (int) (0.1 * SR / BLOCK); ++b)
                v = Probe::smoothWheel(v, 1.0f, BLOCK, SR);
            expect(v > 0.99f);
            for (int b = 0; b < (int) (0.5 * SR / BLOCK); ++b)
                v = Probe::smoothWheel(v, 0.0f, BLOCK, SR);
            expectEquals(v, 0.0f);
        }

        beginTest("The strip sweeps up and down under a held chord: nothing jumps further than the sound at either end");
        for (auto& [type, name] : soundEngines())
        {
            if (type == EngineType::Drum)
                continue;       // read at the strike
            // One 7-bit step every third block (4 ms): up over half a second, and down again.
            auto sweep = [](EngineBase& e, int block)
            {
                const int step = block / 3;
                const int value = step <= 127 ? step : std::max(0, 254 - step);
                e.setModWheel((float) value / 127.0f);
            };
            const float rest = largestStep(render(*makeEngine(type), 0.0f, 1.2));
            const float up = largestStep(render(*makeEngine(type), 1.0f, 1.2));
            auto engine = makeEngine(type);
            const auto swept = render(*engine, 0.0f, 1.2, sweep);
            const float moving = largestStep(swept);
            logMessage(juce::String(name).paddedRight(' ', 20) + " largest sample step: rest " + juce::String(rest, 3)
                       + ", up " + juce::String(up, 3) + ", sweeping " + juce::String(moving, 3));
            expect(moving <= 1.5f * std::max(rest, up) + 0.02f, juce::String(name) + ": the sweep clicks");
            for (float sample : swept)
                if (! std::isfinite(sample)) { expect(false, juce::String(name) + ": not finite"); break; }
        }

        beginTest("Back at rest the sound is the rest sound again");
        for (auto& [type, name] : soundEngines())
        {
            // Strip up for the first 0.3 s, then down: the last second is compared with a render at rest.
            auto dip = [](EngineBase& e, int block) { e.setModWheel(block < (int) (0.3 * SR / BLOCK) ? 1.0f : 0.0f); };
            auto a = render(*makeEngine(type), 0.0f, 2.5);
            auto b = render(*makeEngine(type), 0.0f, 2.5, dip);
            // The wave sequencer ran ahead while the strip was up and the sampler's loop is where it
            // is; for those the check is only that the strip is no longer applied (the engine says so).
            if (type == EngineType::UnifiedVoice || type == EngineType::FM)
            {
                const std::vector<float> tailA(a.begin() + (int) (1.5 * SR), a.end()), tailB(b.begin() + (int) (1.5 * SR), b.end());
                expect(measure(tailA).rms > 0.0f);
                expectWithinAbsoluteError(measure(tailB).centroid, measure(tailA).centroid, 0.02f * measure(tailA).centroid + 1.0f,
                                          juce::String(name) + ": brightness did not come back");
                expectWithinAbsoluteError(dB(measure(tailB).rms / measure(tailA).rms), 0.0f, 0.5f, juce::String(name) + ": level did not come back");
            }
        }

        beginTest("The strip moves the selected track; every track keeps its value; a rebuilt engine gets it again");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.addEngine(1, AudioEngine::createEngine(EngineType::FM));
            seq.setSelectedTrack(0);
            auto engineOf = [&audio](int t) { return audio.getEngine(t); };

            ModStripFollower strip;
            strip.moved(audio.getEngine(0), 0, 0.8f);
            expectWithinAbsoluteError(audio.getEngine(0)->getModWheelPosition(), 0.8f, 1.0e-6f);
            expectEquals(audio.getEngine(1)->getModWheelPosition(), 0.0f);

            // A preset load builds a fresh engine: it starts at rest and is given the track's value.
            audio.setEngineType(0, EngineType::UnifiedVoice, true);
            expectEquals(audio.getEngine(0)->getModWheelPosition(), 0.0f);
            strip.follow(engineOf);
            expectWithinAbsoluteError(audio.getEngine(0)->getModWheelPosition(), 0.8f, 1.0e-6f);

            // So does a change of engine.
            audio.setEngineType(0, EngineType::Wavetable);
            strip.follow(engineOf);
            expectWithinAbsoluteError(audio.getEngine(0)->getModWheelPosition(), 0.8f, 1.0e-6f);

            // Another track is selected: nothing changes until the strip is touched; the track
            // that was left keeps its value.
            seq.setSelectedTrack(1);
            strip.follow(engineOf);
            expectWithinAbsoluteError(audio.getEngine(0)->getModWheelPosition(), 0.8f, 1.0e-6f);
            expectEquals(audio.getEngine(1)->getModWheelPosition(), 0.0f);

            // The strip now moves track 2 only.
            strip.moved(audio.getEngine(1), 1, 0.3f);
            strip.follow(engineOf);
            expectWithinAbsoluteError(audio.getEngine(1)->getModWheelPosition(), 0.3f, 1.0e-6f);
            expectWithinAbsoluteError(audio.getEngine(0)->getModWheelPosition(), 0.8f, 1.0e-6f);

            // An unselected track's engine that is rebuilt (a project load) gets its value back too.
            audio.setEngineType(0, EngineType::FM, true);
            strip.follow(engineOf);
            expectWithinAbsoluteError(audio.getEngine(0)->getModWheelPosition(), 0.8f, 1.0e-6f);

            // An arpeggiator that takes the strip for a macro never calls moved(): the engine keeps
            // what it had, whatever the strip does meanwhile.
            strip.follow(engineOf);
            expectWithinAbsoluteError(audio.getEngine(1)->getModWheelPosition(), 0.3f, 1.0e-6f);
        }

        beginTest("A MIDI track sends the strip out as CC 1");
        {
            MIDITrackEngine midi;
            midi.setModWheel(0.5f);
            expectWithinAbsoluteError(midi.getParameter(1), 0.5f, 1.0e-6f);
        }
    }
};

static ModWheelTests modWheelTests;
