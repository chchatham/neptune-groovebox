#pragma once
#include <juce_core/juce_core.h>
#include "live_record_tests.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Metronome.h"
#include "../src/project/ProjectBridge.h"
#include "../src/ui/screens/SettingsScreen.h"

// The metronome: the sequencer's beat ticks and count-in, the click's sound, where it is mixed
// (past the master chain, never into a resample), the CLICK page of SETTINGS, Record + Play with
// a count-in through ScreenManager, and that the settings are the app's, not a project's.
namespace metrotest
{
    static constexpr int BEAT = 22050;      // samples per beat at 120 BPM, 44.1 kHz

    // Runs a sequencer and notes every beat tick with its absolute sample.
    struct Ticker
    {
        struct Tick { juce::int64 at; bool accent; bool countIn; };
        SequencerCore& seq;
        juce::int64 now = 0;
        std::vector<Tick> ticks;
        juce::int64 startedAt = -1;         // the block-exact sample the transport began playing

        explicit Ticker(SequencerCore& s) : seq(s) {}
        void run(juce::int64 samples, int block = 64)
        {
            const auto end = now + samples;
            while (now < end)
            {
                const int n = (int) juce::jmin((juce::int64) block, end - now);
                const bool was = seq.isPlaying();
                seq.processBlock(n);
                for (int i = 0; i < seq.getBeatTickCount(); ++i)
                {
                    const auto& t = seq.getBeatTick(i);
                    ticks.push_back({ now + t.sample, t.accent, t.countIn });
                    if (!was && !t.countIn && startedAt < 0)
                        startedAt = now + t.sample;
                }
                now += n;
            }
        }
    };

    // The whole instrument with silent tracks: whatever comes out is the click.
    struct Rig
    {
        static constexpr int BLOCK = 256;
        SequencerCore seq;
        AudioEngine audio;
        std::vector<float> l = std::vector<float>(BLOCK), r = std::vector<float>(BLOCK);
        std::vector<float> out;             // left channel of everything rendered

        Rig()
        {
            seq.prepareToPlay(44100.0, BLOCK);
            audio.setSequencer(&seq);
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                audio.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.prepare(44100.0, BLOCK);
        }

        void render(int samples)
        {
            for (int done = 0; done < samples; done += BLOCK)
            {
                audio.renderBlock(l.data(), r.data(), BLOCK);
                out.insert(out.end(), l.begin(), l.end());
            }
        }
        float peak(int from, int to) const
        {
            float p = 0.0f;
            for (int i = juce::jmax(0, from); i < juce::jmin(to, (int) out.size()); ++i)
                p = std::max(p, std::abs(out[(size_t) i]));
            return p;
        }
        // The first sample louder than `threshold` at or after `from` (-1 = none).
        int onset(int from, float threshold = 1.0e-4f) const
        {
            for (int i = from; i < (int) out.size(); ++i)
                if (std::abs(out[(size_t) i]) > threshold)
                    return i;
            return -1;
        }
    };

    struct StartCounter : SequencerCore::Listener
    {
        int started = 0, stopped = 0;
        void transportStarted() override { ++started; }
        void transportStopped() override { ++stopped; }
    };
}

class MetronomeTests : public juce::UnitTest
{
public:
    MetronomeTests() : UnitTest("Metronome") {}

    void runTest() override
    {
        using namespace metrotest;
        using Mode = Metronome::Mode;

        beginTest("Beat ticks: one per beat on the clock's own sample, the bar's first accented, from the first sample");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 64);
            Ticker t(seq);
            t.run(1000);
            expectEquals((int) t.ticks.size(), 0, "nothing while stopped");

            seq.start();
            t.now = 0;
            t.run(9 * BEAT - 100);
            expectEquals((int) t.ticks.size(), 9);
            for (int i = 0; i < (int) t.ticks.size(); ++i)
            {
                // (the clock counts a beat as done on its last sample, as it does a step)
                expect(std::abs(t.ticks[(size_t) i].at - (juce::int64) i * BEAT) <= 1,
                       "beat " + juce::String(i) + " at " + juce::String(t.ticks[(size_t) i].at));
                expect(t.ticks[(size_t) i].accent == (i % 4 == 0), "accent of beat " + juce::String(i));
                expect(!t.ticks[(size_t) i].countIn);
            }
        }

        beginTest("Beat ticks fall on the sample a step starts, whatever the block size");
        {
            for (int block : { 1, 64, 441, 4096 })
            {
                SequencerCore seq;
                seq.prepareToPlay(44100.0, block);
                auto& tr = seq.getPattern().getTrack(0);
                for (int s = 0; s < 16; s += 4)
                {
                    tr.steps[(size_t) s].trig = true;
                    tr.steps[(size_t) s].notes = { Note { 60, 0.8f } };
                }
                std::vector<juce::int64> ticksAt;
                int trigs = 0, trigsBeforeTick = 0;
                // Within one processBlock a trig of step 0 / 4 / 8 / 12 and its beat's tick come
                // from the same sample: count the trigs seen and compare block by block.
                seq.setTrigCallback([&](int, const Step&) { ++trigs; });
                seq.start();
                juce::int64 now = 0;
                int ticks = 0;
                const juce::int64 end = 5 * BEAT - 50;
                while (now < end)
                {
                    const int n = (int) juce::jmin((juce::int64) block, end - now);
                    seq.processBlock(n);
                    ticks += seq.getBeatTickCount();
                    if (trigs != ticks) ++trigsBeforeTick;
                    now += n;
                }
                expectEquals(ticks, 5, "block " + juce::String(block));
                expectEquals(trigs, 5);
                expectEquals(trigsBeforeTick, 0, "a beat and its step in different blocks, block " + juce::String(block));
            }
        }

        beginTest("Beat ticks follow a tempo change and end with the transport");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 64);
            Ticker t(seq);
            seq.start();
            t.run(BEAT + 64);                       // beats 0 and 1 at 120
            seq.setTempo(60.0f);
            t.run(2 * 44100);
            expect(t.ticks.size() >= 3);
            if (t.ticks.size() >= 3)
            {
                // (the beat under way is re-measured at the new tempo from where it stands)
                const auto gap = t.ticks[2].at - t.ticks[1].at;
                expect(std::abs(gap - 44100) <= 64, "gap after the change: " + juce::String(gap));
            }
            seq.stop();
            const auto before = t.ticks.size();
            t.run(3 * 44100);
            expectEquals((int) t.ticks.size(), (int) before, "no beat after stop");
            seq.start();
            t.run(64);
            expectEquals((int) t.ticks.size(), (int) before + 1);
            expect(t.ticks.back().accent, "a new start begins on an accent");
        }

        beginTest("Count-in: only with recording armed; the transport starts on the sample it ends");
        {
            for (int bars = 0; bars <= 2; ++bars)
            {
                SequencerCore seq;
                seq.prepareToPlay(44100.0, 64);
                StartCounter listener;
                seq.addListener(&listener);
                auto& tr = seq.getPattern().getTrack(0);
                tr.steps[0].trig = true;
                tr.steps[0].notes = { Note { 60, 0.8f } };
                int trigs = 0;
                seq.setTrigCallback([&](int, const Step&) { ++trigs; });
                seq.setCountInBars(bars);
                expectEquals(seq.getCountInBars(), bars);

                // Not recording: Play starts at once.
                seq.requestStart();
                expect(seq.isPlaying() && !seq.isCountingIn(), "no count-in without recording");
                seq.stop();
                expectEquals(listener.started, 1);

                seq.setRecording(true);
                seq.requestStart();
                Ticker t(seq);
                if (bars == 0)
                {
                    expect(seq.isPlaying());
                    continue;
                }
                expect(!seq.isPlaying() && seq.isCountingIn() && seq.isRunning());
                expectEquals(seq.getCountInBeatsLeft(), bars * 4);
                expectEquals(listener.started, 1, "not started yet");

                seq.selectPattern(3, SequencerCore::ChangeMode::NextBeat);
                t.run((juce::int64) bars * 4 * BEAT - 64);
                expect(!seq.isPlaying(), "still counting one block before the end");
                expectEquals(trigs, 0, "nothing plays during the count-in");
                expectEquals(seq.getCurrentStep(0), 0);
                expectEquals(seq.getActivePatternIndex(), 0, "a queued pattern waits");
                expectWithinAbsoluteError(seq.getBeatPosition(), 0.0, 1.0e-9);
                expectEquals((int) t.ticks.size(), bars * 4);
                for (int i = 0; i < (int) t.ticks.size(); ++i)
                {
                    expectEquals((int) t.ticks[(size_t) i].at, i * BEAT);
                    expect(t.ticks[(size_t) i].countIn);
                    expect(t.ticks[(size_t) i].accent == (i % 4 == 0));
                }

                t.run(128);
                expect(seq.isPlaying() && !seq.isCountingIn());
                expect(seq.isRecording(), "recording stays armed through the count-in");
                expectEquals(listener.started, 2);
                expectEquals((int) t.startedAt, bars * 4 * BEAT, "the transport starts exactly where the count-in ends");
                expect(!t.ticks.back().countIn && t.ticks.back().accent);
                expectEquals(trigs, 1, "step 1 plays as the transport starts");
                expect(seq.consumeCountInStart());
                expect(!seq.consumeCountInStart(), "reported once");
                expectEquals(seq.getActivePatternIndex(), 0);
                t.run(BEAT);
                expectEquals(seq.getActivePatternIndex(), 3, "the queued pattern comes at the first beat played");
                seq.removeListener(&listener);
            }
        }

        beginTest("Count-in: stop calls it off, start skips it, it follows the tempo");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 64);
            seq.setCountInBars(1);
            seq.setRecording(true);
            Ticker t(seq);

            seq.requestStart();
            t.run(2 * BEAT);
            seq.stop();
            expect(!seq.isRunning());
            const auto n = t.ticks.size();
            t.run(8 * BEAT);
            expect(!seq.isPlaying(), "a count-in called off never starts the transport");
            expectEquals((int) t.ticks.size(), (int) n);

            seq.requestStart();
            t.run(BEAT);
            seq.start();
            expect(seq.isPlaying() && !seq.isCountingIn());
            seq.stop();

            // A new count-in begins with its first beat, at 240 BPM half as long.
            seq.setTempo(240.0f);
            seq.requestStart();
            Ticker fast(seq);
            fast.run(4 * BEAT);
            expectEquals((int) fast.startedAt, 4 * BEAT / 2);
            expect(fast.ticks.size() >= 4 && fast.ticks[0].at == 0 && fast.ticks[0].accent);
        }

        beginTest("The click: starts on the beat's sample, accent higher and louder, over in 40 ms, level scales it");
        {
            auto renderClick = [this](bool accent, float level, int at)
            {
                Metronome m;
                m.prepare(44100.0);
                m.setMode(Mode::Play);
                m.setLevel(level);
                std::vector<float> l(4096, 0.0f), r(4096, 0.0f);
                m.beginBlock(true);
                m.beat(at, accent, false, false);
                m.render(l.data(), r.data(), 4096);
                expect(l == r);
                return l;
            };
            auto peakOf = [](const std::vector<float>& v) { float p = 0; for (float x : v) p = std::max(p, std::abs(x)); return p; };
            auto crossings = [](const std::vector<float>& v, int from, int to)
            {
                int c = 0;
                for (int i = from + 1; i < to; ++i) if ((v[(size_t) i - 1] < 0.0f) != (v[(size_t) i] < 0.0f)) ++c;
                return c;
            };
            const auto beat = renderClick(false, 1.0f, 100);
            const auto accent = renderClick(true, 1.0f, 100);
            for (int i = 0; i <= 100; ++i)
                expect(beat[(size_t) i] == 0.0f && accent[(size_t) i] == 0.0f, "silent up to the beat (it starts at a zero crossing)");
            expect(beat[101] != 0.0f && accent[101] != 0.0f);
            const float pb = peakOf(beat), pa = peakOf(accent);
            logMessage("click peaks at level 1: beat " + juce::String(pb, 3) + ", accent " + juce::String(pa, 3));
            expect(pa > pb * 1.5f && pa <= 0.9f && pb > 0.3f);
            const int cb = crossings(beat, 101, 101 + 441), ca = crossings(accent, 101, 101 + 441);
            expectWithinAbsoluteError(cb, 17, 2);       // 880 Hz for 10 ms
            expectWithinAbsoluteError(ca, 35, 2);       // 1760 Hz
            for (int i = 100 + 1764; i < 4096; ++i)
                expect(beat[(size_t) i] == 0.0f && accent[(size_t) i] == 0.0f);
            expect(std::abs(beat[100 + 1700]) < 0.002f, "decayed before it is cut");

            const auto half = renderClick(true, 0.5f, 100);
            expectWithinAbsoluteError(peakOf(half), pa * 0.5f, 1.0e-4f);
            expectEquals(peakOf(renderClick(true, 0.0f, 100)), 0.0f);
        }

        beginTest("Modes: OFF never, REC only while recording, PLAY always; a count-in always");
        {
            Metronome m;
            expect(m.getMode() == Mode::Record, "as shipped: the click while recording");
            expectWithinAbsoluteError(m.getLevel(), 0.5f, 1.0e-6f);
            m.setMode(Mode::Off);
            expect(!m.wants(false, false) && !m.wants(false, true) && m.wants(true, false));
            m.setMode(Mode::Record);
            expect(!m.wants(false, false) && m.wants(false, true) && m.wants(true, false));
            m.setMode(Mode::Play);
            expect(m.wants(false, false) && m.wants(false, true) && m.wants(true, true));
        }

        beginTest("In the instrument: clicks on the beats while the transport runs, by mode, and gone when it stops");
        {
            Rig rig;
            auto& m = rig.audio.getMetronome();
            rig.render(BEAT);
            expectEquals(rig.peak(0, BEAT), 0.0f, "stopped: silent");

            // REC (the default): playing without recording is silent.
            rig.seq.start();
            rig.out.clear();
            rig.render(2 * BEAT);
            expectEquals(rig.peak(0, 2 * BEAT), 0.0f, "REC mode, not recording");
            rig.seq.stop();

            rig.seq.setRecording(true);
            rig.seq.start();
            rig.out.clear();
            rig.render(5 * BEAT);
            for (int b = 0; b < 5; ++b)
            {
                const int at = rig.onset(b == 0 ? 0 : b * BEAT - 200);
                expect(std::abs(at - b * BEAT) <= 2, "click " + juce::String(b) + " at " + juce::String(at));
                expectEquals(rig.peak(b * BEAT + 1800, (b + 1) * BEAT - 200), 0.0f, "silent between clicks");
            }
            expect(rig.peak(0, 1800) > rig.peak(BEAT, BEAT + 1800) * 1.5f, "the bar's first beat is accented");
            expectWithinAbsoluteError(rig.peak(0, 1800), rig.peak(4 * BEAT - 2, 4 * BEAT + 1800), 0.01f);
            rig.seq.stop();
            rig.seq.setRecording(false);
            rig.render(4410);

            m.setMode(Mode::Off);
            rig.seq.start();
            rig.out.clear();
            rig.render(2 * BEAT);
            expectEquals(rig.peak(0, 2 * BEAT), 0.0f, "OFF");
            rig.seq.stop();

            // PLAY; stopped in the middle of a click: gone within 10 ms, and no beat after.
            m.setMode(Mode::Play);
            rig.seq.start();
            rig.out.clear();
            rig.render(Rig::BLOCK);
            expect(rig.peak(0, Rig::BLOCK) > 0.1f);
            rig.seq.stop();
            rig.render(3 * BEAT);
            expect(rig.peak(Rig::BLOCK, 2 * Rig::BLOCK) > 0.0f, "faded, not chopped");
            expectEquals(rig.peak(Rig::BLOCK + 441, (int) rig.out.size()), 0.0f, "silent 10 ms after stop");
        }

        beginTest("The click passes no master effect: the same samples at any master volume and compression");
        {
            auto clicks = [this](bool squashed)
            {
                Rig rig;
                rig.audio.getMetronome().setMode(Mode::Play);
                if (squashed)
                {
                    rig.audio.setMasterLevel(0.05f);
                    rig.audio.setMasterParam(0, 18.0f);     // LOW
                    rig.audio.setMasterParam(4, -60.0f);    // THR
                    rig.audio.setMasterParam(5, 20.0f);     // RATI
                    rig.audio.setMasterParam(7, -3.0f);     // CEIL
                }
                rig.seq.start();
                rig.render(2 * BEAT);
                expect(rig.peak(0, 2 * BEAT) > 0.1f);
                return rig.out;
            };
            expect(clicks(false) == clicks(true), "bit-identical with the master chain at its extremes");
        }

        beginTest("The click is never resampled: master and track takes stay empty while it sounds");
        {
            for (int source : { (int) Resampler::MASTER, 0 })
            {
                Rig rig;
                rig.audio.getMetronome().setMode(Mode::Play);
                rig.audio.getMetronome().setLevel(1.0f);
                rig.seq.setCountInBars(1);
                rig.seq.setRecording(true);
                auto& rec = rig.audio.getResampler();
                rec.setSource(source);
                rec.setThreshold(0.0f);
                rec.setBarStart(false);
                rec.arm();
                rig.seq.requestStart();             // a bar of count-in clicks, then a bar of beats
                rig.render(8 * BEAT);
                expect(rig.peak(0, 4 * BEAT) > 0.3f, "count-in clicks are heard");
                expect(rig.peak(4 * BEAT, 8 * BEAT) > 0.3f, "beat clicks are heard");
                expect(rec.length() >= 8 * BEAT, "the take ran all the while");
                float taken = 0.0f;
                for (int i = 0; i < rec.length(); ++i)
                    taken = std::max(taken, std::max(std::abs(rec.left()[i]), std::abs(rec.right()[i])));
                expectEquals(taken, 0.0f, "source " + juce::String(source));
            }
        }

        beginTest("In the instrument: the count-in clicks even with CLCK off; the MIDI clock starts when it ends");
        {
            Rig rig;
            MIDIClock clock;
            clock.prepareToPlay(44100.0, Rig::BLOCK);
            rig.audio.setMIDIClock(&clock);
            rig.audio.getMetronome().setMode(Mode::Off);
            rig.seq.setCountInBars(1);
            rig.seq.setRecording(true);
            rig.seq.requestStart();
            rig.render(4 * BEAT - Rig::BLOCK);
            expect(!clock.isRunning(), "no clock during the count-in");
            for (int b = 0; b < 4; ++b)
                expect(std::abs(rig.onset(b == 0 ? 0 : b * BEAT - 200) - b * BEAT) <= 2, "count-in click " + juce::String(b));
            rig.render(BEAT);
            expect(rig.seq.isPlaying());
            expect(clock.isRunning(), "the clock starts with the transport");
            expectEquals(rig.peak(4 * BEAT - 200, (int) rig.out.size()), 0.0f, "CLCK off: no click once recording runs");
        }

        beginTest("Audition: one click at once with the transport stopped and CLCK off");
        {
            Rig rig;
            rig.audio.getMetronome().setMode(Mode::Off);
            rig.audio.getMetronome().audition();
            rig.render(BEAT);
            expect(rig.peak(0, 1764) > 0.1f, "heard, and not faded as a stopped transport's click is");
            expect(rig.peak(1000, 1764) > 1.0e-4f);
            expectEquals(rig.peak(1764 + Rig::BLOCK, BEAT), 0.0f, "once");
        }

        beginTest("ScreenManager: Record + Play counts in, then records; Play during the count-in calls it off");
        {
            lrtest::Rig r;
            lrtest::RecordingKeys keys(r.seq);
            r.mgr.setKeyboardListener(&keys);
            r.seq.setCountInBars(1);
            Ticker t(r.seq);

            lrtest::recordPlay(r);
            expect(r.seq.isCountingIn() && !r.seq.isPlaying() && r.seq.isRecording());
            expectEquals(r.text(), juce::String("COUNT IN 1 BAR"));
            expect(r.mgr.isTransportRunning(), "a count-in counts as running: Play stops it");

            // A key during the count-in sounds but is not written.
            t.run(2 * BEAT);
            r.keyOn(60); r.keyOff(60);
            expect(keys.log.contains("on60"));
            expect(lrtest::findNote(r.seq, 0, 60) == nullptr, "nothing is recorded during the count-in");

            t.run(2 * BEAT + 64);
            expect(r.seq.isPlaying() && r.seq.isRecording());
            r.keyOn(62); r.keyOff(62);
            expect(lrtest::findNote(r.seq, 0, 62) != nullptr, "recording runs after the count-in");
            r.tap(lrtest::PLAY);
            expect(!r.seq.isRunning() && !r.seq.isRecording());

            // Record tapped (armed), then Play: the same count-in; Play again calls it off.
            r.tap(lrtest::RECORD);
            expect(r.seq.isRecording());
            r.tap(lrtest::PLAY);
            expect(r.seq.isCountingIn());
            expectEquals(r.text(), juce::String("COUNT IN 1 BAR"));
            r.tap(lrtest::PLAY);
            expect(!r.seq.isRunning(), "called off");
            expect(!r.seq.isRecording(), "and recording is off, as after any stop");
            t.run(8 * BEAT);
            expect(!r.seq.isPlaying());

            // Two bars; Shift + Play calls it off too. Plain Play without recording never counts in.
            r.seq.setCountInBars(2);
            lrtest::recordPlay(r);
            expectEquals(r.text(), juce::String("COUNT IN 2 BARS"));
            r.shift(true); r.tap(lrtest::PLAY); r.shift(false);
            expect(!r.seq.isRunning() && !r.seq.isRecording());
            r.tap(lrtest::PLAY);
            expect(r.seq.isPlaying() && !r.seq.isCountingIn());
            r.tap(lrtest::PLAY);

            // The Record help card says what Record + Play will do.
            r.seq.setCountInBars(0);
            lrtest::recordPlay(r);
            expect(r.seq.isPlaying(), "CNT off: at once, as before");
            expectEquals(r.text(), juce::String("REC FREE"));
        }

        beginTest("SETTINGS: the CLICK page, its three cells, choices follow the turn direction");
        {
            int mode = 1, countIn = 0, levelSets = 0;
            float level = 0.5f;
            SettingsScreen::Hooks h;
            h.getClickMode = [&] { return mode; };       h.setClickMode = [&](int v) { mode = v; };
            h.getClickLevel = [&] { return level; };     h.setClickLevel = [&](float v) { level = v; ++levelSets; };
            h.getCountIn = [&] { return countIn; };      h.setCountIn = [&](int v) { countIn = v; };
            SettingsScreen screen(h);
            expectEquals(screen.getPageCount(), 4);
            expectEquals(screen.getPageKeyCount(), 4);
            expectEquals(screen.getPageKeyName(3), juce::String("CLICK"));
            screen.onPageKey(3);
            expectEquals(screen.getName(), juce::String("CLICK"));
            expectEquals(screen.getEncoderName(0), juce::String("CLCK"));
            expectEquals(screen.getEncoderName(1), juce::String("CLVL"));
            expectEquals(screen.getEncoderName(2), juce::String("CNT"));
            expectEquals(screen.getEncoderName(3), juce::String("---"));

            screen.onEncoderChange(0, 1);       expectEquals(mode, 2);
            screen.onEncoderChange(0, 1);       expectEquals(mode, 2, "stays at PLAY");
            screen.onEncoderChange(0, 127);     expectEquals(mode, 1);
            screen.onEncoderChange(0, 127);     expectEquals(mode, 0);
            screen.onEncoderChange(0, 127);     expectEquals(mode, 0, "stays at OFF");

            screen.onEncoderChange(2, 1);       expectEquals(countIn, 1);
            screen.onEncoderChange(2, 1);       expectEquals(countIn, 2);
            screen.onEncoderChange(2, 1);       expectEquals(countIn, 2);
            screen.onEncoderChange(2, 127);     expectEquals(countIn, 1);

            screen.onEncoderChange(1, 10);
            expectWithinAbsoluteError(level, 0.6f, 0.001f);
            screen.onEncoderChange(1, 127);
            expectWithinAbsoluteError(level, 0.59f, 0.001f);
            expectEquals(levelSets, 2, "every turn reaches the app (which plays a click)");

            // Through the screen manager: Func + top pad 4 is the page key.
            cgtest::Rig r;
            auto* settings = new SettingsScreen(h);
            r.mgr.addScreen(std::unique_ptr<Screen>(settings));
            r.mgr.setActiveScreen(2);
            r.func(true); r.pad(3); r.func(false);
            expectEquals(settings->getName(), juce::String("CLICK"));
            mode = 1;
            r.turn(0, 1);
            expectEquals(mode, 2, "encoder 1 reaches CLCK through ScreenManager");
        }

        beginTest("The settings are the app's: a text line that reads back, and no project changes them");
        {
            Metronome::Settings s;
            expectEquals(s.mode, 1);
            expectEquals(s.countInBars, 0);
            s.mode = 2; s.level = 0.37f; s.countInBars = 2;
            const auto back = Metronome::fromText(Metronome::toText(s), SequencerCore::MAX_COUNT_IN_BARS);
            expectEquals(back.mode, 2);
            expectWithinAbsoluteError(back.level, 0.37f, 0.001f);
            expectEquals(back.countInBars, 2);

            const auto junk = Metronome::fromText("x 900 77", 2);
            expectEquals(junk.mode, 1, "unreadable: the default");
            expectWithinAbsoluteError(junk.level, 1.0f, 1.0e-6f);
            expectEquals(junk.countInBars, 2, "limited");
            const auto empty = Metronome::fromText("", 2);
            expectEquals(empty.mode, 1);
            expectWithinAbsoluteError(empty.level, 0.5f, 1.0e-6f);
            expectEquals(empty.countInBars, 0);

            // A project saved with one set of metronome settings, loaded into an app with another.
            Rig saved;
            saved.audio.getMetronome().setMode(Mode::Off);
            saved.audio.getMetronome().setLevel(0.1f);
            saved.seq.setCountInBars(0);
            const auto json = ProjectBridge::capture({ saved.seq, saved.audio }, "M").toJSON();
            expect(!json.containsIgnoreCase("metronome")
                   && !json.containsIgnoreCase("countIn"), "nothing of it in the project file");

            Rig live;
            live.audio.getMetronome().setMode(Mode::Play);
            live.audio.getMetronome().setLevel(0.8f);
            live.seq.setCountInBars(2);
            ProjectBridge::apply(ProjectData::fromJSON(json), { live.seq, live.audio });
            expect(live.audio.getMetronome().getMode() == Mode::Play, "loading a project keeps the mode");
            expectWithinAbsoluteError(live.audio.getMetronome().getLevel(), 0.8f, 1.0e-6f);
            expectEquals(live.seq.getCountInBars(), 2);

            // A new project (the app applies an empty project's data).
            Project fresh;
            fresh.newProject();
            ProjectBridge::apply(fresh.getData(), { live.seq, live.audio });
            expect(live.audio.getMetronome().getMode() == Mode::Play, "a new project keeps the mode");
            expectWithinAbsoluteError(live.audio.getMetronome().getLevel(), 0.8f, 1.0e-6f);
            expectEquals(live.seq.getCountInBars(), 2);
        }
    }
};

static MetronomeTests metronomeTests;
