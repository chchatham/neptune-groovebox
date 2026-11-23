#pragma once

#include "Track.h"
#include "../engine/PerfFx.h"
#include <vector>

struct Pattern
{
    std::vector<Track> tracks;
    float swing = 0.0f; // 0.0 = no swing, 1.0 = full triplet
    PerfFx::MoveLane perfMoves;     // recorded PERF pad moves for this pattern

    Pattern(int trackCount = 8)
    {
        tracks.resize(trackCount);
    }

    int getTrackCount() const { return (int) tracks.size(); }

    void addTrack()
    {
        tracks.emplace_back();
    }

    void removeTrack(int index)
    {
        if (index >= 0 && index < (int) tracks.size() && tracks.size() > 1)
            tracks.erase(tracks.begin() + index);
    }

    Track& getTrack(int index)
    {
        return tracks[index];
    }

    const Track& getTrack(int index) const
    {
        return tracks[index];
    }

    void clear()
    {
        for (auto& track : tracks)
            track.clearAllSteps();
        perfMoves.clear();
    }

    Pattern duplicate() const
    {
        Pattern copy((int) tracks.size());
        copy.tracks = tracks;
        copy.swing = swing;
        copy.perfMoves = perfMoves;
        return copy;
    }
};
