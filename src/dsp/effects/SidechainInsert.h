#pragma once

#include "Detector.h"

// Ducker, DUCK: the shared compressor (dyn::CompressorFx in Detector.h) set up to press a track
// down under another one (the bass under the kick; on the master chain, the whole mix under it),
// with these cells.
//
//   FX    SRC   what the detector listens to: the one key choice every keyed effect has (CMP's,
//               TRNS's and WAH's KEY are the same cell): SELF (ordinary compression), track 1..16
//               (that track's sound after its inserts), or IN (the track's input)
//         THR   threshold        RATI  ratio        REL  how fast the track comes back
//   FX2   ATK   how fast it goes down (2 ms to start with)
//         HOLD  how long it stays down after the key has fallen: the shape of the pump
//         KNEE  width of the soft knee (6 dB to start with)
//         SCHP  high-pass in front of the detector, 20 Hz = none
//
// The effect cannot reach other tracks' audio by itself, and has no door of its own for it: it
// says what it listens to (EffectBase::keySource) and is handed that sound through
// EffectBase::setInput before each block, like every other effect (KeyFeed in EffectChain.h).
// The pointers are used for the next processBlock call only and then dropped, so a missing call
// falls back to SELF and a stale pointer is never read.
class SidechainInsert : public dyn::CompressorFx
{
public:
    using S = dyn::Setting;
    enum Param { Source = 0, Threshold, Ratio, Release };
    enum Extra { Attack = 0, Hold, Knee, SideHp };

    SidechainInsert()
        : CompressorFx({ S::Source, S::Threshold, S::Ratio, S::Release, S::Attack, S::Hold, S::Knee, S::SideHp },
                       { { S::Threshold, -24.0f }, { S::Release, 150.0f }, { S::Attack, 2.0f }, { S::Knee, 6.0f } })
    {}
};
