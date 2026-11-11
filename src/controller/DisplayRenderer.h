#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <atomic>
#include <functional>
#include <vector>

class LaunchkeyController;

class DisplayRenderer
{
public:
    static constexpr int WIDTH = 128;
    static constexpr int HEIGHT = 64;
    static constexpr int BYTES_PER_ROW = 19;
    static constexpr int SYSEX_DATA_SIZE = BYTES_PER_ROW * HEIGHT; // 1216
    static constexpr int ACK_TIMEOUT_MS = 200;
    static constexpr int PERIODIC_RESEND_MS = 5000;     // safety net only; a still screen is not resent more often

    DisplayRenderer();

    void setController(LaunchkeyController* ctrl) { controller = ctrl; }

    void clear();
    void setPixel(int x, int y, bool on);
    bool getPixel(int x, int y) const;

    void drawHLine(int x, int y, int width);
    void drawVLine(int x, int y, int height);
    void drawRect(int x, int y, int w, int h, bool filled = false);
    void drawChar(int x, int y, char ch);
    void drawCharInverted(int x, int y, char ch);
    void drawString(int x, int y, const juce::String& text);
    void drawStringInverted(int x, int y, const juce::String& text);
    void invertRegion(int x, int y, int w, int h);
    void drawBitmap(int x, int y, const uint8_t* data, int w, int h);

    // Threads. The pixel buffer belongs to whoever draws (the screen manager, under its own lock).
    // sendFrame() takes a copy of the finished picture; the device's ready reply, which arrives on
    // the MIDI thread, only ever sends such a copy, never the buffer that may be half drawn.
    void sendFrame(bool stationary = true);

    void onBitmapAck();
    void forceResend();

    // Nothing waiting to go out and no reply outstanding: the device shows the last picture drawn.
    bool isIdle() const { const juce::ScopedLock sl(sendLock); return !waitingForAck && !pendingValid; }
    // Called (on the sending thread) with every frame that goes to the device. For the screen audit.
    std::function<void(const std::array<juce::uint8, SYSEX_DATA_SIZE>&)> onFrameSent;
    // The frame the device was sent last, as pixels.
    void getLastSentPixels(std::array<std::array<bool, WIDTH>, HEIGHT>& out) const;

    // ---- Drawing audit (off unless a DrawAudit is attached; see ui/ScreenAudit.h) ----------------
    // Records where one frame's drawing collides: text drawn over something already lit, something
    // drawn over text, and text that runs off the edge of the glass.
    struct DrawAudit
    {
        struct Box
        {
            int x0 = WIDTH, y0 = HEIGHT, x1 = -1, y1 = -1, count = 0;
            void add(int x, int y) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); ++count; }
            bool any() const { return count > 0; }
        };
        std::array<std::array<juce::uint8, WIDTH>, HEIGHT> owner {};   // 0 dark, 1 text, 2 other
        Box overText;        // pixels where text and something else were both drawn
        Box clipped;         // text pixels that fell outside the glass
        void reset() { for (auto& r : owner) r.fill(0); overText = {}; clipped = {}; }
    };
    void setDrawAudit(DrawAudit* a) { drawAudit = a; }

    void tickAckTimeout();

    // The time used for the reply time-out and the safety-net resend; tests put their own here.
    std::function<juce::int64()> clock;

    // Counters for the log: frames sent, acknowledged, timed out, and the mean time to acknowledge.
    int getFramesSent() const { return framesSent; }
    int getAckCount() const { return ackCount; }
    int getTimeoutCount() const { return timeoutCount; }
    double getMeanAckMs() const { return ackCount > 0 ? (double) ackMsTotal / ackCount : 0.0; }

    bool isDirty() const { return dirty; }
    bool isWaitingForAck() const { return waitingForAck; }

    const std::array<juce::uint8, SYSEX_DATA_SIZE>& getSysExData() const { return sysexBuffer; }

    using PixelBuffer = std::array<std::array<bool, WIDTH>, HEIGHT>;
    const PixelBuffer& getPixels() const { return pixels; }

private:
    void pixelsToSysEx();
    juce::int64 nowMs() const { return clock ? clock() : juce::Time::currentTimeMillis(); }

    PixelBuffer pixels {};
    std::array<juce::uint8, SYSEX_DATA_SIZE> sysexBuffer {};

    LaunchkeyController* controller = nullptr;
    std::atomic<bool> dirty { false };
    std::atomic<bool> waitingForAck { false };

    // The send side, shared by the drawing thread and the MIDI thread.
    void flushPending();                 // sendLock held
    mutable juce::CriticalSection sendLock;
    std::array<juce::uint8, SYSEX_DATA_SIZE> pendingBuffer {};    // the newest finished picture
    std::atomic<bool> pendingValid { false };                     // ... and it has not gone out yet
    bool everSubmitted = false;
    bool pendingStationary = true;
    std::array<juce::uint8, SYSEX_DATA_SIZE> lastSentBuffer {};

    DrawAudit* drawAudit = nullptr;
    bool drawingText = false;

    juce::int64 lastSendTimeMs { 0 };
    juce::int64 lastResendTimeMs { 0 };
    int framesSent = 0, ackCount = 0, timeoutCount = 0;
    juce::int64 ackMsTotal = 0;
};
