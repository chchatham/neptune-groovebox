#pragma once

// ---------------------------------------------------------------------------------------------
// Robustness: nothing a key, a knob, a tempo, a sample rate or a file can hold may read or write
// outside a buffer, divide by zero, loop without end or put a number that is not one into the
// output. (The app's first crash, 2026-11-14: a string an octave below MIDI note 0 read 3,549
// samples before its delay line. Every test of that engine had played middle C.)
//
// These tests drive every engine, every effect, the sequencer and the project loader at the ends
// of their ranges and past them. What they can see by themselves is a number that is not finite
// and a loop that does not end (the watchdog below ends the run and names the case). A read
// outside a buffer is silent in an ordinary build: run this suite under AddressSanitizer and
// UndefinedBehaviorSanitizer as well (.ralph/progress.md -> How to run).
//
// NEPTUNE_ROBUST_DEEP=1 : more random combinations and longer renders (minutes).
// NEPTUNE_ROBUST_ONLY=n : only part n (1 engines, 2 absurd parameters, 3 effects, 4 whole instrument,
//                         5 tiny samples, 6 damaged projects, 7 presets, 8 arpeggiators and roll,
//                         9 random controller input).
// ---------------------------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include "../src/sequencer/SongMode.h"
#include "../src/project/ProjectBridge.h"
#include "../src/project/Preset.h"
#include "../src/project/PresetManager.h"
#include "../src/project/FactoryPresets.h"
#include "../src/arpeggiator/Arpeggiator.h"
#include "../src/sync/MIDIClock.h"
#include "../src/controller/CustomScaleManager.h"
#include "../src/controller/ChordPads.h"
#include "../src/controller/DisplayRenderer.h"
#include "../src/controller/LaunchkeyController.h"
#include "../src/controller/PadManager.h"
#include "../src/ui/ScreenManager.h"
#include "../src/ui/UiClock.h"
#include "../src/ui/GrammarHooks.h"
#include "../src/ui/PadLEDFeedback.h"
#include "../src/ui/screens/HomeScreen.h"
#include "../src/ui/screens/StepViewScreen.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include "../src/ui/screens/DrumPadScreen.h"
#include "../src/ui/screens/PresetScreen.h"
#include "../src/ui/screens/SettingsScreen.h"
#include "../src/ui/screens/FilterViewScreen.h"
#include "../src/ui/screens/ArpScreen.h"
#include "../src/ui/screens/CustomScaleScreen.h"
#include "../src/ui/screens/SongScreen.h"
#include "../src/ui/screens/AftertouchScreen.h"
#include "../src/ui/screens/WaveformScreen.h"
#include "../src/ui/screens/TrackFxScreen.h"
#include "../src/ui/screens/ChordPadScreen.h"

namespace robust
{
    // A case that does not come back ends the whole run with its name: a hang must fail the suite,
    // not stop it. (The limits are wide: the sanitizer builds are several times slower.)
    class Watchdog
    {
    public:
        Watchdog() : thread([this] { run(); }) {}
        ~Watchdog()
        {
            quit.store(true);
            thread.join();
        }

        void arm(const juce::String& what, double seconds)
        {
            const std::lock_guard<std::mutex> guard(lock);
            name = what.toStdString();
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long) (seconds * 1000.0));
            armed = true;
        }

        void disarm()
        {
            const std::lock_guard<std::mutex> guard(lock);
            armed = false;
        }

    private:
        void run()
        {
            while (!quit.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                const std::lock_guard<std::mutex> guard(lock);
                if (armed && std::chrono::steady_clock::now() > deadline)
                {
                    std::fprintf(stderr, "\n!!! Robustness watchdog: \"%s\" did not finish in time (a loop that does not end, "
                                         "or one that spins on a huge value)\n", name.c_str());
                    std::fflush(stderr);
                    std::_Exit(3);
                }
            }
        }

        std::mutex lock;
        std::string name;
        std::chrono::steady_clock::time_point deadline;
        bool armed = false;
        std::atomic<bool> quit { false };
        std::thread thread;
    };

    struct Armed
    {
        Armed(Watchdog& w, const juce::String& what, double seconds = 120.0) : dog(w) { dog.arm(what, seconds); }
        ~Armed() { dog.disarm(); }
        Watchdog& dog;
    };

    // Numbers, and of a size a sound can have: a level that runs away (an oscillator's phase left
    // unwrapped grew its output to 1e15 before it stopped being a number) fails as well.
    inline bool allFinite(const float* x, int n)
    {
        for (int i = 0; i < n; ++i)
            if (!(std::abs(x[i]) < 1000.0f))
                return false;
        return true;
    }

    inline bool deep()
    {
        static const bool on = juce::SystemStats::getEnvironmentVariable("NEPTUNE_ROBUST_DEEP", {}).isNotEmpty();
        return on;
    }

    // Every cell an engine shows, once.
    inline std::vector<ParamSpec> cellsOf(const EngineBase& e)
    {
        std::vector<ParamSpec> cells;
        for (auto& page : e.getParamPages())
            for (auto& p : page.params)
            {
                bool known = !p.isValid();
                for (auto& c : cells)
                    known = known || c.id == p.id;
                if (!known)
                    cells.push_back(p);
            }
        return cells;
    }

    inline bool labelled(const ParamSpec& p, std::initializer_list<const char*> names)
    {
        for (auto* n : names)
            if (p.label != nullptr && std::string(p.label) == n)
                return true;
        return false;
    }

    inline int choiceNamed(const ParamSpec& p, const char* name)
    {
        for (int i = 0; p.choices != nullptr && i < p.numChoices; ++i)
            if (p.choices[i] != nullptr && std::string(p.choices[i]) == name)
                return i;
        return -1;
    }

    // One engine rendered in blocks of `block`; false when a sample is not a finite number.
    inline bool render(EngineBase& e, int numSamples, int block)
    {
        juce::AudioBuffer<float> buffer(2, block);
        juce::MidiBuffer midi;
        bool ok = true;
        for (int done = 0; done < numSamples; done += block)
        {
            buffer.clear();
            midi.clear();
            e.processBlock(buffer, midi);
            ok = ok && allFinite(buffer.getReadPointer(0), block) && allFinite(buffer.getReadPointer(1), block);
        }
        return ok;
    }

    // Keys at both ends and in the middle, bent, pressed and with the strip up; then everything the
    // other way, the keys struck again; then a new sample rate and block size under the held notes.
    inline bool playHard(EngineBase& e, double sr, int block, double otherSr, int otherBlock, float seconds)
    {
        e.pitchBend(-1.0f);
        e.setModWheel(1.0f);
        for (int note : { 0, 127, 60 })
        {
            e.noteOn(note, 1.0f, -1);
            e.aftertouch(note, 1.0f);
        }
        bool ok = render(e, (int) (sr * seconds), block);
        e.pitchBend(1.0f);
        e.setModWheel(0.0f);
        e.noteOff(60, -1);
        for (int note : { 127, 0, 1, 126 })
            e.noteOn(note, 0.01f, -1);
        ok = render(e, (int) (sr * seconds * 0.5f), block) && ok;
        e.prepareToPlay(otherSr, otherBlock);
        const float otherSeconds = deep() ? seconds : (float) ((double) seconds * sr / otherSr);     // as many samples
        ok = render(e, (int) (otherSr * otherSeconds * 0.5f), otherBlock) && ok;
        e.noteOn(0, 1.0f, -1);
        e.pitchBend(-1.0f);
        ok = render(e, (int) (otherSr * otherSeconds * 0.5f), otherBlock) && ok;
        e.allNotesOff();
        return render(e, otherBlock * 2, otherBlock) && ok;
    }

    static constexpr int NUM_SOUND_ENGINES = 7;      // every EngineType but the MIDI track
    inline EngineType soundEngine(int i)
    {
        static const EngineType types[NUM_SOUND_ENGINES] = { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc,
                                                             EngineType::Wavetable, EngineType::WaveSeq, EngineType::Sampler,
                                                             EngineType::Drum };
        return types[i];
    }

    // What a hand-edited file can hold where a number is expected.
    inline const std::vector<float>& absurdValues()
    {
        static const std::vector<float> v { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                            -std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f, 1.0e9f, -1.0e9f,
                                            2147483648.0f, -2147483904.0f, 1.0e-30f, -0.0f };
        return v;
    }
}

class RobustnessTests : public juce::UnitTest
{
public:
    RobustnessTests() : juce::UnitTest("Robustness") {}

    robust::Watchdog dog;

    struct Rates { double sr; int block; double otherSr; int otherBlock; };
    static const std::vector<Rates>& rates()
    {
        // The usual ones, then what a headset in call mode (8 / 16 kHz) or a 384 kHz interface gives.
        static const std::vector<Rates> r { { 44100.0, 64, 96000.0, 16 }, { 48000.0, 16, 192000.0, 4096 },
                                            { 96000.0, 4096, 44100.0, 1 }, { 192000.0, 1, 48000.0, 64 },
                                            { 8000.0, 16, 384000.0, 512 }, { 384000.0, 512, 16000.0, 64 } };
        return r;
    }

    // ---- 1 ------------------------------------------------------------------------------------
    void enginesAtTheirEnds()
    {
        beginTest("Every engine, notes 0 and 127, every cell at both ends and in random combinations, 8 to 384 kHz, "
                  "blocks of 1 to 4096, the rate and block changed under held notes");
        for (int t = 0; t < robust::NUM_SOUND_ENGINES; ++t)
            sweepEngine(robust::soundEngine(t), robust::deep() ? 40 : 4, (int) rates().size(), {}, {});
    }

    // One engine type through the cases: 0 as it comes, 1 every cell at its lowest, 2 at its highest,
    // 3 / 4 everything that moves the pitch down / up, then random combinations. `after` is called
    // once the cells are set (to choose a sample, say).
    void sweepEngine(EngineType type, int randomCases, int numRates, const juce::String& label,
                     const std::function<void(EngineBase&)>& after)
    {
        using namespace robust;
        juce::Random rng(20261114);

        {
            for (int ri = 0; ri < numRates; ++ri)
                for (int c = 0; c < 5 + (ri < 4 || deep() ? randomCases : 0); ++c)
                {
                    const auto& rate = rates()[(size_t) ri];
                    // (about as many samples at every rate: the short run is for the suite's sake)
                    const float seconds = deep() ? 0.4f : (float) (2500.0 / rate.sr);
                    auto e = AudioEngine::createEngine(type);
                    const juce::String what = e->getName() + label + " at " + juce::String(rate.sr, 0) + " Hz, blocks of "
                                              + juce::String(rate.block) + ", case " + juce::String(c);
                    const Armed armed(dog, what);
                    e->prepareToPlay(rate.sr, rate.block);
                    e->setTempo(c % 2 == 0 ? 20.0f : 300.0f);
                    const auto cells = cellsOf(*e);
                    for (auto& p : cells)
                    {
                        float v = p.defaultVal;
                        if (c == 1) v = p.minVal;
                        else if (c == 2) v = p.maxVal;
                        else if (c == 3 || c == 4)
                        {
                            // Everything that moves the pitch, all the way down (3) or up (4): the
                            // tuning cells, the widest bend, both LFOs and every strip route on pitch.
                            const bool down = (c == 3);
                            if (labelled(p, { "TUNE", "FINE", "OCT", "PTCH", "PMOD" })) v = down ? p.minVal : p.maxVal;
                            else if (labelled(p, { "BEND", "WVIB", "RATE", "VRAT" }))  v = p.maxVal;
                            else if (labelled(p, { "DPTH", "AMT1", "AMT2", "AMT3" }))  v = down ? p.minVal : p.maxVal;
                            else if (labelled(p, { "DEST", "DST1", "DST2", "DST3" }) && choiceNamed(p, "PTCH") >= 0)
                                v = (float) choiceNamed(p, "PTCH");
                        }
                        else if (c >= 5)
                        {
                            const int pick = rng.nextInt(5);
                            v = pick < 2 ? p.minVal : (pick < 4 ? p.maxVal : p.fromNorm(rng.nextFloat()));
                        }
                        e->setParameter(p.id, v);
                    }
                    if (after)
                        after(*e);
                    expect(playHard(*e, rate.sr, rate.block, rate.otherSr, rate.otherBlock, seconds), what);
                }
        }
    }

    // ---- 2 ------------------------------------------------------------------------------------
    // The one entry point for a track's parameters (screens, step locks, presets, projects) takes
    // any id and any value a file can hold.
    void absurdTrackParameters()
    {
        beginTest("Track parameters from a file: every id from -8 to 700 and every cell with huge, tiny, infinite and "
                  "not-a-number values still gives finite sound");
        using namespace robust;
        int reported = 0;
        // One instrument per engine type (its engine on track 0, two insert effects, the sends).
        for (int type = 0; type <= NUM_SOUND_ENGINES; ++type)
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(type < NUM_SOUND_ENGINES ? soundEngine(type) : EngineType::MIDITrack));
            audio.prepare(48000.0, 32);
            audio.addInsertEffect(0, EffectType::DelayEffect);
            audio.addInsertEffect(0, EffectType::ChorusEffect);
            const juce::String name = audio.getEngine(0)->getName();

            std::vector<float> l(32), r(32);
            auto check = [&](int id, float value)
            {
                const Armed armed(dog, name + ", id " + juce::String(id) + " = " + juce::String(value), 60.0);
                audio.getEngine(0)->noteOn(0, 1.0f, -1);
                audio.getEngine(0)->noteOn(127, 1.0f, -1);
                // The engine's own output is what is judged: AudioEngine drops a block that is not
                // finite and counts it, so the count must stay where it is.
                const int dropped = audio.getNonFiniteBlocks();
                audio.renderBlock(l.data(), r.data(), 32);
                const bool ok = allFinite(l.data(), 32) && allFinite(r.data(), 32) && audio.getNonFiniteBlocks() == dropped;
                if (!ok && reported++ < 60)
                    expect(false, name + ", id " + juce::String(id) + " = " + juce::String(value));
                if (!ok)
                    audio.setEngineType(0, audio.getEngineTypeOf(0), true);     // a clean engine: the next failure is its own
                return ok;
            };

            // One id at a time first, so that a failure names it; then all of them together.
            for (float value : absurdValues())
                for (int id = -8; id < 700; ++id)
                {
                    const float before = audio.getTrackParameter(0, id);
                    audio.setTrackParameter(0, id, value);
                    juce::ignoreUnused(audio.getTrackParameter(0, id));
                    if (EngineParamRange::contains(id) || id % 8 == 0)
                        check(id, value);
                    audio.setTrackParameter(0, id, before);
                }
            for (float value : absurdValues())
            {
                for (int id = -8; id < 700; ++id)
                    audio.setTrackParameter(0, id, value);
                check(700, value);
                audio.setEngineType(0, audio.getEngineTypeOf(0), true);
            }
            // Tracks that do not exist.
            for (int t : { -1, AudioEngine::MAX_TRACKS, AudioEngine::MAX_TRACKS + 1, 1000000 })
                for (int id = -8; id < 700; ++id)
                {
                    audio.setTrackParameter(t, id, 0.5f);
                    juce::ignoreUnused(audio.getTrackParameter(t, id));
                }
        }
        expect(reported == 0, juce::String(reported) + " parameter values gave sound that is not a finite number");
    }

    // ---- 3 ------------------------------------------------------------------------------------
    static void feed(float* l, float* r, int n, juce::Random& rng, bool loud)
    {
        for (int i = 0; i < n; ++i)
        {
            l[i] = loud ? rng.nextFloat() * 2.0f - 1.0f : 0.0f;
            r[i] = loud ? rng.nextFloat() * 2.0f - 1.0f : 0.0f;
        }
    }

    void effectsAtTheirEnds()
    {
        beginTest("Every effect type, its four cells at every corner, synced at 20 and 300 BPM, 44.1 to 192 kHz, "
                  "the rate changed while it rings, then absurd values");
        using namespace robust;
        juce::Random rng(7);
        std::vector<float> l(4096), r(4096);

        for (int type = 0; type < (int) EffectType::NumEffectTypes; ++type)
            for (auto& rate : rates())
            {
                const float seconds = deep() ? 0.5f : (float) (2000.0 / rate.sr);
                const juce::String what = juce::String(AudioEngine::effectTypeName((EffectType) type)) + " at "
                                          + juce::String(rate.sr, 0) + " Hz";
                const Armed armed(dog, what);
                auto fx = AudioEngine::createEffect((EffectType) type);
                expect(fx != nullptr, what);
                if (fx == nullptr)
                    continue;
                fx->prepare(rate.sr, rate.block);
                bool ok = true;
                auto run = [&](double sr, int block, float secs)
                {
                    const int total = (int) (sr * secs);
                    for (int done = 0; done < total; done += block)
                    {
                        feed(l.data(), r.data(), block, rng, done < total / 2);
                        fx->processStereo(l.data(), r.data(), block);
                        ok = ok && allFinite(l.data(), block) && allFinite(r.data(), block);
                    }
                };
                for (int corner = 0; corner < 16; ++corner)
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        const ParamSpec spec = fx->getParamSpec(i);
                        if (spec.isValid())
                            fx->setUserParameter(i, (corner >> i) & 1 ? spec.maxVal : spec.minVal);
                    }
                    fx->setMix((corner & 1) ? 1.0f : 0.5f);
                    run(rate.sr, rate.block, seconds);
                }
                // Tempo-synced times and rates at both ends of the tempo and of the division list.
                for (float bpm : { 20.0f, 300.0f })
                    for (int division : { 0, FxSync::NUM_DIVISIONS - 1, -5, 1000 })
                    {
                        fx->setTempo(bpm);
                        fx->setSync(true);
                        fx->setSyncDivision(division);
                        run(rate.sr, rate.block, seconds);
                    }
                fx->setSync(false);
                // A new rate while it rings; back again; reset.
                fx->prepare(rate.otherSr, rate.otherBlock);
                run(rate.otherSr, rate.otherBlock, deep() ? seconds : (float) (2000.0 / rate.otherSr));
                fx->prepare(rate.sr, rate.block);
                run(rate.sr, rate.block, seconds);
                expect(ok, what);

                // Values no knob gives (a hand-edited file); what is not a number never arrives
                // here through AudioEngine, but a huge one does.
                ok = true;
                for (float value : absurdValues())
                {
                    if (!std::isfinite(value) || std::abs(value) > 1.0e9f)      // (AudioEngine passes on +-1e9 at most)
                        continue;
                    for (int i = -1; i < 6; ++i)
                        fx->setUserParameter(i, value);
                    run(rate.sr, rate.block, 0.01f);
                    fx->reset();
                }
                expect(ok, what + ", absurd values");
            }
    }

    // ---- 4 ------------------------------------------------------------------------------------
    void wholeInstrumentAtTheEnds()
    {
        beginTest("The whole instrument: tempo 20 and 300, track lengths 1 and 1024, speeds at their ends, steps off "
                  "the grid, locks on every kind of parameter, effect blocks, sends and master at their ends, "
                  "a block longer than the prepared one");
        using namespace robust;
        juce::Random rng(99);
        for (auto& rate : rates())
            for (float bpm : { 20.0f, 300.0f })
            {
                const juce::String what = "instrument at " + juce::String(rate.sr, 0) + " Hz, " + juce::String(bpm, 0) + " BPM";
                const Armed armed(dog, what, 300.0);
                SequencerCore seq;
                AudioEngine audio;
                audio.setSequencer(&seq);
                wire(seq, audio);
                for (int t = 0; t < 8; ++t)
                    audio.addEngine(t, AudioEngine::createEngine(soundEngine(t % NUM_SOUND_ENGINES)));
                audio.prepare(rate.sr, rate.block);
                seq.setTempo(bpm);
                for (int t = 0; t < 8; ++t)
                {
                    auto& track = seq.getPattern().getTrack(t);
                    track.setLength(t % 2 == 0 ? 1 : 1024);
                    static const float speeds[] = { 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };
                    track.speedMultiplier = speeds[t];
                    for (int s = 0; s < track.length; s += (track.length > 64 ? 37 : 1))
                    {
                        auto& step = track.getStep(s);
                        step.trig = true;
                        step.notes = { { rng.nextBool() ? 0 : 127, 1.0f, {} }, { 60, 0.5f, {} } };
                        step.microTiming = rng.nextFloat() - 0.5f;
                        step.length = rng.nextBool() ? 0.01f : 64.0f;
                        step.trigCondition.type = (TrigCondition) rng.nextInt(9);
                        step.trigCondition.a = rng.nextInt(9) - 1;
                        step.trigCondition.b = rng.nextInt(9) - 1;
                        step.trigCondition.probability = rng.nextFloat() * 2.0f - 0.5f;
                        for (int id : { 0, 3, 24, 100, 160, 190, 191, 192, 199, 200, 215, 240, 256, 270, 272, 280, 296, 304, 400, 600 })
                            if (rng.nextInt(4) == 0)
                                step.paramLocks[id] = rng.nextFloat() * 4.0f - 2.0f;
                    }
                    audio.addInsertEffect(t, (EffectType) rng.nextInt((int) EffectType::NumEffectTypes));
                    for (int id = TrackFxParam::DIRT_BASE; id < TrackFxParam::END; ++id)
                        audio.setTrackParameter(t, id, rng.nextBool() ? 1.0e9f : -1.0e9f);
                    for (int s = 0; s < MixParam::MAX_SENDS; ++s)
                        audio.setTrackParameter(t, MixParam::SEND_BASE + s, 1.0f);
                }
                for (int i = 0; i < AudioEngine::NUM_MASTER_PARAMS; ++i)
                    audio.setMasterParam(i, rng.nextBool() ? 1.0e9f : -1.0e9f);
                seq.start();

                std::vector<float> l((size_t) rate.block * 3), r((size_t) rate.block * 3);
                bool ok = true;
                const int total = deep() ? (int) (rate.sr * 4.0) : 12000;
                for (int done = 0, n = 0; done < total; done += rate.block, ++n)
                {
                    audio.renderBlock(l.data(), r.data(), rate.block);
                    ok = ok && allFinite(l.data(), rate.block) && allFinite(r.data(), rate.block);
                    if (n == 3)
                        seq.setTempo(bpm > 100.0f ? 20.0f : 300.0f);
                    if (n == 5)
                        for (int t = 0; t < 8; ++t)
                            seq.getPattern().getTrack(t).setLength(t % 2 == 0 ? 1024 : 1);     // under the playhead
                    if (n == 7)
                        seq.selectPattern(3, SequencerCore::ChangeMode::Immediate);
                }
                // The device hands over a longer block than it announced (JUCE: "not guaranteed").
                {
                    float* out[2] = { l.data(), r.data() };
                    audio.audioDeviceIOCallbackWithContext(nullptr, 0, out, 2, rate.block * 3, {});
                    ok = ok && allFinite(l.data(), rate.block * 3) && allFinite(r.data(), rate.block * 3);
                }
                seq.stop();
                expect(ok, what);
                expectEquals(audio.getNonFiniteBlocks(), 0, what + ": blocks dropped as not finite");
            }
    }

    // As main.cpp wires the sequencer to the engines.
    static void wire(SequencerCore& seq, AudioEngine& audio)
    {
        seq.setTrigCallback([&audio](int track, const Step& step)
        {
            auto* e = audio.getEngine(track);
            if (e == nullptr) return;
            audio.applyStepLocks(track, step);
            for (auto& n : step.notes)
                e->noteOn(juce::jlimit(0, 127, n.pitch), n.velocity, -1);
        });
        seq.setNoteOffCallback([&audio](int track, int note)
        {
            if (auto* e = audio.getEngine(track))
                e->noteOff(juce::jlimit(0, 127, note), -1);
        });
    }

    // ---- 5 ------------------------------------------------------------------------------------
    void tinySamples()
    {
        beginTest("Samples of one, two and seven frames, silent ones, and sample rates of 1 Hz and 1 MHz: the sampler "
                  "and the wavetable's USER and KEYS tables at the ends of their cells");
        using namespace robust;
        auto& bank = SampleBank::get();
        struct Tiny { int frames; double sr; bool stereo; float value; };
        std::vector<int> indices;
        for (auto tiny : { Tiny { 1, 44100.0, false, 0.5f }, Tiny { 2, 48000.0, true, 0.0f }, Tiny { 7, 1.0, false, -1.0f },
                           Tiny { 300, 1.0e6, true, 1.0f } })
        {
            std::vector<float> data((size_t) tiny.frames, tiny.value);
            const int index = bank.addRecording(data.data(), tiny.stereo ? data.data() : nullptr, tiny.frames, tiny.sr, false);
            if (index >= 0)
                indices.push_back(index);
        }
        expect(! indices.empty(), "the bank had no room for the test samples");

        // Grains that move further than their area in one sample: the top key over the lowest root,
        // PTCH and the bend up, 64 slices, at 8 kHz. (The grain ran off to positions past the range
        // of a whole number; only UndefinedBehaviorSanitizer shows it.)
        for (double sr : { 8000.0, 48000.0 })
        {
            const Armed armed(dog, "grains faster than their area");
            SamplerEngine e;
            e.prepareToPlay(sr, 64);
            if (! indices.empty())
                e.setParameter(SamplerEngine::SampleSelect, (float) indices.back());    // (300 frames at 1 MHz: faster still)
            e.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Granular);
            e.setParameter(SamplerEngine::SliceCount, 64.0f);
            e.setParameter(SamplerEngine::RootNote, 0.0f);
            e.setParameter(SamplerEngine::PitchSemi, 24.0f);
            e.setParameter(SamplerEngine::BendRange, 24.0f);
            e.setParameter(SamplerEngine::GrainSize, 0.5f);
            e.setParameter(SamplerEngine::GrainDensity, 200.0f);
            e.pitchBend(1.0f);
            for (int note : { 127, 126, 0 })
                e.noteOn(note, 1.0f, -1);
            expect(render(e, (int) sr, 64), "grains faster than their area at " + juce::String(sr, 0) + " Hz");
        }

        for (int index : indices)
        {
            const juce::String label = " with sample " + juce::String(index) + " (" + juce::String((int) bank.entry(index)->left.size())
                                       + " frames at " + juce::String(bank.entry(index)->sampleRate, 0) + " Hz)";
            sweepEngine(EngineType::Sampler, 4, 2, label, [index](EngineBase& e)
            {
                e.setParameter(SamplerEngine::SampleSelect, (float) index);
            });
            for (int keys = 0; keys < 2; ++keys)
                sweepEngine(EngineType::Wavetable, 2, 2, label + (keys ? ", KEYS" : ", USER"), [index, keys](EngineBase& e)
                {
                    for (auto& p : cellsOf(e))
                        if (p.id == WavetableEngine::TableSelect && choiceNamed(p, keys ? "KEYS" : "USER") >= 0)
                            e.setParameter(p.id, (float) choiceNamed(p, keys ? "KEYS" : "USER"));
                    e.setParameter(WavetableEngine::UserSample, (float) index);
                    for (int k = 0; k < WavetableKeys::MAX_KEYS; ++k)
                        e.setParameter(WavetableEngine::KeySampleBase + k, k % 3 == 2 ? 0.0f : (float) (index + 1));
                });
        }
    }

    // ---- 6 ------------------------------------------------------------------------------------
    // A whole instrument as the app builds it, for loading projects into.
    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        SongMode song;
        CustomScaleManager scales;
        ChordPads chords;
        std::array<Arpeggiator, AudioEngine::MAX_TRACKS> arps;

        Rig(double sr = 48000.0, int block = 64)
        {
            audio.setSequencer(&seq);
            RobustnessTests::wire(seq, audio);
            for (int t = 0; t < 8; ++t)
                audio.addEngine(t, AudioEngine::createEngine(robust::soundEngine(t % robust::NUM_SOUND_ENGINES)));
            for (int t = 0; t < AudioEngine::MAX_TRACKS; ++t)
            {
                audio.setTrackArpeggiator(t, &arps[(size_t) t]);
                arps[(size_t) t].onNoteOn = [this, t](int pitch, float velocity)
                {
                    if (auto* e = audio.getEngine(t)) e->noteOn(juce::jlimit(0, 127, pitch), velocity, -1);
                };
                arps[(size_t) t].onNoteOff = [this, t](int pitch)
                {
                    if (auto* e = audio.getEngine(t)) e->noteOff(juce::jlimit(0, 127, pitch), -1);
                };
            }
            audio.prepare(sr, block);
        }

        ProjectBridge::Live live()
        {
            return { seq, audio, &song, &scales, nullptr, [this](int t) { return t >= 0 && t < (int) arps.size() ? &arps[(size_t) t] : nullptr; },
                     &chords, {}, {} };
        }

        // Play what was loaded: keys and pads on every track, the transport running.
        bool play(int blocks, int block = 64)
        {
            std::vector<float> l((size_t) block), r((size_t) block);
            bool ok = true;
            seq.start();
            for (int n = 0; n < blocks; ++n)
            {
                if (n == 1)
                    for (int t = 0; t < AudioEngine::MAX_TRACKS; ++t)
                    {
                        for (int note : { 0, 64, 127 })
                            arps[(size_t) t].noteOn(note, 1.0f);
                        if (auto* e = audio.getEngine(t))
                            e->noteOn(t * 4, 1.0f, -1);
                        audio.padNoteOn(t, t % 16, 36 + t, 1.0f);
                        audio.padPressure(t % 16, 1.0f);
                    }
                audio.renderBlock(l.data(), r.data(), block);
                ok = ok && robust::allFinite(l.data(), block) && robust::allFinite(r.data(), block);
            }
            seq.stop();
            return ok;
        }
    };

    // A project with something in every field.
    static juce::var fullProject()
    {
        Rig rig;
        juce::Random rng(5);
        for (int p = 0; p < 2; ++p)
            for (int t = 0; t < 8; ++t)
            {
                auto& track = rig.seq.getPattern(p).getTrack(t);
                track.setLength(8 + t * 8);
                for (int s = 0; s < track.length; s += 3)
                {
                    auto& step = track.getStep(s);
                    step.trig = true;
                    step.notes = { { 36 + s, 0.9f, {} }, { 60, 0.5f, {} } };
                    step.notes[0].lane.points = { 0, 100, 255, 30 };
                    step.microTiming = 0.2f;
                    step.trigCondition = { TrigCondition::A_B, 1, 2, 0.5f };
                    step.paramLocks = { { 3, 0.5f }, { 190, 0.7f }, { 240, 0.4f } };
                }
            }
        for (int t = 0; t < 8; ++t)
        {
            rig.audio.addInsertEffect(t, (EffectType) (t * 3));
            auto& params = rig.seq.getPattern().getTrack(t).soundParams;
            params[ProjectBridge::INSERT_TYPE_BASE] = (float) (t * 3 + 1);
            for (auto& cell : robust::cellsOf(*rig.audio.getEngine(t)))
                params[cell.id] = cell.fromNorm(rng.nextFloat());
            for (int id : { 190, 191, 192, 193, 200, 201, 240, 247, 256, 270, 280, 288, 296, 304 })
                params[id] = rng.nextFloat();
            rig.arps[(size_t) t].setEnabled(t % 2 == 0);
            rig.seq.getPattern().getTrack(t).engineType = rig.audio.getEngineTypeOf(t);
        }
        rig.song.addEntry(0, 2);
        rig.song.addEntry(1, 1);
        return ProjectBridge::capture(rig.live(), "ROBUST").toVar();
    }

    // The same tree with some of its values replaced by what a damaged or hand-edited file holds.
    static juce::var damaged(const juce::var& v, juce::Random& rng, float chance)
    {
        if (auto* obj = v.getDynamicObject())
        {
            auto* out = new juce::DynamicObject();
            for (auto& prop : obj->getProperties())
                if (rng.nextFloat() >= chance * 0.3f)        // (else: the field is missing)
                    out->setProperty(prop.name, damaged(prop.value, rng, chance));
            return juce::var(out);
        }
        if (auto* arr = v.getArray())
        {
            if (rng.nextFloat() < chance * 0.1f)
                return juce::var(rng.nextInt(100) - 50);    // a number where a list belongs
            juce::Array<juce::var> out;
            const int keep = rng.nextFloat() < chance * 0.3f ? rng.nextInt(arr->size() + 1) : arr->size();
            for (int i = 0; i < keep; ++i)
                out.add(damaged((*arr)[i], rng, chance));
            if (arr->size() > 0 && arr->size() < 64 && rng.nextFloat() < chance * 0.2f)
                for (int i = 0; i < 40; ++i)                 // a list much longer than the app writes
                    out.add(damaged((*arr)[rng.nextInt(arr->size())], rng, chance));
            return juce::var(out);
        }
        if ((v.isInt() || v.isInt64() || v.isDouble() || v.isBool()) && rng.nextFloat() < chance)
        {
            static const double values[] = { 0.0, -1.0, 1.0, 0.5, 99.0, -99.0, 1.0e9, -1.0e9, 1.0e30, -1.0e30, 2147483647.0, -2147483648.0,
                                             4294967296.0, 1.0e-30, std::numeric_limits<double>::infinity(),
                                             -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() };
            const int pick = rng.nextInt((int) std::size(values) + 2);
            if (pick == (int) std::size(values))     return juce::var("text");
            if (pick == (int) std::size(values) + 1) return juce::var();
            return juce::var(values[pick]);
        }
        return v;
    }

    void hostileProjects()
    {
        beginTest("Projects and sessions from damaged or hand-edited files load and play: missing fields, wrong types, "
                  "negative, huge and non-finite numbers, 0 where a length belongs, lists too short and too long");
        using namespace robust;
        const juce::var good = fullProject();

        // The file as written loads and plays.
        {
            const Armed armed(dog, "the undamaged project");
            Rig rig;
            ProjectBridge::apply(ProjectData::fromJSON(juce::JSON::toString(good)), rig.live());
            expect(rig.play(40), "the undamaged project");
            expectEquals(rig.audio.getNonFiniteBlocks(), 0);
        }

        // Text that is not a project at all.
        for (const char* text : { "", "{", "[]", "123", "null", "{\"patterns\": 5}", "{\"patterns\": [1, \"x\", null, {\"tracks\": 3}]}",
                                  "{\"patterns\": [{\"tracks\": [{\"steps\": 7, \"params\": \"x\", \"length\": 1e999}]}], \"bpm\": 1e999}",
                                  "{\"bpm\": -1e999, \"masterLevel\": 1e999, \"activePattern\": 1e999, \"roll\": [1e999, -1e999]}" })
        {
            const Armed armed(dog, juce::String("project text: ") + text);
            Rig rig;
            ProjectBridge::apply(ProjectData::fromJSON(text), rig.live());
            expect(rig.play(10), juce::String("project text: ") + text);
        }

        // Chosen damage: one field at a time, on every track of every pattern.
        struct Field { const char* name; std::vector<double> values; };
        const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
        const std::vector<Field> trackFields {
            { "length", { 0, -5, 1, 3, 1024, 100000, 2147483647.0, -2147483648.0, 1.0e30, nan } },
            { "speed", { 0, -1, 1.0e-30, 1.0e-6, 1.0e6, 1.0e30, inf, nan } },
            { "engine", { -1, 7, 8, 99, 1.0e9, -1.0e30, nan } },
            { "midiCh", { -1, 0, 17, 255, 1.0e9, nan } } };
        const std::vector<Field> stepFields {
            { "len", { 0, -1, 1.0e-9, 1.0e9, 1.0e30, inf, nan } },
            { "micro", { -1.0e9, -0.5, 0.5, 5, 1.0e30, inf, nan } },
            { "trigType", { -1, 9, 99, 1.0e9, nan } },
            { "trigA", { 0, -1, 2147483647.0, -2147483648.0 } },
            { "trigB", { 0, -1, 2147483647.0, -2147483648.0 } },
            { "trigProb", { -1, 2, inf, nan } },
            { "vel", { -1, 2, 1.0e30, nan } },
            { "pres", { -1, 1.0e30, nan } } };
        auto forEachTrack = [](juce::var& project, const std::function<void(juce::DynamicObject&)>& fn)
        {
            if (auto* patterns = project["patterns"].getArray())
                for (auto& pattern : *patterns)
                    if (auto* tracks = pattern["tracks"].getArray())
                        for (auto& track : *tracks)
                            if (auto* obj = track.getDynamicObject())
                                fn(*obj);
        };
        int loads = 0;
        auto loadAndPlay = [&](const juce::var& project, const juce::String& what, int blocks)
        {
            const Armed armed(dog, what);
            Rig rig;
            ++loads;
            ProjectBridge::apply(ProjectData::fromVar(project), rig.live());
            expect(rig.play(blocks), what);
            expectEquals(rig.audio.getNonFiniteBlocks(), 0, what + ": blocks dropped as not finite, the last from "
                                                            + juce::String(rig.audio.getNonFiniteTrack()));
            // Once more through the text, as the app does it (now and then, in the ordinary run).
            if (!deep() && loads % 6 != 1)
                return;
            Rig second;
            ProjectBridge::apply(ProjectData::fromJSON(ProjectBridge::capture(rig.live(), "AGAIN").toJSON()), second.live());
            expect(second.play(4), what + ", saved and loaded again");
        };
        // (The ordinary run takes every other value of each list, the first and the last always.)
        auto skipped = [](const Field& field, size_t i) { return !deep() && i % 2 == 1 && i + 1 != field.values.size(); };
        for (auto& field : trackFields)
            for (size_t vi = 0; vi < field.values.size(); ++vi)
            {
                if (skipped(field, vi)) continue;
                const double value = field.values[vi];
                juce::var project = good.clone();
                forEachTrack(project, [&](juce::DynamicObject& track) { track.setProperty(field.name, value); });
                loadAndPlay(project, juce::String("track ") + field.name + " = " + juce::String(value), 24);
            }
        for (auto& field : stepFields)
            for (size_t vi = 0; vi < field.values.size(); ++vi)
            {
                if (skipped(field, vi)) continue;
                const double value = field.values[vi];
                juce::var project = good.clone();
                forEachTrack(project, [&](juce::DynamicObject& track)
                {
                    if (auto* steps = track.getProperty("steps").getArray())
                        for (auto& step : *steps)
                            if (auto* obj = step.getDynamicObject())
                            {
                                obj->setProperty(field.name, value);
                                if (auto* notes = obj->getProperty("notes").getArray())
                                    for (auto& note : *notes)
                                        if (auto* n = note.getDynamicObject())
                                        {
                                            n->setProperty("pitch", value);
                                            n->setProperty("vel", value);
                                            n->setProperty("laneShift", value);
                                        }
                                if (auto* locks = obj->getProperty("locks").getArray())
                                    for (auto& lock : *locks)
                                        if (auto* l = lock.getDynamicObject())
                                            l->setProperty(juce::Random::getSystemRandom().nextBool() ? "id" : "v", value);
                            }
                });
                loadAndPlay(project, juce::String("step ") + field.name + " = " + juce::String(value), 24);
            }

        // Pad-pressure routes with depths no knob gives, under notes that carry recorded pressure
        // (the deep run found this one: a depth of 1e30 gave notes that were not numbers).
        for (double value : { 1.0e30, -1.0e30, inf, nan })
        {
            juce::var project = good.clone();
            auto damageRoutes = [value](const juce::var& list)
            {
                if (auto* routes = list["routes"].getArray())
                    for (auto& route : *routes)
                        if (auto* r = route.getDynamicObject())
                        {
                            r->setProperty("depth", value);
                            r->setProperty("dest", 1 + (int) (std::abs(value) > 1.0e30 ? 2 : 0));
                        }
            };
            damageRoutes(project["aftertouch"]);
            if (auto* perTrack = project["trackAftertouch"].getArray())
                for (auto& list : *perTrack)
                    damageRoutes(list);
            loadAndPlay(project, "pressure route depth = " + juce::String(value), 24);
        }

        // More tracks than the app ever makes, in the pattern that is playing and in the others.
        for (int count : { 9, 32, 33, 100 })
        {
            juce::var project = good.clone();
            if (auto* patterns = project["patterns"].getArray())
                for (auto& pattern : *patterns)
                    if (auto* tracks = pattern["tracks"].getArray())
                        while (tracks->size() > 0 && tracks->size() < count)
                            tracks->add((*tracks)[tracks->size() % 8].clone());
            loadAndPlay(project, juce::String(count) + " tracks", 24);
        }

        // Random damage everywhere.
        juce::Random rng(2026);
        const int rounds = deep() ? 600 : 24;
        for (int i = 0; i < rounds; ++i)
        {
            const float chance = i % 4 == 0 ? 0.5f : 0.05f;
            loadAndPlay(damaged(good, rng, chance), "random damage " + juce::String(i), 12);
        }
    }

    // ---- 7 ------------------------------------------------------------------------------------
    void hostilePresets()
    {
        beginTest("Presets: every factory preset at notes 0 and 127 at 48 and 192 kHz, and damaged preset files");
        using namespace robust;
        static const auto all = FactoryPresets::getAll();
        {
            const Armed armed(dog, "factory presets", 300.0);
            for (auto& preset : all)
                for (double sr : { 48000.0, 192000.0 })
                {
                    auto e = AudioEngine::createEngine(preset.engineType);
                    e->prepareToPlay(sr, 64);
                    for (auto& [id, value] : PresetManager::soundOf(preset))
                        e->setParameter(id, value);
                    e->pitchBend(-1.0f);
                    e->setModWheel(1.0f);
                    e->noteOn(0, 1.0f, -1);
                    e->noteOn(127, 1.0f, -1);
                    e->aftertouch(0, 1.0f);
                    bool ok = render(*e, deep() ? (int) (sr * 0.5) : 1536, 64);
                    e->pitchBend(1.0f);
                    ok = render(*e, deep() ? (int) (sr * 0.5) : 1024, 64) && ok;
                    expect(ok, preset.name + " at " + juce::String(sr, 0) + " Hz");
                }
        }

        juce::Random rng(77);
        const int rounds = deep() ? 2000 : 50;
        for (int i = 0; i < rounds; ++i)
        {
            const auto& source = all[(size_t) rng.nextInt((int) all.size())];
            const juce::String what = "damaged preset " + juce::String(i) + " (" + source.name + ")";
            const Armed armed(dog, what);
            const Preset preset = Preset::fromVar(damaged(source.toVar(), rng, i % 3 == 0 ? 0.6f : 0.1f));
            juce::ignoreUnused(Preset::fromJSON(preset.toJSON()));

            // As PRESET applies it: a fresh engine of its type, its values, its insert effects, its arp.
            Rig rig;
            rig.audio.setEngineType(0, preset.engineType, true);
            rig.seq.getPattern().getTrack(0).engineType = preset.engineType;
            const ParamMap sound = PresetManager::soundOf(preset);
            rig.audio.applyAllTrackParameters(0, sound);
            for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
            {
                auto it = sound.find(ProjectBridge::INSERT_TYPE_BASE + slot);
                if (it != sound.end() && std::isfinite(it->second) && it->second >= 0.5f)
                    rig.audio.addInsertEffect(0, (EffectType) (juce::jlimit(1, (int) EffectType::NumEffectTypes, (int) juce::jlimit(0.0f, 1000.0f, it->second)) - 1));
            }
            rig.audio.applyAllTrackParameters(0, sound);
            ProjectBridge::applyArp(preset.arp, rig.arps[0]);
            expect(rig.play(12), what);
            expectEquals(rig.audio.getNonFiniteBlocks(), 0, what + ": blocks dropped as not finite, the last from "
                                                            + juce::String(rig.audio.getNonFiniteTrack()));
        }
    }

    // ---- 8 ------------------------------------------------------------------------------------
    void generatorsAtTheirEnds()
    {
        beginTest("Arpeggiators and the pad roll: every mode and rate, tempo 20 and 300, 8 to 384 kHz, settings from "
                  "a damaged file");
        using namespace robust;
        juce::Random rng(31);
        for (double sr : { 8000.0, 48000.0, 384000.0 })
            for (float bpm : { 20.0f, 300.0f })
            {
                const juce::String what = "generators at " + juce::String(sr, 0) + " Hz, " + juce::String(bpm, 0) + " BPM";
                const Armed armed(dog, what, 300.0);
                Rig rig(sr, 256);
                rig.seq.setTempo(bpm);
                auto& roll = rig.audio.getPadRatchet();
                roll.setEnabled(true);
                bool ok = true;
                for (int mode = 0; mode < (int) Arpeggiator::Mode::NumModes; ++mode)
                    for (int rate = mode % 4; rate < (int) Arpeggiator::Rate::NumRates; rate += (deep() ? 1 : 4))
                    {
                        for (int t = 0; t < 8; ++t)
                        {
                            auto& arp = rig.arps[(size_t) t];
                            std::vector<float> state = ProjectBridge::captureArp(arp);
                            if (t >= 4)
                                for (auto& v : state)                       // a damaged file's arp
                                    if (rng.nextInt(3) == 0)
                                        v = absurdValues()[(size_t) rng.nextInt((int) absurdValues().size())];
                            ProjectBridge::applyArp(state, arp);
                            arp.setMode((Arpeggiator::Mode) mode);
                            arp.setRate((Arpeggiator::Rate) rate);
                            arp.setOctaveRange(1 + (mode + t) % 8);
                            arp.setEnabled(true);
                        }
                        roll.setMode(mode % 2 == 0 ? PadRatchet::Mode::Quantized : PadRatchet::Mode::Free);
                        roll.setBaseDivision(mode % (int) PadRatchet::kNumDivisions);
                        roll.setMaxDivision((int) PadRatchet::kNumDivisions - 1 - rate % (int) PadRatchet::kNumDivisions);
                        roll.setBaseRateHz(rate % 2 == 0 ? 0.0f : 1.0e9f);
                        roll.setMaxRateHz(rate % 2 == 0 ? 1.0e9f : 0.0f);
                        ok = rig.play(deep() ? 40 : 6, 256) && ok;
                        for (int pad = 0; pad < 16; ++pad)
                            rig.audio.padNoteOff(pad);
                    }
                expect(ok, what);
                expectEquals(rig.audio.getNonFiniteBlocks(), 0, what + ": blocks dropped as not finite, the last from "
                                                                + juce::String(rig.audio.getNonFiniteTrack()));
            }
    }

    // ---- 9 ------------------------------------------------------------------------------------
    // The screens as the app wires them (the help-card tests' rig, with recordings kept in memory).
    struct UiRig
    {
        SequencerCore seq;
        AudioEngine audio;
        PadManager pads;
        PadLEDFeedback leds;
        LaunchkeyController controller;
        DisplayRenderer display;
        PresetManager presets;
        CustomScaleManager scales;
        ChordPads chordPads;
        SongMode song;
        Arpeggiator arp;
        ScreenManager mgr;

        UiRig()
        {
            mgr.setHelpBusyWindow(0);
            seq.prepareToPlay(48000.0, 64);
            audio.setSequencer(&seq);
            RobustnessTests::wire(seq, audio);
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
            {
                const EngineType type = robust::soundEngine((t + 6) % robust::NUM_SOUND_ENGINES);     // track 2 is the drum kit
                audio.addEngine(t, AudioEngine::createEngine(t == 1 ? EngineType::Drum : type));
                seq.getPattern().getTrack(t).engineType = audio.getEngineTypeOf(t);
            }
            audio.prepare(48000.0, 64);
            display.setController(&controller);
            mgr.setSequencer(&seq);
            mgr.setDisplay(&display);
            mgr.setEngineHooks(GrammarHooks::make(seq, audio));

            auto* steps = new StepViewScreen(seq, pads, leds);
            auto* param = new ParamPageScreen(seq, &audio);
            auto* wave = new WaveformScreen(seq, &audio);
            wave->setWriteFiles(false);
            steps->setLockPages(param);
            add(new HomeScreen(seq, &audio));
            add(steps);
            add(param);
            // (As main.cpp: engines are swapped at any time, so the drum screen looks its engine up.)
            auto* drums = new DrumPadScreen(nullptr);
            drums->setEngineProvider([this]() -> DrumEngine*
            {
                if (auto* d = dynamic_cast<DrumEngine*>(audio.getEngine(seq.getSelectedTrack())))
                    return d;
                for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                    if (auto* d = dynamic_cast<DrumEngine*>(audio.getEngine(t)))
                        return d;
                return nullptr;
            });
            add(drums);
            add(new PresetScreen(seq, &audio, presets));
            add(new SettingsScreen(SettingsScreen::Hooks()));
            add(new FilterViewScreen(seq, &audio));
            add(new ArpScreen(arp));
            add(new CustomScaleScreen(scales));
            add(new SongScreen(song, seq));
            add(new AftertouchScreen(audio, [this] { return seq.getSelectedTrack(); }));
            add(wave);
            add(new TrackFxScreen(seq, &audio));
            add(new ChordPadScreen(chordPads));

            static constexpr int map[16] = { 2, 0, 12, 1, 4, 9, 5, 10, 1, 3, 13, 8, 11, 6, -1, -1 };
            for (int pad = 0; pad < 16; ++pad)
                mgr.setShiftPadScreen(pad, map[pad]);
            mgr.setButtonScreens(5, 7, 8);
            mgr.setStepScreen(1);
            mgr.setPinnable(3, true);
            mgr.setPinnable(8, true);
            mgr.setPinnable(13, true);
            mgr.setScreenContext(1, ScreenManager::CopyContext::Steps);
            mgr.setScreenContext(0, ScreenManager::CopyContext::Pattern);
            mgr.setScreenContext(2, ScreenManager::CopyContext::Sound);
            mgr.setScreenContext(6, ScreenManager::CopyContext::Sound);
            mgr.setScreenContext(12, ScreenManager::CopyContext::Fx);
            mgr.projectLoaded();
            mgr.setActiveScreen(0);
        }

        void add(Screen* s) { mgr.addScreen(std::unique_ptr<Screen>(s)); }
    };

    void randomControllerInput()
    {
        beginTest("Anything the controller can send, in any order: buttons, modifiers, pads, keys, knobs, pressure, on "
                  "every screen, with the transport running and time passing");
        using namespace robust;
        const int rounds = deep() ? 40 : 4, messages = deep() ? 20000 : 2500;
        for (int round = 0; round < rounds; ++round)
        {
            const juce::String what = "random controller input, round " + juce::String(round);
            const Armed armed(dog, what, 300.0);
            juce::Random rng(1000 + round);
            UiRig rig;
            std::vector<float> l(64), r(64);
            bool ok = true;
            static const int buttons[] = { 63, 104, 105, 106, 107, 108, 109, 110, 115, 117, 51, 52, 102, 103 };
            for (int i = 0; i < messages; ++i)
            {
                const int kind = rng.nextInt(20);
                if (kind < 5)           // a button or modifier, down or up
                {
                    const int cc = buttons[rng.nextInt((int) std::size(buttons))];
                    rig.mgr.controlChangeReceived(cc == 63 ? 7 : 1, cc, rng.nextBool() ? 127 : 0);
                }
                else if (kind < 10)     // a knob, a click or many, either way
                    rig.mgr.controlChangeReceived(16, 0x15 + rng.nextInt(8), rng.nextBool() ? 1 + rng.nextInt(63) : 65 + rng.nextInt(63));
                else if (kind < 14)     // a pad
                {
                    const int pad = rng.nextInt(16);
                    const int note = pad < 8 ? LaunchkeyController::PAD_TOP_ROW_START + pad
                                             : LaunchkeyController::PAD_BOTTOM_ROW_START + (pad - 8);
                    const int what3 = rng.nextInt(3);
                    if (what3 == 0)      rig.mgr.noteOnReceived(1, note, 1 + rng.nextInt(127));
                    else if (what3 == 1) rig.mgr.noteOffReceived(1, note);
                    else                 rig.mgr.aftertouchReceived(1, note, rng.nextInt(128));
                }
                else if (kind < 16)     // a key
                {
                    const int note = rng.nextInt(128);
                    if (rng.nextBool()) rig.mgr.noteOnReceived(LaunchkeyController::KEYS_PORT_CHANNEL, note, rng.nextInt(128));
                    else                rig.mgr.noteOffReceived(LaunchkeyController::KEYS_PORT_CHANNEL, note);
                }
                else if (kind < 17)     // something no control sends
                {
                    rig.mgr.controlChangeReceived(rng.nextInt(20) - 1, rng.nextInt(140) - 6, rng.nextInt(140) - 6);
                    rig.mgr.noteOnReceived(rng.nextInt(20) - 1, rng.nextInt(140) - 6, rng.nextInt(140) - 6);
                    rig.mgr.aftertouchReceived(rng.nextInt(20) - 1, rng.nextInt(140) - 6, rng.nextInt(140) - 6);
                    rig.mgr.noteOffReceived(rng.nextInt(20) - 1, rng.nextInt(140) - 6);
                }
                else                    // time passes: the refresh, the audio, sometimes long enough for a hold
                {
                    UiClock::advance(kind == 17 ? 33 : rng.nextInt(1500));
                    {
                        const juce::ScopedLock ui(rig.mgr.getLock());
                        rig.audio.motionPoll();
                        rig.mgr.requestRedraw();
                        rig.mgr.redrawIfNeeded();
                    }
                    for (int b = 0; b < 4; ++b)
                    {
                        rig.audio.renderBlock(l.data(), r.data(), 64);
                        ok = ok && allFinite(l.data(), 64) && allFinite(r.data(), 64);
                    }
                }
            }
            expect(ok, what);
            expectEquals(rig.audio.getNonFiniteBlocks(), 0, what + ": blocks dropped as not finite, the last from "
                                                            + juce::String(rig.audio.getNonFiniteTrack()));
        }
    }

    void runTest() override
    {
        // NEPTUNE_ROBUST_ONLY=<1..9>: one part only (while looking for the cause of a report).
        const int only = juce::SystemStats::getEnvironmentVariable("NEPTUNE_ROBUST_ONLY", {}).getIntValue();
        if (only == 0 || only == 1) enginesAtTheirEnds();
        if (only == 0 || only == 2) absurdTrackParameters();
        if (only == 0 || only == 3) effectsAtTheirEnds();
        if (only == 0 || only == 4) wholeInstrumentAtTheEnds();
        if (only == 0 || only == 5) tinySamples();
        if (only == 0 || only == 6) hostileProjects();
        if (only == 0 || only == 7) hostilePresets();
        if (only == 0 || only == 8) generatorsAtTheirEnds();
        if (only == 0 || only == 9) randomControllerInput();
    }
};

static RobustnessTests robustnessTests;
