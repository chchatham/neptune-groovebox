#pragma once

#include "../src/ui/screens/FilterViewScreen.h"
#include "../src/ui/FilterCurve.h"
#include "../src/controller/DisplayRenderer.h"
#include <juce_core/juce_core.h>
#include <cmath>

// The filter picture (FilterCurve: the response measured from the real filter classes) and the
// FLTR screen that shows it for every engine with a filter.
class FilterViewScreenTests : public juce::UnitTest
{
public:
    FilterViewScreenTests() : UnitTest("Filter View Screen") {}

    static constexpr int N = 128;

    static std::vector<float> db(FilterModel model, int mode, float cutoff, float reso, float morph = 0.0f)
    {
        return FilterCurve::measureDb(FilterCurve::single(model, mode, cutoff, reso, morph), N);
    }
    static float at(const std::vector<float>& curve, float hz) { return curve[(size_t) FilterCurve::frequencyColumn(hz, N)]; }

    // The loudest column between two frequencies: { dB, Hz }.
    static std::pair<float, float> peak(const std::vector<float>& curve, float fromHz, float toHz)
    {
        std::pair<float, float> best { -1000.0f, 0.0f };
        for (int c = FilterCurve::frequencyColumn(fromHz, N); c <= FilterCurve::frequencyColumn(toHz, N); ++c)
            if (curve[(size_t) c] > best.first)
                best = { curve[(size_t) c], FilterCurve::columnFrequency(c, N) };
        return best;
    }

    // Peaks that stand at least `prominence` dB above the dips on both sides, as frequencies.
    static std::vector<float> peaks(const std::vector<float>& curve, float fromHz, float toHz, float prominence)
    {
        std::vector<float> found;
        const int a = FilterCurve::frequencyColumn(fromHz, N), b = FilterCurve::frequencyColumn(toHz, N);
        for (int c = a + 1; c < b; ++c)
        {
            if (curve[(size_t) c] < curve[(size_t) (c - 1)] || curve[(size_t) c] <= curve[(size_t) (c + 1)])
                continue;
            float left = curve[(size_t) c], right = curve[(size_t) c];
            for (int i = c - 1; i >= a && curve[(size_t) i] <= curve[(size_t) (i + 1)]; --i) left = curve[(size_t) i];
            for (int i = c + 1; i <= b && curve[(size_t) i] <= curve[(size_t) (i - 1)]; ++i) right = curve[(size_t) i];
            if (curve[(size_t) c] - left >= prominence && curve[(size_t) c] - right >= prominence)
                found.push_back(FilterCurve::columnFrequency(c, N));
        }
        return found;
    }

    static int litPixels(const DisplayRenderer& display, int x0, int y0, int w, int h)
    {
        int n = 0;
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x)
                if (display.getPixel(x, y)) ++n;
        return n;
    }

    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        FilterViewScreen screen;
        explicit Rig(EngineType type) : screen(seq, &audio)
        {
            audio.addEngine(0, AudioEngine::createEngine(type));
            seq.getPattern().getTrack(0).engineType = type;
        }
        FilterCurve::Binding binding() { return FilterCurve::bind(audio.getEngine(0)->getParamPages()); }
        float value(int id) { return audio.getTrackParameter(0, id); }
    };

    void runTest() override
    {
        // ------------------------------------------------------------------ the curve helper
        beginTest("Curve: every model and mode gives N finite heights between 0 and 1");
        {
            for (int m = 0; m < FilterSlot::NUM_MODELS; ++m)
            {
                const auto model = FilterSlot::modelFromIndex(m);
                for (int mode = 0; mode < FilterSlot::modeCount(model); ++mode)
                    for (float reso : { 0.0f, 0.5f, 1.0f })
                    {
                        auto h = FilterCurve::response(model, mode, 1000.0f, reso, 0.0f, N);
                        expectEquals((int) h.size(), N);
                        bool ok = true;
                        for (float v : h) ok = ok && std::isfinite(v) && v >= 0.0f && v <= 1.0f;
                        expect(ok, juce::String(FilterSlot::modelNames()[m]) + " mode " + juce::String(mode));
                    }
            }
            expectWithinAbsoluteError(FilterCurve::columnFrequency(0, N), 20.0f, 0.01f);
            expectWithinAbsoluteError(FilterCurve::columnFrequency(N - 1, N), 20000.0f, 1.0f);
            expectEquals(FilterCurve::frequencyColumn(20000.0f, N), N - 1);
        }

        beginTest("Curve: bypass is flat at 0 dB");
        {
            auto c = db(FilterModel::Bypass, 0, 1000.0f, 0.5f);
            for (float hz : { 30.0f, 300.0f, 3000.0f, 15000.0f })
                expectWithinAbsoluteError(at(c, hz), 0.0f, 0.5f);
            expectWithinAbsoluteError(FilterCurve::heightFromDb(0.0f), 48.0f / 72.0f, 0.001f);
        }

        beginTest("Curve: low-pass modes pass below the cutoff and fall above it");
        {
            struct Case { FilterModel model; int mode; float morph; const char* name; };
            const Case cases[] = { { FilterModel::SVF, 0, 0, "SVF LP" }, { FilterModel::SEM, 0, 0, "SEM LP" },
                                   { FilterModel::Ladder, 0, 0, "LADR LP24" }, { FilterModel::Ladder, 2, 0, "LADR LP12" },
                                   { FilterModel::Diode, 0, 0, "DIOD LP" } };
            for (auto& k : cases)
            {
                auto c = db(k.model, k.mode, 1000.0f, 0.1f, k.morph);
                expect(at(c, 100.0f) > -6.0f, juce::String(k.name) + " passband " + juce::String(at(c, 100.0f)));
                expect(at(c, 8000.0f) < at(c, 100.0f) - 18.0f,
                       juce::String(k.name) + " falls above the cutoff: " + juce::String(at(c, 8000.0f)));
            }
            // The ladder's slopes are in order.
            const float lp24 = at(db(FilterModel::Ladder, 0, 1000.0f, 0.0f), 8000.0f);
            const float lp12 = at(db(FilterModel::Ladder, 2, 1000.0f, 0.0f), 8000.0f);
            const float lp6  = at(db(FilterModel::Ladder, 3, 1000.0f, 0.0f), 8000.0f);
            expect(lp24 < lp12 - 10.0f && lp12 < lp6 - 6.0f,
                   juce::String(lp24) + " / " + juce::String(lp12) + " / " + juce::String(lp6));
        }

        beginTest("Curve: high-pass modes fall below the cutoff");
        {
            struct Case { FilterModel model; int mode; float morph; const char* name; };
            const Case cases[] = { { FilterModel::SVF, 2, 0, "SVF HP" }, { FilterModel::SEM, 0, 1, "SEM morph HP" },
                                   { FilterModel::Ladder, 5, 0, "LADR HP" }, { FilterModel::Diode, 1, 0, "DIOD HP" } };
            for (auto& k : cases)
            {
                auto c = db(k.model, k.mode, 1000.0f, 0.1f, k.morph);
                expect(at(c, 100.0f) < at(c, 8000.0f) - 12.0f,
                       juce::String(k.name) + ": " + juce::String(at(c, 100.0f)) + " vs " + juce::String(at(c, 8000.0f)));
            }
        }

        beginTest("Curve: band-pass modes fall on both sides; the notch dips at the cutoff");
        {
            struct Case { FilterModel model; int mode; const char* name; };
            const Case cases[] = { { FilterModel::SVF, 1, "SVF BP" }, { FilterModel::SEM, 1, "SEM BP" },
                                   { FilterModel::Ladder, 4, "LADR BP" }, { FilterModel::Diode, 2, "DIOD BP" } };
            for (auto& k : cases)
            {
                auto c = db(k.model, k.mode, 1000.0f, 0.3f);
                const float centre = peak(c, 500.0f, 2000.0f).first;
                expect(at(c, 40.0f) < centre - 10.0f && at(c, 18000.0f) < centre - 10.0f,
                       juce::String(k.name) + ": " + juce::String(at(c, 40.0f)) + " / " + juce::String(centre) + " / " + juce::String(at(c, 18000.0f)));
            }
            auto notch = db(FilterModel::SVF, 3, 1000.0f, 0.3f);
            expect(at(notch, 1000.0f) < at(notch, 100.0f) - 10.0f && at(notch, 1000.0f) < at(notch, 10000.0f) - 10.0f,
                   "Notch: " + juce::String(at(notch, 1000.0f)));
            // SEM morph at half way is its notch.
            auto semNotch = db(FilterModel::SEM, 0, 1000.0f, 0.3f, 0.5f);
            expect(at(semNotch, 1000.0f) < at(semNotch, 100.0f) - 6.0f, "SEM notch: " + juce::String(at(semNotch, 1000.0f)));
        }

        beginTest("Curve: the peak sits at the cutoff and grows with resonance, on every resonant model");
        {
            for (auto model : { FilterModel::SVF, FilterModel::SEM, FilterModel::Ladder, FilterModel::Diode })
            {
                const juce::String name = FilterSlot::modelNames()[FilterSlot::indexFromModel(model)];
                for (float cutoff : { 300.0f, 2000.0f })
                {
                    const auto low = peak(db(model, 0, cutoff, 0.1f), cutoff / 1.6f, cutoff * 1.6f);
                    const auto high = peak(db(model, 0, cutoff, 0.85f), cutoff / 1.6f, cutoff * 1.6f);
                    expect(high.first > low.first + 3.0f,
                           name + " at " + juce::String(cutoff) + ": " + juce::String(low.first) + " -> " + juce::String(high.first));
                    expect(high.second > cutoff / 1.25f && high.second < cutoff * 1.25f,
                           name + " peak at " + juce::String(high.second) + " for cutoff " + juce::String(cutoff));
                }
            }
            // The cutoff moves the curve.
            auto lowCut = db(FilterModel::SVF, 0, 200.0f, 0.2f), highCut = db(FilterModel::SVF, 0, 5000.0f, 0.2f);
            expect(at(lowCut, 2000.0f) < at(highCut, 2000.0f) - 20.0f);
        }

        beginTest("Curve: the comb has repeated peaks - every harmonic, or the odd ones an octave down");
        {
            auto pos = db(FilterModel::Comb, 0, 500.0f, 0.8f);
            auto found = peaks(pos, 300.0f, 2800.0f, 6.0f);
            expect(found.size() >= 4, "Positive comb peaks found: " + juce::String((int) found.size()));
            if (found.size() >= 2)
            {
                expectWithinAbsoluteError(found[0], 500.0f, 40.0f);
                expectWithinAbsoluteError(found[1], 1000.0f, 80.0f);
            }
            auto neg = db(FilterModel::Comb, 1, 500.0f, 0.8f);
            expect(at(neg, 250.0f) > at(neg, 500.0f) + 6.0f && at(neg, 750.0f) > at(neg, 500.0f) + 6.0f,
                   "Negative comb peaks at 250 and 750, dip at 500");
            // No resonance: flat.
            auto flat = db(FilterModel::Comb, 0, 500.0f, 0.0f);
            expect(peaks(flat, 300.0f, 2800.0f, 3.0f).empty());
        }

        beginTest("Curve: two filters are shown as routed; the cache measures only when something changes");
        {
            FilterCurve::Settings s = FilterCurve::single(FilterModel::SVF, 0, 1000.0f, 0.0f);
            s.model[1] = FilterModel::SVF;
            s.cutoffHz[1] = 1000.0f;
            const float single = at(FilterCurve::measureDb(s, N), 8000.0f);
            s.routing = FilterRouting::Series;
            const float series = at(FilterCurve::measureDb(s, N), 8000.0f);
            expect(series < single - 20.0f, "Series is twice as steep: " + juce::String(single) + " / " + juce::String(series));

            s.routing = FilterRouting::Parallel;
            s.mode[1] = 2;                       // B is a high-pass
            s.balance = 0.0f;                    // A only
            expect(at(FilterCurve::measureDb(s, N), 8000.0f) < -20.0f);
            s.balance = 1.0f;                    // B only
            expect(at(FilterCurve::measureDb(s, N), 8000.0f) > -6.0f);

            FilterCurve::Cache cache;
            auto a = cache.get(s, N);
            cache.get(s, N);
            expectEquals(cache.getMeasurementCount(), 1);
            s.cutoffHz[0] = 1200.0f;
            cache.get(s, N);
            expectEquals(cache.getMeasurementCount(), 2);
            cache.get(s, 64);
            expectEquals(cache.getMeasurementCount(), 3);
        }

        beginTest("Curve: an engine's filter is found from its FLTR pages, in Hz or 0..1");
        {
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                               EngineType::WaveSeq, EngineType::Sampler })
            {
                auto engine = AudioEngine::createEngine(type);
                auto b = FilterCurve::bind(engine->getParamPages());
                expect(b.valid && b.reso.isValid() && b.mode.isValid(), "Engine " + juce::String((int) type) + " has a filter");
            }
            for (auto type : { EngineType::Drum, EngineType::MIDITrack })
                expect(!FilterCurve::bind(AudioEngine::createEngine(type)->getParamPages()).valid);

            ParamSpec norm { "CUT", 5, 0, 1, 1 };
            expectWithinAbsoluteError(FilterCurve::cutoffHz(norm, 0.5f), 632.5f, 1.0f);
            ParamSpec hz { "CUT", 5, 20, 20000, 8000, ParamKind::Log, "Hz" };
            expectWithinAbsoluteError(FilterCurve::cutoffHz(hz, 440.0f), 440.0f, 0.01f);
        }

        // ------------------------------------------------------------------ the FLTR screen
        beginTest("FLTR unbound: named, draws nothing, ignores input");
        {
            FilterViewScreen screen;
            DisplayRenderer display;
            expectEquals(screen.getName(), juce::String("FLTR"));
            expect(!screen.hasFilter());
            display.clear();
            screen.render(display);
            screen.onEncoderChange(0, 1);
            screen.onPadPress(3, 100);
            screen.onArrowRight();
            screen.onFuncDown();
            expect(screen.getEncoderName(0).isEmpty() && screen.getEncoderReadout(0).isEmpty());
            expect(!screen.isStepHeld());
        }

        beginTest("FLTR on the unified voice: CUT RESO TYPE MODE lead, values are the track's own");
        {
            Rig rig(EngineType::UnifiedVoice);
            auto& params = rig.seq.getPattern().getTrack(0).soundParams;
            expect(rig.screen.hasFilter());
            expect(rig.screen.usesEncoderBar() && rig.screen.getEncoderLayout() == Screen::EncoderLayout::Bar);
            expectEquals(rig.screen.getSubPage(), juce::String("FILTR"));
            expectEquals(rig.screen.getPageCount(), 3);

            const char* labels[8] = { "CUT", "RESO", "TYPE", "MODE", "ENV", "DRIV", "MORP", "FFM" };
            const char* names[8] = { "CT", "RS", "TY", "MD", "EN", "DR", "MO", "FM" };
            auto page = rig.screen.getCurrentPage();
            for (int i = 0; i < 8; ++i)
            {
                expectEquals(juce::String(page.params[(size_t) i].label), juce::String(labels[i]));
                expectEquals(rig.screen.getEncoderName(i), juce::String(names[i]));
            }
            expectEquals(page.params[0].id, (int) UnifiedParam::CUTOFF);
            expectEquals(rig.screen.getEncoderReadout(0), juce::String("CUT 8.0k"));

            // Plain, fine and coarse turns; stored with the sound.
            rig.screen.onEncoderChange(1, 10);                               // RESO 0.20 -> 0.30
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.30f, 0.001f);
            expectWithinAbsoluteError(params[UnifiedParam::RESONANCE], 0.30f, 0.001f);
            rig.screen.setModifiers(true, false);
            rig.screen.onEncoderChange(1, 1);                                // fine: a tenth of a step
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.301f, 0.0002f);
            rig.screen.setModifiers(false, true);
            rig.screen.onEncoderChange(0, 127);                              // coarse: an octave down
            expectWithinAbsoluteError(rig.value(UnifiedParam::CUTOFF), 4000.0f, 1.0f);
            rig.screen.setModifiers(false, false);
            expectEquals(rig.screen.getEncoderReadout(0), juce::String("CUT 4.0k"));

            // The other filter pages follow as the engine has them.
            rig.screen.onArrowRight();
            expectEquals(rig.screen.getSubPage(), juce::String("FLT B"));
            expectEquals(rig.screen.getEncoderName(4), juce::String("RT"));
            rig.screen.onEncoderChange(0, 127);                              // TYPB: OFF -> COMB
            rig.screen.onEncoderChange(4, 1);                                // ROUT: series
            FilterCurve::Settings s;
            expect(rig.screen.getFilterSettings(s));
            expect(s.routing == FilterRouting::Series && s.model[1] == FilterModel::Comb);
            rig.screen.onArrowRight();
            expectEquals(rig.screen.getSubPage(), juce::String("F ENV"));
            // An envelope knob on F ENV: the picture is the filter envelope for a second.
            DisplayRenderer filterPicture, envelopePicture;
            rig.screen.render(filterPicture);
            rig.screen.onEncoderChange(0, 1);                                // ATK
            expect(rig.screen.isEnvelopePictureActive());
            rig.screen.render(envelopePicture);
            int differing = 0;
            for (int y = ScreenLayout::CONTENT_Y; y <= ScreenLayout::CONTENT_BOTTOM; ++y)
                for (int x = 0; x < 128; ++x)
                    if (filterPicture.getPixel(x, y) != envelopePicture.getPixel(x, y)) ++differing;
            expect(differing > 40, "The envelope replaces the response curve");
            rig.screen.expireFilterPicture();
            DisplayRenderer after;
            rig.screen.render(after);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 128; ++x)
                    if (after.getPixel(x, y) != filterPicture.getPixel(x, y)) { expect(false, "The curve comes back unchanged"); y = 64; break; }

            // The bank buttons go round.
            rig.screen.onArrowRight();
            expectEquals(rig.screen.getPageIndex(), 0);
            rig.screen.onArrowLeft();
            expectEquals(rig.screen.getPageIndex(), 2);
        }

        beginTest("FLTR draws the measured curve for every model and mode, and it follows the knobs");
        {
            Rig rig(EngineType::UnifiedVoice);
            DisplayRenderer display;

            for (int m = 0; m < FilterSlot::NUM_MODELS; ++m)
            {
                rig.audio.setTrackParameter(0, UnifiedParam::MODEL_A, (float) m);
                for (int mode = 0; mode < FilterSlot::modeCount(FilterSlot::modelFromIndex(m)); ++mode)
                {
                    rig.audio.setTrackParameter(0, UnifiedParam::MODE_A, (float) mode);
                    display.clear();
                    rig.screen.render(display);
                    // A curve crosses every column of the content area.
                    int columns = 0;
                    for (int x = 0; x < 128; ++x)
                        columns += litPixels(display, x, 11, 1, 42) > 0 ? 1 : 0;
                    expectEquals(columns, 128, juce::String(FilterSlot::modelNames()[m]) + " mode " + juce::String(mode));
                    expectEquals(litPixels(display, 0, 53, 128, 11), 0, "The bar area is left to the framework");
                }
            }

            // Low-pass at 200 Hz against 5 kHz: the right-hand part of the picture drops.
            rig.audio.setTrackParameter(0, UnifiedParam::MODEL_A, 0.0f);
            rig.audio.setTrackParameter(0, UnifiedParam::MODE_A, 0.0f);
            auto lowestLit = [&](int x)
            {
                for (int y = 11; y <= 52; ++y)
                    if (display.getPixel(x, y)) return y;
                return 99;
            };
            rig.audio.setTrackParameter(0, UnifiedParam::CUTOFF, 5000.0f);
            display.clear(); rig.screen.render(display);
            const int open = lowestLit(90);          // about 2.7 kHz
            rig.audio.setTrackParameter(0, UnifiedParam::CUTOFF, 200.0f);
            display.clear(); rig.screen.render(display);
            expect(lowestLit(90) > open + 8, "Closing the filter lowers the curve at 2.7 kHz");

            // A resonant peak rises above the passband.
            const int flatTop = lowestLit(41);       // just below 200 Hz (column 42 holds the cutoff mark)
            rig.screen.onEncoderChange(1, 60);       // RESO up
            display.clear(); rig.screen.render(display);
            expect(lowestLit(41) < flatTop - 3, "Resonance raises a peak at the cutoff");
        }

        beginTest("FLTR: hold a pad + turn locks the filter on that step; the picture shows the held step");
        {
            Rig rig(EngineType::UnifiedVoice);
            auto& track = rig.seq.getPattern().getTrack(0);
            rig.screen.onPadPress(4, 100);
            expect(rig.screen.isStepHeld());
            rig.screen.setModifiers(false, true);
            rig.screen.onEncoderChange(0, 126);                              // two octaves down, locked
            rig.screen.setModifiers(false, false);
            expectWithinAbsoluteError(track.getStep(4).paramLocks[UnifiedParam::CUTOFF], 2000.0f, 1.0f);
            expectWithinAbsoluteError(rig.value(UnifiedParam::CUTOFF), 8000.0f, 1.0f);
            FilterCurve::Settings s;
            expect(rig.screen.getFilterSettings(s));
            expectWithinAbsoluteError(s.cutoffHz[0], 2000.0f, 1.0f);
            expectEquals(rig.screen.getEncoderReadout(0), juce::String("CUT 2.0k"));
            expectEquals(rig.screen.getSubPage(), juce::String("S5 FILTR"));
            rig.screen.onPadRelease(4);
            expect(rig.screen.getFilterSettings(s));
            expectWithinAbsoluteError(s.cutoffHz[0], 8000.0f, 1.0f);
        }

        beginTest("FLTR on an engine with the shared voice filter (FM, macro, wavetable, wave sequencer)");
        {
            for (auto type : { EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable, EngineType::WaveSeq })
            {
                Rig rig(type);
                const juce::String name = "Engine " + juce::String((int) type);
                auto b = rig.binding();
                expect(rig.screen.hasFilter(), name);
                expectEquals(rig.screen.getPageCount(), 2, name);
                const char* names[8] = { "CT", "RS", "TY", "MD", "EN", "DR", "MO", "KT" };
                for (int i = 0; i < 8; ++i)
                    expectEquals(rig.screen.getEncoderName(i), juce::String(names[i]), name);

                rig.screen.onEncoderChange(0, 20);                           // already fully open
                expectWithinAbsoluteError(rig.value(b.cut.id), 20000.0f, 1.0f);
                rig.screen.onEncoderChange(0, 128 - 50);                     // half the range down
                expect(rig.value(b.cut.id) < 1000.0f, name + " cutoff " + juce::String(rig.value(b.cut.id)));
                expectWithinAbsoluteError(rig.seq.getPattern().getTrack(0).soundParams[b.cut.id], rig.value(b.cut.id), 0.01f);

                rig.screen.onEncoderChange(2, 2);                            // TYPE: SVF -> LADR
                expectEquals((int) rig.value(b.type.id), 2, name);
                rig.screen.onEncoderChange(3, 5);                            // MODE: the ladder has six
                expectEquals((int) rig.value(b.mode.id), 5, name);
                FilterCurve::Settings s;
                expect(rig.screen.getFilterSettings(s));
                expect(s.model[0] == FilterModel::Ladder && s.mode[0] == 5 && s.routing == FilterRouting::Single, name);
                expectWithinAbsoluteError(s.cutoffHz[0], rig.value(b.cut.id), 0.01f);

                DisplayRenderer display;
                display.clear();
                rig.screen.render(display);
                expect(litPixels(display, 0, 11, 128, 42) > 128, name + " draws a curve");
            }
        }

        beginTest("FLTR on the sampler: CUT is 0..1 over 20 Hz - 20 kHz");
        {
            Rig rig(EngineType::Sampler);
            expect(rig.screen.hasFilter());
            expectEquals(rig.screen.getPageCount(), 1);
            const char* names[8] = { "CT", "RS", "TY", "MD", "VL", "EN", "KT", "DR" };
            for (int i = 0; i < 8; ++i)
                expectEquals(rig.screen.getEncoderName(i), juce::String(names[i]));

            rig.screen.onEncoderChange(0, 128 - 50);                         // CUT 1.0 -> 0.5
            expectWithinAbsoluteError(rig.value(SamplerEngine::FilterCutoff), 0.5f, 0.001f);
            rig.screen.onEncoderChange(2, 3);                                // TYPE -> DIOD
            FilterCurve::Settings s;
            expect(rig.screen.getFilterSettings(s));
            expectWithinAbsoluteError(s.cutoffHz[0], 632.5f, 1.0f);
            expect(s.model[0] == FilterModel::Diode);
            expectWithinAbsoluteError(s.resonance[0], rig.value(SamplerEngine::FilterReso), 0.0001f);

            DisplayRenderer display;
            display.clear();
            rig.screen.render(display);
            expect(litPixels(display, 0, 11, 128, 42) > 128);
        }

        beginTest("FLTR on an engine without a filter says so, and follows the selected track");
        {
            for (auto type : { EngineType::Drum, EngineType::MIDITrack })
            {
                Rig rig(type);
                DisplayRenderer display;
                expect(!rig.screen.hasFilter());
                expectEquals(rig.screen.getSubPage(), juce::String("NO FILTER"));
                display.clear();
                rig.screen.render(display);
                expect(litPixels(display, 0, 20, 128, 22) > 50, "A message is drawn");
                rig.screen.onEncoderChange(0, 5);
                rig.screen.onPadPress(2, 100);
                expect(!rig.screen.isStepHeld());
                expect(rig.screen.getEncoderName(0).isEmpty());
            }

            Rig rig(EngineType::Drum);
            rig.audio.addEngine(1, AudioEngine::createEngine(EngineType::Wavetable));
            rig.seq.setSelectedTrack(1);
            expect(rig.screen.hasFilter(), "Track 2 has a filter");
            rig.seq.setSelectedTrack(0);
            expect(!rig.screen.hasFilter());
        }

        beginTest("FLTR bound through setAudio (as the app does)");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            FilterViewScreen screen;
            screen.setAudio(&seq, &audio);
            expect(screen.hasFilter());
            screen.onEncoderChange(1, 10);
            expectWithinAbsoluteError(audio.getTrackParameter(0, UnifiedParam::RESONANCE), 0.30f, 0.001f);
        }
    }
};

static FilterViewScreenTests filterViewScreenTests;
