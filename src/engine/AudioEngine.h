#pragma once

#include "UnifiedVoiceEngine.h"
#include "FMEngine.h"
#include "MIDITrack.h"
#include "MacroOscEngine.h"
#include "WavetableEngine.h"
#include "WaveSeqEngine.h"
#include "DrumEngine.h"
#include "SamplerEngine.h"
#include "ResonatorEngine.h"
#include "../sequencer/SequencerCore.h"
#include "../arpeggiator/Arpeggiator.h"
#include "../sequencer/PadRatchet.h"
#include "Metronome.h"
#include "../sync/MIDIClock.h"
#include "Resampler.h"
#include "SampleBank.h"
#include "../dsp/effects/SendBus.h"
#include "../dsp/effects/EffectChain.h"
#include "../dsp/effects/Reverb.h"
#include "../dsp/effects/Delay.h"
#include "../dsp/effects/Chorus.h"
#include "../dsp/effects/Drive.h"
#include "../dsp/effects/Flanger.h"
#include "../dsp/effects/Phaser.h"
#include "../dsp/effects/Tremolo.h"
#include "../dsp/effects/ParametricEQ.h"
#include "../dsp/effects/Compressor.h"
#include "../dsp/effects/Limiter.h"
#include "../dsp/effects/FilterEffect.h"
#include "../dsp/effects/TrackFx.h"
#include "../dsp/effects/DirtEffect.h"
#include "../dsp/effects/TalkEffect.h"
#include "../dsp/effects/Ensemble.h"
#include "../dsp/effects/AutoPan.h"
#include "../dsp/effects/HallReverb.h"
#include "../dsp/effects/SpringReverb.h"
#include "../dsp/effects/ShimmerReverb.h"
#include "../dsp/effects/ReverseDelay.h"
#include "../dsp/effects/TransientShaper.h"
#include "../dsp/effects/RingMod.h"
#include "../dsp/effects/Vibrato.h"
#include "../dsp/effects/Wah.h"
#include "../dsp/effects/AmpSim.h"
#include "../dsp/effects/GraphicEQ.h"
#include "../dsp/effects/TapeDelay.h"
#include "../dsp/effects/LCRDelay.h"
#include "../dsp/effects/MultiTapInsert.h"
#include "../dsp/effects/SidechainInsert.h"
#include "../dsp/effects/Vocoder.h"
#include "PerfFx.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

enum class EffectType
{
    ChorusEffect,
    DriveEffect,
    FlangerEffect,
    PhaserEffect,
    TremoloEffect,
    DelayEffect,
    ReverbEffect,
    EQEffect,
    CompressorEffect,
    LimiterEffect,
    FilterEffectType,
    EnsembleEffect,
    AutoPanEffect,
    RotaryEffect,
    WaveshaperEffect,
    RateReducerEffect,
    HallReverbEffect,
    SpringReverbEffect,
    ShimmerReverbEffect,
    ReverseDelayEffect,
    TransientEffect,
    // Appended only: projects store the index.
    RingModEffect,
    VibratoEffect,
    WahEffect,
    AmpEffect,
    GraphicEQEffect,
    TapeDelayEffect,
    LCRDelayEffect,
    MultiTapEffect,
    SidechainEffect,
    VocoderEffect,
    DirtInsertEffect,
    TalkInsertEffect,
    NumEffectTypes
};

class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    static constexpr int SEND_REVERB = 0;
    static constexpr int SEND_DELAY = 1;
    static constexpr int DEFAULT_NUM_SENDS = 2;
    static constexpr int MAX_SENDS = 8;
    static constexpr int MAX_INSERTS_PER_TRACK = 8;

    static constexpr int MAX_TRACKS = 32;
    // The master chain as a track for the insert functions and the insert parameter ids: the mix
    // has an insert chain like any track (addInsertEffect, setInsertEffect, removeInsertEffect,
    // getInsertEffectType, getInsertChain, and set / getTrackParameter with the ids of the slots:
    // EFFECT_PARAM_BASE, INSERT_ON_BASE .. INSERT_DIV_BASE, INSERT_EXTRA_BASE). It has nothing
    // else of a track: no engine, mixer, blocks, input or locks.
    static constexpr int MASTER_TRACK = MAX_TRACKS;
    // A send bus is addressed the same way: sendTrack(bus) names the bus's chain for the same
    // functions and the same ids. Any insert type can sit on a send, several in a row; what the
    // tracks send is run through the chain and what comes out is added to the mix (so an effect
    // put on a send starts with its mix at 1: the wet sound alone, as the send reverb and delay
    // always were; a send with no effect gives nothing).
    static constexpr int SEND_TRACK_BASE = MASTER_TRACK + 1;
    static constexpr int sendTrack(int bus) { return SEND_TRACK_BASE + bus; }
    static constexpr bool isSendTrack(int trackIndex) { return trackIndex >= SEND_TRACK_BASE && trackIndex < SEND_TRACK_BASE + MAX_SENDS; }
    // A chain without a track (the mix, a send): insert slots and nothing else of a track.
    static constexpr bool isChainTrack(int trackIndex) { return trackIndex == MASTER_TRACK || isSendTrack(trackIndex); }
    static constexpr int MAX_ACTIVE_LOCKS = 16;
    // Track mixer parameters (volume, pan, send levels) are MixParam ids: applied here, never
    // forwarded to an engine.
    static constexpr int PARAM_SEND_BASE = MixParam::SEND_BASE;
    static constexpr int PARAM_SEND1 = MixParam::SEND_BASE;
    static constexpr int PARAM_SEND2 = MixParam::SEND_BASE + 1;
    static constexpr int SEND_TYPE_PARAM_BASE = 150;  // 150 + bus: the id of the TRACK page's BUS cells (a screen's cell, not a parameter here)
    static constexpr int EFFECT_PARAM_BASE = 200;  // 200 + slot*4 + paramIdx

    // MOD param IDs are in ModParam namespace (EngineBase.h)

    // The track input's values on a new track (TrackInParam; SRC and MODE are 0 = OFF).
    static constexpr float TRACK_IN_LEVEL = 1.0f;
    static constexpr float TRACK_IN_ATTACK_MS = 5.0f;
    static constexpr float TRACK_IN_RELEASE_MS = 150.0f;
    static float trackInDefault(int id)
    {
        return id == TrackInParam::LEVEL ? TRACK_IN_LEVEL : id == TrackInParam::FOLLOW_ATK ? TRACK_IN_ATTACK_MS
             : id == TrackInParam::FOLLOW_REL ? TRACK_IN_RELEASE_MS : 0.0f;
    }
    // The ids in use, for whoever stores or resets them.
    static constexpr int TRACK_IN_IDS[5] = { TrackInParam::SRC, TrackInParam::LEVEL, TrackInParam::MODE,
                                             TrackInParam::FOLLOW_ATK, TrackInParam::FOLLOW_REL };

    AudioEngine()
    {
        padRatchet.onNoteOn = [this](int pad, int note, float velocity)
        {
            const int t = padTrack[(size_t) pad];
            if (auto* e = getEngine(t))
            {
                e->notePlayed((float) note);
                e->noteOn(note, velocity, -1);
            }
            padSounding[(size_t) pad] = note;
            const uint32_t serial = sequencer != nullptr ? sequencer->getRecordSerial() : 0u;
            if (onPadHit)
                onPadHit(t, note, velocity);
            // The hit was recorded: so is what the pad's pressure does while it is held (not on a
            // rolling pad, whose pressure is the roll's rate).
            if (sequencer != nullptr && sequencer->getRecordSerial() != serial && !padRatchet.isRolling(pad))
                sequencer->beginPressureCapture(pad, t, note);
        };
        padRatchet.onNoteOff = [this](int pad, int note)
        {
            const int t = padTrack[(size_t) pad];
            if (auto* e = getEngine(t))
                e->noteOff(note, -1);
            padSounding[(size_t) pad] = -1;
            if (sequencer != nullptr)
            {
                sequencer->endPressureCapture(pad, !padRatchet.isRolling(pad));
                sequencer->refreshLanePressure(t);      // a recorded lane the pad overrode carries on
            }
        };
        padSounding.fill(-1);

        for (auto& m : trackMix)
        {
            m.volume = MixParam::DEFAULT_VOLUME;
            m.pan = 0.0f;
            for (auto& s : m.sends) s = 0.0f;
        }
        for (auto& shape : trackShape)
            for (int i = 0; i < LFO::USER_POINTS; ++i)
                shape[(size_t) i] = LFO::defaultUserLevel(i);
        buildDefaultMasterChain();
        perfFx.setCreator(&AudioEngine::createPerfEffect, &AudioEngine::perfWays, &structureMutex);
        sendBuses.resize(DEFAULT_NUM_SENDS);
        buildDefaultSends();
    }

    static std::unique_ptr<EffectBase> createEffect(EffectType type)
    {
        switch (type)
        {
            case EffectType::ChorusEffect:      return std::make_unique<Chorus>();
            case EffectType::DriveEffect:        return std::make_unique<Drive>();
            case EffectType::FlangerEffect:      return std::make_unique<Flanger>();
            case EffectType::PhaserEffect:       return std::make_unique<Phaser>();
            case EffectType::TremoloEffect:      return std::make_unique<Tremolo>();
            case EffectType::DelayEffect:        return std::make_unique<Delay>();
            case EffectType::ReverbEffect:       return std::make_unique<Reverb>();
            case EffectType::EQEffect:           return std::make_unique<ParametricEQ>();
            case EffectType::CompressorEffect:   return std::make_unique<Compressor>();
            case EffectType::LimiterEffect:      return std::make_unique<Limiter>();
            case EffectType::FilterEffectType:   return std::make_unique<FilterEffect>();
            case EffectType::EnsembleEffect:     return std::make_unique<Ensemble>();
            case EffectType::AutoPanEffect:      return std::make_unique<AutoPan>();
            case EffectType::RotaryEffect:       return std::make_unique<RotarySpeaker>();
            case EffectType::WaveshaperEffect:   return std::make_unique<Waveshaper>();
            case EffectType::RateReducerEffect:  return std::make_unique<RateReducer>();
            case EffectType::HallReverbEffect:   return std::make_unique<HallReverb>();
            case EffectType::SpringReverbEffect: return std::make_unique<SpringReverb>();
            case EffectType::ShimmerReverbEffect: return std::make_unique<ShimmerReverb>();
            case EffectType::ReverseDelayEffect: return std::make_unique<ReverseDelay>();
            case EffectType::TransientEffect:    return std::make_unique<TransientShaper>();
            case EffectType::RingModEffect: return std::make_unique<RingMod>();
            case EffectType::VibratoEffect: return std::make_unique<Vibrato>();
            case EffectType::WahEffect: return std::make_unique<Wah>();
            case EffectType::AmpEffect: return std::make_unique<AmpSim>();
            case EffectType::GraphicEQEffect: return std::make_unique<GraphicEQ>();
            case EffectType::TapeDelayEffect: return std::make_unique<TapeDelay>();
            case EffectType::LCRDelayEffect: return std::make_unique<LCRDelay>();
            case EffectType::MultiTapEffect: return std::make_unique<MultiTapInsert>();
            case EffectType::SidechainEffect: return std::make_unique<SidechainInsert>();
            case EffectType::VocoderEffect: return std::make_unique<Vocoder>();
            case EffectType::DirtInsertEffect: return std::make_unique<DirtInsert>();
            case EffectType::TalkInsertEffect: return std::make_unique<TalkInsert>();
            case EffectType::NumEffectTypes:     break;
        }
        return nullptr;
    }

    static const char* effectTypeName(EffectType type)
    {
        switch (type)
        {
            case EffectType::ChorusEffect:      return "CHR";
            case EffectType::DriveEffect:       return "DRV";
            case EffectType::FlangerEffect:     return "FLG";
            case EffectType::PhaserEffect:      return "PHS";
            case EffectType::TremoloEffect:     return "TRM";
            case EffectType::DelayEffect:       return "DLY";
            case EffectType::ReverbEffect:      return "RVB";
            case EffectType::EQEffect:          return "EQ";
            case EffectType::CompressorEffect:  return "CMP";
            case EffectType::LimiterEffect:     return "LMT";
            case EffectType::FilterEffectType:  return "FLT";
            case EffectType::EnsembleEffect:    return "ENS";
            case EffectType::AutoPanEffect:     return "PAN";
            case EffectType::RotaryEffect:      return "ROT";
            case EffectType::WaveshaperEffect:  return "WSH";
            case EffectType::RateReducerEffect: return "RED";
            case EffectType::HallReverbEffect:  return "HALL";
            case EffectType::SpringReverbEffect: return "SPRG";
            case EffectType::ShimmerReverbEffect: return "SHIM";
            case EffectType::ReverseDelayEffect: return "RVSE";
            case EffectType::TransientEffect:   return "TRNS";
            case EffectType::RingModEffect: return "RING";
            case EffectType::VibratoEffect: return "VIB";
            case EffectType::WahEffect: return "WAH";
            case EffectType::AmpEffect: return "AMP";
            case EffectType::GraphicEQEffect: return "GEQ";
            case EffectType::TapeDelayEffect: return "TAPE";
            case EffectType::LCRDelayEffect: return "LCR";
            case EffectType::MultiTapEffect: return "MTAP";
            case EffectType::SidechainEffect: return "DUCK";
            case EffectType::VocoderEffect: return "VOC";
            case EffectType::DirtInsertEffect: return "DIRT";
            case EffectType::TalkInsertEffect: return "TALK";
            case EffectType::NumEffectTypes:    break;
        }
        return "---";
    }

    // ---- PERF: effects played from the pads (PerfFx.h) -----------------------------------------
    // A pad's effect: one of the insert types (kind = type + 1), or one of the pads' own.
    static std::unique_ptr<EffectBase> createPerfEffect(int kind)
    {
        switch (kind)
        {
            case PerfFx::DUB:  return std::make_unique<DubDelay>();
            case PerfFx::FRZ:  return std::make_unique<FreezeReverb>();
            case PerfFx::RPT:  return std::make_unique<BeatRepeat>();
            case PerfFx::STOP: return std::make_unique<TapeStop>();
            default: break;
        }
        const int type = kind - PerfFx::INSERT_BASE;
        // (DUCK is CMP with other cells, and CMP is on the list with the same KEY choice; VOC is
        // nothing without an input. A pad's effect is handed what its KEY cell names like any
        // insert, so VOC on a pad is possible now: offered to the owner, not built.)
        if (type < 0 || type >= (int) EffectType::NumEffectTypes || type == (int) EffectType::SidechainEffect
            || type == (int) EffectType::VocoderEffect)
            return nullptr;
        return createEffect((EffectType) type);
    }

    static const char* perfKindName(int kind)
    {
        switch (kind)
        {
            case PerfFx::NONE: return "--";
            case PerfFx::DUB:  return "DUB";
            case PerfFx::FRZ:  return "FRZ";
            case PerfFx::RPT:  return "RPT";
            case PerfFx::STOP: return "STOP";
            default: break;
        }
        const int type = kind - PerfFx::INSERT_BASE;
        return (type >= 0 && type < (int) EffectType::NumEffectTypes) ? effectTypeName((EffectType) type) : "--";
    }

    // The pads' own effects are made for one way of pressing. Of the insert types, those with a
    // tail (echoes and reverbs: all mix dry and wet as dry (1 - mix) + wet mix) can be thrown too.
    static PerfFx::Ways perfWays(int kind)
    {
        switch (kind)
        {
            case PerfFx::DUB: case PerfFx::FRZ:   return { false, true };
            case PerfFx::RPT: case PerfFx::STOP:  return { true, false };
            default: break;
        }
        switch ((EffectType) (kind - PerfFx::INSERT_BASE))
        {
            case EffectType::DelayEffect: case EffectType::ReverbEffect: case EffectType::HallReverbEffect:
            case EffectType::SpringReverbEffect: case EffectType::ShimmerReverbEffect: case EffectType::ReverseDelayEffect:
            case EffectType::TapeDelayEffect: case EffectType::LCRDelayEffect: case EffectType::MultiTapEffect:
                return { true, true };
            default: break;
        }
        return { true, false };
    }

    // The kinds in the order the FX knob offers them: nothing, the pads' own, then the insert types.
    static const std::vector<int>& perfKinds()
    {
        static const std::vector<int> kinds = []
        {
            std::vector<int> k { PerfFx::NONE, PerfFx::DUB, PerfFx::FRZ, PerfFx::RPT, PerfFx::STOP };
            for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
                if (t != (int) EffectType::SidechainEffect && t != (int) EffectType::VocoderEffect)
                    k.push_back(PerfFx::INSERT_BASE + t);
            return k;
        }();
        return kinds;
    }

    PerfFxRack& getPerfFx() { return perfFx; }

    // The pads as a new project has them, so that PERF plays at the first touch: the same eight
    // moves twice, on the whole mix (top row) and on the selected track (bottom row).
    void loadFactoryPerfPads()
    {
        using namespace PerfFx;
        auto insert = [](EffectType t) { return INSERT_BASE + (int) t; };
        const int kinds[8] = { DUB, FRZ, RPT, STOP, insert(EffectType::FilterEffectType), insert(EffectType::PhaserEffect),
                               insert(EffectType::RateReducerEffect), insert(EffectType::HallReverbEffect) };
        perfFx.silence();
        for (int pad = 0; pad < PADS; ++pad)
        {
            const int kind = kinds[pad % 8];
            perfFx.clear(pad);
            perfFx.setKind(pad, 0, kind);
            Setting s = perfFx.get(pad);
            Layer& p = s.layer[0];
            p.target = pad < 8 ? (int) MASTER : (int) SEL;
            if (kind == RPT)
                s.quantize = Q_16;
            if (kind == insert(EffectType::FilterEffectType))
            {
                // Open at rest; pressing harder closes it.
                p.cell[0] = 12000.0f;
                p.cell[1] = 0.45f;
                p.pressure[0] = -0.7f;
            }
            if (kind == insert(EffectType::PhaserEffect))
            {
                p.cell[1] = 0.2f;
                p.pressure[1] = 0.8f;
            }
            if (kind == insert(EffectType::RateReducerEffect))
            {
                p.cell[0] = 0.25f;
                p.pressure[0] = 0.6f;
            }
            perfFx.set(pad, s);
        }
    }
    const PerfFxRack& getPerfFx() const { return perfFx; }

    // ---- Structure lock -----------------------------------------------------------------------
    // Anything that builds or destroys objects the audio thread uses (effects, engines, the whole
    // project) holds this. The audio thread only ever tries it: if a rebuild is in progress it
    // plays one block of silence instead of touching half-built state. Re-entrant.
    using StructureLock = juce::CriticalSection::ScopedLockType;
    juce::CriticalSection& structure() { return structureMutex; }

    void addInsertEffect(int trackIndex, EffectType type)
    {
        const StructureLock lock(structureMutex);
        if (! isChainTrack(trackIndex))
            ensureInsertChains(trackIndex);
        auto* chain = getInsertChain(trackIndex);
        if (chain == nullptr || chain->getEffectCount() >= MAX_INSERTS_PER_TRACK)
            return;
        auto fx = createSlotEffect(trackIndex, type);
        if (!fx) return;
        if (currentSampleRate > 0)
            fx->prepare(currentSampleRate, currentBlockSize);
        chain->addEffect(std::move(fx), (int) type);
        declickTrack(trackIndex, false);
    }

    void removeInsertEffect(int trackIndex, int slot)
    {
        const StructureLock lock(structureMutex);
        auto* chain = getInsertChain(trackIndex);
        if (chain == nullptr)
            return;
        chain->removeEffect(slot);
        declickTrack(trackIndex, false);
    }

    // Put an effect of the given type into a slot: replaces what is there, or appends when the slot
    // is the first free one.
    void setInsertEffect(int trackIndex, int slot, EffectType type)
    {
        const StructureLock lock(structureMutex);
        if (! isChainTrack(trackIndex))
            ensureInsertChains(trackIndex);
        auto* chain = getInsertChain(trackIndex);
        if (chain == nullptr) return;
        if (slot >= chain->getEffectCount())
        {
            addInsertEffect(trackIndex, type);
            return;
        }
        if (slot < 0) return;
        auto fx = createSlotEffect(trackIndex, type);
        if (!fx) return;
        chain->replaceEffect(slot, std::move(fx), (int) type);
        declickTrack(trackIndex, false);
    }

    // What is on a track is about to be cut or swapped between two blocks (its engine replaced or
    // silenced, an effect put in, taken out or exchanged): the jump is carried off over 2 ms
    // instead of being heard as a click. engineToo: the jump is in the engine's own output.
    void declickTrack(int trackIndex, bool engineToo)
    {
        if (isChainTrack(trackIndex))     // (a send's sound has no stage of its own: the output's)
            masterDeclickArm.store(true, std::memory_order_relaxed);
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
            return;
        trackDeclickArm[(size_t) trackIndex].store(true, std::memory_order_relaxed);
        if (engineToo)
            engineDeclickArm[(size_t) trackIndex].store(true, std::memory_order_relaxed);
    }

    // Type of the effect in a slot; false when the slot is empty. (The chain knows what it holds:
    // EffectChain::getKind, said when the effect was put in.)
    bool getInsertEffectType(int trackIndex, int slot, EffectType& typeOut) const
    {
        const auto* chain = getInsertChain(trackIndex);
        const int kind = chain != nullptr ? chain->getKind(slot) : EffectChain::NO_KIND;
        if (kind < 0 || kind >= (int) EffectType::NumEffectTypes) return false;
        typeOut = static_cast<EffectType>(kind);
        return true;
    }

    // A track's insert chain (null until the track has had an insert), with MASTER_TRACK the
    // master chain, with sendTrack(bus) that send's chain (null: no such send).
    EffectChain* getInsertChain(int trackIndex)
    {
        if (trackIndex == MASTER_TRACK)
            return &masterChain;
        if (isSendTrack(trackIndex))
            return trackIndex - SEND_TRACK_BASE < (int) sendBuses.size() ? &sendBuses[(size_t) (trackIndex - SEND_TRACK_BASE)].getChain() : nullptr;
        if (trackIndex < 0 || trackIndex >= (int) trackInsertChains.size())
            return nullptr;
        return &trackInsertChains[(size_t) trackIndex];
    }

    const EffectChain* getInsertChain(int trackIndex) const
    {
        return const_cast<AudioEngine*>(this)->getInsertChain(trackIndex);
    }

    // ---- Track parameters --------------------------------------------------------------------
    // One entry point for every per-track parameter: mixer ids go to the mixer, effect ids to the
    // insert chain, everything else to the track's engine. Screens and the sequencer use this
    // rather than talking to engines directly.
    void setTrackParameter(int trackIndex, int id, float value)
    {
        applyTrackParameter(trackIndex, id, value);

        // If a step lock is currently overriding this parameter, the new value becomes what it
        // returns to (otherwise the next step would undo the edit).
        if (trackIndex >= 0 && trackIndex < MAX_TRACKS)
        {
            auto& locks = activeLocks[(size_t) trackIndex];
            for (int i = 0; i < activeLockCount[(size_t) trackIndex]; ++i)
                if (locks[(size_t) i].id == id)
                    locks[(size_t) i].base = value;
        }
    }

    // Move a parameter without changing what it returns to: used while a pad's pressure holds it
    // somewhere inside its range. setTrackParameter with the stored value puts it back.
    void modulateTrackParameter(int trackIndex, int id, float value)
    {
        applyTrackParameter(trackIndex, id, value);
    }

    float getTrackParameter(int trackIndex, int id) const
    {
        if (isChainTrack(trackIndex))
            return isInsertParam(id) ? getInsertParameter(trackIndex, id) : 0.0f;
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
            return 0.0f;

        if (MixParam::isMixParam(id))
        {
            auto& m = trackMix[(size_t) trackIndex];
            if (id == MixParam::VOLUME) return m.volume.load();
            if (id == MixParam::PAN) return m.pan.load();
            return m.sends[(size_t) (id - MixParam::SEND_BASE)].load();
        }

        // Ids 240-263 (the old TrackFx blocks) are no longer a fixed section: they read 0 for
        // backward compat. Projects that stored them are loaded by the converter in ProjectBridge.
        if (TrackFxParam::isTrackFxParam(id))
            return 0.0f;

        if (isArpLockParam(id))
            return arpLockValues[(size_t) trackIndex][(size_t) (id - ARP_LOCK_BASE)];

        if (TrackInParam::contains(id))
        {
            const auto* value = trackIn[(size_t) trackIndex].valueOf(id);
            return value != nullptr ? value->load(std::memory_order_relaxed) : 0.0f;
        }

        if (isInsertParam(id))
            return getInsertParameter(trackIndex, id);
        if (id >= EFFECT_PARAM_BASE && id < EngineParamRange::THIRD_BASE)     // (the rest of the block: nobody's; 400 up is the engine's third block)
            return 0.0f;

        if (auto* e = getEngine(trackIndex))
            return e->getParameter(id);
        return 0.0f;
    }

    // Called at each trig: parameters locked on the previous trig go back to their base values
    // unless this step locks them again, then this step's locks are applied.
    void applyStepLocks(int trackIndex, const Step& step)
    {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
            return;

        auto& locks = activeLocks[(size_t) trackIndex];
        int& count = activeLockCount[(size_t) trackIndex];

        int kept = 0;
        for (int i = 0; i < count; ++i)
        {
            if (step.paramLocks.find(locks[(size_t) i].id) == step.paramLocks.end())
                applyTrackParameter(trackIndex, locks[(size_t) i].id, locks[(size_t) i].base);
            else
                locks[(size_t) kept++] = locks[(size_t) i];
        }
        count = kept;

        const auto* engine = getEngine(trackIndex);
        for (auto& [id, value] : step.paramLocks)
        {
            // A cell whose change rebuilds data (a sample swap, a re-cut table) is no lock: the
            // screen offers none (ParamPageScreen::canLock), and a pattern that holds one all the
            // same (a file, an older build) does not rebuild tables on the audio thread.
            if (engine != nullptr && EngineParamRange::contains(id) && engine->isRebuildParam(id))
                continue;

            bool known = false;
            for (int i = 0; i < count; ++i)
                known = known || locks[(size_t) i].id == id;

            if (!known && count < MAX_ACTIVE_LOCKS)
                locks[(size_t) count++] = { id, getTrackParameter(trackIndex, id) };

            applyTrackParameter(trackIndex, id, value);
        }
    }

    // The value a parameter returns to once the lock that holds it is over; with no lock holding
    // it, the parameter itself. (setTrackParameter would also move a locked parameter now.)
    void setTrackParameterBase(int trackIndex, int id, float value)
    {
        if (trackIndex >= 0 && trackIndex < MAX_TRACKS)
        {
            auto& locks = activeLocks[(size_t) trackIndex];
            for (int i = 0; i < activeLockCount[(size_t) trackIndex]; ++i)
                if (locks[(size_t) i].id == id)
                {
                    locks[(size_t) i].base = value;
                    return;
                }
        }
        applyTrackParameter(trackIndex, id, value);
    }

    // ================= Knob movements while recording =============================================
    // A lockable knob turned while the sequencer records and plays is written into the pattern: a
    // parameter lock with the knob's value on every step the track passes, from the step of the
    // first turn for one lap of the track (so the movement loops as it was made, and a knob left
    // somewhere stays there until the lap is full). A step without a trig becomes a trig without
    // notes that only carries the lock. When the lap is full or recording ends, the value the
    // track had before is its own value again: steps the lap did not reach go back to it.
    // Called from the screens (motionTouch) and the 30 Hz refresh (motionPoll), both holding the
    // screens' lock; never from the audio thread. The recording pass's undo step covers the locks.
    struct Motion
    {
        int track = -1, id = -1;
        float value = 0.0f, base = 0.0f;
        int stepsLeft = 0, lastStep = -1;
    };
    static constexpr int MAX_MOTIONS = 16;

    bool isMotionRecording() const { return sequencer != nullptr && sequencer->isRecording() && sequencer->isPlaying(); }

    // `base`: the track's own value of the parameter before this turn.
    void motionTouch(int trackIndex, int id, float value, float base)
    {
        if (!isMotionRecording() || trackIndex < 0 || trackIndex >= sequencer->getPattern().getTrackCount())
            return;
        Motion* m = nullptr;
        for (auto& x : motions)
            if (x.track == trackIndex && x.id == id) m = &x;
        if (m == nullptr)
        {
            for (auto& x : motions)
                if (m == nullptr && x.track < 0) m = &x;
            if (m == nullptr)
                return;                     // sixteen knobs in motion at once: this one is not recorded
            const auto& track = sequencer->getPattern().getTrack(trackIndex);
            const int length = juce::jmax(1, track.length);
            const int now = juce::jlimit(0, length - 1, sequencer->getCurrentStep(trackIndex));
            *m = { trackIndex, id, value, base, length, (now + length - 1) % length };
        }
        else
        {
            // Still on the step written last: it takes the newest value of the turn.
            auto& track = sequencer->getPattern().getTrack(trackIndex);
            if (m->lastStep >= 0 && m->lastStep < track.length
                && m->lastStep == juce::jlimit(0, juce::jmax(1, track.length) - 1, sequencer->getCurrentStep(trackIndex)))
                track.getStep(m->lastStep).paramLocks[id] = value;
        }
        m->value = value;
        motionPoll();
    }

    void motionPoll()
    {
        if (sequencer == nullptr)
            return;
        const bool recordingNow = isMotionRecording();
        for (auto& m : motions)
        {
            if (m.track < 0)
                continue;
            if (m.track >= sequencer->getPattern().getTrackCount())
            {
                m = Motion();
                continue;
            }
            auto& track = sequencer->getPattern().getTrack(m.track);
            const int length = juce::jmax(1, track.length);
            if (recordingNow)
            {
                const int now = juce::jlimit(0, length - 1, sequencer->getCurrentStep(m.track));
                int behind = ((now - m.lastStep) % length + length) % length;
                while (behind-- > 0 && m.stepsLeft > 0)
                {
                    m.lastStep = (m.lastStep + 1) % length;
                    auto& step = track.getStep(m.lastStep);
                    step.paramLocks[m.id] = m.value;
                    if (!step.trig)
                    {
                        step.notes.clear();
                        step.trig = true;
                    }
                    --m.stepsLeft;
                }
            }
            if (!recordingNow || m.stepsLeft <= 0)
            {
                track.soundParams[m.id] = m.base;
                setTrackParameterBase(m.track, m.id, m.base);
                m = Motion();
            }
        }
    }

    bool isMotionActive(int trackIndex, int id) const
    {
        for (auto& m : motions)
            if (m.track == trackIndex && m.id == id) return true;
        return false;
    }

    // Send every stored parameter of a track to its engine / mixer / effects (after a preset or
    // project load, or an engine change).
    void applyAllTrackParameters(int trackIndex, const ParamMap& params)
    {
        if (trackIndex >= 0 && trackIndex < MAX_TRACKS)
            activeLockCount[(size_t) trackIndex] = 0;

        int masterId = -1;
        if (auto* e = getEngine(trackIndex))
            masterId = e->getMasterParamId();
        if (masterId >= 0)
        {
            auto it = params.find(masterId);
            if (it != params.end())
                applyTrackParameter(trackIndex, masterId, it->second);
        }

        for (auto& [id, value] : params)
            if (id != masterId)
                applyTrackParameter(trackIndex, id, value);
    }

    // The track's effect blocks, for display; null until the track has an engine.

    // ================= Pad pressure routing ========================================================
    // A track's pressure routes are cells of its engine's router (PressureSlotParam, ids 773-784;
    // ModRouter.h: THE PRESSURE SLOTS). They are part of the track's sound: set through
    // setTrackParameter like any cell (AT ROUTE does), captured with the sound, and an engine is
    // made with its defaults in them (the level on the synths, EXC on the resonator, DEC on a drum
    // kit, nothing on the sampler), so nothing here has to seed or copy them.
    // What follows is the routes as they were BEFORE the slots had cells - one AftertouchRouter a
    // track, a Destination and a depth a route - for the projects that store them so, and for
    // reading the slots that way.
    using PressureDest = AftertouchRouter::Destination;
    struct PressureDestInfo { PressureDest dest; const char* label; };

    // The destinations AT ROUTE offered each engine then (OFF not included), with the labels the
    // engine's own pages use: a stored route outside this list was, and is, loaded as OFF.
    // Returns how many.
    static int pressureDestinations(EngineType type, const PressureDestInfo*& list)
    {
        using D = PressureDest;
        static const PressureDestInfo unified[] = { { D::Volume, "LEVL" }, { D::FilterCutoff, "CUT" }, { D::Vibrato, "VIB" },
                                                    { D::MacroTimbre, "PW" }, { D::WavePosition, "MIX" }, { D::FMDepth, "XMOD" } };
        static const PressureDestInfo fm[]      = { { D::Volume, "LEVL" }, { D::FilterCutoff, "CUT" }, { D::FMDepth, "FM" } };
        static const PressureDestInfo macro[]   = { { D::Volume, "LEVL" }, { D::FilterCutoff, "CUT" }, { D::Vibrato, "VIB" },
                                                    { D::MacroTimbre, "TIMB" }, { D::WavePosition, "MORP" }, { D::Excite, "BOW" } };
        static const PressureDestInfo resonator[] = { { D::Excite, "EXC" }, { D::Volume, "LEVL" }, { D::FilterCutoff, "CUT" },
                                                      { D::Vibrato, "VIB" } };
        static const PressureDestInfo table[]   = { { D::Volume, "LEVL" }, { D::FilterCutoff, "CUT" }, { D::Vibrato, "VIB" },
                                                    { D::WavePosition, "POS" } };
        static const PressureDestInfo waveSeq[] = { { D::Volume, "LEVL" }, { D::FilterCutoff, "CUT" } };
        static const PressureDestInfo drum[]    = { { D::DrumDecay, "DEC" }, { D::DrumPitch, "TUNE" }, { D::FilterCutoff, "TONE" },
                                                    { D::Volume, "LEVL" }, { D::DrumDrive, "DRIV" }, { D::DrumSnap, "SNAP" },
                                                    { D::PanSpread, "PAN" } };
        switch (type)
        {
            case EngineType::UnifiedVoice: list = unified; return 6;
            case EngineType::FM:           list = fm;      return 3;
            case EngineType::MacroOsc:     list = macro;   return 6;
            case EngineType::Wavetable:    list = table;   return 4;
            case EngineType::WaveSeq:      list = waveSeq; return 2;
            case EngineType::Drum:         list = drum;    return 7;
            case EngineType::Resonator:    list = resonator; return 4;
            case EngineType::Sampler:                                   // had no route then
            case EngineType::MIDITrack:                                 // sent on as poly pressure
            default:                       list = nullptr; return 0;
        }
    }

    static bool pressureDestinationValid(EngineType type, PressureDest dest)
    {
        if (dest == PressureDest::Off) return true;
        const PressureDestInfo* list = nullptr;
        const int n = pressureDestinations(type, list);
        for (int i = 0; i < n; ++i)
            if (list[i].dest == dest) return true;
        return false;
    }

    // How many routes a track of this engine had then: a drum kit one destination for all its pads.
    static int pressureRouteCount(EngineType type)
    {
        const PressureDestInfo* list = nullptr;
        if (pressureDestinations(type, list) == 0) return 0;
        return type == EngineType::Drum ? 1 : AftertouchRouter::MAX_ROUTES;
    }

    // How many pressure slots AT ROUTE shows for a track of this engine now: a drum kit has one
    // (it goes to every pad), a MIDI track none (its pressure is sent on as poly pressure).
    static int pressureSlotCount(EngineType type)
    {
        return type == EngineType::MIDITrack ? 0 : (type == EngineType::Drum ? 1 : (int) mod::Router::NUM_PRESSURE);
    }

    // What a new track of this engine does with pressure, as routes: the level on the synths,
    // decay on a drum kit, the exciter on the resonator, nothing where the engine had no response.
    // (The engines are made so: DestTable::pressureRoute in each engine's table.)
    static AftertouchRouter defaultPressureRouter(EngineType type)
    {
        AftertouchRouter r;
        r.clear();
        if (type == EngineType::Drum)
            r.setRoute(0, { PressureDest::DrumDecay, 1.0f, false });
        else if (type == EngineType::Resonator)
            r.setRoute(0, { PressureDest::Excite, 1.0f, false });
        else if (pressureRouteCount(type) > 0)
            r.setRoute(0, { PressureDest::Volume, 1.0f, false });
        return r;
    }

    EngineType getEngineTypeOf(int trackIndex) const
    {
        auto* e = getEngine(trackIndex);
        return e != nullptr ? e->getType() : EngineType::UnifiedVoice;
    }

    // One pressure slot of a track, by its cells (dest: a value of the slot's DEST).
    void setPressureSlot(int trackIndex, int slot, int dest, float amount, bool bipolar)
    {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS || slot < 0 || slot >= mod::Router::NUM_PRESSURE) return;
        setTrackParameter(trackIndex, PressureSlotParam::id(slot, PressureSlotParam::DEST_OFFSET), (float) dest);
        setTrackParameter(trackIndex, PressureSlotParam::id(slot, PressureSlotParam::AMT_OFFSET), amount);
        setTrackParameter(trackIndex, PressureSlotParam::id(slot, PressureSlotParam::BIPOLAR_OFFSET), bipolar ? 1.0f : 0.0f);
    }

    // An old route into a track's slot. A destination AT ROUTE did not offer the track's engine
    // then becomes OFF, as it did; the rest is what the engine says the Destination is
    // (EngineBase::pressureDestOf).
    void setPressureRoute(int trackIndex, int routeIndex, AftertouchRouter::Route route)
    {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
        auto* e = getEngine(trackIndex);
        if (e == nullptr || e->modRouter() == nullptr) return;
        const auto type = e->getType();
        // (a drum kit had one route: what a file holds beyond it never played)
        if (! pressureDestinationValid(type, route.dest) || routeIndex >= juce::jmax(1, pressureRouteCount(type)))
            route.dest = PressureDest::Off;
        e->setPressureRoute(routeIndex, route);
    }

    // All of a track's old routes (a project stored before the slots had cells).
    void setPressureRouter(int trackIndex, const AftertouchRouter& routes)
    {
        for (int i = 0; i < AftertouchRouter::MAX_ROUTES; ++i)
            setPressureRoute(trackIndex, i, routes.effectiveRoute(i));
    }

    // The slots read as old routes: for the list a project still carries beside the cells, and for
    // whoever asks "what does pressure do here" in those words. A slot aimed at something no
    // Destination names (RESO, an effect's cell) reads OFF: the cells are the truth.
    AftertouchRouter getPressureRouter(int trackIndex) const
    {
        AftertouchRouter out;
        out.clear();
        auto* e = getEngine(trackIndex);
        const auto* r = e != nullptr ? e->modRouter() : nullptr;
        if (r == nullptr) return out;
        const auto type = e->getType();
        for (int i = 0; i < AftertouchRouter::MAX_ROUTES; ++i)
        {
            const auto slot = r->pressureSlot(i);
            auto dest = e->pressureDestAsRoute(slot.dest, [type](PressureDest d) { return pressureDestinationValid(type, d); });
            if (dest == PressureDest::Off)
                dest = e->pressureDestAsRoute(slot.dest);
            if (dest != PressureDest::Off)
                out.setRoute(i, { dest, slot.amount, slot.bipolar });
        }
        return out;
    }

    // The slots as the track's engine is made with them.
    void resetPressureRouter(int trackIndex)
    {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
        auto* e = getEngine(trackIndex);
        const auto* r = e != nullptr ? e->modRouter() : nullptr;
        if (r == nullptr) return;
        for (int p = 0; p < mod::Router::NUM_PRESSURE; ++p)
        {
            const auto def = r->dests().pressureDefault(p);
            e->setParameter(PressureSlotParam::id(p, PressureSlotParam::DEST_OFFSET), (float) def.dest);
            e->setParameter(PressureSlotParam::id(p, PressureSlotParam::AMT_OFFSET), def.amount);
            e->setParameter(PressureSlotParam::id(p, PressureSlotParam::BIPOLAR_OFFSET), 0.0f);
        }
    }

    // ---- Recorded pressure -> parameter locks -------------------------------------------------------
    // The page cell a pressure destination moves on this track (the cell with the destination's
    // label; on a drum kit the cell of the pad `note` plays). False when the engine has no such
    // cell (vibrato, for one): that route cannot become a lock.
    bool pressureLockCell(int trackIndex, PressureDest dest, int note, ParamSpec& cell) const
    {
        auto* e = getEngine(trackIndex);
        if (e == nullptr) return false;
        const PressureDestInfo* list = nullptr;
        const int n = pressureDestinations(e->getType(), list);
        const char* label = nullptr;
        for (int i = 0; i < n; ++i)
            if (list[i].dest == dest) label = list[i].label;
        if (label == nullptr) return false;

        const auto pages = e->getParamPages();
        const bool drum = e->getType() == EngineType::Drum;
        if (drum && (note < 0 || note >= (int) pages.size())) return false;
        for (size_t pg = drum ? (size_t) note : 0; pg < (drum ? (size_t) note + 1 : pages.size()); ++pg)
            for (auto& spec : pages[pg].params)
                if (spec.isValid() && !spec.isDiscrete() && juce::String(spec.label) == label && !e->isRebuildParam(spec.id))
                {
                    cell = spec;
                    return true;
                }
        return false;
    }

    // The same for a pressure slot's DEST as it stands (a value of the slot's list): the cell an
    // old route to it locked, or else the page cell the destination itself is (a slot aimed at
    // RESO, at a macro's cell ...). False: nothing of it is a cell (VIB, EXC, PAN on a synth, an
    // effect's cell).
    bool pressureLockCellOf(int trackIndex, int slotDest, int note, ParamSpec& cell) const
    {
        auto* e = getEngine(trackIndex);
        const auto* r = e != nullptr ? e->modRouter() : nullptr;
        if (r == nullptr || slotDest <= 0) return false;
        const auto type = e->getType();
        const auto route = e->pressureDestAsRoute(slotDest, [type](PressureDest d) { return pressureDestinationValid(type, d); });
        if (route != PressureDest::Off && pressureLockCell(trackIndex, route, note, cell))
            return true;
        // (also where the route's label is not the cell's: the macro oscillator names TIMB by its model)
        if (type == EngineType::Drum || slotDest >= r->dests().size())
            return false;       // (a kit's cells are a pad's own: only the seven above are known by pad)
        const int id = r->dests()[slotDest].id;
        if (id < 0 || e->isRebuildParam(id)) return false;
        for (auto& page : e->getParamPages())
            if (const auto* spec = page.find(id); spec != nullptr && spec->isValid() && ! spec->isDiscrete())
            {
                cell = *spec;
                return true;
            }
        return false;
    }

    // Turns the pressure recorded on a step into parameter locks, through the track's pressure
    // slots as they are now: on each step the note spans, each routed cell is locked to where the
    // lane's mean pressure over that step would hold it (the slot's amount is how far towards the
    // cell's top, or bottom when negative, full pressure goes). Steps without a trig become lock
    // trigs: a trig with no note. Several notes on a synth step share the cells, so their lanes are
    // averaged; drum pads have their own cells. The lanes are then removed. Nothing is changed
    // when no route can be locked. Not the audio thread.
    struct PressureBake { int routes = 0, locked = 0, steps = 0; };
    PressureBake bakePressureToLocks(int trackIndex, Track& track, int stepIndex)
    {
        PressureBake result;
        if (stepIndex < 0 || stepIndex >= track.length) return result;
        auto& source = track.getStep(stepIndex);
        if (!source.hasPressure()) return result;

        const auto type = getEngineTypeOf(trackIndex);
        auto* engine = getEngine(trackIndex);
        const auto* slots = engine != nullptr ? engine->modRouter() : nullptr;
        if (slots == nullptr) return result;
        const int span = juce::jlimit(1, track.length, (int) std::ceil(source.length - 0.001f));
        const bool drum = type == EngineType::Drum;

        for (int r = 0; r < pressureSlotCount(type); ++r)
        {
            const auto route = slots->pressureSlot(r);
            if (route.dest <= 0 || route.amount == 0.0f) continue;
            ++result.routes;
            bool locked = false;
            // One pass per cell: a drum pad's own, or the one all the synth's notes share.
            for (size_t first = 0; first < source.notes.size(); ++first)
            {
                if (source.notes[first].lane.empty()) continue;
                ParamSpec cell;
                if (!pressureLockCellOf(trackIndex, route.dest, source.notes[first].pitch, cell)) { if (drum) continue; break; }
                locked = true;
                const auto stored = track.soundParams.find(cell.id);
                const float base = cell.clamp(stored != track.soundParams.end() ? stored->second
                                                                                : getTrackParameter(trackIndex, cell.id));
                track.soundParams[cell.id] = base;      // what the cell returns to when a lock is removed
                const float baseNorm = cell.toNorm(base);
                for (int k = 0; k < span; ++k)
                {
                    const float to = juce::jmin((float) k + 1.0f, juce::jmax(source.length, (float) k + 0.01f));
                    float mean = 0.0f;
                    int lanes = 0;
                    for (size_t i = first; i < source.notes.size(); ++i)
                        if (!source.notes[i].lane.empty())
                        {
                            mean += source.notes[i].lane.meanOver((float) k, to);
                            ++lanes;
                            if (drum) break;
                        }
                    mean = juce::jlimit(0.0f, 1.0f, mean / (float) juce::jmax(1, lanes) * source.pressureScale);
                    const float mod = juce::jlimit(-1.0f, 1.0f, (route.bipolar ? mean * 2.0f - 1.0f : mean) * route.amount);
                    const float norm = mod >= 0.0f ? baseNorm + mod * (1.0f - baseNorm) : baseNorm + mod * baseNorm;
                    auto& target = track.getStep(stepIndex + k);
                    target.paramLocks[cell.id] = cell.fromNorm(norm);
                    if (!target.trig)
                    {
                        target.notes.clear();
                        target.trig = true;
                    }
                }
                if (!drum) break;
            }
            if (locked) ++result.locked;
        }

        if (result.locked > 0)
        {
            result.steps = span;
            source.clearPressureLanes();
        }
        return result;
    }

    // The sequencer's recorded pad pressure goes to the note's voice like a live pad's (the app
    // replaces this callback with one that also follows the song's transpose).
    void setSequencer(SequencerCore* seq)
    {
        sequencer = seq;
        if (seq != nullptr)
            seq->setPressureCallback([this](int trackIndex, int note, float pressure) { lanePressure(trackIndex, note, pressure); });
    }

    // Recorded pressure for one note of a track (audio thread): to that note's voice through the
    // track's pressure routes, or out as poly pressure on a MIDI track. While a pad is playing the
    // same note on the same track the pad's own pressure wins and this is ignored.
    void lanePressure(int trackIndex, int note, float pressure)
    {
        for (int pad = 0; pad < PadRatchet::kNumPads; ++pad)
            if (padSounding[(size_t) pad] == note && padTrack[(size_t) pad] == trackIndex)
                return;
        if (auto* e = getEngine(trackIndex))
            e->aftertouch(note, pressure);
    }
    void setArpeggiator(Arpeggiator* arp) { arpeggiator = arp; }

    // Everything that makes notes by itself, run once per block before the engines render: the
    // arpeggiators and the pad ratchet.
    bool generatorsSawTransport = false;
    void processGenerators(int numSamples)
    {
        const double sr = currentSampleRate > 0 ? currentSampleRate : 44100.0;
        const float tempo = sequencer ? sequencer->getTempo() : 120.0f;
        if (arpeggiator && arpeggiator->isEnabled())
            arpeggiator->processBlock(numSamples, sr, tempo);
        // While the sequencer runs, the arpeggiators' steps sit on its grid; otherwise they free-run.
        const bool running = sequencer != nullptr && sequencer->isPlaying();
        const double beat = running ? sequencer->getBeatPosition() : 0.0;
        const bool justStarted = running && !generatorsSawTransport;   // Play restarts every arp's pattern
        generatorsSawTransport = running;
        for (auto* arp : trackArps)
            if (arp != nullptr && arp != arpeggiator && arp->isEnabled())
            {
                if (justStarted)
                    arp->resetPhase();
                if (running)
                    arp->syncToTransport(beat);
                arp->processBlock(numSamples, sr, tempo);
            }
        padRatchet.processBlock(numSamples, sr, tempo);

        // Pads whose pressure is being recorded with their note: this block's reading.
        if (sequencer != nullptr)
            for (int pad = 0; pad < PadRatchet::kNumPads; ++pad)
                if (sequencer->isCapturingPressure(pad))
                {
                    if (padRatchet.isRolling(pad)) sequencer->endPressureCapture(pad, false);
                    else                           sequencer->setCapturePressure(pad, padRatchet.getPressure(pad));
                }
    }

    // ---- Pads that play notes (drum pads, scale pads) --------------------------------------------
    // They go through the pad ratchet: with ROLL off it simply passes the note on; with ROLL on a
    // held pad retriggers at a rate set by its pressure. `pad` is 0..15 and identifies the finger;
    // the note and track are remembered for the release. Call from the MIDI thread only.
    void padNoteOn(int trackIndex, int pad, int note, float velocity)
    {
        if (pad < 0 || pad >= PadRatchet::kNumPads) return;
        padTrack[(size_t) pad] = trackIndex;
        padRatchet.padDown(pad, note, velocity);
    }
    void padPressure(int pad, float pressure) { padRatchet.setPressure(pad, pressure); }
    void padNoteOff(int pad) { padRatchet.padUp(pad); }

    PadRatchet& getPadRatchet() { return padRatchet; }

    // Called (audio thread) for every note a pad produces, including each retrigger: for recording.
    std::function<void(int trackIndex, int note, float velocity)> onPadHit;

    // One arpeggiator per track (owned by the app). All enabled ones run every block, so a latched
    // arpeggio keeps playing on its track while another track is selected.
    void setTrackArpeggiator(int trackIndex, Arpeggiator* arp)
    {
        if (trackIndex >= 0 && trackIndex < MAX_TRACKS)
            trackArps[(size_t) trackIndex] = arp;
    }
    Arpeggiator* getTrackArpeggiator(int trackIndex) const
    {
        return (trackIndex >= 0 && trackIndex < MAX_TRACKS) ? trackArps[(size_t) trackIndex] : nullptr;
    }

    // Arpeggiator parameter locks as track parameters (so a step can lock them like anything else):
    // ARP_LOCK_BASE + 0 gate, + 1 velocity, + 2 ratchet count. A negative value (ratchet: below 1)
    // means "not locked".
    static constexpr int ARP_LOCK_BASE = 270;
    static constexpr int NUM_ARP_LOCKS = 3;
    static bool isArpLockParam(int id) { return id >= ARP_LOCK_BASE && id < ARP_LOCK_BASE + NUM_ARP_LOCKS; }

    // Insert slots' on/off and wet amount as track parameters (so projects keep them and steps can
    // lock them): INSERT_ON_BASE + slot, INSERT_WET_BASE + slot.
    static constexpr int INSERT_ON_BASE = 280;
    static constexpr int INSERT_WET_BASE = 288;
    // INSERT_SYNC_BASE + slot: the effect's TIME / RATE follows the tempo; INSERT_DIV_BASE + slot: which division.
    static constexpr int INSERT_SYNC_BASE = 296;
    static constexpr int INSERT_DIV_BASE = 304;
    static bool isInsertSwitchParam(int id) { return id >= INSERT_ON_BASE && id < INSERT_DIV_BASE + MAX_INSERTS_PER_TRACK; }
    // INSERT_EXTRA_BASE + slot * 4 + n: the effect's cells beyond its four (EffectBase::getExtraSpec; TRK FX page FX2).
    static constexpr int INSERT_EXTRA_BASE = 312;
    static bool isInsertExtraParam(int id) { return id >= INSERT_EXTRA_BASE && id < INSERT_EXTRA_BASE + MAX_INSERTS_PER_TRACK * EffectBase::MAX_EXTRAS; }
    // Any id of an insert slot (its four cells, its switches, its extra cells): what a chain without a track answers to.
    static bool isInsertParam(int id)
    {
        return isInsertSwitchParam(id) || isInsertExtraParam(id)
            || (id >= EFFECT_PARAM_BASE && id < EFFECT_PARAM_BASE + MAX_INSERTS_PER_TRACK * 4);
    }

    // ---- Who listens to whom ------------------------------------------------------------------
    // The stored parameter id that holds the KEY / SRC choice for a given effect type in a given
    // insert slot, or -1 if the type has no KEY cell. Used by MultiPreset to remap every keyed
    // effect (CMP's KEY, DUCK's SRC, TRNS's KEY, WAH's KEY, every LFO effect's KEY, and
    // FilterEffect's KEY), not only DUCK's SRC.
    static int effectKeyParamId(EffectType type, int slot)
    {
        // Cache: computed once per effect type, stored as the cell index (0-7) or -1.
        static std::array<int, (size_t) EffectType::NumEffectTypes> cache;
        static bool cached = false;
        if (! cached)
        {
            cache.fill(-1);
            for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
            {
                auto fx = createEffect((EffectType) t);
                if (! fx) continue;
                // Check the main cells (0-3) and the extras (4-7) for a keyNames() spec.
                for (int c = 0; c < 4; ++c)
                {
                    auto spec = fx->getParamSpec(c);
                    if (spec.isValid() && spec.choices == dyn::keyNames())
                    { cache[(size_t) t] = c; break; }
                }
                if (cache[(size_t) t] < 0)
                {
                    for (int c = 0; c < EffectBase::MAX_EXTRAS; ++c)
                    {
                        auto spec = fx->getExtraSpec(c);
                        if (spec.isValid() && spec.choices == dyn::keyNames())
                        { cache[(size_t) t] = 4 + c; break; }
                    }
                }
            }
            cached = true;
        }
        const int cell = cache[(size_t) type];
        if (cell < 0 || slot < 0 || slot >= MAX_INSERTS_PER_TRACK) return -1;
        return cell < 4 ? EFFECT_PARAM_BASE + slot * 4 + cell
                        : INSERT_EXTRA_BASE + slot * 4 + (cell - 4);
    }

    // All tracks listened to by ANYTHING: every track's IN source and the keySource() of every
    // insert, plus the master chain's, every send bus's and every PERF pad's. A bitmask (bit t set
    // = track t is listened to): the full trackFeeds mask. MIDI tracks are excluded: a muted MIDI
    // track sends its notes to an instrument outside, which no fader here could shut. Audio thread.
    uint32_t allListenedTo() const
    {
        uint32_t mask = 0;
        auto mark = [&](int src, int self)
        {
            if (src >= 0 && src != self && src < MAX_TRACKS && src < (int) engines.size() && engines[(size_t) src]
                && engines[(size_t) src]->getType() != EngineType::MIDITrack)
                mask |= (1u << (uint32_t) src);
        };
        for (int t = 0; t < (int) engines.size() && t < MAX_TRACKS; ++t)
        {
            if (! engines[(size_t) t]) continue;
            mark(trackInputSource(t), t);
            if (t < (int) trackInsertChains.size())
            {
                auto& chain = trackInsertChains[(size_t) t];
                for (int i = 0; i < chain.getEffectCount(); ++i)
                    mark(chain.getEffect(i)->keySource(), t);
            }
        }
        auto keyFeeds = [&mark](const EffectChain& chain, int self)
        {
            for (int i = 0; i < chain.getEffectCount(); ++i)
                mark(chain.getEffect(i)->keySource(), self);
        };
        keyFeeds(masterChain, -1);
        for (auto& bus : sendBuses)
            keyFeeds(bus.getChain(), -1);
        perfFx.eachKeyTrack(mark);
        return mask;
    }

    void addEngine(int trackIndex, std::shared_ptr<EngineBase> engine)
    {
        const StructureLock lock(structureMutex);
        if (trackIndex >= (int) engines.size())
            engines.resize((size_t) trackIndex + 1);
        engines[(size_t) trackIndex] = std::move(engine);
        adoptUserShape(trackIndex);
    }

    EngineBase* getEngine(int trackIndex) const
    {
        if (trackIndex < 0 || trackIndex >= (int) engines.size())
            return nullptr;
        return engines[(size_t) trackIndex].get();
    }

    static std::shared_ptr<EngineBase> createEngine(EngineType type, int maxVoices = 8)
    {
        switch (type)
        {
            case EngineType::FM:
                return std::make_shared<FMEngine>(maxVoices);
            case EngineType::MacroOsc:
                return std::make_shared<MacroOscEngine>(maxVoices);
            case EngineType::Wavetable:
                return std::make_shared<WavetableEngine>(maxVoices);
            case EngineType::WaveSeq:
                return std::make_shared<WaveSeqEngine>(maxVoices);
            case EngineType::Sampler:
                return std::make_shared<SamplerEngine>(maxVoices);
            case EngineType::Drum:
                return std::make_shared<DrumEngine>();
            case EngineType::MIDITrack:
                return std::make_shared<MIDITrackEngine>();
            case EngineType::Resonator:
                return std::make_shared<ResonatorEngine>(maxVoices);
            case EngineType::UnifiedVoice:
            default:
                return std::make_shared<UnifiedVoiceEngine>(maxVoices);
        }
    }

    // force: build a fresh engine even when the type is unchanged (loading a preset starts from
    // the engine's defaults, not from whatever was dialled in before).
    void setEngineType(int trackIndex, EngineType type, bool force = false)
    {
        const StructureLock lock(structureMutex);
        if (trackIndex < 0 || trackIndex >= (int) engines.size())
            return;

        auto& current = engines[(size_t) trackIndex];
        if (current && current->getType() == type && !force)
            return;

        auto newEngine = createEngine(type);
        if (currentSampleRate > 0)
            newEngine->prepareToPlay(currentSampleRate, currentBlockSize);
        // (A new engine has its own default pressure routes in its slots: they are part of the
        // sound, and whoever loads one sets them with the rest of its parameters.)
        newEngine->setTempo(lastTempo);

        // The audio thread may still be inside the old engine: keep it alive for a while.
        if (current)
        {
            retiredEngines.push_back(current);
            if (retiredEngines.size() > 16)
                retiredEngines.erase(retiredEngines.begin());
        }
        engines[(size_t) trackIndex] = std::move(newEngine);
        adoptUserShape(trackIndex);
        declickTrack(trackIndex, true);

        // The old engine's stored parameters mean nothing to the new one (every engine has its
        // own ids). Keep only the mixer and insert-effect values.
        if (trackIndex < MAX_TRACKS)
            activeLockCount[(size_t) trackIndex] = 0;
        if (sequencer && trackIndex < sequencer->getPattern().getTrackCount())
        {
            auto& params = sequencer->getPattern().getTrack(trackIndex).soundParams;
            for (auto it = params.begin(); it != params.end();)
            {
                // Engine parameters, and the pressure ranges set on them.
                const bool engineParam = EngineParamRange::contains(it->first);
                const bool engineRange = RangeParam::isRangeParam(it->first)
                                         && EngineParamRange::contains(RangeParam::targetOf(it->first));
                it = (engineParam || engineRange) ? params.erase(it) : std::next(it);
            }
        }
    }

    // PRESET audition: a fresh engine of `type` takes the track and the engine that was there is
    // handed back, silenced but otherwise untouched, to be returned with unparkEngine. Unlike
    // setEngineType nothing stored is changed: the caller owns Track::soundParams. (The pressure
    // routes are the engines' own cells: the fresh one has its defaults, the parked one keeps its.)
    std::shared_ptr<EngineBase> parkEngine(int trackIndex, EngineType type)
    {
        const StructureLock lock(structureMutex);
        if (trackIndex < 0 || trackIndex >= (int) engines.size())
            return nullptr;
        auto parked = engines[(size_t) trackIndex];
        auto fresh = createEngine(type);
        if (currentSampleRate > 0)
            fresh->prepareToPlay(currentSampleRate, currentBlockSize);
        if (trackIndex < MAX_TRACKS)
            activeLockCount[(size_t) trackIndex] = 0;
        fresh->setTempo(lastTempo);
        if (parked)
            parked->allNotesOff();
        engines[(size_t) trackIndex] = std::move(fresh);
        adoptUserShape(trackIndex);
        declickTrack(trackIndex, true);
        return parked;
    }

    // The parked engine takes its track back; the engine that stood in for it is retired.
    void unparkEngine(int trackIndex, std::shared_ptr<EngineBase> parked)
    {
        const StructureLock lock(structureMutex);
        if (!parked || trackIndex < 0 || trackIndex >= (int) engines.size())
            return;
        auto& current = engines[(size_t) trackIndex];
        if (current)
        {
            current->allNotesOff();
            retiredEngines.push_back(current);
            if (retiredEngines.size() > 16)
                retiredEngines.erase(retiredEngines.begin());
        }
        if (trackIndex < MAX_TRACKS)
            activeLockCount[(size_t) trackIndex] = 0;
        parked->setTempo(lastTempo);
        parked->allNotesOff();
        engines[(size_t) trackIndex] = std::move(parked);
        adoptUserShape(trackIndex);
        declickTrack(trackIndex, true);
    }

    int getSendCount() const { return (int) sendBuses.size(); }

    // One more send bus with one effect on it (-1: there are MAX_SENDS already), and one taken
    // away. What is on a bus is its chain's: add / set / removeInsertEffect with sendTrack(bus).
    int addSendBus(EffectType type)
    {
        const StructureLock lock(structureMutex);
        if ((int) sendBuses.size() >= MAX_SENDS) return -1;
        sendBuses.emplace_back();
        int idx = (int) sendBuses.size() - 1;
        sendBuses[(size_t) idx].setSendLevel(1.0f);
        if (currentSampleRate > 0)
            sendBuses[(size_t) idx].prepare(currentSampleRate, currentBlockSize);
        addInsertEffect(sendTrack(idx), type);
        return idx;
    }

    void removeSendBus(int index)
    {
        const StructureLock lock(structureMutex);
        if (index < 0 || index >= (int) sendBuses.size()) return;
        sendBuses.erase(sendBuses.begin() + index);
        declickTrack(sendTrack(index), false);
    }

    // The type of a send's first effect (what the TRACK page's BUS cells show and choose); a send
    // with nothing on it: `none`.
    EffectType getSendBusType(int index, EffectType none = EffectType::ReverbEffect) const
    {
        EffectType type = none;
        getInsertEffectType(sendTrack(index), 0, type);
        return type;
    }

    // The sends as a new project has them: two, a reverb (DCAY 0.6, DAMP 0.4) and a delay
    // (0.375 s, FDBK 0.35), each alone on its bus with its mix at 1.
    void resetSendBuses()
    {
        const StructureLock lock(structureMutex);
        sendBuses.resize(DEFAULT_NUM_SENDS);
        buildDefaultSends();
        for (auto& bus : sendBuses)
            if (currentSampleRate > 0)
                bus.prepare(currentSampleRate, currentBlockSize);
        declickTrack(sendTrack(0), false);
    }
    // The type a new project's send holds (a file from before the sends stored their chains names
    // one type a bus: that type here means the bus as a new project has it).
    static EffectType defaultSendType(int bus) { return bus == SEND_DELAY ? EffectType::DelayEffect : EffectType::ReverbEffect; }

    SendBus& getSendBus(int index) { return sendBuses[index]; }
    const SendBus& getSendBus(int index) const { return sendBuses[index]; }

    // The first effect of a type in the master chain (the MASTR page's EQ, CMP and LMT are
    // these), and its slot; null / -1 when the chain holds none.
    int getMasterSlot(EffectType type) const
    {
        for (int i = 0; i < masterChain.getEffectCount(); ++i)
            if (masterChain.getKind(i) == (int) type) return i;
        return -1;
    }
    EffectBase* getMasterEffect(EffectType type)
    {
        const int slot = getMasterSlot(type);
        return slot >= 0 ? masterChain.getEffect(slot) : nullptr;
    }
    // The master chain as a new project has it: EQ (flat), CMP (0 dB, 1:1: nothing), LMT.
    void resetMasterChain()
    {
        const StructureLock lock(structureMutex);
        buildDefaultMasterChain();
        declickTrack(MASTER_TRACK, false);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        prepare(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
    }

    // Everything the audio callback needs sized and ready. Tests call this, then renderBlock.
    void prepare(double sampleRate, int blockSize)
    {
        currentSampleRate = sampleRate;
        currentBlockSize = blockSize;

        if (midiClock)
            midiClock->prepareToPlay(currentSampleRate, currentBlockSize);

        if (sequencer)
            sequencer->prepareToPlay(currentSampleRate, currentBlockSize);

        for (auto& e : engines)
            if (e) e->prepareToPlay(currentSampleRate, currentBlockSize);

        scratchL.resize((size_t) currentBlockSize, 0.0f);
        trackMidi.ensureSize(2048);
        resampler.prepare(currentSampleRate);
        metronome.prepare(currentSampleRate);
        trackTapL.assign((size_t) MAX_TRACKS, std::vector<float>((size_t) currentBlockSize, 0.0f));
        trackTapR.assign((size_t) MAX_TRACKS, std::vector<float>((size_t) currentBlockSize, 0.0f));
        for (int t = 0; t < MAX_TRACKS; ++t)
        {
            tapLeft[(size_t) t] = trackTapL[(size_t) t].data();
            tapRight[(size_t) t] = trackTapR[(size_t) t].data();
        }
        scratchR.resize((size_t) currentBlockSize, 0.0f);
        trackInL.assign((size_t) MAX_TRACKS, std::vector<float>((size_t) currentBlockSize, 0.0f));
        trackInR.assign((size_t) MAX_TRACKS, std::vector<float>((size_t) currentBlockSize, 0.0f));
        for (auto& in : trackIn)
        {
            in.follower.reset();
            in.setRate = 0.0f;          // the follower's times are made again for this sample rate
            in.lastSource = -1;
            in.lastLevel = in.lastMix = in.lastRing = 0.0f;
        }

        for (auto& chain : trackInsertChains)
            chain.prepare(currentSampleRate, currentBlockSize);
        masterChain.prepare(currentSampleRate, currentBlockSize);

        for (auto& bus : sendBuses)
            bus.prepare(currentSampleRate, currentBlockSize);
        {
            const StructureLock lock(structureMutex);
            perfFx.prepare(currentSampleRate, currentBlockSize);
        }

        for (auto& pair : engineDeclick)
            for (auto& d : pair) d.setSampleRate(currentSampleRate);
        for (auto& pair : trackDeclick)
            for (auto& d : pair) d.setSampleRate(currentSampleRate);
        for (auto& d : masterDeclick)
            d.setSampleRate(currentSampleRate);
    }

    void audioDeviceStopped() override
    {
        for (auto& e : engines)
            if (e) e->releaseResources();
    }

    // ---- Resampling ---------------------------------------------------------------------------
    Resampler& getResampler() { return resampler; }
    // The click (SETTINGS -> CLICK). It is added to the output last of all: see Metronome.h.
    Metronome& getMetronome() { return metronome; }

    // A finished take becomes a new sample in the bank (message thread). Mono when both channels
    // are the same. Returns the bank index, or -1 when there is nothing to keep.
    int commitResample(bool normalise, bool writeFile = true)
    {
        if (! resampler.isFinished())
            return -1;
        const int n = resampler.length();
        int index = -1;
        if (n > 0)
        {
            std::vector<float> l(resampler.left(), resampler.left() + n), r(resampler.right(), resampler.right() + n);
            float peak = 0.0f;
            bool stereo = false;
            for (int i = 0; i < n; ++i)
            {
                peak = std::max(peak, std::max(std::abs(l[(size_t) i]), std::abs(r[(size_t) i])));
                stereo = stereo || std::abs(l[(size_t) i] - r[(size_t) i]) > 1.0e-5f;
            }
            if (peak > 1.0e-5f)
            {
                // Normalised: peak at 0.9. Otherwise only turned down if it would clip.
                const float gain = normalise ? 0.9f / peak : (peak > 1.0f ? 1.0f / peak : 1.0f);
                for (int i = 0; i < n; ++i) { l[(size_t) i] *= gain; r[(size_t) i] *= gain; }
                index = SampleBank::get().addRecording(l.data(), stereo ? r.data() : nullptr, n,
                                                       resampler.getSampleRate(), writeFile);
            }
        }
        resampler.release();
        return index;
    }

    // One block of the whole instrument into two buffers (at most the prepared block size).
    void renderBlock(float* left, float* right, int numSamples)
    {
        float* out[2] = { left, right };
        audioDeviceIOCallbackWithContext(nullptr, 0, out, 2, juce::jmin(numSamples, currentBlockSize), {});
    }

    void audioDeviceIOCallbackWithContext(
        const float* const* /*inputChannelData*/,
        int /*numInputChannels*/,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& /*context*/) override
    {
        if (numOutputChannels < 2) return;

        // Everything below is sized for the prepared block. A device may hand over more than it
        // announced (JUCE does not promise the size): a longer block is rendered in pieces, and
        // one that comes before prepare() is silence.
        const int maxBlock = (int) juce::jmin(scratchL.size(), scratchR.size());
        if (numSamples > maxBlock)
        {
            if (maxBlock <= 0)
            {
                juce::FloatVectorOperations::clear(outputChannelData[0], juce::jmax(0, numSamples));
                juce::FloatVectorOperations::clear(outputChannelData[1], juce::jmax(0, numSamples));
                return;
            }
            for (int done = 0; done < numSamples; done += maxBlock)
            {
                float* part[2] = { outputChannelData[0] + done, outputChannelData[1] + done };
                audioDeviceIOCallbackWithContext(nullptr, 0, part, 2, juce::jmin(maxBlock, numSamples - done), {});
            }
            return;
        }

        // A rebuild is in progress on another thread: one silent block, then carry on.
        const juce::CriticalSection::ScopedTryLockType structureLock(structureMutex);
        if (! structureLock.isLocked())
        {
            juce::FloatVectorOperations::clear(outputChannelData[0], numSamples);
            juce::FloatVectorOperations::clear(outputChannelData[1], numSamples);
            // Not cut off: what was sounding is carried down to the silence over 2 ms, and the
            // sound is carried back up from it when it returns.
            for (int c = 0; c < 2; ++c)
            {
                if (! masterSkipped)
                    masterDeclick[(size_t) c].arm();
                masterDeclick[(size_t) c].process(outputChannelData[c], numSamples);
            }
            masterSkipped = true;
            skippedBlocks.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Shift + Play asked for silence: every note is let go and every effect's memory is emptied
        // (send buses, inserts), here on the audio thread, between two blocks.
        if (silenceRequested.exchange(false, std::memory_order_relaxed))
        {
            for (auto& d : masterDeclick)
                d.arm();        // the output is shut from this block on: faded, not cut
            silenceNow();
            // What a voice still gives while it stops would start the effects ringing again: the
            // output stays shut for a quarter of a second and the effects are emptied once more.
            silenceSamplesLeft = (int) (0.25 * currentSampleRate);
        }

        float* mainL = outputChannelData[0];
        float* mainR = outputChannelData[1];

        juce::FloatVectorOperations::clear(mainL, numSamples);
        juce::FloatVectorOperations::clear(mainR, numSamples);

        // Where the sequencer is at the first sample of this block (for a bar-started resample).
        const bool seqPlaying = sequencer != nullptr && sequencer->isPlaying();
        const double beatAtStart = sequencer != nullptr ? sequencer->getBeatPosition() : 0.0;
        const double beatsPerSample = (sequencer != nullptr ? (double) sequencer->getTempo() : 120.0) / 60.0
                                      / (currentSampleRate > 0.0 ? currentSampleRate : 44100.0);

        processGenerators(numSamples);
        perfFx.beginBlock({ sequencer != nullptr ? sequencer->getTempo() : 120.0f, beatAtStart, beatsPerSample, seqPlaying }, numSamples);

        if (midiClock)
            midiClock->processBlock(numSamples, currentSampleRate);
        if (midiClock && sequencer)
            followExternalClock();

        // Which tracks anything listens to (track input, keyed inserts on any chain or PERF pad).
        // Such a track goes on playing while muted; its fader is shut instead (below), so it feeds.
        const uint32_t listenMask = allListenedTo();
        for (int t = 0; t < MAX_TRACKS; ++t)
            trackFeeds[(size_t) t] = (listenMask & (1u << (uint32_t) t)) != 0;
        if (sequencer)
            for (int t = 0; t < MAX_TRACKS; ++t)
                sequencer->setTrackFeeds(t, trackFeeds[(size_t) t]);

        if (sequencer)
        {
            const float tempo = sequencer->getTempo();
            if (tempo != lastTempo)
            {
                lastTempo = tempo;
                for (auto& e : engines)
                    if (e) e->setTempo(tempo);
            }
            sequencer->processBlock(numSamples);

            // The beats of this block, for the click; a count-in that has just ended starts the
            // MIDI clock as Play would have.
            metronome.beginBlock(sequencer->isRunning());
            const bool recording = sequencer->isRecording();
            for (int i = 0; i < sequencer->getBeatTickCount(); ++i)
            {
                const auto& tick = sequencer->getBeatTick(i);
                metronome.beat(tick.sample, tick.accent, tick.countIn, recording);
            }
            if (sequencer->consumeCountInStart() && midiClock)
                midiClock->startTransport();
        }
        else
            metronome.beginBlock(false);

        for (auto& bus : sendBuses)
            bus.clearBuffer();

        // What an effect may listen to beside its own sound (EffectBase::keySource): any track's
        // sound after its inserts. The mix and the sends are handed this; a track's inserts and
        // its PERF pads the same with the track's input and the track's own number.
        KeyFeed masterKeys;
        if ((int) trackTapL.size() >= MAX_TRACKS && (int) trackTapL[0].size() >= numSamples)
        {
            masterKeys.tapLeft = tapLeft.data();
            masterKeys.tapRight = tapRight.data();
            masterKeys.taps = MAX_TRACKS;
            masterKeys.frames = numSamples;
        }

        for (int t = 0; t < (int) engines.size(); ++t)
        {
            auto& e = engines[(size_t) t];
            if (!e) continue;

            std::fill(scratchL.begin(), scratchL.begin() + numSamples, 0.0f);
            std::fill(scratchR.begin(), scratchR.begin() + numSamples, 0.0f);

            float* scratch[2] = { scratchL.data(), scratchR.data() };
            juce::AudioBuffer<float> trackBuf(scratch, 2, numSamples);
            trackMidi.clear();

            // The track's input: its source's sound (after that track's inserts, before its fader:
            // this block's from a lower-numbered track, the block before's from a higher one).
            const TrackInput in = t < MAX_TRACKS ? readTrackInput(t, numSamples) : TrackInput();
            e->setAudioInput(in.view);
            e->processBlock(trackBuf, trackMidi);

            if (t < MAX_TRACKS)
            {
                const int jump = e->takeJump();
                if (engineDeclickArm[(size_t) t].exchange(false, std::memory_order_relaxed) || jump == EngineBase::SoundCut)
                    for (auto& d : engineDeclick[(size_t) t])
                        d.armTail();
                else if (jump == EngineBase::SoundSwapped)
                    for (auto& d : engineDeclick[(size_t) t])
                        d.arm();
                engineDeclick[(size_t) t][0].process(scratchL.data(), numSamples);
                engineDeclick[(size_t) t][1].process(scratchR.data(), numSamples);
            }

            // The last line: a number that is not one (a voice gone wrong) must not get into the
            // memories of the effects, the sends or the speakers. The block is dropped and the
            // engine's notes are let go. Counted, so that it is seen (getNonFiniteBlocks).
            if (droppedUnlessFinite(scratchL.data(), scratchR.data(), numSamples))
            {
                e->allNotesOff();
                nonFiniteTrack.store(t, std::memory_order_relaxed);
            }

            if (!trackMidi.isEmpty() && midiOutput)
                midiOutput->sendBlockOfMessagesNow(trackMidi);

            // MODE of the input: RING multiplies what the engine made by it, MIX adds it. Both
            // before the track's effects, so the track is a bus for its source.
            if (t < MAX_TRACKS)
                mixTrackInput(t, in, scratchL.data(), scratchR.data(), numSamples);

            // What the engine's router does to the cells of the track's effects (E4), before they run.
            if (t < MAX_TRACKS)
                moveTrackEffects(t, *e);

            // What an effect on this track may listen to (EffectBase::keySource): the track's
            // input, or a track's sound after its inserts (this block's from a lower-numbered
            // track, the block before's from a higher one).
            KeyFeed keys = masterKeys;
            keys.inLeft = in.view.left;
            keys.inRight = in.view.right;
            keys.inFrames = in.view.frames;
            keys.self = t;

            bool worked = false;
            if (t < (int) trackInsertChains.size() && trackInsertChains[(size_t) t].getEffectCount() > 0)
            {
                auto& chain = trackInsertChains[(size_t) t];
                prepareChainBlock(chain, sequencer ? sequencer->getTempo() : 120.0f, e->getLastNoteHz());
                chain.processStereo(scratchL.data(), scratchR.data(), numSamples, &keys);
                worked = true;
            }

            // A block or an insert gone wrong: the block is dropped and the chain's memory emptied
            // before it can reach the tap that other tracks listen to, the sends or the sum.
            if (worked && droppedUnlessFinite(scratchL.data(), scratchR.data(), numSamples))
            {
                if (t < (int) trackInsertChains.size())
                    trackInsertChains[(size_t) t].reset();
                nonFiniteTrack.store(t + 1000, std::memory_order_relaxed);
            }
            if (t < (int) trackTapL.size() && (int) trackTapL[(size_t) t].size() >= numSamples)
            {
                std::copy(scratchL.begin(), scratchL.begin() + numSamples, trackTapL[(size_t) t].begin());
                std::copy(scratchR.begin(), scratchR.begin() + numSamples, trackTapR[(size_t) t].begin());
            }
            if (t < MAX_TRACKS)
            {
                if (trackDeclickArm[(size_t) t].exchange(false, std::memory_order_relaxed))
                    for (auto& d : trackDeclick[(size_t) t])
                        d.arm();
                trackDeclick[(size_t) t][0].process(scratchL.data(), numSamples);
                trackDeclick[(size_t) t][1].process(scratchR.data(), numSamples);
            }
            // Resampling one track: after its effects, before its fader and pan.
            if (resampler.getSource() == t)
                resampler.process(scratchL.data(), scratchR.data(), numSamples, beatAtStart, beatsPerSample, seqPlaying);

            // Track mixer: volume and pan (balance law, unity at centre), ramped across the block.
            if (t < MAX_TRACKS)
            {
                auto& m = trackMix[(size_t) t];
                // A track that another listens to plays on while muted (it still feeds): here is
                // where it falls silent, after the tap its listeners take.
                const bool shut = trackFeeds[(size_t) t] && sequencer != nullptr && ! sequencer->isTrackAudible(t);
                const float vol = shut ? 0.0f : m.volume.load();
                const float pan = juce::jlimit(-1.0f, 1.0f, m.pan.load());
                const float targetL = vol * (pan > 0.0f ? 1.0f - pan : 1.0f);
                const float targetR = vol * (pan < 0.0f ? 1.0f + pan : 1.0f);
                float gL = m.lastGainL, gR = m.lastGainR;
                const float stepL = (targetL - gL) / (float) numSamples;
                const float stepR = (targetR - gR) / (float) numSamples;
                for (int i = 0; i < numSamples; ++i)
                {
                    gL += stepL;
                    gR += stepR;
                    scratchL[(size_t) i] *= gL;
                    scratchR[(size_t) i] *= gR;
                }
                m.lastGainL = targetL;
                m.lastGainR = targetR;

                int numSends = juce::jmin((int) sendBuses.size(), MixParam::MAX_SENDS);
                for (int s = 0; s < numSends; ++s)
                {
                    // A send level that was moved is ramped across the block, like the fader.
                    const float sendLevel = m.sends[(size_t) s].load();
                    const float from = m.lastSend[(size_t) s];
                    m.lastSend[(size_t) s] = sendLevel;
                    if (from != sendLevel)
                        sendBuses[s].addInputRamped(scratchL.data(), scratchR.data(), numSamples, from, sendLevel);
                    else if (sendLevel > 0.0f)
                        sendBuses[s].addInput(scratchL.data(), scratchR.data(), numSamples, sendLevel);
                }
            }

            // The PERF pads that act on this track: after its fader and its sends.
            perfFx.process(t, scratchL.data(), scratchR.data(), numSamples, &keys);

            juce::FloatVectorOperations::add(mainL, scratchL.data(), numSamples);
            juce::FloatVectorOperations::add(mainR, scratchR.data(), numSamples);
        }

        // The mix and the sends have no input of their own; what an effect on them listens to is
        // a track's sound of this block (a compressor keyed from the kick: the whole mix ducks
        // under it). A send's chain is an insert chain run on what the tracks sent; a send with
        // nothing on it adds nothing (not a second copy of the dry sound).
        for (size_t b = 0; b < sendBuses.size(); ++b)
        {
            auto& bus = sendBuses[b];
            if (bus.getChain().getEffectCount() == 0)
                continue;
            prepareChainBlock(bus.getChain(), sequencer ? sequencer->getTempo() : 120.0f, 0.0f);
            bus.process(&masterKeys, numSamples);       // (the samples played, not the prepared length)
            // A send gone wrong is dropped on its own bus, its chain emptied: the tracks play on,
            // and nothing of it reaches the sum or stays in the chain's memory.
            if (droppedUnlessFinite(bus.left(), bus.right(), juce::jmin(numSamples, bus.frames())))
            {
                bus.getChain().reset();
                nonFiniteTrack.store(2000 + (int) b, std::memory_order_relaxed);
            }
            bus.mixInto(mainL, mainR, numSamples);
        }

        // The PERF pads that act on the whole mix: before the master chain.
        perfFx.process(PerfFx::ON_MASTER, mainL, mainR, numSamples, &masterKeys);

        // The master chain: an insert chain like a track's, a block at a time (as a new project
        // has it: EQ, compressor, limiter).
        prepareChainBlock(masterChain, sequencer ? sequencer->getTempo() : 120.0f, 0.0f);
        masterChain.processStereo(mainL, mainR, numSamples, &masterKeys);

        for (int i = 0; i < numSamples; ++i)
        {
            mainL[i] *= masterLevel;
            mainR[i] *= masterLevel;
        }

        // The same last line for the sum (a send or the master section gone wrong): silence, and
        // the master section's memory emptied.
        if (droppedUnlessFinite(mainL, mainR, numSamples))
        {
            masterChain.reset();
            for (auto& bus : sendBuses)
                bus.getChain().reset();
        }

        if (silenceSamplesLeft > 0)
        {
            juce::FloatVectorOperations::clear(mainL, numSamples);
            juce::FloatVectorOperations::clear(mainR, numSamples);
            silenceSamplesLeft -= numSamples;
            if (silenceSamplesLeft <= 0)
                silenceNow();
        }

        // Where the output was shut (above) or a block was skipped (a rebuild), the jump to and
        // from silence is carried off over 2 ms. Costs nothing while neither happens.
        if (masterDeclickArm.exchange(false, std::memory_order_relaxed) || masterSkipped)
        {
            masterSkipped = false;
            for (auto& d : masterDeclick)
                d.arm();
        }
        masterDeclick[0].process(mainL, numSamples);
        masterDeclick[1].process(mainR, numSamples);

        if (resampler.getSource() == Resampler::MASTER)
            resampler.process(mainL, mainR, numSamples, beatAtStart, beatsPerSample, seqPlaying);

        // The click goes in last: past the master effects, the limiter and the master volume (it
        // cannot pump the mix), past the silence above and past the resampler (never in a sample).
        // Only while it sounds, the sum is kept within full scale.
        if (metronome.isSounding())
        {
            metronome.render(mainL, mainR, numSamples);
            juce::FloatVectorOperations::clip(mainL, mainL, -1.0f, 1.0f, numSamples);
            juce::FloatVectorOperations::clip(mainR, mainR, -1.0f, 1.0f, numSamples);
        }

        if (midiClock && midiOutput)
        {
            MIDIClock::ClockEvent evt;
            while (midiClock->popOutputEvent(evt))
            {
                switch (evt.type)
                {
                    case MIDIClock::ClockEvent::Clock:
                        midiOutput->sendMessageNow(juce::MidiMessage(0xF8));
                        break;
                    case MIDIClock::ClockEvent::Start:
                        midiOutput->sendMessageNow(juce::MidiMessage(0xFA));
                        break;
                    case MIDIClock::ClockEvent::Stop:
                        midiOutput->sendMessageNow(juce::MidiMessage(0xFC));
                        break;
                    case MIDIClock::ClockEvent::Continue:
                        midiOutput->sendMessageNow(juce::MidiMessage(0xFB));
                        break;
                    case MIDIClock::ClockEvent::SongPosition:
                        midiOutput->sendMessageNow(
                            juce::MidiMessage::songPositionPointer(evt.position));
                        break;
                }
            }
        }
    }

    // The master section as eight values (the MASTR page): 0 low gain, 1 mid gain, 2 mid frequency,
    // 3 high gain (EQ); 4 threshold, 5 ratio, 6 make-up (compressor); 7 ceiling (limiter).
    static constexpr int NUM_MASTER_PARAMS = 8;

    // Each is a cell of the first EQ, CMP or LMT in the master chain (a view of the chain: the
    // same cells are reached with setTrackParameter(MASTER_TRACK, EFFECT_PARAM_BASE + ...)); a
    // chain without that effect has no such value.
    // What each is at rest: a project without stored values, or a chain without the effect.
    static constexpr float MASTER_DEFAULTS[NUM_MASTER_PARAMS] = { 0, 0, 1000, 0, 0, 1, 0, -0.3f };
    struct MasterCell { EffectType type; int cell; };
    static MasterCell masterCell(int index)
    {
        switch (index)
        {
            case 0: case 1: case 2: case 3: return { EffectType::EQEffect, index };
            case 4: return { EffectType::CompressorEffect, 0 };     // THR
            case 5: return { EffectType::CompressorEffect, 1 };     // RATI
            case 6: return { EffectType::CompressorEffect, 3 };     // MAKE
            case 7: return { EffectType::LimiterEffect, 0 };        // CEIL
            default: return { EffectType::NumEffectTypes, 0 };
        }
    }

    void setMasterParam(int index, float value)
    {
        const MasterCell c = masterCell(index);
        if (auto* fx = getMasterEffect(c.type))
            fx->setParameter(c.cell, value);
    }

    float getMasterParam(int index) const
    {
        const MasterCell c = masterCell(index);
        if (auto* fx = const_cast<AudioEngine*>(this)->getMasterEffect(c.type))
            return fx->getParameter(c.cell);
        return index >= 0 && index < NUM_MASTER_PARAMS ? MASTER_DEFAULTS[index] : 0.0f;
    }

    void setMasterLevel(float level) { if (std::isfinite(level)) masterLevel = juce::jlimit(0.0f, 1.0f, level); }   // (MVOL is 0..1; a file may hold anything)
    float getMasterLevel() const { return masterLevel; }

    void setMIDIOutput(juce::MidiOutput* output) { midiOutput = output; }
    juce::MidiOutput* getMIDIOutput() const { return midiOutput; }

    void setMIDIClock(MIDIClock* clock) { midiClock = clock; }

    // CLK EXT, or AUTO while a clock comes in: the external MIDI clock leads. Its Start (with the
    // first tick after it) and Stop move the transport, its tempo is the tempo, and the sequencer's
    // position is pulled onto the clock's, by 3 % of the tempo at most. Play on the controller still
    // starts and stops the sequencer by hand. Audio thread.
    void followExternalClock()
    {
        const bool following = midiClock->isFollowingExternal();
        const bool clockRuns = following && midiClock->isRunning() && midiClock->getExternalTickCount() > 0;
        if (clockRuns != extClockRan)
        {
            extClockRan = clockRuns;
            if (clockRuns)
            {
                if (! sequencer->isPlaying())
                    sequencer->start();
                extSeqPlayed = false;           // (the position is taken below)
            }
            else if (following)
                sequencer->stop();
        }
        if (! clockRuns)
            return;

        const double bpm = midiClock->getDetectedTempo();
        const bool playing = sequencer->isPlaying();
        if (playing && ! extSeqPlayed)
            extBeatOffset = midiClock->getExternalBeat() - sequencer->getBeatPosition();
        extSeqPlayed = playing;
        if (bpm <= 0.0)
            return;

        double pull = 0.0;
        if (playing)
        {
            const double error = (midiClock->getExternalBeat() - extBeatOffset) - sequencer->getBeatPosition();
            pull = juce::jlimit(-0.03, 0.03, error * 0.25);
        }
        const float target = (float) (bpm * (1.0 + pull));
        if (std::abs(target - sequencer->getTempo()) > 0.005f)
            sequencer->setTempo(target);
    }
    MIDIClock* getMIDIClock() const { return midiClock; }

private:
    void applyTrackParameter(int trackIndex, int id, float value)
    {
        if (trackIndex < 0 || RangeParam::isRangeParam(id))
            return;     // range ends are data for the parameter screen, not sound parameters
        if (isChainTrack(trackIndex) && ! isInsertParam(id))
            return;     // the master chain and the sends have insert slots and nothing else of a track

        // Not a number, or infinite (a damaged file, a lock made from one): no parameter has such a
        // value, and jlimit / std::clamp would hand a NaN on to the mixer or an engine.
        if (! std::isfinite(value))
            return;
        // (and nothing is larger than this: a whole-number cast further down stays defined)
        value = juce::jlimit(-1.0e9f, 1.0e9f, value);

        if (MixParam::isMixParam(id))
        {
            if (trackIndex >= MAX_TRACKS) return;
            auto& m = trackMix[(size_t) trackIndex];
            if (id == MixParam::VOLUME) m.volume = juce::jlimit(0.0f, 1.0f, value);
            else if (id == MixParam::PAN) m.pan = juce::jlimit(-1.0f, 1.0f, value);
            else m.sends[(size_t) (id - MixParam::SEND_BASE)] = juce::jlimit(0.0f, 1.0f, value);
            return;
        }

        // Ids 240-263 (the old TrackFx blocks) are no longer a fixed section: ignored.
        if (TrackFxParam::isTrackFxParam(id))
            return;

        // An insert slot's cell, switch or extra cell: of the track's chain, the mix's or a send's.
        if (isInsertParam(id))
        {
            setInsertParameter(trackIndex, id, value);
            return;
        }

        if (isArpLockParam(id))
        {
            if (trackIndex >= MAX_TRACKS) return;
            const int which = id - ARP_LOCK_BASE;
            arpLockValues[(size_t) trackIndex][(size_t) which] = value;
            if (auto* arp = trackArps[(size_t) trackIndex])
            {
                const auto param = static_cast<Arpeggiator::LockParam>(which);
                const bool off = (param == Arpeggiator::LockParam::Ratchet) ? value < 0.5f : value < 0.0f;
                if (off) arp->clearParamLock(param);
                else     arp->setParamLock(param, value);
            }
            return;
        }

        // The track's input (TRK FX page IN): kept here, read by the audio callback.
        if (TrackInParam::contains(id))
        {
            if (trackIndex >= MAX_TRACKS) return;
            if (auto* stored = trackIn[(size_t) trackIndex].valueOf(id))
                stored->store(TrackIn::held(id, value), std::memory_order_relaxed);
            return;
        }

        if (! EngineParamRange::contains(id))
            return;   // (nobody's: the rest of the effect block, a screen's own cell)

        if (auto* e = getEngine(trackIndex))
        {
            e->setParameter(id, value);
            // A level of the track's custom LFO shape: the effects on the track play the same one.
            if (id >= LfoShapeParam::BASE && id < LfoShapeParam::END && trackIndex < MAX_TRACKS && trackHasShape[(size_t) trackIndex])
            {
                trackShape[(size_t) trackIndex][(size_t) (id - LfoShapeParam::BASE)] = juce::jlimit(-1.0f, 1.0f, e->getParameter(id));
                if (auto* chain = getInsertChain(trackIndex))
                    for (int s = 0; s < chain->getEffectCount(); ++s)
                        chain->getEffect(s)->setUserShape(trackShape[(size_t) trackIndex].data());
            }
        }
    }

    // ---- An insert slot's ids, on any chain ----------------------------------------------------
    // The one place where the ids of a slot (isInsertParam) reach an effect: its four cells
    // (EFFECT_PARAM_BASE + slot * 4 + n), ON / WET / SYNC / division (INSERT_ON_BASE ..), its cells
    // beyond four (INSERT_EXTRA_BASE + slot * 4 + n). The chain is a track's, the mix's or a send's.
    struct SlotId { int slot, part, cell; };        // part 0 cells, 1 ON, 2 WET, 3 SYNC, 4 division, 5 extras
    static SlotId slotIdOf(int id)
    {
        if (isInsertSwitchParam(id))
            return { (id - INSERT_ON_BASE) % MAX_INSERTS_PER_TRACK, 1 + (id - INSERT_ON_BASE) / MAX_INSERTS_PER_TRACK, 0 };
        if (isInsertExtraParam(id))
            return { (id - INSERT_EXTRA_BASE) / EffectBase::MAX_EXTRAS, 5, (id - INSERT_EXTRA_BASE) % EffectBase::MAX_EXTRAS };
        return { (id - EFFECT_PARAM_BASE) / 4, 0, (id - EFFECT_PARAM_BASE) % 4 };
    }

    void setInsertParameter(int trackIndex, int id, float value)
    {
        const SlotId at = slotIdOf(id);
        auto* chain = getInsertChain(trackIndex);
        auto* fx = chain != nullptr ? chain->getEffect(at.slot) : nullptr;
        if (fx == nullptr)
            return;
        switch (at.part)
        {
            case 0:  fx->setUserParameter(at.cell, value); break;
            case 1:  fx->setEnabled(value > 0.5f); break;
            case 2:  fx->setMix(juce::jlimit(0.0f, 1.0f, value)); break;
            case 3:  fx->setTempo(sequencer ? sequencer->getTempo() : 120.0f); fx->setSync(value > 0.5f); break;
            case 4:  fx->setSyncDivision((int) std::lround(value)); break;
            default: fx->setExtra(at.cell, value); break;
        }
    }

    float getInsertParameter(int trackIndex, int id) const
    {
        const SlotId at = slotIdOf(id);
        const auto* chain = getInsertChain(trackIndex);
        const auto* fx = chain != nullptr ? chain->getEffect(at.slot) : nullptr;
        if (fx == nullptr)      // an empty slot reads as a new effect would: on, all of it, not synced
            return at.part == 1 || at.part == 2 ? 1.0f : (at.part == 4 ? (float) FxSync::DEFAULT_DIVISION : 0.0f);
        switch (at.part)
        {
            case 0:  return fx->getUserParameter(at.cell);
            case 1:  return fx->isEnabled() ? 1.0f : 0.0f;
            case 2:  return fx->getMix();
            case 3:  return fx->isSynced() ? 1.0f : 0.0f;
            case 4:  return (float) fx->getSyncDivision();
            default: return fx->getExtra(at.cell);
        }
    }

    // An effect for a slot of a chain, from the one factory. On a track it plays the track's
    // custom LFO shape; on a send its mix starts at 1 (the bus gives the wet sound alone).
    std::unique_ptr<EffectBase> createSlotEffect(int trackIndex, EffectType type) const
    {
        auto fx = createEffect(type);
        if (fx == nullptr)
            return fx;
        fx->setUserShape(userShapeOf(trackIndex));
        if (isSendTrack(trackIndex))
            fx->setMix(1.0f);
        return fx;
    }

    // The sends as a new project has them (resetSendBuses; the constructor).
    void buildDefaultSends()
    {
        for (auto& bus : sendBuses)
        {
            while (bus.getChain().getEffectCount() > 0)
                bus.getChain().removeEffect(0);
            bus.setSendLevel(1.0f);
        }

        auto reverb = std::make_unique<Reverb>();
        reverb->setDecay(0.6f);
        reverb->setDamping(0.4f);
        reverb->setMix(1.0f);
        sendBuses[SEND_REVERB].getChain().addEffect(std::move(reverb), (int) EffectType::ReverbEffect);

        auto delay = std::make_unique<Delay>();
        delay->setTime(0.375f);
        delay->setFeedback(0.35f);
        delay->setMix(1.0f);
        sendBuses[SEND_DELAY].getChain().addEffect(std::move(delay), (int) EffectType::DelayEffect);
    }

    // ---- What a chain's effects are told before a block (audio thread) -----------------------
    // Tempo for synced times and rates, and the pitch of the note the track played last (VOC's
    // TUNE follows it; 0 = none: the master chain).
    static void prepareChainBlock(EffectChain& chain, float bpm, float noteHz)
    {
        for (int i = 0; i < chain.getEffectCount(); ++i)
            if (auto* fx = chain.getEffect(i))
            {
                fx->setTempo(bpm);
                fx->setPitch(noteHz);
            }
    }

    // ---- The track's effect cells as destinations of its engine's router (E4, audio thread) ----
    // A slot of the track's router (an LFO's DEST, a STRIP or XIN route, a free slot) may be aimed
    // at a cell of an effect on the track (mod::TrackDest: the eight insert slots' cells and WET).
    // After the engine's block the router holds one offset per such cell (Router::trackOffset: a
    // share of the cell's range, -1 .. 1); here it is handed to EffectBase::modulateCell, once a
    // block, before the effects run.
    // (mod::TrackDest still lists eight more after them, CH.R .. RO.D: the cells of the MODFX
    // block there once was. Nothing receives them - a slot aimed at one moves nothing - and they
    // stay in the lists until a stored DEST can be read without them: design debt.)
    // The effects glide to it (the chain's own mix reads the moved WET through wetWanted()).
    //   Let go, exactly once (offset 0): a cell no slot is aimed at any more (the slot was aimed
    //   elsewhere or emptied, the engine was exchanged for one whose slots are not, a project was
    //   applied); and an effect that is another one than the slot held when the offsets were given
    //   (exchanged, put in, or moved up because the one before it was taken out) is let go of
    //   whole when it is first met, so none carries an offset to a place nobody aims at.
    // A track with no slot on an effect cell, and none still held: one call and two tests.
    // (Sends and the master chain have no router: nothing moves their cells.)
    struct TrackMoves
    {
        mod::Mask held;         // the cells an offset was given to and not yet let go
        std::array<const EffectBase*, MAX_INSERTS_PER_TRACK> insert {};     // who was in each slot then (compared, never read through)
    };
    static_assert(mod::TrackDest::SLOTS == MAX_INSERTS_PER_TRACK, "an insert slot a group of destinations");
    static_assert(mod::TrackDest::CELLS == 4 + EffectBase::MAX_EXTRAS && mod::TrackDest::WET_CELL == EffectBase::WET_CELL,
                  "a track destination's cell is modulateCell's");

    void moveTrackEffects(int t, const EngineBase& engine)
    {
        using namespace mod;
        auto& moves = trackMoves[(size_t) t];
        const Router* router = engine.modRouter();
        if ((router == nullptr || ! router->trackDests().any()) && ! moves.held.any())
            return;
        const Mask used = router != nullptr ? router->trackDests() : Mask();

        EffectChain* chain = t < (int) trackInsertChains.size() ? &trackInsertChains[(size_t) t] : nullptr;
        for (int slot = 0; slot < TrackDest::SLOTS; ++slot)
        {
            EffectBase* fx = chain != nullptr ? chain->getEffect(slot) : nullptr;
            if (fx != moves.insert[(size_t) slot])
            {
                moves.insert[(size_t) slot] = fx;
                for (int cell = 0; cell < TrackDest::PER_SLOT; ++cell)
                {
                    if (fx != nullptr)
                        fx->modulateCell(cell, 0.0f);
                    moves.held.clear(TrackDest::ofInsert(slot, cell));
                }
            }
            if (fx == nullptr)
                continue;
            for (int cell = 0; cell < TrackDest::PER_SLOT; ++cell)
            {
                const int d = TrackDest::ofInsert(slot, cell);
                if (used.test(d))
                {
                    fx->modulateCell(cell, router->trackOffset(d));
                    moves.held.set(d);
                }
                else if (moves.held.test(d))
                {
                    fx->modulateCell(cell, 0.0f);
                    moves.held.clear(d);
                }
            }
        }
    }
    std::array<TrackMoves, MAX_TRACKS> trackMoves;

public:
    // What the display shows for a DEST that is a cell of an effect on the track (mod::TrackDest):
    // the slot's number and the cell's own label for the effect that is in the slot now ("1TIME",
    // "2:MIX", "1:WET"), or "1:--" where there is nothing to move (an empty slot, no such cell, a
    // list or a count, an effect that takes no modulation there). Anything else (the list's own
    // names): empty. Message thread (read as the screens read an insert's cells: without the
    // structure lock, which a screen that draws thirty times a second must not take from the audio thread).
    juce::String trackDestText(int trackIndex, int d) const
    {
        if (! mod::TrackDest::isInsert(d))
            return {};
        const int slot = mod::TrackDest::slot(d), cell = mod::TrackDest::cell(d);
        const juce::String number(slot + 1);
        const auto* chain = getInsertChain(trackIndex);
        const auto* fx = chain != nullptr ? chain->getEffect(slot) : nullptr;
        if (fx == nullptr || ! fx->takesModulation(cell))
            return number + ":--";
        const juce::String label(cell == EffectBase::WET_CELL ? "WET" : (cell < 4 ? fx->getParamSpec(cell) : fx->getExtraSpec(cell - 4)).label);
        return number + (label.length() < 4 ? ":" : "") + label;
    }
    // The track destination a DEST cell's value stands for (-1: none): `spec` is the cell.
    int trackDestOfCell(int trackIndex, const ParamSpec& spec, float value) const
    {
        const auto* e = getEngine(trackIndex);
        const mod::Router* router = e != nullptr ? e->modRouter() : nullptr;
        if (router == nullptr || spec.choices == nullptr)
            return -1;
        const int v = (int) std::lround(value);
        const int first = spec.choices == router->dests().cellLabels() ? router->dests().trackCell()
                        : spec.choices == router->dests().slotLabels() ? router->dests().trackSlotChoice()
                        : spec.choices == UnifiedVoice::lfoDestCellLabels() ? (int) UnifiedVoice::LfoDest::Count : -1;
        return first >= 0 && v >= first && v < first + mod::TrackDest::COUNT ? v - first : -1;
    }

private:

    // ---- The track's custom LFO shape, for the effects on it -----------------------------------
    // The eight levels (LfoShapeParam) live in the track's engine; the effects on the track read
    // this copy, which outlives a change of engine. An engine without the shape: the rising
    // staircase. The master chain has no track: null (the staircase).
    const float* userShapeOf(int trackIndex) const
    {
        return trackIndex >= 0 && trackIndex < MAX_TRACKS ? trackShape[(size_t) trackIndex].data() : nullptr;
    }

    // The track has an engine (another one, or the same again): the copy is made anew from it.
    // Message thread, under the structure lock.
    void adoptUserShape(int trackIndex)
    {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
            return;
        auto* e = getEngine(trackIndex);
        bool has = false;
        if (e != nullptr)
            for (const auto& page : e->getParamPages())
                for (const auto& spec : page.params)
                    has = has || spec.id == LfoShapeParam::BASE;
        trackHasShape[(size_t) trackIndex] = has;
        for (int i = 0; i < LFO::USER_POINTS; ++i)
            trackShape[(size_t) trackIndex][(size_t) i] = has ? juce::jlimit(-1.0f, 1.0f, e->getParameter(LfoShapeParam::BASE + i))
                                                              : LFO::defaultUserLevel(i);
        // Propagate the custom shape to every insert in the track's chain (TALK uses it for USR/USRS).
        if (auto* chain = getInsertChain(trackIndex))
            for (int s = 0; s < chain->getEffectCount(); ++s)
                chain->getEffect(s)->setUserShape(trackShape[(size_t) trackIndex].data());
    }

    // The master chain as a new project has it. The compressor starts neutral (0 dB, 1:1), as in
    // a project without stored master settings (ProjectBridge): at the Compressor class's own
    // -20 dB / 4:1 a single note was pulled down 7 dB within 20 ms of its onset, which was heard
    // as a click on every loud note.
    void buildDefaultMasterChain()
    {
        auto& chain = masterChain;
        while (chain.getEffectCount() > 0)
            chain.removeEffect(0);
        for (EffectType type : { EffectType::EQEffect, EffectType::CompressorEffect, EffectType::LimiterEffect })
        {
            auto fx = createEffect(type);
            if (type == EffectType::CompressorEffect)
            {
                fx->setParameter(0, 0.0f);      // THR
                fx->setParameter(1, 1.0f);      // RATI
            }
            if (currentSampleRate > 0)
                fx->prepare(currentSampleRate, currentBlockSize);
            chain.addEffect(std::move(fx), (int) type);
        }
    }

    // ---- Track input (audio thread) ----------------------------------------------------------
    // The track a track listens to: -1 when SRC is OFF, the track itself, or a track without an engine.
    int trackInputSource(int t) const
    {
        const int src = (int) trackIn[(size_t) t].source.load(std::memory_order_relaxed) - 1;
        if (src < 0 || src == t || src >= MAX_TRACKS || src >= (int) engines.size() || ! engines[(size_t) src])
            return -1;
        return src;
    }

    struct TrackInput
    {
        AudioInput view;            // a default one = no input
        float mode = 0.0f;
    };

    // Fills the track's input buffers for this block (the source's tap times LEVL, held inside
    // +-1 by the soft limiter, which is what keeps a feedback pair bounded) and runs the follower.
    TrackInput readTrackInput(int t, int numSamples)
    {
        TrackInput result;
        auto& in = trackIn[(size_t) t];
        result.mode = in.mode.load(std::memory_order_relaxed);
        const int src = trackInputSource(t);
        if (src < 0 || src >= (int) trackTapL.size() || (int) trackTapL[(size_t) src].size() < numSamples
            || t >= (int) trackInL.size() || (int) trackInL[(size_t) t].size() < numSamples)
        {
            in.follower.reset();
            in.lastSource = -1;
            in.lastLevel = 0.0f;
            return result;
        }

        const float attack = in.attackMs.load(std::memory_order_relaxed), release = in.releaseMs.load(std::memory_order_relaxed);
        if (attack != in.setAttack || release != in.setRelease || (float) currentSampleRate != in.setRate)
        {
            in.setAttack = attack;
            in.setRelease = release;
            in.setRate = (float) currentSampleRate;
            in.follower.setTimes(currentSampleRate > 0.0 ? currentSampleRate : 44100.0, attack, release);
        }

        // Another source than in the block before comes up from nothing over this block.
        if (src != in.lastSource)
            in.lastLevel = 0.0f;
        in.lastSource = src;

        const float level = in.level.load(std::memory_order_relaxed);
        float g = in.lastLevel;
        const float step = (level - g) / (float) numSamples;
        in.lastLevel = level;

        const float* tapL = trackTapL[(size_t) src].data();
        const float* tapR = trackTapR[(size_t) src].data();
        float* outL = trackInL[(size_t) t].data();
        float* outR = trackInR[(size_t) t].data();
        result.view.envelope = juce::jlimit(0.0f, 1.0f, in.follower.env);     // as the block starts
        for (int i = 0; i < numSamples; ++i)
        {
            g += step;
            // A sample of the tap that is not a number (NaN, infinite) is silence here: it must
            // reach neither the engine, the follower nor a MIX loop (x - x is 0 only for a number).
            const float tl = tapL[i], tr = tapR[i];
            const float l = fxb::softSat(tl - tl == 0.0f ? tl * g : 0.0f), r = fxb::softSat(tr - tr == 0.0f ? tr * g : 0.0f);
            outL[i] = l;
            outR[i] = r;
            in.follower.process(std::max(std::abs(l), std::abs(r)));
        }
        result.view.left = outL;
        result.view.right = outR;
        result.view.frames = numSamples;
        return result;
    }

    // MODE: how much of RING and of MIX there is moves across the block, so a change of MODE (a
    // step lock, the knob) is not a jump. Nothing is done while both are and stay at nothing.
    void mixTrackInput(int t, const TrackInput& input, float* left, float* right, int numSamples)
    {
        auto& in = trackIn[(size_t) t];
        const bool has = input.view.connected();
        const float mixTo = (has && input.mode == 1.0f) ? 1.0f : 0.0f;
        const float ringTo = (has && input.mode == 2.0f) ? 1.0f : 0.0f;
        float mix = in.lastMix, ring = in.lastRing;
        in.lastMix = mixTo;
        in.lastRing = ringTo;
        if (mix == 0.0f && ring == 0.0f && mixTo == 0.0f && ringTo == 0.0f)
            return;
        if (! has)
        {
            // The input went away: the engine's own sound comes back over this block.
            const float ringStep = -ring / (float) numSamples;
            for (int i = 0; i < numSamples && ring > 0.0f; ++i)
            {
                ring += ringStep;
                const float keep = 1.0f - juce::jmax(0.0f, ring);
                left[i] *= keep;
                right[i] *= keep;
            }
            return;
        }
        const float mixStep = (mixTo - mix) / (float) numSamples, ringStep = (ringTo - ring) / (float) numSamples;
        const float* inL = input.view.left;
        const float* inR = input.view.right;
        for (int i = 0; i < numSamples; ++i)
        {
            mix += mixStep;
            ring += ringStep;
            left[i] = left[i] * (1.0f - ring + ring * inL[i]) + mix * inL[i];
            right[i] = right[i] * (1.0f - ring + ring * inR[i]) + mix * inR[i];
        }
    }

    PerfFxRack perfFx;
    juce::MidiBuffer trackMidi;         // a MIDI track's messages for one block (kept: no allocation in the callback)
    std::array<Arpeggiator*, MAX_TRACKS> trackArps {};
    PadRatchet padRatchet;
    std::array<int, PadRatchet::kNumPads> padTrack {};
    std::array<int, PadRatchet::kNumPads> padSounding {};      // the note each pad is sounding, -1 = none (audio thread)
    std::array<std::array<float, NUM_ARP_LOCKS>, MAX_TRACKS> arpLockValues = makeArpLockDefaults();

    static std::array<std::array<float, NUM_ARP_LOCKS>, MAX_TRACKS> makeArpLockDefaults()
    {
        std::array<std::array<float, NUM_ARP_LOCKS>, MAX_TRACKS> v;
        for (auto& t : v) t = { -1.0f, -1.0f, 0.0f };     // gate off, velocity off, ratchet off
        return v;
    }

    struct TrackMix
    {
        std::atomic<float> volume { MixParam::DEFAULT_VOLUME };
        std::atomic<float> pan { 0.0f };
        std::array<std::atomic<float>, MixParam::MAX_SENDS> sends;
        float lastGainL = MixParam::DEFAULT_VOLUME;   // audio thread only
        float lastGainR = MixParam::DEFAULT_VOLUME;
        std::array<float, MixParam::MAX_SENDS> lastSend {};
    };
    struct ActiveLock { int id = -1; float base = 0.0f; };

    // A track's input (TrackInParam): the values are written by whoever sets parameters and read
    // once a block by the audio thread; the rest is the audio thread's.
    struct TrackIn
    {
        std::atomic<float> source { 0.0f };         // 0 = none, n = track n
        std::atomic<float> level { TRACK_IN_LEVEL };
        std::atomic<float> mode { 0.0f };           // 0 OFF, 1 MIX, 2 RING
        std::atomic<float> attackMs { TRACK_IN_ATTACK_MS };
        std::atomic<float> releaseMs { TRACK_IN_RELEASE_MS };

        fxb::EnvelopeFollower follower;
        float setAttack = -1.0f, setRelease = -1.0f, setRate = 0.0f;    // what the follower's times were made from
        int lastSource = -1;                        // the source of the block before (-1 = none)
        float lastLevel = 0.0f, lastMix = 0.0f, lastRing = 0.0f;       // ramped across a block, like the fader

        std::atomic<float>* valueOf(int id)
        {
            switch (id)
            {
                case TrackInParam::SRC:        return &source;
                case TrackInParam::LEVEL:      return &level;
                case TrackInParam::MODE:       return &mode;
                case TrackInParam::FOLLOW_ATK: return &attackMs;
                case TrackInParam::FOLLOW_REL: return &releaseMs;
                default:                       return nullptr;      // 349-359: not in use
            }
        }
        const std::atomic<float>* valueOf(int id) const { return const_cast<TrackIn*>(this)->valueOf(id); }

        // Any finite value brought to what the cell can hold.
        static float held(int id, float value)
        {
            switch (id)
            {
                case TrackInParam::SRC:        return (float) juce::jlimit(0, (int) MAX_TRACKS, (int) std::lround(value));
                case TrackInParam::LEVEL:      return juce::jlimit(0.0f, 1.0f, value);
                case TrackInParam::MODE:       return (float) juce::jlimit(0, 2, (int) std::lround(value));
                case TrackInParam::FOLLOW_ATK: return juce::jlimit(1.0f, 200.0f, value);
                case TrackInParam::FOLLOW_REL: return juce::jlimit(10.0f, 2000.0f, value);
                default:                       return value;
            }
        }
    };
    std::array<TrackIn, MAX_TRACKS> trackIn;
    std::vector<std::vector<float>> trackInL, trackInR;       // each track's input for one block (sized in prepare)
    std::array<bool, MAX_TRACKS> trackFeeds {};               // audio thread: another track listens to this one

    std::array<TrackMix, MAX_TRACKS> trackMix;
    std::array<std::array<ActiveLock, MAX_ACTIVE_LOCKS>, MAX_TRACKS> activeLocks {};
    std::array<int, MAX_TRACKS> activeLockCount {};
    std::vector<std::shared_ptr<EngineBase>> retiredEngines;
    float lastTempo = 120.0f;

    void ensureInsertChains(int trackIndex)
    {
        if (trackIndex < 0) return;
        if ((size_t) trackIndex >= trackInsertChains.size())
        {
            size_t oldSize = trackInsertChains.size();
            trackInsertChains.resize((size_t) trackIndex + 1);
            if (currentSampleRate > 0)
            {
                for (size_t i = oldSize; i < trackInsertChains.size(); ++i)
                    trackInsertChains[i].prepare(currentSampleRate, currentBlockSize);
            }
        }
    }

    SequencerCore* sequencer = nullptr;
    Arpeggiator* arpeggiator = nullptr;
    MIDIClock* midiClock = nullptr;
    bool extClockRan = false, extSeqPlayed = false;     // followExternalClock
    double extBeatOffset = 0.0;
    juce::MidiOutput* midiOutput = nullptr;
    std::vector<std::shared_ptr<EngineBase>> engines;
    std::vector<EffectChain> trackInsertChains;
    EffectChain masterChain;                    // the insert chain of the mix (MASTER_TRACK)
    float masterLevel = 0.8f;
    std::array<Motion, MAX_MOTIONS> motions;
    double currentSampleRate = 0.0;
    int currentBlockSize = 0;

    Resampler resampler;
    Metronome metronome;
    juce::CriticalSection structureMutex;
    std::atomic<int> skippedBlocks { 0 };
    std::atomic<int> nonFiniteBlocks { 0 };
    std::atomic<int> nonFiniteTrack { -1 };
    std::atomic<bool> silenceRequested { false };
    int silenceSamplesLeft = 0;         // audio thread only

    // Jumps between two blocks carried off over 2 ms (declickTrack, the silence, a skipped block):
    // on each track after its engine and after its inserts, and on the master output.
    std::array<std::array<dsp::BlockDeclick, 2>, MAX_TRACKS> engineDeclick, trackDeclick;
    std::array<std::atomic<bool>, MAX_TRACKS> engineDeclickArm {}, trackDeclickArm {};
    std::array<dsp::BlockDeclick, 2> masterDeclick;
    std::atomic<bool> masterDeclickArm { false };
    bool masterSkipped = false;         // audio thread only

    // Each track's custom LFO shape as its effects read it (adoptUserShape).
    std::array<std::array<float, LFO::USER_POINTS>, MAX_TRACKS> trackShape {};
    std::array<bool, MAX_TRACKS> trackHasShape {};

    // Audio thread: every note off, every insert chain and send bus emptied.
    void silenceNow()
    {
        for (auto& e : engines)
            if (e) e->allNotesOff();
        for (auto& chain : trackInsertChains)
            chain.reset();
        masterChain.reset();
        for (auto& bus : sendBuses)
            bus.getChain().reset();
        perfFx.silence();
    }
public:
    // Stop everything that sounds: notes are released and effect tails cut at the next block.
    // The way out of any effect that rings on (Shift + Play).
    void requestSilence() { silenceRequested.store(true, std::memory_order_relaxed); }
    int getSkippedBlocks() const { return skippedBlocks.load(); }     // blocks silenced by a rebuild
    // Blocks dropped because an engine, an insert chain or the sum held a number that is not finite.
    // It stays 0 while everything is right: the robustness tests fail on any other value.
    int getNonFiniteBlocks() const { return nonFiniteBlocks.load(std::memory_order_relaxed) + perfFx.getNonFiniteCount(); }
    // Where the last one came from: a track's engine (the track), its insert effects (1000 + the
    // track), a send's chain (2000 + the bus), or -1.
    int getNonFiniteTrack() const { return nonFiniteTrack.load(std::memory_order_relaxed); }
    static bool blockIsFinite(const float* left, const float* right, int numSamples)
    {
        // (x - x is 0 for a finite x and NaN for an infinite one or a NaN: one test for the sum)
        float sum = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            sum += (left[i] - left[i]) + (right[i] - right[i]);
        return sum == 0.0f;
    }
private:
    // The guard itself, for the four places it stands (after an engine, after a track's effects,
    // after a send's chain, on the sum): a block that holds a number that is not one is made silence and counted. True:
    // it was dropped, and the caller empties what made it.
    bool droppedUnlessFinite(float* left, float* right, int numSamples)
    {
        if (blockIsFinite(left, right, numSamples))
            return false;
        std::fill(left, left + numSamples, 0.0f);
        std::fill(right, right + numSamples, 0.0f);
        nonFiniteBlocks.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::vector<float> scratchL;
    std::vector<std::vector<float>> trackTapL, trackTapR;     // each track's output after its inserts (sidechain sources)
    std::vector<float> scratchR;

    std::vector<SendBus> sendBuses;             // each one's chain is the insert chain of sendTrack(bus)
    // The tracks' sounds by track, as a KeyFeed wants them (set in prepare).
    std::array<const float*, MAX_TRACKS> tapLeft {}, tapRight {};
};
