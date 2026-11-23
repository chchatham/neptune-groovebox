#pragma once
#include <juce_core/juce_core.h>
#include "../controller/ChordPads.h"
#include "Project.h"

// Chord sets: the four banks of chord pads saved to and loaded from numbered slots, app-wide
// files beside the presets (~/Music/Neptune/Chords/C01.json ... C32.json). A project still
// carries its own chords; these slots let the player recall a chord set across projects.
class ChordSetManager
{
public:
    static constexpr int NUM_SLOTS = 32;

    explicit ChordSetManager(const juce::File& folder) : dir(folder) { dir.createDirectory(); }

    // Save the current chords to a slot (0-based). Returns true on success.
    bool save(int slot, const ChordPads& chords)
    {
        if (slot < 0 || slot >= NUM_SLOTS)
            return false;
        const auto file = slotFile(slot);
        auto* obj = new juce::DynamicObject();
        obj->setProperty("type", "NeptuneChordSet");
        obj->setProperty("version", 1);
        obj->setProperty("data", floatsToVar(chords.toFlat()));
        return file.replaceWithText(juce::JSON::toString(juce::var(obj)));
    }

    // Load chords from a slot. Returns true if the file existed and was readable.
    bool load(int slot, ChordPads& chords)
    {
        if (slot < 0 || slot >= NUM_SLOTS)
            return false;
        const auto file = slotFile(slot);
        if (!file.existsAsFile())
            return false;
        const auto text = file.loadFileAsString();
        auto parsed = juce::JSON::parse(text);
        auto* obj = parsed.getDynamicObject();
        if (obj == nullptr || obj->getProperty("type").toString() != "NeptuneChordSet")
            return false;
        if (! obj->getProperty("data").isArray())
            return false;
        chords.fromFlat(floatsFromVar(obj->getProperty("data")));
        return true;
    }

    // Whether a slot has a saved chord set.
    bool slotExists(int slot) const
    {
        return slot >= 0 && slot < NUM_SLOTS && slotFile(slot).existsAsFile();
    }

    // The file for a slot.
    juce::File slotFile(int slot) const
    {
        return dir.getChildFile("C" + juce::String(slot + 1).paddedLeft('0', 2) + ".json");
    }

private:
    juce::File dir;
};
