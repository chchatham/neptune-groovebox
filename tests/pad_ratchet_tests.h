#pragma once

// Hold-to-ratchet pads (src/sequencer/PadRatchet.h).

#include "../src/sequencer/PadRatchet.h"

#include <cmath>
#include <vector>

class PadRatchetTests : public juce::UnitTest
{
public:
    PadRatchetTests() : UnitTest("Pad Ratchet") {}

    struct Event
    {
        juce::int64 time;   // start of the block the callback fired in
        bool on;
        int pad;
        int note;
        float velocity;
    };

    // A ratchet with its callbacks recorded against a sample clock.
    struct Rig
    {
        PadRatchet r;
        std::vector<Event> events;
        juce::int64 now = 0;
        double sampleRate = 48000.0;
        float bpm = 120.0f;

        Rig()
        {
            events.reserve(4096);
            r.onNoteOn = [this](int pad, int note, float vel) { events.push_back({ now, true, pad, note, vel }); };
            r.onNoteOff = [this](int pad, int note) { events.push_back({ now, false, pad, note, 0.0f }); };
        }

        void run(juce::int64 total, int block)
        {
            while (total > 0)
            {
                const int n = (int) juce::jmin((juce::int64) block, total);
                r.processBlock(n, sampleRate, bpm);
                now += n;
                total -= n;
            }
        }

        std::vector<Event> ons(int pad = -1) const
        {
            std::vector<Event> out;
            for (auto& e : events)
                if (e.on && (pad < 0 || e.pad == pad))
                    out.push_back(e);
            return out;
        }

        std::vector<Event> offs(int pad = -1) const
        {
            std::vector<Event> out;
            for (auto& e : events)
                if (! e.on && (pad < 0 || e.pad == pad))
                    out.push_back(e);
            return out;
        }

        int numOns(int pad = -1) const { return (int) ons(pad).size(); }
        int numOffs(int pad = -1) const { return (int) offs(pad).size(); }

        // Per pad: on and off strictly alternate, starting with on, same note,
        // and nothing is left sounding at the end.
        bool balanced() const
        {
            for (int p = 0; p < PadRatchet::kNumPads; ++p)
            {
                bool sounding = false;
                int note = -1;
                for (auto& e : events)
                {
                    if (e.pad != p)
                        continue;
                    if (e.on)
                    {
                        if (sounding)
                            return false;
                        sounding = true;
                        note = e.note;
                    }
                    else
                    {
                        if (! sounding || e.note != note)
                            return false;
                        sounding = false;
                    }
                }
                if (sounding)
                    return false;
            }
            return true;
        }
    };

    // Hits of one pad held for `total` samples with a fixed pressure.
    static int countHits(PadRatchet::Mode mode, float pressure, float depth, juce::int64 total, int block)
    {
        Rig rig;
        rig.r.setEnabled(true);
        rig.r.setMode(mode);
        rig.r.setBaseDivision(PadRatchet::Div1_8);
        rig.r.setMaxDivision(PadRatchet::Div1_32);
        rig.r.setBaseRateHz(2.0f);
        rig.r.setMaxRateHz(32.0f);
        rig.r.setPressureDepth(depth);
        rig.r.padDown(0, 60, 1.0f);
        rig.r.setPressure(0, pressure);
        rig.run(total, block);
        return rig.numOns();
    }

    void runTest() override
    {
        beginTest("Defaults, ranges and names");
        {
            PadRatchet r;
            expect(! r.isEnabled());
            expect(r.getMode() == PadRatchet::Mode::Quantized);
            expectEquals(r.getBaseDivision(), (int) PadRatchet::Div1_8);
            expectEquals(r.getMaxDivision(), (int) PadRatchet::Div1_128);
            expect(! r.isLatch(), "the roll latch is off to begin with");
            expectEquals(juce::String(PadRatchet::getDivisionName(PadRatchet::Div1_128)), juce::String("1/128"));
            expectWithinAbsoluteError(PadRatchet::getDivisionBeats(PadRatchet::Div1_128), 1.0 / 32.0, 1.0e-12);

            r.setGate(0.0f);
            expectWithinAbsoluteError(r.getGate(), 0.05f, 1.0e-6f);
            r.setGate(3.0f);
            expectWithinAbsoluteError(r.getGate(), 1.0f, 1.0e-6f);
            r.setPressureDepth(-1.0f);
            expectEquals(r.getPressureDepth(), 0.0f);
            r.setPressureToVelocity(2.0f);
            expectEquals(r.getPressureToVelocity(), 1.0f);
            r.setVelocityDecay(2.0f);
            expectEquals(r.getVelocityDecay(), 1.0f);
            r.setBaseDivision(-4);
            expectEquals(r.getBaseDivision(), 0);
            r.setMaxDivision(999);
            expectEquals(r.getMaxDivision(), PadRatchet::getNumDivisions() - 1);
            r.setBaseRateHz(0.0f);
            expectEquals(r.getBaseRateHz(), PadRatchet::kMinRateHz);
            r.setMaxRateHz(1000.0f);
            expectEquals(r.getMaxRateHz(), PadRatchet::kMaxRateHz);
            r.setMode(PadRatchet::Mode::Free);
            expect(r.getMode() == PadRatchet::Mode::Free);

            expect(PadRatchet::getNumDivisions() >= 8);
            for (int d = 0; d < PadRatchet::getNumDivisions(); ++d)
            {
                const juce::String name(PadRatchet::getDivisionName(d));
                expect(name.isNotEmpty() && name.length() <= 5, name);
                if (d > 0)
                    expect(PadRatchet::getDivisionBeats(d) < PadRatchet::getDivisionBeats(d - 1), name);
            }
            expectEquals(juce::String(PadRatchet::getDivisionName(PadRatchet::Div1_16T)), juce::String("1/16T"));
            expectEquals(juce::String(PadRatchet::getDivisionName(PadRatchet::Div1_4)), juce::String("1/4"));
            expectEquals(juce::String(PadRatchet::getDivisionName(PadRatchet::Div1_64)), juce::String("1/64"));
            expectWithinAbsoluteError(PadRatchet::getDivisionBeats(PadRatchet::Div1_16), 0.25, 1.0e-12);
            expectWithinAbsoluteError(PadRatchet::getDivisionBeats(PadRatchet::Div1_8T), 1.0 / 3.0, 1.0e-12);
            expect(juce::String(PadRatchet::getModeName(PadRatchet::Mode::Free)).length() <= 5);
            expect(juce::String(PadRatchet::getModeName(PadRatchet::Mode::Quantized)).length() <= 5);
        }

        beginTest("Exact hit count and spacing, any block size");
        {
            // 1/16 at 120 bpm, 48 kHz = 6000 samples. 99000 samples: hits at 0..96000.
            for (int block : { 1, 64, 512, 99000 })
            {
                Rig rig;
                rig.r.setEnabled(true);
                rig.r.setBaseDivision(PadRatchet::Div1_16);
                rig.r.setMaxDivision(PadRatchet::Div1_16);
                rig.r.padDown(3, 40, 0.9f);
                rig.run(99000, block);

                auto ons = rig.ons();
                expectEquals((int) ons.size(), 17, "block " + juce::String(block));
                if (block < 99000)
                    for (size_t k = 0; k < ons.size(); ++k)
                    {
                        const juce::int64 exact = (juce::int64) k * 6000;
                        expect(ons[k].time == (exact / block) * block,
                               "block " + juce::String(block) + " hit " + juce::String((int) k));
                        expectEquals(ons[k].pad, 3);
                        expectEquals(ons[k].note, 40);
                    }
            }
        }

        beginTest("Fractional intervals carry across blocks");
        {
            // 1/32 at 120 bpm, 44.1 kHz = 2756.25 samples.
            for (int block : { 1, 64, 512 })
            {
                Rig rig;
                rig.sampleRate = 44100.0;
                rig.r.setEnabled(true);
                rig.r.setBaseDivision(PadRatchet::Div1_32);
                rig.r.setMaxDivision(PadRatchet::Div1_32);
                rig.r.padDown(0, 60, 1.0f);
                rig.run(110250 + 100, block);   // 40 intervals and a bit

                auto ons = rig.ons();
                expectEquals((int) ons.size(), 41, "block " + juce::String(block));
                for (size_t k = 0; k < ons.size(); ++k)
                {
                    const juce::int64 exact = (juce::int64) std::floor((double) k * 2756.25);
                    expect(ons[k].time == (exact / block) * block,
                           "block " + juce::String(block) + " hit " + juce::String((int) k));
                }
            }
        }

        beginTest("Divisions follow the tempo, triplets included");
        {
            // 1/8T at 90 bpm, 48 kHz = 10666.67 samples: 45 hits in 475000 samples.
            for (int block : { 64, 512 })
            {
                Rig rig;
                rig.bpm = 90.0f;
                rig.r.setEnabled(true);
                rig.r.setBaseDivision(PadRatchet::Div1_8T);
                rig.r.setMaxDivision(PadRatchet::Div1_8T);
                rig.r.padDown(0, 60, 1.0f);
                rig.run(475000, block);
                expectEquals(rig.numOns(), 45);
            }

            // A tempo change applies from the next hit, not mid-interval.
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setBaseDivision(PadRatchet::Div1_16);
            rig.r.setMaxDivision(PadRatchet::Div1_16);
            rig.r.padDown(0, 60, 1.0f);
            rig.run(3000, 1);
            rig.bpm = 240.0f;
            rig.run(10000, 1);
            auto ons = rig.ons();
            expect(ons.size() >= 4);
            if (ons.size() >= 4)
            {
                expect(ons[1].time == 6000);
                expect(ons[2].time == 9000);
                expect(ons[3].time == 12000);
            }
        }

        beginTest("Quantized: pressure raises the rate in division steps");
        {
            // 1/8 .. 1/32 at 120 bpm, 48 kHz: 12000, 8000, 6000, 4000, 3000 samples.
            for (int block : { 64, 512 })
            {
                expectEquals(countHits(PadRatchet::Mode::Quantized, 0.0f, 1.0f, 48100, block), 5);
                expectEquals(countHits(PadRatchet::Mode::Quantized, 0.3f, 1.0f, 48100, block), 7);
                expectEquals(countHits(PadRatchet::Mode::Quantized, 0.5f, 1.0f, 48100, block), 9);
                expectEquals(countHits(PadRatchet::Mode::Quantized, 0.7f, 1.0f, 48100, block), 13);
                expectEquals(countHits(PadRatchet::Mode::Quantized, 1.0f, 1.0f, 48100, block), 17);
            }

            // Max slower than base: pressure slows the roll.
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setBaseDivision(PadRatchet::Div1_16);
            rig.r.setMaxDivision(PadRatchet::Div1_8);
            rig.r.padDown(0, 60, 1.0f);
            rig.r.setPressure(0, 1.0f);
            rig.run(48100, 64);
            expectEquals(rig.numOns(), 5);
        }

        beginTest("As it comes: the harder the pad is pressed, the faster the roll, through every rate from 1/8 to 1/128");
        {
            // Nine rates from BASE to MAX, each with its own share of the pressure, lightest first.
            const int first = PadRatchet::Div1_8, last = PadRatchet::Div1_128, bands = last - first + 1;
            expectEquals(bands, 9);
            double previous = 1.0e12;
            for (int band = 0; band < bands; ++band)
            {
                Rig rig;
                rig.r.setEnabled(true);                    // nothing else is set: the defaults
                rig.r.padDown(0, 60, 1.0f);
                rig.r.setPressure(0, ((float) band + 0.5f) / (float) bands);
                rig.run(48000, 16);
                auto ons = rig.ons();
                expect(ons.size() >= 3);
                if (ons.size() < 3) continue;
                const double gap = (double) (ons[2].time - ons[1].time);
                expectWithinAbsoluteError(gap, PadRatchet::getDivisionBeats(first + band) * 24000.0, 17.0,
                                          juce::String(PadRatchet::getDivisionName(first + band)));
                expect(gap < previous, "faster than with less pressure");
                previous = gap;
            }
            // Let go: it stops (no latch).
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.padDown(0, 60, 1.0f);
            rig.r.setPressure(0, 1.0f);
            rig.run(24000, 16);
            rig.r.padUp(0);
            rig.run(64, 16);
            const int hits = rig.numOns();
            rig.run(48000, 16);
            expectEquals(rig.numOns(), hits, "nothing after the pad is released");
            expect(rig.balanced());
        }

        beginTest("Quantized: a rate change waits for the next hit");
        {
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setBaseDivision(PadRatchet::Div1_8);
            rig.r.setMaxDivision(PadRatchet::Div1_32);
            rig.r.padDown(0, 60, 1.0f);
            rig.run(1000, 1);
            rig.r.setPressure(0, 1.0f);
            rig.run(20000, 1);
            auto ons = rig.ons();
            expect(ons.size() >= 4);
            if (ons.size() >= 4)
            {
                expect(ons[1].time == 12000);   // still the 1/8 it started
                expect(ons[2].time == 15000);   // then 1/32
                expect(ons[3].time == 18000);
            }
        }

        beginTest("Quantized: hysteresis at a division boundary");
        {
            // 1/16 .. 1/32 = three bands, boundary between 1/16 and 1/16T at 1/3.
            auto wobble = [](float startPressure)
            {
                Rig rig;
                rig.r.setEnabled(true);
                rig.r.setBaseDivision(PadRatchet::Div1_16);
                rig.r.setMaxDivision(PadRatchet::Div1_32);
                rig.r.padDown(0, 60, 1.0f);
                rig.r.setPressure(0, startPressure);
                rig.run(64, 64);
                for (int i = 0; i < 751; ++i)
                {
                    rig.r.setPressure(0, (i & 1) ? 0.32f : 0.345f);
                    rig.run(64, 64);
                }
                return rig.ons();
            };

            auto low = wobble(0.2f);    // starts on 1/16 and stays there
            expectEquals((int) low.size(), 9);
            auto high = wobble(0.45f);  // starts on 1/16T and stays there
            expectEquals((int) high.size(), 13);

            // A clear move past the boundary does change the division.
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setBaseDivision(PadRatchet::Div1_16);
            rig.r.setMaxDivision(PadRatchet::Div1_32);
            rig.r.padDown(0, 60, 1.0f);
            rig.r.setPressure(0, 0.2f);
            rig.run(64, 64);
            rig.r.setPressure(0, 0.5f);
            rig.run(48100 - 64, 64);
            // 0, 6000, then every 4000 up to 46000
            expectEquals(rig.numOns(), 12);
        }

        beginTest("Free: pressure sweeps the rate");
        {
            // 2 .. 32 Hz at 48 kHz: 24000, 6000 (half pressure), 1500 samples.
            for (int block : { 64, 512 })
            {
                expectEquals(countHits(PadRatchet::Mode::Free, 0.0f, 1.0f, 96100, block), 5);
                expectEquals(countHits(PadRatchet::Mode::Free, 0.5f, 1.0f, 48100, block), 9);
                expectEquals(countHits(PadRatchet::Mode::Free, 1.0f, 1.0f, 48100, block), 33);
            }

            // Half depth: full pressure reaches half way (exponentially) = 8 Hz.
            expectEquals(countHits(PadRatchet::Mode::Free, 1.0f, 0.5f, 48100, 64), 9);

            // The sweep is continuous: more pressure, monotonically more hits.
            int previous = 0;
            for (int i = 0; i <= 10; ++i)
            {
                const int hits = countHits(PadRatchet::Mode::Free, (float) i / 10.0f, 1.0f, 96100, 64);
                expect(hits >= previous);
                if (i > 0 && i % 5 == 0)
                    expect(hits > previous);
                previous = hits;
            }

            // Free mode follows pressure mid-interval instead of waiting 0.5 s.
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setMode(PadRatchet::Mode::Free);
            rig.r.setBaseRateHz(2.0f);
            rig.r.setMaxRateHz(32.0f);
            rig.r.padDown(0, 60, 1.0f);
            rig.run(4800, 64);
            rig.r.setPressure(0, 1.0f);
            rig.run(19200, 64);
            auto ons = rig.ons();
            expect(ons.size() >= 3);
            if (ons.size() >= 3)
            {
                expect(ons[1].time > 4800 && ons[1].time < 8000, juce::String(ons[1].time));
                // settled at 32 Hz afterwards
                const auto gap = ons.back().time - ons[ons.size() - 2].time;
                expect(gap >= 1500 - 64 && gap <= 1500 + 64, juce::String(gap));
            }
        }

        beginTest("Pressure smoothing: noisy aftertouch does not jitter the rate");
        {
            // Free mode, pressure alternating 0.45 / 0.55 every block behaves
            // like a steady 0.5 (8 Hz = 6000 samples) to within a few percent.
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setMode(PadRatchet::Mode::Free);
            rig.r.setBaseRateHz(2.0f);
            rig.r.setMaxRateHz(32.0f);
            rig.r.padDown(0, 60, 1.0f);
            rig.r.setPressure(0, 0.5f);
            for (int i = 0; i < 3000; ++i)
            {
                rig.r.setPressure(0, (i & 1) ? 0.45f : 0.55f);
                rig.run(64, 64);
            }
            auto ons = rig.ons();
            expect(ons.size() >= 30);
            for (size_t k = 2; k < ons.size(); ++k)
            {
                const auto gap = ons[k].time - ons[k - 1].time;
                expect(gap > 5600 && gap < 6400, juce::String(gap));
            }
        }

        beginTest("Depth 0 ignores pressure");
        {
            expectEquals(countHits(PadRatchet::Mode::Quantized, 1.0f, 0.0f, 48100, 64), 5);
            expectEquals(countHits(PadRatchet::Mode::Quantized, 0.6f, 0.0f, 48100, 512), 5);
            expectEquals(countHits(PadRatchet::Mode::Free, 1.0f, 0.0f, 96100, 64), 5);
            expectEquals(countHits(PadRatchet::Mode::Free, 0.6f, 0.0f, 96100, 512), 5);
        }

        beginTest("First hit is the pad press; no hits after padUp");
        {
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setBaseDivision(PadRatchet::Div1_16);
            rig.r.setMaxDivision(PadRatchet::Div1_16);
            expect(! rig.r.isRolling(2));
            rig.r.padDown(2, 72, 0.7f);
            expectEquals(rig.numOns(), 0);          // callbacks only come from processBlock
            rig.run(64, 64);
            expectEquals(rig.numOns(), 1);
            expect(rig.r.isRolling(2));
            auto first = rig.ons();
            expect(first[0].time == 0);
            expectEquals(first[0].pad, 2);
            expectEquals(first[0].note, 72);
            expectWithinAbsoluteError(first[0].velocity, 0.7f, 1.0e-3f);

            rig.run(20000 - 64, 64);
            expectEquals(rig.numOns(), 4);
            rig.r.padUp(2);
            rig.run(48000, 64);
            expectEquals(rig.numOns(), 4);
            expectEquals(rig.numOffs(), 4);
            expect(rig.balanced());
            expect(! rig.r.isRolling(2));

            // The held note (hit at 18000, gate end at 21000) is cut by padUp,
            // in the first block after it.
            expect(rig.offs().back().time == 20000);
        }

        beginTest("Every note-on gets its note-off");
        {
            for (float gate : { 0.05f, 0.5f, 1.0f })
            {
                // padUp
                {
                    Rig rig;
                    rig.r.setEnabled(true);
                    rig.r.setGate(gate);
                    rig.r.padDown(0, 60, 1.0f);
                    rig.r.setPressure(0, 1.0f);
                    rig.run(40000, 64);
                    expect(rig.numOns() > 5);
                    rig.r.padUp(0);
                    rig.run(128, 64);
                    expect(rig.balanced());
                    expectEquals(rig.numOns(), rig.numOffs());
                }
                // allOff with several pads
                {
                    Rig rig;
                    rig.r.setEnabled(true);
                    rig.r.setGate(gate);
                    for (int p : { 0, 7, 15 })
                    {
                        rig.r.padDown(p, 36 + p, 1.0f);
                        rig.r.setPressure(p, 0.1f * (float) p);
                    }
                    rig.run(40100, 512);
                    rig.r.allOff();
                    rig.run(512, 512);
                    expect(rig.balanced());
                    const int count = rig.numOns();
                    rig.run(48000, 512);
                    expectEquals(rig.numOns(), count);   // and the rolls have stopped
                    for (int p : { 0, 7, 15 })
                        expect(! rig.r.isRolling(p));
                }
                // disabled mid-roll, pads still held
                {
                    Rig rig;
                    rig.r.setEnabled(true);
                    rig.r.setGate(gate);
                    rig.r.padDown(1, 61, 1.0f);
                    rig.r.padDown(2, 62, 1.0f);
                    rig.run(100, 64);            // both notes sounding
                    rig.r.setEnabled(false);
                    rig.run(64, 64);
                    expect(rig.balanced());
                    const int count = rig.numOns();
                    rig.run(48000, 64);
                    expectEquals(rig.numOns(), count);
                    rig.r.padUp(1);
                    rig.r.padUp(2);
                    rig.run(64, 64);
                    expect(rig.balanced());
                    expectEquals(rig.numOns(), rig.numOffs());
                }
                // pressing the same pad again without a release
                {
                    Rig rig;
                    rig.r.setEnabled(true);
                    rig.r.setGate(gate);
                    rig.r.padDown(4, 50, 1.0f);
                    rig.run(100, 64);
                    rig.r.padDown(4, 53, 1.0f);
                    rig.run(100, 64);
                    rig.r.padUp(4);
                    rig.run(64, 64);
                    expect(rig.balanced());
                    expectEquals(rig.numOns(), 2);
                }
            }

            // A tap that starts and ends between two blocks still plays once.
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.padDown(9, 45, 0.5f);
            rig.r.padUp(9);
            rig.run(48000, 512);
            expectEquals(rig.numOns(), 1);
            expectEquals(rig.numOffs(), 1);
            expect(rig.balanced());

            // Far more events than the queue holds, with no audio running:
            // nothing is left sounding afterwards.
            Rig flood;
            flood.r.setEnabled(true);
            for (int i = 0; i < 1000; ++i)
                flood.r.padDown(i % 16, 60 + (i % 16), 1.0f);
            flood.run(64, 64);
            expect(flood.balanced());
            const int count = flood.numOns();
            flood.run(48000, 64);
            expectEquals(flood.numOns(), count);
        }

        beginTest("Pads are independent");
        {
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setBaseDivision(PadRatchet::Div1_8);
            rig.r.setMaxDivision(PadRatchet::Div1_32);
            rig.r.padDown(0, 36, 1.0f);
            rig.r.padDown(5, 41, 0.5f);
            rig.r.setPressure(5, 1.0f);
            rig.run(48100, 64);
            expectEquals(rig.numOns(0), 5);
            expectEquals(rig.numOns(5), 17);
            for (auto& e : rig.ons(0))
                expectEquals(e.note, 36);
            for (auto& e : rig.ons(5))
                expectEquals(e.note, 41);

            rig.r.padUp(0);
            rig.run(48000 - 100 + 100, 64);   // to 96100
            expectEquals(rig.numOns(0), 5);
            expectEquals(rig.numOns(5), 33);
            expectEquals(rig.numOns(1), 0);

            rig.r.padUp(5);
            rig.run(64, 64);
            expect(rig.balanced());

            // Out-of-range pads are ignored.
            rig.r.padDown(-1, 60, 1.0f);
            rig.r.padDown(16, 60, 1.0f);
            rig.r.setPressure(99, 1.0f);
            rig.r.padUp(-3);
            const int count = rig.numOns();
            rig.run(4800, 64);
            expectEquals(rig.numOns(), count);
        }

        beginTest("Gate sets the note length");
        {
            auto firstLength = [](float gate)
            {
                Rig rig;
                rig.r.setEnabled(true);
                rig.r.setBaseDivision(PadRatchet::Div1_16);
                rig.r.setMaxDivision(PadRatchet::Div1_16);
                rig.r.setGate(gate);
                rig.r.padDown(0, 60, 1.0f);
                rig.run(20000, 1);
                auto ons = rig.ons();
                auto offs = rig.offs();
                if (offs.size() < 3 || ons.size() < 3)
                    return (juce::int64) -1;
                // every note the same length
                if (offs[1].time - ons[1].time != offs[0].time - ons[0].time)
                    return (juce::int64) -2;
                return offs[0].time - ons[0].time;
            };

            expect(firstLength(0.25f) == 1500);
            expect(firstLength(0.5f) == 3000);
            expect(firstLength(0.05f) == 300);
            expect(firstLength(1.0f) == 6000);

            // At full gate the note-off comes immediately before the next note-on.
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setBaseDivision(PadRatchet::Div1_16);
            rig.r.setMaxDivision(PadRatchet::Div1_16);
            rig.r.setGate(1.0f);
            rig.r.padDown(0, 60, 1.0f);
            rig.run(20000, 64);
            expect(rig.events.size() >= 5);
            for (size_t i = 0; i < rig.events.size(); ++i)
                expect(rig.events[i].on == ((i & 1) == 0));
        }

        beginTest("Velocity follows pressure");
        {
            auto velocities = [](float amount, float decay, float pressure, float padVelocity)
            {
                Rig rig;
                rig.r.setEnabled(true);
                rig.r.setBaseDivision(PadRatchet::Div1_16);
                rig.r.setMaxDivision(PadRatchet::Div1_16);
                rig.r.setPressureToVelocity(amount);
                rig.r.setVelocityDecay(decay);
                rig.r.padDown(0, 60, padVelocity);
                rig.r.setPressure(0, pressure);
                rig.run(60100, 64);
                std::vector<float> v;
                for (auto& e : rig.ons())
                    v.push_back(e.velocity);
                return v;
            };

            auto full = velocities(1.0f, 0.0f, 0.8f, 0.3f);
            expectEquals((int) full.size(), 11);
            expectWithinAbsoluteError(full[0], 0.3f, 1.0e-3f);   // the press keeps its own velocity
            for (size_t i = 1; i < full.size(); ++i)
                expectWithinAbsoluteError(full[i], 0.8f, 1.0e-3f);

            auto none = velocities(0.0f, 0.0f, 0.8f, 0.3f);
            for (float v : none)
                expectWithinAbsoluteError(v, 0.3f, 1.0e-3f);

            auto half = velocities(0.5f, 0.0f, 0.8f, 0.3f);
            for (size_t i = 1; i < half.size(); ++i)
                expectWithinAbsoluteError(half[i], 0.55f, 1.0e-3f);

            // Zero pressure at full amount never produces a zero-velocity note-on.
            auto floor = velocities(1.0f, 0.0f, 0.0f, 0.9f);
            for (size_t i = 1; i < floor.size(); ++i)
                expectWithinAbsoluteError(floor[i], PadRatchet::kMinVelocity, 1.0e-6f);

            // Decay: each hit half the one before, down to the floor.
            auto fade = velocities(0.0f, 0.5f, 0.0f, 1.0f);
            expectEquals((int) fade.size(), 11);
            expectWithinAbsoluteError(fade[0], 1.0f, 1.0e-3f);
            expectWithinAbsoluteError(fade[1], 0.5f, 1.0e-3f);
            expectWithinAbsoluteError(fade[2], 0.25f, 1.0e-3f);
            expectWithinAbsoluteError(fade[3], 0.125f, 1.0e-3f);
            expectWithinAbsoluteError(fade[10], PadRatchet::kMinVelocity, 1.0e-6f);

            // A pressure swell swells the roll.
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setBaseDivision(PadRatchet::Div1_16);
            rig.r.setMaxDivision(PadRatchet::Div1_16);
            rig.r.setPressureToVelocity(1.0f);
            rig.r.padDown(0, 60, 0.1f);
            for (int i = 0; i < 1000; ++i)
            {
                rig.r.setPressure(0, (float) i / 1000.0f);
                rig.run(64, 64);
            }
            auto ons = rig.ons();
            expectEquals((int) ons.size(), 11);
            for (size_t i = 2; i < ons.size(); ++i)
                expect(ons[i].velocity > ons[i - 1].velocity);
            expect(ons.back().velocity > 0.85f);
        }

        beginTest("Disabled: a single pass-through note");
        {
            Rig rig;
            expect(! rig.r.isEnabled());
            rig.r.padDown(6, 67, 0.6f);
            rig.r.setPressure(6, 1.0f);
            rig.run(96000, 64);
            expectEquals(rig.numOns(), 1);
            expectEquals(rig.numOffs(), 0);       // held until the pad is released
            expect(! rig.r.isRolling(6));
            auto ons = rig.ons();
            expectEquals(ons[0].pad, 6);
            expectEquals(ons[0].note, 67);
            expectWithinAbsoluteError(ons[0].velocity, 0.6f, 1.0e-3f);

            rig.r.padUp(6);
            rig.run(64, 64);
            expectEquals(rig.numOffs(), 1);
            expect(rig.balanced());

            // Enabling while a plain note is held does not start a roll, and the
            // note still ends on release.
            rig.r.padDown(6, 67, 0.6f);
            rig.run(64, 64);
            rig.r.setEnabled(true);
            rig.run(48000, 64);
            expectEquals(rig.numOns(), 2);
            rig.r.padUp(6);
            rig.run(64, 64);
            expect(rig.balanced());

            // allOff ends plain notes too.
            rig.r.setEnabled(false);
            rig.r.padDown(3, 50, 1.0f);
            rig.run(64, 64);
            rig.r.allOff();
            rig.run(64, 64);
            expect(rig.balanced());
        }

        beginTest("No callbacks set: runs without crashing");
        {
            PadRatchet r;
            r.setEnabled(true);
            r.padDown(0, 60, 1.0f);
            r.setPressure(0, 1.0f);
            for (int i = 0; i < 100; ++i)
                r.processBlock(512, 48000.0, 120.0f);
            r.processBlock(0, 48000.0, 120.0f);
            r.processBlock(512, 0.0, 0.0f);
            r.padUp(0);
            r.processBlock(512, 48000.0, 120.0f);
            expect(! r.isRolling(0));
        }

        beginTest("Roll extras: defaults and ranges");
        {
            PadRatchet r;
            expectEquals(r.getSwing(), 0.0f);
            expectEquals(r.getGlide(), 0.0f);
            expectEquals(r.getThreshold(), 0.0f);
            expect(! r.isLatch());
            expectEquals(r.getHumanize(), 0.0f);
            expectEquals(r.getAccent(), 0);

            r.setSwing(2.0f);       expectEquals(r.getSwing(), 1.0f);
            r.setSwing(-1.0f);      expectEquals(r.getSwing(), 0.0f);
            r.setGlide(99.0f);      expectEquals(r.getGlide(), PadRatchet::kMaxGlideSeconds);
            r.setGlide(-1.0f);      expectEquals(r.getGlide(), 0.0f);
            r.setThreshold(1.5f);   expectEquals(r.getThreshold(), 1.0f);
            r.setThreshold(-0.5f);  expectEquals(r.getThreshold(), 0.0f);
            r.setHumanize(7.0f);    expectEquals(r.getHumanize(), 1.0f);
            r.setHumanize(-7.0f);   expectEquals(r.getHumanize(), 0.0f);
            r.setLatch(true);       expect(r.isLatch());
            r.setAccent(1);         expectEquals(r.getAccent(), 0);
            r.setAccent(-3);        expectEquals(r.getAccent(), 0);
            r.setAccent(4);         expectEquals(r.getAccent(), 4);
            r.setAccent(99);        expectEquals(r.getAccent(), PadRatchet::kMaxAccent);
            expect(! r.isLatched(0));
            expect(! r.isLatched(-1));
        }

        beginTest("Swing delays every second hit, quantized only");
        {
            // 1/16 at 120 bpm, 48 kHz = 6000 samples.
            auto times = [](Rig& rig, float swing, PadRatchet::Mode mode, float gate)
            {
                rig.r.setEnabled(true);
                rig.r.setMode(mode);
                rig.r.setBaseDivision(PadRatchet::Div1_16);
                rig.r.setMaxDivision(PadRatchet::Div1_16);
                rig.r.setBaseRateHz(8.0f);
                rig.r.setMaxRateHz(8.0f);
                rig.r.setSwing(swing);
                rig.r.setGate(gate);
                rig.r.padDown(0, 60, 1.0f);
                rig.run(60100, 1);
                rig.r.padUp(0);
                rig.run(8, 1);
            };

            for (float swing : { 0.0f, 0.5f, 1.0f })
            {
                Rig rig;
                times(rig, swing, PadRatchet::Mode::Quantized, 0.5f);
                auto ons = rig.ons();
                expectEquals((int) ons.size(), 11);
                const juce::int64 shift = (juce::int64) std::lround(swing * 3000.0f);
                for (size_t k = 0; k < ons.size(); ++k)
                {
                    const juce::int64 expected = (juce::int64) k * 6000 + ((k & 1) ? shift : 0);
                    expect(ons[k].time == expected, "swing " + juce::String(swing) + " hit " + juce::String((int) k)
                                                        + " at " + juce::String(ons[k].time));
                }
                // ratio of the two halves of a pair: 50/50 .. 75/25
                const double a = (double) (ons[1].time - ons[0].time);
                const double b = (double) (ons[2].time - ons[1].time);
                expectWithinAbsoluteError(a / (a + b), 0.5 + 0.25 * (double) swing, 1.0e-9);

                // gate follows the swung gaps
                auto offs = rig.offs();
                expect(offs.size() >= 2);
                expect(offs[0].time - ons[0].time == (6000 + shift) / 2);
                expect(offs[1].time - ons[1].time == (6000 - shift) / 2);
                expect(rig.balanced());
                expectEquals(rig.numOns(), rig.numOffs());
            }

            // Free mode ignores swing.
            Rig rig;
            times(rig, 1.0f, PadRatchet::Mode::Free, 0.5f);
            auto ons = rig.ons();
            expectEquals((int) ons.size(), 11);
            for (size_t k = 0; k < ons.size(); ++k)
                expect(ons[k].time == (juce::int64) k * 6000);
            expect(rig.balanced());

            // Legato gate with full swing stays balanced.
            Rig legato;
            times(legato, 1.0f, PadRatchet::Mode::Quantized, 1.0f);
            expect(legato.balanced());
            expectEquals(legato.numOns(), 11);
        }

        beginTest("Snap glide: the interval slides to the new division and lands exactly");
        {
            // 1/8 (12000) .. 1/32 (3000) at 120 bpm, 48 kHz, glide 0.5 s = 24000 samples.
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setBaseDivision(PadRatchet::Div1_8);
            rig.r.setMaxDivision(PadRatchet::Div1_32);
            rig.r.setGlide(0.5f);
            rig.r.padDown(0, 60, 1.0f);
            rig.run(1000, 1);
            rig.r.setPressure(0, 1.0f);
            rig.run(80000, 1);

            auto gapsOf = [](const std::vector<Event>& ons, size_t from)
            {
                std::vector<juce::int64> g;
                for (size_t k = from + 1; k < ons.size(); ++k)
                    g.push_back(ons[k].time - ons[k - 1].time);
                return g;
            };

            auto up = rig.ons();
            auto gaps = gapsOf(up, 0);
            expect(gaps.size() > 10);
            expect(gaps[0] == 12000);                 // the interval running when pressure changed
            int between = 0;
            juce::int64 glideEnd = -1;
            for (size_t k = 1; k < gaps.size(); ++k)
            {
                expect(gaps[k] <= gaps[k - 1], "gap " + juce::String((int) k) + " = " + juce::String(gaps[k]));
                expect(gaps[k] >= 3000);
                if (gaps[k] > 3001 && gaps[k] < 11999)
                    ++between;
                if (gaps[k] == 3000 && glideEnd < 0)
                    glideEnd = up[k].time;            // start of the first exact interval
            }
            expect(between >= 3, "intermediate intervals: " + juce::String(between));
            expect(gaps[1] < 12000);                  // the glide starts at the first hit after the change
            expect(glideEnd > 12000 + 12000 && glideEnd <= 12000 + 24000 + 3000, juce::String(glideEnd));
            // exact from there on
            for (size_t k = 1; k < up.size(); ++k)
                if (up[k - 1].time >= glideEnd)
                    expect(up[k].time - up[k - 1].time == 3000);
            expect(gaps.back() == 3000);

            // And back down: intervals grow monotonically to exactly 1/8.
            const size_t mark = up.size() - 1;
            rig.r.setPressure(0, 0.0f);
            rig.run(120000, 1);
            auto down = gapsOf(rig.ons(), mark);
            expect(down.size() > 5);
            between = 0;
            for (size_t k = 1; k < down.size(); ++k)
            {
                expect(down[k] >= down[k - 1], "down gap " + juce::String((int) k) + " = " + juce::String(down[k]));
                expect(down[k] <= 12000);
                if (down[k] > 3001 && down[k] < 11999)
                    ++between;
            }
            expect(between >= 2, "intermediate intervals: " + juce::String(between));
            expect(down.back() == 12000);
            expect(down[down.size() - 2] == 12000);

            rig.r.padUp(0);
            rig.run(8, 1);
            expect(rig.balanced());
            expectEquals(rig.numOns(), rig.numOffs());

            // A glide shorter than the faster interval is a jump (same as glide 0).
            Rig jump;
            jump.r.setEnabled(true);
            jump.r.setBaseDivision(PadRatchet::Div1_8);
            jump.r.setMaxDivision(PadRatchet::Div1_32);
            jump.r.setGlide(0.05f);
            jump.r.padDown(0, 60, 1.0f);
            jump.run(1000, 1);
            jump.r.setPressure(0, 1.0f);
            jump.run(20000, 1);
            auto j = jump.ons();
            expect(j.size() >= 4);
            if (j.size() >= 4)
            {
                expect(j[1].time == 12000);
                expect(j[2].time == 15000);
                expect(j[3].time == 18000);
            }

            // Free mode is unaffected by glide.
            Rig a, b;
            for (Rig* f : { &a, &b })
            {
                f->r.setEnabled(true);
                f->r.setMode(PadRatchet::Mode::Free);
                f->r.setGlide(f == &a ? 0.0f : 2.0f);
                f->r.padDown(0, 60, 1.0f);
                f->run(4800, 64);
                f->r.setPressure(0, 1.0f);
                f->run(48000, 64);
            }
            expectEquals(a.numOns(), b.numOns());
        }

        beginTest("Pressure threshold gates the repeats, with hysteresis");
        {
            // 1/16 = 6000 samples. Threshold 0.5, repeats stop below 0.45.
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setBaseDivision(PadRatchet::Div1_16);
            rig.r.setMaxDivision(PadRatchet::Div1_16);
            rig.r.setThreshold(0.5f);
            rig.r.padDown(0, 60, 0.7f);
            rig.r.setPressure(0, 0.2f);
            rig.run(30016, 64);
            expectEquals(rig.numOns(), 1);            // the press, held
            expectEquals(rig.numOffs(), 0);
            expect(! rig.r.isRolling(0));
            expectWithinAbsoluteError(rig.ons()[0].velocity, 0.7f, 1.0e-3f);

            // Crossing upward retriggers at once and rolls.
            rig.r.setPressure(0, 0.8f);
            rig.run(20032, 64);
            auto ons = rig.ons();
            expectEquals((int) ons.size(), 5);
            if (ons.size() == 5)
            {
                expect(ons[1].time >= 30016 && ons[1].time < 30016 + 1000, juce::String(ons[1].time));
                for (size_t k = 2; k < 5; ++k)
                {
                    const auto gap = ons[k].time - ons[k - 1].time;
                    expect(gap >= 6000 - 64 && gap <= 6000 + 64, juce::String(gap));
                }
            }
            expect(rig.r.isRolling(0));

            // Inside the hysteresis band: keeps rolling.
            rig.r.setPressure(0, 0.47f);
            rig.run(12032, 64);
            expectEquals(rig.numOns(), 7);
            expect(rig.r.isRolling(0));

            // Clearly below: no more hits, the current note ends at its gate.
            rig.r.setPressure(0, 0.3f);
            rig.run(64 * 20, 64);
            const int count = rig.numOns();
            expect(! rig.r.isRolling(0));
            rig.run(48000, 64);
            expectEquals(rig.numOns(), count);
            expect(count <= 8);
            expectEquals(rig.numOffs(), count);
            expect(rig.balanced());

            // Up again, then release.
            rig.r.setPressure(0, 0.9f);
            rig.run(9984, 64);
            expectEquals(rig.numOns(), count + 2);
            rig.r.padUp(0);
            rig.run(64, 64);
            expect(rig.balanced());
            expectEquals(rig.numOns(), rig.numOffs());
            expect(! rig.r.isRolling(0));

            // Never reaching the threshold: one note, released by padUp.
            Rig low;
            low.r.setEnabled(true);
            low.r.setThreshold(0.5f);
            low.r.padDown(3, 50, 1.0f);
            low.r.setPressure(3, 0.44f);
            low.run(96000, 64);
            expectEquals(low.numOns(), 1);
            expectEquals(low.numOffs(), 0);
            low.r.padUp(3);
            low.run(64, 64);
            expectEquals(low.numOffs(), 1);
            expect(low.balanced());

            // Noise around the threshold does not chatter: once rolling, every
            // gap is a full interval.
            Rig noisy;
            noisy.r.setEnabled(true);
            noisy.r.setBaseDivision(PadRatchet::Div1_16);
            noisy.r.setMaxDivision(PadRatchet::Div1_16);
            noisy.r.setThreshold(0.5f);
            noisy.r.padDown(0, 60, 1.0f);
            for (int i = 0; i < 1500; ++i)
            {
                noisy.r.setPressure(0, (i & 1) ? 0.47f : 0.56f);
                noisy.run(64, 64);
            }
            auto n = noisy.ons();
            expect(n.size() >= 10);
            for (size_t k = 2; k < n.size(); ++k)
            {
                const auto gap = n[k].time - n[k - 1].time;
                expect(gap >= 6000 - 64 && gap <= 6000 + 64, juce::String(gap));
            }
            noisy.r.allOff();
            noisy.run(64, 64);
            expect(noisy.balanced());

            // Free mode too.
            Rig free;
            free.r.setEnabled(true);
            free.r.setMode(PadRatchet::Mode::Free);
            free.r.setBaseRateHz(8.0f);
            free.r.setMaxRateHz(8.0f);
            free.r.setThreshold(0.5f);
            free.r.padDown(0, 60, 1.0f);
            free.run(24000, 64);
            expectEquals(free.numOns(), 1);
            free.r.setPressure(0, 1.0f);
            free.run(24000, 64);
            expectEquals(free.numOns(), 5);
            free.r.padUp(0);
            free.run(64, 64);
            expect(free.balanced());
        }

        beginTest("Latch keeps the roll going after release");
        {
            // 1/8 .. 1/32; full pressure = 1/32 = 3000 samples, velocity = pressure.
            Rig rig;
            rig.r.setEnabled(true);
            rig.r.setLatch(true);
            rig.r.setBaseDivision(PadRatchet::Div1_8);
            rig.r.setMaxDivision(PadRatchet::Div1_32);
            rig.r.setPressureToVelocity(1.0f);
            rig.r.setVelocityDecay(0.05f);
            rig.r.padDown(4, 62, 0.5f);
            rig.r.setPressure(4, 1.0f);
            rig.run(19968, 64);
            const int held = rig.numOns();
            expectEquals(held, 7);                    // 0, 3000 .. 18000
            const float lastVelocity = rig.ons().back().velocity;

            rig.r.padUp(4);                           // also zeroes the pad's pressure
            rig.run(30016, 64);
            expect(rig.r.isRolling(4));
            expect(rig.r.isLatched(4));
            auto ons = rig.ons();
            expectEquals((int) ons.size(), 17);       // .. 48000: the same rate
            for (size_t k = (size_t) held; k < ons.size(); ++k)
            {
                expect(ons[k].time == ((juce::int64) k * 3000 / 64) * 64, juce::String(ons[k].time));
                expectWithinAbsoluteError(ons[k].velocity, lastVelocity, 1.0e-6f);   // and velocity
                expectEquals(ons[k].note, 62);
            }

            // Pressure sent while latched is ignored.
            rig.r.setPressure(4, 0.0f);
            rig.run(6016, 64);
            expectEquals(rig.numOns(), 19);

            // Pressing the pad again stops the roll and plays nothing.
            rig.r.padDown(4, 62, 1.0f);
            rig.run(64, 64);
            expect(! rig.r.isRolling(4));
            expect(! rig.r.isLatched(4));
            expect(rig.balanced());
            expectEquals(rig.numOns(), 19);
            rig.r.padUp(4);
            rig.run(48000, 64);
            expectEquals(rig.numOns(), 19);
            expectEquals(rig.numOffs(), 19);

            // The next press rolls again.
            rig.r.padDown(4, 62, 1.0f);
            rig.run(64, 64);
            expectEquals(rig.numOns(), 20);
            expect(rig.r.isRolling(4));
            expect(! rig.r.isLatched(4));
            rig.r.padUp(4);
            rig.run(24000, 64);
            expect(rig.r.isLatched(4));
            expect(rig.numOns() > 21);

            // allOff ends latched rolls, other pads included.
            rig.r.padDown(9, 70, 1.0f);
            rig.r.padUp(9);
            rig.run(24000, 64);
            expect(rig.r.isLatched(9));
            rig.r.allOff();
            rig.run(64, 64);
            expect(rig.balanced());
            int count = rig.numOns();
            rig.run(48000, 64);
            expectEquals(rig.numOns(), count);
            expect(! rig.r.isLatched(4) && ! rig.r.isLatched(9));
            expect(! rig.r.isRolling(4) && ! rig.r.isRolling(9));

            // Switching latch off ends latched rolls; so does disabling.
            for (int how = 0; how < 2; ++how)
            {
                Rig off;
                off.r.setEnabled(true);
                off.r.setLatch(true);
                off.r.setGate(how == 0 ? 0.5f : 1.0f);
                off.r.padDown(1, 40, 1.0f);
                off.run(10000, 64);
                off.r.padUp(1);
                off.run(30000, 64);
                expect(off.r.isLatched(1));
                if (how == 0) off.r.setLatch(false); else off.r.setEnabled(false);
                off.run(64, 64);
                expect(off.balanced());
                expect(! off.r.isLatched(1));
                count = off.numOns();
                off.run(48000, 64);
                expectEquals(off.numOns(), count);
            }

            // A pad held below the threshold is not repeating, so it does not latch.
            Rig quiet;
            quiet.r.setEnabled(true);
            quiet.r.setLatch(true);
            quiet.r.setThreshold(0.5f);
            quiet.r.padDown(0, 60, 1.0f);
            quiet.r.setPressure(0, 0.1f);
            quiet.run(20000, 64);
            quiet.r.padUp(0);
            quiet.run(48000, 64);
            expectEquals(quiet.numOns(), 1);
            expectEquals(quiet.numOffs(), 1);
            expect(! quiet.r.isLatched(0));

            // Above the threshold it latches although release drops the pressure.
            quiet.r.padDown(0, 60, 1.0f);
            quiet.r.setPressure(0, 0.9f);
            quiet.run(20000, 64);
            quiet.r.padUp(0);
            quiet.run(48000, 64);
            expect(quiet.r.isLatched(0));
            expect(quiet.numOns() > 8);
            quiet.r.allOff();
            quiet.run(64, 64);
            expect(quiet.balanced());

            // Free mode latches at the rate it had.
            Rig free;
            free.r.setEnabled(true);
            free.r.setLatch(true);
            free.r.setMode(PadRatchet::Mode::Free);
            free.r.setBaseRateHz(2.0f);
            free.r.setMaxRateHz(32.0f);
            free.r.padDown(0, 60, 1.0f);
            free.r.setPressure(0, 1.0f);
            free.run(12032, 64);
            free.r.padUp(0);
            free.run(48000, 64);
            auto f = free.ons();
            expect(f.size() > 35);
            for (size_t k = 9; k < f.size(); ++k)
            {
                const auto gap = f[k].time - f[k - 1].time;
                expect(gap >= 1500 - 64 && gap <= 1500 + 64, juce::String(gap));
            }
            free.r.padDown(0, 60, 1.0f);
            free.run(64, 64);
            expect(free.balanced());
        }

        beginTest("Humanize: bounded, ordered, repeatable");
        {
            auto roll = [](Rig& rig, float amount, uint32_t seed, float gate, float swing)
            {
                rig.r.setEnabled(true);
                rig.r.setBaseDivision(PadRatchet::Div1_16);
                rig.r.setMaxDivision(PadRatchet::Div1_16);
                rig.r.setPressureToVelocity(0.0f);
                rig.r.setHumanize(amount);
                rig.r.setHumanizeSeed(seed);
                rig.r.setGate(gate);
                rig.r.setSwing(swing);
                rig.r.padDown(0, 60, 0.6f);
                rig.run(6000 * 60 + 3000, 1);
                rig.r.padUp(0);
                rig.run(8, 1);
            };
            auto rollOns = [&roll](float amount, uint32_t seed, float gate, float swing)
            {
                Rig rig;
                roll(rig, amount, seed, gate, swing);
                return rig.ons();
            };

            for (float amount : { 0.3f, 1.0f })
            {
                Rig rig;
                roll(rig, amount, 1234u, 0.9f, 0.0f);
                auto ons = rig.ons();
                expect(ons.size() >= 60 && ons.size() <= 61);
                expect(ons[0].time == 0);
                expectWithinAbsoluteError(ons[0].velocity, 0.6f, 1.0e-3f);   // the press is untouched

                const double maxShift = (double) (amount * PadRatchet::kHumanizeTime) * 6000.0 + 1.0;
                const float maxVel = 0.6f * (1.0f + amount * PadRatchet::kHumanizeVelocity) + 1.0e-4f;
                const float minVel = 0.6f * (1.0f - amount * PadRatchet::kHumanizeVelocity) - 1.0e-4f;
                double biggest = 0.0;
                float vLow = 1.0f, vHigh = 0.0f;
                for (size_t k = 1; k < ons.size(); ++k)
                {
                    const double shift = (double) ons[k].time - (double) k * 6000.0;   // no drift
                    expect(std::abs(shift) <= maxShift, "hit " + juce::String((int) k) + " shift " + juce::String(shift));
                    biggest = std::max(biggest, std::abs(shift));
                    expect(ons[k].time > ons[k - 1].time);
                    expect(ons[k].velocity <= maxVel && ons[k].velocity >= minVel, juce::String(ons[k].velocity));
                    vLow = std::min(vLow, ons[k].velocity);
                    vHigh = std::max(vHigh, ons[k].velocity);
                }
                expect(biggest > maxShift * 0.5, juce::String(biggest));
                expect(vHigh - vLow > 0.6f * amount * PadRatchet::kHumanizeVelocity, juce::String(vHigh - vLow));

                // every note-off lies between its note-on and the next note-on
                expect(rig.balanced());
                expectEquals(rig.numOns(), rig.numOffs());
                for (size_t i = 0; i < rig.events.size(); ++i)
                    expect(rig.events[i].on == ((i & 1) == 0));
                auto offs = rig.offs();
                for (size_t k = 0; k + 1 < ons.size(); ++k)
                    expect(offs[k].time > ons[k].time && offs[k].time <= ons[k + 1].time);
            }

            // Same seed, same roll; another seed, another roll.
            auto a = rollOns(1.0f, 77u, 0.5f, 0.0f);
            auto b = rollOns(1.0f, 77u, 0.5f, 0.0f);
            auto c = rollOns(1.0f, 78u, 0.5f, 0.0f);
            expect(a.size() == b.size());
            bool same = a.size() == b.size(), differs = a.size() != c.size();
            for (size_t k = 0; k < a.size() && k < b.size(); ++k)
                same = same && a[k].time == b[k].time && a[k].velocity == b[k].velocity;
            for (size_t k = 0; k < a.size() && k < c.size(); ++k)
                differs = differs || a[k].time != c[k].time;
            expect(same);
            expect(differs);

            // With full swing and a long gate the order still holds.
            Rig swung;
            roll(swung, 1.0f, 5u, 0.95f, 1.0f);
            expect(swung.balanced());
            auto so = swung.ons();
            for (size_t k = 1; k < so.size(); ++k)
                expect(so[k].time > so[k - 1].time);
            for (size_t i = 0; i < swung.events.size(); ++i)
                expect(swung.events[i].on == ((i & 1) == 0));

            // Free mode with moving pressure, and humanize 0 after a seed: exact grid.
            Rig free;
            free.r.setEnabled(true);
            free.r.setMode(PadRatchet::Mode::Free);
            free.r.setHumanize(1.0f);
            free.r.padDown(0, 60, 1.0f);
            for (int i = 0; i < 1500; ++i)
            {
                free.r.setPressure(0, 0.5f + 0.5f * std::sin((float) i * 0.05f));
                free.run(64, 64);
            }
            free.r.padUp(0);
            free.run(64, 64);
            expect(free.numOns() > 20);
            expect(free.balanced());

            auto straight = rollOns(0.0f, 99u, 0.5f, 0.0f);
            for (size_t k = 0; k < straight.size(); ++k)
            {
                expect(straight[k].time == (juce::int64) k * 6000);
                expectWithinAbsoluteError(straight[k].velocity, 0.6f, 1.0e-3f);
            }
        }

        beginTest("Accent: every Nth hit at full velocity, the rest scaled");
        {
            auto velocities = [](int accent, float threshold)
            {
                Rig rig;
                rig.r.setEnabled(true);
                rig.r.setBaseDivision(PadRatchet::Div1_16);
                rig.r.setMaxDivision(PadRatchet::Div1_16);
                rig.r.setPressureToVelocity(0.0f);
                rig.r.setAccent(accent);
                rig.r.setThreshold(threshold);
                rig.r.padDown(0, 60, 0.8f);
                rig.run(12000, 64);
                rig.r.setPressure(0, 1.0f);
                rig.run(60000, 64);
                rig.r.padUp(0);
                rig.run(64, 64);
                std::vector<float> v;
                for (auto& e : rig.ons())
                    v.push_back(e.velocity);
                if (! rig.balanced())
                    v.clear();
                return v;
            };

            const float soft = 0.8f * PadRatchet::kAccentOffScale;
            for (int n : { 2, 3, 4, 8 })
            {
                auto v = velocities(n, 0.0f);
                expectEquals((int) v.size(), 12);   // 0 .. 66000
                for (size_t k = 0; k < v.size(); ++k)
                    expectWithinAbsoluteError(v[k], (k % (size_t) n) == 0 ? 0.8f : soft, 1.0e-3f);
            }

            auto none = velocities(0, 0.0f);
            for (float v : none)
                expectWithinAbsoluteError(v, 0.8f, 1.0e-3f);

            // With a threshold the count restarts at the hit that starts the roll.
            auto v = velocities(4, 0.5f);
            expect(v.size() >= 10);
            if (v.size() >= 10)
            {
                expectWithinAbsoluteError(v[0], 0.8f, 1.0e-3f);   // the held press
                for (size_t k = 1; k < v.size(); ++k)
                    expectWithinAbsoluteError(v[k], ((k - 1) % 4) == 0 ? 0.8f : soft, 1.0e-3f);
            }
        }

        beginTest("Probability skips repeats but keeps their place");
        {
            PadRatchet d;
            expectEquals(d.getProbability(), 1.0f);
            d.setProbability(3.0f);   expectEquals(d.getProbability(), 1.0f);
            d.setProbability(-1.0f);  expectEquals(d.getProbability(), 0.0f);

            auto roll = [](Rig& rig, float prob, float gate, float swing, int accent)
            {
                rig.r.setEnabled(true);
                rig.r.setBaseDivision(PadRatchet::Div1_16);
                rig.r.setMaxDivision(PadRatchet::Div1_16);
                rig.r.setPressureToVelocity(0.0f);
                rig.r.setProbability(prob);
                rig.r.setHumanizeSeed(321u);
                rig.r.setGate(gate);
                rig.r.setSwing(swing);
                rig.r.setAccent(accent);
                rig.r.padDown(0, 60, 0.8f);
                rig.run(6000 * 100 + 100, 1);
                rig.r.padUp(0);
                rig.run(8, 1);
            };

            // 0: only the press, ended by its gate.
            {
                Rig rig;
                roll(rig, 0.0f, 0.5f, 0.0f, 0);
                expectEquals(rig.numOns(), 1);
                expectEquals(rig.numOffs(), 1);
                expect(rig.offs()[0].time == 3000);
                expect(rig.balanced());
            }
            // 1: every hit.
            {
                Rig rig;
                roll(rig, 1.0f, 0.5f, 0.0f, 0);
                expectEquals(rig.numOns(), 101);
                expect(rig.balanced());
            }
            // 0.5: about half, each on its own grid slot, accents by slot.
            {
                Rig rig;
                roll(rig, 0.5f, 0.5f, 0.0f, 4);
                auto ons = rig.ons();
                expect(ons.size() > 30 && ons.size() < 72, juce::String((int) ons.size()));
                expect(ons[0].time == 0);
                bool sawSoft = false, sawAccent = false;
                for (size_t k = 1; k < ons.size(); ++k)
                {
                    expect(ons[k].time % 6000 == 0, juce::String(ons[k].time));
                    const bool accented = (ons[k].time / 6000) % 4 == 0;
                    expectWithinAbsoluteError(ons[k].velocity, accented ? 0.8f : 0.8f * PadRatchet::kAccentOffScale, 1.0e-3f);
                    (accented ? sawAccent : sawSoft) = true;
                }
                expect(sawSoft && sawAccent);
                expect(rig.balanced());
                expectEquals(rig.numOns(), rig.numOffs());
            }
            // Swing parity follows the slot, not the count of sounded hits.
            {
                Rig rig;
                roll(rig, 0.5f, 0.5f, 1.0f, 0);
                auto ons = rig.ons();
                expect(ons.size() > 30 && ons.size() < 72);
                int odd = 0;
                for (auto& e : ons)
                {
                    const juce::int64 slot = e.time / 6000;
                    expect(e.time == slot * 6000 + ((slot & 1) ? 3000 : 0), juce::String(e.time));
                    odd += (int) (slot & 1);
                }
                expect(odd > 5);
                expect(rig.balanced());
            }
            // Full gate: a note before a skipped hit ends at that hit's time.
            {
                Rig rig;
                roll(rig, 0.5f, 1.0f, 0.0f, 0);
                auto ons = rig.ons();
                auto offs = rig.offs();
                expect(rig.balanced());
                expect(ons.size() > 30 && ons.size() < 72);
                int gaps = 0;
                for (size_t k = 0; k + 1 < ons.size(); ++k)
                {
                    expect(offs[k].time == ons[k].time + 6000, juce::String(offs[k].time));
                    if (ons[k + 1].time > offs[k].time)
                        ++gaps;
                }
                expect(gaps > 5);
            }
        }

        beginTest("Max repeats ends the roll after N hits");
        {
            PadRatchet d;
            expectEquals(d.getMaxRepeats(), 0);
            d.setMaxRepeats(-2);  expectEquals(d.getMaxRepeats(), 0);
            d.setMaxRepeats(99);  expectEquals(d.getMaxRepeats(), PadRatchet::kMaxRepeatLimit);
            d.setMaxRepeats(5);   expectEquals(d.getMaxRepeats(), 5);

            auto setup = [](Rig& rig, int limit, float gate)
            {
                rig.r.setEnabled(true);
                rig.r.setBaseDivision(PadRatchet::Div1_16);
                rig.r.setMaxDivision(PadRatchet::Div1_16);
                rig.r.setMaxRepeats(limit);
                rig.r.setGate(gate);
            };

            for (int limit : { 1, 4, 16 })
                for (float gate : { 0.5f, 1.0f })
                {
                    Rig rig;
                    setup(rig, limit, gate);
                    rig.r.padDown(0, 60, 1.0f);
                    rig.run(64, 1);
                    expect(rig.r.isRolling(0) == (limit > 1));
                    rig.run(6000 * 20, 1);              // still held
                    auto ons = rig.ons();
                    expectEquals((int) ons.size(), limit);
                    for (size_t k = 0; k < ons.size(); ++k)
                        expect(ons[k].time == (juce::int64) k * 6000);
                    expectEquals(rig.numOffs(), limit);  // the last note ended by itself
                    expect(rig.offs().back().time == (juce::int64) (limit - 1) * 6000 + (gate < 1.0f ? 3000 : 6000));
                    expect(! rig.r.isRolling(0));
                    expect(rig.balanced());

                    // A new press starts a fresh count.
                    rig.r.padUp(0);
                    rig.r.padDown(0, 60, 1.0f);
                    rig.run(6000 * 20, 1);
                    expectEquals(rig.numOns(), limit * 2);
                    rig.r.padUp(0);
                    rig.run(8, 1);
                    expect(rig.balanced());
                    expectEquals(rig.numOns(), rig.numOffs());
                }

            // Released early: fewer hits.
            {
                Rig rig;
                setup(rig, 8, 0.5f);
                rig.r.padDown(0, 60, 1.0f);
                rig.run(13000, 64);
                rig.r.padUp(0);
                rig.run(60000, 64);
                expectEquals(rig.numOns(), 3);
                expect(rig.balanced());
            }

            // Latched: the roll ends itself after N hits and the latch clears.
            for (float gate : { 0.5f, 1.0f })
            {
                Rig rig;
                setup(rig, 8, gate);
                rig.r.setLatch(true);
                rig.r.padDown(2, 50, 1.0f);
                rig.run(10048, 64);                     // hits at 0 and 6000
                rig.r.padUp(2);
                rig.run(64, 64);
                expect(rig.r.isLatched(2));
                rig.run(6000 * 12, 64);
                expectEquals(rig.numOns(), 8);
                expectEquals(rig.numOffs(), 8);
                expect(! rig.r.isLatched(2));
                expect(! rig.r.isRolling(2));
                expect(rig.balanced());

                // so the next press plays again instead of "stopping" a latch
                rig.r.padDown(2, 50, 1.0f);
                rig.run(64, 64);
                expectEquals(rig.numOns(), 9);
                rig.r.padUp(2);
                rig.run(6000 * 12, 64);
                expectEquals(rig.numOns(), 16);
                expect(! rig.r.isLatched(2));
                expect(rig.balanced());
            }

            // Threshold: a new crossing starts a fresh count; staying above does not.
            {
                Rig rig;
                setup(rig, 3, 0.5f);
                rig.r.setThreshold(0.5f);
                rig.r.padDown(0, 60, 1.0f);
                rig.r.setPressure(0, 0.8f);
                rig.run(48000, 64);
                expectEquals(rig.numOns(), 3);
                expectEquals(rig.numOffs(), 3);
                rig.r.setPressure(0, 0.2f);
                rig.run(4800, 64);
                expectEquals(rig.numOns(), 3);
                rig.r.setPressure(0, 0.8f);
                rig.run(48000, 64);
                expectEquals(rig.numOns(), 6);
                expectEquals(rig.numOffs(), 6);
                rig.r.padUp(0);
                rig.run(64, 64);
                expect(rig.balanced());

                // held below the threshold first: the press, then N hits from the crossing
                Rig low;
                setup(low, 3, 0.5f);
                low.r.setThreshold(0.5f);
                low.r.padDown(0, 60, 1.0f);
                low.run(12000, 64);
                low.r.setPressure(0, 0.9f);
                low.run(48000, 64);
                expectEquals(low.numOns(), 4);
                low.r.padUp(0);
                low.run(64, 64);
                expect(low.balanced());
            }

            // Skipped hits count towards the limit (the burst keeps its length).
            {
                Rig rig;
                setup(rig, 4, 0.5f);
                rig.r.setProbability(0.0f);
                rig.r.setLatch(true);
                rig.r.padDown(0, 60, 1.0f);
                rig.r.padUp(0);
                rig.run(12000, 64);
                expect(rig.r.isLatched(0));
                rig.run(18000, 64);
                expectEquals(rig.numOns(), 1);
                expectEquals(rig.numOffs(), 1);
                expect(! rig.r.isLatched(0));
                expect(! rig.r.isRolling(0));
            }

            // Free mode.
            {
                Rig rig;
                setup(rig, 5, 0.5f);
                rig.r.setMode(PadRatchet::Mode::Free);
                rig.r.setBaseRateHz(8.0f);
                rig.r.setMaxRateHz(8.0f);
                rig.r.padDown(0, 60, 1.0f);
                rig.run(96000, 64);
                expectEquals(rig.numOns(), 5);
                expectEquals(rig.numOffs(), 5);
                rig.r.padUp(0);
                rig.run(64, 64);
                expect(rig.balanced());
            }
        }

        beginTest("Roll extras together stay balanced");
        {
            for (auto mode : { PadRatchet::Mode::Quantized, PadRatchet::Mode::Free })
                for (float gate : { 0.05f, 0.6f, 1.0f })
                    for (int block : { 1, 64, 512 })
                    {
                        Rig rig;
                        rig.r.setEnabled(true);
                        rig.r.setMode(mode);
                        rig.r.setGate(gate);
                        rig.r.setSwing(0.8f);
                        rig.r.setGlide(0.3f);
                        rig.r.setThreshold(0.3f);
                        rig.r.setLatch(true);
                        rig.r.setHumanize(1.0f);
                        rig.r.setAccent(3);
                        rig.r.setHumanizeSeed(42u);
                        rig.r.setProbability(block == 64 ? 1.0f : 0.7f);
                        rig.r.setMaxRepeats(block == 512 ? 6 : 0);
                        const int blocks = 96000 / block;
                        const int every = juce::jmax(1, 2048 / block);
                        for (int i = 0; i < blocks; ++i)
                        {
                            if (i % every == 0)
                            {
                                const int k = i / every;
                                for (int p : { 0, 5, 15 })
                                    rig.r.setPressure(p, 0.5f + 0.5f * std::sin((float) (k + p * 7) * 0.37f));
                                if (k % 9 == 0)  rig.r.padDown(0, 60, 0.9f);
                                if (k % 9 == 5)  rig.r.padUp(0);
                                if (k % 7 == 1)  rig.r.padDown(5, 65, 0.4f);
                                if (k % 7 == 3)  rig.r.padUp(5);
                                if (k % 11 == 2) rig.r.padDown(15, 72, 1.0f);
                                if (k % 11 == 3) rig.r.padUp(15);
                            }
                            rig.run(block, block);
                        }
                        expect(rig.numOns() > 20);
                        for (auto& e : rig.ons())
                            expect(e.velocity >= PadRatchet::kMinVelocity && e.velocity <= 1.0f);
                        rig.r.allOff();
                        rig.run(block, block);
                        expect(rig.balanced(), "block " + juce::String(block) + " gate " + juce::String(gate));
                        expectEquals(rig.numOns(), rig.numOffs());
                        const int count = rig.numOns();
                        rig.run(24000, juce::jmax(block, 64));
                        expectEquals(rig.numOns(), count);
                    }
        }
    }
};

static PadRatchetTests padRatchetTests;
