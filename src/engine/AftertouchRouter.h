#pragma once

#include <array>
#include <algorithm>

class AftertouchRouter
{
public:
    enum class Destination
    {
        Off,
        Volume,
        FilterCutoff,
        FMDepth,
        MacroTimbre,
        WavePosition,
        Vibrato,
        PanSpread,
        // Appended only (projects store the index). Drum tracks: one destination per kit, applied
        // to every pad's voice; a drum track also uses Volume (level), FilterCutoff (tone) and
        // PanSpread (pan).
        DrumPitch,
        DrumDecay,
        DrumDrive,
        DrumSnap
    };

    struct Route
    {
        Destination dest = Destination::Volume;
        float depth = 1.0f;
        bool bipolar = false;
    };

    static constexpr int MAX_ROUTES = 4;

    AftertouchRouter()
    {
        routes[0] = { Destination::Volume, 1.0f, false };
        routeCount = 1;
    }

    void setRoute(int index, const Route& route)
    {
        if (index < 0 || index >= MAX_ROUTES)
            return;
        routes[static_cast<size_t>(index)] = route;
        if (index >= routeCount)
            routeCount = index + 1;
    }

    Route getRoute(int index) const
    {
        if (index < 0 || index >= MAX_ROUTES)
            return {};
        return routes[static_cast<size_t>(index)];
    }

    int getRouteCount() const { return routeCount; }

    void setRouteCount(int count)
    {
        routeCount = count < 0 ? 0 : (count > MAX_ROUTES ? MAX_ROUTES : count);
    }

    struct ModValues
    {
        float volumeMod = 0.0f;
        float cutoffMod = 0.0f;
        float fmDepthMod = 0.0f;
        float timbreMod = 0.0f;
        float wavePosMod = 0.0f;
        float vibratoMod = 0.0f;
        float panMod = 0.0f;
    };

    void removeRoute(int index)
    {
        if (index < 0 || index >= routeCount) return;
        routes[static_cast<size_t>(index)] = { Destination::Off, 0.0f, false };
    }

    static const char* getDestName(Destination d)
    {
        switch (d)
        {
            case Destination::Off:          return "OFF";
            case Destination::Volume:       return "VOL";
            case Destination::FilterCutoff: return "FLT";
            case Destination::FMDepth:      return "FM";
            case Destination::MacroTimbre:  return "TBR";
            case Destination::WavePosition: return "WAV";
            case Destination::Vibrato:      return "VIB";
            case Destination::PanSpread:    return "PAN";
            case Destination::DrumPitch:    return "TUNE";
            case Destination::DrumDecay:    return "DEC";
            case Destination::DrumDrive:    return "DRIV";
            case Destination::DrumSnap:     return "SNAP";
            default:                        return "---";
        }
    }

    static constexpr int DEST_COUNT = 12;

    ModValues computeModulation(float pressure) const
    {
        ModValues vals;
        for (int i = 0; i < routeCount; ++i)
        {
            const auto& r = routes[static_cast<size_t>(i)];
            if (r.dest == Destination::Off)
                continue;

            float mod;
            if (r.bipolar)
                mod = (pressure * 2.0f - 1.0f) * r.depth;
            else
                mod = pressure * r.depth;

            switch (r.dest)
            {
                case Destination::Volume:       vals.volumeMod   += mod; break;
                case Destination::FilterCutoff:  vals.cutoffMod   += mod; break;
                case Destination::FMDepth:       vals.fmDepthMod  += mod; break;
                case Destination::MacroTimbre:   vals.timbreMod   += mod; break;
                case Destination::WavePosition:  vals.wavePosMod  += mod; break;
                case Destination::Vibrato:       vals.vibratoMod  += mod; break;
                case Destination::PanSpread:     vals.panMod      += mod; break;
                default:                         break;      // Off, and the drum destinations
            }
        }
        return vals;
    }

    // Every route off.
    void clear()
    {
        for (auto& r : routes) r = { Destination::Off, 0.5f, false };
        routeCount = MAX_ROUTES;
    }

    // True when both routers do the same thing (unused and OFF routes are the same).
    bool sameRoutesAs(const AftertouchRouter& other) const
    {
        for (int i = 0; i < MAX_ROUTES; ++i)
        {
            const Route a = effectiveRoute(i), b = other.effectiveRoute(i);
            if (a.dest != b.dest) return false;
            if (a.dest != Destination::Off && (a.depth != b.depth || a.bipolar != b.bipolar)) return false;
        }
        return true;
    }

    // Route i, or OFF beyond the route count.
    Route effectiveRoute(int index) const
    {
        if (index < 0 || index >= routeCount) return { Destination::Off, 0.5f, false };
        return routes[static_cast<size_t>(index)];
    }

private:
    std::array<Route, MAX_ROUTES> routes {};
    int routeCount = 1;
};
