#pragma once
#include <juce_core/juce_core.h>
#include "../src/engine/ParamSpec.h"
#include "../src/engine/AudioEngine.h"
#include "../src/project/FactoryPresets.h"
#include "../src/sequencer/SequencerCore.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include "fast_measure.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <cstring>
#include <fstream>
#include <vector>

// -----------------------------------------------------------------------------
// Mod slots (SRC, DEST, AMT, VIA on pages SLT1 ...) and the custom LFO shape (LSHP,
// shapes USR and USRS), on the seven sound engines: src/engine/EngineModSection.h.
//
// Every engine is driven through EngineBase alone: parameters by id, notes, pad
// pressure, the strip, and the track input's follower through setAudioInput.
// "The same sound" is the same samples; "moved" is a difference of the renders
// (sum |a - b| / sum (|a| + |b|)) or a level ratio over the same stretch of both.
//
// Two tools ride along (they write and return; the tests do not run):
//   NEPTUNE_MODSLOT_CHECKSUMS=<file>   a checksum of a fixed render of every factory preset, of
//       every engine's default sound and of every LFO destination of every engine: two trees
//       make the same sound without slots exactly when their files are the same.
//   NEPTUNE_MODSLOT_PAGES=1            every engine's pages and LFO destination names.
// -----------------------------------------------------------------------------
class ModSlotsTests : public juce::UnitTest
{
public:
    ModSlotsTests() : UnitTest("Mod Slots") {}

    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;

    // ---- The "nothing changed" tool -----------------------------------------------------------
    // NEPTUNE_MODSLOT_CHECKSUMS=<file> writes one line a render: every factory preset, every
    // engine's default sound, and every engine with its LFOs on every destination in turn (with
    // the strip up and pad pressure on one note). Two trees render the same sound exactly when
    // their files are the same.
    static void makeRepeatable(EngineBase& e)
    {
        if (auto* u = dynamic_cast<UnifiedVoiceEngine*>(&e))
            for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                u->getVoice(v).getAnalogCharacter().enabled = false;
    }

    static std::shared_ptr<EngineBase> makeEngine(EngineType type, const ParamMap* sound = nullptr, double sr = SR, int block = BLOCK)
    {
        std::shared_ptr<EngineBase> e = AudioEngine::createEngine(type);
        e->prepareToPlay(sr, block);
        if (sound != nullptr)
        {
            const int master = e->getMasterParamId();
            auto it = master >= 0 ? sound->find(master) : sound->end();
            if (it != sound->end())
                e->setParameter(master, it->second);
            for (auto& [id, value] : *sound)
                if (id != master && EngineParamRange::contains(id))
                    e->setParameter(id, value);
        }
        makeRepeatable(*e);
        return e;
    }

    static uint64_t hashRender(EngineBase& e, double seconds, float wheel, bool pressure)
    {
        const bool kit = e.getType() == EngineType::Drum;
        e.setModWheel(wheel);
        juce::AudioBuffer<float> buf(2, BLOCK);
        juce::MidiBuffer midi;
        uint64_t h = 1469598103934665603ull;
        const int blocks = (int) (seconds * SR / BLOCK);
        int hit = 0;
        for (int block = 0; block < blocks; ++block)
        {
            if (kit && block % 40 == 0 && hit < 16)
                e.noteOn(hit++, 0.9f);
            else if (! kit && block == 0)
                for (int note : { 48, 63 })
                    e.noteOn(note, 0.8f);
            if (pressure && block == 30)
                e.aftertouch(kit ? 0 : 63, 0.7f);
            if (! kit && block == (blocks * 3) / 4)
                for (int note : { 48, 63 })
                    e.noteOff(note);
            buf.clear();
            e.processBlock(buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < BLOCK; ++i)
                {
                    uint32_t bits;
                    const float s = buf.getSample(ch, i);
                    std::memcpy(&bits, &s, sizeof(bits));
                    for (int b = 0; b < 4; ++b)
                    {
                        h ^= (bits >> (8 * b)) & 0xffu;
                        h *= 1099511628211ull;
                    }
                }
        }
        return h;
    }

    // The ids of an engine's LFO cells: { rate, depth, shape, dest } of LFO `lfo`, and how many
    // destinations its DEST cell offers.
    struct LfoIds { int rate, depth, shape, dest, numDests; };
    static LfoIds lfoIds(EngineBase& e, int lfo)
    {
        LfoIds ids {};
        if (e.getType() == EngineType::UnifiedVoice)
            ids = { ModParam::lfoParamId(lfo, ModParam::LFO_RATE), ModParam::lfoParamId(lfo, ModParam::LFO_DEPTH),
                    ModParam::lfoParamId(lfo, ModParam::LFO_SHAPE), ModParam::lfoParamId(lfo, ModParam::LFO_DEST), 0 };
        else
            ids = { 158 + lfo * 8, 159 + lfo * 8, 160 + lfo * 8, 165 + lfo * 8, 0 };
        for (auto& page : e.getParamPages())
            if (const auto* spec = page.find(ids.dest))
                ids.numDests = spec->numChoices;
        if (e.getType() == EngineType::UnifiedVoice)
            ids.numDests = (int) UnifiedVoice::LfoDest::Count;      // (its LFO pages are the screen's)
        return ids;
    }

    static const std::vector<EngineType>& soundEngines()
    {
        static const std::vector<EngineType> types = { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc,
            EngineType::Wavetable, EngineType::WaveSeq, EngineType::Sampler, EngineType::Drum };
        return types;
    }

    static void writeChecksums(const juce::String& path)
    {
        std::ofstream out(path.toStdString());
        auto line = [&out](const juce::String& name, uint64_t h)
        {
            out << name.toStdString() << " " << juce::String::toHexString((juce::int64) h).toStdString() << "\n";
        };
        for (auto& p : FactoryPresets::getAll())
        {
            auto e = makeEngine(p.engineType, &p.soundParams);
            line("preset " + p.name.replaceCharacter(' ', '_'), hashRender(*e, 1.0, 0.0f, false));
            auto e2 = makeEngine(p.engineType, &p.soundParams);
            line("preset+strip " + p.name.replaceCharacter(' ', '_'), hashRender(*e2, 0.6, 0.8f, true));
        }
        for (EngineType type : soundEngines())
        {
            const juce::String tn((int) type);
            {
                auto e = makeEngine(type);
                line("default " + tn, hashRender(*e, 1.0, 0.0f, false));
                auto e2 = makeEngine(type);
                line("default+strip " + tn, hashRender(*e2, 1.0, 0.7f, true));
            }
            auto probe = makeEngine(type);
            const int n = lfoIds(*probe, 0).numDests;
            for (int d = 1; d < n; ++d)
            {
                auto e = makeEngine(type);
                if (type == EngineType::UnifiedVoice)
                    e->setParameter(ModParam::LFO_COUNT, 2.0f);
                const auto a = lfoIds(*e, 0), b = lfoIds(*e, 1);
                e->setParameter(a.rate, 7.0f);
                e->setParameter(a.depth, d % 2 == 0 ? 0.45f : -0.6f);
                e->setParameter(a.shape, (float) (d % 9));
                e->setParameter(a.dest, (float) d);
                e->setParameter(b.rate, 3.3f);
                e->setParameter(b.depth, 0.35f);
                e->setParameter(b.shape, (float) ((d + 4) % 9));
                e->setParameter(b.dest, (float) (1 + (d * 7) % (n - 1)));
                line("lfo " + tn + " dest " + juce::String(d), hashRender(*e, 0.35, d % 3 == 0 ? 0.6f : 0.0f, d % 2 == 1));
            }
            // The unified voice's extra envelopes on every destination of theirs.
            if (type == EngineType::UnifiedVoice)
                for (int d = 0; d < UnifiedVoice::NUM_ENV_DESTS; ++d)
                {
                    auto e = makeEngine(type);
                    e->setParameter(ModParam::ENV_COUNT, 1.0f);
                    e->setParameter(ModParam::envParamId(0, ModParam::ENV_DEC), 0.2f);
                    e->setParameter(ModParam::envParamId(0, ModParam::ENV_DEPTH), 0.6f);
                    e->setParameter(ModParam::envParamId(0, ModParam::ENV_TARGET), (float) d);
                    line("menv dest " + juce::String(d), hashRender(*e, 0.35, 0.0f, d % 2 == 1));
                }
        }
    }

    // ---- Playing and measuring ------------------------------------------------------------------
    static juce::String engineName(EngineType type)
    {
        static const char* const names[] = { "synth", "FM", "macro", "wavetable", "wave seq", "sampler", "drums" };
        return names[(int) type];
    }

    // (A DEST cell's list without the cells of the track's effects at its end: those are E4's,
    // mod::TrackDest, pinned and played in "Effect Destinations".)
    static std::vector<juce::String> choicesOf(EngineBase& e, int cellId)
    {
        std::vector<juce::String> out;
        for (auto& page : e.getParamPages())
            if (const auto* spec = page.find(cellId))
            {
                for (int i = 0; i < mod::TrackDest::firstIn(spec->choices, spec->numChoices); ++i)
                    out.push_back(spec->choices[i]);
                break;
            }
        return out;
    }

    // The names of the LFOs' DEST cell (the unified voice's LFO pages are the screen's).
    static std::vector<juce::String> lfoDestNames(EngineBase& e)
    {
        if (e.getType() != EngineType::UnifiedVoice)
            return choicesOf(e, EngineLfoSection::id(0, EngineLfoSection::Dest));
        std::vector<juce::String> out;
        for (int i = 0; i < (int) UnifiedVoice::LfoDest::Count; ++i)
            out.push_back(UnifiedVoice::lfoDestLabels()[i]);
        return out;
    }
    static std::vector<juce::String> slotDestNames(EngineBase& e) { return choicesOf(e, ModSlotParam::id(0, ModSlotParam::DEST)); }

    static uint32_t hashNames(const std::vector<juce::String>& names)
    {
        juce::StringArray all;
        for (auto& n : names)
            all.add(n);
        uint32_t h = 2166136261u;
        for (auto c : all.joinIntoString(" ").toStdString())
            h = (h ^ (uint8_t) c) * 16777619u;
        return h;
    }

    static int indexOf(const std::vector<juce::String>& names, const juce::String& label)
    {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == label)
                return (int) i;
        return -1;
    }

    void setSlot(EngineBase& e, int slot, int src, const char* dest, float amount, int via = ModSrc::Off)
    {
        const int index = indexOf(slotDestNames(e), dest);
        expect(index > 0, engineName(e.getType()) + " has no slot destination " + dest);
        e.setParameter(ModSlotParam::id(slot, ModSlotParam::DEST), (float) std::max(0, index));
        e.setParameter(ModSlotParam::id(slot, ModSlotParam::AMT), amount);
        e.setParameter(ModSlotParam::id(slot, ModSlotParam::VIA), (float) via);
        e.setParameter(ModSlotParam::id(slot, ModSlotParam::SRC), (float) src);
    }

    static void setLfoRate(EngineBase& e, int lfo, float hz)
    {
        if (e.getType() == EngineType::UnifiedVoice)
        {
            if (e.getParameter(ModParam::LFO_COUNT) < (float) (lfo + 1))
                e.setParameter(ModParam::LFO_COUNT, (float) (lfo + 1));
            e.setParameter(ModParam::lfoParamId(lfo, ModParam::LFO_RATE), hz);
        }
        else
            e.setParameter(EngineLfoSection::id(lfo, EngineLfoSection::Rate), hz);
    }

    // trigger: 0 FREE, 1 KEY.
    void setLfo(EngineBase& e, int lfo, const char* dest, float depth, int shape, float hz, int trigger = 0)
    {
        const int index = juce::String(dest) == "OFF" ? 0 : indexOf(lfoDestNames(e), dest);
        expect(index >= 0, engineName(e.getType()) + " has no LFO destination " + dest);
        setLfoRate(e, lfo, hz);
        if (e.getType() == EngineType::UnifiedVoice)
        {
            e.setParameter(ModParam::lfoParamId(lfo, ModParam::LFO_SHAPE), (float) shape);
            e.setParameter(ModParam::lfoParamId(lfo, ModParam::LFO_TRIG), (float) trigger);
            e.setParameter(ModParam::lfoParamId(lfo, ModParam::LFO_DEPTH), depth);
            e.setParameter(ModParam::lfoParamId(lfo, ModParam::LFO_DEST), (float) std::max(0, index));
            return;
        }
        using S = EngineLfoSection;
        e.setParameter(S::id(lfo, S::Shape), (float) shape);
        e.setParameter(S::id(lfo, S::Trigger), (float) trigger);
        e.setParameter(S::id(lfo, S::Depth), depth);
        e.setParameter(S::id(lfo, S::Dest), (float) std::max(0, index));
    }

    // The page cell behind a destination name.
    static int keyedCell(EngineBase& e, const char* dest)
    {
        if (e.getType() == EngineType::UnifiedVoice)
            return UnifiedVoice::paramOfDest(indexOf(lfoDestNames(e), dest));
        for (auto& page : e.getParamPages())
            for (auto& cell : page.params)
                if (cell.isValid() && juce::String(cell.label) == dest)
                    return cell.id;
        return -1;
    }

    // A sound whose level holds while the note does (the windows of a render are compared).
    static void steady(EngineBase& e)
    {
        if (e.getType() == EngineType::FM)
            for (int op = 0; op < 6; ++op)
                e.setParameter(op * 10 + 6, 1.0f);      // every operator's sustain
        if (e.getType() == EngineType::UnifiedVoice)
            e.setParameter(UnifiedParam::AMP_SUSTAIN, 1.0f);
    }

    struct Play
    {
        double seconds = 0.35;
        std::vector<int> notes;         // empty: one note (48; on the kit pad 3, a held SUB)
        float velocity = 0.8f;
        float wheel = 0.0f;
        float pressure = -1.0f;         // >= 0: pad pressure from the first block on ...
        int pressedNote = -2;           // ... on this note (-2: every note, -1: none)
        float input = -1.0f;            // >= 0: the track input's follower, given before every block
        bool holdToEnd = false;         // otherwise the notes are let go after three quarters
        std::function<void (EngineBase&, int)> atBlock;
    };

    // Interleaved left / right.
    static std::vector<float> render(EngineBase& e, const Play& play)
    {
        const bool kit = e.getType() == EngineType::Drum;
        std::vector<int> notes = play.notes;
        if (notes.empty())
            notes.push_back(kit ? 2 : 48);
        e.setModWheel(play.wheel);
        juce::AudioBuffer<float> buf(2, BLOCK);
        juce::MidiBuffer midi;
        std::vector<float> out;
        const int blocks = (int) (play.seconds * SR / BLOCK);
        out.reserve((size_t) (blocks * BLOCK * 2));
        for (int block = 0; block < blocks; ++block)
        {
            if (block == 0)
            {
                for (int note : notes)
                    e.noteOn(note, play.velocity);
                if (play.pressure >= 0.0f && play.pressedNote != -1)
                    for (int note : notes)
                        if (play.pressedNote == -2 || play.pressedNote == note)
                            e.aftertouch(note, play.pressure);
                if (play.pressure >= 0.0f && play.pressedNote >= 0 && std::find(notes.begin(), notes.end(), play.pressedNote) == notes.end())
                    e.aftertouch(play.pressedNote, play.pressure);      // a pad that plays no note here
            }
            if (! play.holdToEnd && block == (blocks * 3) / 4)
                for (int note : notes)
                    e.noteOff(note);
            if (play.input >= 0.0f)
                e.setAudioInput({ nullptr, nullptr, 0, play.input });
            if (play.atBlock)
                play.atBlock(e, block);
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < BLOCK; ++i)
            {
                out.push_back(buf.getSample(0, i));
                out.push_back(buf.getSample(1, i));
            }
        }
        // Everything is let go and stopped: the engine can play the next render from silence.
        e.allNotesOff();
        e.setAudioInput({});
        return out;
    }

    static std::vector<float> mono(const std::vector<float>& stereo)
    {
        std::vector<float> out(stereo.size() / 2);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = 0.5f * (stereo[2 * i] + stereo[2 * i + 1]);
        return out;
    }

    static float rms(const std::vector<float>& x)
    {
        double sum = 0.0;
        for (float v : x)
            sum += (double) v * v;
        return x.empty() ? 0.0f : (float) std::sqrt(sum / (double) x.size());
    }

    static float peak(const std::vector<float>& x)
    {
        float top = 0.0f;
        for (float v : x)
            top = std::max(top, std::abs(v));
        return top;
    }

    // 0 = the same samples, 1 = nothing in common.
    static float difference(const std::vector<float>& a, const std::vector<float>& b)
    {
        double diff = 0.0, sum = 0.0;
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i)
        {
            diff += std::abs((double) a[i] - b[i]);
            sum += std::abs((double) a[i]) + std::abs((double) b[i]);
        }
        return sum > 0.0 ? (float) (diff / sum) : 0.0f;
    }

    // The largest move from one sample to the next (of the same channel) in x[from, to).
    static float steepest(const std::vector<float>& x, int from, int to)
    {
        float top = 0.0f;
        for (int i = std::max(from, 2); i < to && i < (int) x.size(); ++i)
            top = std::max(top, std::abs(x[(size_t) i] - x[(size_t) (i - 2)]));
        return top;
    }

    static bool finiteAndHeld(const std::vector<float>& x, float limit)
    {
        for (float v : x)
            if (! std::isfinite(v) || std::abs(v) > limit)
                return false;
        return true;
    }

    static double hz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

    void runTest() override
    {
        if (const auto path = juce::SystemStats::getEnvironmentVariable("NEPTUNE_MODSLOT_CHECKSUMS", {}); path.isNotEmpty())
        {
            beginTest("Checksums written");
            writeChecksums(path);
            expect(juce::File(path).existsAsFile());
            return;
        }

        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_MODSLOT_PAGES", {}).isNotEmpty())
        {
            beginTest("Pages dumped");
            for (EngineType type : soundEngines())
            {
                auto e = makeEngine(type);
                juce::String text = "ENGINE " + juce::String((int) type) + "\n";
                for (auto& page : e->getParamPages())
                {
                    text << "  " << page.name << " [" << page.section << "]:";
                    for (auto& c : page.params)
                        text << " " << (c.isValid() ? juce::String(c.label) + "=" + juce::String(c.id) : juce::String("----"));
                    text << "\n";
                }
                const auto ids = lfoIds(*e, 0);
                text << "  DESTS " << ids.numDests << ":";
                for (auto& page : e->getParamPages())
                    if (const auto* spec = page.find(ids.dest))
                        for (int i = 0; i < spec->numChoices; ++i)
                            text << " " << spec->choices[i];
                logMessage(text);
            }
            return;
        }

        beginTest("Sixteen slots of four ids from 700, inside the engine's fourth block");
        {
            for (int slot = 0; slot < ModSlotParam::MAX_SLOTS; ++slot)
                for (int offset : { ModSlotParam::SRC, ModSlotParam::DEST, ModSlotParam::AMT, ModSlotParam::VIA })
                {
                    const int id = ModSlotParam::id(slot, offset);
                    expectEquals(id, ModSlotParam::BASE + slot * ModSlotParam::STRIDE + offset);
                    expect(id < ModSlotParam::END && EngineParamRange::contains(id));
                }
        }

        beginTest("Index stability: every engine's LFO destinations, the LFO shapes and the sources are where they were");
        {
            // Count, first and last name and a hash of the whole list as they were at commit 1f457e9
            // (before the slots): saved sounds store these indices.
            struct Pin { EngineType type; int count; const char* first; const char* last; uint32_t hash; };
            const Pin pins[] = {
                { EngineType::UnifiedVoice, 75, "PTCH", "EDPT", 0x7af783feu },
                { EngineType::FM,           84, "PTCH", "PMAL", 0xd8bf56e9u },     // 75 to POS6 (0xbf2a84cd), then ETIM FTIM PM1 .. PM6 PMAL (U4-FM)
                { EngineType::MacroOsc,     45, "PTCH", "BWIR", 0u },     // +8 BDY2 cells (hash TBD)
                { EngineType::Wavetable,    42, "PTCH", "PM", 0x6ca2b20fu },       // 39 to DRVB (0xc0fb09f1), then ETIM, FTIM and PM (U4)
                { EngineType::WaveSeq,      94, "PTCH", "PM", 0u },      // +8 BDY2 cells (hash TBD)
                { EngineType::Sampler,      43, "PTCH", "ETIM", 0x5f706f0cu },     // 42 to LDPT (0xcee2b609), then ETIM (U4)
                { EngineType::Drum,         46, "PTCH", "FTIM", 0u } };           // 18 to LDPT (0x0f3d7e6f), then the resonator drum's 24 (U2b, 42: 0x58a8e361), then PM WPOS ETIM FTIM (U4-DRUM)
            for (auto& pin : pins)
            {
                auto e = makeEngine(pin.type);
                const juce::String name = engineName(pin.type);
                const auto lfo = lfoDestNames(*e);
                expectEquals((int) lfo.size(), pin.count, name + ": LFO destination count");
                if ((int) lfo.size() != pin.count)
                    continue;
                expectEquals(lfo[0], juce::String("OFF"), name);
                expectEquals(lfo[1], juce::String(pin.first), name + ": first destination");
                expectEquals(lfo.back(), juce::String(pin.last), name + ": last destination");
                if (pin.hash != 0u)     // (the drum kit's whole list is pinned up to HARD below, and its four new names by "U4 Drum")
                    expectEquals((juce::int64) hashNames(lfo), (juce::int64) pin.hash, name + ": the whole list");
                if (pin.type == EngineType::MacroOsc)
                {
                    // The envelope times were added at the end: what was there before them is where it was.
                    const std::vector<juce::String> before(lfo.begin(), lfo.begin() + 27);
                    expectEquals((juce::int64) hashNames(before), (juce::int64) 0x9c7ae467u, name + ": the list up to LDPT");
                    const std::vector<juce::String> toFtim(lfo.begin(), lfo.begin() + 29);
                    expectEquals((juce::int64) hashNames(toFtim), (juce::int64) 0xe5a0256au, name + ": the list up to FTIM");
                }
                if (pin.type == EngineType::FM)
                {
                    // The envelope times and the operators' phases were added at the end: what was there before them is where it was.
                    const std::vector<juce::String> before(lfo.begin(), lfo.begin() + 75);
                    expectEquals((juce::int64) hashNames(before), (juce::int64) 0xbf2a84cdu, name + ": the list up to POS6");
                    expectEquals(before.back(), juce::String("POS6"), name);
                }
                if (pin.type == EngineType::Wavetable)
                {
                    // The envelope times and the phase were added at the end: what was there before them is where it was.
                    const std::vector<juce::String> before(lfo.begin(), lfo.begin() + 39);
                    expectEquals((juce::int64) hashNames(before), (juce::int64) 0xc0fb09f1u, name + ": the list up to DRVB");
                }
                if (pin.type == EngineType::Sampler)
                {
                    const std::vector<juce::String> before(lfo.begin(), lfo.begin() + 42);
                    expectEquals((juce::int64) hashNames(before), (juce::int64) 0xcee2b609u, name + ": the list up to LDPT");
                }
                if (pin.type == EngineType::Drum)
                {
                    // The resonator drum's destinations were added at the end: what was there before them is where it was.
                    const std::vector<juce::String> before(lfo.begin(), lfo.begin() + 18);
                    expectEquals((juce::int64) hashNames(before), (juce::int64) 0x0f3d7e6fu, name + ": the list up to LDPT");
                    // The router's PM, WPOS, ETIM and FTIM were added after them: the 42 before are where they were.
                    const std::vector<juce::String> upToHard(lfo.begin(), lfo.begin() + 42);
                    expectEquals((juce::int64) hashNames(upToHard), (juce::int64) 0x58a8e361u, name + ": the list up to HARD");
                }
                if (pin.type == EngineType::WaveSeq)
                {
                    // BODY's destinations were added at the end: what was there before them is where it was.
                    const std::vector<juce::String> before(lfo.begin(), lfo.begin() + 75);
                    expectEquals((juce::int64) hashNames(before), (juce::int64) 0xd8cd2408u, name + ": the list up to T16");
                    // The router's ETIM, FTIM and PM were added after BODY's: the 83 before them are where they were.
                    const std::vector<juce::String> withBody(lfo.begin(), lfo.begin() + 83);
                    expectEquals((juce::int64) hashNames(withBody), (juce::int64) 0xa512a2efu, name + ": the list up to BKTK");
                }

                // A slot's DEST: the same names at the same indices, then what only a slot reaches.
                const auto slot = slotDestNames(*e);
                // (U4-UV: the unified voice is on the router: after its LFOs' list come PM RING SYNC
                // ETIM FTIM, then the four LFOs' L1RT .. L4DP; a slot's AMT1 .. moved by 13.)
                const bool unifiedVoice = pin.type == EngineType::UnifiedVoice;
                const int extra = unifiedVoice ? 5 + 8 : 4;
                expectEquals((int) slot.size(), pin.count + extra + 16, name + ": slot destination count");
                for (int i = 0; i < pin.count && i < (int) slot.size(); ++i)
                    expectEquals(slot[(size_t) i], lfo[(size_t) i], name + ": slot destination " + juce::String(i));
                if (unifiedVoice && (int) slot.size() == pin.count + extra + 16)
                {
                    const char* const added[5] = { "PM", "RING", "SYNC", "ETIM", "FTIM" };
                    for (int i = 0; i < 5; ++i)
                        expectEquals(slot[(size_t) (pin.count + i)], juce::String(added[i]), name);
                    expectEquals(slot[(size_t) pin.count + 5], juce::String("L1RT"), name);
                    expectEquals(slot[(size_t) pin.count + 12], juce::String("L4DP"), name);
                }
                else if (extra > 0)
                {
                    expectEquals(slot[(size_t) pin.count], juce::String("L1RT"), name);
                    expectEquals(slot[(size_t) pin.count + 3], juce::String("L2DP"), name);
                }
                // (These come after the engine's list: if that list grows, a saved slot aimed at one
                // of them moves. This pin is the reminder to write the migration first.)
                expectEquals(slot[(size_t) (pin.count + extra)], juce::String("AMT1"), name + ": the first slot amount follows the engine's list");
                expectEquals(slot.back(), juce::String("AM16"), name);
                std::set<juce::String> unique(slot.begin(), slot.end());
                expectEquals((int) unique.size(), (int) slot.size(), name + ": every slot destination has its own name");
            }

            const char* shapes[] = { "SIN", "TRI", "SQR", "SAW", "RND", "SRND", "RAMP", "EXPU", "EXPD", "USR", "USRS" };
            expectEquals((int) LFO::Shape::COUNT, 11);
            for (int i = 0; i < 11; ++i)
                expectEquals(juce::String(LFO::shapeLabels()[i]), juce::String(shapes[i]));
            expectEquals((int) LFO::Shape::ExpDown, 8);
            expectEquals((int) LFO::Shape::User, 9);
            expectEquals((int) LFO::Shape::UserSmooth, 10);

            const char* sources[] = { "OFF", "LFO1", "LFO2", "ENV", "FENV", "VEL", "PRES", "STRP", "KEY", "RAND", "IN", "LFO3", "LFO4" };
            expectEquals((int) ModSrc::COUNT, 13);
            for (int i = 0; i < 13; ++i)
                expectEquals(juce::String(ModSrc::labels()[i]), juce::String(sources[i]));
        }

        beginTest("Pages: SLT1 and LSHP in front of STRIP / LFO1 / LFO2, eight working cells; the slot pages follow use");
        {
            for (EngineType type : soundEngines())
            {
                auto e = makeEngine(type);
                const juce::String name = engineName(type);
                auto names = [&e] { juce::StringArray n; for (auto& p : e->getParamPages()) n.add(p.name); return n; };
                auto first = names();
                expect(first.contains("SLT1") && first.contains("LSHP") && ! first.contains("SLT2"), name + ": " + first.joinIntoString(" "));
                expectEquals(first.indexOf("LSHP"), first.indexOf("SLT1") + 1, name);
                if (type == EngineType::UnifiedVoice)
                {
                    // (the router's STRIP page after LSHP; the screen's MOD pages follow)
                    expectEquals(first[first.size() - 1], juce::String("STRIP"), name + ": the screen's MOD pages follow");
                    expectEquals(first.indexOf("STRIP"), first.indexOf("LSHP") + 1, name);
                }
                else
                {
                    expectEquals(first[first.size() - 3], juce::String("STRIP"), name);
                    expectEquals(first[first.size() - 2], juce::String("LFO1"), name);
                    expectEquals(first[first.size() - 1], juce::String("LFO2"), name);
                    expectEquals(first.indexOf("STRIP"), first.indexOf("LSHP") + 1, name);
                }
                for (auto& page : e->getParamPages())
                    if (page.name.startsWith("SLT") || page.name == "LSHP")
                    {
                        expectEquals(page.section, juce::String("SLOT"), name);
                        for (auto& cell : page.params)
                        {
                            expect(cell.isValid() && EngineModSlots::owns(cell.id), name + " " + page.name);
                            expectEquals(e->getParameter(cell.id), cell.defaultVal, name + " " + page.name + " " + cell.label + " default");
                            expect(e->getParameterName(cell.id).isNotEmpty(), name + " " + cell.label + " has a name");
                            if (cell.kind == ParamKind::Choice)
                                expect(cell.choices != nullptr && cell.numChoices == (int) cell.maxVal + 1, name + " " + cell.label);
                        }
                    }
                const auto sltCells = e->getParamPages()[(size_t) first.indexOf("SLT1")].params;
                const char* labels[8] = { "SRC", "DEST", "AMT", "VIA", "SRC", "DEST", "AMT", "VIA" };
                for (int c = 0; c < 8; ++c)
                {
                    expectEquals(juce::String(sltCells[(size_t) c].label), juce::String(labels[c]));
                    expectEquals(sltCells[(size_t) c].id, 700 + c);
                }
                // (The macro oscillator is on the modulation router: XIN, XLP, XHP, XBP, VIB and ONE follow IN,
                // then RND2 RND3 RND4, ONST, the sixteen bands and its own OSC: "Router Sources".)
                // (So is the wave sequencer: the same list, then its four lanes PLAN SLAN LLAN TLAN.)
                expectEquals(sltCells[0].numChoices, type == EngineType::UnifiedVoice ? 43 : (type == EngineType::MacroOsc ? 38 : (type == EngineType::FM ? 43 : (type == EngineType::WaveSeq ? 41 : (type == EngineType::Wavetable ? 40 : (type == EngineType::Sampler ? 40 : (type == EngineType::Drum ? 37 : 11)))))),     // (FM: + OP1 .. OP6, U4-FM; wavetable: + SCAN XRCT XFLT; sampler: + POS GRN SLEV; the drum kit: the router's own, U4-DRUM)
                             name + ": LFO3 and LFO4 only on the unified voice (on the router: + ENV2 .. ENV5, U4-UV)");
                expect(sltCells[2].isBipolar() && sltCells[2].minVal == -1.0f && sltCells[2].maxVal == 1.0f);
                expect(! e->isRebuildParam(ModSlotParam::id(0, ModSlotParam::AMT)), "AMT can be locked on a step");

                // A slot in use opens the page after its own; the last cell cleared closes it again.
                e->setParameter(ModSlotParam::id(1, ModSlotParam::AMT), 0.5f);
                expect(names().contains("SLT2") && ! names().contains("SLT3"), name + ": slot 2 in use: " + names().joinIntoString(" "));
                e->setParameter(ModSlotParam::id(6, ModSlotParam::SRC), (float) ModSrc::Velocity);
                expect(names().contains("SLT4") && names().contains("SLT5") && ! names().contains("SLT6"), name + ": slot 7 in use");
                e->setParameter(ModSlotParam::id(15, ModSlotParam::VIA), (float) ModSrc::Key);
                expect(names().contains("SLT8"), name + ": slot 16 in use");
                expectEquals(names().size(), first.size() + 7, name + ": eight slot pages at most");
                e->setParameter(ModSlotParam::id(15, ModSlotParam::VIA), 0.0f);
                e->setParameter(ModSlotParam::id(6, ModSlotParam::SRC), 0.0f);
                e->setParameter(ModSlotParam::id(1, ModSlotParam::AMT), 0.0f);
                expect(names() == first, name + ": back to one slot page");
                // Id 772 (no cell) keeps more pages open; it never hides a slot that is in use.
                expectEquals(e->getParameter(ModSlotParam::COUNT), 2.0f);
                e->setParameter(ModSlotParam::COUNT, 9.0f);
                expect(names().contains("SLT5") && ! names().contains("SLT6"), name + ": nine slots asked for");
                expectEquals(e->getParameter(ModSlotParam::COUNT), 9.0f);
                e->setParameter(ModSlotParam::COUNT, 0.0f);
                e->setParameter(ModSlotParam::id(4, ModSlotParam::SRC), (float) ModSrc::Velocity);
                expect(names().contains("SLT3") && names().contains("SLT4"), name + ": a slot in use is shown whatever 772 says");
                e->setParameter(ModSlotParam::COUNT, 99.0f);
                expectEquals(e->getParameter(ModSlotParam::COUNT), 16.0f);
            }

            // PARAM follows: the page list is rebuilt when a slot comes into use (ParamPageScreen::pageStateKey).
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::FM));
            seq.getPattern().getTrack(0).engineType = EngineType::FM;
            ParamPageScreen param(seq, &audio);
            const int before = param.getPageCount();
            audio.setTrackParameter(0, ModSlotParam::id(2, ModSlotParam::SRC), (float) ModSrc::Velocity);
            expectEquals(param.getPageCount(), before + 2, "PARAM shows SLT2 and SLT3 once slot 3 is in use");
            audio.setTrackParameter(0, ModSlotParam::id(2, ModSlotParam::SRC), 0.0f);
            expectEquals(param.getPageCount(), before);
        }

        beginTest("Unused is unheard: a slot with SRC at OFF, or AMT at 0, renders the same samples as no slot at all");
        {
            for (EngineType type : soundEngines())
            {
                const juce::String name = engineName(type);
                Play play;
                play.wheel = 0.5f;
                play.pressure = 0.6f;
                const auto plain = render(*makeEngine(type), play);
                expect(rms(plain) > 1.0e-4f, name + " sounds");

                auto off = makeEngine(type);        // every cell but SRC set
                for (int s = 0; s < 16; ++s)
                {
                    off->setParameter(ModSlotParam::id(s, ModSlotParam::DEST), (float) (1 + s));
                    off->setParameter(ModSlotParam::id(s, ModSlotParam::AMT), s % 2 == 0 ? 1.0f : -1.0f);
                    off->setParameter(ModSlotParam::id(s, ModSlotParam::VIA), (float) ModSrc::Velocity);
                }
                for (int i = 0; i < 8; ++i)         // and the custom shape, which no LFO plays
                    off->setParameter(LfoShapeParam::BASE + i, i % 2 == 0 ? 1.0f : -1.0f);
                expect(render(*off, play) == plain, name + ": SRC OFF is the sound without slots, sample for sample");

                auto zero = makeEngine(type);       // a source and a destination, no amount
                setSlot(*zero, 0, ModSrc::Velocity, "AMP", 0.0f);
                setSlot(*zero, 1, ModSrc::Lfo1, "PTCH", 0.0f);
                expect(render(*zero, play) == plain, name + ": AMT 0 is the sound without slots, sample for sample");
            }
        }

        beginTest("Every source moves the level through a slot, and the pitch too");
        {
            for (EngineType type : soundEngines())
            {
                const juce::String name = engineName(type);
                const bool unified = type == EngineType::UnifiedVoice;
                // What the sources read: a soft-ish note off middle C, the strip and the pad half
                // way, an input that is loud, LFOs that have no destination of their own.
                auto prepare = [&](EngineBase& e)
                {
                    for (int l = 0; l < (unified ? 4 : 2); ++l)
                        setLfoRate(e, l, 5.0f + (float) l);
                };
                Play play;
                play.seconds = 0.4;
                play.wheel = 0.6f;
                play.pressure = 0.7f;
                play.input = 0.6f;
                play.notes = { type == EngineType::Drum ? 3 : 50 };
                auto base = makeEngine(type);
                prepare(*base);
                const auto plain = render(*base, play);

                for (int src = 1; src < (unified ? (int) ModSrc::COUNT : (int) ModSrc::BASIC_COUNT); ++src)
                {
                    float moved = 0.0f;
                    // (RAND is drawn per note: a value near zero is tried again on another voice.)
                    for (int attempt = 0; attempt < (src == ModSrc::Random ? 4 : 1) && moved < 0.02f; ++attempt)
                    {
                        auto e = makeEngine(type);
                        prepare(*e);
                        // (the unified voice's LFO3 / LFO4 are where the router's list has them)
                        setSlot(*e, 2, unified ? indexOf(choicesOf(*e, ModSlotParam::id(0, ModSlotParam::SRC)), ModSrc::labels()[src]) : src, "AMP", 0.8f);
                        moved = difference(plain, render(*e, play));
                    }
                    const bool silentSource = type == EngineType::Drum && src == ModSrc::FilterEnv;     // a drum has no filter envelope
                    if (silentSource)
                        expect(moved < 1.0e-3f, name + ": FENV is nothing on the kit, moved " + juce::String(moved, 4));
                    else
                        expect(moved > 0.02f, name + ": " + ModSrc::labels()[src] + " > AMP moved the sound by only " + juce::String(moved, 4));
                }

                // Velocity on the pitch: the note is heard somewhere else.
                auto pitched = makeEngine(type);
                prepare(*pitched);
                setSlot(*pitched, 0, ModSrc::Velocity, "PTCH", 0.7f);
                expect(difference(plain, render(*pitched, play)) > 0.1f, name + ": VEL > PTCH");
            }
        }

        beginTest("IN follows the follower of the track input (setAudioInput): nothing at rest, the level x 1.7 at 0.7");
        {
            for (EngineType type : soundEngines())
            {
                const juce::String name = engineName(type);
                Play play;
                play.seconds = 0.3;
                const float plain = rms(render(*makeEngine(type), play));

                // The follower at 0: nothing. At 0.7 (given through setAudioInput): the level x 1.7.
                auto quiet = makeEngine(type);
                setSlot(*quiet, 0, ModSrc::Input, "AMP", 1.0f);
                play.input = 0.0f;
                expectWithinAbsoluteError(rms(render(*quiet, play)) / plain, 1.0f, 0.02f, name + ": IN at rest");
                auto loud = makeEngine(type);
                setSlot(*loud, 0, ModSrc::Input, "AMP", 1.0f);
                play.input = 0.7f;
                const float ratio = rms(render(*loud, play)) / plain;
                expect(ratio > 1.5f && ratio < 1.85f, name + ": IN 0.7 > AMP gave x " + juce::String(ratio, 3));
                auto down = makeEngine(type);
                setSlot(*down, 0, ModSrc::Input, "AMP", -1.0f);
                const float less = rms(render(*down, play)) / plain;
                expect(less > 0.2f && less < 0.4f, name + ": IN 0.7 > AMP at -100 gave x " + juce::String(less, 3));
            }
        }

        beginTest("PRES is each voice's own pressure: of two notes only the pressed one is bent");
        {
            for (EngineType type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable })
            {
                const juce::String name = engineName(type);
                // PRES > PTCH at full amount: half pressure = six semitones up.
                auto pitches = [&](int pressedNote)
                {
                    auto e = makeEngine(type);
                    setSlot(*e, 0, ModSrc::Pressure, "PTCH", 1.0f);
                    Play play;
                    play.seconds = 0.5;
                    play.notes = { 48, 61 };
                    play.pressure = 0.5f;
                    play.pressedNote = pressedNote;
                    const auto x = mono(render(*e, play));
                    std::array<double, 4> p {};
                    const int notes[4] = { 48, 54, 61, 67 };
                    for (int i = 0; i < 4; ++i)
                        p[(size_t) i] = fastmeasure::goertzel(x, (size_t) (0.1 * SR), (size_t) (0.3 * SR), hz(notes[i]), SR);
                    return p;
                };
                const auto none = pitches(-1), low = pitches(48), high = pitches(61);
                expect(none[0] > 8.0 * none[1] && none[2] > 8.0 * none[3], name + ": unpressed, both notes at their own pitch");
                expect(low[1] > 8.0 * low[0], name + ": the pressed low note is six semitones up");
                expect(low[2] > 8.0 * low[3], name + ": ... and the other note is not");
                expect(high[3] > 8.0 * high[2], name + ": the pressed high note is six semitones up");
                expect(high[0] > 8.0 * high[1], name + ": ... and the other note is not");
            }
            // The sampler and the wave sequencer (no steady pitch to measure): two notes with the
            // pad of one pressed are that note pressed plus the other unpressed, and not the
            // other way round.
            for (EngineType type : { EngineType::Sampler, EngineType::WaveSeq })
            {
                const juce::String name = engineName(type);
                auto play = [&](std::vector<int> notes, int pressedNote)
                {
                    auto e = makeEngine(type);
                    setSlot(*e, 0, ModSrc::Pressure, "AMP", 1.0f);
                    Play p;
                    p.notes = notes;
                    p.pressure = 0.9f;
                    p.pressedNote = pressedNote;
                    return render(*e, p);
                };
                auto sum = [](std::vector<float> a, const std::vector<float>& b)
                {
                    for (size_t i = 0; i < a.size(); ++i)
                        a[i] += b[i];
                    return a;
                };
                const auto both = play({ 48, 61 }, 48);
                const float right = difference(both, sum(play({ 48 }, 48), play({ 61 }, -1)));
                const float wrong = difference(both, sum(play({ 48 }, -1), play({ 61 }, 61)));
                expect(right < 0.05f, name + ": the pressed note alone carries the pressure, off by " + juce::String(right, 4));
                expect(wrong > 4.0f * right + 0.1f, name + ": the other note does not, " + juce::String(wrong, 4));
            }

            // The kit: pressure on one drum's pad does not reach another drum.
            auto kit = [&](int pressedPad)
            {
                auto e = makeEngine(EngineType::Drum);
                setSlot(*e, 0, ModSrc::Pressure, "AMP", 1.0f);
                Play play;
                play.notes = { 2 };
                play.pressure = 1.0f;
                play.pressedNote = pressedPad;
                return rms(render(*e, play));
            };
            expect(kit(2) > 1.5f * kit(-1), "kit: the pressed drum is louder");
            expectWithinAbsoluteError(kit(5) / kit(-1), 1.0f, 0.01f, "kit: pressure on another pad leaves this drum alone");
        }

        beginTest("VIA scales the amount: nothing at 0, all of it at 1");
        {
            for (EngineType type : soundEngines())
            {
                const juce::String name = engineName(type);
                // VEL (0.8) > AMP at full amount, VIA the input's follower (which does nothing else).
                auto level = [&](int via, float input, bool slot)
                {
                    auto e = makeEngine(type);
                    if (slot)
                        setSlot(*e, 0, ModSrc::Velocity, "AMP", 1.0f, via);
                    Play play;
                    play.input = input;
                    return rms(render(*e, play));
                };
                const float none = level(0, 1.0f, false), full = level(ModSrc::Off, 1.0f, true);
                expectWithinAbsoluteError(level(ModSrc::Input, 0.0f, true) / none, 1.0f, 0.01f, name + ": VIA source at 0 silences the slot");
                expectWithinAbsoluteError(full / none, 1.8f, 0.08f, name + ": the slot alone lifts the level");
                expectWithinAbsoluteError(level(ModSrc::Input, 1.0f, true) / full, 1.0f, 0.02f, name + ": VIA source at 1 gives the full amount");
                expectWithinAbsoluteError(level(ModSrc::Input, 0.5f, true) / none, 1.4f, 0.08f, name + ": VIA half way gives half of it");
                // A source that swings both ways counts from 0 to 1 as VIA: KEY at middle C is a half.
                auto keyed = makeEngine(type);
                setSlot(*keyed, 0, ModSrc::Velocity, "AMP", 1.0f, ModSrc::Key);
                Play play;
                if (type != EngineType::Drum)
                    play.notes = { 60 };
                const float expected = type == EngineType::Drum ? 1.0f + 0.8f * 0.5f * (1.0f + (2 * 8 - 60) / 60.0f) : 1.4f;
                auto plainEngine = makeEngine(type);
                expectWithinAbsoluteError(rms(render(*keyed, play)) / rms(render(*plainEngine, play)), expected, 0.08f, name + ": VIA KEY");
            }
        }

        beginTest("A slot moves another slot's AMT; aimed at its own it does nothing");
        {
            for (EngineType type : soundEngines())
            {
                const juce::String name = engineName(type);
                Play play;
                const float plain = rms(render(*makeEngine(type), play));
                // Slot 1: VEL > AMP with no amount of its own. Slot 2: VEL (0.8) > the amount of slot 1.
                auto e = makeEngine(type);
                setSlot(*e, 0, ModSrc::Velocity, "AMP", 0.0f);
                expectWithinAbsoluteError(rms(render(*e, play)) / plain, 1.0f, 1.0e-3f, name + ": nothing yet");
                auto both = makeEngine(type);
                setSlot(*both, 0, ModSrc::Velocity, "AMP", 0.0f);
                setSlot(*both, 1, ModSrc::Velocity, "AMT1", 1.0f);
                auto direct = makeEngine(type);
                setSlot(*direct, 0, ModSrc::Velocity, "AMP", 0.8f);
                const float moved = rms(render(*both, play)), same = rms(render(*direct, play));
                expect(moved > 1.4f * plain, name + ": the second slot opens the first, x " + juce::String(moved / plain, 3));
                expectWithinAbsoluteError(moved / same, 1.0f, 0.04f, name + ": as far as an amount of its own would");
                // No way round: slot 1 aimed at its own amount is no slot.
                auto self = makeEngine(type);
                setSlot(*self, 0, ModSrc::Velocity, "AMT1", 1.0f);
                expect(render(*self, play) == render(*makeEngine(type), play), name + ": a slot on its own amount does nothing");
                // Two slots on each other's amounts stay inside -1..1.
                auto loop = makeEngine(type);
                setSlot(*loop, 0, ModSrc::Velocity, "AMT2", 1.0f);
                setSlot(*loop, 1, ModSrc::Velocity, "AMT1", 1.0f);
                setSlot(*loop, 2, ModSrc::Velocity, "AMT1", 1.0f);
                expect(finiteAndHeld(render(*loop, play), 20.0f), name + ": slots on each other's amounts");
            }
        }

        beginTest("A slot reaches one LFO's rate (L1RT) and leaves the other alone; a page cell goes back when the slot lets go");
        {
            for (EngineType type : soundEngines())
            {
                const juce::String name = engineName(type);
                const bool unified = type == EngineType::UnifiedVoice;
                auto withLfo = [&](const char* dest, float amount)
                {
                    auto e = makeEngine(type);
                    setLfo(*e, 0, "AMP", 0.8f, 0, 4.0f);
                    if (dest != nullptr)
                        setSlot(*e, 0, ModSrc::Velocity, dest, amount);
                    Play play;
                    play.seconds = 0.5;
                    return render(*e, play);
                };
                const auto plain = withLfo(nullptr, 0.0f);
                expect(difference(plain, withLfo(unified ? "RAT1" : "L1RT", 1.0f)) > 0.05f, name + ": VEL > the rate of LFO 1");
                expect(difference(plain, withLfo(unified ? "RAT2" : "L2RT", 1.0f)) < 1.0e-3f, name + ": the rate of LFO 2 is not LFO 1's");
                expect(difference(plain, withLfo(unified ? "DPT1" : "L1DP", -1.0f)) > 0.05f, name + ": VEL > the depth of LFO 1");
                if (! unified)
                    expect(difference(plain, withLfo("LRAT", 1.0f)) > 0.05f, name + ": LRAT from a slot moves both LFOs");
            }

            // A page cell as the destination, on an engine of each kind (the engine pushes the value
            // into its voices / the voice reads it / the unified voice). Taken off under a sounding
            // note the slot lets go, and the next note is the plain sound again.
            struct Cell { EngineType type; const char* dest; };
            for (auto& cell : { Cell { EngineType::FM, "LVL2" }, Cell { EngineType::MacroOsc, "MORP" }, Cell { EngineType::Wavetable, "SUB" },
                                Cell { EngineType::WaveSeq, "XFAD" }, Cell { EngineType::Sampler, "RATE" }, Cell { EngineType::UnifiedVoice, "DRIV" } })
            {
                const juce::String name = engineName(cell.type) + " " + cell.dest;
                Play play;
                play.seconds = 0.3;
                auto plainEngine = makeEngine(cell.type);
                const auto plain = render(*plainEngine, play);
                const auto plainAgain = render(*plainEngine, play);

                auto e = makeEngine(cell.type);
                const int destId = keyedCell(*e, cell.dest);
                const float set = e->getParameter(destId);
                setSlot(*e, 0, ModSrc::Velocity, cell.dest, 1.0f);
                expectEquals(e->getParameter(destId), set, name + ": the cell's own value is not touched");
                play.atBlock = [](EngineBase& engine, int block)
                {
                    if (block == 150)
                        engine.setParameter(ModSlotParam::id(0, ModSlotParam::SRC), 0.0f);      // under the note
                };
                const auto moved = render(*e, play);
                expect(difference(plain, moved) > 0.02f, name + ": moved by " + juce::String(difference(plain, moved), 4));
                expect(finiteAndHeld(moved, 20.0f), name);
                play.atBlock = {};
                expect(difference(plainAgain, render(*e, play)) < 2.0e-3f, name + ": the next note is the plain sound, off by "
                                                                               + juce::String(difference(plainAgain, render(*e, play)), 5));
            }
        }

        beginTest("AMT set under a sounding note (a step lock): heard, moved to and not jumped to");
        {
            for (EngineType type : soundEngines())
            {
                const juce::String name = engineName(type);
                Play play;
                play.seconds = 0.6;
                play.holdToEnd = true;
                auto plainEngine = makeEngine(type);
                steady(*plainEngine);
                const auto plain = render(*plainEngine, play);

                auto e = makeEngine(type);
                steady(*e);
                setSlot(*e, 0, ModSrc::Velocity, "AMP", 0.0f);
                const int amt = ModSlotParam::id(0, ModSlotParam::AMT);
                play.atBlock = [amt](EngineBase& engine, int block)
                {
                    if (block == 150) engine.setParameter(amt, 1.0f);       // the lock
                    if (block == 300) engine.setParameter(amt, 0.0f);       // ... and back to the set value
                };
                const auto locked = render(*e, play);
                auto part = [&](const std::vector<float>& x, int fromBlock, int toBlock)
                {
                    return rms(std::vector<float>(x.begin() + fromBlock * BLOCK * 2, x.begin() + toBlock * BLOCK * 2));
                };
                expectWithinAbsoluteError(part(locked, 60, 150) / part(plain, 60, 150), 1.0f, 0.01f, name + ": before the lock");
                const float during = part(locked, 170, 300) / part(plain, 170, 300);
                expect(during > 1.6f && during < 1.9f, name + ": locked at +100, velocity 0.8: x " + juce::String(during, 3));
                expectWithinAbsoluteError(part(locked, 320, 440) / part(plain, 320, 440), 1.0f, 0.02f, name + ": after the lock");
                // No step: the largest sample-to-sample move at the two changes is of the size the
                // sound has anyway (a jump of the gain by 0.8 would be most of the peak).
                const float allowed = 2.0f * steepest(plain, 0, (int) plain.size()) + 0.02f * peak(plain);
                expect(steepest(locked, 148 * BLOCK * 2, 156 * BLOCK * 2) < allowed, name + ": the lock arrives without a step");
                expect(steepest(locked, 298 * BLOCK * 2, 306 * BLOCK * 2) < allowed, name + ": ... and leaves without one");
            }
        }

        beginTest("The custom shape: USR holds each of the eight levels for an eighth of the cycle, USRS joins them, the last to the first");
        {
            const float levels[8] = { 0.9f, -0.4f, 0.1f, 1.0f, -1.0f, 0.3f, -0.7f, 0.5f };
            for (int smooth = 0; smooth < 2; ++smooth)
            {
                LFO lfo;
                lfo.setSampleRate(8000.0);
                lfo.setRate(1.0f);                  // 1000 samples a level
                lfo.setDepth(1.0f);
                lfo.setShape(smooth ? LFO::Shape::UserSmooth : LFO::Shape::User);
                lfo.setTriggerMode(LFO::TriggerMode::KeySync);
                lfo.setUserShape(levels);
                lfo.trigger();
                std::vector<float> out;
                for (int i = 0; i < 16000; ++i)
                    out.push_back(lfo.process());
                for (int cycle = 0; cycle < 2; ++cycle)
                    for (int k = 0; k < 8; ++k)
                    {
                        const int at = cycle * 8000 + k * 1000;
                        const float next = levels[(k + 1) % 8];
                        if (! smooth)
                        {
                            for (int offset : { 5, 500, 990 })
                                expectWithinAbsoluteError(out[(size_t) (at + offset)], levels[k], 1.0e-5f, "USR level " + juce::String(k + 1));
                        }
                        else
                        {
                            expectWithinAbsoluteError(out[(size_t) (at + 1)], levels[k], 0.005f, "USRS starts level " + juce::String(k + 1) + " at its value");
                            expectWithinAbsoluteError(out[(size_t) (at + 500)], 0.5f * (levels[k] + next), 0.005f, "USRS half way to the next");
                            expectWithinAbsoluteError(out[(size_t) (at + 900)], levels[k] + 0.9f * (next - levels[k]), 0.005f, "USRS nine tenths of the way");
                        }
                    }
                float step = 0.0f;
                for (size_t i = 1; i < out.size(); ++i)
                    step = std::max(step, std::abs(out[i] - out[i - 1]));
                if (smooth)
                    expect(step < 0.005f, "USRS never jumps, the wrap included: " + juce::String(step, 4));
                else
                    expect(step > 1.9f, "USR steps");
                // Without levels of its own: the rising staircase.
                LFO plain;
                plain.setSampleRate(8000.0);
                plain.setShape(LFO::Shape::User);
                plain.setTriggerMode(LFO::TriggerMode::KeySync);
                plain.trigger();
                float last = -2.0f;
                for (int k = 0; k < 8; ++k)
                {
                    float v = 0.0f;
                    for (int i = 0; i < 1000; ++i)
                    {
                        const float now = plain.process();
                        if (i == 500)
                            v = now;
                    }
                    expectWithinAbsoluteError(v, -1.0f + 2.0f * (float) k / 7.0f, 1.0e-5f);
                    expect(v > last);
                    last = v;
                }
            }

            // On an engine: LSHP's cells are the levels, one shape for both LFOs of the track.
            for (EngineType type : soundEngines())
            {
                const juce::String name = engineName(type);
                Play play;
                play.seconds = 0.5;
                play.holdToEnd = true;
                auto build = [&](int shape, const std::function<float (int)>& level, int lfo = 0)
                {
                    auto e = makeEngine(type);
                    steady(*e);
                    for (int i = 0; i < 8; ++i)
                        e->setParameter(LfoShapeParam::BASE + i, level(i));
                    setLfo(*e, lfo, "AMP", 1.0f, shape, 4.0f, 1);       // KEY: the cycle starts with the note
                    return e;
                };
                const auto flat = render(*build(9, [](int) { return 0.0f; }), play);
                const auto flatSmooth = render(*build(10, [](int) { return 0.0f; }), play);
                auto plainEngine = makeEngine(type);
                steady(*plainEngine);
                const auto plain = render(*plainEngine, play);
                expect(difference(plain, flat) < 2.0e-3f, name + ": eight levels at 0 = no modulation (USR)");
                expect(difference(plain, flatSmooth) < 2.0e-3f, name + ": ... and USRS");
                // Levels 1-4 down, 5-8 up: the first half of each 250 ms cycle is quiet, the second loud.
                auto halves = [](int i) { return i < 4 ? -0.8f : 0.8f; };
                for (int lfo = 0; lfo < 2; ++lfo)
                {
                    const auto x = render(*build(9, halves, lfo), play);
                    auto window = [&](const std::vector<float>& v, double from, double to)
                    {
                        return rms(std::vector<float>(v.begin() + (int) (from * SR) * 2, v.begin() + (int) (to * SR) * 2));
                    };
                    const float quiet = window(x, 0.27, 0.36) / window(plain, 0.27, 0.36);
                    const float loud = window(x, 0.395, 0.485) / window(plain, 0.395, 0.485);
                    expectWithinAbsoluteError(quiet, 0.2f, 0.05f, name + " LFO " + juce::String(lfo + 1) + ": levels 1-4 at -80");
                    expectWithinAbsoluteError(loud, 1.8f, 0.1f, name + " LFO " + juce::String(lfo + 1) + ": levels 5-8 at +80");
                }
                // A level's cell keeps what it is given and is held to -1..1.
                auto e = makeEngine(type);
                e->setParameter(LfoShapeParam::BASE + 3, 0.25f);
                expectEquals(e->getParameter(LfoShapeParam::BASE + 3), 0.25f);
                e->setParameter(LfoShapeParam::BASE + 3, -7.0f);
                expectEquals(e->getParameter(LfoShapeParam::BASE + 3), -1.0f);
                expectWithinAbsoluteError(e->getParameter(LfoShapeParam::BASE + 7), 1.0f, 1.0e-6f, name + ": the default staircase ends at +100");
                expectWithinAbsoluteError(e->getParameter(LfoShapeParam::BASE), -1.0f, 1.0e-6f, name + ": ... and starts at -100");
            }
        }

        beginTest("An LFO as a source: its shape without its depth, whether or not it has a destination of its own");
        {
            for (EngineType type : soundEngines())
            {
                const juce::String name = engineName(type);
                Play play;
                play.seconds = 0.5;
                play.holdToEnd = true;
                // LFO 1: a square at 4 Hz from the note, DPTH 0, no destination. Slot: LFO1 > AMP at 50.
                auto e = makeEngine(type);
                steady(*e);
                setLfo(*e, 0, "OFF", 0.0f, 2, 4.0f, 1);
                setSlot(*e, 0, ModSrc::Lfo1, "AMP", 0.5f);
                auto plainEngine = makeEngine(type);
                steady(*plainEngine);
                const auto x = render(*e, play), plain = render(*plainEngine, play);
                auto window = [&](const std::vector<float>& v, double from, double to)
                {
                    return rms(std::vector<float>(v.begin() + (int) (from * SR) * 2, v.begin() + (int) (to * SR) * 2));
                };
                expectWithinAbsoluteError(window(x, 0.27, 0.36) / window(plain, 0.27, 0.36), 1.5f, 0.08f, name + ": the square's upper half");
                expectWithinAbsoluteError(window(x, 0.395, 0.485) / window(plain, 0.395, 0.485), 0.5f, 0.08f, name + ": its lower half");
            }
        }

        beginTest("Extremes: every slot on every destination, notes 0 and 127, 44.1 to 192 kHz, blocks of 1 to 2048: finite and held");
        {
            int renders = 0;
            for (EngineType type : soundEngines())
            {
                const juce::String name = engineName(type);
                const bool unified = type == EngineType::UnifiedVoice;
                const int numDests = (int) slotDestNames(*makeEngine(type)).size();
                const int numSources = unified ? UnifiedVoiceEngine::sourceCount() : (type == EngineType::MacroOsc ? MacroOscEngine::sources().size()
                                                                         : (type == EngineType::FM ? FMEngine::sources().size()
                                     : (type == EngineType::WaveSeq ? WaveSeqEngine::sources().size()
                                     : (type == EngineType::Wavetable ? WavetableEngine::sources().size()
                                     : (type == EngineType::Sampler ? SamplerEngine::sources().size()
                                     : (type == EngineType::Drum ? mod::SourceTable::standard(2).size() : (int) ModSrc::BASIC_COUNT))))));
                struct Run { double sr; int block; };
                const Run runs[] = { { 44100.0, 64 }, { 48000.0, 1 }, { 96000.0, 333 }, { 192000.0, 2048 } };
                // Sixteen slots a round: enough rounds for every destination to be aimed at.
                const int rounds = (numDests + 15) / 16;
                for (int round = 0; round < rounds; ++round)
                {
                    const Run run = runs[round % 4];
                    auto e = makeEngine(type, nullptr, run.sr, run.block);
                    for (int s = 0; s < 16; ++s)
                    {
                        const int dest = 1 + (round * 16 + s) % (numDests - 1);
                        e->setParameter(ModSlotParam::id(s, ModSlotParam::SRC), (float) (1 + (s + round) % (numSources - 1)));
                        e->setParameter(ModSlotParam::id(s, ModSlotParam::DEST), (float) dest);
                        e->setParameter(ModSlotParam::id(s, ModSlotParam::AMT), (s + round) % 2 == 0 ? 1.0f : -1.0f);
                        e->setParameter(ModSlotParam::id(s, ModSlotParam::VIA), (float) ((s * 3 + round) % numSources));
                    }
                    for (int l = 0; l < 2; ++l)
                        setLfo(*e, l, l == 0 ? "PTCH" : "AMP", l == 0 ? 1.0f : -1.0f, 9 + l, 40.0f);
                    for (int i = 0; i < 8; ++i)
                        e->setParameter(LfoShapeParam::BASE + i, i % 2 == 0 ? 1.0f : -1.0f);
                    e->setModWheel(1.0f);
                    juce::AudioBuffer<float> buf(2, run.block);
                    juce::MidiBuffer midi;
                    const bool kit = type == EngineType::Drum;
                    const int a = kit ? round % 16 : 0, b = kit ? (round + 7) % 16 : 127;
                    e->noteOn(a, 1.0f);
                    e->noteOn(b, 1.0f);
                    e->aftertouch(a, 1.0f);
                    bool ok = true;
                    float top = 0.0f;
                    const int total = (int) (0.06 * run.sr);
                    for (int done = 0, block = 0; done < total; done += run.block, ++block)
                    {
                        // The input's follower: loud, then not a number, then far too much.
                        const float follower = block % 3 == 0 ? 1.0f : (block % 3 == 1 ? std::numeric_limits<float>::quiet_NaN() : 1.0e9f);
                        e->setAudioInput({ nullptr, nullptr, 0, follower });
                        if (done > total / 2 && done - run.block <= total / 2)
                        {
                            // Half way: every slot somewhere else, under the notes.
                            for (int s = 0; s < 16; ++s)
                                e->setParameter(ModSlotParam::id(s, ModSlotParam::DEST), (float) (1 + (round * 16 + s * 5 + 3) % (numDests - 1)));
                            e->noteOff(a);
                        }
                        buf.clear();
                        e->processBlock(buf, midi);
                        for (int ch = 0; ch < 2; ++ch)
                            for (int i = 0; i < run.block; ++i)
                            {
                                const float v = buf.getSample(ch, i);
                                ok = ok && std::isfinite(v);
                                top = std::max(top, std::abs(v));
                            }
                    }
                    ++renders;
                    expect(ok, name + " round " + juce::String(round) + " at " + juce::String(run.sr) + ": not finite");
                    expect(top < 60.0f, name + " round " + juce::String(round) + ": peak " + juce::String(top, 1));
                    // Values that are no index or amount are held to the cell's range.
                    e->setParameter(ModSlotParam::id(0, ModSlotParam::DEST), 1.0e6f);
                    // (The list goes on behind the engine's own with the cells of the track's effects: E4.)
                    expectEquals(e->getParameter(ModSlotParam::id(0, ModSlotParam::DEST)), (float) (numDests + mod::TrackDest::COUNT - 1));
                    e->setParameter(ModSlotParam::id(0, ModSlotParam::SRC), -5.0f);
                    expectEquals(e->getParameter(ModSlotParam::id(0, ModSlotParam::SRC)), 0.0f);
                    e->setParameter(ModSlotParam::id(0, ModSlotParam::AMT), std::numeric_limits<float>::quiet_NaN());
                    expectEquals(e->getParameter(ModSlotParam::id(0, ModSlotParam::AMT)), 0.0f);
                    e->setParameter(ModSlotParam::id(0, ModSlotParam::VIA), 99.0f);
                    expectEquals(e->getParameter(ModSlotParam::id(0, ModSlotParam::VIA)), (float) (numSources - 1));
                }
            }
            expect(renders >= 30);
        }

        beginTest("Sixteen slots on one destination: every destination of every engine in turn, the resonance up: finite and held");
        {
            // (Sixteen slots on one oscillator level of the unified voice once multiplied to x 65536.)
            for (EngineType type : soundEngines())
            {
                const juce::String name = engineName(type);
                const auto dests = slotDestNames(*makeEngine(type));
                const int numDests = (int) dests.size() - 16;       // (the slots' own amounts: the test above)
                const bool kit = type == EngineType::Drum;
                float worst = 0.0f;
                for (int d = 1; d < numDests; ++d)
                {
                    const bool high = d % 2 == 0;
                    const double sr = high ? 192000.0 : 44100.0;
                    const int block = high ? 512 : 64;
                    auto e = makeEngine(type, nullptr, sr, block);
                    for (auto& page : e->getParamPages())
                        for (auto& c : page.params)
                            if (c.isValid() && (juce::String(c.label) == "RESO" || juce::String(c.label) == "RES"))
                                e->setParameter(c.id, c.maxVal);
                    for (int s = 0; s < 16; ++s)
                    {
                        e->setParameter(ModSlotParam::id(s, ModSlotParam::DEST), (float) d);
                        e->setParameter(ModSlotParam::id(s, ModSlotParam::AMT), 1.0f);
                        e->setParameter(ModSlotParam::id(s, ModSlotParam::SRC), (float) (s % 2 == 0 ? ModSrc::Velocity : ModSrc::Lfo1));
                    }
                    setLfoRate(*e, 0, 30.0f);
                    juce::AudioBuffer<float> buf(2, block);
                    juce::MidiBuffer midi;
                    e->noteOn(kit ? d % 16 : (high ? 0 : 127), 1.0f);
                    e->noteOn(kit ? (d + 5) % 16 : (high ? 24 : 100), 1.0f);
                    bool ok = true;
                    float top = 0.0f;
                    const int total = (int) (0.02 * sr);
                    for (int done = 0; done < total; done += block)
                    {
                        buf.clear();
                        e->processBlock(buf, midi);
                        for (int ch = 0; ch < 2; ++ch)
                            for (int i = 0; i < block; ++i)
                            {
                                const float v = buf.getSample(ch, i);
                                ok = ok && std::isfinite(v);
                                top = std::max(top, std::abs(v));
                            }
                    }
                    worst = std::max(worst, top);
                    expect(ok, name + ": sixteen slots on " + dests[(size_t) d] + ": not finite");
                    expect(top < 8.0f, name + ": sixteen slots on " + dests[(size_t) d] + ": peak " + juce::String(top, 1));
                }
                if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_MODSLOT_PEAKS", "").isNotEmpty())
                    logMessage("PEAK " + name + " " + juce::String(worst, 2));
            }
        }
    }
};

static ModSlotsTests modSlotsTests;
