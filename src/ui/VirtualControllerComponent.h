#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <set>
#include <vector>
#include <cmath>

class VirtualControllerComponent : public juce::Component
{
public:
    std::function<void(int channel, int note, int velocity)> onNoteOn;
    std::function<void(int channel, int note)> onNoteOff;
    std::function<void(int channel, int cc, int value)> onCC;
    std::function<void(int value14)> onPitchBend;   // pitch strip: 0..16383, 8192 = centre
    std::function<void(int value)> onModStrip;      // mod strip: 0..127 (the hardware's CC 1)

    // What the hardware sends (docs/launchkey-mk4-control-map.md). The test compares these with
    // the numbers ScreenManager and LaunchkeyController listen for.
    static constexpr int BUTTON_CHANNEL = 1;
    static constexpr int SHIFT_CHANNEL = 7;
    static constexpr int ENCODER_CHANNEL = 16;   // the app's internal step format, CC 0x15-0x1C
    static constexpr int KEYS_CHANNEL = 17;      // LaunchkeyController::KEYS_PORT_CHANNEL: a key is never a pad
    static constexpr int CC_SHIFT = 63;
    static constexpr int CC_PATTERN_KEY = 104;
    static constexpr int CC_FUNC = 105;
    static constexpr int CC_UP = 106;
    static constexpr int CC_DOWN = 107;
    static constexpr int CC_SETTINGS = 108;
    static constexpr int CC_ARP = 109;
    static constexpr int CC_SCALE = 110;
    static constexpr int CC_BANK_UP = 51;
    static constexpr int CC_BANK_DOWN = 52;
    static constexpr int CC_PLAY = 115;
    static constexpr int CC_RECORD = 117;

    static constexpr int CTRL_W = 880;
    static constexpr int CTRL_H = 400;

    VirtualControllerComponent()
    {
        setSize(CTRL_W, CTRL_H);
        buildLayout();
    }

    void setPadColor(int padIndex, int r, int g, int b)
    {
        if (padIndex >= 0 && padIndex < 16)
        {
            padColors[padIndex] = { r, g, b };
            repaint();
        }
    }

    // Press / release a control by its label, and move a strip, as the mouse does (also for tests).
    int elementNamed(const juce::String& label) const
    {
        for (int i = 0; i < (int)elements.size(); ++i)
            if (elements[(size_t)i].label == label)
                return i;
        return -1;
    }
    void press(int idx)   { if (idx >= 0 && idx < (int)elements.size()) pressElement(idx); }
    void release(int idx) { if (idx >= 0 && idx < (int)elements.size()) releaseElement(idx); }
    int pitchStripElement() const { return pitchStripIdx; }
    int modStripElement() const   { return modStripIdx; }

    // position: 0 = bottom of the strip, 1 = top.
    void moveStrip(int idx, float position)
    {
        position = juce::jlimit(0.0f, 1.0f, position);
        if (idx == pitchStripIdx)
        {
            pitchPosition = position;
            if (onPitchBend) onPitchBend(juce::jlimit(0, 16383, (int)std::lround(position * 16383.0f)));
        }
        else if (idx == modStripIdx)
        {
            modPosition = position;
            if (onModStrip) onModStrip(juce::jlimit(0, 127, (int)std::lround(position * 127.0f)));
        }
        repaint();
    }

    // The pitch strip returns to its centre when let go; the mod strip keeps its value.
    void releaseStrip(int idx)
    {
        if (idx == pitchStripIdx)
        {
            pitchPosition = 0.5f;
            if (onPitchBend) onPitchBend(8192);
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xFF1A1A1A));

        g.setColour(juce::Colour(0xFF2D2D2D));
        g.fillRoundedRectangle(2.0f, 2.0f, (float)(CTRL_W - 4), (float)(CTRL_H - 4), 8.0f);

        drawOLEDPlaceholder(g);

        for (int i = 0; i < (int)elements.size(); ++i)
        {
            auto& el = elements[i];
            bool held = heldElements.count(i) > 0;
            bool pressed = (pressedElement == i);
            bool hovered = (hoveredElement == i);
            drawElement(g, el, i, held, pressed, hovered);
        }

        drawOctaveIndicator(g);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        int hit = hitTestElement(e.x, e.y);

        if (e.mods.isRightButtonDown())
        {
            if (hit >= 0 && elements[hit].type != ElemType::Encoder && elements[hit].type != ElemType::Strip)
            {
                if (heldElements.count(hit))
                {
                    heldElements.erase(hit);
                    releaseElement(hit);
                }
                else
                {
                    heldElements.insert(hit);
                    pressElement(hit);
                }
                repaint();
            }
            return;
        }

        if (hit >= 0 && elements[hit].type == ElemType::Encoder)
        {
            dragEncoder = hit;
            dragLastY = e.y;
            return;
        }

        if (hit >= 0 && elements[hit].type == ElemType::Strip)
        {
            dragStrip = hit;
            moveStrip(hit, stripPositionAt(hit, e.y));
            return;
        }

        if (hit >= 0)
        {
            pressedElement = hit;
            pressElement(hit);
            repaint();
        }
        else if (!heldElements.empty())
        {
            releaseAllHeld();
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isRightButtonDown())
            return;

        dragEncoder = -1;

        if (dragStrip >= 0)
        {
            releaseStrip(dragStrip);
            dragStrip = -1;
        }

        if (pressedElement >= 0)
        {
            releaseElement(pressedElement);
            pressedElement = -1;
            repaint();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragStrip >= 0)
        {
            moveStrip(dragStrip, stripPositionAt(dragStrip, e.y));
            return;
        }

        if (dragEncoder < 0)
            return;

        int dy = dragLastY - e.y;
        if (dy == 0)
            return;

        dragLastY = e.y;

        auto& el = elements[dragEncoder];
        int encIdx = el.data1 - 0x15;
        if (encIdx < 0 || encIdx >= 8)
            return;

        int steps = dy / 2;
        if (steps == 0)
            steps = (dy > 0) ? 1 : -1;

        encoderValues[encIdx] = juce::jlimit(0, 127, encoderValues[encIdx] + steps);

        for (int s = 0; s < std::abs(steps); ++s)
        {
            int ccVal = (steps > 0) ? 1 : 127;
            if (onCC) onCC(el.channel, el.data1, ccVal);
        }
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        int hit = hitTestElement(e.x, e.y);
        if (hit != hoveredElement)
        {
            hoveredElement = hit;
            repaint();
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hoveredElement >= 0)
        {
            hoveredElement = -1;
            repaint();
        }
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        int hit = hitTestElement(e.x, e.y);
        if (hit >= 0 && elements[hit].type == ElemType::Encoder)
        {
            auto& el = elements[hit];
            int steps = (int)std::round(wheel.deltaY * 5.0f);
            if (steps == 0)
                steps = wheel.deltaY > 0 ? 1 : -1;

            int encIdx = el.data1 - 0x15;
            if (encIdx >= 0 && encIdx < 8)
                encoderValues[encIdx] = juce::jlimit(0, 127, encoderValues[encIdx] + steps);

            for (int s = 0; s < std::abs(steps); ++s)
            {
                int ccVal = (steps > 0) ? 1 : 127;
                if (onCC) onCC(el.channel, el.data1, ccVal);
            }
            repaint();
        }
    }

private:
    enum class ElemType { Button, Pad, WhiteKey, BlackKey, Encoder, Strip };

    struct Element
    {
        juce::Rectangle<int> bounds;
        ElemType type;
        juce::String label;
        int channel = 1;
        int data1 = 0;
        bool isNote = false;
    };

    struct PadColor { int r = 40, g = 40, b = 40; };

    std::vector<Element> elements;
    std::set<int> heldElements;
    int pressedElement = -1;
    int hoveredElement = -1;
    PadColor padColors[16];
    int octaveShift = 0;
    int encoderValues[8] = { 64, 64, 64, 64, 64, 64, 64, 64 };

    int octDownIdx = -1, octUpIdx = -1;
    int shiftIdx = -1, arrowUpIdx = -1, arrowDownIdx = -1;
    int dragEncoder = -1;
    int dragLastY = 0;
    int pitchStripIdx = -1, modStripIdx = -1;
    int dragStrip = -1;
    float pitchPosition = 0.5f;
    float modPosition = 0.0f;

    float stripPositionAt(int idx, int y) const
    {
        auto b = elements[(size_t)idx].bounds;
        return juce::jlimit(0.0f, 1.0f, (float)(b.getBottom() - y) / (float)juce::jmax(1, b.getHeight()));
    }

    // Layout regions
    static constexpr int LEFT_COL = 12;
    static constexpr int BTN_COL = 90;
    static constexpr int PAD_COL = 285;
    static constexpr int KEY_Y = 260;

    int addElement(juce::Rectangle<int> bounds, ElemType type, const juce::String& label,
                   int channel, int data1, bool isNote)
    {
        int idx = (int)elements.size();
        elements.push_back({ bounds, type, label, channel, data1, isNote });
        return idx;
    }

    void buildLayout()
    {
        elements.clear();
        elements.reserve(80);

        // ─── LEFT COLUMN: Pitch + Mod strips (vertical, far left) ───
        pitchStripIdx = addElement({ LEFT_COL, 15, 30, 230 }, ElemType::Strip, "Pitch", 1, 0, false);
        modStripIdx   = addElement({ LEFT_COL + 38, 15, 30, 230 }, ElemType::Strip, "Mod", 1, 1, false);

        // ─── BUTTON CLUSTER (left of pads, below OLED area) ───
        // Arp + Scale (right of OLED placeholder area)
        addElement({ 225, 14, 48, 28 }, ElemType::Button, "Arp", BUTTON_CHANNEL, CC_ARP, false);
        addElement({ 225, 48, 48, 28 }, ElemType::Button, "Scale", BUTTON_CHANNEL, CC_SCALE, false);

        // Shift + Settings
        shiftIdx = addElement({ BTN_COL, 90, 62, 32 }, ElemType::Button, "Shift", SHIFT_CHANNEL, CC_SHIFT, false);
        addElement({ BTN_COL + 68, 90, 62, 32 }, ElemType::Button, "Settings", BUTTON_CHANNEL, CC_SETTINGS, false);

        // Play + Record
        addElement({ BTN_COL, 130, 62, 32 }, ElemType::Button,
                   juce::CharPointer_UTF8("\xe2\x96\xb6 Play"), BUTTON_CHANNEL, CC_PLAY, false);
        addElement({ BTN_COL + 68, 130, 62, 32 }, ElemType::Button,
                   juce::CharPointer_UTF8("\xe2\x97\x8f Rec"), BUTTON_CHANNEL, CC_RECORD, false);

        // Oct - / Oct +
        octDownIdx = addElement({ BTN_COL, 170, 62, 32 }, ElemType::Button, "Oct -", 1, 0, false);
        octUpIdx   = addElement({ BTN_COL + 68, 170, 62, 32 }, ElemType::Button, "Oct +", 1, 0, false);

        // ─── ENCODER ROW (above pads, right section) ───
        int encStartX = PAD_COL + 42;
        int encSpacing = 62;
        for (int i = 0; i < 8; ++i)
        {
            int cx = encStartX + i * encSpacing;
            addElement({ cx, 10, 46, 46 }, ElemType::Encoder,
                       "E" + juce::String(i + 1), ENCODER_CHANNEL, 0x15 + i, false);
        }

        // ─── PAD SECTION (2x8 grid with arrow buttons flanking) ───
        int padStartX = PAD_COL + 42;
        int padSpacing = 62;
        int padW = 55;
        int padH = 50;
        int topPadY = 72;
        int botPadY = 130;

        // Arrow Up (left of top pad row)
        arrowUpIdx = addElement({ PAD_COL, topPadY, 36, padH }, ElemType::Button,
                   juce::CharPointer_UTF8("\xe2\x96\xb2"), BUTTON_CHANNEL, CC_UP, false);

        // Top 8 pads
        for (int col = 0; col < 8; ++col)
        {
            int x = padStartX + col * padSpacing;
            int note = 0x60 + col;
            addElement({ x, topPadY, padW, padH }, ElemType::Pad, "", 1, note, true);
            padColors[col] = { 200, 100, 220 };
        }

        // Arrow Right (right of top pad row)
        int rightBtnX = padStartX + 8 * padSpacing;
        addElement({ rightBtnX, topPadY, 36, padH }, ElemType::Button,
                   juce::CharPointer_UTF8("\xe2\x96\xba"), BUTTON_CHANNEL, CC_PATTERN_KEY, false);

        // Arrow Down (left of bottom pad row)
        arrowDownIdx = addElement({ PAD_COL, botPadY, 36, padH }, ElemType::Button,
                   juce::CharPointer_UTF8("\xe2\x96\xbc"), BUTTON_CHANNEL, CC_DOWN, false);

        // Bottom 8 pads
        for (int col = 0; col < 8; ++col)
        {
            int x = padStartX + col * padSpacing;
            int note = 0x70 + col;
            addElement({ x, botPadY, padW, padH }, ElemType::Pad, "", 1, note, true);
            padColors[8 + col] = { 100, 200, 220 };
        }

        // Func (right of bottom pad row)
        addElement({ rightBtnX, botPadY, 36, padH }, ElemType::Button, "Func", BUTTON_CHANNEL, CC_FUNC, false);

        // ─── FAR RIGHT: encoder scroll page buttons ───
        addElement({ rightBtnX + 2, 12, 32, 22 }, ElemType::Button,
                   juce::CharPointer_UTF8("\xe2\x89\xa1"), BUTTON_CHANNEL, CC_BANK_UP, false);  // ≡ enc bank up
        addElement({ rightBtnX + 2, 38, 32, 22 }, ElemType::Button,
                   juce::CharPointer_UTF8("\xe2\x89\xa1"), BUTTON_CHANNEL, CC_BANK_DOWN, false);  // ≡ enc bank down

        // No left arrow exists on the physical MK4

        // ─── KEYBOARD (full width, bottom) ───
        int keyAreaX = LEFT_COL;
        int keyAreaEnd = CTRL_W - 12;
        int keyAreaW = keyAreaEnd - keyAreaX;
        int whiteKeyW = keyAreaW / 15;
        int whiteKeyH = 120;
        int blackKeyH = 72;
        int blackKeyW = whiteKeyW * 2 / 3;

        static constexpr int whiteNoteOffsets[15] = {
            0, 2, 4, 5, 7, 9, 11,
            12, 14, 16, 17, 19, 21, 23,
            24
        };
        for (int i = 0; i < 15; ++i)
        {
            int x = keyAreaX + i * whiteKeyW;
            int midiNote = 48 + whiteNoteOffsets[i];
            addElement({ x, KEY_Y, whiteKeyW - 2, whiteKeyH }, ElemType::WhiteKey,
                       juce::MidiMessage::getMidiNoteName(midiNote, true, true, 3),
                       KEYS_CHANNEL, midiNote, true);
        }

        struct BlackKeyDef { int afterWhite; int noteOffset; };
        static constexpr BlackKeyDef blackDefs[10] = {
            {0, 1}, {1, 3}, {3, 6}, {4, 8}, {5, 10},
            {7, 13}, {8, 15}, {10, 18}, {11, 20}, {12, 22}
        };
        for (int i = 0; i < 10; ++i)
        {
            int wx = keyAreaX + blackDefs[i].afterWhite * whiteKeyW;
            int bx = wx + whiteKeyW - blackKeyW / 2;
            int midiNote = 48 + blackDefs[i].noteOffset;
            addElement({ bx, KEY_Y, blackKeyW, blackKeyH }, ElemType::BlackKey,
                       juce::MidiMessage::getMidiNoteName(midiNote, true, true, 3),
                       KEYS_CHANNEL, midiNote, true);
        }
    }

    int hitTestElement(int x, int y) const
    {
        for (int i = (int)elements.size() - 1; i >= 0; --i)
        {
            if (elements[i].bounds.contains(x, y))
                return i;
        }
        return -1;
    }

    void pressElement(int idx)
    {
        auto& el = elements[idx];

        if (idx == octDownIdx)
        {
            octaveShift = juce::jlimit(-3, 3, octaveShift - 1);
            return;
        }
        if (idx == octUpIdx)
        {
            octaveShift = juce::jlimit(-3, 3, octaveShift + 1);
            return;
        }

        if (el.isNote)
        {
            int note = el.data1;
            if (el.type == ElemType::WhiteKey || el.type == ElemType::BlackKey)
                note += octaveShift * 12;
            note = juce::jlimit(0, 127, note);
            if (onNoteOn) onNoteOn(el.channel, note, 100);
        }
        else if (el.type != ElemType::Strip)
        {
            bool isShiftHeld = heldElements.count(shiftIdx) > 0;
            if (isShiftHeld && idx == arrowUpIdx)
            {
                if (onCC) onCC(1, 102, 127);
            }
            else if (isShiftHeld && idx == arrowDownIdx)
            {
                if (onCC) onCC(1, 103, 127);
            }
            else
            {
                if (onCC) onCC(el.channel, el.data1, 127);
            }
        }
    }

    void releaseElement(int idx)
    {
        auto& el = elements[idx];

        if (idx == octDownIdx || idx == octUpIdx)
            return;

        if (el.isNote)
        {
            int note = el.data1;
            if (el.type == ElemType::WhiteKey || el.type == ElemType::BlackKey)
                note += octaveShift * 12;
            note = juce::jlimit(0, 127, note);
            if (onNoteOff) onNoteOff(el.channel, note);
        }
        else if (el.type != ElemType::Strip)
        {
            bool isShiftHeld = heldElements.count(shiftIdx) > 0;
            if (isShiftHeld && idx == arrowUpIdx)
            {
                if (onCC) onCC(1, 102, 0);
            }
            else if (isShiftHeld && idx == arrowDownIdx)
            {
                if (onCC) onCC(1, 103, 0);
            }
            else
            {
                if (onCC) onCC(el.channel, el.data1, 0);
            }
        }
    }

    void releaseAllHeld()
    {
        for (int idx : heldElements)
            releaseElement(idx);
        heldElements.clear();
    }

    // ─── Drawing ────────────────────────────────────────────────

    void drawOLEDPlaceholder(juce::Graphics& g) const
    {
        juce::Rectangle<float> oled(BTN_COL, 14.0f, 130.0f, 65.0f);
        g.setColour(juce::Colour(0xFF0A0A0A));
        g.fillRoundedRectangle(oled, 3.0f);
        g.setColour(juce::Colour(0xFF444444));
        g.drawRoundedRectangle(oled, 3.0f, 0.5f);
        g.setColour(juce::Colour(0xFF555555));
        g.setFont(juce::FontOptions(10.0f));
        g.drawText(juce::CharPointer_UTF8("OLED \xe2\x86\x91"), oled.toNearestInt(),
                   juce::Justification::centred);
    }

    void drawOctaveIndicator(juce::Graphics& g) const
    {
        if (octaveShift == 0) return;
        juce::String text = "Oct " + juce::String(octaveShift > 0 ? "+" : "") + juce::String(octaveShift);
        g.setColour(juce::Colour(0xFFFFAA00));
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(text, BTN_COL, 208, 130, 16, juce::Justification::centred);
    }

    void drawElement(juce::Graphics& g, const Element& el, int idx,
                     bool held, bool pressed, bool hovered) const
    {
        switch (el.type)
        {
            case ElemType::Button:   drawButton(g, el, held, pressed, hovered); break;
            case ElemType::Pad:      drawPad(g, el, held, pressed, hovered); break;
            case ElemType::Encoder:  drawEncoder(g, el, hovered); break;
            case ElemType::WhiteKey: drawWhiteKey(g, el, held, pressed, hovered); break;
            case ElemType::BlackKey: drawBlackKey(g, el, held, pressed, hovered); break;
            case ElemType::Strip:    drawStrip(g, el, hovered); break;
        }
        juce::ignoreUnused(idx);
    }

    void drawButton(juce::Graphics& g, const Element& el,
                    bool held, bool pressed, bool hovered) const
    {
        auto b = el.bounds.toFloat();
        juce::Colour bg(0xFF404040);
        if (held)          bg = juce::Colour(0xFF0077CC);
        else if (pressed)  bg = juce::Colour(0xFF555555);
        else if (hovered)  bg = juce::Colour(0xFF4A4A4A);

        g.setColour(bg);
        g.fillRoundedRectangle(b, 4.0f);
        g.setColour(juce::Colour(0xFF606060));
        g.drawRoundedRectangle(b, 4.0f, 1.0f);

        g.setColour(held ? juce::Colours::white : juce::Colour(0xFFCCCCCC));
        g.setFont(juce::FontOptions(el.bounds.getHeight() < 30 ? 9.0f : 11.0f));
        g.drawText(el.label, el.bounds, juce::Justification::centred);
    }

    void drawPad(juce::Graphics& g, const Element& el,
                 bool held, bool pressed, bool hovered) const
    {
        auto b = el.bounds.toFloat();

        int padIdx = -1;
        if (el.data1 >= 0x60 && el.data1 < 0x68) padIdx = el.data1 - 0x60;
        else if (el.data1 >= 0x70 && el.data1 < 0x78) padIdx = 8 + (el.data1 - 0x70);

        juce::Colour baseColor(0xFF333333);
        if (padIdx >= 0 && padIdx < 16)
        {
            auto& pc = padColors[padIdx];
            baseColor = juce::Colour((juce::uint8)pc.r, (juce::uint8)pc.g, (juce::uint8)pc.b);
        }

        if (held)          baseColor = baseColor.brighter(0.8f);
        else if (pressed)  baseColor = baseColor.brighter(0.5f);
        else if (hovered)  baseColor = baseColor.brighter(0.15f);

        g.setColour(baseColor);
        g.fillRoundedRectangle(b, 5.0f);

        if (held)
        {
            g.setColour(juce::Colour(0xFF00AAFF));
            g.drawRoundedRectangle(b.reduced(1.0f), 5.0f, 2.0f);
        }
        else
        {
            g.setColour(baseColor.darker(0.3f));
            g.drawRoundedRectangle(b, 5.0f, 1.0f);
        }
    }

    void drawEncoder(juce::Graphics& g, const Element& el, bool hovered) const
    {
        auto b = el.bounds.toFloat();
        float cx = b.getCentreX(), cy = b.getCentreY();
        float radius = juce::jmin(b.getWidth(), b.getHeight()) * 0.45f;

        g.setColour(hovered ? juce::Colour(0xFF555555) : juce::Colour(0xFF3A3A3A));
        g.fillEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
        g.setColour(juce::Colour(0xFF666666));
        g.drawEllipse(cx - radius, cy - radius, radius * 2, radius * 2, 1.5f);

        int encIdx = el.data1 - 0x15;
        float val = (encIdx >= 0 && encIdx < 8) ? (float)encoderValues[encIdx] / 127.0f : 0.5f;
        float angle = juce::MathConstants<float>::pi * 0.75f
                      + val * juce::MathConstants<float>::pi * 1.5f;
        float ix = cx + std::cos(angle) * radius * 0.65f;
        float iy = cy + std::sin(angle) * radius * 0.65f;

        g.setColour(juce::Colour(0xFFCCCCCC));
        g.drawLine(cx, cy, ix, iy, 2.0f);
    }

    void drawWhiteKey(juce::Graphics& g, const Element& el,
                      bool held, bool pressed, bool hovered) const
    {
        auto b = el.bounds.toFloat();
        juce::Colour bg(0xFFF0F0F0);
        if (held)          bg = juce::Colour(0xFF66BBFF);
        else if (pressed)  bg = juce::Colour(0xFFCCCCCC);
        else if (hovered)  bg = juce::Colour(0xFFE8E8E8);

        g.setColour(bg);
        g.fillRoundedRectangle(b, 2.0f);
        g.setColour(juce::Colour(0xFF999999));
        g.drawRoundedRectangle(b, 2.0f, 0.5f);
    }

    void drawBlackKey(juce::Graphics& g, const Element& el,
                      bool held, bool pressed, bool hovered) const
    {
        auto b = el.bounds.toFloat();
        juce::Colour bg(0xFF222222);
        if (held)          bg = juce::Colour(0xFF0077CC);
        else if (pressed)  bg = juce::Colour(0xFF444444);
        else if (hovered)  bg = juce::Colour(0xFF333333);

        g.setColour(bg);
        g.fillRoundedRectangle(b, 2.0f);
        g.setColour(juce::Colour(0xFF111111));
        g.drawRoundedRectangle(b, 2.0f, 0.5f);
    }

    void drawStrip(juce::Graphics& g, const Element& el, bool hovered) const
    {
        auto b = el.bounds.toFloat();
        g.setColour(hovered ? juce::Colour(0xFF444444) : juce::Colour(0xFF353535));
        g.fillRoundedRectangle(b, 3.0f);
        g.setColour(juce::Colour(0xFF555555));
        g.drawRoundedRectangle(b, 3.0f, 1.0f);

        g.setColour(juce::Colour(0xFF888888));
        g.setFont(juce::FontOptions(9.0f));

        // Draw strip labels vertically
        auto labelArea = el.bounds.reduced(2);
        g.drawText(el.label, labelArea, juce::Justification::centredTop);

        // Where the finger is: the pitch strip rests in the middle, the mod strip where it was left.
        const float pos = (&el == &elements[(size_t)pitchStripIdx]) ? pitchPosition : modPosition;
        const float y = b.getBottom() - 4.0f - pos * (b.getHeight() - 8.0f);
        g.setColour(juce::Colour(0xFF00AAFF));
        g.fillRoundedRectangle(b.getX() + 3.0f, y - 2.0f, b.getWidth() - 6.0f, 4.0f, 2.0f);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VirtualControllerComponent)
};
