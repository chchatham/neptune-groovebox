#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../src/engine/AudioEngine.h"
#include "../src/engine/VoiceFilterSection.h"
#include "../src/sequencer/SequencerCore.h"

// Clicks at the start of a note, measured on the whole instrument as the app runs it: the engine a
// track has right after it is chosen (AudioEngine::setEngineType), through the track mixer and the
// master EQ / compressor / limiter, at 48 kHz in 64-sample blocks.
//
// Two things were heard as a click (2026-11-11):
//   - the master compressor ran at the Compressor class's own -20 dB / 4:1, so a single loud note
//     came through at full level for about 15 ms and was then pulled down by up to 8 dB;
//   - a voice struck again while it still sounded (the same key, the release tail of the note
//     before, a stolen voice) restarted its oscillators at phase zero: a jump in the waveform.
// The measure for a jump is the second difference of the output (a smooth waveform has a small
// one; a step has one as large as the step).
class OnsetClickTests : public juce::UnitTest
{
public:
    OnsetClickTests() : UnitTest("Onset Clicks") {}

    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;
    static constexpr int ONSET_WINDOW = 400;    // samples after the note-on block starts (master latency is 144)

    struct Event { int block; int note; float velocity; };      // velocity 0 = note off

    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        std::vector<float> out;

        explicit Rig(EngineType type)
        {
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.prepare(SR, BLOCK);
            audio.setEngineType(0, type);
        }

        void render(int blocks)
        {
            float l[BLOCK], r[BLOCK];
            for (int b = 0; b < blocks; ++b)
            {
                audio.renderBlock(l, r, BLOCK);
                out.insert(out.end(), l, l + BLOCK);
            }
        }

        void play(const std::vector<Event>& events, int blocks)
        {
            for (int b = 0; b < blocks; ++b)
            {
                for (auto& e : events)
                    if (e.block == b)
                    {
                        if (e.velocity > 0.0f) audio.getEngine(0)->noteOn(e.note, e.velocity, -1);
                        else                   audio.getEngine(0)->noteOff(e.note, -1);
                    }
                render(1);
            }
        }

        float peak(int from, int to) const
        {
            float p = 0.0f;
            for (int i = std::max(0, from); i < std::min(to, (int) out.size()); ++i)
                p = std::max(p, std::abs(out[(size_t) i]));
            return p;
        }

        // Largest second difference in [from, to).
        float jump(int from, int to) const
        {
            float d = 0.0f;
            for (int i = std::max(2, from); i < std::min(to, (int) out.size()); ++i)
                d = std::max(d, std::abs(out[(size_t) i] - 2.0f * out[(size_t) i - 1] + out[(size_t) i - 2]));
            return d;
        }
    };

    static const char* engineName(EngineType t)
    {
        switch (t)
        {
            case EngineType::Wavetable: return "wavetable";
            case EngineType::FM:        return "FM";
            case EngineType::MacroOsc:  return "macro osc";
            case EngineType::WaveSeq:   return "wave seq";
            default:                    return "?";
        }
    }

    // The ways a voice gets struck while it still sounds; the last note-on is the one measured.
    struct Scenario { const char* name; std::vector<Event> events; int watchBlock; };

    static std::vector<Scenario> restrikes(float v)
    {
        return {
            { "same key again in its release tail", { { 8, 60, v }, { 83, 60, 0 }, { 120, 60, v } }, 120 },
            { "another key in the release tail",    { { 8, 60, v }, { 83, 60, 0 }, { 120, 67, v } }, 120 },
            { "same key again while held",          { { 8, 60, v }, { 120, 60, v } }, 120 },
            { "softer after louder",                { { 8, 60, 1.0f }, { 83, 60, 0 }, { 120, 64, v * 0.4f } }, 120 },
            { "same key again while held, softer",  { { 8, 60, 1.0f }, { 120, 60, v * 0.4f } }, 120 },
            { "ninth note steals a voice",          { { 8, 48, v }, { 20, 50, v }, { 30, 52, v }, { 40, 53, v }, { 50, 55, v },
                                                      { 60, 57, v }, { 70, 59, v }, { 80, 60, v }, { 120, 62, v } }, 120 },
        };
    }

    void runTest() override
    {
        const float velocities[] = { 60.0f / 127.0f, 1.0f };

        beginTest("A single note of the default wavetable / FM sound is not squashed by the master chain");
        for (EngineType type : { EngineType::Wavetable, EngineType::FM })
            for (float vel : velocities)
            {
                Rig r(type);
                r.render(8);
                const int on = (int) r.out.size();
                r.audio.getEngine(0)->noteOn(60, vel, -1);
                float reduction = 0.0f;
                for (int b = 0; b < 120; ++b)
                {
                    r.render(1);
                    reduction = std::min(reduction, r.audio.getMasterComp().getGainReductionDb());
                }
                // Attack 10 ms, then the decay takes 300 ms to fall to 0.7: at 100 ms the note is still
                // at 0.9 of its peak. (Before: 0.46 of it at full velocity, the compressor's doing.)
                const float early = r.peak(on, on + (int) (0.040 * SR));
                const float later = r.peak(on + (int) (0.100 * SR), on + (int) (0.110 * SR));
                const juce::String what = juce::String(engineName(type)) + " vel " + juce::String(vel, 2);
                logMessage(what + ": peak " + juce::String(early, 3) + ", at 100 ms " + juce::String(later, 3)
                           + ", compressor " + juce::String(reduction, 2) + " dB");
                expect(reduction > -0.5f, what + ": master compressor pulled a single note down by " + juce::String(reduction, 2) + " dB");
                expect(later > 0.8f * early, what + ": level collapses after the onset (" + juce::String(early, 3) + " -> " + juce::String(later, 3) + ")");
                expect(early > 0.1f && early < 0.9f, what + ": a single note peaks at " + juce::String(early, 3));
            }

        beginTest("The first note of the default wavetable / FM sound starts from silence without a jump");
        for (EngineType type : { EngineType::Wavetable, EngineType::FM })
            for (float vel : velocities)
                for (int note : { 36, 60, 84 })
                {
                    Rig r(type);
                    r.play({ { 8, note, vel } }, 100);
                    const int on = 8 * BLOCK;
                    const float onset = r.jump(on, on + ONSET_WINDOW);
                    const float body = r.jump(on + 1200, on + 4800);    // the held note, past the attack's corner
                    expect(onset <= 2.0f * body, juce::String(engineName(type)) + " note " + juce::String(note) + " vel " + juce::String(vel, 2)
                                                 + ": onset " + juce::String(onset, 5) + " against " + juce::String(body, 5) + " in the held note");
                }

        beginTest("Wavetable / FM default sound: a voice struck again while it sounds does not jump");
        for (EngineType type : { EngineType::Wavetable, EngineType::FM })
            for (float vel : velocities)
                for (auto& sc : restrikes(vel))
                {
                    Rig r(type);
                    r.play(sc.events, 200);
                    const int on = sc.watchBlock * BLOCK;
                    const float onset = r.jump(on, on + ONSET_WINDOW);
                    const float level = r.peak(0, (int) r.out.size());
                    const juce::String what = juce::String(engineName(type)) + " vel " + juce::String(vel, 2) + ", " + sc.name;
                    logMessage(what + ": jump " + juce::String(onset, 5) + ", peak " + juce::String(level, 3));
                    // Before: 0.05 - 0.13 for a peak of 0.36 (0.016 - 0.024 for the stolen voice among eight).
                    expect(onset < 0.045f * level, what + ": jump of " + juce::String(onset, 5) + " at the note-on (peak " + juce::String(level, 3) + ")");
                    expect(onset < 0.010f, what + ": jump of " + juce::String(onset, 5) + " at the note-on");
                }

        beginTest("Macro oscillator / wave sequencer: the same, heard through a closed filter");
        // Their default waveforms are bright enough to hide the jump in this measure; with the
        // voice's low-pass at 300 Hz the waveform is smooth and the jump shows.
        for (EngineType type : { EngineType::MacroOsc, EngineType::WaveSeq })
            for (auto& sc : restrikes(1.0f))
            {
                Rig r(type);
                r.audio.setTrackParameter(0, 100 + VoiceFilterSection::Cutoff, 300.0f);
                r.play(sc.events, 200);
                const int on = sc.watchBlock * BLOCK;
                const float onset = r.jump(on, on + ONSET_WINDOW);
                const float body = r.jump(on - 2400, on);
                const juce::String what = juce::String(engineName(type)) + ", " + sc.name;
                logMessage(what + ": jump " + juce::String(onset, 5) + ", before it " + juce::String(body, 5));
                expect(onset < 4.0f * body + 0.001f, what + ": jump of " + juce::String(onset, 5) + " at the note-on (" + juce::String(body, 5) + " before it)");
            }
    }
};

static OnsetClickTests onsetClickTests;
