#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

// Keyboard split: the lower octave of the 25 keys is shifted down by a further 0-3 octaves relative
// to the rest. Shift + Oct - steps the amount (0, 1, 2, 3, 0 ...); Shift + Oct + steps it back.
//
// The octave buttons belong to the firmware and send nothing to the host. In the firmware,
// Shift + Oct -/+ transposes the whole keyboard by a semitone. This class works through the Neptune
// firmware's debug command (peek / poke over SysEx, builds NP5 and later; addresses measured on NPD,
// see docs/launchkey-mk4-firmware-map.md):
//   - while Shift is held (and just after) the firmware's transpose is read; any transpose is taken
//     as the split gesture: it is set back to zero and the split amount stepped instead;
//   - each key's note is the low byte of its mapping word; the firmware rewrites all of them when
//     an octave button is pressed, so while a split is on the octave is read a few times a second
//     and the lower keys are written again after every change.
// The firmware still does all octave handling. Nothing here runs on the audio thread.
//
// A preset or a project stores the keyboard (KeyboardSetup) and sets it again: the split, and the
// octave, which is written into the firmware's own octave word, then the keys as the firmware
// would have written them. For that the octave is read every two seconds while nothing else is
// going on (the octave buttons send nothing).

// The keyboard as a preset or a project stores it. A negative split / an UNKNOWN octave = not
// stored: left as it is.
struct KeyboardSetup
{
    static constexpr int UNKNOWN = -100;
    int split = -1;                 // octaves the lower twelve keys are moved down, 0-3
    int octave = UNKNOWN;           // the octave the Oct buttons had chosen
    bool operator== (const KeyboardSetup& o) const { return split == o.split && octave == o.octave; }
    bool operator!= (const KeyboardSetup& o) const { return ! (*this == o); }
};

class KeyboardSplit
{
public:
    static constexpr int IDLE_POLLS = 10;                          // polls between two idle reads of the octave
    static constexpr juce::uint32 FW_BASE = 0x08008000;
    static constexpr juce::uint32 PEEK32 = FW_BASE + 0xA190;       // ldr r0,[r0]; bx lr
    static constexpr juce::uint32 POKE32 = FW_BASE + 0x0196;       // str r1,[r0]; bx lr
    static constexpr juce::uint32 OCTAVE = 0x2000F440;             // the octave the buttons step (signed)
    static constexpr juce::uint32 OCTAVE_SHOWN = 0x20001038;       // a copy the firmware keeps
    static constexpr juce::uint32 TRANSPOSE = 0x2000F444;          // semitones, set by Shift + Oct (signed)
    static constexpr juce::uint32 TRANSPOSE_SHOWN = 0x2000103C;
    static constexpr juce::uint32 NOTE_TABLE = 0x2000E194;         // 128 bytes: note of key index i (octave and
                                                                   // transpose included); the firmware copies
                                                                   // it into the mapping words when the host connects
    static constexpr juce::uint32 KEY_RECORDS = 0x2000371C;        // per-key record (20 bytes each): its note as a
    static constexpr juce::uint32 KEY_RECORD_SIZE = 20;            // word; reloaded into the mapping words on connect
    static constexpr juce::uint32 STORED_WORD = 0x200015BC;        // byte 1 of this word: a stored copy of the transpose
    static constexpr juce::uint32 KEY_MAP = 0x20008170 + 4 * 0x80; // mapping word of key 0
    static constexpr juce::uint32 KEY_WORD = 0x21000000;           // "note" mapping, note in the low byte
    static constexpr int NUM_KEYS = 25, SPLIT_KEYS = 12, MAX_SPLIT = 3;
    static constexpr juce::int64 SHIFT_GRACE_MS = 600;

    // Sends one SysEx body (no F0 / F7) to the controller's MIDI port.
    std::function<void(const std::vector<juce::uint8>&)> send;
    std::function<bool()> isShiftHeld;
    std::function<void(const juce::String&)> showMessage;
    std::function<juce::int64()> clock = [] { return juce::Time::currentTimeMillis(); };

    int getSplit() const { return split; }
    bool isOctaveKnown() const { return known; }
    int getOctave() const { return octave; }

    void setSplit(int octavesDown)
    {
        octavesDown = juce::jlimit(0, MAX_SPLIT, octavesDown);
        if (octavesDown == split) return;
        split = octavesDown;
        if (known) writeKeys(octave);
    }

    // The octave, as the Oct buttons would have set it. Before the firmware's octave is known the
    // wish is kept and carried out when it is.
    void setOctave(int newOctave)
    {
        newOctave = juce::jlimit(-8, 8, newOctave);
        if (! known) { wantedOctave = newOctave; return; }
        if (newOctave == octave) return;
        octave = newOctave;
        if (send)
        {
            send(call(POKE32, OCTAVE, (juce::uint32) newOctave));
            send(call(POKE32, OCTAVE_SHOWN, (juce::uint32) newOctave));
        }
        writeNoteTable(octave);
        writeKeyRecords(octave);
        writeKeys(octave);
    }

    KeyboardSetup getSetup() const { return { split, known ? octave : KeyboardSetup::UNKNOWN }; }
    void apply(const KeyboardSetup& k)
    {
        if (k.octave != KeyboardSetup::UNKNOWN) setOctave(k.octave);
        if (k.split >= 0) setSplit(k.split);
    }

    // Call a few times a second (message thread). Asks the firmware for its transpose and octave
    // when the answer matters at once: until the octave is first known, while Shift is held or
    // was just released (the gesture), and while a split is on (an octave press undoes it). The
    // rest of the time only the octave is read, every IDLE_POLLS-th call, which leaves the debug
    // channel almost free for the host-side tools.
    void poll()
    {
        const bool shift = isShiftHeld && isShiftHeld();
        if (shift) lastShiftMs = clock();
        if (known && split == 0 && ! shift && clock() - lastShiftMs > SHIFT_GRACE_MS)
        {
            if (++idlePolls >= IDLE_POLLS) { idlePolls = 0; ask(Awaiting::Octave); }
            return;
        }
        idlePolls = 0;
        ask(Awaiting::Transpose);
    }

    // Every SysEx body from the MIDI port goes here; debug replies are 00 20 29 00 7D + five 7-bit bytes.
    void sysExReceived(const juce::uint8* data, int size)
    {
        if (size != 10 || data[0] != 0x00 || data[1] != 0x20 || data[2] != 0x29 || data[3] != 0x00 || data[4] != 0x7D)
            return;
        if (awaiting == Awaiting::Nothing) return;      // the reply to one of our pokes
        juce::uint32 raw = 0;
        for (int i = 5; i < 10; ++i) raw = (raw << 7) | data[i];
        const int value = (int) (juce::int32) raw;
        if (awaiting == Awaiting::Stored)
        {
            // The stored copy of the transpose: cleared only if it holds what was just undone.
            if ((raw >> 16) != 0) return;                       // a poke's reply (an address): not ours
            awaiting = Awaiting::Nothing;
            if (((raw >> 8) & 0xFF) == (juce::uint32) (undone & 0xFF) && undone != 0 && send)
                send(call(POKE32, STORED_WORD, raw & ~0xFF00u));
            undone = 0;
            return;
        }
        // A poke's reply (an address) can arrive in between: anything that is not a small number is not ours.
        if (value < -24 || value > 24) return;

        if (awaiting == Awaiting::Transpose)
        {
            if (value != 0) transposeSeen(value);
            ask(Awaiting::Octave);
        }
        else
        {
            awaiting = Awaiting::Nothing;
            octaveRead(value);
            if (undone != 0) ask(Awaiting::Stored);
        }
    }

    // Put the keys back as the firmware left them (on quit).
    void restore()
    {
        if (known && split != 0) { split = 0; writeKeys(octave); }
    }

    static int noteOfKey(int key, int octave, int split)
    {
        const int note = 48 + 12 * octave + key - (key < SPLIT_KEYS ? 12 * split : 0);
        return juce::jlimit(0, 127, note);
    }

private:
    enum class Awaiting { Nothing, Transpose, Octave, Stored };

    void ask(Awaiting what)
    {
        awaiting = what;
        if (send) send(call(PEEK32, what == Awaiting::Transpose ? TRANSPOSE : (what == Awaiting::Octave ? OCTAVE : STORED_WORD), 0));
    }

    // Shift + Oct - / Oct + reached the firmware as a transpose: undo it, step the split instead.
    void transposeSeen(int semitones)
    {
        // A transpose left over from before the app started is only cleared, not counted as a gesture.
        if (known)
            for (int i = 0; i < std::abs(semitones); ++i)
                split = semitones < 0 ? (split + 1) % (MAX_SPLIT + 1) : juce::jmax(0, split - 1);
        if (send)
        {
            send(call(POKE32, TRANSPOSE, 0));
            send(call(POKE32, TRANSPOSE_SHOWN, 0));
        }
        keysStale = true;                    // keys, records and note table are written when the octave reply arrives
        undone = semitones;
        if (known && showMessage) showMessage(split == 0 ? juce::String("SPLIT OFF") : "SPLIT -" + juce::String(split) + " OCT");
    }

    void octaveRead(int value)
    {
        if (value < -8 || value > 8) return;
        const bool changed = ! known || value != octave;
        known = true;
        octave = value;
        // After the gesture every key is rewritten (the firmware had transposed them all); after a
        // plain octave press the firmware has already written them unsplit, so only a split needs it.
        if (keysStale)
        {
            writeNoteTable(octave);          // the firmware had transposed these too
            writeKeyRecords(octave);
        }
        if (keysStale || (changed && split != 0))
            writeKeys(octave);
        keysStale = false;
        if (wantedOctave != KeyboardSetup::UNKNOWN)
        {
            const int wanted = wantedOctave;
            wantedOctave = KeyboardSetup::UNKNOWN;
            setOctave(wanted);
        }
    }

    void writeKeys(int forOctave)
    {
        if (! send) return;
        for (int k = 0; k < NUM_KEYS; ++k)
            send(call(POKE32, KEY_MAP + 4 * (juce::uint32) k, KEY_WORD | (juce::uint32) noteOfKey(k, forOctave, split)));
    }

    // The per-key records, without transpose or split (what the firmware would hold itself).
    void writeKeyRecords(int forOctave)
    {
        if (! send) return;
        for (int k = 0; k < NUM_KEYS; ++k)
            send(call(POKE32, KEY_RECORDS + KEY_RECORD_SIZE * (juce::uint32) k, (juce::uint32) noteOfKey(k, forOctave, 0)));
    }

    // The firmware's own table, without transpose (it never holds the split).
    void writeNoteTable(int forOctave)
    {
        if (! send) return;
        for (int i = 0; i < 128; i += 4)
        {
            juce::uint32 word = 0;
            for (int b = 0; b < 4; ++b)
                word |= (juce::uint32) ((48 + 12 * forOctave + i + b) & 0xFF) << (8 * b);
            send(call(POKE32, NOTE_TABLE + (juce::uint32) i, word));
        }
    }

    static std::vector<juce::uint8> call(juce::uint32 function, juce::uint32 a0, juce::uint32 a1)
    {
        std::vector<juce::uint8> m { 0x00, 0x20, 0x29, 0x00, 0x7D };
        for (juce::uint32 v : { function | 1u, a0, a1 })
            for (int s = 28; s >= 0; s -= 7)
                m.push_back((juce::uint8) ((v >> s) & 0x7F));
        return m;
    }

    int split = 0, octave = 0;
    int wantedOctave = KeyboardSetup::UNKNOWN, idlePolls = 0;
    bool known = false, keysStale = false;
    int undone = 0;                          // the transpose just cancelled, until its stored copy is cleared
    Awaiting awaiting = Awaiting::Nothing;
    juce::int64 lastShiftMs = -100000;
};
