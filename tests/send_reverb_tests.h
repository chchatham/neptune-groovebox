#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <cmath>
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"

// The send buses die away. The owner sent a track to RVB1 and got an endless drone that only
// quitting the app stopped (2026-11-11).
class SendReverbTests : public juce::UnitTest
{
public:
    SendReverbTests() : UnitTest("Send Reverb") {}

    struct Rig
    {
        static constexpr int BLOCK = 64;
        SequencerCore seq;
        AudioEngine audio;
        std::vector<float> l = std::vector<float>(BLOCK), r = std::vector<float>(BLOCK);

        explicit Rig(EngineType type = EngineType::UnifiedVoice)
        {
            seq.prepareToPlay(48000.0, BLOCK);
            audio.setSequencer(&seq);
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                audio.addEngine(t, AudioEngine::createEngine(t == 0 ? type : EngineType::UnifiedVoice));
            seq.getPattern().getTrack(0).engineType = type;
            audio.prepare(48000.0, BLOCK);
        }

        // RMS of the next `seconds` of the whole instrument; `finite` goes false on a NaN or infinity.
        float render(double seconds, bool& finite)
        {
            double sq = 0.0;
            const int blocks = (int) (seconds * 48000.0 / BLOCK);
            for (int b = 0; b < blocks; ++b)
            {
                audio.renderBlock(l.data(), r.data(), BLOCK);
                for (int i = 0; i < BLOCK; ++i)
                {
                    finite = finite && std::isfinite(l[(size_t) i]) && std::isfinite(r[(size_t) i]);
                    sq += (double) l[(size_t) i] * l[(size_t) i] + (double) r[(size_t) i] * r[(size_t) i];
                }
            }
            return (float) std::sqrt(sq / (2.0 * blocks * BLOCK));
        }
    };

    struct Tail { float playing = 0, early = 0, loudest = 0, late = 0; bool finite = true; };

    // One second of noise through the effect, then 30 s of silence: the level while it plays, in
    // the first second after, the loudest three seconds of the silence (a long delay starts late)
    // and the last three. Levels are of the moving part: a constant offset is not a sound.
    static Tail tailOf(EffectType type, int setting)     // 0 = as created, 1 = every cell at its maximum, 2 = at its minimum
    {
        auto fx = AudioEngine::createEffect(type);
        Tail t;
        if (fx == nullptr) return t;
        fx->prepare(48000.0, 64);
        if (setting != 0)
            for (int i = 0; i < 4; ++i)
            {
                const auto spec = fx->getParamSpec(i);
                if (spec.isValid()) fx->setParameter(spec.id, setting == 1 ? spec.maxVal : spec.minVal);
            }
        juce::Random rng(1234);
        auto run = [&](int samples, bool noise)
        {
            double sq = 0.0, sum = 0.0;
            for (int n = 0; n < samples; ++n)
            {
                float l = noise ? (rng.nextFloat() * 2.0f - 1.0f) * 0.25f : 0.0f;
                float r = l;
                fx->process(l, r);
                t.finite = t.finite && std::isfinite(l) && std::isfinite(r);
                sq += (double) l * l + (double) r * r;
                sum += (double) l + (double) r;
            }
            const double mean = sum / (2.0 * samples);
            return (float) std::sqrt(std::max(0.0, sq / (2.0 * samples) - mean * mean));
        };
        t.playing = run(48000, true);
        t.early = run(48000, false);
        for (int w = 0; w < 9; ++w)
            t.loudest = std::max(t.loudest, run(48000 * 3, false));
        t.late = run(48000 * 2, false);
        t.loudest = std::max(t.loudest, t.early);
        return t;
    }

    void runTest() override
    {
        beginTest("Every effect type falls silent after its input stops, as created and with every cell at either end");
        {
            static const char* const settingNames[] = { "created", "maximum", "minimum" };
            for (int type = 0; type < (int) EffectType::NumEffectTypes; ++type)
                for (int setting = 0; setting < 3; ++setting)
                {
                    const Tail t = tailOf((EffectType) type, setting);
                    const juce::String what = "effect " + juce::String(type) + " " + settingNames[setting];
                    logMessage(what + ": playing " + juce::String(t.playing, 4) + "  1 s after " + juce::String(t.early, 5)
                               + "  30 s after " + juce::String(t.late, 6));
                    expect(t.finite, what + " is not finite");
                    expect(t.late <= t.loudest * 0.5f + 1.0e-5f, what + " does not die away: " + juce::String(t.late, 6));
                    expect(t.late < 0.05f, what + " still sounds after 30 s: " + juce::String(t.late, 6));
                }
        }

        beginTest("Shift + Play's silence: notes are let go and a long reverb tail is cut at once");
        {
            Rig r;
            r.audio.setTrackParameter(0, MixParam::SEND_BASE, 1.0f);
            if (auto* reverb = r.audio.getSendBus(0).getChain().getEffect(0))
                reverb->setParameter(0, 0.99f);                 // DCAY at its longest
            bool finite = true;
            r.audio.getEngine(0)->noteOn(48, 1.0f, -1);
            const float playing = r.render(1.0, finite);
            const float held = r.render(1.0, finite);           // the key is still down
            expect(held > playing * 0.3f);
            r.audio.requestSilence();
            r.render(0.5, finite);
            const float after = r.render(1.0, finite);
            logMessage("silence: playing " + juce::String(playing, 4) + ", held " + juce::String(held, 4) + ", after " + juce::String(after, 6));
            expect(finite);
            expect(after < playing * 0.01f, "still sounding: " + juce::String(after, 6));
        }

        for (int bus = 0; bus < 2; ++bus)
        {
            beginTest("Send " + juce::String(bus + 1) + " at full: a note's tail dies away after the note");
            Rig r;
            r.audio.setTrackParameter(0, MixParam::SEND_BASE + bus, 1.0f);
            bool finite = true;
            r.audio.getEngine(0)->noteOn(48, 1.0f, -1);
            r.audio.getEngine(0)->noteOn(55, 1.0f, -1);
            const float playing = r.render(1.0, finite);
            r.audio.getEngine(0)->allNotesOff();
            juce::String line = "send " + juce::String(bus + 1) + ": playing " + juce::String(playing, 4) + "  then";
            float last = 0.0f;
            for (int s = 0; s < 30; s += 3)
            {
                last = r.render(3.0, finite);
                line << " " << juce::String(last, 5);
            }
            logMessage(line);
            expect(finite);
            expect(playing > 0.01f);
            expect(last < playing * 0.001f, "30 s after the note: " + juce::String(last, 6));
        }
    }
};

static SendReverbTests sendReverbTests;
