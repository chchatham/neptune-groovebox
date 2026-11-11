#pragma once

#include "../sequencer/Pattern.h"
#include <juce_core/juce_core.h>
#include <deque>
#include <functional>
#include <vector>

// -----------------------------------------------------------------------------
// The undo history: up to MAX_LEVELS actions that can be taken back, and the ones
// taken back that can be done again. ScreenManager fills and applies it (it knows
// the sequencer and the engine hooks); this file is the data and the two stacks.
//
// One entry = one action (a clear, a paste, a reset, an effect swapped, a
// recording pass ...). What an entry holds depends on what the action touched:
//
//   Steps   steps of one pattern: one step, or whole tracks (their steps, length
//           and speed), and the swing when the whole pattern was replaced. The
//           entry holds the OTHER state - before the action while it can be
//           undone, after it while it can be redone - and undo / redo swap it
//           with the pattern. At most one pattern's worth of steps.
//   Sound   one track's sound as two parameter maps, before and after. Undo puts
//           back the parameters the action changed (all of them, with the engine,
//           when the action changed the engine type) and leaves the others as they
//           are now.
//   Custom  a list of numbers a screen captures and applies itself (the
//           arpeggiator's mutation), swapped like Steps.
//
// Nothing here is touched by the audio thread. Entries are built and applied on
// the control threads under ScreenManager's lock; whatever replaces steps or
// rebuilds an engine holds AudioEngine::structure() while it does.
// -----------------------------------------------------------------------------
struct UndoEntry
{
    enum class Scope { Steps, Sound, Custom };
    Scope scope = Scope::Steps;

    // What it was, in words: verb "CLEAR", names longest first ("T3 STEPS", "STEPS").
    juce::String verb;
    juce::StringArray names;

    int serial = 0;                 // given by the history; identifies the entry
    // Quick repeats of one control (FX TYPE turned through its list) are one action: an entry with
    // the same non-zero key, touched within COALESCE_MS, takes the new end state instead of a new entry.
    int coalesceKey = 0;
    juce::int64 touchedMs = 0;

    // ---- Steps ----
    struct Part
    {
        int track = 0;
        int step = -1;              // -1 = the whole track: steps, length, speed
        EngineType engine = EngineType::UnifiedVoice;   // the track's engine then (locks are its parameter ids)
        std::vector<Step> steps;
        int length = 16;
        float speed = 1.0f;
    };
    int pattern = -1;
    std::vector<Part> parts;
    bool hasSwing = false;
    float swing = 0.0f;

    // ---- Sound ----
    int track = -1;
    EngineType typeBefore = EngineType::UnifiedVoice, typeAfter = EngineType::UnifiedVoice;
    ParamMap before, after;

    // ---- Custom ----
    std::function<std::vector<float>()> capture;
    std::function<void(const std::vector<float>&)> apply;
    std::vector<float> blob;

    // "CLEAR T3 STEPS", "CLEAR STEPS", "CLEAR": longest first.
    juce::StringArray labels() const
    {
        juce::StringArray out;
        for (auto& name : names)
            out.add(verb + " " + name);
        out.add(verb);
        return out;
    }
};

namespace UndoData
{
    inline bool sameStep(const Step& a, const Step& b)
    {
        if (a.trig != b.trig || a.velocity != b.velocity || a.length != b.length || a.microTiming != b.microTiming
            || a.trigCondition.type != b.trigCondition.type || a.trigCondition.a != b.trigCondition.a
            || a.trigCondition.b != b.trigCondition.b || a.trigCondition.probability != b.trigCondition.probability
            || a.paramLocks != b.paramLocks || a.pageChangeLock != b.pageChangeLock
            || a.pressureScale != b.pressureScale
            || a.notes.size() != b.notes.size())
            return false;
        for (size_t i = 0; i < a.notes.size(); ++i)
            if (a.notes[i].pitch != b.notes[i].pitch || a.notes[i].velocity != b.notes[i].velocity
                || !(a.notes[i].lane == b.notes[i].lane))
                return false;
        return true;
    }

    inline bool sameSteps(const std::vector<Step>& a, const std::vector<Step>& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!sameStep(a[i], b[i]))
                return false;
        return true;
    }

    // What a pattern holds of a track: its steps, length and speed (the sound is not the pattern's).
    inline bool sameTrackSteps(const Track& a, const Track& b)
    {
        return a.length == b.length && a.speedMultiplier == b.speedMultiplier && sameSteps(a.steps, b.steps);
    }
}

class UndoHistory
{
public:
    static constexpr int MAX_LEVELS = 15;               // the project's decided cap
    static constexpr juce::int64 COALESCE_MS = 2000;

    // A new action: it can be undone, nothing can be redone any more, and the sixteenth pushes
    // the oldest out. Returns the entry's serial.
    int push(UndoEntry entry)
    {
        entry.serial = ++lastSerial;
        redoStack.clear();
        undoStack.push_back(std::move(entry));
        while ((int) undoStack.size() > MAX_LEVELS)
            undoStack.pop_front();
        return lastSerial;
    }

    void clear()
    {
        undoStack.clear();
        redoStack.clear();
    }

    int undoCount() const { return (int) undoStack.size(); }
    int redoCount() const { return (int) redoStack.size(); }

    // Newest last. ScreenManager walks them from the back.
    std::deque<UndoEntry>& undos() { return undoStack; }
    std::deque<UndoEntry>& redos() { return redoStack; }
    const std::deque<UndoEntry>& undos() const { return undoStack; }
    const std::deque<UndoEntry>& redos() const { return redoStack; }

    // The newest entry was undone (redone): it changes sides. Neither side can pass MAX_LEVELS,
    // since entries only move between them.
    void undone()
    {
        if (undoStack.empty()) return;
        redoStack.push_back(std::move(undoStack.back()));
        undoStack.pop_back();
    }
    void redone()
    {
        if (redoStack.empty()) return;
        undoStack.push_back(std::move(redoStack.back()));
        redoStack.pop_back();
    }

    // An entry a quick repeat of the same control can be folded into: same key, touched within
    // COALESCE_MS, with only entries just as recent above it. Null = none.
    UndoEntry* coalesceTarget(int key, juce::int64 now)
    {
        if (key == 0)
            return nullptr;
        for (auto it = undoStack.rbegin(); it != undoStack.rend(); ++it)
        {
            if (now - it->touchedMs > COALESCE_MS)
                break;
            if (it->coalesceKey == key)
                return &*it;
        }
        return nullptr;
    }

private:
    std::deque<UndoEntry> undoStack, redoStack;
    int lastSerial = 0;
};
