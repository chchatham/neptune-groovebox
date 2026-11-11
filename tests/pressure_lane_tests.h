#pragma once

#include <juce_core/juce_core.h>
#include "held_step_pages_tests.h"
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include "../src/project/Project.h"
#include "../src/project/ProjectBridge.h"

// Pressure lanes: what a pad's pressure does after the strike is recorded with the note (Record +
// Play) and played back to that note's voice. Driven through the app's own path: the pads go
// through AudioEngine::padNoteOn / padPressure / padNoteOff, every hit is recorded by the
// sequencer, and the sequencer's trigs, note-offs and recorded pressure reach an engine that
// writes down what it is sent and when.
namespace pltest
{
    static constexpr double STEP = 5512.5;      // samples per step at 120 BPM, 44.1 kHz
    static constexpr int BLOCK = 64;

    struct Probe : EngineBase
    {
        struct Event { juce::int64 at; char kind; int note; float value; };     // 'n' on, 'f' off, 'p' pressure
        const juce::int64* now = nullptr;
        std::vector<Event> log;
        EngineType type = EngineType::UnifiedVoice;

        EngineType getType() const override { return type; }
        juce::String getName() const override { return "PROBE"; }
        void prepareToPlay(double, int) override {}
        void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        void noteOn(int note, float velocity, int) override { log.push_back({ *now, 'n', note, velocity }); }
        void noteOff(int note, int) override { log.push_back({ *now, 'f', note, 0.0f }); }
        void allNotesOff() override {}
        void setParameter(ParamId, float) override {}
        float getParameter(ParamId) const override { return 0.0f; }
        juce::String getParameterName(ParamId) const override { return {}; }
        int getParameterCount() const override { return 0; }
        void aftertouch(int note, float pressure) override { log.push_back({ *now, 'p', note, pressure }); }

        int count(char kind, int note = -1) const
        {
            int n = 0;
            for (auto& e : log)
                if (e.kind == kind && (note < 0 || e.note == note)) ++n;
            return n;
        }
        // When `note` first got an event of this kind, in steps (-1 = never).
        double firstAt(char kind, int note) const
        {
            for (auto& e : log)
                if (e.kind == kind && e.note == note) return (double) e.at / STEP;
            return -1.0;
        }
        // The pressure `note` had been sent last at this time (steps); -1 = none yet.
        float pressureAt(int note, double steps) const
        {
            float v = -1.0f;
            for (auto& e : log)
                if (e.kind == 'p' && e.note == note && (double) e.at / STEP <= steps) v = e.value;
            return v;
        }
        float lastPressure(int note) const { return pressureAt(note, 1.0e12); }
    };

    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        std::shared_ptr<Probe> probe = std::make_shared<Probe>();
        juce::int64 now = 0;

        Rig()
        {
            probe->now = &now;
            seq.prepareToPlay(44100.0, 512);
            audio.setSequencer(&seq);
            audio.addEngine(0, probe);
            // As in main.cpp: every hit a pad makes is recorded; trigs and note-offs go to the engine.
            audio.onPadHit = [this](int track, int note, float velocity)
            {
                if (seq.isRecording() && seq.isPlaying())
                    seq.recordNote(track, note, velocity);
            };
            seq.setTrigCallback([this](int track, const Step& step)
            {
                if (auto* e = audio.getEngine(track))
                    for (auto& n : step.notes)
                        e->noteOn(n.pitch, n.velocity, -1);
            });
            seq.setNoteOffCallback([this](int track, int note)
            {
                if (auto* e = audio.getEngine(track))
                    e->noteOff(note, -1);
            });
        }

        void record(SequencerCore::RecordMode mode = SequencerCore::RecordMode::Free)
        {
            seq.setRecordMode(mode);
            seq.setRecording(true);
            seq.start();
        }

        double steps() const { return (double) now / STEP; }

        // One audio block, as the audio callback does it: generators (the pads), then the sequencer.
        void block(int n = BLOCK)
        {
            audio.processGenerators(n);
            seq.processBlock(n);
            now += n;
        }
        void runTo(double toSteps)
        {
            const auto target = (juce::int64) std::llround(toSteps * STEP);
            while (now < target)
                block((int) juce::jmin((juce::int64) BLOCK, target - now));
        }
        // Runs to `toSteps`, calling `each` before every block (to move a pad's pressure).
        template <typename Fn> void runTo(double toSteps, Fn each)
        {
            const auto target = (juce::int64) std::llround(toSteps * STEP);
            while (now < target)
            {
                each();
                block((int) juce::jmin((juce::int64) BLOCK, target - now));
            }
        }

        void padDown(int pad, int note, int track = 0) { audio.padNoteOn(track, pad, note, 0.8f); }
        void padUp(int pad) { audio.padNoteOff(pad); }

        // Pad `pad` (note `note`) pressed at `from` steps, its pressure following fn(steps since the
        // press), let go at `to`.
        template <typename Fn> void playPad(int pad, int note, double from, double to, Fn pressure)
        {
            runTo(from);
            padDown(pad, note);
            runTo(to, [&] { audio.padPressure(pad, pressure(steps() - from)); });
            padUp(pad);
            block();
        }

        Track& track(int t = 0) { return seq.getPattern().getTrack(t); }
        const Note* find(int pitch, int* stepIndex = nullptr, int t = 0)
        {
            auto& tr = track(t);
            for (int s = 0; s < tr.length; ++s)
                for (auto& n : tr.steps[(size_t) s].notes)
                    if (tr.steps[(size_t) s].trig && n.pitch == pitch)
                    {
                        if (stepIndex != nullptr) *stepIndex = s;
                        return &n;
                    }
            return nullptr;
        }
        int lanes(int t = 0)
        {
            int n = 0;
            for (auto& s : track(t).steps)
                for (auto& note : s.notes)
                    if (!note.lane.empty()) ++n;
            return n;
        }
    };

    // A lane rising in a straight line from 0 to `top` over `steps` steps, 8 points per step.
    inline PressureLane ramp(float steps, float top = 1.0f)
    {
        PressureLane lane;
        const int n = (int) std::lround(steps * (float) PressureLane::BASE_POINTS_PER_STEP);
        for (int i = 0; i <= n; ++i)
            lane.points.push_back((uint8_t) std::lround(255.0f * top * (float) i / (float) n));
        return lane;
    }
}

class PressureLaneRecordTests : public juce::UnitTest
{
public:
    PressureLaneRecordTests() : UnitTest("Pressure Lane") {}

    void runTest() override
    {
        using namespace pltest;
        using Mode = SequencerCore::RecordMode;

        beginTest("The lane: straight lines between points, the last point held, twice the spacing when full");
        {
            PressureLane lane = ramp(2.0f);
            expectEquals((int) lane.points.size(), 17);
            expectWithinAbsoluteError(lane.stepsPerPoint(), 0.125f, 1.0e-6f);
            expectWithinAbsoluteError(lane.lengthInSteps(), 2.0f, 1.0e-5f);
            expectWithinAbsoluteError(lane.valueAt(0.0f), 0.0f, 0.005f);
            expectWithinAbsoluteError(lane.valueAt(1.0f), 0.5f, 0.005f);
            expectWithinAbsoluteError(lane.valueAt(0.56f), 0.28f, 0.005f, "between two points");
            expectWithinAbsoluteError(lane.valueAt(5.0f), 1.0f, 0.005f, "after the end the last point is held");
            expectWithinAbsoluteError(lane.meanOver(0.0f, 1.0f), 0.25f, 0.01f);
            expectWithinAbsoluteError(lane.meanOver(1.0f, 2.0f), 0.75f, 0.01f);

            PressureLane empty;
            expect(empty.empty());
            expectEquals((int) empty.points.capacity(), 0, "a note without pressure owns no memory");
            expectWithinAbsoluteError(empty.valueAt(1.0f), 0.0f, 1.0e-6f);
            expect(!empty.append(10), "appending never allocates: no room reserved, no point");

            // 40 steps of a slow ramp: more points than fit at 1/8 step.
            PressureLane longLane;
            longLane.points.reserve((size_t) PressureLane::MAX_POINTS);
            const auto* buffer = longLane.points.data();
            for (int i = 0; i <= 320; ++i)
            {
                const float t = (float) i / 8.0f;
                while ((float) longLane.points.size() * longLane.stepsPerPoint() <= t)
                    if (!longLane.append((uint8_t) std::lround(255.0f * t / 40.0f))) break;
            }
            expectEquals((int) longLane.shift, 1, "the spacing doubled once");
            expect((int) longLane.points.size() <= PressureLane::MAX_POINTS);
            expect(longLane.points.data() == buffer, "still the same buffer");
            expectWithinAbsoluteError(longLane.lengthInSteps(), 40.0f, 0.3f);
            expectWithinAbsoluteError(longLane.valueAt(10.0f), 0.25f, 0.02f);
            expectWithinAbsoluteError(longLane.valueAt(30.0f), 0.75f, 0.02f);
        }

        for (int pass = 0; pass < 2; ++pass)
        {
            const bool quant = pass == 1;
            beginTest(quant ? "REC QUANT: a pad's swell is recorded with its note and played back from the grid"
                            : "REC FREE: a pad's swell is recorded with its note and played back as played");
            Rig r;
            r.record(quant ? Mode::Quant : Mode::Free);
            const int ready = r.seq.getReadyLaneBuffers();
            expectEquals(ready, (int) SequencerCore::LANE_POOL_READY);

            // Struck a little after step 5 starts, pressed harder and harder for two steps.
            r.playPad(0, 60, 4.2, 6.2, [](double t) { return (float) (t / 2.0); });

            int at = -1;
            const Note* note = r.find(60, &at);
            expect(note != nullptr);
            if (note == nullptr) continue;
            auto& step = r.track().getStep(at);
            expectEquals(at, 4);
            expectWithinAbsoluteError(step.microTiming, quant ? 0.0f : 0.2f, 0.02f);
            expect(!note->lane.empty(), "the pressure was recorded");
            expectEquals((int) note->lane.shift, 0);
            expectWithinAbsoluteError(note->lane.lengthInSteps(), 2.0f, 0.15f);
            expectWithinAbsoluteError(step.length, 2.0f, 0.05f, "the note lasts as long as the pad was held");
            expectWithinAbsoluteError(note->lane.valueAt(0.5f), 0.25f, 0.04f);
            expectWithinAbsoluteError(note->lane.valueAt(1.0f), 0.5f, 0.04f);
            expectWithinAbsoluteError(note->lane.valueAt(1.9f), 0.95f, 0.05f);
            // Recorded into a buffer that was waiting, not into one made for it.
            expectEquals((int) note->lane.points.capacity(), (int) PressureLane::MAX_POINTS);
            expectEquals(r.seq.getReadyLaneBuffers(), ready - 1);
            expect(!r.seq.isCapturingPressure(0));

            // The next time round, without the pad.
            r.seq.setRecording(false);
            r.runTo(16.0);
            r.probe->log.clear();
            r.runTo(32.0);
            const double start = quant ? 20.0 : 20.2;
            expectWithinAbsoluteError(r.probe->firstAt('n', 60), start, 0.02);
            expectWithinAbsoluteError(r.probe->firstAt('f', 60), start + 2.0, 0.04);
            expect(r.probe->count('p', 60) > 60, "many small moves, not a few jumps: " + juce::String(r.probe->count('p', 60)));
            for (double t : { 0.25, 0.5, 1.0, 1.5, 1.9 })
                expectWithinAbsoluteError(r.probe->pressureAt(60, start + t), (float) (t / 2.0), 0.05f,
                                          "pressure " + juce::String(t) + " steps into the note");
            expectWithinAbsoluteError(r.probe->lastPressure(60), 0.0f, 1.0e-6f, "let go with the note");
            // The release comes before the note-off, as from a pad.
            juce::int64 zeroAt = -1, offAt = -1;
            for (size_t i = 0; i < r.probe->log.size(); ++i)
            {
                auto& e = r.probe->log[i];
                if (e.kind == 'p' && e.value == 0.0f) zeroAt = (juce::int64) i;
                if (e.kind == 'f') offAt = (juce::int64) i;
            }
            expect(zeroAt >= 0 && zeroAt < offAt);
            expectEquals(r.seq.getActiveLanePlayers(), 0);
            // Never louder steps than a pad sends.
            float last = 0.0f, biggest = 0.0f;
            for (auto& e : r.probe->log)
                if (e.kind == 'p' && e.value > 0.0f) { biggest = juce::jmax(biggest, std::abs(e.value - last)); last = e.value; }
            expect(biggest < 0.03f, "largest step " + juce::String(biggest));
        }

        beginTest("Two pads held together keep their own swells");
        {
            Rig r;
            r.record();
            r.runTo(8.1);
            r.padDown(0, 60);
            r.padDown(1, 64);
            r.runTo(11.1, [&]
            {
                const double t = r.steps() - 8.1;
                r.audio.padPressure(0, (float) (t / 3.0));                              // rises all the way
                r.audio.padPressure(1, t < 1.0 ? (float) (0.6 * t) : (float) juce::jmax(0.0, 0.6 - 0.3 * (t - 1.0)));   // up, then back
            });
            r.padUp(0);
            r.padUp(1);
            r.block();

            int a = -1, b = -1;
            const Note* low = r.find(60, &a);
            const Note* high = r.find(64, &b);
            expect(low != nullptr && high != nullptr && a == b, "one step, two notes");
            if (low == nullptr || high == nullptr) return;
            expect(!low->lane.empty() && !high->lane.empty());
            expect(low->lane != high->lane);

            r.seq.setRecording(false);
            r.runTo(16.0);
            r.probe->log.clear();
            r.runTo(32.0);
            expectWithinAbsoluteError(r.probe->pressureAt(60, 24.1 + 1.0), 1.0f / 3.0f, 0.05f);
            expectWithinAbsoluteError(r.probe->pressureAt(64, 24.1 + 1.0), 0.6f, 0.05f);
            expectWithinAbsoluteError(r.probe->pressureAt(60, 24.1 + 2.5), 2.5f / 3.0f, 0.05f);
            expectWithinAbsoluteError(r.probe->pressureAt(64, 24.1 + 2.5), 0.15f, 0.05f);
            expectWithinAbsoluteError(r.probe->lastPressure(60), 0.0f, 1.0e-6f);
            expectWithinAbsoluteError(r.probe->lastPressure(64), 0.0f, 1.0e-6f);
        }

        beginTest("A note without pressure stores nothing; a pattern without lanes plays as before and sends no pressure");
        {
            Rig r;
            r.record();
            const int ready = r.seq.getReadyLaneBuffers();
            r.playPad(0, 60, 2.1, 4.0, [](double) { return 0.0f; });
            r.playPad(1, 62, 6.1, 8.0, [](double) { return 0.02f; });       // a resting finger
            int at = -1;
            const Note* note = r.find(60, &at);
            expect(note != nullptr && note->lane.empty());
            expectEquals((int) (note != nullptr ? note->lane.points.capacity() : 1), 0);
            expectWithinAbsoluteError(r.track().getStep(at).length, 1.0f, 1.0e-6f, "its length is left alone");
            expect(r.find(62) != nullptr && r.find(62)->lane.empty());
            expectEquals(r.seq.getReadyLaneBuffers(), ready, "the buffers went back");
            expectEquals(r.lanes(), 0);

            r.seq.setRecording(false);
            r.runTo(16.0);
            r.probe->log.clear();
            r.runTo(48.0);
            expectEquals(r.probe->count('p'), 0, "no pressure is sent for notes that have none");
            expectEquals(r.seq.getActiveLanePlayers(), 0);
            expectEquals(r.probe->count('n'), 4);

            // The same notes with a lane on one of them: on and off at the same samples, pressure added.
            auto plain = r.probe->log;
            Rig laned;
            laned.track() = r.track();
            laned.track().getStep(at).notes[0].lane = ramp(1.0f);
            laned.seq.start();
            laned.runTo(16.0);
            laned.probe->log.clear();
            laned.runTo(48.0);
            std::vector<Probe::Event> notesOnly;
            for (auto& e : laned.probe->log)
                if (e.kind != 'p') notesOnly.push_back(e);
            expectEquals((int) notesOnly.size(), (int) plain.size());
            bool same = notesOnly.size() == plain.size();
            for (size_t i = 0; same && i < plain.size(); ++i)
                same = notesOnly[i].at == plain[i].at && notesOnly[i].kind == plain[i].kind && notesOnly[i].note == plain[i].note;
            expect(same, "a lane changes nothing about when notes start and end");
            expect(laned.probe->count('p', 60) > 10);
        }

        beginTest("The lane is in musical time: tempo and track speed stretch it, micro-timing and swing move its start");
        {
            Rig r;
            auto& step = r.track().getStep(4);
            step.trig = true;
            step.length = 2.0f;
            step.notes.push_back({ 60, 0.8f, ramp(2.0f) });

            // Half the tempo: a step is twice as long, and so is the swell.
            r.seq.setTempo(60.0f);
            r.seq.start();
            r.runTo(16.0);
            expectWithinAbsoluteError(r.probe->firstAt('n', 60), 8.0, 0.02);
            expectWithinAbsoluteError(r.probe->pressureAt(60, 8.0 + 2.0), 0.5f, 0.04f, "half way after two 120-BPM steps");
            expectWithinAbsoluteError(r.probe->pressureAt(60, 8.0 + 3.6), 0.9f, 0.04f);
            expectWithinAbsoluteError(r.probe->firstAt('f', 60), 12.0, 0.04);
            r.seq.stop();

            // A tempo change while the note sounds is followed from there on.
            r.probe->log.clear();
            r.now = 0;
            r.seq.setTempo(120.0f);
            r.seq.start();
            r.runTo(5.0);                       // one step into the note
            expectWithinAbsoluteError(r.probe->lastPressure(60), 0.5f, 0.04f);
            r.seq.setTempo(240.0f);
            r.runTo(5.25);                      // half of the second step at the doubled tempo
            expectWithinAbsoluteError(r.probe->lastPressure(60), 0.75f, 0.04f);
            r.seq.stop();

            // Track speed 2x: half as long.
            r.probe->log.clear();
            r.now = 0;
            r.seq.setTempo(120.0f);
            r.track().speedMultiplier = 2.0f;
            r.seq.start();
            r.runTo(4.0);
            expectWithinAbsoluteError(r.probe->firstAt('n', 60), 2.0, 0.02);
            expectWithinAbsoluteError(r.probe->pressureAt(60, 2.0 + 0.5), 0.5f, 0.04f);
            expectWithinAbsoluteError(r.probe->firstAt('f', 60), 3.0, 0.04);
            r.seq.stop();
            r.track().speedMultiplier = 1.0f;

            // A late note: the lane starts when the note sounds.
            r.probe->log.clear();
            r.now = 0;
            step.microTiming = 0.3f;
            r.seq.start();
            r.runTo(8.0);
            expectWithinAbsoluteError(r.probe->firstAt('n', 60), 4.3, 0.02);
            expectWithinAbsoluteError(r.probe->pressureAt(60, 4.3 + 1.0), 0.5f, 0.04f);
            expect(r.probe->pressureAt(60, 4.25) < 0.0f, "nothing before the note");
            r.seq.stop();

            // Swing moves the odd steps; the lane goes with its note.
            r.probe->log.clear();
            r.now = 0;
            step.microTiming = 0.0f;
            r.track().getStep(5) = step;
            step.clear();
            r.seq.setSwing(0.5f);
            r.seq.setQuantizeAmount(1.0f);
            r.seq.start();
            r.runTo(9.0);
            expectWithinAbsoluteError(r.probe->firstAt('n', 60), 5.25, 0.02);
            expectWithinAbsoluteError(r.probe->pressureAt(60, 5.25 + 1.0), 0.5f, 0.04f);
        }

        beginTest("PRES scales the lane; 0 sends nothing and keeps it");
        {
            Rig r;
            auto& step = r.track().getStep(0);
            step.trig = true;
            step.length = 2.0f;
            step.notes.push_back({ 60, 0.8f, ramp(2.0f, 0.5f) });
            step.pressureScale = 2.0f;
            r.seq.start();
            r.runTo(4.0);
            expectWithinAbsoluteError(r.probe->pressureAt(60, 1.0), 0.5f, 0.04f);
            expectWithinAbsoluteError(r.probe->pressureAt(60, 1.98), 1.0f, 0.04f);
            r.seq.stop();

            r.probe->log.clear();
            r.now = 0;
            step.pressureScale = 0.0f;
            r.seq.start();
            r.runTo(4.0);
            expectEquals(r.probe->count('p'), 0);
            expect(!step.notes[0].lane.empty());
        }

        beginTest("A long note: the spacing doubles, nothing is cut; one pass of the track is the limit");
        {
            Rig r;
            r.track().setLength(64);
            r.record();
            // 40 steps: up for 20, down for 20.
            r.playPad(0, 60, 2.0, 42.0, [](double t) { return (float) (t < 20.0 ? t / 20.0 : (40.0 - t) / 20.0); });
            int at = -1;
            const Note* note = r.find(60, &at);
            expect(note != nullptr && !note->lane.empty());
            if (note != nullptr)
            {
                expectEquals((int) note->lane.shift, 1, "40 steps do not fit at 1/8 step: 1/4 step");
                expect((int) note->lane.points.size() <= PressureLane::MAX_POINTS);
                expectEquals((int) note->lane.points.capacity(), (int) PressureLane::MAX_POINTS);
                expectWithinAbsoluteError(note->lane.lengthInSteps(), 40.0f, 0.5f);
                expectWithinAbsoluteError(r.track().getStep(at).length, 40.0f, 0.1f);
                expectWithinAbsoluteError(note->lane.valueAt(10.0f), 0.5f, 0.04f);
                expectWithinAbsoluteError(note->lane.valueAt(20.0f), 1.0f, 0.04f);
                expectWithinAbsoluteError(note->lane.valueAt(35.0f), 0.25f, 0.04f);
            }

            // Held for longer than the track: the lane and the note stop at one pass.
            Rig s;
            s.record();
            s.runTo(1.0);
            s.padDown(0, 62);
            s.runTo(18.0, [&] { s.audio.padPressure(0, 0.5f); });
            expect(!s.seq.isCapturingPressure(0), "the recording ended by itself after 16 steps");
            const Note* drone = s.find(62, &at);
            expect(drone != nullptr && !drone->lane.empty());
            if (drone != nullptr)
            {
                expectWithinAbsoluteError(s.track().getStep(at).length, 16.0f, 0.05f);
                expectWithinAbsoluteError(drone->lane.lengthInSteps(), 16.0f, 0.2f);
                expectWithinAbsoluteError(drone->lane.valueAt(8.0f), 0.5f, 0.01f);
            }
            s.runTo(22.0, [&] { s.audio.padPressure(0, 0.9f); });
            s.padUp(0);
            s.block();
            drone = s.find(62);
            expect(drone != nullptr && drone->lane.peak() < 0.6f, "what the pad did afterwards is not recorded");
        }

        beginTest("Overdub: a later pass replaces the step's notes and their lanes; the buffers go back");
        {
            Rig r;
            r.record();
            const int ready = r.seq.getReadyLaneBuffers();
            r.playPad(0, 60, 4.1, 5.5, [](double t) { return (float) juce::jmin(1.0, t); });
            expectEquals(r.lanes(), 1);
            expectEquals(r.seq.getReadyLaneBuffers(), ready - 1);

            // The same step on the next pass, another pad, no pressure.
            r.playPad(1, 67, 20.1, 20.6, [](double) { return 0.0f; });
            int at = -1;
            expect(r.find(60) == nullptr, "the first pass's note is gone");
            expect(r.find(67, &at) != nullptr && at == 4);
            expectEquals(r.lanes(), 0);
            expectEquals(r.seq.getReadyLaneBuffers(), ready, "its buffer is back in the pool");
            expectWithinAbsoluteError(r.track().getStep(4).pressureScale, 1.0f, 1.0e-6f);

            // And again with pressure: the new lane, not the old one.
            r.playPad(1, 67, 36.1, 36.9, [](double) { return 0.4f; });
            const Note* note = r.find(67);
            expect(note != nullptr && !note->lane.empty());
            if (note != nullptr)
                expectWithinAbsoluteError(note->lane.peak(), 0.4f, 0.01f);
            expectEquals((int) r.track().getStep(4).notes.size(), 1);
        }

        beginTest("Rolling pads: the pressure sets the roll's rate and is not recorded as a lane");
        {
            Rig r;
            auto& roll = r.audio.getPadRatchet();
            roll.setEnabled(true);
            r.record();
            r.playPad(0, 36, 2.0, 6.0, [](double t) { return (float) juce::jmin(1.0, t / 2.0); });
            int notes = 0;
            for (auto& s : r.track().steps)
                notes += s.trig ? (int) s.notes.size() : 0;
            expect(notes >= 3, "every repeat is a recorded note: " + juce::String(notes));
            expectEquals(r.lanes(), 0);
            expectEquals(r.seq.getReadyLaneBuffers(), (int) SequencerCore::LANE_POOL_READY);

            // With a threshold, a pad pressed below it is a plain held note: its pressure is recorded.
            Rig s;
            s.audio.getPadRatchet().setEnabled(true);
            s.audio.getPadRatchet().setThreshold(0.8f);
            s.record();
            s.playPad(0, 36, 2.0, 4.0, [](double t) { return (float) juce::jmin(0.5, t / 2.0); });
            expectEquals(s.lanes(), 1);
            // Pressed through the threshold it starts rolling, and that hold's lane is dropped.
            s.playPad(1, 38, 8.0, 11.0, [](double t) { return (float) juce::jmin(1.0, t / 2.0); });
            const Note* rolled = s.find(38);
            expect(rolled != nullptr);
            for (auto& st : s.track().steps)
                for (auto& n : st.notes)
                    if (n.pitch == 38) expect(n.lane.empty());
        }

        beginTest("Mute, solo and stop: nothing is sent, and a sounding lane is let go");
        {
            Rig r;
            auto& step = r.track().getStep(2);
            step.trig = true;
            step.length = 4.0f;
            step.notes.push_back({ 60, 0.8f, ramp(4.0f) });

            r.track().muted = true;
            r.seq.start();
            r.runTo(16.0);
            expectEquals((int) r.probe->log.size(), 0, "a muted track sends nothing");
            r.track().muted = false;
            r.track(1).soloed = true;
            r.runTo(32.0);
            expectEquals((int) r.probe->log.size(), 0, "nor does a track that is not the soloed one");
            r.track(1).soloed = false;

            // Muted while the note sounds: the pressure is let go at once.
            r.runTo(36.0);
            expect(r.probe->lastPressure(60) > 0.3f);
            r.track().muted = true;
            r.block();
            expectWithinAbsoluteError(r.probe->lastPressure(60), 0.0f, 1.0e-6f);
            const int sent = r.probe->count('p');
            r.runTo(38.0);
            expectEquals(r.probe->count('p'), sent);
            r.track().muted = false;

            // Stop while the note sounds.
            r.runTo(52.0);
            expect(r.probe->lastPressure(60) > 0.3f);
            expectEquals(r.seq.getActiveLanePlayers(), 1);
            r.seq.stop();
            expectWithinAbsoluteError(r.probe->lastPressure(60), 0.0f, 1.0e-6f);
            expectEquals(r.seq.getActiveLanePlayers(), 0);

            // The step taken away while its note sounds.
            r.probe->log.clear();
            r.now = 0;
            r.seq.start();
            r.runTo(4.0);
            expect(r.probe->lastPressure(60) > 0.3f);
            step.clear();
            r.block();
            expectWithinAbsoluteError(r.probe->lastPressure(60), 0.0f, 1.0e-6f);
            expectEquals(r.seq.getActiveLanePlayers(), 0);
        }

        beginTest("A pad played over a recorded note: the pad wins while it is held, then the lane carries on");
        {
            Rig r;
            auto& step = r.track().getStep(0);
            step.trig = true;
            step.length = 8.0f;
            step.notes.push_back({ 60, 0.8f, ramp(8.0f) });
            step.notes.push_back({ 64, 0.8f, ramp(8.0f) });
            r.seq.start();
            r.runTo(2.0);
            r.padDown(0, 60);
            r.block();
            const int low = r.probe->count('p', 60), high = r.probe->count('p', 64);
            r.runTo(4.0);
            expectEquals(r.probe->count('p', 60), low, "the recorded pressure of the note under the finger is held back");
            expect(r.probe->count('p', 64) > high + 20, "the other note's goes on");
            r.padUp(0);
            r.runTo(4.2);
            expect(r.probe->count('p', 60) > low, "let go: the lane is heard again");
            expectWithinAbsoluteError(r.probe->lastPressure(60), 4.2f / 8.0f, 0.04f);
        }

        beginTest("More swells in one take than there are buffers: the notes are kept, without their pressure");
        {
            Rig r;
            r.track().setLength(128);
            r.record();
            const int hits = SequencerCore::LANE_POOL_READY + 6;
            for (int i = 0; i < hits; ++i)
                r.playPad(i % 16, 36 + i, i + 0.1, i + 0.6, [](double) { return 0.7f; });
            int notes = 0, capacityOk = 0;
            for (auto& s : r.track().steps)
                for (auto& n : s.notes)
                {
                    ++notes;
                    if (!n.lane.empty() && (int) n.lane.points.capacity() == PressureLane::MAX_POINTS) ++capacityOk;
                }
            expectEquals(notes, hits);
            expectEquals(r.lanes(), (int) SequencerCore::LANE_POOL_READY);
            expectEquals(capacityOk, (int) SequencerCore::LANE_POOL_READY, "every lane sits in a pool buffer");
            expectEquals(r.seq.getReadyLaneBuffers(), 0);
            r.seq.serviceLanePool();        // the app does this 30 times a second
            expectEquals(r.seq.getReadyLaneBuffers(), (int) SequencerCore::LANE_POOL_READY);
            r.playPad(0, 120, hits + 2.1, hits + 2.6, [](double) { return 0.7f; });
            expect(r.find(120) != nullptr && !r.find(120)->lane.empty());
        }

        beginTest("Projects: lanes and PRES are saved and loaded; an old project has none");
        {
            Rig r;
            r.record();
            r.playPad(0, 60, 4.2, 7.0, [](double t) { return (float) juce::jmin(1.0, t / 2.0); });
            r.seq.stop();
            int at = -1;
            const Note* note = r.find(60, &at);
            expect(note != nullptr && !note->lane.empty());
            if (note == nullptr) return;
            r.track().getStep(at).pressureScale = 0.5f;

            ProjectData data;
            data.patterns[0].tracks.resize(1);
            for (int s = 0; s < r.track().length; ++s)
                data.patterns[0].tracks[0].steps.push_back(ProjectBridge::captureStep(r.track().getStep(s)));
            const auto json = data.toJSON();
            expect(json.contains("\"lane\""));
            auto loaded = ProjectData::fromJSON(json);
            Step back;
            ProjectBridge::applyStep(loaded.patterns[0].tracks[0].steps[(size_t) at], back);
            expectEquals((int) back.notes.size(), 1);
            expect(back.notes[0].lane == note->lane, "the same points at the same spacing");
            expectWithinAbsoluteError(back.pressureScale, 0.5f, 1.0e-6f);
            expectWithinAbsoluteError(back.length, r.track().getStep(at).length, 1.0e-4f);
            Step other;
            ProjectBridge::applyStep(loaded.patterns[0].tracks[0].steps[0], other);
            expect(!other.hasPressure());
            // Compact: two characters a point.
            expect(json.contains(TrackData::StepData::laneToHex(note->lane)));

            // A long lane keeps its spacing.
            TrackData::StepData sd;
            sd.notes.push_back({ 48, 1.0f, ramp(4.0f) });
            sd.notes[0].lane.shift = 3;
            auto again = TrackData::StepData::fromVar(sd.toVar());
            expect(again.notes[0].lane == sd.notes[0].lane);

            // A project from before lanes, with the per-step array nothing ever wrote or played.
            auto old = TrackData::StepData::fromVar(juce::JSON::parse(
                R"({"trig": true, "vel": 1.0, "len": 1.0, "micro": 0.0, "trigType": 0, "trigA": 1, "trigB": 2,
                    "trigProb": 1.0, "pageLock": -1, "notes": [{"pitch": 60, "vel": 0.9}],
                    "pressure": [0.1, 0.2, 0.3]})"));
            expect(old.trig && old.notes.size() == 1 && old.notes[0].lane.empty());
            expectWithinAbsoluteError(old.pressureScale, 1.0f, 1.0e-6f);
        }

        beginTest("Copy, paste and clear carry the lanes");
        {
            Track track;
            auto& step = track.getStep(3);
            step.trig = true;
            step.notes.push_back({ 60, 0.8f, ramp(2.0f) });
            step.pressureScale = 1.5f;

            std::vector<Step> buffer;
            track.copyStepRange(0, 8, buffer);
            track.pasteStepRange(8, buffer);
            expect(track.getStep(11).notes.size() == 1 && track.getStep(11).notes[0].lane == step.notes[0].lane);
            expectWithinAbsoluteError(track.getStep(11).pressureScale, 1.5f, 1.0e-6f);

            Pattern pattern;
            pattern.getTrack(2) = track;
            const Pattern copy = pattern.duplicate();
            expect(copy.getTrack(2).getStep(3).notes[0].lane == step.notes[0].lane);

            track.getStep(11).clearPressureLanes();
            expect(!track.getStep(11).hasPressure() && track.getStep(11).notes.size() == 1);
            expectWithinAbsoluteError(track.getStep(11).pressureScale, 1.0f, 1.0e-6f);
            track.clearAllSteps();
            expect(!track.getStep(3).hasPressure());
            expectWithinAbsoluteError(track.getStep(3).pressureScale, 1.0f, 1.0e-6f);

            // On STEP: Func + Record / > with a step held (the screen's own copy and paste).
            heldsteptest::Rig r;
            r.step(4).trig = true;
            r.step(4).notes.push_back({ 64, 0.8f, ramp(1.0f) });
            r.padDown(4);
            r.wait();
            expect(r.steps->onCopy());
            r.padUp(4);
            r.padDown(9);
            r.wait();
            expect(r.steps->onPaste());
            expect(r.step(9).hasPressure() && r.step(9).notes[0].lane == r.step(4).notes[0].lane);
            expect(r.steps->onClear());
            expect(!r.step(9).hasPressure());
            r.padUp(9);
        }

        beginTest("STEP: a step with recorded pressure has a mark, and a PRES page after its lock pages");
        {
            heldsteptest::Rig r;
            juce::StringArray said;
            r.steps->setAudioEngine(&r.audio);
            r.steps->showStatus = [&](const juce::String& text) { said.add(text); };
            r.step(4).trig = true;
            r.step(4).length = 2.0f;
            r.step(4).notes.push_back({ 64, 0.8f, ramp(2.0f) });
            r.step(5).trig = true;
            r.step(5).notes.push_back({ 64, 0.8f, {} });

            // The mark: a dark dot at the lower right of the step's cell (step 5 = column 4, 6 = column 5).
            r.tick();
            auto dot = [&](int col) { return r.display.getPixels()[(size_t) (11 + 19 - 5)][(size_t) (col * 16 + 1 + 14 - 3)] != 0; };
            expect(!dot(4), "step 5 carries pressure");
            expect(dot(5), "step 6 does not");

            const int lockPages = r.param->getPageCount();
            r.padDown(5);
            r.wait();
            expectEquals(r.steps->getPageCount(), lockPages + 1, "no pressure, no PRES page");
            r.padUp(5);

            r.padDown(4);
            r.wait();
            expectEquals(r.steps->getPageCount(), lockPages + 2);
            expectEquals(r.steps->getPageIndex(), 1, "still opens on the first lock page");
            r.bankPrev();
            expectEquals(r.mgr.composeNameBar().left, juce::String("TRIG T1"));
            r.bankPrev();                                       // round to the last page
            expect(r.steps->isPressurePage());
            expectEquals(r.steps->getPageIndex(), lockPages + 1);
            auto bar = r.mgr.composeNameBar();
            expectEquals(bar.left, juce::String("PRES T1"));
            expectEquals(bar.text, juce::String("S5"));
            expectEquals(r.names(), juce::String("PRES >LCK CLR --- --- --- --- ---"));
            expectEquals(r.steps->getEncoderReadout(0), juce::String("PRES 100%"));

            // The recorded curve is drawn in the lower half: a rising line.
            r.tick();
            auto lit = [&](int x0, int x1, int y0, int y1)
            {
                int n = 0;
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                        n += r.display.getPixels()[(size_t) y][(size_t) x] != 0 ? 1 : 0;
                return n;
            };
            expect(lit(100, 125, 35, 40) > 5, "high on the right");
            expectEquals(lit(2, 30, 35, 40), 0, "low on the left");

            // PRES: 1 % a click, 0..200 %.
            r.turn(0, -10);
            expectWithinAbsoluteError(r.step(4).pressureScale, 0.9f, 1.0e-4f);
            expectEquals(r.steps->getEncoderReadout(0), juce::String("PRES 90%"));
            for (int i = 0; i < 3; ++i) r.turn(0, -50);
            expectWithinAbsoluteError(r.step(4).pressureScale, 0.0f, 1.0e-6f);
            expect(r.step(4).hasPressure(), "PRES 0 keeps the recording");
            expect(r.steps->isPressurePage());
            r.tick();
            expectEquals(lit(2, 125, 35, 50), 0, "the picture shows what will play: nothing");
            for (int i = 0; i < 5; ++i) r.turn(0, 50);
            expectWithinAbsoluteError(r.step(4).pressureScale, 2.0f, 1.0e-6f);
            // Hold > + turn: back to as recorded.
            r.mgr.controlChangeReceived(1, ScreenManager::CC_PATTERN_KEY, 127);
            r.turn(0, 1);
            r.mgr.controlChangeReceived(1, ScreenManager::CC_PATTERN_KEY, 0);
            expectWithinAbsoluteError(r.step(4).pressureScale, 1.0f, 1.0e-6f);

            // CLR needs four clicks clockwise, not all at once.
            r.turn(2, 1);
            expectEquals(r.steps->getEncoderReadout(2), juce::String("CLR >"));
            r.turn(2, -1);
            expectEquals(r.steps->getEncoderReadout(2), juce::String("CLR TURN>"), "a turn back starts again");
            for (int i = 0; i < 3; ++i) { r.turn(2, 1); UiClock::advance(120); }
            expect(r.step(4).hasPressure(), "three clicks are not enough");
            r.turn(2, 1);
            expect(!r.step(4).hasPressure(), "the fourth clears");
            expectEquals(said.joinIntoString("|"), juce::String("PRESSURE CLEARED"));
            expectEquals((int) r.step(4).notes.size(), 1);
            expectWithinAbsoluteError(r.step(4).length, 2.0f, 1.0e-6f, "the note itself is untouched");
            expectEquals(r.steps->getPageCount(), lockPages + 1, "the page has gone with the pressure");
            expectEquals(r.mgr.composeNameBar().left, juce::String("TRIG T1"));
            r.padUp(4);
            expect(r.step(4).trig, "editing while held never removes the trig");
            r.tick();
            expect(dot(4), "and the mark has gone");
        }

        beginTest(">LCK: the recorded pressure becomes parameter locks through the track's routes");
        {
            heldsteptest::Rig r;                    // track 1: the unified voice; pressure -> LEVL by default
            juce::StringArray said;
            r.steps->setAudioEngine(&r.audio);
            r.steps->showStatus = [&](const juce::String& text) { said.add(text); };
            using D = AftertouchRouter::Destination;
            r.audio.setPressureRoute(1 - 1, 1, { D::FilterCutoff, 0.5f, false });
            r.audio.setPressureRoute(1 - 1, 2, { D::Vibrato, 1.0f, false });        // no cell to lock

            ParamSpec levl, cut, none;
            expect(r.audio.pressureLockCell(0, D::Volume, 60, levl));
            expect(r.audio.pressureLockCell(0, D::FilterCutoff, 60, cut));
            expect(!r.audio.pressureLockCell(0, D::Vibrato, 60, none), "vibrato depth is not a cell on this engine");
            expectEquals(juce::String(levl.label), juce::String("LEVL"));
            expectEquals(juce::String(cut.label), juce::String("CUT"));
            const float levlBase = r.audio.getTrackParameter(0, levl.id), cutBase = r.audio.getTrackParameter(0, cut.id);

            r.step(4).trig = true;
            r.step(4).length = 3.0f;
            r.step(4).notes.push_back({ 60, 0.8f, ramp(3.0f) });
            r.step(5).notes.push_back({ 72, 0.8f, {} });        // a step switched off, its note still there
            r.padDown(4);
            r.wait();
            r.bankPrev();
            r.bankPrev();
            expect(r.steps->isPressurePage());
            for (int i = 0; i < 4; ++i) { r.turn(1, 1); UiClock::advance(120); }

            expectEquals(said.joinIntoString("|"), juce::String("2 OF 3 ROUTES"));
            expect(!r.step(4).hasPressure(), "the lane has become locks");
            for (int k = 0; k < 3; ++k)
            {
                auto& st = r.step(4 + k);
                const float mean = ((float) k + 0.5f) / 3.0f;
                expect(st.trig, "step " + juce::String(5 + k) + " carries locks, so it is a trig");
                expectEquals((int) st.paramLocks.size(), 2);
                expectWithinAbsoluteError(st.getParamLock(levl.id, -1.0f),
                                          levl.fromNorm(levl.toNorm(levlBase) + mean * (1.0f - levl.toNorm(levlBase))), 0.01f);
                const float cutNorm = cut.toNorm(cutBase) + mean * 0.5f * (1.0f - cut.toNorm(cutBase));
                expectWithinAbsoluteError(st.getParamLock(cut.id, -1.0f) / cut.fromNorm(cutNorm), 1.0f, 0.02f);
            }
            expectEquals((int) r.step(4).notes.size(), 1, "the note stays");
            expect(r.step(5).notes.empty() && r.step(6).notes.empty(), "the steps it spans are lock trigs: no note of their own");
            expect(r.step(7).paramLocks.empty() && !r.step(7).trig);
            expectWithinAbsoluteError(r.track().soundParams[levl.id], levlBase, 1.0e-6f, "what the cells return to is kept");
            expectEquals(r.mgr.composeNameBar().left, juce::String("TRIG T1"));
            r.padUp(4);

            // All routes lockable: says how many steps.
            said.clear();
            r.audio.setPressureRoute(0, 2, { D::Off, 0.0f, false });
            r.step(10).trig = true;
            r.step(10).notes.push_back({ 60, 0.8f, ramp(1.0f) });
            r.padDown(10);
            r.wait();
            expectEquals(r.mgr.composeNameBar().left, juce::String("TRIG T1"), "PRES was left for TRIG when the pressure went");
            r.bankPrev();
            expect(r.steps->isPressurePage());
            for (int i = 0; i < 4; ++i) { r.turn(1, 1); UiClock::advance(120); }
            expectEquals(said.joinIntoString("|"), juce::String("LOCKED 1 STEP"));
            r.padUp(10);

            // Nothing to lock (every route off): the pressure stays.
            said.clear();
            r.audio.setPressureRoute(0, 0, { D::Off, 0.0f, false });
            r.audio.setPressureRoute(0, 1, { D::Off, 0.0f, false });
            r.step(12).trig = true;
            r.step(12).notes.push_back({ 60, 0.8f, ramp(1.0f) });
            r.padDown(12);
            r.wait();
            r.bankPrev();
            expect(r.steps->isPressurePage());
            for (int i = 0; i < 4; ++i) { r.turn(1, 1); UiClock::advance(120); }
            expectEquals(said.joinIntoString("|"), juce::String("NO ROUTE"));
            expect(r.step(12).hasPressure());
            expect(r.step(12).paramLocks.empty());
            r.padUp(12);
        }

        beginTest(">LCK on a drum kit: each pad's own cell; PRES scales what is locked");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::Drum));
            auto& track = seq.getPattern().getTrack(0);
            track.engineType = EngineType::Drum;
            using D = AftertouchRouter::Destination;
            audio.setPressureRoute(0, 0, { D::Volume, 1.0f, false });

            ParamSpec pad2, pad9;
            expect(audio.pressureLockCell(0, D::Volume, 2, pad2));
            expect(audio.pressureLockCell(0, D::Volume, 9, pad9));
            expect(pad2.id != pad9.id);
            for (auto d : { D::DrumDecay, D::DrumPitch, D::FilterCutoff, D::DrumDrive, D::DrumSnap, D::PanSpread })
            {
                ParamSpec cell;
                expect(audio.pressureLockCell(0, d, 2, cell), juce::String("drum destination ") + AftertouchRouter::getDestName(d));
            }

            auto& step = track.getStep(0);
            step.trig = true;
            step.pressureScale = 0.5f;
            step.notes.push_back({ 2, 0.8f, ramp(1.0f) });
            step.notes.push_back({ 9, 0.8f, {} });
            const float base = audio.getTrackParameter(0, pad2.id);
            const auto done = audio.bakePressureToLocks(0, track, 0);
            expectEquals(done.routes, 1);
            expectEquals(done.locked, 1);
            expectEquals(done.steps, 1);
            expectEquals((int) step.paramLocks.size(), 1, "only the pad that was pressed");
            expectWithinAbsoluteError(step.getParamLock(pad2.id, -1.0f), base + 0.5f * 0.5f * (1.0f - base), 0.01f);
            expect(!step.hasPressure());

            // The sampler has no pressure routes: nothing to do, nothing lost.
            audio.addEngine(1, AudioEngine::createEngine(EngineType::Sampler));
            auto& sampler = seq.getPattern().getTrack(1);
            sampler.getStep(0).trig = true;
            sampler.getStep(0).notes.push_back({ 60, 0.8f, ramp(1.0f) });
            const auto nothing = audio.bakePressureToLocks(1, sampler, 0);
            expectEquals(nothing.locked, 0);
            expect(sampler.getStep(0).hasPressure());
        }

        beginTest("Which destinations can become locks, engine by engine");
        {
            AudioEngine audio;
            const EngineType types[] = { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc,
                                         EngineType::Wavetable, EngineType::WaveSeq };
            juce::StringArray report;
            for (int t = 0; t < 5; ++t)
            {
                audio.addEngine(t, AudioEngine::createEngine(types[t]));
                const AudioEngine::PressureDestInfo* list = nullptr;
                const int n = AudioEngine::pressureDestinations(types[t], list);
                juce::String line = audio.getEngine(t)->getName() + ":";
                for (int i = 0; i < n; ++i)
                {
                    ParamSpec cell;
                    const bool ok = audio.pressureLockCell(t, list[i].dest, 60, cell);
                    line << " " << list[i].label << (ok ? "" : "(no)");
                    if (list[i].dest == AftertouchRouter::Destination::Volume
                        || list[i].dest == AftertouchRouter::Destination::FilterCutoff)
                        expect(ok, line);
                }
                report.add(line);
            }
            logMessage(report.joinIntoString("\n"));
        }
    }
};

static PressureLaneRecordTests pressureLaneRecordTests;
