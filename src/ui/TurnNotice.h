#pragma once

#include "UiClock.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>

// -----------------------------------------------------------------------------
// A knob that cannot do anything right now says why.
//
// "A control that acts on only some choices of another control reads as broken":
// where a knob has nothing to act on (MODB while filter B is OFF, RW on an empty
// song, the sampler's markers on a synth track) the screen says so in the name
// bar at the moment of the turn, for about a second, instead of doing nothing.
//
//   notice.say("SONG IS EMPTY");
//
// The text goes to the name bar's message (ScreenManager::showStatus, through the
// sink the app sets: the mechanism that shows "COPY TRK"), which makes room for
// it. A screen also returns shown() from getSubPage() - and from
// getEncoderReadout() on a bar screen - so that it is on the display where no
// sink is set (a screen used by itself in a test). Keep the texts to 12
// characters or so: the name bar holds about twenty with the screen's name.
//
// The screen audit counts what was said (count()): a turn that changes nothing
// else but says why is a knob that answered.
//
// All of it is called with the screen manager's lock held, like everything that
// changes screen state.
// -----------------------------------------------------------------------------
class TurnNotice
{
public:
    static constexpr juce::int64 SHOW_MS = 1000;

    void say(const juce::String& what)
    {
        text = what;
        saidMs = UiClock::nowMs();
        post(what);
    }

    void clear() { text.clear(); }
    bool showing() const { return text.isNotEmpty() && UiClock::nowMs() - saidMs < SHOW_MS; }
    juce::String shown() const { return showing() ? text : juce::String(); }

    // For a screen that keeps its own message (WAVE): counted and sent to the name bar.
    static void post(const juce::String& what)
    {
        ++counter();
        if (sink())
            sink()(what);
    }

    // How many notices have been said since the program started.
    static int count() { return counter().load(); }

    // Where the texts go (the app: ScreenManager::showStatus). Set and cleared by whoever owns
    // the screen manager.
    static void setSink(std::function<void (const juce::String&)> s) { sink() = std::move(s); }

private:
    static std::atomic<int>& counter() { static std::atomic<int> n { 0 }; return n; }
    static std::function<void (const juce::String&)>& sink()
    {
        static std::function<void (const juce::String&)> s;
        return s;
    }

    juce::String text;
    juce::int64 saidMs = 0;
};
