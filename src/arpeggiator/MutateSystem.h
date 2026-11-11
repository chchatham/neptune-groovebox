#pragma once

#include "../sequencer/Step.h"
#include <vector>
#include <algorithm>
#include <cstdint>

class MutateSystem
{
public:
    enum class Mode
    {
        Ancestor,    // Always mutate from original snapshot
        Cumulative   // Mutate from last result (compounds over time)
    };

    MutateSystem() = default;

    // --- Configuration ---

    void setMode(Mode m) { mode = m; }
    Mode getMode() const { return mode; }

    // How many loop cycles between mutations (1-16)
    void setInterval(int cycles) { interval = std::max(1, std::min(16, cycles)); }
    int getInterval() const { return interval; }

    // How many steps are mutated per cycle (0.0-1.0)
    void setAmount(float a) { amount = std::max(0.0f, std::min(1.0f, a)); }
    float getAmount() const { return amount; }

    // --- Snapshot management ---

    // Store a deep copy of the original step data
    void setAncestor(const std::vector<Step>& steps)
    {
        ancestor = steps;
        hasAncestorData = true;
    }

    // Restore original pattern data
    void returnToAncestor(std::vector<Step>& steps) const
    {
        if (hasAncestorData)
            steps = ancestor;
    }

    // Current becomes the new ancestor (accept mutations as baseline)
    void commitAncestor(const std::vector<Step>& steps)
    {
        ancestor = steps;
    }

    bool hasAncestor() const { return hasAncestorData; }
    const std::vector<Step>& getAncestor() const { return ancestor; }

    // --- Mutation ---

    // Apply one round of mutations to the step data.
    // In Ancestor mode: mutates from original snapshot into steps.
    // In Cumulative mode: mutates steps in-place.
    void mutate(std::vector<Step>& steps, uint32_t seed)
    {
        if (!hasAncestorData || steps.empty())
            return;

        if (mode == Mode::Ancestor)
        {
            // Start from the ancestor each time
            steps = ancestor;
        }

        int stepCount = (int)steps.size();
        int mutationsToApply = std::max(1, (int)(amount * (float)stepCount));

        uint32_t s = seed;

        for (int m = 0; m < mutationsToApply; ++m)
        {
            s = xorshift(s);
            int targetStep = (int)(s % (uint32_t)stepCount);

            s = xorshift(s);
            int mutationType = (int)(s % 4u);

            switch (mutationType)
            {
                case 0: mutateFlipTrig(steps[targetStep], s); break;
                case 1: mutateShiftNote(steps[targetStep], s); break;
                case 2: mutateRandomizeVelocity(steps[targetStep], s); break;
                case 3: mutateParamLock(steps[targetStep], s); break;
            }
        }
    }

    // --- Note-slot mutation (real-time safe: fixed storage, no allocation) ---
    //
    // Used by the arpeggiator. A slot is one position of the played sequence; its
    // mutation is kept as an offset from the notes that are held, so the original
    // is never overwritten. Two layers: the ancestor and the current variation.
    //   Ancestor mode:   each round is a fresh variation of the ancestor.
    //   Cumulative mode: each round builds on the previous one.

    struct NoteMutation
    {
        int   pitchOffset   = 0;      // semitones
        float velocityScale = 1.0f;
        bool  muted         = false;

        bool isIdentity() const { return pitchOffset == 0 && velocityScale == 1.0f && !muted; }
    };

    static constexpr int MAX_NOTE_SLOTS = 128;

    // One round of mutations over the first `slotCount` slots.
    void mutateNotes(int slotCount, uint32_t seed)
    {
        slotCount = std::max(0, std::min(MAX_NOTE_SLOTS, slotCount));
        if (slotCount == 0)
            return;

        if (mode == Mode::Ancestor)
        {
            for (int i = 0; i < MAX_NOTE_SLOTS; ++i)
                noteCurrent[i] = noteAncestor[i];
            noteGeneration = 0;
        }

        int mutationsToApply = std::max(1, (int)(amount * (float)slotCount));
        uint32_t s = (seed != 0u) ? seed : 1u;
        int target = 0;

        for (int m = 0; m < mutationsToApply; ++m)
        {
            s = xorshift(s);
            target = (int)((s >> 8) % (uint32_t)slotCount);
            s = xorshift(s);
            int mutationType = (int)((s >> 8) % 4u);
            s = xorshift(s);
            uint32_t r = s >> 8;
            auto& slot = noteCurrent[target];

            switch (mutationType)
            {
                case 0:     // flip the trig
                    slot.muted = !slot.muted;
                    break;
                case 1:     // shift by 1-5 semitones
                {
                    int shift = (int)(r % 11u) - 5;
                    if (shift == 0) shift = 1;
                    slot.pitchOffset = std::max(-24, std::min(24, slot.pitchOffset + shift));
                    break;
                }
                case 2:     // new velocity
                    slot.velocityScale = 0.4f + 0.6f * ((float)(r & 0xFFFFu) / 65535.0f);
                    break;
                default:    // octave jump
                {
                    int jump = (r & 1u) ? 12 : -12;
                    if (slot.pitchOffset + jump > 24 || slot.pitchOffset + jump < -24)
                        jump = -jump;
                    slot.pitchOffset += jump;
                    break;
                }
            }
        }

        // Never leave the whole sequence silent.
        bool anySounding = false;
        for (int i = 0; i < slotCount; ++i)
            if (!noteCurrent[i].muted) { anySounding = true; break; }
        if (!anySounding)
            noteCurrent[target].muted = false;

        ++noteGeneration;
    }

    // Current variation goes back to the ancestor.
    void returnNotesToAncestor()
    {
        for (int i = 0; i < MAX_NOTE_SLOTS; ++i)
            noteCurrent[i] = noteAncestor[i];
        noteGeneration = 0;
    }

    // Current variation becomes the ancestor.
    void commitNotesToAncestor()
    {
        notesCommitted = false;
        for (int i = 0; i < MAX_NOTE_SLOTS; ++i)
        {
            noteAncestor[i] = noteCurrent[i];
            if (!noteAncestor[i].isIdentity())
                notesCommitted = true;
        }
        noteGeneration = 0;
    }

    // Ancestor and current variation both go back to the unchanged notes.
    void resetNotes()
    {
        for (int i = 0; i < MAX_NOTE_SLOTS; ++i)
        {
            noteAncestor[i] = NoteMutation();
            noteCurrent[i] = NoteMutation();
        }
        noteGeneration = 0;
        notesCommitted = false;
    }

    const NoteMutation& getNoteMutation(int slot) const
    {
        return noteCurrent[std::max(0, std::min(MAX_NOTE_SLOTS - 1, slot))];
    }

    // The whole note-mutation state (for the undo of "return to ancestor"): plain data, no allocation.
    struct NoteState
    {
        NoteMutation ancestor[MAX_NOTE_SLOTS];
        NoteMutation current[MAX_NOTE_SLOTS];
        int generation = 0;
        bool committed = false;
    };
    void getNoteState(NoteState& out) const
    {
        for (int i = 0; i < MAX_NOTE_SLOTS; ++i)
        {
            out.ancestor[i] = noteAncestor[i];
            out.current[i] = noteCurrent[i];
        }
        out.generation = noteGeneration;
        out.committed = notesCommitted;
    }
    void setNoteState(const NoteState& in)
    {
        for (int i = 0; i < MAX_NOTE_SLOTS; ++i)
        {
            noteAncestor[i] = in.ancestor[i];
            noteCurrent[i] = in.current[i];
        }
        noteGeneration = in.generation;
        notesCommitted = in.committed;
    }

    // Rounds applied on top of the ancestor (0 = playing the ancestor).
    int getNoteGeneration() const { return noteGeneration; }

    // True when the ancestor is a committed mutation rather than the unchanged notes.
    bool hasCommittedNotes() const { return notesCommitted; }

private:
    NoteMutation noteAncestor[MAX_NOTE_SLOTS];
    NoteMutation noteCurrent[MAX_NOTE_SLOTS];
    int noteGeneration = 0;
    bool notesCommitted = false;

    std::vector<Step> ancestor;
    bool hasAncestorData = false;
    Mode mode = Mode::Ancestor;
    int interval = 4;
    float amount = 0.3f;

    // xorshift32 PRNG step
    static uint32_t xorshift(uint32_t seed)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    }

    // Flip the trig state of a step
    static void mutateFlipTrig(Step& step, uint32_t /*seed*/)
    {
        step.trig = !step.trig;
        // If we just enabled a trig on an empty step, give it a default note
        if (step.trig && step.notes.empty())
            step.notes.push_back({60, 0.8f});
    }

    // Shift a random note by +-1-5 semitones
    static void mutateShiftNote(Step& step, uint32_t seed)
    {
        if (step.notes.empty())
            return;

        uint32_t s = xorshift(seed);
        int noteIdx = (int)(s % (uint32_t)step.notes.size());

        s = xorshift(s);
        int shift = (int)(s % 11u) - 5;  // -5 to +5
        if (shift == 0) shift = 1;

        int newPitch = step.notes[noteIdx].pitch + shift;
        step.notes[noteIdx].pitch = std::max(0, std::min(127, newPitch));
    }

    // Randomize the velocity of a step
    static void mutateRandomizeVelocity(Step& step, uint32_t seed)
    {
        uint32_t s = xorshift(seed);
        float r = (float)(s & 0x7FFFFFFF) / (float)0x80000000;
        step.velocity = std::max(0.1f, std::min(1.0f, r));
    }

    // Add or remove a random parameter lock
    static void mutateParamLock(Step& step, uint32_t seed)
    {
        uint32_t s = xorshift(seed);

        if (!step.paramLocks.empty() && (s & 1u))
        {
            // Remove a random lock
            s = xorshift(s);
            int idx = (int)(s % (uint32_t)step.paramLocks.size());
            auto it = step.paramLocks.begin();
            std::advance(it, idx);
            step.paramLocks.erase(it);
        }
        else
        {
            // Add a random lock (param IDs 0-99)
            s = xorshift(s);
            int paramId = (int)(s % 100u);
            s = xorshift(s);
            float val = (float)(s & 0x7FFFFFFF) / (float)0x80000000;
            step.paramLocks[paramId] = val;
        }
    }
};
