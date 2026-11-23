#pragma once

#include "Preset.h"
#include "ProjectBridge.h"
#include "TrackPresetNames.h"
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

// -----------------------------------------------------------------------------
// A multi preset: several tracks that belong together (a kick and the pad it
// ducks, a source and the track that hears it), loaded in one go into the
// earliest EMPTY tracks of the project.
//
//   members     each a Preset (one track's sound) and, optionally, a starter
//               pattern: that track's steps and length, in the form a project
//               file holds steps (TrackData::StepData)
//   playMember  the member the keys play after loading (1-based)
//
// Routing between the members is stored by MEMBER NUMBER (1-based): a member's
// input source (TrackInParam::SRC) and the SRC cell of any DUCK insert in its
// chain. They are rewritten to real track numbers when the multi is loaded
// (membersToTracks) and back when one is saved (tracksToMembers). A reference to
// something outside the multi becomes 0: no input / SELF.
//
// Files: <name>.multi.json in the presets folder. What makes a file a multi is
// its content (a "members" list), not its name: a single preset never has one.
// -----------------------------------------------------------------------------
struct MultiPreset
{
    static constexpr int VERSION = 1;
    static constexpr int MAX_MEMBERS = 16;          // (DUCK's SRC reaches sixteen tracks)
    static constexpr int MAX_PATTERN_STEPS = 1024;
    static const char* categoryName() { return "MULT"; }
    static const char* fileSuffix() { return ".multi.json"; }

    struct Member
    {
        Preset preset;
        bool hasPattern = false;                    // false: the track's steps are left as they are
        int length = 16;
        std::vector<TrackData::StepData> steps;
    };

    juce::String name;
    juce::String category = categoryName();
    int version = VERSION;
    std::vector<Member> members;
    int playMember = 1;
    bool isFactory = false;
    // Not stored: names the file holds that their lists no longer have (said once, in the app's log).
    juce::StringArray lost;

    int memberCount() const { return (int) members.size(); }
    bool isValid() const { return name.isNotEmpty() && !members.empty() && memberCount() <= MAX_MEMBERS; }
    // 0-based, always a member that exists (0 for a multi without members).
    int playIndex() const { return juce::jlimit(0, juce::jmax(0, memberCount() - 1), playMember - 1); }

    // ---- The file ---------------------------------------------------------------------------------
    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("name", name);
        obj->setProperty("category", category);
        obj->setProperty("version", version);
        obj->setProperty("isFactory", isFactory);
        obj->setProperty("playMember", playMember);

        juce::Array<juce::var> list;
        for (auto& m : members)
        {
            auto* mo = new juce::DynamicObject();
            mo->setProperty("preset", m.preset.toVar());
            if (m.hasPattern)
            {
                mo->setProperty("length", m.length);
                juce::Array<juce::var> steps;
                for (auto& s : m.steps)
                    steps.add(s.toVar((int) m.preset.engineType));
                mo->setProperty("steps", steps);
            }
            list.add(juce::var(mo));
        }
        obj->setProperty("members", list);
        return juce::var(obj);
    }

    // Is this parsed file a multi preset? (A single preset has no "members".)
    static bool isMultiVar(const juce::var& v)
    {
        auto* obj = v.getDynamicObject();
        return obj != nullptr && obj->getProperty("members").isArray();
    }

    // What the file holds, brought inside what a multi can be: at most MAX_MEMBERS members, engine
    // types that exist, a play member that exists, lengths a track can have. A file that is not a
    // multi gives one without a name (isValid() false).
    static MultiPreset fromVar(const juce::var& v)
    {
        MultiPreset m;
        if (!isMultiVar(v))
            return m;
        auto* obj = v.getDynamicObject();
        m.name = obj->getProperty("name").toString();
        m.category = categoryName();
        m.isFactory = (bool) obj->getProperty("isFactory");
        if (auto ver = obj->getProperty("version"); !ver.isVoid())
            m.version = juce::jmax(1, (int) ver);

        for (auto& mv : *obj->getProperty("members").getArray())
        {
            if (m.memberCount() >= MAX_MEMBERS)
                break;
            auto* mo = mv.getDynamicObject();
            if (mo == nullptr)
                continue;
            Member member;
            // (Each member's sound says by its own "version" how its positions are stored - by name
            // since format 3, StoredLists.h - and its steps' locks are stored as its sound is.)
            member.preset = Preset::fromVar(mo->getProperty("preset"), true);
            member.preset.isFactory = false;
            const int type = (int) member.preset.engineType;
            if (auto* steps = mo->getProperty("steps").getArray())
            {
                member.hasPattern = true;
                for (auto& sv : *steps)
                    if ((int) member.steps.size() < MAX_PATTERN_STEPS)
                    {
                        member.steps.push_back(TrackData::StepData::fromVar(sv, member.preset.fileVersion, type, &member.preset.lost));
                        if (member.preset.fileVersion < Preset::FORMAT_VERSION)
                            stored::locksOfBefore(type, member.preset.oldLayout, member.steps.back().paramLocks, member.preset.lost);
                    }
                const auto len = mo->getProperty("length");
                member.length = juce::jlimit(1, MAX_PATTERN_STEPS, len.isVoid() ? (int) member.steps.size() : (int) len);
            }
            if (type < 0 || type >= ENGINE_TYPE_COUNT)
                member.preset.engineType = EngineType::UnifiedVoice;
            for (auto& line : member.preset.lost)
                m.lost.addIfNotAlreadyThere(line);
            m.members.push_back(std::move(member));
        }
        m.playMember = juce::jlimit(1, juce::jmax(1, m.memberCount()), (int) obj->getProperty("playMember"));
        stored::report("multi preset " + m.name, m.lost);
        return m;
    }

    juce::String toJSON() const { return juce::JSON::toString(toVar()); }
    static MultiPreset fromJSON(const juce::String& json) { return fromVar(juce::JSON::parse(json)); }

    // ---- Routing: the places in a stored sound that name another track ---------------------------
    // TrackInParam::SRC and, for every keyed insert in the chain (DUCK's SRC, CMP / TRNS / WAH's
    // KEY, every LFO effect's KEY, FilterEffect's KEY), the cell that names a track.
    // `fn(int number)` returns the number to store instead.
    template <typename Fn>
    static void rewriteSources(ParamMap& sound, Fn&& fn)
    {
        auto rewrite = [&](int id)
        {
            auto it = sound.find(id);
            if (it == sound.end())
                return;
            const int now = std::isfinite(it->second) ? (int) std::lround(juce::jlimit(0.0f, 1000.0f, it->second)) : 0;
            it->second = now > 0 ? (float) juce::jmax(0, (int) fn(now)) : 0.0f;
        };
        rewrite(TrackInParam::SRC);
        for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
        {
            auto type = sound.find(ProjectBridge::INSERT_TYPE_BASE + slot);
            if (type == sound.end() || type->second < 0.5f)
                break;          // (a chain is filled from its first slot: the first empty one ends it)
            const int typeIdx = (int) std::lround(type->second) - 1;
            if (typeIdx < 0 || typeIdx >= (int) EffectType::NumEffectTypes)
                continue;
            const int keyId = AudioEngine::effectKeyParamId((EffectType) typeIdx, slot);
            if (keyId >= 0)
                rewrite(keyId);
        }
    }

    // The sources a stored sound names (track or member numbers, 1-based; none of them 0).
    static std::vector<int> sourcesOf(const ParamMap& sound)
    {
        std::vector<int> found;
        ParamMap copy = sound;
        rewriteSources(copy, [&](int n) { found.push_back(n); return n; });
        return found;
    }

    // Loading: member numbers become the tracks the members landed on (`tracks[m]` = the 0-based
    // track of member m + 1). A number that is no member becomes 0.
    static void membersToTracks(ParamMap& sound, const std::vector<int>& tracks)
    {
        rewriteSources(sound, [&](int member)
        {
            return member >= 1 && member <= (int) tracks.size() ? tracks[(size_t) (member - 1)] + 1 : 0;
        });
    }

    // Saving: track numbers become member numbers. A track outside the set becomes 0.
    static void tracksToMembers(ParamMap& sound, const std::vector<int>& tracks)
    {
        rewriteSources(sound, [&](int trackNumber)
        {
            for (size_t m = 0; m < tracks.size(); ++m)
                if (tracks[m] + 1 == trackNumber)
                    return (int) m + 1;
            return 0;
        });
    }

    // A single preset never carries a track's input (TRK FX page IN): which track feeds it is the
    // project's business, and only a multi can say it in a way that survives being loaded elsewhere.
    static void stripTrackInput(ParamMap& sound)
    {
        for (auto it = sound.begin(); it != sound.end();)
            it = TrackInParam::contains(it->first) ? sound.erase(it) : std::next(it);
    }
};

// -----------------------------------------------------------------------------
// EMPTY TRACKS. A multi preset loads only into empty tracks, so that it can never
// take anything away. A track is empty when ALL of these hold:
//
//   1. no step of it has a trig, in any of the patterns of the bank;
//   2. no preset is named for it (TrackPresetNames, for the engine it has now:
//      a kit loaded with LD is the player's choice even when it equals the
//      engine's defaults);
//   3. its sound is as a new project or a cleared sound leaves it:
//        - every engine parameter is at the default of the engine type the
//          track has NOW (so a changed engine type alone, TRACKS -> ENG, does
//          not make a track non-empty, and track 2's drum kit in a new project
//          is empty);
//        - no insert effect, and the DIRT / TALK / MODFX blocks at their
//          defaults (a preset replaces those: with them set there is something
//          to lose);
//        - no input source (TrackInParam::SRC).
//      The mixer (volume, pan, sends) does not count: a preset load leaves it
//      alone, and so does a clear.
//
// The engine is asked, not Track::soundParams: a project that was saved and
// loaded holds every value of every track in that map, touched or not, and a
// turned knob need not be in it at all. The defaults are read once per engine
// type from a fresh engine and kept.
// -----------------------------------------------------------------------------
class EmptyTracks
{
public:
    enum class Reason { Empty, NoTrack, Trigs, Named, Sound, Effects, Input };

    template <typename Fn>
    static void forEachEngineId(Fn&& fn)
    {
        for (int id = 0; id < EngineParamRange::END; ++id)
            if (EngineParamRange::contains(id))
                fn(id);
    }

    // Every engine parameter of a fresh engine of this type (its pressure slots among them: an
    // engine is made with its default routes).
    const std::vector<float>& defaultsOf(EngineType type)
    {
        const int index = juce::jlimit(0, ENGINE_TYPE_COUNT - 1, (int) type);
        auto& values = defaults[(size_t) index];
        if (values.empty())
            if (auto engine = AudioEngine::createEngine((EngineType) index))
                forEachEngineId([&](int id) { values.push_back(engine->getParameter(id)); });
        return values;
    }

    // Why track `t` is not empty (Reason::Empty when it is). `audio` may be null (a screen used
    // without an engine): the stored parameters are then all there is.
    Reason reasonFor(const SequencerCore& seq, const AudioEngine* audio, const TrackPresetNames* names, int t)
    {
        const auto& active = seq.getPattern();
        if (t < 0 || t >= active.getTrackCount())
            return Reason::NoTrack;

        for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
        {
            const auto& pattern = seq.getPattern(p);
            if (t >= pattern.getTrackCount())
                continue;
            for (auto& step : pattern.getTrack(t).steps)
                if (step.trig)
                    return Reason::Trigs;
        }

        const auto& track = active.getTrack(t);
        const EngineBase* engine = audio != nullptr ? audio->getEngine(t) : nullptr;
        if (audio != nullptr && engine == nullptr)
            return Reason::NoTrack;                 // (nothing could be loaded there)
        const EngineType type = engine != nullptr ? engine->getType() : track.engineType;

        if (names != nullptr && names->nameFor(t, type).isNotEmpty())
            return Reason::Named;

        const auto& fresh = defaultsOf(type);
        size_t i = 0;
        bool same = true;
        forEachEngineId([&](int id)
        {
            if (i < fresh.size())
            {
                if (engine != nullptr)
                    same = same && engine->getParameter(id) == fresh[i];
                else if (auto it = track.soundParams.find(id); it != track.soundParams.end())
                    same = same && it->second == fresh[i];
            }
            ++i;
        });
        if (!same)
            return Reason::Sound;

        if (audio != nullptr)
        {
            EffectType first {};
            if (audio->getInsertEffectType(t, 0, first))
                return Reason::Effects;
            if (std::lround(audio->getTrackParameter(t, TrackInParam::SRC)) != 0)
                return Reason::Input;
        }
        else
        {
            if (auto it = track.soundParams.find(ProjectBridge::INSERT_TYPE_BASE); it != track.soundParams.end() && it->second >= 0.5f)
                return Reason::Effects;
            // Old TrackFx params (240-263) in stored data: any non-default means effects in use.
            for (int id = TrackFxParam::DIRT_BASE; id < TrackFxParam::END; ++id)
                if (auto it = track.soundParams.find(id); it != track.soundParams.end() && it->second != 0.0f)
                    return Reason::Effects;
        }
        if (auto it = track.soundParams.find(TrackInParam::SRC); it != track.soundParams.end() && std::lround(it->second) != 0)
            return Reason::Input;
        return Reason::Empty;
    }

    bool isEmpty(const SequencerCore& seq, const AudioEngine* audio, const TrackPresetNames* names, int t)
    {
        return reasonFor(seq, audio, names, t) == Reason::Empty;
    }

    // The empty tracks, ascending.
    std::vector<int> find(const SequencerCore& seq, const AudioEngine* audio, const TrackPresetNames* names)
    {
        std::vector<int> out;
        for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
            if (isEmpty(seq, audio, names, t))
                out.push_back(t);
        return out;
    }

    // Where `count` members would land: the earliest empty tracks in ascending order, member order
    // kept (they need not be adjacent). Fewer empty tracks than members: an empty list, and
    // `available` says how many there are.
    std::vector<int> landing(const SequencerCore& seq, const AudioEngine* audio, const TrackPresetNames* names,
                             int count, int* available = nullptr)
    {
        auto empty = find(seq, audio, names);
        if (available != nullptr)
            *available = (int) empty.size();
        if (count <= 0 || (int) empty.size() < count)
            return {};
        empty.resize((size_t) count);
        return empty;
    }

    // A sound as a clear leaves it (ScreenManager's CLEAR on PARAM / FLTR): no engine parameter in
    // it at all. The app drops the track's preset name when it applies one, so that a cleared track
    // counts as empty again.
    static bool isClearedSound(const ParamMap& sound)
    {
        for (auto& [id, value] : sound)
        {
            juce::ignoreUnused(value);
            if (EngineParamRange::contains(id))
                return false;
        }
        return true;
    }

private:
    std::array<std::vector<float>, ENGINE_TYPE_COUNT> defaults;
};

// -----------------------------------------------------------------------------
// The name bar's words for where a multi would land, within `maxChars` characters
// (the bar holds 20 with the screen's name and track): "3TRK>T7-9" for tracks
// side by side, "T3 7 8" when they are not; shorter forms when those do not fit
// ("T7-9", "T3 5-8", "T3.."). `tracks` are 0-based and ascending.
// -----------------------------------------------------------------------------
namespace MultiLanding
{
    inline juce::String text(const std::vector<int>& tracks, int maxChars = 10)
    {
        if (tracks.empty())
            return {};
        const int n = (int) tracks.size();
        const int first = tracks.front() + 1, last = tracks.back() + 1;
        const bool adjacent = last - first == n - 1;

        juce::String each = "T", runs = "T";
        for (int i = 0; i < n; ++i)
            each << (i > 0 ? " " : "") << (tracks[(size_t) i] + 1);
        for (int i = 0; i < n; ++i)
        {
            int end = i;
            while (end + 1 < n && tracks[(size_t) (end + 1)] == tracks[(size_t) end] + 1)
                ++end;
            runs << (runs.length() > 1 ? " " : "") << (tracks[(size_t) i] + 1);
            if (end > i + 1)       { runs << "-" << (tracks[(size_t) end] + 1); i = end; }
        }

        juce::StringArray forms;
        if (n == 1)
            forms.add("1TRK>T" + juce::String(first));
        else if (adjacent)
        {
            forms.add(juce::String(n) + "TRK>T" + juce::String(first) + "-" + juce::String(last));
            forms.add("T" + juce::String(first) + "-" + juce::String(last));
        }
        else
        {
            forms.add(each);
            forms.add(runs);
        }
        forms.add("T" + juce::String(first) + "..");
        for (auto& form : forms)
            if (form.length() <= maxChars)
                return form;
        return forms[forms.size() - 1].substring(0, juce::jmax(0, maxChars));
    }

    inline juce::String needText(int members) { return "NEED " + juce::String(members) + " EMPTY"; }
}
