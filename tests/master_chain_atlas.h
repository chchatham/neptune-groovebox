#pragma once

#include <juce_core/juce_core.h>
#include "engine/AudioEngine.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------------------------
// Phase E5 of .ralph/spec-unify.md, the "nothing lost" renders: what the changes of the phase can
// reach (the master section, every insert type at WET 1 and at WET 0.5, the keyed effects), played
// with the same gesture before and after and written as 48 kHz stereo float WAV, so the two can be
// compared by number and by ear.
//
// It uses only what the AudioEngine offered before the phase too, and a sound source of its own
// that plays the same samples in any build: this file compiles unchanged against the commit before
// the phase (b4ee701). There the renders were made with
//     NEPTUNE_E5_ATLAS=<folder> NEPTUNE_E5_TAG=old NEPTUNE_E5_KEYIN=1 <test binary>
// (a three-line hook in that tree's main()), here with
//     NEPTUNE_E5_ATLAS=<folder> NEPTUNE_E5_TAG=new NEPTUNE_TEST_ONLY="Master Chain" <test binary>
// and compared with
//     NEPTUNE_E5_COMPARE=<folder> NEPTUNE_TEST_ONLY="Master Chain" <test binary>
// which prints, per pair: the level of each, the level of their difference against the old one
// (the null test), the largest difference of level in any 100 ms window, brightness early and
// late, and the time the sound takes to come up.
// (NEPTUNE_E5_KEYIN: the value of a KEY cell that means IN. It was 1; with the one key choice of
// the phase it is 17, the end of the list SELF, T1..T16, IN.)
// -----------------------------------------------------------------------------------------------
namespace e5atlas
{

// A sound source that is the same in every build: voices of six harmonics with an envelope, or a
// drum that strikes by itself four times a second. No randomness, no tables, no shared code.
class ToneEngine : public EngineBase
{
public:
    enum Kind { Keys = 0, Kick, Hot };

    explicit ToneEngine(Kind k = Keys) : kind(k) {}

    EngineType getType() const override { return EngineType::UnifiedVoice; }
    juce::String getName() const override { return "E5 tone"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr > 0.0 ? sr : 48000.0; }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int n = buffer.getNumSamples();
        float* l = buffer.getWritePointer(0);
        float* r = buffer.getWritePointer(1);
        const double twoPi = 6.283185307179586;
        for (int i = 0; i < n; ++i)
        {
            double sl = 0.0, sr = 0.0;
            if (kind == Kick)
            {
                const int period = (int) (sampleRate * 0.25);
                const int at = (int) (clock % (int64_t) period);
                if (at == 0) kickPhase = 0.0;
                const double t = (double) at / sampleRate;
                const double hz = 50.0 + 140.0 * std::exp(-t * 40.0);
                kickPhase += hz / sampleRate;
                const double env = std::exp(-t * 18.0) * std::min(1.0, t * 2000.0);
                sl = sr = 0.9 * env * std::sin(twoPi * kickPhase);
            }
            else
            {
                for (size_t v = 0; v < voices.size(); ++v)
                {
                    auto& voice = voices[v];
                    if (voice.level <= 1.0e-6 && ! voice.held) continue;
                    const double target = voice.held ? voice.velocity : 0.0;
                    const double rate = voice.held ? (voice.level < target ? 1.0 / (0.003 * sampleRate) : 0.0) : 0.0;
                    if (voice.held) voice.level = std::min(target, voice.level + rate * target);
                    else            voice.level *= std::exp(-1.0 / (0.12 * sampleRate));
                    if (voice.held) voice.body *= std::exp(-1.0 / (1.5 * sampleRate));
                    voice.phase += voice.hz / sampleRate;
                    if (voice.phase >= 1.0) voice.phase -= 1.0;
                    double s = 0.0;
                    for (int h = 1; h <= 6; ++h)
                        if (voice.hz * h < 0.45 * sampleRate)
                            s += std::sin(twoPi * voice.phase * h) / (double) h * (h == 1 ? 1.0 : voice.body);
                    s *= voice.level * (kind == Hot ? 1.6 : 0.35);
                    const double pan = 0.5 + 0.3 * (((int) v % 3) - 1);
                    sl += s * (1.0 - pan);
                    sr += s * pan;
                }
            }
            l[i] = (float) sl;
            r[i] = (float) sr;
            ++clock;
        }
    }

    void noteOn(int note, float velocity, int = -1) override
    {
        auto& v = voices[(size_t) (next++ % (int) voices.size())];
        v.note = note;
        v.hz = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        v.velocity = velocity;
        v.held = true;
        v.body = 1.0;
        v.phase = 0.0;
    }
    void noteOff(int note, int = -1) override
    {
        for (auto& v : voices)
            if (v.note == note) v.held = false;
    }
    void allNotesOff() override { for (auto& v : voices) { v.held = false; v.level = 0.0; } }

    // (ids 5 and 6 are two cells for the step-lock test; 764..771 the custom LFO shape)
    void setParameter(ParamId id, float value) override
    {
        if (id == 5 || id == 6) cell[(size_t) (id - 5)] = value;
        if (id >= LfoShapeParam::BASE && id < LfoShapeParam::END) shape[(size_t) (id - LfoShapeParam::BASE)] = value;
    }
    float getParameter(ParamId id) const override
    {
        if (id == 5 || id == 6) return cell[(size_t) (id - 5)];
        if (id >= LfoShapeParam::BASE && id < LfoShapeParam::END) return shape[(size_t) (id - LfoShapeParam::BASE)];
        return 0.0f;
    }
    juce::String getParameterName(ParamId) const override { return {}; }
    int getParameterCount() const override { return 2; }
    bool isRebuildParam(ParamId id) const override { return id == 5; }
    ParamPages getParamPages() const override
    {
        if (! withShape) return {};
        ParamPage page;
        page.name = "LSHP";
        for (int i = 0; i < 8; ++i)
            page.params[(size_t) i] = { "L", LfoShapeParam::BASE + i, -1, 1, 0 };
        return { page };
    }

    bool withShape = false;
    std::array<float, 2> cell { 0.0f, 0.0f };
    std::array<float, 8> shape { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

private:
    struct Voice { int note = -1; double hz = 0.0, phase = 0.0, level = 0.0, velocity = 0.0, body = 1.0; bool held = false; };
    Kind kind;
    double sampleRate = 48000.0;
    std::array<Voice, 8> voices;
    int next = 0;
    int64_t clock = 0;
    double kickPhase = 0.0;
};

struct Stereo { std::vector<float> left, right; };

inline void writeWav(const juce::File& file, const Stereo& s, int sampleRate)
{
    const uint32_t frames = (uint32_t) s.left.size(), bytes = frames * 8u;
    std::ofstream out(file.getFullPathName().toStdString(), std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write((const char*) &v, 4); };
    auto u16 = [&](uint16_t v) { out.write((const char*) &v, 2); };
    out.write("RIFF", 4); u32(36u + bytes); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(3); u16(2); u32((uint32_t) sampleRate); u32((uint32_t) sampleRate * 8u); u16(8); u16(32);
    out.write("data", 4); u32(bytes);
    for (uint32_t i = 0; i < frames; ++i)
    {
        out.write((const char*) &s.left[i], 4);
        out.write((const char*) &s.right[i], 4);
    }
}

inline bool readWav(const juce::File& file, Stereo& s)
{
    std::ifstream in(file.getFullPathName().toStdString(), std::ios::binary);
    if (! in) return false;
    char head[44];
    in.read(head, 44);
    uint32_t bytes = 0;
    std::memcpy(&bytes, head + 40, 4);
    const uint32_t frames = bytes / 8u;
    s.left.resize(frames);
    s.right.resize(frames);
    for (uint32_t i = 0; i < frames; ++i)
    {
        in.read((char*) &s.left[i], 4);
        in.read((char*) &s.right[i], 4);
    }
    return (bool) in;
}

static constexpr int RATE = 48000;
static constexpr int BLOCK = 256;

// One instrument: track 1 a drum that strikes by itself (heard only where `kickHeard`), track 2
// the keys. `setup` puts the effects in; the same notes are then played into every one.
inline Stereo play(double seconds, bool hot, bool kickHeard, const std::function<void(AudioEngine&)>& setup)
{
    AudioEngine audio;
    audio.addEngine(0, std::make_shared<ToneEngine>(ToneEngine::Kick));
    audio.addEngine(1, std::make_shared<ToneEngine>(hot ? ToneEngine::Hot : ToneEngine::Keys));
    audio.setTrackParameter(0, MixParam::VOLUME, kickHeard ? 0.8f : 0.0f);
    audio.setTrackParameter(1, MixParam::VOLUME, 0.8f);
    setup(audio);
    audio.prepare((double) RATE, BLOCK);

    struct Event { double at; int note; bool on; };
    const Event events[] = {
        { 0.02, 48, true }, { 0.02, 55, true }, { 0.02, 64, true }, { 0.70, 48, false }, { 0.70, 55, false }, { 0.70, 64, false },
        { 0.80, 72, true }, { 1.10, 72, false }, { 1.15, 36, true }, { 1.15, 67, true }, { 1.70, 36, false }, { 1.70, 67, false } };

    Stereo out;
    const int blocks = (int) (seconds * RATE / BLOCK);
    std::vector<float> l((size_t) BLOCK), r((size_t) BLOCK);
    size_t next = 0;
    for (int b = 0; b < blocks; ++b)
    {
        const double now = (double) b * BLOCK / RATE;
        while (next < std::size(events) && events[next].at <= now)
        {
            if (events[next].on) audio.getEngine(1)->noteOn(events[next].note, 0.9f, -1);
            else                 audio.getEngine(1)->noteOff(events[next].note, -1);
            ++next;
        }
        audio.renderBlock(l.data(), r.data(), BLOCK);
        out.left.insert(out.left.end(), l.begin(), l.end());
        out.right.insert(out.right.end(), r.begin(), r.end());
    }
    return out;
}

inline int insertWetId(int slot) { return AudioEngine::INSERT_ON_BASE + AudioEngine::MAX_INSERTS_PER_TRACK + slot; }

struct Entry { std::string name; Stereo sound; };

// Every sound of the atlas. keyIn: the value of a KEY cell that means IN in this build.
inline void render(float keyIn, const std::function<void(const Entry&)>& take)
{
    const int CELL = AudioEngine::EFFECT_PARAM_BASE, EXTRA = AudioEngine::INSERT_EXTRA_BASE;

    // The master section. As a new project has it (the limiter working on a hot sound), and with
    // every one of its eight values moved.
    take({ "master_default", play(2.0, true, true, [](AudioEngine&) {}) });
    take({ "master_default_quiet", play(2.0, false, true, [](AudioEngine&) {}) });
    take({ "master_worked", play(2.0, true, true, [](AudioEngine& a)
    {
        const float v[8] = { 6.0f, -4.0f, 800.0f, 3.0f, -24.0f, 4.0f, 3.0f, -1.0f };
        for (int i = 0; i < 8; ++i) a.setMasterParam(i, v[i]);
    }) });

    // Every insert type on the keys' track, as it starts: at WET 1 (how every stored sound has
    // it unless WET was turned) and at WET 0.5.
    for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
    {
        const std::string name = AudioEngine::effectTypeName((EffectType) t);
        for (float wet : { 1.0f, 0.5f })
            take({ (wet == 1.0f ? "wet100_" : "wet50_") + name, play(wet == 1.0f ? 1.25 : 2.0, false, false, [&](AudioEngine& a)
            {
                a.addInsertEffect(1, (EffectType) t);
                // (the equalisers and the limiter flat or idle would say nothing: they are set to work)
                if ((EffectType) t == EffectType::EQEffect)        { a.setTrackParameter(1, CELL + 0, 9.0f); a.setTrackParameter(1, CELL + 3, -9.0f); }
                if ((EffectType) t == EffectType::GraphicEQEffect) { a.setTrackParameter(1, CELL + 0, 9.0f); a.setTrackParameter(1, CELL + 2, -9.0f); }
                if ((EffectType) t == EffectType::LimiterEffect)   a.setTrackParameter(1, CELL + 0, -3.0f);
                if ((EffectType) t == EffectType::CompressorEffect || (EffectType) t == EffectType::SidechainEffect)
                {
                    const bool duck = (EffectType) t == EffectType::SidechainEffect;
                    a.setTrackParameter(1, CELL + (duck ? 1 : 0), -30.0f);
                    a.setTrackParameter(1, CELL + (duck ? 2 : 1), 8.0f);
                }
                a.setTrackParameter(1, insertWetId(0), wet);
            }) });
    }

    // The keyed effects, listening to the drum on track 1 (its fader shut: only its work is heard).
    take({ "duck_src_t1", play(2.0, false, false, [&](AudioEngine& a)
    {
        a.addInsertEffect(1, EffectType::SidechainEffect);
        a.setTrackParameter(1, CELL + 0, 1.0f);         // SRC T1
        a.setTrackParameter(1, CELL + 1, -30.0f);
        a.setTrackParameter(1, CELL + 2, 10.0f);
    }) });
    take({ "duck_src_in", play(2.0, false, false, [&](AudioEngine& a)
    {
        a.setTrackParameter(1, TrackInParam::SRC, 1.0f);
        a.addInsertEffect(1, EffectType::SidechainEffect);
        a.setTrackParameter(1, CELL + 0, 17.0f);        // SRC IN
        a.setTrackParameter(1, CELL + 1, -30.0f);
        a.setTrackParameter(1, CELL + 2, 10.0f);
    }) });
    take({ "cmp_key_in", play(2.0, false, false, [&](AudioEngine& a)
    {
        a.setTrackParameter(1, TrackInParam::SRC, 1.0f);
        a.addInsertEffect(1, EffectType::CompressorEffect);
        a.setTrackParameter(1, CELL + 0, -30.0f);
        a.setTrackParameter(1, CELL + 1, 10.0f);
        a.setTrackParameter(1, EXTRA + 0, keyIn);
    }) });
    take({ "wah_key_in", play(2.0, false, false, [&](AudioEngine& a)
    {
        a.setTrackParameter(1, TrackInParam::SRC, 1.0f);
        a.addInsertEffect(1, EffectType::WahEffect);
        a.setTrackParameter(1, CELL + 2, 1.0f);         // SENS
        a.setTrackParameter(1, EXTRA + 3, keyIn);
    }) });
    take({ "trns_key_in", play(2.0, false, false, [&](AudioEngine& a)
    {
        a.setTrackParameter(1, TrackInParam::SRC, 1.0f);
        a.addInsertEffect(1, EffectType::TransientEffect);
        a.setTrackParameter(1, CELL + 0, 1.0f);
        a.setTrackParameter(1, CELL + 1, -1.0f);
        a.setTrackParameter(1, EXTRA + 0, keyIn);
    }) });
    take({ "voc_in", play(2.0, false, false, [&](AudioEngine& a)
    {
        a.setTrackParameter(1, TrackInParam::SRC, 1.0f);
        a.addInsertEffect(1, EffectType::VocoderEffect);
    }) });
}

inline int renderAll(const juce::File& folder, const juce::String& tag, float keyIn)
{
    folder.createDirectory();
    int count = 0;
    render(keyIn, [&](const Entry& e)
    {
        writeWav(folder.getChildFile(juce::String(e.name) + "_" + tag + ".wav"), e.sound, RATE);
        ++count;
    });
    std::cout << "E5 atlas: " << count << " sounds written to " << folder.getFullPathName() << " as *_" << tag << ".wav" << std::endl;
    return 0;
}

// ---- cost: the whole instrument --------------------------------------------------------------
// Microseconds per block of 512 samples at 48 kHz: four tracks of the test tone, each with a chorus
// and a compressor, both sends up, the master section as a new project has it.
inline double engineBlockMicroseconds()
{
    AudioEngine audio;
    for (int t = 0; t < 4; ++t)
    {
        audio.addEngine(t, std::make_shared<ToneEngine>(ToneEngine::Keys));
        audio.addInsertEffect(t, EffectType::ChorusEffect);
        audio.addInsertEffect(t, EffectType::CompressorEffect);
        audio.setTrackParameter(t, MixParam::VOLUME, 0.6f);
        audio.setTrackParameter(t, MixParam::SEND_BASE, 0.3f);
        audio.setTrackParameter(t, MixParam::SEND_BASE + 1, 0.3f);
    }
    audio.prepare(48000.0, 512);
    for (int t = 0; t < 4; ++t)
        for (int note : { 48 + t, 55 + t, 64 + t })
            audio.getEngine(t)->noteOn(note, 0.8f, -1);
    std::vector<float> l(512), r(512);
    for (int b = 0; b < 200; ++b) audio.renderBlock(l.data(), r.data(), 512);
    const int blocks = 3000;
    const juce::int64 t0 = juce::Time::getHighResolutionTicks();
    for (int b = 0; b < blocks; ++b) audio.renderBlock(l.data(), r.data(), 512);
    return 1.0e6 * juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0) / blocks;
}

// ---- measuring a pair --------------------------------------------------------------------------
struct Measure
{
    double oldDb = 0.0, newDb = 0.0;        // level of each, dB re full scale (both channels)
    double nullDb = 0.0;                    // level of (new - old) against old
    double windowDb = 0.0;                  // the largest difference of level in a 100 ms window
    double brightOld[2] {}, brightNew[2] {};// brightness (level of the first difference against the level), first and second half
    double riseOldMs = 0.0, riseNewMs = 0.0;// time until a 5 ms window first reaches half the loudest one
    double peakDiff = 0.0;
};

inline double levelOf(const Stereo& s, size_t from, size_t to)
{
    double sum = 0.0;
    to = std::min(to, s.left.size());
    for (size_t i = from; i < to; ++i)
        sum += (double) s.left[i] * s.left[i] + (double) s.right[i] * s.right[i];
    return to > from ? std::sqrt(sum / (2.0 * (double) (to - from))) : 0.0;
}
inline double toDb(double x) { return 20.0 * std::log10(std::max(x, 1.0e-9)); }

inline double brightness(const Stereo& s, size_t from, size_t to)
{
    double d = 0.0, a = 0.0;
    to = std::min(to, s.left.size());
    for (size_t i = from + 1; i < to; ++i)
    {
        const double x = (double) s.left[i] - s.left[i - 1];
        d += x * x;
        a += (double) s.left[i] * s.left[i];
    }
    return a > 0.0 ? std::sqrt(d / a) : 0.0;
}

inline double riseMs(const Stereo& s)
{
    const size_t w = (size_t) (RATE * 0.005);
    double loudest = 0.0;
    for (size_t i = 0; i + w <= s.left.size(); i += w) loudest = std::max(loudest, levelOf(s, i, i + w));
    for (size_t i = 0; i + w <= s.left.size(); i += w)
        if (levelOf(s, i, i + w) >= 0.5 * loudest) return 1000.0 * (double) i / RATE;
    return 0.0;
}

inline Measure measure(const Stereo& before, const Stereo& after)
{
    Measure m;
    const size_t n = std::min(before.left.size(), after.left.size());
    Stereo diff;
    diff.left.resize(n); diff.right.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        diff.left[i] = after.left[i] - before.left[i];
        diff.right[i] = after.right[i] - before.right[i];
        m.peakDiff = std::max(m.peakDiff, (double) std::max(std::abs(diff.left[i]), std::abs(diff.right[i])));
    }
    const double oldLevel = levelOf(before, 0, n), newLevel = levelOf(after, 0, n);
    m.oldDb = toDb(oldLevel);
    m.newDb = toDb(newLevel);
    m.nullDb = toDb(levelOf(diff, 0, n) / std::max(oldLevel, 1.0e-9));
    const size_t w = (size_t) (RATE / 10);
    for (size_t i = 0; i + w <= n; i += w)
    {
        const double a = levelOf(before, i, i + w), b = levelOf(after, i, i + w);
        if (std::max(a, b) > 1.0e-4)        // (windows of near silence say nothing)
            m.windowDb = std::max(m.windowDb, std::abs(toDb(b) - toDb(a)));
    }
    for (int half = 0; half < 2; ++half)
    {
        m.brightOld[half] = brightness(before, half * n / 2, (half + 1) * n / 2);
        m.brightNew[half] = brightness(after, half * n / 2, (half + 1) * n / 2);
    }
    m.riseOldMs = riseMs(before);
    m.riseNewMs = riseMs(after);
    return m;
}

// MATCH: the difference is below -80 dB of the sound (inaudible; -inf = the same samples).
// CLOSE: no 100 ms window differs by more than 1 dB and the brightness by no more than 10 %.
inline const char* verdict(const Measure& m)
{
    if (m.nullDb <= -80.0) return "MATCH";
    bool bright = true;
    for (int h = 0; h < 2; ++h)
        bright = bright && std::abs(m.brightNew[h] - m.brightOld[h]) <= 0.1 * std::max(m.brightOld[h], 1.0e-6);
    return (m.windowDb <= 1.0 && bright) ? "CLOSE" : "DIFFERENT";
}

inline int compareAll(const juce::File& folder)
{
    auto files = folder.findChildFiles(juce::File::findFiles, false, "*_old.wav");
    files.sort();
    std::cout << "name | old dB | new dB | null dB | worst 100 ms window dB | bright old (1st, 2nd half) | bright new | rise old ms | rise new ms | verdict" << std::endl;
    for (const auto& f : files)
    {
        const juce::String name = f.getFileNameWithoutExtension().dropLastCharacters(4);
        Stereo before, after;
        if (! readWav(f, before) || ! readWav(folder.getChildFile(name + "_new.wav"), after))
        {
            std::cout << name << " | no pair" << std::endl;
            continue;
        }
        const Measure m = measure(before, after);
        std::cout << name << " | " << juce::String(m.oldDb, 2) << " | " << juce::String(m.newDb, 2) << " | "
                  << (m.peakDiff == 0.0 ? juce::String("-inf (same samples)") : juce::String(m.nullDb, 1)) << " | " << juce::String(m.windowDb, 2)
                  << " | " << juce::String(m.brightOld[0], 4) << ", " << juce::String(m.brightOld[1], 4)
                  << " | " << juce::String(m.brightNew[0], 4) << ", " << juce::String(m.brightNew[1], 4)
                  << " | " << (int) std::lround(m.riseOldMs) << " | " << (int) std::lround(m.riseNewMs) << " | " << verdict(m) << std::endl;
    }
    return 0;
}

} // namespace e5atlas
