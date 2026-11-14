#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
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
            c.notes[(size_t) c.count++] = { n.pitch, juce::jlimit(0.05f, 1.0f, n.velocity) };
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
    bool playedVelocity = true;     // each note at the level it was stored with (scaled by the pad's);
                                    // off: every note at the pad's velocity

    // The notes a pad sounds now (transposed; a note pushed out of the MIDI range folds back by octaves).
    std::vector<Note> sounding(int pad, float padVelocity) const
    {
        std::vector<Note> out;
        const auto& c = get(pad);
        for (int i = 0; i < c.count; ++i)
        {
            int pitch = c.notes[(size_t) i].pitch + transpose + octave * 12;
            while (pitch > 127) pitch -= 12;
            while (pitch < 0) pitch += 12;
            const float v = playedVelocity ? padVelocity * (0.4f + 0.6f * c.notes[(size_t) i].velocity) : padVelocity;
            out.push_back({ pitch, juce::jlimit(0.02f, 1.0f, v) });
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
    // [version 1, bank, transpose, octave, playedVelocity, then for every bank and pad: count, (pitch, velocity) x count]
    std::vector<float> toFlat() const
    {
        std::vector<float> v { 1.0f, (float) bank, (float) transpose, (float) octave, playedVelocity ? 1.0f : 0.0f };
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
        playedVelocity = v[4] > 0.5f;
        size_t at = 5;
        for (auto& b : chords)
            for (auto& c : b)
            {
                if (at >= v.size()) return;
                const int count = juce::jlimit(0, MAX_NOTES, (int) std::lround(v[at++]));
                for (int i = 0; i < count && at + 1 < v.size() + 0; ++i)
                {
                    c.notes[(size_t) i] = { juce::jlimit(0, 127, (int) std::lround(v[at])), juce::jlimit(0.05f, 1.0f, v[at + 1]) };
                    c.count = i + 1;
                    at += 2;
                }
            }
    }

    bool anyStored() const
    {
        for (auto& b : chords)
            for (auto& c : b)
                if (!c.empty()) return true;
        return false;
    }

private:
    std::array<std::array<Chord, PADS>, NUM_BANKS> chords {};
    int bank = 0;
};
