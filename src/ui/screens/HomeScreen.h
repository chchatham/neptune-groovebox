#pragma once

#include "../ScreenManager.h"
#include "../NoteUtils.h"
#include "../ParamConstants.h"
#include "../../sequencer/SequencerCore.h"
#include "../../engine/AudioEngine.h"
#include "../../controller/PadManager.h"

// -----------------------------------------------------------------------------
// TRACKS: the paged mixer. Knob N is always track N; the bank buttons step
// through five pages:
//
//   VOL   track volume          PAN   pan
//   SND1  send 1                SND2  send 2
//   ENG   engine. The knob only chooses a pending engine (shown, not applied);
//         tapping that track's top pad commits it (the track's sound is
//         replaced). Leaving the page drops every pending choice.
//
// Pads: top row selects a track, bottom row mutes, Func + bottom row solos.
// Func + top row jumps straight to a page (page keys 1-5).
// -----------------------------------------------------------------------------
class HomeScreen : public Screen
{
public:
    enum Page { VOL = 0, PAN, SND1, SND2, ENG, NUM_PAGES };

    HomeScreen(SequencerCore& seq, AudioEngine* audio)
        : sequencer(seq), audioEngine(audio)
    {
        pending.fill(-1);
    }

    juce::String getName() const override { return "TRACKS"; }

    // Page name and pattern number: "VOL P1". Six characters are what the name bar has left beside
    // "TRACKS T1" and the page mark, so longer combinations are tightened ("SND1P1", "S1 P12"), and
    // while a pattern is queued the status is just that: "P1>3".
    juce::String getSubPage() const override
    {
        const juce::String pattern = "P" + juce::String(sequencer.getActivePatternIndex() + 1);
        if (sequencer.getPendingPattern() >= 0)
            return pattern + ">" + juce::String(sequencer.getPendingPattern() + 1);

        const juce::String name = pageName(page);
        if (name.length() + 1 + pattern.length() <= STATUS_ROOM)
            return name + " " + pattern;
        if (name.length() + pattern.length() <= STATUS_ROOM)
            return name + pattern;
        return name.substring(0, 1) + name.getLastCharacters(1) + " " + pattern;
    }

    int getPageCount() const override { return NUM_PAGES; }
    int getPageIndex() const override { return page; }

    int getPageKeyCount() const override { return NUM_PAGES; }
    int getActivePageKey() const override { return page; }
    void onPageKey(int index) override { setPage(index); }
    juce::String getPageKeyName(int index) const override
    {
        return (index >= 0 && index < NUM_PAGES) ? juce::String(pageName(index)) : juce::String();
    }
    // Help cards: Func + bank = first / last page, Func + a bottom-row pad = solo, Up / Down held +
    // turn = every track (not on ENG, where a knob only chooses its own track's engine).
    void getHelpFacts(HelpFacts& facts) const override
    {
        facts.bankJump = BankJump::FirstLast;
        facts.funcBottomRow = "SOLO";
        facts.allTracksTurn = (page != ENG);
    }

    // The bank buttons go round the pages.
    void onArrowLeft() override  { setPage((page + NUM_PAGES - 1) % NUM_PAGES); }
    void onArrowRight() override { setPage((page + 1) % NUM_PAGES); }
    void onSectionPrev() override { setPage(0); }
    void onSectionNext() override { setPage(NUM_PAGES - 1); }

    void setPage(int index)
    {
        index = juce::jlimit(0, (int) NUM_PAGES - 1, index);
        if (index != page)
            pending.fill(-1);       // leaving ENG cancels what was not committed
        page = index;
    }

    int getPendingEngine(int trackIndex) const
    {
        return (trackIndex >= 0 && trackIndex < 8) ? pending[(size_t) trackIndex] : -1;
    }

    void render(DisplayRenderer& display) override
    {
        const int trackCount = juce::jmin(8, sequencer.getPattern().getTrackCount());
        const int selected = sequencer.getSelectedTrack();

        for (int i = 0; i < trackCount; ++i)
        {
            const int x = i * 16;
            auto& track = sequencer.getPattern().getTrack(i);

            display.drawChar(x + 5, 12, '1' + (char) i);
            display.drawString(x + 1, 21, NoteUtils::engineAbbrev((int) track.engineType));

            if (track.soloed)
                display.drawString(x + 5, 29, "S");
            else if (track.muted)
                display.drawString(x + 5, 29, "M");

            // The page's value: a level bar, a pan marker, or the pending engine.
            if (page == ENG)
            {
                if (pending[(size_t) i] >= 0)
                {
                    display.drawString(x + 1, 39, NoteUtils::engineAbbrev(pending[(size_t) i]));
                    display.drawRect(x, 37, 15, 11, false);
                }
            }
            else if (page == PAN)
            {
                const float pan = value(i, PAN);
                display.drawRect(x + 2, 40, 12, 5, false);
                display.setPixel(x + 7, 39, true);
                display.setPixel(x + 8, 39, true);
                int panX = juce::jlimit(x + 3, x + 11, x + 3 + (int) std::lround((pan + 1.0f) * 0.5f * 8.0f));
                display.drawRect(panX, 41, 2, 3, true);
            }
            else
            {
                const int barH = 10;
                display.drawRect(x + 2, 36, 12, barH, false);
                int fillH = (int) (value(i, page) * (float) (barH - 2));
                if (fillH > 0)
                    display.drawRect(x + 3, 36 + barH - 1 - fillH, 10, fillH, true);
            }
        }

        if (selected >= 0 && selected < trackCount)
            display.invertRegion(selected * 16, 11, 16, 42);
    }

    // Top row: select track 1-8 (on ENG: commit that track's pending engine). Bottom row: mute /
    // unmute; with Func held, solo / unsolo.
    void onPadPress(int padIndex, int velocity) override
    {
        juce::ignoreUnused(velocity);
        const int trackCount = sequencer.getPattern().getTrackCount();

        if (padIndex >= 0 && padIndex < PADS_PER_ROW)
        {
            if (padIndex < trackCount)
            {
                if (page == ENG && pending[(size_t) padIndex] >= 0)
                    commitEngine(padIndex);
                sequencer.setSelectedTrack(padIndex);
                if (trackSelectionCb)
                    trackSelectionCb(padIndex);
            }
            return;
        }

        const int trackIdx = padIndex - PADS_PER_ROW;
        if (trackIdx < 0 || trackIdx >= PADS_PER_ROW || trackIdx >= trackCount)
            return;

        auto& track = sequencer.getPattern().getTrack(trackIdx);
        if (funcDown())
            track.soloed = !track.soloed;
        else
            track.muted = !track.muted;
    }

    void onActivate() override
    {
        pending.fill(-1);
        if (padManager)
            padManager->setMode(PadManager::Mode::Mute);
    }

    void onDeactivate() override { pending.fill(-1); }

    void setPadManager(PadManager* pm) { padManager = pm; }

    void onEncoderChange(int encoderIndex, int value) override
    {
        turn(encoderIndex, value);
    }

    // Track layer (Up or Down held) + turn: the same change on every track.
    void onEncoderChangeAllTracks(int encoderIndex, int value) override
    {
        if (page == ENG)
        {
            turn(encoderIndex, value);
            return;
        }
        const int trackCount = juce::jmin(8, sequencer.getPattern().getTrackCount());
        for (int t = 0; t < trackCount; ++t)
            turn(t, value);
    }

    // The bar shows each track's value on this page.
    juce::String getEncoderName(int encoderIndex) const override
    {
        if (encoderIndex < 0 || encoderIndex >= juce::jmin(8, sequencer.getPattern().getTrackCount()))
            return {};
        return valueText(encoderIndex);
    }

    // "T3 VOL 80", "T3 PAN L20", "T3 ENG >FM" (pending).
    juce::String getEncoderReadout(int encoderIndex) const override
    {
        if (encoderIndex < 0 || encoderIndex >= juce::jmin(8, sequencer.getPattern().getTrackCount()))
            return {};
        juce::String text = "T" + juce::String(encoderIndex + 1) + " " + pageName(page) + " ";
        if (page == ENG && pending[(size_t) encoderIndex] >= 0)
            return text + ">" + NoteUtils::engineAbbrev(pending[(size_t) encoderIndex]);
        return text + valueText(encoderIndex);
    }

    // Top row: tracks (selected bright; amber while an engine waits to be committed). Bottom row:
    // green = sounding, red = muted, blue = solo.
    bool getPadColours(std::array<PadColour, 16>& out) const override
    {
        const int trackCount = juce::jmin(8, sequencer.getPattern().getTrackCount());
        const int selected = sequencer.getSelectedTrack();
        for (int i = 0; i < 8; ++i)
        {
            if (i >= trackCount)
            {
                out[(size_t) i] = {};
                out[(size_t) (8 + i)] = {};
                continue;
            }
            const auto& track = sequencer.getPattern().getTrack(i);
            if (page == ENG && pending[(size_t) i] >= 0)
                out[(size_t) i] = { 127, 70, 0 };
            else if (i == selected)
                out[(size_t) i] = { 127, 127, 127 };
            else
                out[(size_t) i] = { 10, 10, 10 };

            if (track.soloed)      out[(size_t) (8 + i)] = { 0, 0, 127 };
            else if (track.muted)  out[(size_t) (8 + i)] = { 40, 0, 0 };
            else                   out[(size_t) (8 + i)] = { 0, 90, 0 };
        }
        return true;
    }

    using EngineChangeCallback = std::function<void(int trackIndex, EngineType type)>;
    void setEngineChangeCallback(EngineChangeCallback cb) { engineChangeCb = std::move(cb); }

    void setTrackSelectionCallback(std::function<void(int)> cb)
    {
        trackSelectionCb = std::move(cb);
    }

    static const char* pageName(int p)
    {
        static const char* const names[NUM_PAGES] = { "VOL", "PAN", "SND1", "SND2", "ENG" };
        return names[juce::jlimit(0, (int) NUM_PAGES - 1, p)];
    }

    static constexpr int ENGINE_TYPE_COUNT = 8;
    static constexpr int STATUS_ROOM = 6;

private:
    static const ParamSpec& spec(int p)
    {
        static const char* const engines[ENGINE_TYPE_COUNT] = { "VA", "FM", "MO", "WT", "WS", "SP", "DR", "MD" };
        static const ParamSpec specs[NUM_PAGES] = {
            { "VOL",  MixParam::VOLUME,        0.0f, 1.0f, MixParam::DEFAULT_VOLUME },
            { "PAN",  MixParam::PAN,          -1.0f, 1.0f, 0.0f },
            { "SND1", MixParam::SEND_BASE,     0.0f, 1.0f, 0.0f },
            { "SND2", MixParam::SEND_BASE + 1, 0.0f, 1.0f, 0.0f },
            { "ENG",  0, 0.0f, (float) (ENGINE_TYPE_COUNT - 1), 0.0f, ParamKind::Choice, "", engines, ENGINE_TYPE_COUNT }
        };
        return specs[juce::jlimit(0, (int) NUM_PAGES - 1, p)];
    }

    float value(int trackIndex, int p) const
    {
        const auto& s = spec(p);
        return ParamConstants::getTrackParam(sequencer.getPattern().getTrack(trackIndex), s.id, s.defaultVal);
    }

    juce::String valueText(int trackIndex) const
    {
        if (page == ENG)
            return NoteUtils::engineAbbrev((int) sequencer.getPattern().getTrack(trackIndex).engineType);
        const float v = value(trackIndex, page);
        if (page == PAN)
        {
            const int pct = juce::jlimit(-99, 99, (int) std::lround(v * 100.0f));
            return pct == 0 ? juce::String("C") : (pct < 0 ? "L" + juce::String(-pct) : "R" + juce::String(pct));
        }
        return juce::String((int) std::lround(v * 100.0f));
    }

    void turn(int trackIndex, int encoderValue)
    {
        if (trackIndex < 0 || trackIndex >= 8 || trackIndex >= sequencer.getPattern().getTrackCount())
            return;
        const int clicks = encoderValue > 64 ? encoderValue - 128 : encoderValue;
        if (clicks == 0)
            return;
        auto& track = sequencer.getPattern().getTrack(trackIndex);
        const auto& s = spec(page);

        if (page == ENG)
        {
            // Only a choice: nothing changes until the track's top pad is tapped.
            const int current = (int) track.engineType;
            const int from = pending[(size_t) trackIndex] >= 0 ? pending[(size_t) trackIndex] : current;
            const int next = (int) std::lround(s.turned((float) from, clicks, turnStep()));
            pending[(size_t) trackIndex] = (next == current) ? -1 : next;
            return;
        }

        const float next = s.turned(value(trackIndex, page), clicks, turnStep());
        track.soundParams[s.id] = next;
        if (audioEngine)
            audioEngine->setTrackParameter(trackIndex, s.id, next);
    }

    void commitEngine(int trackIndex)
    {
        const auto type = static_cast<EngineType>(pending[(size_t) trackIndex]);
        pending[(size_t) trackIndex] = -1;
        if (audioEngine)
            audioEngine->setEngineType(trackIndex, type);
        // A track's sound is the same in every pattern.
        for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
            if (trackIndex < sequencer.getPattern(p).getTrackCount())
                sequencer.getPattern(p).getTrack(trackIndex).engineType = type;
        if (engineChangeCb)
            engineChangeCb(trackIndex, type);
    }

    SequencerCore& sequencer;
    AudioEngine* audioEngine;
    static constexpr int PADS_PER_ROW = 8;

    PadManager* padManager = nullptr;
    int page = VOL;
    std::array<int, 8> pending {};      // engine type waiting for its pad, or -1
    std::function<void(int)> trackSelectionCb;
    EngineChangeCallback engineChangeCb;
};
