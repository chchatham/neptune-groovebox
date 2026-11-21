#pragma once
#include <array>
#include <cstdint>
#include <functional>

// The pads that play notes (SCALE, PRESET, AT ROUTE, the chord pads) send note numbers; what
// sounds them wants one slot per sounding note (the pad ratchet's sixteen), or, with the track's
// arp on, the keys' path. This keeps the books between the two.
//
// A note can be held by more than one pad at once (two chord pads that share a pitch): it is
// counted. The second press strikes the note again in the slot it has; the note ends when the
// last pad holding it is let go. (Without the count the second press took a second slot and the
// first was never given back: after a few overlapping chords every slot was taken and new notes
// were silently not played - the owner's "a pad pressed second is not heard", 2026-11-15.)
//
// One thread at a time (the callers hold the screen manager's lock).
class NotePadSlots
{
public:
    static constexpr int SLOTS = 16;

    std::function<void(int slot, int note, float velocity)> slotOn;
    std::function<void(int slot, float pressure)> slotPressure;
    std::function<void(int slot)> slotOff;
    std::function<void(int note, int velocity)> keyOn;         // the keys' path (the arp)
    std::function<void(int note)> keyOff;

    // False when the note could not be played: every slot is taken.
    bool down(int note, int velocity, bool viaKeys)
    {
        if (note < 0 || note > 127)
            return false;
        auto& n = notes[(size_t) note];
        if (n.holds > 0 && n.viaKeys != viaKeys)
            end(note);                      // (the arp was switched while the note was held)
        if (n.holds > 0)
        {
            ++n.holds;
            if (n.viaKeys)      { if (keyOn) keyOn(note, velocity); }
            else if (slotOn)    slotOn(n.slot, note, (float) velocity / 127.0f);
            return true;
        }
        if (viaKeys)
        {
            n = { 1, -1, true };
            if (keyOn) keyOn(note, velocity);
            return true;
        }
        for (int slot = 0; slot < SLOTS; ++slot)
            if (!busy[(size_t) slot])
            {
                busy[(size_t) slot] = true;
                n = { 1, (int8_t) slot, false };
                if (slotOn) slotOn(slot, note, (float) velocity / 127.0f);
                return true;
            }
        return false;
    }

    void pressure(int note, int value)
    {
        if (note < 0 || note > 127) return;
        const auto& n = notes[(size_t) note];
        if (n.holds > 0 && !n.viaKeys && slotPressure)
            slotPressure(n.slot, (float) value / 127.0f);
    }

    void up(int note)
    {
        if (note < 0 || note > 127) return;
        auto& n = notes[(size_t) note];
        if (n.holds <= 0) return;
        if (--n.holds == 0)
        {
            n.holds = 1;
            end(note);
        }
    }

    int slotsInUse() const
    {
        int count = 0;
        for (bool b : busy) count += b ? 1 : 0;
        return count;
    }
    bool isHeld(int note) const { return note >= 0 && note <= 127 && notes[(size_t) note].holds > 0; }

private:
    struct Held
    {
        int holds = 0;
        int8_t slot = -1;
        bool viaKeys = false;
    };

    void end(int note)
    {
        auto& n = notes[(size_t) note];
        if (n.viaKeys)      { if (keyOff) keyOff(note); }
        else if (n.slot >= 0)
        {
            if (slotOff) slotOff(n.slot);
            busy[(size_t) n.slot] = false;
        }
        n = {};
    }

    std::array<Held, 128> notes {};
    std::array<bool, SLOTS> busy {};
};
