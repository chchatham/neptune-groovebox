#pragma once

#include "ScreenManager.h"
#include "../engine/AudioEngine.h"
#include "../project/ProjectBridge.h"
#include "../controller/CustomScaleManager.h"

// -----------------------------------------------------------------------------
// Binds what ScreenManager's buttons need from the sound side to the real
// AudioEngine: the per-track arpeggiators (Arp button, Shift + Arp latch), a
// track's whole sound (Func + Record / Play / > on PARAM and TRK FX) and the
// tempo (tap tempo). ScreenManager itself stays free of the engine headers.
// -----------------------------------------------------------------------------
namespace GrammarHooks
{
    // Scale lock (the Scale button). The scale is the set of pitch classes on the active scale
    // page's sixteen pads; a key plays the nearest of them (the lower one when two are equally
    // near). A chromatic page changes nothing.
    inline int snapToScale(const CustomScaleManager& scales, int note)
    {
        bool allowed[12] = {};
        for (int pad = 0; pad < CustomScaleManager::PADS_PER_PAGE; ++pad)
            allowed[((scales.getNoteForPad(pad) % 12) + 12) % 12] = true;
        for (int distance = 0; distance < 12; ++distance)
        {
            const int down = note - distance, up = note + distance;
            if (down >= 0 && allowed[down % 12]) return down;
            if (up <= 127 && allowed[up % 12])   return up;
        }
        return note;
    }

    // "C MAJOR": the active scale page's root and name, for the name bar.
    inline juce::String scaleLabel(const CustomScaleManager& scales)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int root = ((scales.getRootNote() % 12) + 12) % 12;
        return juce::String(names[root]) + " " + juce::String(scales.getPageName(scales.getActivePage())).toUpperCase();
    }

    inline ScreenManager::EngineHooks make(SequencerCore& sequencer, AudioEngine& audio,
                                           std::function<void(float)> tempoChanged = nullptr)
    {
        ScreenManager::EngineHooks h;
        auto* seq = &sequencer;
        auto* eng = &audio;

        h.arpEnabled = [eng](int t) { auto* a = eng->getTrackArpeggiator(t); return a != nullptr && a->isEnabled(); };
        h.setArpEnabled = [eng](int t, bool on) { if (auto* a = eng->getTrackArpeggiator(t)) a->setEnabled(on); };
        h.arpLatch = [eng](int t) { auto* a = eng->getTrackArpeggiator(t); return a != nullptr && a->getLatch(); };
        h.setArpLatch = [eng](int t, bool on) { if (auto* a = eng->getTrackArpeggiator(t)) a->setLatch(on); };

        h.captureSound = [seq, eng](int t)
        {
            ProjectBridge::Live live { *seq, *eng };
            return ProjectBridge::captureTrackSound(live, t, seq->getPattern().getTrack(t).soundParams);
        };

        // The same steps as loading a project does for one track: a fresh engine of the type, the
        // stored sound in every pattern (one sound per track), the insert chain, every parameter.
        h.applySound = [seq, eng](int t, EngineType type, const ParamMap& params)
        {
            if (eng->getEngine(t) == nullptr)
                return;
            eng->setEngineType(t, type, true);
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
            {
                if (t >= seq->getPattern(p).getTrackCount()) continue;
                auto& track = seq->getPattern(p).getTrack(t);
                track.engineType = type;
                track.soundParams = params;
            }

            while (eng->getInsertChain(t) != nullptr && eng->getInsertChain(t)->getEffectCount() > 0)
                eng->removeInsertEffect(t, 0);
            for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
            {
                auto it = params.find(ProjectBridge::INSERT_TYPE_BASE + slot);
                if (it == params.end() || it->second < 0.5f)
                    break;
                const int type1 = juce::jlimit(1, (int) EffectType::NumEffectTypes, (int) std::lround(it->second));
                eng->addInsertEffect(t, static_cast<EffectType>(type1 - 1));
            }
            eng->applyAllTrackParameters(t, params);
        };

        // The effects part of a sound (Func + Record / Play / > on TRK FX): the insert slots (their
        // parameters, types and switches), the DIRT / TALK / MODFX blocks, the arp locks, and the
        // pressure ranges set on any of those. Not the mixer: VOL, PAN and the sends stay.
        h.isFxParam = [](int id)
        {
            const int target = RangeParam::isRangeParam(id) ? RangeParam::targetOf(id) : id;
            return (target >= AudioEngine::EFFECT_PARAM_BASE
                    && target < ProjectBridge::INSERT_TYPE_BASE + AudioEngine::MAX_INSERTS_PER_TRACK)
                || TrackFxParam::isTrackFxParam(target) || AudioEngine::isArpLockParam(target)
                || AudioEngine::isInsertSwitchParam(target);
        };
        // Cleared: the three blocks at their defaults (off), no insert slots, the arp locks off.
        h.fxDefaults = [](ParamMap& sound)
        {
            const TrackFx defaults;
            for (int id = TrackFxParam::DIRT_BASE; id < TrackFxParam::END; ++id)
                sound[id] = defaults.get(id);
            sound[AudioEngine::ARP_LOCK_BASE + 0] = -0.02f;     // gate: follow the arpeggiator
            sound[AudioEngine::ARP_LOCK_BASE + 1] = -0.02f;     // velocity
            sound[AudioEngine::ARP_LOCK_BASE + 2] = 0.0f;       // ratchet count
        };
        h.engineType = [seq, eng](int t)
        {
            if (auto* e = eng->getEngine(t))
                return e->getType();
            return t >= 0 && t < seq->getPattern().getTrackCount() ? seq->getPattern().getTrack(t).engineType
                                                                   : EngineType::UnifiedVoice;
        };
        // Whatever replaces steps wholesale (a pattern cleared, an undo) keeps the audio thread out.
        h.structureLocked = [eng](const std::function<void()>& work)
        {
            const AudioEngine::StructureLock lock(eng->structure());
            work();
        };

        h.tempoChanged = std::move(tempoChanged);
        return h;
    }
}
