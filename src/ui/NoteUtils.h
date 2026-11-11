#pragma once

#include <juce_core/juce_core.h>

namespace NoteUtils {

inline juce::String noteToString(int midiNote)
{
    if (midiNote < 0 || midiNote > 127)
        return "---";
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int octave = (midiNote / 12) - 1;
    return juce::String(names[midiNote % 12]) + juce::String(octave);
}

inline juce::String engineAbbrev(int engineTypeInt)
{
    switch (engineTypeInt)
    {
        case 0: return "VA";
        case 1: return "FM";
        case 2: return "MO";
        case 3: return "WT";
        case 4: return "WS";
        case 5: return "SP";
        case 6: return "DR";
        case 7: return "MD";
        default: return "??";
    }
}

} // namespace NoteUtils
