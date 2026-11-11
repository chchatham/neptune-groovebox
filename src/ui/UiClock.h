#pragma once

#include <juce_core/juce_core.h>
#include <atomic>

// -----------------------------------------------------------------------------
// The clock the screens use for everything that appears for a moment (the knob
// readout, the filter and envelope pictures, confirmations). It is the system
// clock plus an offset that only the screen audit moves, so the audit can see
// "a second later" without waiting a second.
// -----------------------------------------------------------------------------
namespace UiClock
{
    inline std::atomic<juce::int64>& offset() { static std::atomic<juce::int64> o { 0 }; return o; }
    inline juce::int64 nowMs() { return juce::Time::currentTimeMillis() + offset().load(); }
    inline juce::uint32 counter() { return juce::Time::getMillisecondCounter() + (juce::uint32) offset().load(); }
    inline void advance(juce::int64 ms) { offset() += ms; }
}
