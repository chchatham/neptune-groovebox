#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <array>
#include <vector>

// Chord pads: a chord held on the keys is stored on a pad and played back from it (the CHORD
// screen). Four banks of sixteen pads, each pad the exact notes that were held, with the level
// each was played at. Stored with the project (ProjectBridge), as the flat list below.
class ChordPads
{
public:
    static constexpr int NUM_BANKS = 4;
    static constexpr int PADS = 16;
    static constexpr int MAX_NOTES = 10;
    static constexpr float MIN_VELOCITY = 1.0f / 127.0f;       // the softest key there is

    struct Note
    {
        int pitch = 60;
        float velocity = 1.0f;      // 0..1, as played
    };

    struct Chord
    {
        std::array<Note, MAX_NOTES> notes {};
        int count = 0;
        bool empty() const { return count == 0; }
    };

    // ---- the pads --------------------------------------------------------------------------------
    int getBank() const { return bank; }
    void setBank(int b) { bank = juce::jlimit(0, NUM_BANKS - 1, b); }

    const Chord& get(int pad) const { return get(bank, pad); }
    const Chord& get(int bankIndex, int pad) const
    {
        static const Chord none;
        if (bankIndex < 0 || bankIndex >= NUM_BANKS || pad < 0 || pad >= PADS)
            return none;
        return chords[(size_t) bankIndex][(size_t) pad];
    }

    // The held notes become the pad's chord: lowest first, one entry per pitch, at most MAX_NOTES
    // (the lowest ones are kept). An empty list clears the pad.
    void store(int pad, std::vector<Note> held) { store(bank, pad, std::move(held)); }
    void store(int bankIndex, int pad, std::vector<Note> held)
    {
        if (bankIndex < 0 || bankIndex >= NUM_BANKS || pad < 0 || pad >= PADS)
            return;
        std::sort(held.begin(), held.end(), [](const Note& a, const Note& b) { return a.pitch < b.pitch; });
        Chord c;
        for (auto& n : held)
        {
            if (n.pitch < 0 || n.pitch > 127) continue;
            if (c.count > 0 && c.notes[(size_t) c.count - 1].pitch == n.pitch) continue;
            if (c.count >= MAX_NOTES) break;
            c.notes[(size_t) c.count++] = { n.pitch, juce::jlimit(MIN_VELOCITY, 1.0f, n.velocity) };
        }
        chords[(size_t) bankIndex][(size_t) pad] = c;
    }

    void clearBank(int bankIndex)
    {
        if (bankIndex >= 0 && bankIndex < NUM_BANKS)
            chords[(size_t) bankIndex].fill(Chord());
    }

    // ---- how they play ---------------------------------------------------------------------------
    int transpose = 0;              // semitones, -24..24, every pad
    int octave = 0;                 // -3..3
    // VL, how hard each note of a pad is played:
    //   VEL_PAD      every note at the pad's velocity
    //   VEL_BALANCE  the chord's loudest note at the pad's velocity, the others below it in the
    //                proportion the keys were played in
    //   VEL_KEYS     every note exactly as its key was played; how hard the pad is hit changes nothing
    enum VelocityMode { VEL_PAD = 0, VEL_BALANCE, VEL_KEYS, NUM_VELOCITY_MODES };
    int velocityMode = VEL_BALANCE;

    // The notes a pad sounds now (transposed; a note pushed out of the MIDI range folds back by octaves).
    std::vector<Note> sounding(int pad, float padVelocity) const
    {
        std::vector<Note> out;
        const auto& c = get(pad);
        float loudest = MIN_VELOCITY;
        for (int i = 0; i < c.count; ++i)
            loudest = std::max(loudest, c.notes[(size_t) i].velocity);
        for (int i = 0; i < c.count; ++i)
        {
            int pitch = c.notes[(size_t) i].pitch + transpose + octave * 12;
            while (pitch > 127) pitch -= 12;
            while (pitch < 0) pitch += 12;
            const float stored = c.notes[(size_t) i].velocity;
            const float v = velocityMode == VEL_KEYS ? stored
                          : velocityMode == VEL_BALANCE ? padVelocity * stored / loudest : padVelocity;
            out.push_back({ pitch, juce::jlimit(MIN_VELOCITY, 1.0f, v) });
        }
        return out;
    }

    // ---- strum: one setting for every pad -----------------------------------------------------------
    static constexpr float MAX_STRUM_MS = 1000.0f;
    enum StrumDir { STRUM_UP = 0, STRUM_DOWN, STRUM_ALT, NUM_STRUM_DIRS };
    float strumMs = 0.0f;           // the time from the chord's first note to its last; 0 = all together
    int strumDir = STRUM_UP;        // lowest note first, highest first, or one then the other
    float strumHuman = 0.0f;        // 0..1: the timing is uneven - the whole stroke faster or slower, the gaps unequal
    float strumVary = 0.0f;         // 0..1: the pattern changes - a stroke the other way, neighbouring notes changing places
    float strumFall = 0.0f;         // 0..1: later notes of a stroke are softer

    struct Stroke
    {
        int pitch = 60;
        float velocity = 1.0f;
        float delayMs = 0.0f;       // after the pad was pressed
    };

    // The notes a pad sounds now and when each one starts, in the order they start. `altDown` is
    // the caller's memory of the last stroke's direction (STRUM_ALT). No note is ever left out, and
    // without a strum time every note starts at once.
    std::vector<Stroke> strum(int pad, float padVelocity, juce::Random& random, bool& altDown) const
    {
        const auto notes = sounding(pad, padVelocity);
        const int n = (int) notes.size();
        std::vector<Stroke> out;
        if (n == 0)
            return out;
        if (strumMs <= 0.0f || n < 2)
        {
            for (auto& note : notes)
                out.push_back({ note.pitch, note.velocity, 0.0f });
            return out;
        }

        // The pattern: which note comes when.
        bool down = strumDir == STRUM_DOWN || (strumDir == STRUM_ALT && altDown);
        if (strumDir == STRUM_ALT)
            altDown = !altDown;
        const float vary = juce::jlimit(0.0f, 1.0f, strumVary);
        if (random.nextFloat() < vary * 0.5f)
            down = !down;
        std::vector<int> order((size_t) n);
        for (int i = 0; i < n; ++i)
            order[(size_t) i] = down ? n - 1 - i : i;
        for (int i = 0; i + 1 < n; ++i)
            if (random.nextFloat() < vary * 0.35f)
            {
                std::swap(order[(size_t) i], order[(size_t) i + 1]);
                ++i;                // (a note moves one place at most)
            }

        // The timing: the stroke's length, and how it is shared among the gaps between the notes.
        const float human = juce::jlimit(0.0f, 1.0f, strumHuman);
        auto either = [&random] { return random.nextFloat() * 2.0f - 1.0f; };
        const float total = strumMs * (1.0f + human * 0.3f * either());
        std::vector<float> gap((size_t) n - 1);
        float sum = 0.0f;
        for (auto& g : gap)
        {
            g = juce::jmax(0.1f, 1.0f + human * 0.9f * either());
            sum += g;
        }
        const float fall = juce::jlimit(0.0f, 1.0f, strumFall);
        float at = 0.0f;
        for (int k = 0; k < n; ++k)
        {
            const auto& note = notes[(size_t) order[(size_t) k]];
            const float level = 1.0f - fall * 0.6f * (float) k / (float) (n - 1);
            out.push_back({ note.pitch, juce::jlimit(MIN_VELOCITY, 1.0f, note.velocity * level), at });
            if (k < n - 1)
                at += total * gap[(size_t) k] / sum;
        }
        return out;
    }

    // ---- naming ----------------------------------------------------------------------------------
    struct Name
    {
        juce::String root;          // "C#"; empty for an empty pad
        juce::String quality;       // "m7", "" for major; for the display's second line
        juce::String bass;          // set when the lowest note is not the root: "E"
        juce::String full() const
        {
            if (root.isEmpty()) return "--";
            return root + quality + (bass.isNotEmpty() ? "/" + bass : juce::String());
        }
        // Two characters for a pad cell's second line ("M" major, so the cell is never blank).
        juce::String cell() const { return quality.isEmpty() ? juce::String("M") : quality.substring(0, 2); }
    };

    static juce::String pitchClassName(int pc)
    {
        static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return names[((pc % 12) + 12) % 12];
    }

    // The chord's name from its pitch classes: the lowest note is tried as the root first, then
    // every other note (an inversion, named with its bass). A set no chord here fits is named by
    // its lowest note with "*".
    static Name nameOf(const Chord& c) { return nameOf(c.notes.data(), c.count); }
    static Name nameOf(const Note* notes, int count)
    {
        Name name;
        if (count <= 0 || notes == nullptr)
            return name;
        int lowest = 127;
        unsigned present = 0;
        for (int i = 0; i < count; ++i)
        {
            lowest = std::min(lowest, notes[i].pitch);
            present |= 1u << (unsigned) (((notes[i].pitch % 12) + 12) % 12);
        }
        const int bassPc = lowest % 12;

        struct Shape { unsigned mask; const char* quality; };
        auto bits = [](std::initializer_list<int> steps) { unsigned m = 0; for (int s : steps) m |= 1u << (unsigned) s; return m; };
        static const Shape shapes[] = {
            { bits({ 0, 4, 7 }), "" },          { bits({ 0, 3, 7 }), "m" },
            { bits({ 0, 4, 7, 10 }), "7" },     { bits({ 0, 4, 7, 11 }), "M7" },
            { bits({ 0, 3, 7, 10 }), "m7" },    { bits({ 0, 3, 7, 11 }), "mM" },
            { bits({ 0, 3, 6 }), "o" },         { bits({ 0, 3, 6, 9 }), "o7" },
            { bits({ 0, 3, 6, 10 }), "h7" },    { bits({ 0, 4, 8 }), "+" },
            { bits({ 0, 5, 7 }), "s4" },        { bits({ 0, 2, 7 }), "s2" },
            { bits({ 0, 4, 7, 9 }), "6" },      { bits({ 0, 3, 7, 9 }), "m6" },
            { bits({ 0, 2, 4, 7 }), "a9" },     { bits({ 0, 2, 3, 7 }), "ma" },
            { bits({ 0, 2, 4, 7, 10 }), "9" },  { bits({ 0, 2, 4, 7, 11 }), "M9" },
            { bits({ 0, 2, 3, 7, 10 }), "m9" }, { bits({ 0, 4, 10 }), "7" },
            { bits({ 0, 4, 11 }), "M7" },       { bits({ 0, 3, 10 }), "m7" },
            { bits({ 0, 7 }), "5" },            { bits({ 0, 4 }), "" },
            { bits({ 0, 3 }), "m" },            { bits({ 0 }), "1" } };

        for (int attempt = 0; attempt < 12; ++attempt)
        {
            const int rootPc = (bassPc + attempt) % 12;
            if ((present & (1u << (unsigned) rootPc)) == 0)
                continue;
            // The set as seen from this root.
            unsigned relative = 0;
            for (int pc = 0; pc < 12; ++pc)
                if (present & (1u << (unsigned) pc))
                    relative |= 1u << (unsigned) ((pc - rootPc + 12) % 12);
            for (auto& shape : shapes)
                if (shape.mask == relative)
                {
                    name.root = pitchClassName(rootPc);
                    name.quality = shape.quality;
                    if (name.quality == "1") name.quality = count > 1 ? "8" : "";      // one note, or octaves of it
                    if (rootPc != bassPc) name.bass = pitchClassName(bassPc);
                    return name;
                }
        }
        name.root = pitchClassName(bassPc);
        name.quality = "*";
        return name;
    }

    // ---- as numbers: the project file and the undo history (append only) --------------------------
    // [version 1, bank, transpose, octave, velocity mode, then for every bank and pad: count, (pitch, velocity) x count,
    //  then the strum: time in ms, direction, humanize, vary, fall (absent in a list from before the strum)]
    std::vector<float> toFlat() const
    {
        std::vector<float> v { 1.0f, (float) bank, (float) transpose, (float) octave, (float) velocityMode };
        for (auto& b : chords)
            for (auto& c : b)
            {
                v.push_back((float) c.count);
                for (int i = 0; i < c.count; ++i)
                {
                    v.push_back((float) c.notes[(size_t) i].pitch);
                    v.push_back(c.notes[(size_t) i].velocity);
                }
            }
        v.insert(v.end(), { strumMs, (float) strumDir, strumHuman, strumVary, strumFall });
        return v;
    }

    // An empty or unreadable list leaves every pad empty.
    void fromFlat(const std::vector<float>& v)
    {
        *this = ChordPads();
        if (v.size() < 5 || (int) std::lround(v[0]) != 1)
            return;
        setBank((int) std::lround(v[1]));
        transpose = juce::jlimit(-24, 24, (int) std::lround(v[2]));
        octave = juce::jlimit(-3, 3, (int) std::lround(v[3]));
        velocityMode = std::isfinite(v[4]) ? juce::jlimit(0, NUM_VELOCITY_MODES - 1, (int) std::lround(v[4])) : VEL_BALANCE;
        size_t at = 5;
        for (auto& b : chords)
            for (auto& c : b)
            {
                if (at >= v.size()) return;
                const int count = juce::jlimit(0, MAX_NOTES, (int) std::lround(v[at++]));
                for (int i = 0; i < count && at + 1 < v.size() + 0; ++i)
                {
                    c.notes[(size_t) i] = { juce::jlimit(0, 127, (int) std::lround(v[at])), juce::jlimit(MIN_VELOCITY, 1.0f, v[at + 1]) };
                    c.count = i + 1;
                    at += 2;
                }
            }
        if (at + 5 <= v.size())
        {
            auto unit = [](float x) { return std::isfinite(x) ? juce::jlimit(0.0f, 1.0f, x) : 0.0f; };
            strumMs = std::isfinite(v[at]) ? juce::jlimit(0.0f, MAX_STRUM_MS, v[at]) : 0.0f;
            strumDir = std::isfinite(v[at + 1]) ? juce::jlimit(0, NUM_STRUM_DIRS - 1, (int) std::lround(v[at + 1])) : STRUM_UP;
            strumHuman = unit(v[at + 2]);
            strumVary = unit(v[at + 3]);
            strumFall = unit(v[at + 4]);
        }
    }

    bool anyStored() const
    {
        for (auto& b : chords)
            for (auto& c : b)
                if (!c.empty()) return true;
        return false;
    }
    // Something a project has to keep: a chord, or a strum that was set.
    bool hasContent() const { return anyStored() || strumMs > 0.0f; }

private:
    std::array<std::array<Chord, PADS>, NUM_BANKS> chords {};
    int bank = 0;
};
