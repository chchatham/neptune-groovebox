#pragma once

#include "DrumVoice.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// The samples a sampler track can pick from the controller (SMPL on the SAMPLE
// page): a small factory set rendered from the drum synthesis voices at start-up
// (no audio assets are shipped) plus one sustaining tone (TONE, what a new sampler track plays),
// followed by any WAV / AIFF files found in ~/Music/Neptune/Samples.
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
    };

    static constexpr int MAX_ENTRIES = 64;

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
            if (size() >= MAX_ENTRIES)
                break;
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(f));
            if (reader == nullptr || reader->lengthInSamples <= 0)
                continue;

            // Cap at 60 seconds per file.
            const int frames = (int) juce::jmin<juce::int64>(reader->lengthInSamples, (juce::int64) (reader->sampleRate * 60.0));
            const int channels = juce::jmin(2, (int) reader->numChannels);
            juce::AudioBuffer<float> buffer(channels, frames);
            reader->read(&buffer, 0, frames, 0, true, channels > 1);

            auto e = std::make_unique<Entry>();
            e->name = f.getFileNameWithoutExtension().toUpperCase().retainCharacters(
                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789").substring(0, 4).toStdString();
            if (e->name.empty())
                e->name = "USER";
            e->left.assign(buffer.getReadPointer(0), buffer.getReadPointer(0) + frames);
            if (channels > 1)
                e->right.assign(buffer.getReadPointer(1), buffer.getReadPointer(1) + frames);
            e->sampleRate = reader->sampleRate;
            entries.push_back(std::move(e));
        }
    }

    std::vector<std::unique_ptr<Entry>> entries;
    std::vector<const char*> namePtrs;
    int defaultEntry = 0;
};
