#pragma once

#include "../sequencer/Track.h"
#include "../engine/ParamSpec.h"

namespace ParamConstants {

// Track mixer parameters (applied by AudioEngine, see MixParam).
static constexpr int PARAM_VOL  = MixParam::VOLUME;
static constexpr int PARAM_PAN  = MixParam::PAN;
static constexpr int PARAM_SND1 = MixParam::SEND_BASE;
static constexpr int PARAM_SND2 = MixParam::SEND_BASE + 1;

inline float getTrackParam(const Track& track, int paramId, float defaultVal)
{
    auto it = track.soundParams.find(paramId);
    return (it != track.soundParams.end()) ? it->second : defaultVal;
}

} // namespace ParamConstants
