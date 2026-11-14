#pragma once

#include "UnifiedVoiceEngine.h"
#include "FMEngine.h"
#include "MIDITrack.h"
#include "MacroOscEngine.h"
#include "WavetableEngine.h"
#include "WaveSeqEngine.h"
#include "DrumEngine.h"
#include "SamplerEngine.h"
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
    static constexpr int MAX_ACTIVE_LOCKS = 16;
    // Track mixer parameters (volume, pan, send levels) are MixParam ids: applied here, never
    // forwarded to an engine.
    static constexpr int PARAM_SEND_BASE = MixParam::SEND_BASE;
    static constexpr int PARAM_SEND1 = MixParam::SEND_BASE;
    static constexpr int PARAM_SEND2 = MixParam::SEND_BASE + 1;
    static constexpr int SEND_TYPE_PARAM_BASE = 150;  // 150 + busIdx = send bus type selector
    static constexpr int EFFECT_PARAM_BASE = 200;  // 200 + slot*4 + paramIdx

    // MOD param IDs are in ModParam namespace (EngineBase.h)

    AudioEngine()
    {
        padRatchet.onNoteOn = [this](int pad, int note, float velocity)
        {
            const int t = padTrack[(size_t) pad];
            if (auto* e = getEngine(t))
                e->noteOn(note, velocity, -1);
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
        // The master compressor starts neutral, as in a project without stored master settings
        // (ProjectBridge). At the Compressor class's own -20 dB / 4:1 a single note was pulled
        // down 7 dB within 20 ms of its onset, which was heard as a click on every loud note.
        masterComp.setThreshold(0.0f);
        masterComp.setRatio(1.0f);
        sendBuses.resize(DEFAULT_NUM_SENDS);
        sendBusTypes.resize(DEFAULT_NUM_SENDS, EffectType::ReverbEffect);

        auto reverb = std::make_unique<Reverb>();
        reverb->setDecay(0.6f);
        reverb->setDamping(0.4f);
        reverb->setMix(1.0f);
        sendBuses[SEND_REVERB].getChain().addEffect(std::move(reverb));
        sendBuses[SEND_REVERB].setSendLevel(1.0f);
        sendBusTypes[SEND_REVERB] = EffectType::ReverbEffect;

        auto delay = std::make_unique<Delay>();
        delay->setTime(0.375f);
        delay->setFeedback(0.35f);
        delay->setMix(1.0f);
        sendBuses[SEND_DELAY].getChain().addEffect(std::move(delay));
        sendBuses[SEND_DELAY].setSendLevel(1.0f);
        sendBusTypes[SEND_DELAY] = EffectType::DelayEffect;
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
            case EffectType::NumEffectTypes:    break;
        }
        return "---";
    }

    // ---- Structure lock -----------------------------------------------------------------------
    // Anything that builds or destroys objects the audio thread uses (effects, engines, the whole
    // project) holds this. The audio thread only ever tries it: if a rebuild is in progress it
    // plays one block of silence instead of touching half-built state. Re-entrant.
    using StructureLock = juce::CriticalSection::ScopedLockType;
    juce::CriticalSection& structure() { return structureMutex; }

    void addInsertEffect(int trackIndex, EffectType type)
    {
        const StructureLock lock(structureMutex);
        ensureInsertChains(trackIndex);
        if (trackInsertChains[(size_t) trackIndex].getEffectCount() >= MAX_INSERTS_PER_TRACK)
            return;
        auto fx = createEffect(type);
        if (!fx) return;
        if (currentSampleRate > 0)
            fx->prepare(currentSampleRate, currentBlockSize);
        trackInsertChains[(size_t) trackIndex].addEffect(std::move(fx));
        insertTypes[(size_t) trackIndex].push_back(type);
    }

    void removeInsertEffect(int trackIndex, int slot)
    {
        const StructureLock lock(structureMutex);
        if (trackIndex < 0 || trackIndex >= (int) trackInsertChains.size())
            return;
        trackInsertChains[(size_t) trackIndex].removeEffect(slot);
        if (trackIndex < (int) insertTypes.size() && slot >= 0 && slot < (int) insertTypes[(size_t) trackIndex].size())
            insertTypes[(size_t) trackIndex].erase(insertTypes[(size_t) trackIndex].begin() + slot);
    }

    // Put an effect of the given type into a slot: replaces what is there, or appends when the slot
    // is the first free one.
    void setInsertEffect(int trackIndex, int slot, EffectType type)
    {
        const StructureLock lock(structureMutex);
        ensureInsertChains(trackIndex);
        if (trackIndex < 0) return;
        auto& chain = trackInsertChains[(size_t) trackIndex];
        if (slot >= chain.getEffectCount())
        {
            addInsertEffect(trackIndex, type);
            return;
        }
        if (slot < 0) return;
        auto fx = createEffect(type);
        if (!fx) return;
        chain.replaceEffect(slot, std::move(fx));
        insertTypes[(size_t) trackIndex][(size_t) slot] = type;
    }

    // Type of the effect in a slot; false when the slot is empty.
    bool getInsertEffectType(int trackIndex, int slot, EffectType& typeOut) const
    {
        if (trackIndex < 0 || trackIndex >= (int) insertTypes.size()) return false;
        auto& types = insertTypes[(size_t) trackIndex];
        if (slot < 0 || slot >= (int) types.size()) return false;
        typeOut = types[(size_t) slot];
        return true;
    }

    EffectChain* getInsertChain(int trackIndex)
    {
        if (trackIndex < 0 || trackIndex >= (int) trackInsertChains.size())
            return nullptr;
        return &trackInsertChains[(size_t) trackIndex];
    }

    const EffectChain* getInsertChain(int trackIndex) const
    {
        if (trackIndex < 0 || trackIndex >= (int) trackInsertChains.size())
            return nullptr;
        return &trackInsertChains[(size_t) trackIndex];
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
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
            return 0.0f;

        if (MixParam::isMixParam(id))
        {
            auto& m = trackMix[(size_t) trackIndex];
            if (id == MixParam::VOLUME) return m.volume.load();
            if (id == MixParam::PAN) return m.pan.load();
            return m.sends[(size_t) (id - MixParam::SEND_BASE)].load();
        }

        if (TrackFxParam::isTrackFxParam(id))
            return trackFx[(size_t) trackIndex] ? trackFx[(size_t) trackIndex]->get(id) : defaultTrackFx().get(id);

        if (isArpLockParam(id))
            return arpLockValues[(size_t) trackIndex][(size_t) (id - ARP_LOCK_BASE)];

        if (isInsertSwitchParam(id))
        {
            const int kind = (id - INSERT_ON_BASE) / MAX_INSERTS_PER_TRACK;      // 0 on, 1 wet, 2 sync, 3 division
            const int slot = (id - INSERT_ON_BASE) % MAX_INSERTS_PER_TRACK;
            auto* chain = const_cast<EffectChain*>(getInsertChain(trackIndex));
            auto* fx = (chain && slot < chain->getEffectCount()) ? chain->getEffect(slot) : nullptr;
            if (fx == nullptr) return kind == 2 ? 0.0f : (kind == 3 ? (float) FxSync::DEFAULT_DIVISION : 1.0f);
            if (kind == 2) return fx->isSynced() ? 1.0f : 0.0f;
            if (kind == 3) return (float) fx->getSyncDivision();
            return kind == 1 ? fx->getMix() : (fx->isEnabled() ? 1.0f : 0.0f);
        }

        if (id >= EFFECT_PARAM_BASE && id < EngineParamRange::THIRD_BASE)     // 400 up is the engine's third block
        {
            int offset = id - EFFECT_PARAM_BASE;
            auto* chain = getInsertChain(trackIndex);
            if (chain && offset / 4 < chain->getEffectCount())
                if (auto* fx = const_cast<EffectChain*>(chain)->getEffect(offset / 4))
                    return fx->getUserParameter(offset % 4);
            return 0.0f;
        }

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

        for (auto& [id, value] : step.paramLocks)
        {
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
    const TrackFx* getTrackFx(int trackIndex) const
    {
        return (trackIndex >= 0 && trackIndex < MAX_TRACKS) ? trackFx[(size_t) trackIndex].get() : nullptr;
    }

    // ================= Pad pressure routing: one router per track =================================
    // Each engine reads its own track's router per voice. AT ROUTE edits the selected track's.
    using PressureDest = AftertouchRouter::Destination;
    struct PressureDestInfo { PressureDest dest; const char* label; };

    // The destinations an engine type really responds to (OFF not included), with the labels its
    // own pages use. Returns how many.
    static int pressureDestinations(EngineType type, const PressureDestInfo*& list)
    {
        using D = PressureDest;
        static const PressureDestInfo unified[] = { { D::Volume, "LEVL" }, { D::FilterCutoff, "CUT" }, { D::Vibrato, "VIB" },
                                                    { D::MacroTimbre, "PW" }, { D::WavePosition, "MIX" }, { D::FMDepth, "XMOD" } };
        static const PressureDestInfo fm[]      = { { D::Volume, "LEVL" }, { D::FilterCutoff, "CUT" }, { D::FMDepth, "FM" } };
        static const PressureDestInfo macro[]   = { { D::Volume, "LEVL" }, { D::FilterCutoff, "CUT" }, { D::Vibrato, "VIB" },
                                                    { D::MacroTimbre, "TIMB" }, { D::WavePosition, "MORP" } };
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
            case EngineType::MacroOsc:     list = macro;   return 5;
            case EngineType::Wavetable:    list = table;   return 4;
            case EngineType::WaveSeq:      list = waveSeq; return 2;
            case EngineType::Drum:         list = drum;    return 7;
            case EngineType::Sampler:                                   // no pressure response
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

    // How many routes a track of this engine has: a drum kit has one destination for all its pads.
    static int pressureRouteCount(EngineType type)
    {
        const PressureDestInfo* list = nullptr;
        if (pressureDestinations(type, list) == 0) return 0;
        return type == EngineType::Drum ? 1 : AftertouchRouter::MAX_ROUTES;
    }

    // What a new track of this engine does with pressure: level on the synths (as before the
    // routers were per track), decay on a drum kit, nothing where the engine has no response.
    static AftertouchRouter defaultPressureRouter(EngineType type)
    {
        AftertouchRouter r;
        r.clear();
        if (type == EngineType::Drum)
            r.setRoute(0, { PressureDest::DrumDecay, 1.0f, false });
        else if (pressureRouteCount(type) > 0)
            r.setRoute(0, { PressureDest::Volume, 1.0f, false });
        return r;
    }

    AftertouchRouter& getPressureRouter(int trackIndex)
    {
        return trackRouters[(size_t) juce::jlimit(0, MAX_TRACKS - 1, trackIndex)];
    }
    const AftertouchRouter& getPressureRouter(int trackIndex) const
    {
        return trackRouters[(size_t) juce::jlimit(0, MAX_TRACKS - 1, trackIndex)];
    }

    EngineType getEngineTypeOf(int trackIndex) const
    {
        auto* e = getEngine(trackIndex);
        return e != nullptr ? e->getType() : EngineType::UnifiedVoice;
    }

    // Set one route of a track. A destination the track's engine does not respond to becomes OFF.
    void setPressureRoute(int trackIndex, int routeIndex, AftertouchRouter::Route route)
    {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
        if (!pressureDestinationValid(getEngineTypeOf(trackIndex), route.dest))
            route.dest = PressureDest::Off;
        auto& router = trackRouters[(size_t) trackIndex];
        router.setRoute(routeIndex, route);
        router.setRouteCount(AftertouchRouter::MAX_ROUTES);
        pushDrumPressure(trackIndex);
    }

    // Replace all of a track's routes (project load, restoring a sound).
    void setPressureRouter(int trackIndex, const AftertouchRouter& routes)
    {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
        for (int i = 0; i < AftertouchRouter::MAX_ROUTES; ++i)
        {
            auto r = routes.effectiveRoute(i);
            if (!pressureDestinationValid(getEngineTypeOf(trackIndex), r.dest))
                r.dest = PressureDest::Off;
            trackRouters[(size_t) trackIndex].setRoute(i, r);
        }
        trackRouters[(size_t) trackIndex].setRouteCount(AftertouchRouter::MAX_ROUTES);
        pushDrumPressure(trackIndex);
    }

    void resetPressureRouter(int trackIndex)
    {
        if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
        trackRouters[(size_t) trackIndex] = defaultPressureRouter(getEngineTypeOf(trackIndex));
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

    // Turns the pressure recorded on a step into parameter locks, through the track's routes as
    // they are now: on each step the note spans, each routed cell is locked to where the lane's
    // mean pressure over that step would hold it (the route's depth is how far towards the cell's
    // top, or bottom when negative, full pressure goes). Steps without a trig become lock trigs:
    // a trig with no note. Several notes on a synth step share the cells, so their lanes are
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
        const auto& router = getPressureRouter(trackIndex);
        const int span = juce::jlimit(1, track.length, (int) std::ceil(source.length - 0.001f));
        const bool drum = type == EngineType::Drum;

        for (int r = 0; r < pressureRouteCount(type); ++r)
        {
            const auto route = router.effectiveRoute(r);
            if (route.dest == PressureDest::Off || route.depth == 0.0f) continue;
            ++result.routes;
            bool locked = false;
            // One pass per cell: a drum pad's own, or the one all the synth's notes share.
            for (size_t first = 0; first < source.notes.size(); ++first)
            {
                if (source.notes[first].lane.empty()) continue;
                ParamSpec cell;
                if (!pressureLockCell(trackIndex, route.dest, source.notes[first].pitch, cell)) { if (drum) continue; break; }
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
                    const float mod = juce::jlimit(-1.0f, 1.0f, (route.bipolar ? mean * 2.0f - 1.0f : mean) * route.depth);
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

    // Legacy: one router for the whole instrument (what the app passed before the routers were per
    // track). It is no longer read by any engine. While it is set, a later change to it is copied
    // to every track whose engine uses a router (drum kits keep their own), so code that still
    // edits the one router keeps working. New code uses the per-track functions above.
    void setAftertouchRouter(const AftertouchRouter* router)
    {
        legacyRouter = router;
        if (router != nullptr)
            legacySnapshot = *router;
    }

    // The legacy router was set to something that is already in the tracks (a project load).
    void adoptLegacyRouter()
    {
        if (legacyRouter != nullptr)
            legacySnapshot = *legacyRouter;
    }

    void syncLegacyRouter()
    {
        if (legacyRouter == nullptr || legacyRouter->sameRoutesAs(legacySnapshot))
            return;
        legacySnapshot = *legacyRouter;
        for (int t = 0; t < (int) engines.size() && t < MAX_TRACKS; ++t)
            if (engines[(size_t) t] && engines[(size_t) t]->getType() != EngineType::Drum)
                setPressureRouter(t, legacySnapshot);
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

    void addEngine(int trackIndex, std::shared_ptr<EngineBase> engine)
    {
        const StructureLock lock(structureMutex);
        if (trackIndex >= (int) engines.size())
            engines.resize((size_t) trackIndex + 1);
        if (trackIndex >= 0 && trackIndex < MAX_TRACKS)
            ensureTrackFx(trackIndex);
        if (engine && trackIndex >= 0 && trackIndex < MAX_TRACKS)
        {
            trackRouters[(size_t) trackIndex] = defaultPressureRouter(engine->getType());
            engine->setAftertouchRouter(&trackRouters[(size_t) trackIndex]);
        }
        engines[(size_t) trackIndex] = std::move(engine);
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
        if (trackIndex < MAX_TRACKS)
        {
            // A different engine starts from its own default routing; the same engine rebuilt
            // (a preset load) keeps the track's routes.
            if (!current || current->getType() != type)
                trackRouters[(size_t) trackIndex] = defaultPressureRouter(type);
            newEngine->setAftertouchRouter(&trackRouters[(size_t) trackIndex]);
        }
        newEngine->setTempo(lastTempo);

        // The audio thread may still be inside the old engine: keep it alive for a while.
        if (current)
        {
            retiredEngines.push_back(current);
            if (retiredEngines.size() > 16)
                retiredEngines.erase(retiredEngines.begin());
        }
        engines[(size_t) trackIndex] = std::move(newEngine);

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
    // setEngineType nothing stored is changed: the caller owns Track::soundParams and the routes.
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
        {
            if (!parked || parked->getType() != type)
                trackRouters[(size_t) trackIndex] = defaultPressureRouter(type);
            fresh->setAftertouchRouter(&trackRouters[(size_t) trackIndex]);
            activeLockCount[(size_t) trackIndex] = 0;
        }
        fresh->setTempo(lastTempo);
        if (parked)
            parked->allNotesOff();
        engines[(size_t) trackIndex] = std::move(fresh);
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
        {
            parked->setAftertouchRouter(&trackRouters[(size_t) trackIndex]);
            activeLockCount[(size_t) trackIndex] = 0;
        }
        parked->setTempo(lastTempo);
        parked->allNotesOff();
        engines[(size_t) trackIndex] = std::move(parked);
    }

    int getSendCount() const { return (int) sendBuses.size(); }

    int addSendBus(EffectType type)
    {
        if ((int) sendBuses.size() >= MAX_SENDS) return -1;
        sendBuses.emplace_back();
        sendBusTypes.push_back(type);
        int idx = (int) sendBuses.size() - 1;
        auto fx = createEffect(type);
        if (fx)
        {
            fx->setMix(1.0f);
            if (currentSampleRate > 0)
                fx->prepare(currentSampleRate, currentBlockSize);
            sendBuses[idx].getChain().addEffect(std::move(fx));
        }
        sendBuses[idx].setSendLevel(1.0f);
        if (currentSampleRate > 0)
            sendBuses[idx].prepare(currentSampleRate, currentBlockSize);
        return idx;
    }

    void removeSendBus(int index)
    {
        if (index < 0 || index >= (int) sendBuses.size()) return;
        sendBuses.erase(sendBuses.begin() + index);
        sendBusTypes.erase(sendBusTypes.begin() + index);
    }

    void setSendBusEffect(int index, EffectType type)
    {
        const StructureLock lock(structureMutex);
        if (index < 0 || index >= (int) sendBuses.size()) return;
        auto& bus = sendBuses[index];
        while (bus.getChain().getEffectCount() > 0)
            bus.getChain().removeEffect(0);
        auto fx = createEffect(type);
        if (fx)
        {
            fx->setMix(1.0f);
            if (currentSampleRate > 0)
                fx->prepare(currentSampleRate, currentBlockSize);
            bus.getChain().addEffect(std::move(fx));
        }
        sendBusTypes[index] = type;
    }

    EffectType getSendBusType(int index) const
    {
        if (index < 0 || index >= (int) sendBusTypes.size())
            return EffectType::ReverbEffect;
        return sendBusTypes[index];
    }

    SendBus& getSendBus(int index) { return sendBuses[index]; }
    const SendBus& getSendBus(int index) const { return sendBuses[index]; }

    ParametricEQ& getMasterEQ() { return masterEQ; }
    Compressor& getMasterComp() { return masterComp; }
    Limiter& getMasterLimiter() { return masterLimiter; }

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
        resampler.prepare(currentSampleRate);
        metronome.prepare(currentSampleRate);
        trackTapL.assign((size_t) MAX_TRACKS, std::vector<float>((size_t) currentBlockSize, 0.0f));
        trackTapR.assign((size_t) MAX_TRACKS, std::vector<float>((size_t) currentBlockSize, 0.0f));
        scratchR.resize((size_t) currentBlockSize, 0.0f);

        for (auto& chain : trackInsertChains)
            chain.prepare(currentSampleRate, currentBlockSize);

        for (auto& fx : trackFx)
            if (fx) fx->prepare(currentSampleRate, currentBlockSize);

        for (auto& bus : sendBuses)
            bus.prepare(currentSampleRate, currentBlockSize);

        masterEQ.setSampleRate(currentSampleRate);
        masterComp.setSampleRate(currentSampleRate);
        masterLimiter.setSampleRate(currentSampleRate);
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

        // A rebuild is in progress on another thread: one silent block, then carry on.
        const juce::CriticalSection::ScopedTryLockType structureLock(structureMutex);
        if (! structureLock.isLocked())
        {
            juce::FloatVectorOperations::clear(outputChannelData[0], numSamples);
            juce::FloatVectorOperations::clear(outputChannelData[1], numSamples);
            skippedBlocks.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        syncLegacyRouter();

        // Shift + Play asked for silence: every note is let go and every effect's memory is emptied
        // (send buses, inserts), here on the audio thread, between two blocks.
        if (silenceRequested.exchange(false, std::memory_order_relaxed))
        {
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

        if (midiClock)
            midiClock->processBlock(numSamples, currentSampleRate);

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

        juce::MidiBuffer trackMidi;
        for (int t = 0; t < (int) engines.size(); ++t)
        {
            auto& e = engines[(size_t) t];
            if (!e) continue;

            std::fill(scratchL.begin(), scratchL.begin() + numSamples, 0.0f);
            std::fill(scratchR.begin(), scratchR.begin() + numSamples, 0.0f);

            float* scratch[2] = { scratchL.data(), scratchR.data() };
            juce::AudioBuffer<float> trackBuf(scratch, 2, numSamples);
            trackMidi.clear();
            e->processBlock(trackBuf, trackMidi);

            if (!trackMidi.isEmpty() && midiOutput)
                midiOutput->sendBlockOfMessagesNow(trackMidi);

            // The track's own effect blocks (DIRT, TALK, MODFX), then its insert slots.
            if (t < MAX_TRACKS && trackFx[(size_t) t])
                trackFx[(size_t) t]->process(scratchL.data(), scratchR.data(), numSamples);

            if (t < (int) trackInsertChains.size()
                && trackInsertChains[(size_t) t].getEffectCount() > 0)
            {
                // A DUCK effect listening to another track gets that track's latest block (this
                // block for a lower-numbered track, the previous one otherwise).
                auto& chain = trackInsertChains[(size_t) t];
                const float bpm = sequencer ? sequencer->getTempo() : 120.0f;
                for (int i = 0; i < chain.getEffectCount(); ++i)
                    if (auto* fx = chain.getEffect(i))
                        fx->setTempo(bpm);      // synced times and rates follow tempo changes
                for (int i = 0; i < chain.getEffectCount(); ++i)
                    if (auto* duck = dynamic_cast<SidechainInsert*>(chain.getEffect(i)))
                    {
                        const int src = duck->getSidechainTrack();
                        if (src >= 0 && src < (int) trackTapL.size() && src != t
                            && (int) trackTapL[(size_t) src].size() >= numSamples)
                            duck->setSidechainBuffers(trackTapL[(size_t) src].data(), trackTapR[(size_t) src].data());
                    }
                chain.processStereo(scratchL.data(), scratchR.data(), numSamples);
            }
            if (t < (int) trackTapL.size() && (int) trackTapL[(size_t) t].size() >= numSamples)
            {
                std::copy(scratchL.begin(), scratchL.begin() + numSamples, trackTapL[(size_t) t].begin());
                std::copy(scratchR.begin(), scratchR.begin() + numSamples, trackTapR[(size_t) t].begin());
            }
            // Resampling one track: after its effects, before its fader and pan.
            if (resampler.getSource() == t)
                resampler.process(scratchL.data(), scratchR.data(), numSamples, beatAtStart, beatsPerSample, seqPlaying);

            // Track mixer: volume and pan (balance law, unity at centre), ramped across the block.
            if (t < MAX_TRACKS)
            {
                auto& m = trackMix[(size_t) t];
                const float vol = m.volume.load();
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
                    float sendLevel = m.sends[(size_t) s].load();
                    if (sendLevel > 0.0f)
                        sendBuses[s].addInput(scratchL.data(), scratchR.data(), numSamples, sendLevel);
                }
            }

            juce::FloatVectorOperations::add(mainL, scratchL.data(), numSamples);
            juce::FloatVectorOperations::add(mainR, scratchR.data(), numSamples);
        }

        for (auto& bus : sendBuses)
        {
            bus.process();
            bus.mixInto(mainL, mainR, numSamples);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            masterEQ.process(mainL[i], mainR[i]);
            masterComp.process(mainL[i], mainR[i]);
            masterLimiter.process(mainL[i], mainR[i]);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            mainL[i] *= masterLevel;
            mainR[i] *= masterLevel;
        }

        if (silenceSamplesLeft > 0)
        {
            juce::FloatVectorOperations::clear(mainL, numSamples);
            juce::FloatVectorOperations::clear(mainR, numSamples);
            silenceSamplesLeft -= numSamples;
            if (silenceSamplesLeft <= 0)
                silenceNow();
        }

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

    void setMasterParam(int index, float value)
    {
        switch (index)
        {
            case 0: case 1: case 2: case 3: masterEQ.setParameter(index, value); break;
            case 4: masterComp.setThreshold(value); break;
            case 5: masterComp.setRatio(value); break;
            case 6: masterComp.setMakeupGain(value); break;
            case 7: masterLimiter.setCeiling(value); break;
            default: break;
        }
    }

    float getMasterParam(int index) const
    {
        switch (index)
        {
            case 0: case 1: case 2: case 3: return masterEQ.getParameter(index);
            case 4: return masterComp.getThreshold();
            case 5: return masterComp.getRatio();
            case 6: return masterComp.getMakeupGain();
            case 7: return masterLimiter.getCeiling();
            default: return 0.0f;
        }
    }

    void setMasterLevel(float level) { masterLevel = level; }
    float getMasterLevel() const { return masterLevel; }

    void setMIDIOutput(juce::MidiOutput* output) { midiOutput = output; }
    juce::MidiOutput* getMIDIOutput() const { return midiOutput; }

    void setMIDIClock(MIDIClock* clock) { midiClock = clock; }
    MIDIClock* getMIDIClock() const { return midiClock; }

private:
    void applyTrackParameter(int trackIndex, int id, float value)
    {
        if (trackIndex < 0 || RangeParam::isRangeParam(id))
            return;     // range ends are data for the parameter screen, not sound parameters

        if (MixParam::isMixParam(id))
        {
            if (trackIndex >= MAX_TRACKS) return;
            auto& m = trackMix[(size_t) trackIndex];
            if (id == MixParam::VOLUME) m.volume = juce::jlimit(0.0f, 1.0f, value);
            else if (id == MixParam::PAN) m.pan = juce::jlimit(-1.0f, 1.0f, value);
            else m.sends[(size_t) (id - MixParam::SEND_BASE)] = juce::jlimit(0.0f, 1.0f, value);
            return;
        }

        if (TrackFxParam::isTrackFxParam(id))
        {
            if (trackIndex < MAX_TRACKS)
                ensureTrackFx(trackIndex).set(id, value);
            return;
        }

        if (isInsertSwitchParam(id))
        {
            const int kind = (id - INSERT_ON_BASE) / MAX_INSERTS_PER_TRACK;
            const int slot = (id - INSERT_ON_BASE) % MAX_INSERTS_PER_TRACK;
            auto* chain = getInsertChain(trackIndex);
            if (auto* fx = (chain && slot < chain->getEffectCount()) ? chain->getEffect(slot) : nullptr)
            {
                if (kind == 0)      fx->setEnabled(value > 0.5f);
                else if (kind == 1) fx->setMix(juce::jlimit(0.0f, 1.0f, value));
                else if (kind == 2) { fx->setTempo(sequencer ? sequencer->getTempo() : 120.0f); fx->setSync(value > 0.5f); }
                else                fx->setSyncDivision((int) std::lround(value));
            }
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

        if (id >= EFFECT_PARAM_BASE && id < EngineParamRange::THIRD_BASE)     // 400 up is the engine's third block
        {
            int offset = id - EFFECT_PARAM_BASE;
            auto* chain = getInsertChain(trackIndex);
            if (chain && offset / 4 < chain->getEffectCount())
                if (auto* fx = chain->getEffect(offset / 4))
                    fx->setUserParameter(offset % 4, value);
            return;
        }

        if (! EngineParamRange::contains(id))
            return;   // send bus types are global and changed through setSendBusEffect

        if (auto* e = getEngine(trackIndex))
            e->setParameter(id, value);
    }

    // Created (and prepared) on the message thread when a track first gets an engine or an
    // effect-block parameter.
    TrackFx& ensureTrackFx(int trackIndex)
    {
        auto& fx = trackFx[(size_t) trackIndex];
        if (!fx)
        {
            auto created = std::make_unique<TrackFx>();
            created->prepare(currentSampleRate > 0 ? currentSampleRate : 44100.0,
                             currentBlockSize > 0 ? currentBlockSize : 2048);
            fx = std::move(created);
        }
        return *fx;
    }

    static const TrackFx& defaultTrackFx()
    {
        static const TrackFx defaults;
        return defaults;
    }

    std::array<std::unique_ptr<TrackFx>, MAX_TRACKS> trackFx;
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
    };
    struct ActiveLock { int id = -1; float base = 0.0f; };

    std::array<TrackMix, MAX_TRACKS> trackMix;
    std::array<std::array<ActiveLock, MAX_ACTIVE_LOCKS>, MAX_TRACKS> activeLocks {};
    std::array<int, MAX_TRACKS> activeLockCount {};
    std::array<AftertouchRouter, MAX_TRACKS> trackRouters;
    const AftertouchRouter* legacyRouter = nullptr;
    AftertouchRouter legacySnapshot;

    // A drum kit has its pressure destination inside its voices: route 1 of the track's router
    // goes to every pad.
    void pushDrumPressure(int trackIndex)
    {
        auto* drum = dynamic_cast<DrumEngine*>(getEngine(trackIndex));
        if (drum == nullptr) return;
        const auto route = trackRouters[(size_t) trackIndex].effectiveRoute(0);
        DrumATDest dest = DrumATDest::Off;
        switch (route.dest)
        {
            case PressureDest::DrumPitch:    dest = DrumATDest::Pitch; break;
            case PressureDest::DrumDecay:    dest = DrumATDest::Decay; break;
            case PressureDest::FilterCutoff: dest = DrumATDest::Filter; break;
            case PressureDest::Volume:       dest = DrumATDest::Level; break;
            case PressureDest::DrumDrive:    dest = DrumATDest::Drive; break;
            case PressureDest::DrumSnap:     dest = DrumATDest::Snap; break;
            case PressureDest::PanSpread:    dest = DrumATDest::Pan; break;
            default: break;
        }
        for (int slot = 0; slot < DrumEngine::NUM_VOICES; ++slot)
        {
            drum->setSlotAftertouchDest(slot, dest);
            drum->setSlotAftertouchDepth(slot, juce::jlimit(0.0f, 1.0f, route.depth));
        }
    }
    std::vector<std::shared_ptr<EngineBase>> retiredEngines;
    float lastTempo = 120.0f;

    void ensureInsertChains(int trackIndex)
    {
        if (trackIndex < 0) return;
        if ((size_t) trackIndex >= trackInsertChains.size())
        {
            size_t oldSize = trackInsertChains.size();
            trackInsertChains.resize((size_t) trackIndex + 1);
            insertTypes.resize((size_t) trackIndex + 1);
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
    juce::MidiOutput* midiOutput = nullptr;
    std::vector<std::shared_ptr<EngineBase>> engines;
    std::vector<EffectChain> trackInsertChains;
    std::vector<std::vector<EffectType>> insertTypes;
    float masterLevel = 0.8f;
    std::array<Motion, MAX_MOTIONS> motions;
    double currentSampleRate = 0.0;
    int currentBlockSize = 0;

    Resampler resampler;
    Metronome metronome;
    juce::CriticalSection structureMutex;
    std::atomic<int> skippedBlocks { 0 };
    std::atomic<bool> silenceRequested { false };
    int silenceSamplesLeft = 0;         // audio thread only

    // Audio thread: every note off, every insert chain and send bus emptied.
    void silenceNow()
    {
        for (auto& e : engines)
            if (e) e->allNotesOff();
        for (auto& chain : trackInsertChains)
            chain.reset();
        for (auto& bus : sendBuses)
            bus.getChain().reset();
    }
public:
    // Stop everything that sounds: notes are released and effect tails cut at the next block.
    // The way out of any effect that rings on (Shift + Play).
    void requestSilence() { silenceRequested.store(true, std::memory_order_relaxed); }
    int getSkippedBlocks() const { return skippedBlocks.load(); }     // blocks silenced by a rebuild
private:
    std::vector<float> scratchL;
    std::vector<std::vector<float>> trackTapL, trackTapR;     // each track's output after its inserts (sidechain sources)
    std::vector<float> scratchR;

    std::vector<SendBus> sendBuses;
    std::vector<EffectType> sendBusTypes;

    ParametricEQ masterEQ;
    Compressor masterComp;
    Limiter masterLimiter;
};
