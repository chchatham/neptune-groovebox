#pragma once

#include "../TurnNotice.h"
#include "../ScreenManager.h"
#include "../../sequencer/SequencerCore.h"
#include "../../engine/AudioEngine.h"
#include "../../engine/SamplerEngine.h"
#include "../../engine/WavetableEngine.h"
#include "../../engine/SampleBank.h"
#include "../../dsp/DSPUtils.h"

// WAVE screen, two pages.
//
//   WAVE  the selected track's sample as a filled waveform with start / end brackets, loop marks,
//         slice ticks and live playheads (one dashed line per sounding voice; in granular mode the
//         scan position as a line and each grain as a short tick).
//         Knobs: ST EN LS LE | ZM SC MD SL  (start, end, loop start, loop end | zoom, scroll, play
//         mode, slice).
//         On a wavetable track the page is the KEY FRAME editor: the sample of one of the eight
//         keys of the KEYS table, a full-height line where that key's cycle is taken and a short
//         tick for every other key taken from the same sample.
//         Knobs: KY SM PO PL | ZM SC AL TB  (which key, its sample, its position in the sample,
//         its place along the table | zoom, scroll, the algorithm between keys, the table).
//         The top-row pads choose the key too. Giving a key a sample selects the KEYS table.
//         The same cells are on the wavetable's KEYS, K SMP, K POS and K PLC pages.
//   REC   resampling: record one track (after its effects) or the master into a new sample.
//         Knobs: SR TH LN QN | RC NM SM CR  (source, threshold, length, start now / on the bar |
//         record, normalise, sample, crop). RC: two clicks clockwise within a second arm (a stray
//         click does nothing), one click anticlockwise stops. A finished take
//         is saved as R001, R002 ... and given to the selected track: a sampler plays it, a
//         wavetable track uses it as its USER table - or, while its table is KEYS, as the sample
//         of the key chosen on the WAVE page. CR (four clicks clockwise) keeps only the
//         start..end region of a sampler track's sample, as a new sample.
class WaveformScreen : public Screen
{
public:
    WaveformScreen(SequencerCore& seq, AudioEngine* audio)
        : sequencer(seq), audioEngine(audio) {}

    juce::String getName() const override { return page == 0 ? "WAVE" : "REC"; }

    juce::String getSubPage() const override
    {
        // A message stays for a few seconds, then the usual text returns.
        if (flash.isNotEmpty() && now() - flashMs < FLASH_MS)
            return flash;
        juce::String sub;
        if (page == 0 && getWavetableEngine() != nullptr)
        {
            const auto* e = SampleBank::get().entry(keySampleIndex(focusKey));
            return "K" + juce::String(focusKey + 1) + " " + (e != nullptr ? juce::String(e->name) : juce::String("OFF"));
        }
        if (const auto* e = currentEntry())
            sub += juce::String(e->name);
        if (page == 0)
            if (auto* samp = getSamplerEngine())
                sub += " " + juce::String(modeName((int) samp->getParameter(SamplerEngine::Mode)));
        return sub.trim();
    }

    // The clock used by RC's arming rule and CR's confirmation (tests inject their own).
    void setClock(std::function<juce::int64()> c) { clock = std::move(c); }
    static constexpr juce::int64 ARM_WINDOW_MS = 1000;

    int getPageCount() const override { return 2; }
    int getPageIndex() const override { return page; }
    // Two pages, and the bank buttons go round: either button changes to the other page.
    void onArrowLeft() override  { page = 1 - page; flash.clear(); cancelGestures(); }
    void onArrowRight() override { page = 1 - page; flash.clear(); cancelGestures(); }
    void onDeactivate() override { cancelGestures(); }

    // ---- From the app's refresh timer: a finished take becomes a sample --------------------------
    void poll()
    {
        if (audioEngine == nullptr || ! audioEngine->getResampler().isFinished())
            return;
        const int index = audioEngine->commitResample(normalise, writeFiles);
        if (index >= 0)
        {
            assignSample(index);
            setFlash("SAVED " + juce::String(SampleBank::get().entry(index)->name));
        }
        else
            setFlash("EMPTY");
    }
    void setWriteFiles(bool on) { writeFiles = on; }      // tests keep recordings in memory only

    // ---- Drawing ---------------------------------------------------------------------------------
    void render(DisplayRenderer& display) override
    {
        if (page == 0) renderWave(display);
        else           renderRec(display);
    }

    juce::String getEncoderName(int i) const override
    {
        static const char* const wave[] = { "ST", "EN", "LS", "LE", "ZM", "SC", "MD", "SL" };
        static const char* const keys[] = { "KY", "SM", "PO", "PL", "ZM", "SC", "AL", "TB" };
        static const char* const rec[]  = { "SR", "TH", "LN", "QN", "RC", "NM", "SM", "CR" };
        if (i < 0 || i >= 8) return {};
        return page == 0 ? (getWavetableEngine() != nullptr ? keys[i] : wave[i]) : rec[i];
    }

    void onEncoderChange(int encoderIndex, int value) override
    {
        const int delta = dsp::encoderDelta(value);
        if (delta == 0) return;
        flash.clear();
        if (page == 0) waveEncoder(encoderIndex, delta);
        else           recEncoder(encoderIndex, delta);
    }

    // Full name and value of knob i, for the name bar.
    juce::String getEncoderReadout(int i) const override
    {
        auto pct = [](float v) { return juce::String(v * 100.0f, 1) + "%"; };
        auto* samp = getSamplerEngine();
        if (page == 0 && getWavetableEngine() != nullptr)
        {
            using W = WavetableEngine;
            const bool on = SampleBank::get().entry(keySampleIndex(focusKey)) != nullptr;
            const juce::String key = "KEY " + juce::String(focusKey + 1);
            switch (i)
            {
                case 0: return key + (on ? "" : " OFF");
                case 1: return key + " SAMPLE " + keyCellText(W::KeySampleBase + focusKey);
                case 2: return on ? key + " AT " + pct(keyValue(W::KeyPositionBase + focusKey)) : key + " OFF";
                case 3: return on ? key + " PLACE " + pct(keyValue(W::KeyPlaceBase + focusKey)) : key + " OFF";
                case 4: return "ZOOM " + juce::String(juce::roundToInt((zoomEnd - zoomStart) * 100.0f)) + "%";
                case 5: return "SCROLL " + juce::String(juce::roundToInt(zoomStart * 100.0f)) + "%";
                case 6: return "ALGORITHM " + keyCellText(W::KeyAlgo);
                case 7: return "TABLE " + keyCellText(W::TableSelect);
                default: return {};
            }
        }
        if (page == 0)
        {
            auto marker = [&](const char* name, int id, float whenZero)
            {
                if (samp == nullptr) return juce::String(name) + " --";
                float v = samp->getParameter(id);
                if (v <= 0.0f && whenZero > 0.0f) v = whenZero;
                return juce::String(name) + " " + pct(v);
            };
            switch (i)
            {
                case 0: return marker("START", SamplerEngine::StartPoint, 0.0f);
                case 1: return marker("END", SamplerEngine::EndPoint, 1.0f);
                case 2: return marker("LOOP START", SamplerEngine::LoopStart, 0.0f);
                case 3: return marker("LOOP END", SamplerEngine::LoopEnd, 1.0f);
                case 4: return "ZOOM " + juce::String(juce::roundToInt((zoomEnd - zoomStart) * 100.0f)) + "%";
                case 5: return "SCROLL " + juce::String(juce::roundToInt(zoomStart * 100.0f)) + "%";
                case 6: return samp ? "MODE " + juce::String(modeName((int) samp->getParameter(SamplerEngine::Mode))) : juce::String("MODE --");
                case 7: return samp && samp->getSliceCount() > 1
                            ? "SLICE " + juce::String(selectedSlice + 1) + "/" + juce::String(samp->getSliceCount())
                            : juce::String("SLICE --");
                default: return {};
            }
        }
        if (audioEngine == nullptr) return {};
        auto& rec = audioEngine->getResampler();
        switch (i)
        {
            case 0: return "SOURCE " + sourceName();
            case 1:
            {
                const int thr = juce::roundToInt(rec.getThreshold() * 100.0f);
                return thr > 0 ? "THRESHOLD " + juce::String(thr) : juce::String("THRESHOLD OFF");
            }
            case 2: return juce::String("LENGTH ") + lengthName(lengthChoice);
            case 3: return rec.isBarStart() ? "START ON BAR" : "START NOW";
            case 4:
                switch (rec.getState())
                {
                    case Resampler::State::Armed:     return "RECORD ARMED";
                    case Resampler::State::Recording: return "RECORDING";
                    case Resampler::State::Finished:  return "RECORD SAVING";
                    default:                          return armClicks > 0 ? "RECORD: ONE MORE" : "RECORD READY";
                }
            case 5: return normalise ? "NORMALISE ON" : "NORMALISE OFF";
            case 6:
                if (const auto* e = currentEntry()) return "SAMPLE " + juce::String(e->name);
                return "SAMPLE --";
            case 7: return "CROP " + confirm.progress();
            default: return {};
        }
    }

    // Pads. WAVE: one per slice, the selected one bright. REC: dark; the top row red while a take
    // is being recorded.
    bool getPadColours(std::array<PadColour, 16>& out) const override
    {
        out.fill({});
        if (page == 0 && getWavetableEngine() != nullptr)
        {
            // The eight keys: the chosen one bright, keys in use amber, unused ones dim.
            for (int k = 0; k < WavetableKeys::MAX_KEYS; ++k)
            {
                const bool on = SampleBank::get().entry(keySampleIndex(k)) != nullptr;
                out[(size_t) k] = k == focusKey ? PadColour { 127, 127, 127 }
                                : on ? PadColour { 127, 110, 40 } : PadColour { 12, 8, 0 };
            }
        }
        else if (page == 0)
        {
            auto* samp = getSamplerEngine();
            const int slices = samp != nullptr ? samp->getSliceCount() : 0;
            const int playing = samp != nullptr ? samp->getPlayingSlice() : -1;
            for (int i = 0; i < 16 && i < slices; ++i)
                out[(size_t) i] = (i == playing) ? PadColour { 127, 127, 127 }
                                : (i == selectedSlice) ? PadColour { 127, 110, 40 } : PadColour { 30, 18, 0 };
        }
        else if (audioEngine != nullptr && audioEngine->getResampler().getState() == Resampler::State::Recording)
        {
            for (int i = 0; i < 8; ++i)
                out[(size_t) i] = { 127, 0, 0 };
        }
        return true;
    }

    // Top-row pads choose a slice (as before).
    void onPadPress(int padIndex, int /*velocity*/) override
    {
        cancelGestures();
        if (page == 0 && getWavetableEngine() != nullptr)
        {
            if (padIndex >= 0 && padIndex < WavetableKeys::MAX_KEYS)
                focusKey = padIndex;
            return;
        }
        auto* samp = getSamplerEngine();
        if (page != 0 || samp == nullptr) return;
        if (padIndex >= 0 && padIndex < samp->getSliceCount())
            selectSlice(padIndex);
    }

    // For tests and the display.
    float getZoomStart() const { return zoomStart; }
    float getZoomEnd() const { return zoomEnd; }
    int getLengthChoice() const { return lengthChoice; }
    int getFocusKey() const { return focusKey; }
    bool isNormalise() const { return normalise; }

private:
    // ================================ WAVE page ================================================
    static const char* modeName(int mode)
    {
        static const char* const names[] = { "FWD", "REV", "1SHT", "GATE", "LOOP", "PING", "GRAN" };
        return (mode >= 0 && mode < 7) ? names[mode] : "";
    }

    void renderWave(DisplayRenderer& display)
    {
        const float* data = nullptr;
        int length = 0;
        auto* samp = getSamplerEngine();
        auto* wt = getWavetableEngine();
        if (samp != nullptr)
        {
            data = samp->getSampleData();
            length = samp->getSampleLength();
        }
        else if (wt != nullptr)                         // a wavetable track: the chosen key's sample
        {
            if (const auto* e = SampleBank::get().entry(keySampleIndex(focusKey)))
            {
                data = e->left.data();
                length = (int) e->left.size();
            }
            else
            {
                display.drawString(8, 22, "KEY " + juce::String(focusKey + 1) + " IS OFF.");
                display.drawString(8, 32, "TURN SM TO GIVE");
                display.drawString(8, 42, "IT A SAMPLE.");
                return;
            }
        }
        else if (const auto* e = currentEntry())       // any other track: the sample last browsed
        {
            data = e->left.data();
            length = (int) e->left.size();
        }
        if (data == nullptr || length <= 0)
        {
            display.drawString(8, 22, "NO SAMPLE ON THIS");
            display.drawString(8, 32, "TRACK. NEXT PAGE");
            display.drawString(8, 42, "RECORDS ONE.");
            return;
        }

        constexpr int x0 = 2, w = 124, y0 = 12, h = 39;
        const int midY = y0 + h / 2;
        const float span = juce::jmax(0.0001f, zoomEnd - zoomStart);
        const int viewStart = (int) (zoomStart * (float) length);
        const int viewLen = juce::jmax(1, (int) (span * (float) length));

        for (int px = 0; px < w; ++px)
        {
            const int a = viewStart + (int) ((juce::int64) px * viewLen / w);
            const int b = juce::jmax(a + 1, viewStart + (int) ((juce::int64) (px + 1) * viewLen / w));
            float lo = 0.0f, hi = 0.0f;
            // At most 64 probes per column keeps a long sample cheap to draw.
            const int step = juce::jmax(1, (b - a) / 64);
            for (int s = a; s < b && s < length; s += step)
            {
                lo = juce::jmin(lo, data[s]);
                hi = juce::jmax(hi, data[s]);
            }
            const int yTop = juce::jlimit(y0, y0 + h - 1, midY - (int) (hi * (float) (h / 2)));
            const int yBot = juce::jlimit(y0, y0 + h - 1, midY - (int) (lo * (float) (h / 2)));
            for (int y = yTop; y <= yBot; ++y)
                display.setPixel(x0 + px, y, true);
        }

        auto toPx = [&](float frac) -> int       // -1 when outside the view
        {
            const float v = (frac - zoomStart) / span;
            return (v < 0.0f || v > 1.0f) ? -1 : x0 + juce::jmin(w - 1, (int) (v * (float) w));
        };

        if (samp != nullptr)
        {
            // Start and end: full-height brackets. Loop: short marks at the bottom. Slices: ticks on top.
            const float start = samp->getParameter(SamplerEngine::StartPoint);
            float end = samp->getParameter(SamplerEngine::EndPoint);
            if (end <= 0.0f) end = 1.0f;
            if (int px = toPx(start); px >= 0)
            {
                display.drawVLine(px, y0, h);
                display.drawHLine(px, y0, 3);
                display.drawHLine(px, y0 + h - 1, 3);
            }
            if (int px = toPx(end); px >= 0)
            {
                display.drawVLine(px, y0, h);
                display.drawHLine(px - 2, y0, 3);
                display.drawHLine(px - 2, y0 + h - 1, 3);
            }
            float loopEnd = samp->getParameter(SamplerEngine::LoopEnd);
            if (loopEnd <= 0.0f) loopEnd = 1.0f;
            if (int px = toPx(samp->getParameter(SamplerEngine::LoopStart)); px >= 0)
            {
                display.drawVLine(px, y0 + h - 5, 5);
                display.setPixel(px + 1, y0 + h - 5, true);
            }
            if (int px = toPx(loopEnd); px >= 0)
            {
                display.drawVLine(px, y0 + h - 5, 5);
                display.setPixel(px - 1, y0 + h - 5, true);
            }
            const int slices = samp->getSliceCount();
            for (int s = 0; s < slices; ++s)       // slice 0 too: a transient slice need not start at 0
                if (int px = toPx(samp->getSliceStartNormalized(s)); px >= 0 && (s > 0 || samp->getSliceStartNormalized(0) > 0.0f))
                    display.drawVLine(px, y0, 3);
            if (slices > 1 && selectedSlice < slices)
            {
                // Real slice positions: transient slices are not evenly spaced.
                const int a = toPx(samp->getSliceStartNormalized(selectedSlice));
                const int b = selectedSlice + 1 < slices ? toPx(samp->getSliceStartNormalized(selectedSlice + 1)) : toPx(1.0f);
                if (a >= 0 && b > a)
                    display.invertRegion(a, y0 + h - 2, b - a, 2);
            }

            // Playheads: sounding voices (dashed lines) and, in granular mode, the grains (ticks).
            float heads[MAX_HEADS];
            bool grain[MAX_HEADS];
            const int n = samp->getPlayheads(heads, MAX_HEADS, grain);
            for (int i = 0; i < n; ++i)
            {
                const int px = toPx(heads[i]);
                if (px < 0) continue;
                if (grain[i])
                    display.invertRegion(px, midY - 3, 1, 7);
                else
                    for (int y = y0; y < y0 + h; y += 2)
                        display.invertRegion(px, y, 1, 1);
            }
        }

        if (wt != nullptr)
        {
            // Where the keys are taken: the chosen key a full-height line with a flag, every other
            // key on this sample a short tick at the top.
            const int sample = keySampleIndex(focusKey);
            for (int k = 0; k < WavetableKeys::MAX_KEYS; ++k)
            {
                if (keySampleIndex(k) != sample)
                    continue;
                const int px = toPx(keyValue(WavetableEngine::KeyPositionBase + k));
                if (px < 0)
                    continue;
                if (k == focusKey)
                {
                    display.invertRegion(px, y0, 1, h);
                    display.drawHLine(px, y0, 3);
                    display.drawHLine(px, y0 + 1, 2);
                }
                else
                    display.drawVLine(px, y0, 4);
            }
        }

        // What part of the sample is in view, when zoomed in.
        if (span < 0.999f)
            display.invertRegion(x0 + (int) (zoomStart * (float) w), y0 + h, juce::jmax(2, (int) (span * (float) w)), 1);
    }

    void waveEncoder(int index, int delta)
    {
        auto* samp = getSamplerEngine();
        const auto mode = turnStep();
        const float span = zoomEnd - zoomStart;
        // Plain: fine enough to place a marker inside the zoomed view (one click = 1/250 of what is
        // shown). Shift: a tenth of that. Func: the quarters of the whole sample.
        auto nudge = [&](int id, float fallbackWhenZero)
        {
            if (samp == nullptr) return;
            float cur = samp->getParameter(id);
            if (cur <= 0.0f && fallbackWhenZero > 0.0f) cur = fallbackWhenZero;
            float next;
            if (mode == TurnStep::Coarse)
                next = ParamSpec { "POS", 0, 0, 1, fallbackWhenZero, ParamKind::Linear }.turned(cur, delta, mode);
            else
                next = cur + (float) delta * span / (mode == TurnStep::Fine ? 2500.0f : 250.0f);
            setTrackParam(id, juce::jlimit(0.0f, 1.0f, next));
        };
        if (auto* wt = getWavetableEngine(); wt != nullptr && (index <= 3 || index >= 6))
        {
            keyEncoder(*wt, index, delta, mode, span);
            return;
        }
        // The markers, play mode and slices belong to a sampler track; on any other track say so
        // instead of doing nothing.
        if (samp == nullptr && (index <= 3 || index >= 6))
        {
            setFlash("SAMPLER ONLY");
            return;
        }
        switch (index)
        {
            case 0: nudge(SamplerEngine::StartPoint, 0.0f); break;
            case 1: nudge(SamplerEngine::EndPoint, 1.0f); break;
            case 2: nudge(SamplerEngine::LoopStart, 0.0f); break;
            case 3: nudge(SamplerEngine::LoopEnd, 1.0f); break;
            case 4:     // ZM: clockwise zooms in around the centre of the view (Func: halves / doubles it)
            {
                const float factor = mode == TurnStep::Coarse ? 0.5f : (mode == TurnStep::Fine ? 0.992f : 0.92f);
                const float centre = (zoomStart + zoomEnd) * 0.5f;
                const float range = juce::jlimit(0.01f, 1.0f, span * std::pow(factor, (float) delta));
                zoomStart = juce::jlimit(0.0f, 1.0f - range, centre - range * 0.5f);
                zoomEnd = zoomStart + range;
                break;
            }
            case 5:     // SC: scroll the view (Func: a whole view at a time)
            {
                if (span >= 0.999f)
                {
                    setFlash("NOT ZOOMED");     // the whole sample is in view: nothing to scroll
                    break;
                }
                const float amount = mode == TurnStep::Coarse ? 1.0f : (mode == TurnStep::Fine ? 0.005f : 0.05f);
                zoomStart = juce::jlimit(0.0f, 1.0f - span, zoomStart + (float) delta * span * amount);
                zoomEnd = zoomStart + span;
                break;
            }
            case 6:     // MD: play mode
                if (samp != nullptr)
                {
                    static const char* const names[] = { "FWD", "REV", "1SHT", "GATE", "LOOP", "PING", "GRAN" };
                    const ParamSpec spec { "MODE", 0, 0, 6, 0, ParamKind::Choice, "", names, 7 };
                    setTrackParam(SamplerEngine::Mode, spec.turned(samp->getParameter(SamplerEngine::Mode), delta, mode));
                }
                break;
            case 7:     // SL: slice
                if (samp != nullptr && samp->getSliceCount() > 1)
                {
                    const ParamSpec spec { "SLIC", 0, 0, (float) (samp->getSliceCount() - 1), 0, ParamKind::Int };
                    selectSlice((int) spec.turned((float) selectedSlice, delta, mode));
                }
                else
                {
                    setFlash("ONE SLICE");      // nothing to choose between
                }
                break;
            default: break;
        }
    }

    // ---- The key frames of a wavetable track -----------------------------------------------------
    // Bank index of key k's sample, -1 when the key is OFF.
    int keySampleIndex(int key) const
    {
        auto* wt = getWavetableEngine();
        return wt != nullptr ? (int) std::lround(wt->getParameter(WavetableEngine::KeySampleBase + key)) - 1 : -1;
    }
    float keyValue(int id) const
    {
        auto* wt = getWavetableEngine();
        return wt != nullptr ? wt->getParameter(id) : 0.0f;
    }
    // The cell of the wavetable's pages with this id (its range, its choices).
    static bool keySpec(const WavetableEngine& wt, int id, ParamSpec& out)
    {
        for (auto& p : wt.getParamPages())
            if (const auto* spec = p.find(id))
            {
                out = *spec;
                return true;
            }
        return false;
    }
    juce::String keyCellText(int id) const
    {
        ParamSpec spec;
        auto* wt = getWavetableEngine();
        return wt != nullptr && keySpec(*wt, id, spec) ? spec.format(wt->getParameter(id)) : juce::String("--");
    }

    void keyEncoder(WavetableEngine& wt, int index, int delta, TurnStep mode, float span)
    {
        using W = WavetableEngine;
        const bool on = SampleBank::get().entry(keySampleIndex(focusKey)) != nullptr;
        auto turnCell = [&](int id)
        {
            ParamSpec spec;
            if (keySpec(wt, id, spec))
                setTrackParam(id, spec.turned(wt.getParameter(id), delta, mode));
        };
        switch (index)
        {
            case 0:     // KY: which key
            {
                const ParamSpec spec { "KEY", 0, 0, (float) (WavetableKeys::MAX_KEYS - 1), 0, ParamKind::Int };
                focusKey = (int) spec.turned((float) focusKey, delta, mode == TurnStep::Coarse ? TurnStep::Plain : mode);
                break;
            }
            case 1:     // SM: its sample (OFF first). A key with a sample is heard: the table becomes KEYS.
                turnCell(W::KeySampleBase + focusKey);
                if (keySampleIndex(focusKey) >= 0 && (int) std::lround(wt.getParameter(W::TableSelect)) != W::KEY_TABLE)
                    setTrackParam(W::TableSelect, (float) W::KEY_TABLE);
                break;
            case 2:     // PO: where in the sample (as the sampler's markers: 1/250 of the view per click)
                if (! on) { setFlash("KEY IS OFF"); break; }
                if (mode == TurnStep::Coarse)
                    turnCell(W::KeyPositionBase + focusKey);
                else
                    setTrackParam(W::KeyPositionBase + focusKey,
                                  juce::jlimit(0.0f, 1.0f, wt.getParameter(W::KeyPositionBase + focusKey)
                                                               + (float) delta * span / (mode == TurnStep::Fine ? 2500.0f : 250.0f)));
                break;
            case 3:     // PL: where along the table
                if (! on) { setFlash("KEY IS OFF"); break; }
                turnCell(W::KeyPlaceBase + focusKey);
                break;
            case 6: turnCell(W::KeyAlgo); break;        // AL
            case 7: turnCell(W::TableSelect); break;    // TB
            default: break;
        }
    }

    void selectSlice(int slice)
    {
        auto* samp = getSamplerEngine();
        if (samp == nullptr) return;
        selectedSlice = slice;
        const int count = juce::jmax(1, samp->getSliceCount());
        setTrackParam(SamplerEngine::SliceSelect, count > 1 ? (float) slice / (float) (count - 1) : 0.0f);
    }

    // ================================ REC page =================================================
    static constexpr int NUM_LENGTHS = 7;
    static const char* lengthName(int i)
    {
        static const char* const names[NUM_LENGTHS] = { "FREE", "1 BEAT", "2 BEAT", "1 BAR", "2 BAR", "4 BAR", "8 BAR" };
        return names[juce::jlimit(0, NUM_LENGTHS - 1, i)];
    }
    static float lengthBeats(int i)
    {
        static const float beats[NUM_LENGTHS] = { 0, 1, 2, 4, 8, 16, 32 };
        return beats[juce::jlimit(0, NUM_LENGTHS - 1, i)];
    }
    juce::String sourceName() const
    {
        const int src = audioEngine ? audioEngine->getResampler().getSource() : Resampler::MASTER;
        return src < 0 ? juce::String("MASTER") : "TRACK " + juce::String(src + 1);
    }

    void renderRec(DisplayRenderer& display)
    {
        if (audioEngine == nullptr) return;
        auto& rec = audioEngine->getResampler();

        display.drawString(2, 13, "FROM " + sourceName());
        display.drawString(2, 23, juce::String("LEN ") + lengthName(lengthChoice));
        display.drawString(74, 23, rec.isBarStart() ? "ON BAR" : "NOW");
        const int thr = juce::roundToInt(rec.getThreshold() * 100.0f);
        display.drawString(2, 33, thr > 0 ? "THRS " + juce::String(thr) : juce::String("THRS OFF"));
        display.drawString(74, 33, normalise ? "NORM ON" : "NORM OFF");

        juce::String state;
        switch (rec.getState())
        {
            case Resampler::State::Armed:     state = "ARMED"; break;
            case Resampler::State::Recording: state = "REC " + juce::String(rec.seconds(), 1) + "S"; break;
            case Resampler::State::Finished:  state = "SAVING"; break;
            default:                          state = "READY"; break;
        }
        display.drawString(2, 43, state);
        if (rec.getState() == Resampler::State::Recording)
            display.invertRegion(0, 42, 58, 9);

        // Input level of the source while armed or recording.
        display.drawRect(74, 43, 52, 7);
        if (rec.isActive())
            display.drawRect(75, 44, juce::jlimit(0, 50, (int) (rec.getPeak() * 50.0f)), 5, true);
    }

    void recEncoder(int index, int delta)
    {
        if (audioEngine == nullptr) return;
        auto& rec = audioEngine->getResampler();
        const auto mode = turnStep();
        const bool busy = rec.getState() != Resampler::State::Idle;
        if (index != 4) armClicks = 0;
        if (index != 7) confirm.cancel();
        switch (index)
        {
            case 0:     // SR: master, then each track
                if (! busy)
                {
                    const ParamSpec spec { "SRC", 0, -1, (float) (sequencer.getPattern().getTrackCount() - 1), -1, ParamKind::Int };
                    rec.setSource((int) spec.turned((float) rec.getSource(), delta, mode));
                }
                break;
            case 1:     // TH: level that starts the take; 0 = at once
            {
                const ParamSpec spec { "THRS", 0, 0, 0.5f, 0, ParamKind::Linear, "", nullptr, 0, 0.01f };
                rec.setThreshold(spec.turned(rec.getThreshold(), delta, mode));
                break;
            }
            case 2:     // LN
            {
                const ParamSpec spec { "LEN", 0, 0, NUM_LENGTHS - 1, 0, ParamKind::Int };
                lengthChoice = (int) spec.turned((float) lengthChoice, delta, mode == TurnStep::Coarse ? TurnStep::Plain : mode);
                rec.setLengthBeats(lengthBeats(lengthChoice));
                break;
            }
            case 3:     // QN: start now (or on the threshold), or on the next bar while the sequencer runs
                rec.setBarStart(delta > 0);
                break;
            case 4:     // RC: two clicks clockwise within a second arm; one anticlockwise stops
                if (delta < 0)
                {
                    armClicks = 0;
                    rec.stop();
                    if (rec.getState() == Resampler::State::Idle) setFlash("CANCELLED");
                }
                else if (! busy)
                {
                    const juce::int64 t = now();
                    if (armClicks > 0 && t - armFirstMs > ARM_WINDOW_MS) armClicks = 0;
                    if (armClicks == 0) armFirstMs = t;
                    armClicks += delta;
                    if (armClicks >= 2)
                    {
                        armClicks = 0;
                        rec.setLengthBeats(lengthBeats(lengthChoice));
                        rec.arm();
                    }
                    else
                        setFlash("REC: ONE MORE");
                }
                break;
            case 5:     // NM
                normalise = delta > 0;
                break;
            case 6:     // SM: which sample the track uses
            {
                const int count = SampleBank::get().size();
                if (count > 0)
                {
                    const ParamSpec spec { "SMPL", 0, 0, (float) (count - 1), 0, ParamKind::Int };
                    assignSample((int) spec.turned((float) currentSampleIndex(), delta, mode));
                }
                break;
            }
            case 7:     // CR: four clicks clockwise, spread over at least 300 ms
                if (confirm.step(7, delta, now()))  crop();
                else if (confirm.steps > 0)         setFlash("CROP " + confirm.progress());
                break;
            default: break;
        }
    }

    // Keep only start..end of the sampler track's sample, as a new sample on that track.
    void crop()
    {
        auto* samp = getSamplerEngine();
        if (samp == nullptr || samp->getSampleLength() <= 0) { setFlash("SAMPLER ONLY"); return; }
        const int length = samp->getSampleLength();
        float end = samp->getParameter(SamplerEngine::EndPoint);
        if (end <= 0.0f) end = 1.0f;
        const int a = juce::jlimit(0, length - 1, (int) (samp->getParameter(SamplerEngine::StartPoint) * (float) length));
        const int b = juce::jlimit(a + 1, length, (int) (end * (float) length));
        const float* l = samp->getSampleData();
        const float* r = samp->getSampleDataR();
        const auto* e = SampleBank::get().entry(currentSampleIndex());
        const int index = SampleBank::get().addRecording(l + a, r != nullptr ? r + a : nullptr, b - a,
                                                         e != nullptr ? e->sampleRate : 44100.0, writeFiles);
        if (index < 0) { setFlash("BANK FULL"); return; }
        assignSample(index);
        setTrackParam(SamplerEngine::StartPoint, 0.0f);
        setTrackParam(SamplerEngine::EndPoint, 1.0f);
        setTrackParam(SamplerEngine::LoopStart, 0.0f);
        setTrackParam(SamplerEngine::LoopEnd, 1.0f);
        zoomStart = 0.0f; zoomEnd = 1.0f;
        setFlash("CROPPED " + juce::String(SampleBank::get().entry(index)->name));
    }

    // ================================ Shared ===================================================
    EngineBase* engine() const
    {
        return audioEngine ? audioEngine->getEngine(sequencer.getSelectedTrack()) : nullptr;
    }
    SamplerEngine* getSamplerEngine() const { return dynamic_cast<SamplerEngine*>(engine()); }
    WavetableEngine* getWavetableEngine() const { return dynamic_cast<WavetableEngine*>(engine()); }

    // The bank sample the selected track uses: a sampler's sample, a wavetable track's USER source;
    // on any other track the one last browsed here.
    int currentSampleIndex() const
    {
        if (auto* samp = getSamplerEngine()) return (int) std::lround(samp->getParameter(SamplerEngine::SampleSelect));
        if (auto* wt = getWavetableEngine())
            return keysInUse(*wt) ? keySampleIndex(focusKey) : (int) std::lround(wt->getParameter(WavetableEngine::UserSample));
        return browsed;
    }
    const SampleBank::Entry* currentEntry() const { return SampleBank::get().entry(currentSampleIndex()); }
    static bool keysInUse(const WavetableEngine& wt)
    {
        return (int) std::lround(wt.getParameter(WavetableEngine::TableSelect)) == WavetableEngine::KEY_TABLE;
    }

    void assignSample(int index)
    {
        browsed = index;
        if (getSamplerEngine() != nullptr)
            setTrackParam(SamplerEngine::SampleSelect, (float) index);
        else if (auto* wt = getWavetableEngine(); wt != nullptr && keysInUse(*wt))
            setTrackParam(WavetableEngine::KeySampleBase + focusKey, (float) (index + 1));     // the chosen key's sample
        else if (getWavetableEngine() != nullptr)
        {
            setTrackParam(WavetableEngine::UserSample, (float) index);
            setTrackParam(WavetableEngine::TableSelect, (float) WavetableEngine::USER_TABLE);
        }
    }

    juce::int64 now() const { return clock ? clock() : UiClock::nowMs(); }
    static constexpr juce::int64 FLASH_MS = 2500;
    void setFlash(const juce::String& text) { flash = text; flashMs = now(); TurnNotice::post(text); }
    juce::int64 flashMs = 0;
    void cancelGestures() { armClicks = 0; confirm.cancel(); }

    void setTrackParam(int id, float value)
    {
        const int t = sequencer.getSelectedTrack();
        if (audioEngine) audioEngine->setTrackParameter(t, id, value);
        sequencer.getPattern().getTrack(t).soundParams[id] = value;
    }

    static constexpr int MAX_HEADS = 48;

    SequencerCore& sequencer;
    AudioEngine* audioEngine;
    int page = 0;
    int selectedSlice = 0;
    int focusKey = 0;           // the key frame the WAVE page shows on a wavetable track
    float zoomStart = 0.0f;
    float zoomEnd = 1.0f;
    int lengthChoice = 0;
    bool normalise = true;
    bool writeFiles = true;
    int browsed = 0;
    TurnConfirm confirm;
    std::function<juce::int64()> clock;
    int armClicks = 0;
    juce::int64 armFirstMs = 0;
    juce::String flash;
};
