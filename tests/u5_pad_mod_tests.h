#pragma once

#include <juce_core/juce_core.h>
#include "../src/engine/DrumEngine.h"
#include "../src/engine/ParamSpec.h"

// U5 Pad Mod: per-pad LFO and slots for the drum kit.
class U5PadModTests : public juce::UnitTest
{
public:
    U5PadModTests() : juce::UnitTest("U5 Pad Mod") {}

    void runTest() override
    {
        testIdSpace();
        testPages();
        testSetGet();
        testPadLfoModulation();
        testPadSlotModulation();
        testProjectStorage();
        testPadLfoTrigger();
        testDestsOffered();
        testViaByTheRoutersRule();
        testLawsOfTheTable();
    }

private:
    static constexpr double SR = 44100.0;
    static constexpr int BLOCK = 256;

    // Render from a DrumEngine directly.
    static juce::AudioBuffer<float> render(DrumEngine& e, int samples, int block = BLOCK)
    {
        juce::AudioBuffer<float> buf(2, block);
        juce::AudioBuffer<float> out(2, samples);
        out.clear();
        juce::MidiBuffer midi;
        int pos = 0;
        while (pos < samples)
        {
            const int n = std::min(block, samples - pos);
            buf.clear();
            juce::AudioBuffer<float> sub(buf.getArrayOfWritePointers(), 2, n);
            e.processBlock(sub, midi);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom(ch, pos, sub, ch, 0, n);
            pos += n;
        }
        return out;
    }

    static float rms(const juce::AudioBuffer<float>& buf)
    {
        float sum = 0;
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            for (int i = 0; i < buf.getNumSamples(); ++i)
            {
                float s = buf.getSample(ch, i);
                sum += s * s;
            }
        return std::sqrt(sum / std::max(1.0f, (float) (buf.getNumSamples() * buf.getNumChannels())));
    }

    static int sourcePlace(DrumEngine& e, int source)
    {
        const auto* router = e.modRouter();
        for (int i = 0; i < router->numSources(); ++i)
            if (router->sources().source(i) == source)
                return i;
        return 0;
    }
    static void padSlot(DrumEngine& e, int pad, int slot, int source, int dest, float amount, int via = -1)
    {
        const int b = PadModParam::id(pad, slot == 0 ? PadModParam::SLOT1_SRC : PadModParam::SLOT2_SRC);
        e.setParameter(b + 0, (float) sourcePlace(e, source));
        e.setParameter(b + 1, (float) dest);
        e.setParameter(b + 2, amount);
        e.setParameter(b + 3, via < 0 ? 0.0f : (float) sourcePlace(e, via));
    }
    static juce::AudioBuffer<float> hit(DrumEngine& e, int pad, int samples = 8192, float velocity = 1.0f)
    {
        e.noteOn(pad, velocity);
        auto out = render(e, samples);
        e.noteOff(pad);
        return out;
    }
    static double apart(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        double diff = 0.0, level = 0.0;
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            const double d = (double) a.getSample(0, i) - b.getSample(0, i);
            diff += d * d;
            level += (double) a.getSample(0, i) * a.getSample(0, i);
        }
        return std::sqrt(diff / std::max(level, 1.0e-20));
    }

    // The pad's DEST cells offer what the pad's own modulation can move, at the table's places.
    void testDestsOffered()
    {
        beginTest("PLFO / PSLT DEST: the table's entries a pad can move, each at its place; not LRAT LDPT PM WPOS, no effect cells");
        DrumEngine drums;
        const auto& table = DrumEngine::lfoDests();
        const int pad = 4;
        const auto pages = drums.padPages(pad);
        int cells = 0;
        for (const auto& page : pages)
            for (const auto& spec : page.params)
            {
                const int cell = PadModParam::contains(spec.id) ? PadModParam::cellOf(spec.id) : -1;
                if (cell != PadModParam::LFO_DEST && cell != PadModParam::SLOT1_DEST && cell != PadModParam::SLOT2_DEST)
                    continue;
                ++cells;
                expectEquals(spec.numChoices, table.visible(), "the table's entries and no more (no cell of the track's effects)");
                expectEquals((int) spec.maxVal, table.visible() - 1);
                int offered = 0;
                for (int d = 0; d < spec.numChoices; ++d)
                {
                    const bool acts = d == 0 || DrumVoice::padDestActs(d);
                    expect(spec.isOffered(d) == acts, juce::String(table.labels()[d]) + (acts ? " is offered" : " is not offered"));
                    if (acts)
                        expectEquals(juce::String(spec.choices[d]), juce::String(table.labels()[d]), "the table's own name at the table's place");
                    offered += acts ? 1 : 0;
                }
                expectEquals(offered, table.visible() - 4, "all but four");
                // A knob passes over the places that are kept: from ACRV (the kick page's last) to MRPH.
                const float next = spec.turned((float) (DrumVoice::ModKickFirst + DrumVoice::NumKickParams - 1), 1, TurnStep::Plain);
                expectEquals((int) next, (int) DrumVoice::ModRsnFirst, "a turn passes over LRAT and LDPT");
                // What is stored and valid keeps its number; what the pad never moved is OFF.
                for (int d = 0; d < table.visible(); ++d)
                {
                    drums.setParameter(spec.id, (float) d);
                    expectEquals((int) drums.getParameter(spec.id), DrumVoice::padDestActs(d) ? d : 0, juce::String(table.labels()[d]) + " as a stored DEST");
                }
                for (float beyond : { (float) table.visible(), (float) table.visible() + 17.0f, (float) (table.cellChoices() - 1), 5000.0f, -3.0f })
                {
                    drums.setParameter(spec.id, beyond);
                    expectEquals((int) drums.getParameter(spec.id), 0, "a cell of the track's effects (or anything past the list) was never moved by a pad: OFF");
                }
            }
        expectEquals(cells, 3, "DEST, DST1, DST2");
        for (int d : { (int) DrumVoice::ModLfoRate, (int) DrumVoice::ModLfoDepth, (int) DrumVoice::ModPhase, (int) DrumVoice::ModWavePos })
            expect(! DrumVoice::padDestActs(d));
    }

    // VIA closes a slot as it does in the router: a one-sided source at 0 is 0, not a half.
    void testViaByTheRoutersRule()
    {
        beginTest("A pad slot's VIA goes by the router's rule: PRES at rest closes the slot, VEL at full opens it, a two-sided source is shifted up");
        const int pad = 8;
        auto level = [&](int via, float velocity)
        {
            DrumEngine drums;
            drums.prepareToPlay(SR, BLOCK);
            if (via != -2)
                padSlot(drums, pad, 0, mod::Src::One, DrumVoice::ModAmp, -0.9f, via);
            return rms(hit(drums, pad, 8192, velocity));
        };
        const float plain = level(-2, 1.0f), noVia = level(-1, 1.0f);
        const float byPressure = level(mod::Src::Pressure, 1.0f), byVelocity = level(mod::Src::Velocity, 1.0f);
        const float byKey = level(mod::Src::Key, 1.0f);
        expect(noVia < 0.2f * plain, "ONE > AMP at -90 with no VIA takes the level down (" + juce::String(noVia / plain, 3) + ")");
        expectWithinAbsoluteError(byPressure / plain, 1.0f, 0.01f, "VIA PRES with the pad not pressed: the slot is closed (it stood half open)");
        expectWithinAbsoluteError(byVelocity / noVia, 1.0f, 0.02f, "VIA VEL at full velocity: wide open");
        // (KEY is the drum's place in the kit, -1 .. 1: pad 9 of 16 is a little over the middle.)
        expect(byKey > 1.5f * noVia && byKey < 0.8f * plain, "VIA KEY, a two-sided source, is shifted up to 0..1: about half (" + juce::String(byKey / plain, 3) + ")");
    }

    // What a pad's LFO or slot does to a control-rate cell is the router's law on the table's entry.
    void testLawsOfTheTable()
    {
        const auto& table = DrumEngine::lfoDests();
        using V = DrumVoice;
        const int kickPad = 8;

        beginTest("A pad slot on a kick cell moves it by the table's span and holds it to the cell's limits (SWEP: 72 st, not 1)");
        {
            auto kickWith = [&](int param, float cell, float amount)
            {
                DrumEngine drums;
                drums.prepareToPlay(SR, BLOCK);
                expect(drums.isKickMachine(kickPad), "pad 9 is the kick machine");
                if (cell > -900.0f)
                    drums.setParameter(DrumEngine::KICK_BASE + kickPad * V::NumKickParams + param, cell);
                if (amount != 0.0f)
                    padSlot(drums, kickPad, 0, mod::Src::One, V::ModKickFirst + param, amount);
                return hit(drums, kickPad);
            };
            const float swep = V::kickDefault(V::KickSweep);
            expectEquals(table[V::ModKickFirst + V::KickSweep].span, 72.0f, "SWEP's span");
            const auto plain = kickWith(V::KickSweep, -999.0f, 0.0f);
            const auto bySlot = kickWith(V::KickSweep, -999.0f, 0.25f), byCell = kickWith(V::KickSweep, swep + 18.0f, 0.0f);
            expect(apart(bySlot, byCell) < 1.0e-3, "ONE > SWEP at +25 is the kick with SWEP 18 st higher (" + juce::String(apart(bySlot, byCell), 5) + " apart)");
            expect(apart(bySlot, plain) > 0.05, "and not the kick as it was (a semitone's quarter moved nothing to speak of)");
            const auto down = kickWith(V::KickSweep, -999.0f, -1.0f), floor = kickWith(V::KickSweep, 0.0f, 0.0f);
            expect(apart(down, floor) < 1.0e-3, "ONE > SWEP at -100 stops at the cell's 0 (" + juce::String(apart(down, floor), 5) + ")");
            const auto held = kickWith(V::KickHold, -999.0f, 1.0f), top = kickWith(V::KickHold, 0.3f, 0.0f);
            expect(apart(held, top) < 1.0e-3, "ONE > HOLD at +100 is HOLD at its top, 0.3 s (" + juce::String(apart(held, top), 5) + ")");
            bool finite = true;
            for (int i = 0; i < held.getNumSamples(); ++i)
                finite = finite && std::isfinite(held.getSample(0, i));
            expect(finite);
        }

        beginTest("A pad slot on a resonator drum's cell: the cell as set plus the share of its span, the same at every sub-block and every hit");
        {
            DrumEngine drums;
            drums.prepareToPlay(SR, BLOCK);
            const int pad = 12;
            expect(drums.isResonator(pad), "pad 13 is a resonator drum");
            const int dest = table.indexOfLabel("MRPH");
            expectEquals(dest, (int) V::ModRsnFirst);
            hit(drums, pad, 4096);
            const float asSet = drums.getVoice(pad).getResonator().getCell(ResonatorVoice::Morph);
            padSlot(drums, pad, 0, mod::Src::One, dest, 0.25f);
            const float want = mod::Voice::lawOf(table[dest], asSet, 0.25f);
            expect(std::abs(want - asSet) > 0.1f, "(the slot asks for a move)");
            for (int n = 0; n < 6; ++n)
            {
                hit(drums, pad, 4096);
                expectWithinAbsoluteError(drums.getVoice(pad).getResonator().getCell(ResonatorVoice::Morph), want, 1.0e-4f,
                                          "hit " + juce::String(n + 1) + ": MRPH is where the slot puts it, not further each sub-block");
            }
        }

        beginTest("A pad slot on ETIM: the table's factor while it is routed, and the envelope's own time again when it lets go");
        {
            DrumEngine drums;
            drums.prepareToPlay(SR, BLOCK);
            const int pad = 12;
            auto scale = [&] { return drums.getVoice(pad).getResonator().getAmpEnvelope().getTimeScale(); };
            hit(drums, pad, 2048);
            expectWithinAbsoluteError(scale(), 1.0f, 1.0e-6f);
            padSlot(drums, pad, 0, mod::Src::One, V::ModEnvTime, 0.5f);
            hit(drums, pad, 2048);
            expectWithinAbsoluteError(scale(), mod::Voice::lawOf(table[V::ModEnvTime], 1.0f, 0.5f), 1.0e-4f, "the router's law on ETIM");
            padSlot(drums, pad, 0, mod::Src::One, V::ModEnvTime, 0.0f);
            hit(drums, pad, 2048);
            expectWithinAbsoluteError(scale(), 1.0f, 1.0e-6f, "given back");
        }
    }

    void testIdSpace()
    {
        beginTest("Per-pad ids are in EngineParamRange and PadModParam");
        {
            for (int pad = 0; pad < 16; ++pad)
                for (int cell = 0; cell < PadModParam::PER_PAD; ++cell)
                {
                    int id = PadModParam::id(pad, cell);
                    expect(EngineParamRange::contains(id), "id " + juce::String(id) + " must be in EngineParamRange");
                    expect(PadModParam::contains(id));
                    expectEquals(PadModParam::padOf(id), pad);
                    expectEquals(PadModParam::cellOf(id), cell);
                }
            expect(! PadModParam::contains(PadModParam::BASE - 1));
            expect(! PadModParam::contains(PadModParam::END));
        }
    }

    void testPages()
    {
        beginTest("Per-pad pages appear in getParamPages and have eight cells each");
        {
            DrumEngine drums;
            drums.prepareToPlay(SR, BLOCK);
            const auto pages = drums.getParamPages();
            const int perPad = DrumEngine::PAGES_PER_PAD * DrumEngine::NUM_VOICES;
            expect((int) pages.size() > perPad, "enough pages");
            bool foundPlfo = false, foundPslt = false;
            for (const auto& p : pages)
            {
                if (p.name == "PLFO") foundPlfo = true;
                if (p.name == "PSLT") foundPslt = true;
            }
            expect(foundPlfo, "PLFO page exists");
            expect(foundPslt, "PSLT page exists");
            const auto plfo = drums.padLfoPage(0);
            int validCells = 0;
            for (const auto& spec : plfo.params)
                if (spec.isValid()) ++validCells;
            expectEquals(validCells, 8, "PLFO has 8 cells");
            const auto pslt = drums.padSlotPage(0);
            validCells = 0;
            for (const auto& spec : pslt.params)
                if (spec.isValid()) ++validCells;
            expectEquals(validCells, 8, "PSLT has 8 cells");
        }
    }

    void testSetGet()
    {
        beginTest("Per-pad mod cells are set and returned");
        {
            DrumEngine drums;
            drums.prepareToPlay(SR, BLOCK);
            const int pad = 5;

            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_RATE), 5.0f);
            expectWithinAbsoluteError(drums.getParameter(PadModParam::id(pad, PadModParam::LFO_RATE)), 5.0f, 0.01f, "rate round-trips");
            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_DEPTH), 0.75f);
            expectWithinAbsoluteError(drums.getParameter(PadModParam::id(pad, PadModParam::LFO_DEPTH)), 0.75f, 0.001f, "depth round-trips");
            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_DEST), 3.0f);
            expectWithinAbsoluteError(drums.getParameter(PadModParam::id(pad, PadModParam::LFO_DEST)), 3.0f, 0.01f, "dest round-trips");
            drums.setParameter(PadModParam::id(pad, PadModParam::SLOT1_SRC), 2.0f);
            expectWithinAbsoluteError(drums.getParameter(PadModParam::id(pad, PadModParam::SLOT1_SRC)), 2.0f, 0.01f, "slot src round-trips");
            drums.setParameter(PadModParam::id(pad, PadModParam::SLOT1_AMT), -0.5f);
            expectWithinAbsoluteError(drums.getParameter(PadModParam::id(pad, PadModParam::SLOT1_AMT)), -0.5f, 0.001f, "slot amt round-trips");
            // Clamp tests.
            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_DEPTH), 2.0f);
            expectWithinAbsoluteError(drums.getParameter(PadModParam::id(pad, PadModParam::LFO_DEPTH)), 1.0f, 0.001f, "depth clamped to 1");
        }
    }

    void testPadLfoModulation()
    {
        beginTest("A per-pad LFO on TONE changes the sound");
        {
            DrumEngine drums;
            drums.prepareToPlay(SR, BLOCK);
            const int pad = 8;    // the kick
            const int toneDestIndex = DrumVoice::ModTone;

            // Render without per-pad LFO.
            drums.noteOn(pad, 1.0f);
            auto noLfo = render(drums, 4096);
            drums.noteOff(pad);
            drums.allNotesOff();

            // Set per-pad LFO: 5 Hz on TONE, full depth.
            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_RATE), 5.0f);
            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_DEPTH), 1.0f);
            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_DEST), (float) toneDestIndex);

            drums.noteOn(pad, 1.0f);
            auto withLfo = render(drums, 4096);
            drums.noteOff(pad);

            // The two renders should differ.
            float diff = 0.0f;
            for (int i = 0; i < 4096; ++i)
            {
                float d = noLfo.getSample(0, i) - withLfo.getSample(0, i);
                diff += d * d;
            }
            diff = std::sqrt(diff / 4096.0f);
            expect(diff > 0.0001f, "Per-pad LFO on TONE changes the sound: diff " + juce::String(diff));
        }
    }

    void testPadSlotModulation()
    {
        beginTest("A per-pad slot with ONE > AMP negative reduces the level");
        {
            DrumEngine drums;
            drums.prepareToPlay(SR, BLOCK);
            const int pad = 8;
            const int ampDest = DrumVoice::ModAmp;

            // Render without slot.
            drums.noteOn(pad, 1.0f);
            auto noSlot = render(drums, 4096);
            drums.noteOff(pad);
            drums.allNotesOff();

            // Set per-pad slot: source ONE > AMP at -0.5 (level reduced).
            int oneSrc = 0;
            const auto* router = drums.modRouter();
            for (int i = 0; i < router->numSources(); ++i)
                if (router->sources().source(i) == mod::Src::One)
                { oneSrc = i; break; }
            drums.setParameter(PadModParam::id(pad, PadModParam::SLOT1_SRC), (float) oneSrc);
            drums.setParameter(PadModParam::id(pad, PadModParam::SLOT1_DEST), (float) ampDest);
            drums.setParameter(PadModParam::id(pad, PadModParam::SLOT1_AMT), -0.5f);

            drums.noteOn(pad, 1.0f);
            auto withSlot = render(drums, 4096);
            drums.noteOff(pad);

            float rmsNo = rms(noSlot);
            float rmsWith = rms(withSlot);
            expect(rmsWith < rmsNo * 0.95f, "Per-pad slot ONE>AMP reduces level: "
                   + juce::String(rmsWith) + " vs " + juce::String(rmsNo));
        }
    }

    void testProjectStorage()
    {
        beginTest("Per-pad mod cell ids appear in getParamPages");
        {
            DrumEngine drums;
            drums.prepareToPlay(SR, BLOCK);
            const int pad = 3;
            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_RATE), 7.5f);
            drums.setParameter(PadModParam::id(pad, PadModParam::SLOT1_AMT), 0.3f);

            const auto pages = drums.getParamPages();
            bool foundRate = false, foundSlotAmt = false;
            for (const auto& page : pages)
                for (const auto& spec : page.params)
                {
                    if (spec.id == PadModParam::id(pad, PadModParam::LFO_RATE)) foundRate = true;
                    if (spec.id == PadModParam::id(pad, PadModParam::SLOT1_AMT)) foundSlotAmt = true;
                }
            expect(foundRate, "LFO rate id found in pages");
            expect(foundSlotAmt, "Slot amount id found in pages");
            expectWithinAbsoluteError(drums.getParameter(PadModParam::id(pad, PadModParam::LFO_RATE)), 7.5f, 0.01f);
        }
    }

    void testPadLfoTrigger()
    {
        beginTest("Per-pad LFO in KEY mode restarts on a note-on");
        {
            DrumEngine drums;
            drums.prepareToPlay(SR, BLOCK);
            const int pad = 12;      // snare (a resonator drum)
            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_RATE), 2.0f);
            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_DEPTH), 1.0f);
            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_DEST), (float) DrumVoice::ModTone);
            drums.setParameter(PadModParam::id(pad, PadModParam::LFO_TRIGGER), 1.0f);  // KEY

            drums.noteOn(pad, 1.0f);
            render(drums, 2048);
            drums.noteOff(pad);
            drums.noteOn(pad, 1.0f);
            auto after = render(drums, 2048);
            drums.noteOff(pad);

            float r = rms(after);
            expect(r > 0.001f, "Sound after retrigger: " + juce::String(r));
        }
    }
};

static U5PadModTests u5PadModTests;
