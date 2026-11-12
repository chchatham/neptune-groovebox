#pragma once

#include "DrumVoice.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <iterator>
#include <vector>

// The break loops of assets/breaks, built into the program by CMake (juce_add_binary_data). A build
// without them still has the two breaks rendered below.
#if __has_include("NeptuneBreakData.h")
 #include "NeptuneBreakData.h"
 #define NEPTUNE_HAS_BREAK_DATA 1
#else
 #define NEPTUNE_HAS_BREAK_DATA 0
#endif

// -----------------------------------------------------------------------------
// The samples a sampler track can pick from the controller (SMPL on the SAMPLE
// page): a small factory set rendered from the drum synthesis voices at start-up
// plus one sustaining tone (TONE, what a new sampler track plays), followed by any WAV / AIFF files
// found in ~/Music/Neptune/Samples, followed by the drum breaks (B100 ...: two rendered from the
// drum voices, the others openly licensed recordings from assets/breaks, credited in
// docs/sample-credits.md), followed by what is recorded while the app runs.
//
// The order is what keeps stored sounds right: the first ten places and the places of the user's
// files are where they were before the breaks existed, so an index stored by an older project or
// preset still means the same sample. The breaks' own places depend on how many files the user
// has, so nothing refers to a break by number: projects and presets store the sample's name
// (ProjectBridge, Preset::sampleName), and the factory presets ask indexOfBreak().
//
// Entries are immutable and live for the whole run, so engines can simply point
// at them.
// -----------------------------------------------------------------------------
class SampleBank
{
public:
    struct Entry
    {
        std::string name;            // up to 4 characters, shown in the cell
        std::vector<float> left;
        std::vector<float> right;    // empty for mono
        double sampleRate = 44100.0;
        int defaultSlices = 0;       // grid slices suggested for this sample (0 = none)
        bool sustains = false;       // a seamless loop meant to be held: the sampler suggests MODE = LOOP
        float bars = 0.0f;           // a drum loop of this many bars of four beats (0 = not one): the
                                     // sampler sets LEN (STRCH page) to it, so SYNC plays it in time

        // The tempo the loop was played at, from its length (0 for anything but a loop).
        double tempo() const
        {
            return bars > 0.0f && ! left.empty() ? (double) bars * 240.0 * sampleRate / (double) left.size() : 0.0;
        }
    };

    // Files of the user's folder fill the list up to FILES_END at most (as before the breaks); the
    // rest is for the breaks and for recordings.
    static constexpr int FILES_END = 64;
    static constexpr int MAX_ENTRIES = 96;

    static SampleBank& get()
    {
        static SampleBank bank;
        return bank;
    }

    int size() const { return (int) entries.size(); }
    const Entry* entry(int index) const
    {
        return (index >= 0 && index < size()) ? entries[(size_t) index].get() : nullptr;
    }

    // Index of the first sample with this name, or -1.
    int indexOf(const juce::String& name) const
    {
        for (int i = 0; i < size(); ++i)
            if (name == juce::String(entries[(size_t) i]->name))
                return i;
        return -1;
    }

    // The drum breaks, in their fixed order (new ones are added at the end).
    int breakCount() const { return breakEnd - breakStart; }
    int breakIndex(int n) const { return n >= 0 && n < breakCount() ? breakStart + n : -1; }

    // Index of the break with this name, or -1: only the breaks are searched, so a file of the
    // user's with the same name cannot be taken for it.
    int indexOfBreak(const juce::String& name) const
    {
        for (int i = breakStart; i < breakEnd; ++i)
            if (name == juce::String(entries[(size_t) i]->name))
                return i;
        return -1;
    }

    // The sample a new sampler track starts with: the sustaining tone, so that a held key sounds
    // for as long as it is held and the envelope (REL above all) is heard.
    int defaultIndex() const { return juce::jmax(0, defaultEntry); }

    // Stable array of C strings for a Choice parameter.
    const char* const* names() const { return namePtrs.data(); }

    static juce::File userFolder()
    {
        return juce::File::getSpecialLocation(juce::File::userMusicDirectory)
            .getChildFile("Neptune").getChildFile("Samples");
    }

    // A recording made in the app becomes a new sample: appended to the bank (message thread; the
    // storage is reserved up front, so entries and names already handed out stay valid) and written
    // to the user folder as R001.wav, R002.wav ... so it is there after a restart. Returns the new
    // index, or -1 when the bank is full or the recording is empty. `writeFile` is off in tests.
    int addRecording(const float* left, const float* right, int numSamples, double sampleRate, bool writeFile = true)
    {
        if (left == nullptr || numSamples <= 0 || size() >= MAX_ENTRIES)
            return -1;

        int number = 1;
        auto taken = [this](const std::string& n)
        {
            for (auto& e : entries) if (e->name == n) return true;
            return false;
        };
        auto nameFor = [](int n) { return ("R" + juce::String(n).paddedLeft('0', 3)).toStdString(); };
        while (taken(nameFor(number)) || (writeFile && userFolder().getChildFile(juce::String(nameFor(number)) + ".wav").existsAsFile()))
            ++number;

        auto e = std::make_unique<Entry>();
        e->name = nameFor(number);
        e->left.assign(left, left + numSamples);
        if (right != nullptr)
            e->right.assign(right, right + numSamples);
        e->sampleRate = sampleRate;

        if (writeFile)
        {
            auto file = userFolder().getChildFile(juce::String(e->name) + ".wav");
            userFolder().createDirectory();
            if (auto stream = file.createOutputStream())
            {
                juce::WavAudioFormat wav;
                const unsigned int channels = right != nullptr ? 2u : 1u;
                if (auto* writer = wav.createWriterFor(stream.get(), sampleRate, channels, 24, {}, 0))
                {
                    stream.release();       // the writer owns it now
                    std::unique_ptr<juce::AudioFormatWriter> owned(writer);
                    const float* chans[2] = { left, right };
                    owned->writeFromFloatArrays(chans, (int) channels, numSamples);
                }
            }
        }

        entries.push_back(std::move(e));
        namePtrs.push_back(entries.back()->name.c_str());
        return size() - 1;
    }

private:
    SampleBank()
    {
        entries.reserve(MAX_ENTRIES);
        namePtrs.reserve(MAX_ENTRIES);
        buildFactory();
        loadUserFiles();
        buildBreaks();
        for (auto& e : entries)
            namePtrs.push_back(e->name.c_str());
    }

    static constexpr double SR = 44100.0;

    static std::vector<float> renderHit(DrumSynthType type, float tune, float decay, float tone,
                                        float drive, float snap, float seconds, float velocity = 1.0f)
    {
        DrumVoice v;
        v.setSampleRate(SR);
        v.setType(type);
        v.setTune(tune);
        v.setDecay(decay);
        v.setTone(tone);
        v.setDrive(drive);
        v.setSnap(snap);
        v.setLevel(0.9f);
        v.noteOn(velocity);

        std::vector<float> out((size_t) (seconds * SR), 0.0f);
        for (auto& s : out)
            s = v.process();

        // Short fade so a hit that is still ringing does not click at the end.
        const int fade = juce::jmin((int) out.size(), 400);
        for (int i = 0; i < fade; ++i)
            out[out.size() - 1 - (size_t) i] *= (float) i / (float) fade;
        return out;
    }

    // Two seconds of a warm, slowly moving tone on middle C (the sampler's default ROOT) that joins
    // up exactly where it repeats: three sawtooth-like voices of twelve harmonics, a few cents
    // apart. Every partial makes a whole number of cycles in the buffer (523 cycles of the middle
    // voice in 2 s = 261.5 Hz), so the last frame runs into the first without a step, and the
    // beating between the voices repeats with the loop.
    static std::vector<float> renderTone()
    {
        const int frames = (int) (2.0 * SR);
        const int harmonics = 12;
        static const int voiceCycles[3] = { 523, 522, 524 };
        static const double voiceGain[3] = { 1.0, 0.35, 0.35 };
        const double twoPi = 6.283185307179586;

        std::vector<double> sum((size_t) frames, 0.0);
        uint32_t lcg = 0x1234ABCDu;
        for (int voice = 0; voice < 3; ++voice)
            for (int h = 1; h <= harmonics; ++h)
            {
                // The outer voices have fixed scattered phases, so the beating never lines up.
                lcg = lcg * 1664525u + 1013904223u;
                const double phase = voice == 0 ? 0.0 : twoPi * (double) (lcg >> 8) / 16777216.0;
                const double gain = voiceGain[voice] / std::pow((double) h, 1.3);
                const double inc = twoPi * (double) (voiceCycles[voice] * h) / (double) frames;
                for (int i = 0; i < frames; ++i)
                    sum[(size_t) i] += gain * std::sin(phase + inc * (double) i);
            }

        double peak = 1.0e-9;
        for (double v : sum)
            peak = std::max(peak, std::abs(v));
        std::vector<float> out((size_t) frames);
        for (int i = 0; i < frames; ++i)
            out[(size_t) i] = (float) (0.5 * sum[(size_t) i] / peak);
        return out;
    }

    void add(const char* name, std::vector<float> data, int slices = 0, bool sustains = false)
    {
        auto e = std::make_unique<Entry>();
        e->name = name;
        e->left = std::move(data);
        e->sampleRate = SR;
        e->defaultSlices = slices;
        e->sustains = sustains;
        entries.push_back(std::move(e));
    }

    void buildFactory()
    {
        auto kick  = renderHit(DrumSynthType::FMKick, 48.0f, 0.8f, 0.5f, 0.3f, 0.7f, 0.9f);
        auto snare = renderHit(DrumSynthType::NoiseSnare, 200.0f, 0.2f, 0.5f, 0.3f, 0.6f, 0.45f);
        auto hat   = renderHit(DrumSynthType::ClosedHat, 8000.0f, 0.04f, 0.5f, 0.0f, 0.5f, 0.15f);
        auto ohat  = renderHit(DrumSynthType::OpenHat, 8000.0f, 0.5f, 0.5f, 0.0f, 0.5f, 0.7f);
        auto clap  = renderHit(DrumSynthType::Clap, 1200.0f, 0.3f, 0.6f, 0.7f, 0.5f, 0.5f);
        auto tom   = renderHit(DrumSynthType::Tom, 100.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.5f);
        auto cymb  = renderHit(DrumSynthType::Cymbal, 7000.0f, 1.0f, 0.6f, 0.0f, 0.5f, 1.4f);
        auto rim   = renderHit(DrumSynthType::Rim, 400.0f, 0.1f, 0.5f, 0.0f, 0.5f, 0.2f);

        // One bar at 120 BPM (16 sixteenths of 125 ms): a loop to slice.
        const int stepLen = (int) (0.125 * SR);
        std::vector<float> beat((size_t) (stepLen * 16), 0.0f);
        auto place = [&](const std::vector<float>& hit, int step, float gain)
        {
            size_t at = (size_t) (step * stepLen);
            for (size_t i = 0; i < hit.size() && at + i < beat.size(); ++i)
                beat[at + i] += hit[i] * gain;
        };
        for (int s : { 0, 6, 8, 11 }) place(kick, s, 0.9f);
        for (int s : { 4, 12 })       place(snare, s, 0.8f);
        for (int s = 0; s < 16; s += 2) place(hat, s, (s % 4 == 2) ? 0.5f : 0.3f);
        place(ohat, 14, 0.4f);
        place(clap, 12, 0.4f);
        for (auto& s : beat)
            s = juce::jlimit(-1.0f, 1.0f, s);

        add("BEAT", std::move(beat), 16);
        add("KICK", std::move(kick));
        add("SNAR", std::move(snare));
        add("HAT",  std::move(hat));
        add("OHAT", std::move(ohat));
        add("CLAP", std::move(clap));
        add("TOM",  std::move(tom));
        add("CYMB", std::move(cymb));
        add("RIM",  std::move(rim));

        // After the hits, so their places in the list stay as they were.
        defaultEntry = (int) entries.size();
        add("TONE", renderTone(), 0, true);
    }

    void loadUserFiles()
    {
        auto folder = userFolder();
        if (!folder.isDirectory())
            return;

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        auto files = folder.findChildFiles(juce::File::findFiles, false, "*.wav;*.aif;*.aiff");
        files.sort();
        for (auto& f : files)
        {
            if (size() >= FILES_END)
                break;
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(f));
            auto e = readEntry(reader.get());
            if (e == nullptr)
                continue;
            e->name = f.getFileNameWithoutExtension().toUpperCase().retainCharacters(
                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789").substring(0, 4).toStdString();
            if (e->name.empty())
                e->name = "USER";
            entries.push_back(std::move(e));
        }
    }

    // The audio of a file (60 seconds of it at most, two channels at most); the name is left empty.
    static std::unique_ptr<Entry> readEntry(juce::AudioFormatReader* reader)
    {
        if (reader == nullptr || reader->lengthInSamples <= 0)
            return nullptr;

        const int frames = (int) juce::jmin<juce::int64>(reader->lengthInSamples, (juce::int64) (reader->sampleRate * 60.0));
        const int channels = juce::jmin(2, (int) reader->numChannels);
        juce::AudioBuffer<float> buffer(channels, frames);
        reader->read(&buffer, 0, frames, 0, true, channels > 1);

        auto e = std::make_unique<Entry>();
        e->left.assign(buffer.getReadPointer(0), buffer.getReadPointer(0) + frames);
        if (channels > 1)
            e->right.assign(buffer.getReadPointer(1), buffer.getReadPointer(1) + frames);
        e->sampleRate = reader->sampleRate;
        return e;
    }

    // ---------------------------------------------------------------------------------------------
    // Drum breaks. A name gives the tempo the break was played at; `bars` is its length.
    //   B100, B170      rendered here from the drum voices (own code, nothing to license)
    //   B080 ... B160   recordings of drummers, released by them under CC0 (docs/sample-credits.md)
    // The order is fixed: a new rendered break or file goes at the END of the whole list.
    // ---------------------------------------------------------------------------------------------
    struct BreakHit { int step; int drum; float velocity; };     // drum: 0 kick, 1 snare, 2 hat, 3 open hat, 4 rim

    // `steps` sixteenths at `bpm`, the odd ones late by `swing` of a sixteenth; what rings past the
    // end comes back in at the start, so the loop joins up.
    static std::vector<float> renderBreak(double bpm, int steps, float swing, const BreakHit* hits, int numHits)
    {
        const double stepLen = 15.0 / bpm * SR;
        const size_t frames = (size_t) std::lround(stepLen * (double) steps);
        std::vector<float> out(frames, 0.0f);
        for (int h = 0; h < numHits; ++h)
        {
            const auto& hit = hits[h];
            std::vector<float> sound;
            switch (hit.drum)
            {
                case 0:  sound = renderHit(DrumSynthType::AnalogKick, 52.0f, 0.35f, 0.45f, 0.35f, 0.6f, 0.5f, hit.velocity); break;
                case 1:  sound = renderHit(DrumSynthType::NoiseSnare, 210.0f, 0.16f, 0.6f, 0.35f, 0.7f, 0.35f, hit.velocity); break;
                case 2:  sound = renderHit(DrumSynthType::ClosedHat, 8500.0f, 0.035f, 0.5f, 0.0f, 0.5f, 0.12f, hit.velocity); break;
                case 3:  sound = renderHit(DrumSynthType::OpenHat, 8000.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.45f, hit.velocity); break;
                default: sound = renderHit(DrumSynthType::Rim, 420.0f, 0.08f, 0.5f, 0.0f, 0.5f, 0.15f, hit.velocity); break;
            }
            const double at = ((double) hit.step + ((hit.step & 1) != 0 ? (double) swing : 0.0)) * stepLen;
            const size_t start = (size_t) std::lround(at) % frames;
            for (size_t i = 0; i < sound.size(); ++i)
                out[(start + i) % frames] += sound[i];
        }

        float peak = 1.0e-6f;
        for (float v : out)
            peak = std::max(peak, std::abs(v));
        for (auto& v : out)
            v *= 0.89f / peak;          // -1 dB, as the recorded breaks
        return out;
    }

    void addBreak(const char* name, float bars, std::unique_ptr<Entry> e)
    {
        if (e == nullptr || e->left.empty() || size() >= MAX_ENTRIES)
            return;
        e->name = name;
        e->bars = bars;
        entries.push_back(std::move(e));
        breakEnd = size();
    }

    void buildBreaks()
    {
        breakStart = breakEnd = size();

        auto rendered = [](std::vector<float> data)
        {
            auto e = std::make_unique<Entry>();
            e->left = std::move(data);
            e->sampleRate = SR;
            return e;
        };

        // Two bars at 100: a syncopated kick under the backbeat, quiet snare strokes between, swung.
        static const BreakHit funk[] = {
            { 0, 0, 1.0f }, { 7, 0, 0.8f }, { 10, 0, 0.9f }, { 16, 0, 1.0f }, { 19, 0, 0.7f }, { 26, 0, 0.9f },
            { 4, 1, 1.0f }, { 12, 1, 0.95f }, { 20, 1, 1.0f }, { 28, 1, 0.95f },
            { 9, 1, 0.22f }, { 15, 1, 0.3f }, { 22, 1, 0.2f }, { 25, 1, 0.25f }, { 31, 1, 0.35f },
            { 0, 2, 0.5f }, { 2, 2, 0.3f }, { 4, 2, 0.5f }, { 6, 2, 0.3f }, { 8, 2, 0.5f }, { 10, 2, 0.3f },
            { 12, 2, 0.5f }, { 16, 2, 0.5f }, { 18, 2, 0.3f }, { 20, 2, 0.5f }, { 22, 2, 0.3f }, { 24, 2, 0.5f },
            { 26, 2, 0.3f }, { 28, 2, 0.5f }, { 3, 2, 0.15f }, { 11, 2, 0.15f }, { 21, 2, 0.15f },
            { 14, 3, 0.4f }, { 30, 3, 0.45f } };
        addBreak("B100", 2.0f, rendered(renderBreak(100.0, 32, 0.16f, funk, (int) std::size(funk))));

        // Two bars at 170: a fast broken beat, the second bar turned round, rim and snare strokes between.
        static const BreakHit fast[] = {
            { 0, 0, 1.0f }, { 10, 0, 0.9f }, { 16, 0, 1.0f }, { 22, 0, 0.8f }, { 27, 0, 0.9f },
            { 4, 1, 1.0f }, { 12, 1, 1.0f }, { 20, 1, 1.0f }, { 30, 1, 1.0f },
            { 7, 1, 0.25f }, { 9, 1, 0.3f }, { 15, 1, 0.3f }, { 25, 1, 0.25f }, { 31, 1, 0.4f },
            { 14, 4, 0.5f }, { 18, 4, 0.4f }, { 24, 4, 0.5f },
            { 0, 2, 0.45f }, { 2, 2, 0.35f }, { 4, 2, 0.45f }, { 6, 2, 0.35f }, { 8, 2, 0.45f }, { 10, 2, 0.35f },
            { 12, 2, 0.45f }, { 14, 2, 0.35f }, { 16, 2, 0.45f }, { 18, 2, 0.35f }, { 20, 2, 0.45f }, { 22, 2, 0.35f },
            { 24, 2, 0.45f }, { 26, 2, 0.35f }, { 6, 3, 0.35f }, { 28, 3, 0.4f } };
        addBreak("B170", 2.0f, rendered(renderBreak(170.0, 32, 0.06f, fast, (int) std::size(fast))));

       #if NEPTUNE_HAS_BREAK_DATA
        struct BreakFile { const char* name; const char* resource; float bars; };
        static const BreakFile files[] = {
            { "B080", "b080_flac", 2.0f }, { "B090", "b090_flac", 2.0f }, { "B105", "b105_flac", 4.0f },
            { "B120", "b120_flac", 2.0f }, { "B157", "b157_flac", 1.0f }, { "B160", "b160_flac", 4.0f } };
        juce::FlacAudioFormat flac;
        for (auto& f : files)
        {
            int bytes = 0;
            const char* data = NeptuneBreaks::getNamedResource(f.resource, bytes);
            if (data == nullptr || bytes <= 0)
                continue;
            std::unique_ptr<juce::AudioFormatReader> reader(
                flac.createReaderFor(new juce::MemoryInputStream(data, (size_t) bytes, false), true));
            addBreak(f.name, f.bars, readEntry(reader.get()));
        }
       #endif
    }

    std::vector<std::unique_ptr<Entry>> entries;
    std::vector<const char*> namePtrs;
    int defaultEntry = 0;
    int breakStart = 0, breakEnd = 0;
};
