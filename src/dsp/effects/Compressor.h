#pragma once

#include "Detector.h"

// Compressor, CMP (and the master section's): the shared compressor (dyn::CompressorFx in
// Detector.h) with these cells.
//
//   FX    THR   threshold                      RATI  ratio, 1 (nothing) to 20
//         ATK   how fast the gain comes down   MAKE  make-up gain
//   FX2   KEY   what the detector listens to: SELF, or IN = the track's input (the source of the
//               track's IN page): the track is then pressed down by another track's sound
//         REL   how fast the gain comes back   KNEE  width of the soft knee, 0 = hard
//         SCHP  high-pass in front of the detector, 20 Hz = none (the bass no longer pumps it)
//   WET   below the top: the dry sound beside the compressed one (parallel compression)
//
// By setter only (the pages are full): setHold, setDetectionMode (peak / RMS).
//
// The master compressor rests at 0 dB / 1:1. At 1:1 the gain is exactly 1 and the sound passes
// bit for bit (the "P01 Keyboard" pin); the detector is not even run.
class Compressor : public dyn::CompressorFx
{
public:
    using S = dyn::Setting;

    Compressor()
        : CompressorFx({ S::Threshold, S::Ratio, S::Attack, S::Makeup, S::Key, S::Release, S::Knee, S::SideHp }, {})
    {}
};
