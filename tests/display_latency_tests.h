#pragma once

#include "../src/ui/ScreenManager.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include "../src/ui/screens/FilterViewScreen.h"
#include "../src/ui/FilterCurve.h"
#include "../src/ui/EnvelopeCurve.h"
#include "../src/controller/DisplayRenderer.h"
#include "../src/controller/LaunchkeyController.h"
#include <juce_core/juce_core.h>
#include <chrono>
#include <cmath>

// The display path from a knob step to the frame leaving the Mac: what one redraw costs with the
// filter or envelope picture up (counted as work, so the checks hold in a Debug build), that the
// curve is measured once per change and not per refresh, and that the newest finished picture is
// what goes out on the device's ready reply. NEPTUNE_DISPLAY_TIMING=1 prints wall times as well
// (meaningful in a Release build only).
namespace DisplayLatency
{
    // FilterCurve::measureDb as it was before the columns were analysed side by side: one Goertzel
    // pass over the response per column, the fade worked out per sample. Kept here as the yardstick
    // for "the picture did not change" and for the before / after times.
    inline std::vector<float> referenceMeasureDb(const FilterCurve::Settings& s, int columns)
    {
        columns = std::clamp(columns, 1, FilterCurve::MAX_COLUMNS);
        std::vector<float> db((size_t) columns, 0.0f);
        static constexpr double SAMPLE_RATE = 44100.0;
        static constexpr int LENGTH = 8192;
        static constexpr float IMPULSE = 0.05f;

        FilterSlot slot;
        slot.setSampleRate(SAMPLE_RATE);
        for (int i = 0; i < 2; ++i)
        {
            slot.setModel(i, s.model[i]);
            slot.setMode(i, s.mode[i]);
            slot.setCutoff(i, std::clamp(s.cutoffHz[i], FilterCurve::MIN_HZ, FilterCurve::MAX_HZ));
            slot.setResonance(i, std::clamp(s.resonance[i], 0.0f, 1.0f));
            slot.setDrive(i, 0.0f);
        }
        slot.setMorph(std::clamp(s.morph, 0.0f, 1.0f));
        slot.setRouting(s.routing);
        slot.setBalance(s.balance);
        slot.reset();

        std::vector<float> h((size_t) LENGTH);
        for (int n = 0; n < LENGTH; ++n)
        {
            const float out = slot.process(n == 0 ? IMPULSE : 0.0f);
            const float fade = 0.5f * (1.0f + std::cos(3.14159265f * (float) n / (float) LENGTH));
            h[(size_t) n] = (std::isfinite(out) ? out : 0.0f) * fade;
        }
        int used = LENGTH;
        while (used > 64 && std::abs(h[(size_t) (used - 1)]) < 1.0e-9f)
            --used;

        for (int c = 0; c < columns; ++c)
        {
            const double w = 2.0 * 3.14159265358979 * (double) FilterCurve::columnFrequency(c, columns) / SAMPLE_RATE;
            const double coeff = 2.0 * std::cos(w);
            double s1 = 0.0, s2 = 0.0;
            for (int n = 0; n < used; ++n)
            {
                const double s0 = (double) h[(size_t) n] + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
            const double mag = std::sqrt(std::max(0.0, power)) / (double) IMPULSE;
            db[(size_t) c] = (float) (20.0 * std::log10(std::max(mag, 1.0e-6)));
        }
        return db;
    }

    using Frame = std::array<juce::uint8, DisplayRenderer::SYSEX_DATA_SIZE>;

    inline bool timingWanted()
    {
        return juce::SystemStats::getEnvironmentVariable("NEPTUNE_DISPLAY_TIMING", {}).isNotEmpty();
    }

    // The app's display path with no device: a controller without ports (sends nothing), the
    // renderer, the screen manager, PARAM (screen 0) and FLTR (screen 1) on one track.
    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        LaunchkeyController controller;
        DisplayRenderer display;
        ScreenManager mgr;
        ParamPageScreen* param = nullptr;
        FilterViewScreen* fltr = nullptr;
        std::vector<Frame> sent;          // every frame that went to the "device"

        explicit Rig(EngineType type = EngineType::UnifiedVoice)
        {
            seq.prepareToPlay(44100.0, 512);
            audio.addEngine(0, AudioEngine::createEngine(type));
            seq.getPattern().getTrack(0).engineType = type;
            display.setController(&controller);
            display.onFrameSent = [this](const Frame& f) { sent.push_back(f); };
            mgr.setSequencer(&seq);
            mgr.setDisplay(&display);
            param = new ParamPageScreen(seq, &audio);
            mgr.addScreen(std::unique_ptr<Screen>(param));
            fltr = new FilterViewScreen(seq, &audio);
            mgr.addScreen(std::unique_ptr<Screen>(fltr));
            mgr.setActiveScreen(0);
        }

        // A knob message as the controller delivers it (internal CC 0x15-0x1C, two's-complement steps).
        void turn(int encoder, int clicks = 1) { mgr.controlChangeReceived(16, 0x15 + encoder, clicks < 0 ? 128 + clicks : clicks); }
        // The app's 30 Hz refresh, as far as the display goes.
        void tick() { const juce::ScopedLock ui(mgr.getLock()); display.tickAckTimeout(); mgr.requestRedraw(); mgr.redrawIfNeeded(); }
        void ready() { display.onBitmapAck(); }       // the device's ready reply
        // Everything sent and answered, the device showing the current state.
        void settle() { tick(); ready(); ready(); tick(); ready(); sent.clear(); }

        ParamCellScreen& show(int screen)
        {
            mgr.setActiveScreen(screen);
            if (screen == 0) param->selectPageByName("FILTR");
            return screen == 0 ? static_cast<ParamCellScreen&> (*param) : *fltr;
        }

        FilterCurve::Binding binding() { return FilterCurve::bind(audio.getEngine(0)->getParamPages()); }
        void set(int id, float v) { seq.getPattern().getTrack(0).soundParams[id] = v; audio.setTrackParameter(0, id, v); }
        static long long measurements() { return FilterCurve::work().measurements.load(); }

        static int cellOf(ParamCellScreen& s, const char* label)
        {
            const auto page = s.getCurrentPage();
            for (int i = 0; i < 8; ++i)
                if (page.params[(size_t) i].isValid() && std::strcmp(page.params[(size_t) i].label, label) == 0)
                    return i;
            return -1;
        }
    };

    // ---- Wall times --------------------------------------------------------------------------------
    inline double nowUs()
    {
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    struct Timing { double meanUs = 0.0, maxUs = 0.0; };

    template <typename Fn>
    inline Timing timeIt(int repeats, Fn fn)
    {
        Timing t;
        double total = 0.0;
        for (int i = 0; i < repeats; ++i)
        {
            const double a = nowUs();
            fn(i);
            const double d = nowUs() - a;
            total += d;
            t.maxUs = std::max(t.maxUs, d);
        }
        t.meanUs = total / std::max(1, repeats);
        return t;
    }

    inline void printTimings()
    {
        auto line = [](const juce::String& what, Timing t)
        {
            std::cout << "[timing] " << what.paddedRight(' ', 58) << juce::String(t.meanUs / 1000.0, 3).paddedLeft(' ', 8)
                      << " ms mean" << juce::String(t.maxUs / 1000.0, 3).paddedLeft(' ', 8) << " ms max" << std::endl;
        };

        // The measurement alone, per model and mode: a low cutoff with high resonance rings longest.
        float sink = 0.0f;
        for (int m = 0; m < FilterSlot::NUM_MODELS; ++m)
        {
            const auto model = FilterSlot::modelFromIndex(m);
            for (int mode = 0; mode < FilterSlot::modeCount(model); ++mode)
                for (float cutoff : { 60.0f, 2000.0f })
                {
                    const juce::String name = juce::String(FilterSlot::modelNames()[m]) + " "
                                            + FilterSlot::modeNames(model)[mode] + " " + juce::String(cutoff, 0) + " Hz";
                    line("measure, before: " + name, timeIt(40, [&](int i)
                    {
                        sink += referenceMeasureDb(FilterCurve::single(model, mode, cutoff + (float) i, 0.9f, 0.3f), 128)[5];
                    }));
                    line("measure, now:    " + name, timeIt(40, [&](int i)
                    {
                        sink += FilterCurve::measureDb(FilterCurve::single(model, mode, cutoff + (float) i, 0.9f, 0.3f), 128)[5];
                    }));
                }
        }
        {
            FilterCurve::Settings s = FilterCurve::single(FilterModel::Ladder, 0, 60.0f, 0.9f);
            s.model[1] = FilterModel::Comb; s.cutoffHz[1] = 80.0f; s.resonance[1] = 0.9f; s.routing = FilterRouting::Series;
            line("measure, before: LADR > COMB series 60 Hz", timeIt(40, [&](int i) { s.cutoffHz[0] = 60.0f + (float) i; sink += referenceMeasureDb(s, 128)[5]; }));
            line("measure, now:    LADR > COMB series 60 Hz", timeIt(40, [&](int i) { s.cutoffHz[0] = 60.0f + (float) i; sink += FilterCurve::measureDb(s, 128)[5]; }));
        }

        // Whole knob messages (parameter set + redraw + frame copy), per engine and screen.
        struct E { EngineType type; const char* name; };
        for (auto e : { E { EngineType::UnifiedVoice, "synth" }, E { EngineType::FM, "FM" }, E { EngineType::MacroOsc, "macro" },
                        E { EngineType::Wavetable, "wavetable" }, E { EngineType::WaveSeq, "waveseq" }, E { EngineType::Sampler, "sampler" } })
        {
            for (int screen = 0; screen < 2; ++screen)
            {
                Rig rig(e.type);
                ParamCellScreen& s = rig.show(screen);
                const auto b = rig.binding();
                const juce::String where = juce::String(e.name) + (screen == 0 ? " PARAM" : " FLTR");
                for (int m = 0; m < FilterSlot::NUM_MODELS; ++m)
                {
                    rig.set(b.type.id, (float) m);
                    if (b.reso.isValid()) rig.set(b.reso.id, b.reso.maxVal * 0.9f);
                    const int cut = Rig::cellOf(s, "CUT");
                    if (cut < 0) continue;
                    for (int i = 0; i < 40; ++i) rig.turn(cut, -40);       // down to the bottom
                    line(where + " turn CUT, " + FilterSlot::modelNames()[m], timeIt(60, [&](int) { rig.turn(cut, 1); rig.ready(); }));
                }
                line(where + " 30 Hz tick, picture up, nothing changed", timeIt(200, [&](int) { rig.tick(); rig.ready(); }));
                const int type = Rig::cellOf(s, "TYPE");
                if (type >= 0)
                    line(where + " turn TYPE", timeIt(60, [&](int i) { rig.turn(type, (i / 5) % 2 == 0 ? 1 : -1); rig.ready(); }));
            }

            // Envelope knobs.
            Rig rig(e.type);
            for (int p = 0; p < rig.param->getPageCount(); ++p)
            {
                rig.param->setCurrentPageIndex(p);
                if (!EnvelopeCurve::bind(rig.param->getCurrentPage()).valid)
                    continue;
                for (const char* label : { "ATK", "REL" })
                {
                    const int cell = Rig::cellOf(*rig.param, label);
                    line(juce::String(e.name) + " PARAM " + rig.param->getPageName() + " turn " + label,
                         timeIt(120, [&](int) { rig.turn(cell, 2); rig.ready(); }));      // up to ten seconds
                }
                line(juce::String(e.name) + " PARAM " + rig.param->getPageName() + " 30 Hz tick, envelope up",
                     timeIt(200, [&](int) { rig.tick(); rig.ready(); }));
                break;
            }
            rig.param->expireFilterPicture();
            line(juce::String(e.name) + " PARAM 30 Hz tick, no picture", timeIt(200, [&](int) { rig.tick(); rig.ready(); }));
        }

        EnvelopeCurve::Settings longest;
        longest.attack = longest.decay = longest.release = 10.0f;
        line("EnvelopeCurve::shape, 10 s stages", timeIt(200, [&](int) { sink += EnvelopeCurve::shape(longest).heights[3]; }));
        juce::ignoreUnused(sink);
    }

    // ---- A device that takes 80 ms per frame, in made-up time --------------------------------------
    // A knob turned at `stepsPerSecond` for `seconds`; the app's 30 Hz refresh; the ready reply
    // `deviceMs` after every frame. Drawing takes no time here (it is measured separately).
    struct Simulation
    {
        int steps = 0, frames = 0, timeouts = 0;
        double meanWaitMs = 0.0, worstWaitMs = 0.0;      // from a step to the frame that shows it leaving
        bool everyFrameWasNewest = true;                 // each frame was the picture of every step so far
        bool sentWhileBusy = false;                      // a frame left before the reply to the one before
    };

    inline Simulation simulate(double stepsPerSecond, double seconds, double deviceMs = 80.0)
    {
        Simulation out;
        Rig rig;
        double now = 1000000.0;
        rig.display.clock = [&now] { return (juce::int64) now; };
        rig.mgr.setClock([&now] { return (juce::int64) now; });
        auto& screen = rig.show(1);
        const int cut = Rig::cellOf(screen, "CUT");
        for (int i = 0; i < 40; ++i) rig.turn(cut, -40);
        rig.settle();

        const double stepMs = 1000.0 / stepsPerSecond, tickMs = 1000.0 / 30.0, start = now, end = now + seconds * 1000.0;
        double nextStep = start + 0.37 * stepMs, nextTick = start + 0.11 * tickMs, replyAt = -1.0;
        std::vector<double> waiting;                     // times of the steps no frame has shown yet
        double total = 0.0;
        size_t seen = 0;

        for (;;)
        {
            const bool stepsLeft = nextStep < end;
            if (!stepsLeft && waiting.empty() && replyAt < 0.0)
                break;

            // The next thing to happen: the reply, a step or a refresh.
            double t = nextTick;
            int what = 2;
            if (stepsLeft && nextStep <= t) { t = nextStep; what = 1; }
            if (replyAt >= 0.0 && replyAt <= t) { t = replyAt; what = 0; }
            now = t;

            const bool busy = replyAt >= 0.0 && what != 0;
            if (what == 0)      { replyAt = -1.0; rig.ready(); }
            else if (what == 1) { rig.turn(cut, (out.steps / 60) % 2 == 0 ? 1 : -1); waiting.push_back(now); ++out.steps; nextStep += stepMs; }
            else                { rig.tick(); nextTick += tickMs; }

            if (rig.sent.size() > seen)
            {
                if (rig.sent.size() > seen + 1 || busy)
                    out.sentWhileBusy = true;
                seen = rig.sent.size();
                replyAt = now + deviceMs;
                // The frame that left is the picture of the state as it is now.
                out.everyFrameWasNewest = out.everyFrameWasNewest && rig.sent.back() == rig.display.getSysExData();
            }
            // A step is on its way once the last frame sent is the picture of the current state
            // (at once, when the step moved no pixel).
            if (!rig.sent.empty() && rig.sent.back() == rig.display.getSysExData())
            {
                for (double at : waiting)
                {
                    total += now - at;
                    out.worstWaitMs = std::max(out.worstWaitMs, now - at);
                }
                waiting.clear();
            }
        }
        out.frames = (int) rig.sent.size();
        out.timeouts = rig.display.getTimeoutCount();
        out.meanWaitMs = total / std::max(1, out.steps);
        return out;
    }
}

class DisplayLatencyTests : public juce::UnitTest
{
public:
    DisplayLatencyTests() : UnitTest("Display Latency") {}

    using Rig = DisplayLatency::Rig;

    static bool samePixels(const DisplayRenderer& a, const DisplayRenderer& b)
    {
        for (int y = 0; y < DisplayRenderer::HEIGHT; ++y)
            for (int x = 0; x < DisplayRenderer::WIDTH; ++x)
                if (a.getPixel(x, y) != b.getPixel(x, y))
                    return false;
        return true;
    }

    // The curve drawn at the three sizes the screens use, from the measurement as it was and as it is.
    void expectSamePicture(const FilterCurve::Settings& s, const juce::String& name)
    {
        const auto before = DisplayLatency::referenceMeasureDb(s, 128);
        const auto now = FilterCurve::measureDb(s, 128);
        float worst = 0.0f;
        for (size_t i = 0; i < before.size(); ++i)
            worst = std::max(worst, std::abs(before[i] - now[i]));
        expect(worst <= 1.0e-4f, name + ": the measurement moved by " + juce::String(worst, 6) + " dB");

        auto heights = [](std::vector<float> v) { for (auto& x : v) x = FilterCurve::heightFromDb(x); return v; };
        DisplayRenderer a, b;
        struct Area { int y, h; };
        for (auto area : { Area { ScreenLayout::CONTENT_Y, ScreenLayout::CONTENT_H },
                           Area { ParamCellScreen::PICTURE_TOP_Y, ParamCellScreen::PICTURE_TOP_H },
                           Area { ParamCellScreen::PICTURE_BOTTOM_Y, ParamCellScreen::PICTURE_BOTTOM_H } })
        {
            a.clear(); b.clear();
            FilterCurve::draw(a, 0, area.y, 128, area.h, heights(before), s.cutoffHz[0]);
            FilterCurve::draw(b, 0, area.y, 128, area.h, heights(now), s.cutoffHz[0]);
            expect(samePixels(a, b), name + ": the picture " + juce::String(area.h) + " rows high changed");
        }
    }

    void runTest() override
    {
        beginTest("Filter picture: pixel for pixel what the column-at-a-time measurement drew, every model and mode");
        {
            for (int m = 0; m < FilterSlot::NUM_MODELS; ++m)
            {
                const auto model = FilterSlot::modelFromIndex(m);
                for (int mode = 0; mode < FilterSlot::modeCount(model); ++mode)
                    for (float cutoff : { 30.0f, 400.0f, 3000.0f, 16000.0f })
                        for (float reso : { 0.0f, 0.7f, 1.0f })
                            expectSamePicture(FilterCurve::single(model, mode, cutoff, reso, model == FilterModel::SEM ? 0.5f : 0.0f),
                                              juce::String(FilterSlot::modelNames()[m]) + " " + FilterSlot::modeNames(model)[mode]
                                              + " " + juce::String(cutoff, 0) + " Hz reso " + juce::String(reso, 1));
            }
            // Two filters, as the synth routes them.
            for (auto routing : { FilterRouting::Series, FilterRouting::Parallel })
            {
                FilterCurve::Settings s = FilterCurve::single(FilterModel::Ladder, 0, 500.0f, 0.8f);
                s.model[1] = FilterModel::Comb; s.mode[1] = 1; s.cutoffHz[1] = 90.0f; s.resonance[1] = 0.9f;
                s.routing = routing; s.balance = 0.3f;
                expectSamePicture(s, routing == FilterRouting::Series ? "LADR > COMB" : "LADR + COMB");
                s.model[0] = FilterModel::SEM; s.morph = 0.5f; s.model[1] = FilterModel::Diode; s.mode[1] = 2; s.cutoffHz[1] = 4000.0f;
                expectSamePicture(s, routing == FilterRouting::Series ? "SEM > DIOD" : "SEM + DIOD");
            }
            // Fewer columns than a group, and a count that is not a multiple of it.
            for (int columns : { 1, 5, 100 })
            {
                const auto s = FilterCurve::single(FilterModel::SVF, 0, 800.0f, 0.6f);
                const auto before = DisplayLatency::referenceMeasureDb(s, columns), now = FilterCurve::measureDb(s, columns);
                expectEquals((int) now.size(), columns);
                bool same = before.size() == now.size();
                for (size_t i = 0; same && i < now.size(); ++i) same = std::abs(before[i] - now[i]) <= 1.0e-4f;
                expect(same, juce::String(columns) + " columns");
            }
        }

        beginTest("One measurement runs the real filter over 8192 samples once and analyses each column once");
        {
            auto& work = FilterCurve::work();
            const auto m0 = work.measurements.load(), f0 = work.filterSamples.load(), a0 = work.analysisSteps.load();
            FilterCurve::measureDb(FilterCurve::single(FilterModel::Ladder, 0, 60.0f, 0.95f), 128);
            expectEquals((int) (work.measurements.load() - m0), 1);
            expectEquals((int) (work.filterSamples.load() - f0), 8192);
            expect(work.analysisSteps.load() - a0 <= 8192 * 128, "At most the whole response per column");
            // A response that has died away is not analysed to the end.
            const auto a1 = work.analysisSteps.load();
            FilterCurve::measureDb(FilterCurve::single(FilterModel::Bypass, 0, 1000.0f, 0.0f), 128);
            expectEquals((int) (work.analysisSteps.load() - a1), 64 * 128);
        }

        beginTest("A knob step measures the curve once; the refresh and knobs that leave the curve alone measure nothing");
        {
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                               EngineType::WaveSeq, EngineType::Sampler })
                for (int screen = 0; screen < 2; ++screen)
                {
                    const juce::String name = "Engine " + juce::String((int) type) + (screen == 0 ? " PARAM" : " FLTR");
                    Rig rig(type);
                    auto& s = rig.show(screen);
                    const int cut = Rig::cellOf(s, "CUT");
                    expect(cut >= 0, name + " has CUT");
                    rig.turn(cut, -2);                               // the picture is up and measured
                    expect(s.isFilterPictureActive(), name);

                    auto before = Rig::measurements();
                    rig.turn(cut, -1);
                    expectEquals((int) (Rig::measurements() - before), 1, name + ": one step, one measurement");

                    before = Rig::measurements();
                    rig.turn(cut, -5);                               // five clicks in one message
                    expectEquals((int) (Rig::measurements() - before), 1, name + ": one message, one measurement");

                    before = Rig::measurements();
                    for (int i = 0; i < 30; ++i) { rig.tick(); rig.ready(); }
                    expectEquals((int) (Rig::measurements() - before), 0, name + ": a second of refreshes measures nothing");

                    // A value at its end stop: the turn changes nothing, so nothing is measured.
                    for (int i = 0; i < 40; ++i) rig.turn(cut, -40);
                    before = Rig::measurements();
                    rig.turn(cut, -1);
                    expectEquals((int) (Rig::measurements() - before), 0, name + ": a turn against the stop measures nothing");
                }

            // Envelope knobs draw the envelope from at most a display's width of envelope samples
            // and never measure the filter.
            Rig rig;
            rig.mgr.setActiveScreen(0);
            expect(rig.param->selectPageByName("AMP"));
            const int rel = Rig::cellOf(*rig.param, "REL");
            expect(rel >= 0);
            const auto before = Rig::measurements();
            for (int i = 0; i < 200; ++i) rig.turn(rel, 3);          // up to the longest release
            expect(rig.param->isEnvelopePictureActive());
            for (int i = 0; i < 30; ++i) rig.tick();
            expectEquals((int) (Rig::measurements() - before), 0);
            EnvelopeCurve::Settings longest;
            expect(rig.param->getEnvelopeSettings(longest));
            expect(longest.release >= 5.0f, "The release is long: " + juce::String(longest.release));
            longest.attack = longest.decay = 10.0f;
            expect((int) EnvelopeCurve::shape(longest).heights.size() <= DisplayRenderer::WIDTH,
                   "Ten-second stages are drawn from one envelope sample per column, not from audio-rate samples");
        }

        beginTest("The picture is in the first frame after the turn, with the new value");
        {
            Rig rig;
            auto& s = rig.show(0);
            const int cut = Rig::cellOf(s, "CUT");
            rig.settle();
            rig.turn(cut, -4);
            expectEquals((int) rig.sent.size(), 1, "The frame leaves inside the knob message when the device is free");
            // The same state drawn again is the same picture: nothing was left for a second frame.
            rig.ready();
            rig.tick();
            expectEquals((int) rig.sent.size(), 1, "The refresh has nothing to add");

            DisplayRenderer::PixelBuffer shown;
            rig.display.getLastSentPixels(shown);
            const auto settings = FilterCurve::read(rig.binding(), [&rig](int id) { return rig.audio.getTrackParameter(0, id); });
            DisplayRenderer alone;
            const int y0 = ParamCellScreen::PICTURE_BOTTOM_Y, y1 = y0 + ParamCellScreen::PICTURE_BOTTOM_H;
            FilterCurve::draw(alone, 0, y0, 128, y1 - y0, FilterCurve::response(settings, 128), settings.cutoffHz[0]);
            int lit = 0;
            bool same = true;
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < 128; ++x)
                {
                    lit += shown[(size_t) y][(size_t) x] ? 1 : 0;
                    same = same && shown[(size_t) y][(size_t) x] == alone.getPixel(x, y);
                }
            expect(lit > 100 && same, "The frame sent holds the curve of the value just set");
        }

        beginTest("While the device is busy nothing is sent; on its ready reply the newest picture goes out, never one in between");
        {
            Rig rig;
            auto& s = rig.show(1);
            const int cut = Rig::cellOf(s, "CUT");
            rig.settle();

            rig.turn(cut, -3);
            expectEquals((int) rig.sent.size(), 1);
            const auto first = rig.sent.back();

            std::vector<DisplayLatency::Frame> between;
            for (int i = 0; i < 6; ++i)
            {
                rig.turn(cut, -3);
                if (i % 2 == 0) rig.tick();                          // the refresh comes by as well
                between.push_back(rig.display.getSysExData());
                expectEquals((int) rig.sent.size(), 1, "Nothing leaves before the reply");
            }
            const auto newest = between.back();
            between.pop_back();

            rig.ready();
            expectEquals((int) rig.sent.size(), 2, "One frame on the reply");
            expect(rig.sent.back() == newest && newest != first, "It is the picture of the last step");
            for (auto& f : between)
                expect(f != newest, "The pictures in between were different, and were dropped");

            rig.ready();
            rig.tick();
            expectEquals((int) rig.sent.size(), 2, "Nothing more: the device shows the newest state");
            expect(rig.display.isIdle());
        }

        beginTest("The safety-net resend is for a still screen: it does not push a repeat in front of a changing picture");
        {
            Rig rig;
            juce::int64 now = 100000;
            rig.display.clock = [&now] { return now; };
            auto& s = rig.show(1);
            const int cut = Rig::cellOf(s, "CUT");
            rig.settle();

            // A knob turned for six seconds, a step every 100 ms, the device answering each frame.
            for (int i = 0; i < 60; ++i)
            {
                rig.turn(cut, (i / 20) % 2 == 0 ? -1 : 1);
                now += 80;
                rig.ready();
                rig.tick();
                now += 20;
                rig.tick();
            }
            bool repeat = false;
            for (size_t i = 1; i < rig.sent.size(); ++i)
                repeat = repeat || rig.sent[i] == rig.sent[i - 1];
            expect(!repeat, "No frame was a repeat of the one before it");
            expectEquals((int) rig.sent.size(), 60);

            // Left alone, the picture is sent again five seconds after the last frame.
            const auto last = rig.sent.back();
            rig.ready();
            now += DisplayRenderer::PERIODIC_RESEND_MS - 200;
            rig.tick();
            expectEquals((int) rig.sent.size(), 60, "Not before five seconds");
            now += 400;
            rig.tick();
            expectEquals((int) rig.sent.size(), 61);
            expect(rig.sent.back() == last, "The same picture again");
            expectEquals(rig.display.getTimeoutCount(), 0);
        }

        beginTest("A knob turned against a device that takes 80 ms per frame: every step is on its way within one frame time");
        {
            for (double rate : { 8.0, 40.0, 150.0 })
            {
                const auto sim = DisplayLatency::simulate(rate, 3.0);
                const juce::String name = juce::String(rate, 0) + " steps a second: ";
                expect(sim.steps >= (int) (rate * 3.0) - 1, name + juce::String(sim.steps) + " steps");
                expect(!sim.sentWhileBusy, name + "no frame left before the reply to the one before");
                expect(sim.everyFrameWasNewest, name + "every frame was the picture of the newest state");
                expectEquals(sim.timeouts, 0, name + "no time-out");
                expect(sim.worstWaitMs <= 80.0 + 0.001, name + "worst wait " + juce::String(sim.worstWaitMs, 1) + " ms");
                expect(sim.frames <= (int) (3000.0 / 80.0) + 3, name + juce::String(sim.frames) + " frames");
                if (rate < 12.0)
                    expect(sim.worstWaitMs < 0.001 && sim.frames == sim.steps,
                           name + "slower than the device, every step leaves at once (" + juce::String(sim.worstWaitMs, 2) + " ms)");
                if (DisplayLatency::timingWanted())
                    std::cout << "[timing] simulated turn, " << name << sim.steps << " steps, " << sim.frames
                              << " frames, step to frame leaving: mean " << juce::String(sim.meanWaitMs, 1) << " ms, worst "
                              << juce::String(sim.worstWaitMs, 1) << " ms" << std::endl;
            }
        }

        if (DisplayLatency::timingWanted())
        {
            beginTest("Wall times (NEPTUNE_DISPLAY_TIMING)");
            DisplayLatency::printTimings();
            expect(true);
        }
    }
};

static DisplayLatencyTests displayLatencyTests;
