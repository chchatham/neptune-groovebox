#pragma once

#include "cymbal_machine_tests.h"
#include "legacy/CymbalMachine494.h"
#include "legacy/LegacyHat.h"
#include "../src/engine/DrumEngine.h"
#include "../src/project/CymbalPresets.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// Old against new, for everything phase U2b folded into the resonator drum and the oscillator
// bank (.ralph/spec-unify.md): every old sound is rendered by the code that made it - the drum
// voice's former types and the cymbal machine as it was on main (b4ee701), all still in the
// product; the hats and the cymbal machine as it was at 494c51e, in tests/legacy - and the sound
// that replaces it by the product, with the same gesture, and the two are measured the same way.
//   NEPTUNE_ATLAS=1          prints the table (the "Resonator Drum" suite)
//   NEPTUNE_ATLAS_DIR=<dir>  also writes every pair as 48 kHz WAV (24 bit, at half scale): <name>_old.wav, <name>_new.wav
//   NEPTUNE_SND_OVERRIDE=<file> lines "TAG id value" laid over a sound of the resonator drum before
//                            it is rendered (id: an RSN parameter id; -1 TUNE, -2 DEC, -3 the
//                            seconds a second of DEC is, -4 LEVL of the pad): for tuning a sound
//                            against its reference without building.
namespace drumatlas
{
    using namespace cymbaltest;
    static constexpr double SR = 48000.0;

    // ---- the old sounds --------------------------------------------------------------------------
    // A former struck drum type, by the drum voice's own old code.
    inline std::vector<float> oldDrum(DrumSynthType type, float tune, float decay, float tone, float drive, float snap,
                                      float level, double seconds, float velocity = 1.0f)
    {
        DrumVoice v;
        v.setSampleRate(SR);
        v.setType(type);
        v.setTune(tune); v.setDecay(decay); v.setTone(tone); v.setDrive(drive); v.setSnap(snap); v.setLevel(level);
        v.noteOn(velocity);
        std::vector<float> out((size_t) (seconds * SR));
        for (auto& s : out) s = v.process();
        return out;
    }

    inline std::vector<float> oldHat(bool open, float tune, float decay, float tone, float level, double seconds,
                                     float velocity = 1.0f, double held = 10.0)
    {
        LegacyHat h;
        h.setSampleRate(SR);
        h.open = open; h.tune = tune; h.decay = decay; h.tone = tone; h.level = level;
        h.noteOn(velocity);
        std::vector<float> out((size_t) (seconds * SR));
        for (size_t i = 0; i < out.size(); ++i)
        {
            if (i == (size_t) (held * SR)) h.noteOff();
            out[i] = h.process();
        }
        return out;
    }

    // One of the cymbal machine's ten ready-made sounds: the pad's own cells and its eight (in
    // the order of its page then: main's MODL PING COLR EXC DAMP BLOM COUP LOFI, and before that
    // MODL PING COLR NOIS DAMP BLOM VSNS LOFI).
    struct OldCymbal
    {
        const char* name;
        float tune, decay, tone, drive, snap, level;
        float page[8];
    };

    inline const std::vector<OldCymbal>& cymbalsMain()
    {
        static const std::vector<OldCymbal> all = {
            { "analog_cymbal", 8000.0f, 1.2f, 0.5f, 0.0f, 0.5f, 0.5f,  { 0, 0.5f, 0.5f, 0.0f, 0.4f, 0.0f, 0.0f, 0.0f } },
            { "ring_cymbal",   8000.0f, 0.9f, 0.5f, 0.0f, 0.5f, 0.45f, { 1, 0.4f, 0.55f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f } },
            { "house_ride",    6400.0f, 0.9f, 0.5f, 0.0f, 0.6f, 0.5f,  { 2, 0.6f, 0.55f, 0.0f, 0.4f, 0.2f, 0.0f, 0.35f } },
            { "ride",          8000.0f, 1.3f, 0.5f, 0.0f, 0.7f, 0.5f,  { 2, 0.5f, 0.5f, 0.0f, 0.4f, 0.3f, 0.0f, 0.0f } },
            { "ride_bell",     8000.0f, 1.0f, 0.5f, 0.0f, 0.75f, 0.5f, { 3, 0.8f, 0.5f, 0.0f, 0.4f, 0.2f, 0.0f, 0.0f } },
            { "crash",         8000.0f, 1.5f, 0.5f, 0.0f, 0.6f, 0.5f,  { 4, 0.4f, 0.5f, 0.0f, 0.45f, 0.5f, 0.0f, 0.0f } },
            { "china",         8000.0f, 1.0f, 0.5f, 0.15f, 0.7f, 0.45f, { 5, 0.5f, 0.5f, 0.0f, 0.4f, 0.5f, 0.0f, 0.0f } },
            { "splash",        8000.0f, 0.8f, 0.5f, 0.0f, 0.6f, 0.45f, { 6, 0.4f, 0.5f, 0.0f, 0.4f, 0.4f, 0.0f, 0.0f } },
            { "gong",          8000.0f, 2.0f, 0.5f, 0.0f, 0.2f, 0.6f,  { 7, 0.5f, 0.5f, 0.0f, 0.4f, 0.6f, 0.0f, 0.0f } },
            { "tam_tam",       8000.0f, 2.0f, 0.5f, 0.0f, 0.15f, 0.6f, { 8, 0.5f, 0.5f, 0.0f, 0.4f, 0.8f, 0.0f, 0.0f } } };
        return all;
    }

    inline const std::vector<OldCymbal>& cymbals494()
    {
        static const std::vector<OldCymbal> all = {
            { "analog_cymbal", 8000.0f, 1.2f, 0.5f, 0.0f, 0.5f, 0.5f,  { 0, 0.5f, 0.5f, 0.0f, 0.4f, 0.0f, 0.5f, 0.0f } },
            { "ring_cymbal",   8000.0f, 0.9f, 0.5f, 0.0f, 0.5f, 0.45f, { 1, 0.4f, 0.55f, 0.25f, 0.5f, 0.0f, 0.5f, 0.0f } },
            { "house_ride",    6400.0f, 0.9f, 0.5f, 0.0f, 0.75f, 0.5f, { 2, 0.6f, 0.55f, 0.6f, 0.4f, 0.2f, 0.5f, 0.45f } },
            { "ride",          8000.0f, 1.3f, 0.5f, 0.0f, 0.7f, 0.5f,  { 2, 0.5f, 0.5f, 0.5f, 0.4f, 0.3f, 0.6f, 0.0f } },
            { "ride_bell",     8000.0f, 1.0f, 0.5f, 0.0f, 0.75f, 0.5f, { 3, 0.8f, 0.5f, 0.2f, 0.4f, 0.2f, 0.5f, 0.0f } },
            { "crash",         8000.0f, 1.5f, 0.5f, 0.0f, 0.6f, 0.5f,  { 4, 0.4f, 0.5f, 0.6f, 0.45f, 0.5f, 0.6f, 0.0f } },
            { "china",         8000.0f, 1.0f, 0.5f, 0.15f, 0.7f, 0.45f, { 5, 0.5f, 0.5f, 0.7f, 0.4f, 0.5f, 0.6f, 0.0f } },
            { "splash",        8000.0f, 0.8f, 0.5f, 0.0f, 0.6f, 0.45f, { 6, 0.4f, 0.5f, 0.6f, 0.4f, 0.4f, 0.6f, 0.0f } },
            { "gong",          8000.0f, 2.0f, 0.5f, 0.0f, 0.2f, 0.6f,  { 7, 0.5f, 0.5f, 0.4f, 0.4f, 0.6f, 0.7f, 0.0f } },
            { "tam_tam",       8000.0f, 2.0f, 0.5f, 0.0f, 0.15f, 0.6f, { 8, 0.5f, 0.5f, 0.5f, 0.4f, 0.8f, 0.7f, 0.0f } } };
        return all;
    }

    // What the drum voice did after the cymbal machine: DRIV (a soft clip that keeps the peak
    // level) and LEVL. (TONE at its middle left the machine's sound as it was.)
    inline float afterMachine(float x, float drive, float level)
    {
        if (drive > 0.0f)
        {
            const float gain = 1.0f + drive * 6.0f;
            x = dsp::fastTanh(std::clamp(x * gain, -3.0f, 3.0f)) * std::pow(gain, -0.3f);
        }
        return x * level;
    }

    template <typename Machine>
    std::vector<float> oldCymbal(const OldCymbal& c, const std::vector<double>& strokes, double seconds, float velocity = 1.0f)
    {
        Machine m;
        m.setSampleRate(SR);
        for (int i = 0; i < 8; ++i) m.setParam(i, c.page[i]);
        typename Machine::Strike hit;
        hit.velocity = velocity; hit.tune = c.tune; hit.decay = c.decay; hit.tone = c.tone; hit.snap = c.snap;
        std::vector<float> out((size_t) (seconds * SR));
        size_t next = 0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            if (next < strokes.size() && i >= (size_t) (strokes[next] * SR))
            {
                m.strike(hit);
                ++next;
            }
            out[i] = afterMachine(m.process(c.tune), c.drive, c.level);
        }
        return out;
    }

    // ---- the new sounds --------------------------------------------------------------------------
    struct Override { std::string tag; int id; float value; };
    inline const std::vector<Override>& overrides()
    {
        static const std::vector<Override> all = []
        {
            std::vector<Override> o;
            if (const char* path = std::getenv("NEPTUNE_SND_OVERRIDE"))
            {
                std::ifstream in(path);
                std::string line;
                while (std::getline(in, line))
                {
                    std::istringstream row(line);
                    Override r;
                    if (row >> r.tag >> r.id >> r.value)
                        o.push_back(r);
                }
            }
            return o;
        }();
        return all;
    }

    // A kit with pad 0 set by `set` (parameter ids and values, as a preset holds them), played.
    struct Played
    {
        std::vector<double> strokes { 0.0 };
        float velocity = 1.0f;
        double release = 0.1;           // seconds after each stroke the pad is let go
        std::vector<std::pair<double, float>> pressure;     // (when, how hard): the pad squeezed
    };

    inline void layOver(DrumEngine& e, int padIndex)
    {
        using D = DrumEngine;
        if (! e.isResonator(padIndex))
            return;
        const auto& sounds = resonatorSounds();
        const int index = (int) e.getParameter(D::SOUND_BASE + padIndex * D::SOUND_CELLS);
        const auto& sound = sounds[(size_t) index];
        float scale = sound.decScale;
        for (const auto& o : overrides())
        {
            if (o.tag != sound.tag)
                continue;
            if (o.id == -1)      e.setParameter(padIndex * D::PARAMS_PER_VOICE + D::TUNE, o.value);
            else if (o.id == -2) e.setParameter(padIndex * D::PARAMS_PER_VOICE + D::DECAY, o.value);
            else if (o.id == -3) scale = o.value;
            else if (o.id == -4) e.setParameter(padIndex * D::PARAMS_PER_VOICE + D::LEVEL, o.value);
            else
            {
                const int cell = ResonatorEngine::voiceCell(o.id);
                if (cell >= 0) e.getVoice(padIndex).setResonatorBase(cell, o.value);
                else           ResonatorEngine::applySound(e.getVoice(padIndex).getResonator(), o.id, o.value);
            }
        }
        e.getVoice(padIndex).setResonatorHost(scale, sound.pressEnergy, sound.pressDamp, sound.gated, ! sound.rings(),
                                              sound.driveWire, sound.dec);
    }

    inline std::vector<float> newDrum(const std::map<int, float>& set, double seconds, const Played& how = {}, double sr = SR)
    {
        DrumEngine e;
        e.prepareToPlay(sr, 64);
        for (const auto& kv : set)
            e.setParameter(kv.first, kv.second);
        e.setParameter(DrumEngine::PAN, 0.5f);
        e.setSlotAftertouchDest(0, DrumATDest::Off);
        layOver(e, 0);
        juce::AudioBuffer<float> buf(2, 64);
        juce::MidiBuffer midi;
        std::vector<float> out;
        const int blocks = (int) (seconds * sr) / 64;
        size_t next = 0, nextPress = 0;
        double letGo = -1.0;
        for (int b = 0; b < blocks; ++b)
        {
            const double t = (double) b * 64.0 / sr;
            if (letGo >= 0.0 && t >= letGo)
            {
                e.noteOff(0);
                letGo = -1.0;
            }
            if (next < how.strokes.size() && t >= how.strokes[next])
            {
                e.noteOn(0, how.velocity);
                letGo = how.strokes[next] + how.release;
                ++next;
            }
            while (nextPress < how.pressure.size() && t >= how.pressure[nextPress].first)
                e.aftertouch(0, how.pressure[nextPress++].second);
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < 64; ++i)        // (a centred pad is 3 dB down on each side)
                out.push_back((buf.getSample(0, i) + buf.getSample(1, i)) * 0.70710678f);
        }
        return out;
    }

    // ---- the measures ----------------------------------------------------------------------------
    struct Features
    {
        double peak = 0.0, level = 0.0;         // the whole render's
        double curve[6] = {};                   // dB in 0-20 ms, 20-100 ms, 0.1-0.3 s, 0.3-1 s, 1-3 s, 3-8 s
        double t60 = 0.0, bandT60[4] = {};      // seconds; below 1 k, 1-4 k, 4-10 k, above
        double bandShare[4] = {};               // of the power at the loudest moment of each band
        double bright = 0.0, brightLate = 0.0;  // centroid, Hz: of the first 21 ms, and 0.3-0.6 s
        double attack = 0.0;                    // seconds to the loudest 5 ms
        double repeats = 0.0;                   // 1 = a periodic tone, 0 = noise
        std::vector<double> partials;           // the strongest, Hz
    };

    inline Features measure(const std::vector<float>& x)
    {
        static const double edge[7] = { 0.0, 0.02, 0.1, 0.3, 1.0, 3.0, 8.0 };
        Features f;
        for (float s : x) f.peak = std::max(f.peak, (double) std::abs(s));
        const double length = (double) x.size() / SR;
        f.level = rms(x, 0.0, length, SR);
        for (int i = 0; i < 6; ++i)
            f.curve[i] = edge[i] < length ? dB(rms(x, edge[i], std::min(edge[i + 1], length), SR)) : -200.0;
        const Bands b = bands(x, SR);
        f.t60 = b.t60[4];
        double total = 0.0;
        for (int k = 0; k < 4; ++k) total += b.level[k];
        for (int k = 0; k < 4; ++k)
        {
            f.bandT60[k] = b.t60[k];
            f.bandShare[k] = total > 0.0 ? b.level[k] / total : 0.0;
        }
        f.bright = centroid(x, 0.0, SR, 1024);
        f.brightLate = length > 0.7 ? brightness(x, 0.3, 0.6, SR) : 0.0;
        double best = 0.0;
        for (double t = 0.0; t + 0.005 < std::min(length, 4.0); t += 0.0025)
        {
            const double r = rms(x, t, t + 0.005, SR);
            if (r > best * 1.05) { best = r; f.attack = t; }
        }
        // (Of a sound that is over by then nothing is said.)
        f.repeats = length > 0.45 && rms(x, 0.1, 0.4, SR) > f.level * 3.0e-3 ? repeat(x, SR) : 0.0;
        // The strongest partials between 40 Hz and 12 kHz in the 0.68 s from 20 ms on: tops of the
        // spectrum within 30 dB of the highest that stand 10 dB above what lies 8 bins either side.
        if (x.size() >= 32768 + (size_t) (0.02 * SR))
        {
            const size_t n = 32768;
            const auto p = spectrum(x, (size_t) (0.02 * SR), n);
            const size_t lo = (size_t) (40.0 * (double) n / SR), hi = (size_t) (12000.0 * (double) n / SR);
            double mx = 0.0;
            for (size_t i = lo; i < hi; ++i) mx = std::max(mx, p[i]);
            std::vector<std::pair<double, double>> byPower;
            for (size_t i = lo + 8; i < hi; ++i)
            {
                if (p[i] < mx * 1.0e-3) continue;
                bool top = true;
                for (int d = -3; d <= 3; ++d) if (d != 0 && p[(size_t) ((int) i + d)] > p[i]) top = false;
                if (! top) continue;
                if (p[i] > 10.0 * std::max(p[i - 8], p[i + 8]))
                    byPower.push_back({ p[i], (double) i * SR / (double) n });
            }
            std::sort(byPower.rbegin(), byPower.rend());
            for (size_t i = 0; i < std::min<size_t>(8, byPower.size()); ++i) f.partials.push_back(byPower[i].second);
            std::sort(f.partials.begin(), f.partials.end());
        }
        return f;
    }

    // The rate at which the level of the band `hz` +- 12 % of an octave beats, Hz (0: it does
    // not): the strongest line of the spectrum of that band's envelope between 0.3 and 12 Hz, where
    // it stands 6 dB above the envelope's mean wobble.
    inline double beatRate(const std::vector<float>& x, double hz, double from, double seconds)
    {
        const size_t hop = 240;         // 5 ms
        std::vector<double> env;
        // (Heterodyne: the band brought down to nothing, then its level 200 times a second.)
        double re = 0.0, im = 0.0;
        const double k = 1.0 - std::exp(-2.0 * 3.14159265358979 * std::max(12.0, 0.06 * hz) / SR);
        double re2 = 0.0, im2 = 0.0;
        const size_t a = (size_t) (from * SR), b = std::min(x.size(), (size_t) ((from + seconds) * SR));
        for (size_t i = a; i < b; ++i)
        {
            const double ph = 2.0 * 3.14159265358979 * hz * (double) i / SR;
            re += k * ((double) x[i] * std::cos(ph) - re);
            im += k * ((double) x[i] * std::sin(ph) - im);
            re2 += k * (re - re2);
            im2 += k * (im - im2);
            if ((i - a) % hop == 0)
                env.push_back(std::sqrt(re2 * re2 + im2 * im2));
        }
        if (env.size() < 64)
            return 0.0;
        // Take the slow decay out: the logarithm, less its straight line.
        const size_t n = env.size();
        std::vector<double> l(n);
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i = 0; i < n; ++i)
        {
            l[i] = std::log(env[i] + 1.0e-12);
            sx += (double) i; sy += l[i]; sxx += (double) i * i; sxy += (double) i * l[i];
        }
        const double slope = ((double) n * sxy - sx * sy) / ((double) n * sxx - sx * sx), offset = (sy - slope * sx) / (double) n;
        double bestPower = 0.0, bestHz = 0.0, mean = 0.0;
        int lines = 0;
        for (double f = 0.3; f <= 12.0; f += 0.05, ++lines)
        {
            double c = 0.0, s = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                const double v = l[i] - offset - slope * (double) i, ph = 2.0 * 3.14159265358979 * f * (double) i * (double) hop / SR;
                c += v * std::cos(ph);
                s += v * std::sin(ph);
            }
            const double power = (c * c + s * s) / ((double) n * (double) n);
            mean += power;
            if (power > bestPower) { bestPower = power; bestHz = f; }
        }
        mean /= (double) std::max(1, lines);
        return bestPower > 4.0 * mean && bestPower > 1.0e-4 ? bestHz : 0.0;
    }

    struct Verdict
    {
        const char* word = "LOST";
        double level = 0.0;         // dB, new against old
        double curve = 0.0;         // the largest difference of the loudness curve, dB (where either is above -50 dB)
        double t60 = 1.0;           // new / old
        double bright = 1.0, brightLate = 1.0;
        double found = 1.0;         // the share of the old sound's strongest partials the new one has within 1.5 %
        double repeats = 0.0;       // new - old
    };

    inline Verdict compare(const Features& o, const Features& n)
    {
        Verdict v;
        v.level = dB(n.level / std::max(o.level, 1.0e-9));
        const double floor = std::max(dB(o.level), dB(n.level)) - 50.0;
        for (int i = 0; i < 6; ++i)
            if (o.curve[i] > floor || n.curve[i] > floor)
                v.curve = std::max(v.curve, std::abs(std::max(n.curve[i], floor) - std::max(o.curve[i], floor)));
        v.t60 = n.t60 / std::max(o.t60, 1.0e-3);
        v.bright = n.bright / std::max(o.bright, 1.0);
        v.brightLate = o.brightLate > 0.0 && n.brightLate > 0.0 ? n.brightLate / o.brightLate : 1.0;
        // (Partials are compared where the old sound is a tone: a noise's tops are anywhere.)
        if (! o.partials.empty() && o.repeats > 0.5)
        {
            int found = 0;
            for (double f : o.partials)
                for (double g : n.partials)
                    if (std::abs(g / f - 1.0) < 0.015) { ++found; break; }
            v.found = (double) found / (double) o.partials.size();
        }
        v.repeats = n.repeats - o.repeats;
        auto within = [](double ratio, double factor) { return ratio <= factor && ratio >= 1.0 / factor; };
        const bool match = std::abs(v.level) <= 1.0 && v.curve <= 3.0 && within(v.t60, 1.2) && within(v.bright, 1.15)
                           && within(v.brightLate, 1.15) && v.found >= 0.8 && std::abs(v.repeats) <= 0.2;
        const bool close = std::abs(v.level) <= 3.0 && v.curve <= 8.0 && within(v.t60, 1.6) && within(v.bright, 1.5)
                           && within(v.brightLate, 1.5) && v.found >= 0.5 && std::abs(v.repeats) <= 0.45;
        v.word = match ? "MATCH" : (close ? "CLOSE" : "LOST");
        return v;
    }

    inline void writeWav(const juce::String& name, const std::vector<float>& x)
    {
        const char* dir = std::getenv("NEPTUNE_ATLAS_DIR");
        if (dir == nullptr || x.empty())
            return;
        juce::File folder(dir);
        folder.createDirectory();
        juce::File file = folder.getChildFile(name + ".wav");
        file.deleteFile();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
        if (stream == nullptr)
            return;
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), SR, 1, 24, {}, 0));
        if (writer == nullptr)
            return;
        stream.release();
        // (Half scale, old and new alike: a tom's stroke peaks a little over full scale.)
        std::vector<float> half(x);
        for (auto& v : half) v *= 0.5f;
        const float* channel = half.data();
        writer->writeFromFloatArrays(&channel, 1, (int) half.size());
    }

    inline juce::String describe(const Features& f)
    {
        juce::String s;
        s << "peak " << juce::String(f.peak, 3) << " rms " << juce::String(dB(f.level), 1) << " dB | curve";
        for (double c : f.curve) s << " " << (c < -150.0 ? juce::String("-") : juce::String(c, 0));
        s << " | t60 " << juce::String(f.t60, 2) << " [";
        for (double t : f.bandT60) s << " " << juce::String(t, 2);
        s << " ] share [";
        for (double t : f.bandShare) s << " " << juce::String(t, 2);
        s << " ] | bright " << juce::String(f.bright, 0) << " late " << juce::String(f.brightLate, 0)
          << " | attack " << juce::String(f.attack * 1000.0, 1) << " ms | repeats " << juce::String(f.repeats, 2) << " | partials";
        for (double p : f.partials) s << " " << juce::String(p, 0);
        return s;
    }

    inline juce::String describe(const Verdict& v)
    {
        juce::String s;
        s << v.word << "  level " << juce::String(v.level, 1) << " dB, curve " << juce::String(v.curve, 1) << " dB, t60 x"
          << juce::String(v.t60, 2) << ", bright x" << juce::String(v.bright, 2) << " late x" << juce::String(v.brightLate, 2)
          << ", partials " << juce::String(v.found * 100.0, 0) << " %, repeats " << juce::String(v.repeats, 2);
        return s;
    }

    // One row of the atlas.
    struct Pair
    {
        juce::String name;
        std::vector<float> oldSound, newSound;
        Features oldF, newF;
        Verdict verdict;
    };

    inline Pair pair(const juce::String& name, std::vector<float> oldSound, std::vector<float> newSound)
    {
        Pair p;
        p.name = name;
        p.oldF = measure(oldSound);
        p.newF = measure(newSound);
        p.verdict = compare(p.oldF, p.newF);
        writeWav(name + "_old", oldSound);
        writeWav(name + "_new", newSound);
        p.oldSound = std::move(oldSound);
        p.newSound = std::move(newSound);
        return p;
    }

    inline bool printing() { return std::getenv("NEPTUNE_ATLAS") != nullptr; }
    inline void print(const Pair& p)
    {
        if (! printing())
            return;
        juce::Logger::writeToLog("[atlas] " + p.name + ": " + describe(p.verdict));
        juce::Logger::writeToLog("[atlas]    old " + describe(p.oldF));
        juce::Logger::writeToLog("[atlas]    new " + describe(p.newF));
    }
}
