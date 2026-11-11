#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include "../src/sequencer/SequencerCore.h"
#include "../src/sequencer/SongMode.h"
#include "../src/sequencer/SongPlayer.h"

// The song as the app runs it: SongPlayer listening to a playing SequencerCore.
class SongPlayerTests : public juce::UnitTest
{
public:
    SongPlayerTests() : UnitTest("Song Player") {}

    struct Rig
    {
        SequencerCore seq;
        SongMode song;
        SongPlayer player;
        std::vector<int> pitches;      // every trig of track 1, in order
        std::vector<int> transposes;   // the song's transposition when each sounded

        Rig()
        {
            seq.prepareToPlay(48000.0, 64);
            // Pattern n plays one note, 60 + n, on its first step.
            for (int p = 0; p < 3; ++p)
            {
                auto& step = seq.getPattern(p).getTrack(0).getStep(0);
                step.trig = true;
                step.notes.push_back({ 60 + p, 1.0f });
            }
            player.song = &song;
            player.sequencer = &seq;
            seq.addListener(&player);
            seq.setTrigCallback([this](int track, const Step& step)
            {
                if (track == 0 && ! step.notes.empty())
                {
                    pitches.push_back(step.notes[0].pitch);
                    transposes.push_back(player.transpose.load());
                }
            });
        }
        ~Rig() { seq.removeListener(&player); }

        // 120 BPM, 48 kHz: a 16-step pattern lasts 96,000 samples.
        void playLoops(double loops)
        {
            int left = (int) (loops * 96000.0);
            while (left > 0) { const int n = juce::jmin(64, left); seq.processBlock(n); left -= n; }
        }
        juce::String heard() const
        {
            juce::String s;
            for (int p : pitches) s << p << " ";
            return s.trim();
        }
    };

    void runTest() override
    {
        beginTest("Each row plays once: the first row is not played twice, no step sounds twice");
        {
            Rig r;
            r.song.addEntry(0, 1);
            r.song.addEntry(1, 1);
            r.song.addEntry(2, 1);
            r.song.setArmed(true);
            r.seq.start();
            r.playLoops(6.5);
            expectEquals(r.heard(), juce::String("60 61 62 60 61 62 60"));
        }

        beginTest("Repeats are counted from the first pass");
        {
            Rig r;
            r.song.addEntry(0, 2);
            r.song.addEntry(1, 3);
            r.song.setArmed(true);
            r.seq.start();
            r.playLoops(7.5);
            expectEquals(r.heard(), juce::String("60 60 61 61 61 60 60 61"));
        }

        beginTest("The song starts on its first row whatever pattern was showing, with that row's transpose");
        {
            Rig r;
            SongEntry a; a.patternIndex = 1; a.repeatCount = 1; a.transposeSemitones = 5;
            SongEntry b; b.patternIndex = 0; b.repeatCount = 1; b.transposeSemitones = -2;
            r.song.addEntry(0, 1); r.song.setEntry(0, a);
            r.song.addEntry(0, 1); r.song.setEntry(1, b);
            r.song.setArmed(true);
            r.seq.selectPattern(2);
            r.seq.start();
            r.playLoops(3.5);
            expectEquals(r.heard(), juce::String("61 60 61 60"));
            expectEquals((int) r.transposes.size(), 4);
            if (r.transposes.size() == 4)
            {
                expectEquals(r.transposes[0], 5);
                expectEquals(r.transposes[1], -2);
                expectEquals(r.transposes[2], 5);
            }
        }

        beginTest("A song that does not loop stops moving on its last row");
        {
            Rig r;
            r.song.addEntry(0, 1);
            r.song.addEntry(1, 1);
            r.song.setLoopMode(false);
            r.song.setArmed(true);
            r.seq.start();
            r.playLoops(4.5);
            expectEquals(r.heard(), juce::String("60 61 61 61 61"));
            expect(r.song.isFinished());
        }

        beginTest("Not armed: the pattern loops and the song does nothing");
        {
            Rig r;
            r.song.addEntry(1, 1);
            r.song.addEntry(2, 1);
            r.seq.start();
            r.playLoops(2.5);
            expectEquals(r.heard(), juce::String("60 60 60"));
        }
    }
};

static SongPlayerTests songPlayerTests;
