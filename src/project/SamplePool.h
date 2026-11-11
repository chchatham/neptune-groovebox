#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <cstring>
#include <mutex>

// SamplePool: manages all samples in a project.
// - Message-thread only for add/remove/rename.
// - Audio thread reads sample data via const pointers (no locking needed
//   because audio thread never sees a sample being added/removed mid-block;
//   engine holds a local copy of the sample data).
// - Reference counting: tracks hold samplePoolId; pool tracks refcounts.
// - Serialization: save/load sample metadata + raw PCM to project folder.

class SamplePool
{
public:
    struct SampleEntry
    {
        int id = -1;
        juce::String name;
        std::vector<float> dataL;
        std::vector<float> dataR;  // empty for mono
        int lengthInSamples = 0;
        double sampleRate = 44100.0;
        int channels = 1;
        int refCount = 0;

        bool isStereo() const { return channels >= 2 && !dataR.empty(); }
        double durationSeconds() const
        {
            return sampleRate > 0 ? (double) lengthInSamples / sampleRate : 0.0;
        }
    };

    static constexpr int MAX_POOL_SIZE = 256;
    static constexpr size_t MAX_TOTAL_BYTES = 512 * 1024 * 1024; // 512 MB

    SamplePool() = default;

    // Add a mono sample. Returns the sample ID, or -1 if pool is full.
    int addSample(const juce::String& name, const float* data, int numSamples, double sampleRate)
    {
        return addSampleInternal(name, data, nullptr, numSamples, 1, sampleRate);
    }

    // Add a stereo sample (split L/R buffers). Returns sample ID or -1.
    int addSampleStereo(const juce::String& name,
                        const float* dataL, const float* dataR,
                        int numFrames, double sampleRate)
    {
        return addSampleInternal(name, dataL, dataR, numFrames, 2, sampleRate);
    }

    // Add a stereo sample from interleaved data. Returns sample ID or -1.
    int addSampleStereoInterleaved(const juce::String& name,
                                   const float* interleavedData,
                                   int numFrames, double sampleRate)
    {
        if ((int) entries.size() >= MAX_POOL_SIZE) return -1;

        std::vector<float> tempL((size_t) numFrames);
        std::vector<float> tempR((size_t) numFrames);
        for (int i = 0; i < numFrames; ++i)
        {
            tempL[(size_t) i] = interleavedData[i * 2];
            tempR[(size_t) i] = interleavedData[i * 2 + 1];
        }

        return addSampleInternal(name, tempL.data(), tempR.data(), numFrames, 2, sampleRate);
    }

    // Remove sample by ID (only if refCount == 0). Returns true if removed.
    bool removeSample(int id)
    {
        auto it = findById(id);
        if (it == entries.end()) return false;
        if (it->refCount > 0) return false;  // still referenced

        totalBytes -= computeEntryBytes(*it);
        entries.erase(it);
        return true;
    }

    // Force-remove even with references (for project cleanup).
    bool forceRemoveSample(int id)
    {
        auto it = findById(id);
        if (it == entries.end()) return false;
        totalBytes -= computeEntryBytes(*it);
        entries.erase(it);
        return true;
    }

    // Rename a sample.
    bool renameSample(int id, const juce::String& newName)
    {
        auto it = findById(id);
        if (it == entries.end()) return false;
        it->name = newName;
        return true;
    }

    // Reference counting (called by track when assigning/unassigning a sample).
    void addRef(int id)
    {
        auto it = findById(id);
        if (it != entries.end())
            it->refCount++;
    }

    void releaseRef(int id)
    {
        auto it = findById(id);
        if (it != entries.end() && it->refCount > 0)
            it->refCount--;
    }

    int getRefCount(int id) const
    {
        auto it = findByIdConst(id);
        return (it != entries.end()) ? it->refCount : 0;
    }

    // Lookup.
    const SampleEntry* getSample(int id) const
    {
        auto it = findByIdConst(id);
        return (it != entries.end()) ? &(*it) : nullptr;
    }

    SampleEntry* getSampleMutable(int id)
    {
        auto it = findById(id);
        return (it != entries.end()) ? &(*it) : nullptr;
    }

    int getSampleCount() const { return (int) entries.size(); }
    bool isFull() const { return (int) entries.size() >= MAX_POOL_SIZE; }

    // Get all entries (for UI listing).
    const std::vector<SampleEntry>& getAllSamples() const { return entries; }

    // Find by name.
    int findByName(const juce::String& name) const
    {
        for (const auto& e : entries)
            if (e.name == name) return e.id;
        return -1;
    }

    // Pool memory usage.
    size_t getTotalBytes() const { return totalBytes; }
    size_t getMaxBytes() const { return MAX_TOTAL_BYTES; }
    bool hasSpaceFor(int numFrames, int channels) const
    {
        size_t needed = (size_t) numFrames * (size_t) channels * sizeof(float);
        return (totalBytes + needed) <= MAX_TOTAL_BYTES;
    }

    // Evict least-recently-used unreferenced samples until space is available.
    // Returns number of samples evicted.
    int evictUntilSpace(size_t bytesNeeded)
    {
        int evicted = 0;
        while (totalBytes + bytesNeeded > MAX_TOTAL_BYTES && !entries.empty())
        {
            // Find first unreferenced entry (oldest first since we push_back)
            bool found = false;
            for (auto it = entries.begin(); it != entries.end(); ++it)
            {
                if (it->refCount == 0)
                {
                    totalBytes -= computeEntryBytes(*it);
                    entries.erase(it);
                    evicted++;
                    found = true;
                    break;
                }
            }
            if (!found) break; // all samples are referenced
        }
        return evicted;
    }

    // --- Serialization ---

    // Save pool metadata to a JSON value object.
    juce::var toVar() const
    {
        juce::Array<juce::var> arr;
        for (const auto& e : entries)
        {
            auto obj = new juce::DynamicObject();
            obj->setProperty("id", e.id);
            obj->setProperty("name", e.name);
            obj->setProperty("length", e.lengthInSamples);
            obj->setProperty("sampleRate", e.sampleRate);
            obj->setProperty("channels", e.channels);
            obj->setProperty("refCount", e.refCount);
            obj->setProperty("file", "sample_" + juce::String(e.id) + ".raw");
            arr.add(juce::var(obj));
        }
        return juce::var(arr);
    }

    // Save raw PCM data files to a directory.
    bool saveRawData(const juce::File& directory) const
    {
        for (const auto& e : entries)
        {
            juce::File file = directory.getChildFile("sample_" + juce::String(e.id) + ".raw");
            juce::FileOutputStream fos(file);
            if (!fos.openedOk()) return false;

            // Header: channels (int32), length (int32), sampleRate (double)
            fos.writeInt(e.channels);
            fos.writeInt(e.lengthInSamples);
            fos.writeDouble(e.sampleRate);

            // L channel
            fos.write(e.dataL.data(), sizeof(float) * e.dataL.size());
            // R channel (if stereo)
            if (e.isStereo())
                fos.write(e.dataR.data(), sizeof(float) * e.dataR.size());
        }
        return true;
    }

    // Load from JSON var + directory of raw files.
    bool loadFromVar(const juce::var& data, const juce::File& directory)
    {
        entries.clear();
        totalBytes = 0;
        nextId = 1;

        if (!data.isArray()) return false;

        for (int i = 0; i < data.size(); ++i)
        {
            auto& obj = data[i];
            SampleEntry e;
            e.id = (int) obj.getProperty("id", -1);
            e.name = obj.getProperty("name", "").toString();
            e.lengthInSamples = (int) obj.getProperty("length", 0);
            e.sampleRate = (double) obj.getProperty("sampleRate", 44100.0);
            e.channels = (int) obj.getProperty("channels", 1);
            e.refCount = (int) obj.getProperty("refCount", 0);

            juce::String filename = obj.getProperty("file", "").toString();
            juce::File rawFile = directory.getChildFile(filename);

            if (rawFile.existsAsFile() && e.lengthInSamples > 0)
            {
                juce::FileInputStream fis(rawFile);
                if (fis.openedOk())
                {
                    int ch = fis.readInt();
                    int len = fis.readInt();
                    double sr = fis.readDouble();

                    // Validate against metadata
                    if (ch == e.channels && len == e.lengthInSamples)
                    {
                        e.sampleRate = sr;
                        e.dataL.resize((size_t) len);
                        fis.read(e.dataL.data(), sizeof(float) * (size_t) len);

                        if (ch >= 2)
                        {
                            e.dataR.resize((size_t) len);
                            fis.read(e.dataR.data(), sizeof(float) * (size_t) len);
                        }
                    }
                }
            }

            if (e.id >= nextId)
                nextId = e.id + 1;

            totalBytes += computeEntryBytes(e);
            entries.push_back(std::move(e));
        }
        return true;
    }

    // Clear entire pool.
    void clear()
    {
        entries.clear();
        totalBytes = 0;
        nextId = 1;
    }

private:
    int addSampleInternal(const juce::String& name,
                          const float* dataL, const float* dataR,
                          int numFrames, int channels, double sampleRate)
    {
        if ((int) entries.size() >= MAX_POOL_SIZE) return -1;

        size_t bytesNeeded = (size_t) numFrames * (size_t) channels * sizeof(float);
        if (!hasSpaceFor(numFrames, channels))
        {
            evictUntilSpace(bytesNeeded);
            if (!hasSpaceFor(numFrames, channels))
                return -1; // still no space after eviction
        }

        SampleEntry e;
        e.id = nextId++;
        e.name = name;
        e.lengthInSamples = numFrames;
        e.sampleRate = sampleRate;
        e.channels = channels;
        e.refCount = 0;

        e.dataL.resize((size_t) numFrames);
        std::memcpy(e.dataL.data(), dataL, sizeof(float) * (size_t) numFrames);

        if (channels >= 2 && dataR != nullptr)
        {
            e.dataR.resize((size_t) numFrames);
            std::memcpy(e.dataR.data(), dataR, sizeof(float) * (size_t) numFrames);
        }

        totalBytes += computeEntryBytes(e);
        int id = e.id;
        entries.push_back(std::move(e));
        return id;
    }

    static size_t computeEntryBytes(const SampleEntry& e)
    {
        return (e.dataL.size() + e.dataR.size()) * sizeof(float);
    }

    std::vector<SampleEntry>::iterator findById(int id)
    {
        return std::find_if(entries.begin(), entries.end(),
            [id](const SampleEntry& e) { return e.id == id; });
    }

    std::vector<SampleEntry>::const_iterator findByIdConst(int id) const
    {
        return std::find_if(entries.begin(), entries.end(),
            [id](const SampleEntry& e) { return e.id == id; });
    }

    std::vector<SampleEntry> entries;
    int nextId = 1;
    size_t totalBytes = 0;
};
