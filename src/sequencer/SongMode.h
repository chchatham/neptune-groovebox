#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <algorithm>

struct SongEntry
{
    int patternIndex = 0;
    int repeatCount = 1;
    int transposeSemitones = 0;
};

class SongMode
{
public:
    static constexpr int MAX_ENTRIES = 256;
    static constexpr int MAX_PATTERN_INDEX = 15;
    static constexpr int MAX_REPEATS = 999;
    static constexpr int MIN_TRANSPOSE = -12;
    static constexpr int MAX_TRANSPOSE = 12;

    SongMode() = default;

    // --- Editing (main thread only) ---

    bool addEntry(int patternIndex, int repeats = 1)
    {
        if ((int) entries.size() >= MAX_ENTRIES)
            return false;

        SongEntry e;
        e.patternIndex = juce::jlimit(0, MAX_PATTERN_INDEX, patternIndex);
        e.repeatCount = juce::jlimit(1, MAX_REPEATS, repeats);
        e.transposeSemitones = 0;
        entries.push_back(e);
        return true;
    }

    bool removeEntry(int index)
    {
        if (index < 0 || index >= (int) entries.size())
            return false;

        entries.erase(entries.begin() + index);

        if (currentEntryIdx >= (int) entries.size())
        {
            currentEntryIdx = 0;
            currentRepeatNum = 0;
        }

        return true;
    }

    bool moveEntry(int from, int to)
    {
        if (from < 0 || from >= (int) entries.size())
            return false;
        if (to < 0 || to >= (int) entries.size())
            return false;
        if (from == to)
            return true;

        SongEntry e = entries[(size_t) from];
        entries.erase(entries.begin() + from);
        entries.insert(entries.begin() + to, e);
        return true;
    }

    bool setEntry(int index, const SongEntry& entry)
    {
        if (index < 0 || index >= (int) entries.size())
            return false;

        entries[(size_t) index].patternIndex = juce::jlimit(0, MAX_PATTERN_INDEX, entry.patternIndex);
        entries[(size_t) index].repeatCount = juce::jlimit(1, MAX_REPEATS, entry.repeatCount);
        entries[(size_t) index].transposeSemitones = juce::jlimit(MIN_TRANSPOSE, MAX_TRANSPOSE, entry.transposeSemitones);
        return true;
    }

    void clear()
    {
        entries.clear();
        currentEntryIdx = 0;
        currentRepeatNum = 0;
        active = false;
    }

    // --- Playback (advancePattern is audio-thread safe: no allocation, no locks) ---

    void start()
    {
        if (entries.empty())
            return;

        active = true;
        currentEntryIdx = cuedEntryIdx;
        currentRepeatNum = 0;
        cuedEntryIdx = 0;
        finished = false;
    }

    void stop(bool holdPosition = false)
    {
        active = false;
        finished = false;
        if (!holdPosition)
        {
            currentEntryIdx = 0;
            currentRepeatNum = 0;
        }
    }

    bool advancePattern()
    {
        if (!active || entries.empty())
            return false;

        currentRepeatNum++;

        if (currentRepeatNum >= entries[(size_t) currentEntryIdx].repeatCount)
        {
            currentRepeatNum = 0;
            currentEntryIdx++;

            if (currentEntryIdx >= (int) entries.size())
            {
                if (loopSong)
                {
                    currentEntryIdx = 0;
                }
                else
                {
                    currentEntryIdx = (int) entries.size() - 1;
                    active = false;
                    finished = true;
                    return false;
                }
            }

            return true;
        }

        return false;
    }

    // --- State queries ---

    int getCurrentPatternIndex() const
    {
        if (entries.empty())
            return 0;
        if (currentEntryIdx >= (int) entries.size())
            return entries.back().patternIndex;
        return entries[(size_t) currentEntryIdx].patternIndex;
    }

    int getCurrentTranspose() const
    {
        if (entries.empty())
            return 0;
        if (currentEntryIdx >= (int) entries.size())
            return entries.back().transposeSemitones;
        return entries[(size_t) currentEntryIdx].transposeSemitones;
    }

    bool isActive() const { return active; }
    bool isFinished() const { return finished; }

    int getCurrentEntryIndex() const { return currentEntryIdx; }
    int getCurrentRepeat() const { return currentRepeatNum; }

    int getEntryCount() const { return (int) entries.size(); }

    const SongEntry& getEntry(int index) const
    {
        jassert(index >= 0 && index < (int) entries.size());
        return entries[(size_t) index];
    }

    float getProgress() const
    {
        if (entries.empty())
            return 0.0f;

        int totalRepeats = 0;
        int completedRepeats = 0;

        for (int i = 0; i < (int) entries.size(); ++i)
        {
            totalRepeats += entries[(size_t) i].repeatCount;
            if (i < currentEntryIdx)
                completedRepeats += entries[(size_t) i].repeatCount;
            else if (i == currentEntryIdx)
                completedRepeats += currentRepeatNum;
        }

        if (totalRepeats == 0)
            return 0.0f;

        return (float) completedRepeats / (float) totalRepeats;
    }

    // --- Loop mode ---

    // Armed: pressing Play runs the song instead of looping the current pattern.
    void setArmed(bool a) { armed = a; }
    bool isArmed() const { return armed; }

    void setLoopMode(bool loop) { loopSong = loop; }
    bool getLoopMode() const { return loopSong; }

    // --- Cue ---

    void cueEntry(int index)
    {
        cuedEntryIdx = juce::jlimit(0, std::max(0, (int) entries.size() - 1), index);
    }

    // --- Serialization ---

    juce::var toVar() const
    {
        juce::Array<juce::var> arr;

        for (auto& e : entries)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty("pattern", e.patternIndex);
            obj->setProperty("repeats", e.repeatCount);
            obj->setProperty("transpose", e.transposeSemitones);
            arr.add(juce::var(obj));
        }

        auto* root = new juce::DynamicObject();
        root->setProperty("entries", arr);
        root->setProperty("loop", loopSong);

        return juce::var(root);
    }

    void fromVar(const juce::var& v)
    {
        clear();

        if (auto* obj = v.getDynamicObject())
        {
            loopSong = obj->getProperty("loop");

            if (auto* arr = obj->getProperty("entries").getArray())
            {
                for (auto& item : *arr)
                {
                    if (auto* entryObj = item.getDynamicObject())
                    {
                        SongEntry e;
                        e.patternIndex = juce::jlimit(0, MAX_PATTERN_INDEX,
                            (int) entryObj->getProperty("pattern"));
                        e.repeatCount = juce::jlimit(1, MAX_REPEATS,
                            (int) entryObj->getProperty("repeats"));
                        e.transposeSemitones = juce::jlimit(MIN_TRANSPOSE, MAX_TRANSPOSE,
                            (int) entryObj->getProperty("transpose"));
                        entries.push_back(e);

                        if ((int) entries.size() >= MAX_ENTRIES)
                            break;
                    }
                }
            }
        }
    }

private:
    std::vector<SongEntry> entries;

    int currentEntryIdx = 0;
    int currentRepeatNum = 0;
    int cuedEntryIdx = 0;
    bool active = false;
    bool finished = false;
    bool loopSong = true;
    bool armed = false;
};
