#include "DisplayRenderer.h"
#include "LaunchkeyController.h"
#include "../ui/BitmapFont.h"

DisplayRenderer::DisplayRenderer()
{
    clear();
}

void DisplayRenderer::clear()
{
    for (auto& row : pixels)
        row.fill(false);
    dirty = true;
    if (drawAudit != nullptr)
        drawAudit->reset();
}

void DisplayRenderer::setPixel(int x, int y, bool on)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT)
    {
        if (drawAudit != nullptr && drawingText && on)
            drawAudit->clipped.add(juce::jlimit(0, WIDTH - 1, x), juce::jlimit(0, HEIGHT - 1, y));
        return;
    }
    if (drawAudit != nullptr)
    {
        auto& owner = drawAudit->owner[(size_t) y][(size_t) x];
        if (!on)
            owner = 0;
        else
        {
            const juce::uint8 kind = drawingText ? 1 : 2;
            if (owner != 0 && (owner == 1 || kind == 1))
                drawAudit->overText.add(x, y);
            owner = kind;
        }
    }
    if (pixels[y][x] != on)
    {
        pixels[y][x] = on;
        dirty = true;
    }
}

bool DisplayRenderer::getPixel(int x, int y) const
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT)
        return false;
    return pixels[y][x];
}

void DisplayRenderer::drawHLine(int x, int y, int width)
{
    for (int i = 0; i < width; ++i)
        setPixel(x + i, y, true);
}

void DisplayRenderer::drawVLine(int x, int y, int height)
{
    for (int i = 0; i < height; ++i)
        setPixel(x, y + i, true);
}

void DisplayRenderer::drawRect(int x, int y, int w, int h, bool filled)
{
    if (filled)
    {
        for (int row = 0; row < h; ++row)
            drawHLine(x, y + row, w);
    }
    else
    {
        drawHLine(x, y, w);
        drawHLine(x, y + h - 1, w);
        drawVLine(x, y, h);
        drawVLine(x + w - 1, y, h);
    }
}

void DisplayRenderer::drawChar(int x, int y, char ch)
{
    auto* glyph = BitmapFont::getGlyph(ch);

    drawingText = true;
    for (int col = 0; col < BitmapFont::CHAR_WIDTH; ++col)
    {
        uint8_t colData = glyph[col];
        for (int row = 0; row < BitmapFont::CHAR_HEIGHT; ++row)
        {
            if (colData & (1 << row))
                setPixel(x + col, y + row, true);
        }
    }
    drawingText = false;
}

void DisplayRenderer::drawCharInverted(int x, int y, char ch)
{
    auto* glyph = BitmapFont::getGlyph(ch);

    for (int col = 0; col < BitmapFont::CHAR_WIDTH; ++col)
    {
        uint8_t colData = glyph[col];
        for (int row = 0; row < BitmapFont::CHAR_HEIGHT; ++row)
        {
            if (colData & (1 << row))
                setPixel(x + col, y + row, false);
        }
    }
}

void DisplayRenderer::drawString(int x, int y, const juce::String& text)
{
    int cx = x;
    for (int i = 0; i < text.length(); ++i)
    {
        drawChar(cx, y, (char) text[i]);
        cx += BitmapFont::TOTAL_CHAR_WIDTH;
    }
}

void DisplayRenderer::drawStringInverted(int x, int y, const juce::String& text)
{
    int cx = x;
    for (int i = 0; i < text.length(); ++i)
    {
        drawCharInverted(cx, y, (char) text[i]);
        cx += BitmapFont::TOTAL_CHAR_WIDTH;
    }
}

void DisplayRenderer::invertRegion(int x, int y, int w, int h)
{
    int x0 = juce::jmax(0, x);
    int y0 = juce::jmax(0, y);
    int x1 = juce::jmin(WIDTH, x + w);
    int y1 = juce::jmin(HEIGHT, y + h);

    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px)
            pixels[py][px] = !pixels[py][px];

    dirty = true;
}

void DisplayRenderer::drawBitmap(int x, int y, const uint8_t* data, int w, int h)
{
    for (int col = 0; col < w; ++col)
    {
        uint8_t colData = data[col];
        for (int row = 0; row < h; ++row)
        {
            if (colData & (1 << row))
                setPixel(x + col, y + row, true);
        }
    }
}

void DisplayRenderer::pixelsToSysEx()
{
    for (int row = 0; row < HEIGHT; ++row)
    {
        for (int byteIdx = 0; byteIdx < BYTES_PER_ROW; ++byteIdx)
        {
            juce::uint8 val = 0;
            for (int bit = 0; bit < 7; ++bit)
            {
                int pixelX = byteIdx * 7 + bit;
                if (pixelX < WIDTH && pixels[row][pixelX])
                    val |= (1 << (6 - bit));
            }
            sysexBuffer[row * BYTES_PER_ROW + byteIdx] = val;
        }
    }
}

void DisplayRenderer::sendFrame(bool stationary)
{
    pixelsToSysEx();            // the caller has finished drawing: this is a whole picture
    if (!controller)
        return;

    const juce::ScopedLock sl(sendLock);
    pendingBuffer = sysexBuffer;
    pendingStationary = stationary;
    pendingValid = true;
    everSubmitted = true;
    dirty = true;
    // While the device is still drawing the previous frame the copy waits; it goes out the moment
    // the acknowledgement arrives (latest frame wins, nothing queues).
    if (!waitingForAck)
        flushPending();
}

void DisplayRenderer::flushPending()
{
    if (!pendingValid)
        return;
    pendingValid = false;
    dirty = false;
    if (pendingBuffer == lastSentBuffer)
        return;

    controller->sendBitmap(pendingBuffer.data(), SYSEX_DATA_SIZE, pendingStationary);
    lastSentBuffer = pendingBuffer;
    ++framesSent;
    waitingForAck = true;
    lastSendTimeMs = nowMs();
    // The safety-net resend is for a still screen: five seconds from the last frame, not every five
    // seconds whatever is going on (that put a repeat of the old picture in front of a new one
    // now and then while a knob was being turned, costing a frame's time).
    lastResendTimeMs = lastSendTimeMs;
    if (onFrameSent)
        onFrameSent(pendingBuffer);
}

void DisplayRenderer::forceResend()
{
    const juce::ScopedLock sl(sendLock);
    lastSentBuffer.fill(0xFF);
    if (everSubmitted)
        pendingValid = true;
    dirty = true;
}

void DisplayRenderer::getLastSentPixels(std::array<std::array<bool, WIDTH>, HEIGHT>& out) const
{
    const juce::ScopedLock sl(sendLock);
    for (int row = 0; row < HEIGHT; ++row)
        for (int x = 0; x < WIDTH; ++x)
            out[(size_t) row][(size_t) x] = (lastSentBuffer[(size_t) (row * BYTES_PER_ROW + x / 7)] >> (6 - x % 7)) & 1;
}

void DisplayRenderer::onBitmapAck()
{
    const juce::ScopedLock sl(sendLock);
    if (waitingForAck)
    {
        ++ackCount;
        ackMsTotal += nowMs() - lastSendTimeMs;
    }
    waitingForAck = false;

    if (controller)
        flushPending();
}

void DisplayRenderer::tickAckTimeout()
{
    const juce::ScopedLock sl(sendLock);
    auto now = nowMs();

    if (waitingForAck && (now - lastSendTimeMs) > ACK_TIMEOUT_MS)
    {
        waitingForAck = false;
        ++timeoutCount;
        if (controller)
            flushPending();
    }

    // Safety net: a still screen is sent again now and then.
    if (!waitingForAck && controller && everSubmitted && (now - lastResendTimeMs) > PERIODIC_RESEND_MS)
    {
        lastResendTimeMs = now;
        lastSentBuffer.fill(0xFF);
        pendingValid = true;
        flushPending();
    }
}
