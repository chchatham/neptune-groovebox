#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <cmath>
#include <algorithm>

// Metronome: the click to record against.
//
// The sequencer's clock says where the beats are (SequencerCore::getBeatTick, sample-exact, also
// during a count-in); this class decides which of them sound and makes the sound: a short sine
// burst, higher and louder on the first beat of the bar. Nothing is read from a file.
//
//   any thread:    setMode(), setLevel(), audition()
//   audio thread:  beginBlock(), beat() for every beat of the block, then render()
//
// AudioEngine adds the click to the output after everything else - after the master EQ,
// compressor, limiter and master volume, and after the resampler has taken its copy - so it never
// pumps the mix, is as loud at any master volume, and is never recorded into a sample.
// Nothing is allocated or locked.
class Metronome
{
public:
    // Stored in the app's settings file: keep the numbers.
    enum class Mode { Off = 0, Record = 1, Play = 2 };
    static constexpr int NUM_MODES = 3;

    static constexpr float DEFAULT_LEVEL = 0.5f;
    static constexpr float ACCENT_HZ = 1760.0f, BEAT_HZ = 880.0f;
    static constexpr float ACCENT_GAIN = 0.9f, BEAT_GAIN = 0.5f;     // peak at level 1
    static constexpr float CLICK_MS = 40.0f;                         // a click is over after this
    static constexpr float DECAY_MS = 6.0f;                          // its time constant
    static constexpr float CUT_MS = 3.0f;                            // fade when the transport stops
    static constexpr int MAX_BEATS_PER_BLOCK = 8;

    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        decay = (float) std::exp(-1.0 / (DECAY_MS * 0.001 * sampleRate));
        cut = (float) std::exp(-6.9 / (CUT_MS * 0.001 * sampleRate));
        clickSamples = (int) (CLICK_MS * 0.001 * sampleRate);
        samplesLeft = 0;
        numPending = 0;
    }

    void setMode(Mode m) { mode.store(m); }
    Mode getMode() const { return mode.load(); }
    void setLevel(float l) { level.store(juce::jlimit(0.0f, 1.0f, l)); }
    float getLevel() const { return level.load(); }

    // One click now, whatever the mode and the transport (the level knob lets itself be heard).
    void audition() { auditionRequested.store(true, std::memory_order_relaxed); }

    // Would a beat sound now? (a count-in beat always does)
    bool wants(bool countIn, bool recording) const
    {
        const Mode m = mode.load(std::memory_order_relaxed);
        return countIn || m == Mode::Play || (m == Mode::Record && recording);
    }

    // ---- Audio thread ----
    // `running`: the transport is playing or counting in. When it is not, a click still sounding
    // is faded out in a few milliseconds.
    void beginBlock(bool running)
    {
        numPending = 0;
        transportRunning = running;
        if (auditionRequested.exchange(false, std::memory_order_relaxed))
        {
            pending[0] = { 0, false, true };
            numPending = 1;
        }
    }

    // A beat at `sample` of this block (in rising order).
    void beat(int sample, bool accent, bool countIn, bool recording)
    {
        if (wants(countIn, recording) && numPending < MAX_BEATS_PER_BLOCK)
            pending[(size_t) numPending++] = { sample, accent, false };
    }

    bool isSounding() const { return samplesLeft > 0 || numPending > 0; }

    // Adds the block's clicks to the two buffers.
    void render(float* l, float* r, int n)
    {
        if (samplesLeft <= 0 && numPending == 0)
            return;
        const float lvl = level.load(std::memory_order_relaxed);
        int next = 0;
        for (int i = 0; i < n; ++i)
        {
            while (next < numPending && pending[(size_t) next].sample <= i)
            {
                const auto& p = pending[(size_t) next++];
                phase = 0.0;
                phaseStep = juce::MathConstants<double>::twoPi * (p.accent ? ACCENT_HZ : BEAT_HZ) / sampleRate;
                env = (p.accent ? ACCENT_GAIN : BEAT_GAIN);
                samplesLeft = clickSamples;
                isAudition = p.audition;
            }
            if (samplesLeft <= 0)
            {
                if (next >= numPending) break;
                continue;
            }
            const float out = (float) std::sin(phase) * env * lvl;
            l[i] += out;
            r[i] += out;
            phase += phaseStep;
            env *= decay;
            --samplesLeft;
            if (!transportRunning && !isAudition)
            {
                env *= cut;
                if (env < 1.0e-5f)
                    samplesLeft = 0;
            }
        }
        numPending = 0;
    }

    // ---- The app-wide settings file (one line: mode, level in percent, count-in bars) ----
    struct Settings
    {
        int mode = (int) Mode::Record;
        float level = DEFAULT_LEVEL;
        int countInBars = 0;
    };

    static juce::String toText(const Settings& s)
    {
        return juce::String(s.mode) + " " + juce::String(juce::roundToInt(s.level * 100.0f)) + " " + juce::String(s.countInBars);
    }

    // Anything missing or unreadable keeps its default.
    static Settings fromText(const juce::String& text, int maxCountInBars)
    {
        Settings s;
        juce::StringArray parts;
        parts.addTokens(text.trim(), " \t\r\n", "");
        parts.removeEmptyStrings();
        auto isNumber = [](const juce::String& p) { return p.isNotEmpty() && p.containsOnly("0123456789"); };
        if (parts.size() > 0 && isNumber(parts[0])) s.mode = juce::jlimit(0, NUM_MODES - 1, parts[0].getIntValue());
        if (parts.size() > 1 && isNumber(parts[1])) s.level = juce::jlimit(0.0f, 1.0f, (float) parts[1].getIntValue() / 100.0f);
        if (parts.size() > 2 && isNumber(parts[2])) s.countInBars = juce::jlimit(0, maxCountInBars, parts[2].getIntValue());
        return s;
    }

private:
    struct Pending { int sample = 0; bool accent = false; bool audition = false; };

    std::atomic<Mode> mode { Mode::Record };
    std::atomic<float> level { DEFAULT_LEVEL };
    std::atomic<bool> auditionRequested { false };

    double sampleRate = 44100.0;
    float decay = 0.996f, cut = 0.95f;
    int clickSamples = 1764;

    // Audio thread only.
    std::array<Pending, MAX_BEATS_PER_BLOCK> pending {};
    int numPending = 0;
    bool transportRunning = false, isAudition = false;
    double phase = 0.0, phaseStep = 0.0;
    float env = 0.0f;
    int samplesLeft = 0;
};
