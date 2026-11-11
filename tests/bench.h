#pragma once
// Performance benchmark: NeptuneTests --bench. Renders audio off-line through the real engine and
// reports the cost as a share of real time on one core, plus the slowest single block against the
// time one block lasts (what matters for drop-outs).
#include <chrono>
#include <cstdio>
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"

namespace bench
{
    struct Result { double percent = 0, worstMs = 0, blockMs = 0; };

    inline Result run(AudioEngine& audio, double sampleRate, int block, double seconds,
                      const std::function<void(int blockIndex)>& perBlock)
    {
        std::vector<float> l((size_t) block), r((size_t) block);
        const int blocks = (int) (seconds * sampleRate / block);
        double total = 0, worst = 0;
        for (int b = 0; b < blocks; ++b)
        {
            perBlock(b);
            const auto t0 = std::chrono::steady_clock::now();
            audio.renderBlock(l.data(), r.data(), block);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            total += ms;
            worst = std::max(worst, ms);
        }
        Result res;
        res.blockMs = 1000.0 * block / sampleRate;
        res.percent = 100.0 * total / (seconds * 1000.0);
        res.worstMs = worst;
        return res;
    }

    inline void chord(EngineBase* e, int base, int notes, bool on)
    {
        static const int steps[8] = { 0, 4, 7, 11, 14, 17, 21, 24 };
        for (int i = 0; i < notes; ++i)
            if (on) e->noteOn(base + steps[i], 0.8f); else e->noteOff(base + steps[i]);
    }

    inline int main()
    {
        const double sr = 48000.0;
        const int block = 64;
        static const char* const names[] = { "main synth", "FM", "macro osc", "wavetable", "wave seq", "sampler", "drums" };
        std::printf("48 kHz, %d-sample blocks (one block = %.2f ms). Percent = share of one core.\n\n", block, 1000.0 * block / sr);

        // 1. One track of each engine, eight notes held and re-struck every half second.
        std::printf("%-34s %8s %10s\n", "one track, 8 notes", "average", "worst blk");
        for (int type = 0; type < 7; ++type)
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine((EngineType) type));
            audio.prepare(sr, block);
            auto* e = audio.getEngine(0);
            auto res = run(audio, sr, block, 8.0, [&](int b)
            {
                const int period = (int) (0.5 * sr / block);
                if (b % period == 0) chord(e, type == 6 ? 0 : 48, 8, true);
                if (b % period == period * 3 / 4) chord(e, type == 6 ? 0 : 48, 8, false);
            });
            std::printf("%-34s %7.2f%% %8.3f ms\n", names[type], res.percent, res.worstMs);
        }

        // 1s. What the mod strip costs: the same eight notes with the strip at rest (nothing is
        // read: the row above again) and fully up with the engine's default STRIP routes.
        std::printf("\n%-34s %8s %10s\n", "mod strip, 8 notes", "at rest", "fully up");
        for (int type = 0; type < 7; ++type)
        {
            double percent[2] = { 0, 0 };
            for (int up = 0; up < 2; ++up)
            {
                SequencerCore seq;
                AudioEngine audio;
                audio.setSequencer(&seq);
                audio.addEngine(0, AudioEngine::createEngine((EngineType) type));
                audio.prepare(sr, block);
                auto* e = audio.getEngine(0);
                e->setModWheel(up ? 1.0f : 0.0f);
                percent[up] = run(audio, sr, block, 8.0, [&](int b)
                {
                    const int period = (int) (0.5 * sr / block);
                    if (b % period == 0) chord(e, type == 6 ? 0 : 48, 8, true);
                    if (b % period == period * 3 / 4) chord(e, type == 6 ? 0 : 48, 8, false);
                }).percent;
            }
            std::printf("%-34s %7.2f%% %9.2f%%\n", names[type], percent[0], percent[1]);
        }

        // 1a. What the LFOs cost: the same eight notes with both LFOs off, both on destinations of
        // the original lists (pitch and amp), and both on destinations added with the long lists
        // (the first two after the original ones, and the two envelope / late-list ones at 3/4).
        {
            static const int oldCount[7] = { 24, 9, 10, 9, 9, 12, 8 };
            std::printf("\n%-14s %9s %9s %9s %9s\n", "LFOs, 8 notes", "off", "old dest", "new dest", "new late");
            for (int type = 0; type < 7; ++type)
            {
                double percent[4] = { 0, 0, 0, 0 };
                int count = 0;
                for (int mode = 0; mode < 4; ++mode)
                {
                    SequencerCore seq;
                    AudioEngine audio;
                    audio.setSequencer(&seq);
                    audio.addEngine(0, AudioEngine::createEngine((EngineType) type));
                    audio.prepare(sr, block);
                    auto* e = audio.getEngine(0);
                    const bool unified = type == 0;
                    const int dest[2] = { unified ? ModParam::lfoParamId(0, ModParam::LFO_DEST) : 165,
                                          unified ? ModParam::lfoParamId(1, ModParam::LFO_DEST) : 173 };
                    const int depth[2] = { unified ? ModParam::lfoParamId(0, ModParam::LFO_DEPTH) : 159,
                                           unified ? ModParam::lfoParamId(1, ModParam::LFO_DEPTH) : 167 };
                    if (unified)
                    {
                        e->setParameter(ModParam::LFO_COUNT, 2.0f);
                        count = (int) UnifiedVoice::LfoDest::Count;
                    }
                    else
                    {
                        for (auto& page : e->getParamPages())
                            for (auto& p : page.params)
                                if (p.id == 165) count = p.numChoices;
                    }
                    int a = 0, b2 = 0;
                    if (mode == 1) { a = 1; b2 = 2; }
                    if (mode == 2) { a = oldCount[type]; b2 = oldCount[type] + 1; }
                    if (mode == 3) { a = oldCount[type] + (count - oldCount[type]) * 3 / 4; b2 = a + 1; }
                    if (a >= count || b2 >= count)
                    {
                        percent[mode] = -1.0;
                        continue;
                    }
                    for (int l = 0; l < 2; ++l)
                    {
                        e->setParameter(depth[l], mode == 0 ? 0.0f : 0.5f);
                        e->setParameter(dest[l], (float) (l == 0 ? a : b2));
                    }
                    percent[mode] = run(audio, sr, block, 8.0, [&](int b)
                    {
                        const int period = (int) (0.5 * sr / block);
                        if (b % period == 0) chord(e, type == 6 ? 0 : 48, 8, true);
                        if (b % period == period * 3 / 4) chord(e, type == 6 ? 0 : 48, 8, false);
                    }).percent;
                }
                std::printf("%-14s", names[type]);
                for (double p : percent)
                    if (p < 0.0) std::printf(" %9s", "-"); else std::printf(" %8.2f%%", p);
                std::printf("   (%d destinations)\n", count);
            }
        }

        // 1b. The main synth with all three oscillator slots set to one kind, 8 notes (and the worst
        // case: harmonic oscillators in an eight-voice stack).
        {
            struct Case { const char* name; int wave; int stack; };
            static const Case cases[] = { { "synth, 3 x SAW (analog)", 1, 0 }, { "synth, 3 x WT", 6, 0 }, { "synth, 3 x FM", 7, 0 },
                                          { "synth, 3 x FORM", 8, 0 }, { "synth, 3 x HARM", 9, 0 },
                                          { "synth, 3 x SAW, stack of 8", 1, 3 }, { "synth, 3 x HARM, stack of 8", 9, 3 } };
            std::printf("\n%-34s %8s %10s\n", "oscillator slots, 8 notes", "average", "worst blk");
            for (auto& c : cases)
            {
                SequencerCore seq;
                AudioEngine audio;
                audio.setSequencer(&seq);
                audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
                audio.prepare(sr, block);
                auto* e = audio.getEngine(0);
                e->setParameter(UnifiedParam::WAVE1, (float) c.wave);
                e->setParameter(UnifiedParam::WAVE2, (float) c.wave);
                e->setParameter(UnifiedParam::WAVE3, (float) c.wave);
                e->setParameter(UnifiedParam::SUB, 0.7f);
                e->setParameter(UnifiedParam::STACK, (float) c.stack);
                auto res = run(audio, sr, block, 8.0, [&](int b)
                {
                    const int period = (int) (0.5 * sr / block);
                    if (b % period == 0) chord(e, 48, 8, true);
                    if (b % period == period * 3 / 4) chord(e, 48, 8, false);
                });
                std::printf("%-34s %7.2f%% %8.3f ms\n", c.name, res.percent, res.worstMs);
            }
        }

        {
            // The heaviest thing in the box: granular sampler, 8 notes, every voice's grain pool full.
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::Sampler));
            audio.prepare(sr, block);
            audio.setTrackParameter(0, SamplerEngine::Mode, (float) SamplerEngine::Granular);
            audio.setTrackParameter(0, SamplerEngine::GrainSize, 0.5f);
            audio.setTrackParameter(0, SamplerEngine::GrainDensity, 200.0f);
            audio.setTrackParameter(0, SamplerEngine::GrainSpray, 0.5f);
            auto* e = audio.getEngine(0);
            auto res = run(audio, sr, block, 8.0, [&](int b) { if (b == 0) chord(e, 48, 8, true); });
            std::printf("%-34s %7.2f%% %8.3f ms\n", "sampler, granular, grains maxed", res.percent, res.worstMs);
        }

        // Time stretch: the BEAT loop looped at 200 % under each algorithm, 8 notes. Ids and the
        // algorithm order (OFF TAPE CYCL GRAN SLCE SMER WSOL) are SamplerEngine's.
        static const char* const stretchNames[] = { "off", "TAPE", "CYCL", "GRAN", "SLCE", "SMER", "WSOL" };
        for (int algo = 1; algo <= 6; ++algo)
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::Sampler));
            audio.prepare(sr, block);
            audio.setTrackParameter(0, SamplerEngine::Mode, (float) SamplerEngine::Loop);
            audio.setTrackParameter(0, SamplerEngine::StretchMode, (float) algo);
            audio.setTrackParameter(0, SamplerEngine::StretchSpeed, 0.5f);
            auto* e = audio.getEngine(0);
            auto res = run(audio, sr, block, 8.0, [&](int b) { if (b == 0) chord(e, 48, 8, true); });
            std::printf("sampler, stretch %-17s %7.2f%% %8.3f ms\n", stretchNames[algo], res.percent, res.worstMs);
        }

        // 2. Effects alone on a busy synth track.
        auto fxCase = [&](const char* label, const std::function<void(AudioEngine&)>& setup)
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.prepare(sr, block);
            setup(audio);
            auto* e = audio.getEngine(0);
            auto res = run(audio, sr, block, 6.0, [&](int b) { if (b % 180 == 0) chord(e, 48, 4, true); if (b % 180 == 120) chord(e, 48, 4, false); });
            std::printf("%-34s %7.2f%% %8.3f ms\n", label, res.percent, res.worstMs);
        };
        std::printf("\n%-34s %8s %10s\n", "synth, 4 notes, plus", "average", "worst blk");
        fxCase("nothing", [](AudioEngine&) {});
        fxCase("DIRT + TALK + MODFX all on", [](AudioEngine& a) { for (int id = TrackFxParam::DIRT_BASE; id < TrackFxParam::END; ++id) a.setTrackParameter(0, id, 0.5f); });
        for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
        {
            const juce::String label = juce::String("insert ") + AudioEngine::effectTypeName((EffectType) t);
            fxCase(label.toRawUTF8(), [t](AudioEngine& a) { a.addInsertEffect(0, (EffectType) t); });
        }

        // 3. A full session: eight tracks, every one busy, three effect blocks and two inserts each.
        auto session = [&](bool heavy, int notes)
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            const EngineType types[8] = { EngineType::UnifiedVoice, EngineType::Drum, EngineType::FM, EngineType::MacroOsc,
                                          EngineType::Wavetable, EngineType::WaveSeq, EngineType::Sampler, EngineType::UnifiedVoice };
            for (int t = 0; t < 8; ++t)
                audio.addEngine(t, AudioEngine::createEngine(types[t]));
            audio.prepare(sr, block);
            if (heavy)
                for (int t = 0; t < 8; ++t)
                {
                    for (int id = TrackFxParam::DIRT_BASE; id < TrackFxParam::END; ++id) audio.setTrackParameter(t, id, 0.5f);
                    audio.addInsertEffect(t, EffectType::DelayEffect);
                    audio.addInsertEffect(t, EffectType::HallReverbEffect);
                    audio.setTrackParameter(t, MixParam::SEND_BASE, 0.4f);
                    audio.setTrackParameter(t, MixParam::SEND_BASE + 1, 0.4f);
                    // Both LFOs of every engine busy.
                    audio.setTrackParameter(t, types[t] == EngineType::UnifiedVoice ? ModParam::lfoParamId(0, ModParam::LFO_DEPTH) : 159, 0.5f);
                    audio.setTrackParameter(t, types[t] == EngineType::UnifiedVoice ? ModParam::lfoParamId(0, ModParam::LFO_DEST) : 165, 1.0f);
                }
            auto res = run(audio, sr, block, 10.0, [&](int b)
            {
                const int period = (int) (0.5 * sr / block);
                for (int t = 0; t < 8; ++t)
                {
                    auto* e = audio.getEngine(t);
                    const bool drum = types[t] == EngineType::Drum;
                    if (b % period == (t * 7) % period) chord(e, drum ? 0 : 40 + t, notes, true);
                    if (b % period == ((t * 7) + period * 3 / 4) % period) chord(e, drum ? 0 : 40 + t, notes, false);
                }
            });
            std::printf("%-34s %7.2f%% %8.3f ms   (block lasts %.2f ms)\n",
                        (juce::String(heavy ? "8 tracks, all effects, " : "8 tracks, no effects, ") + juce::String(notes) + " notes each").toRawUTF8(),
                        res.percent, res.worstMs, res.blockMs);
        };
        std::printf("\n%-34s %8s %10s\n", "full session", "average", "worst blk");
        session(false, 4);
        session(false, 8);
        session(true, 4);
        session(true, 8);
        return 0;
    }
}
