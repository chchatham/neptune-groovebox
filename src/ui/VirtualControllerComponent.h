#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <set>
#include <vector>
#include <cmath>
#include "PanelDimensions.h"
#include "PanelOverlay.h"
#include "../controller/DisplayRenderer.h"

// The controller's panel on the Mac screen, as it looks with the printed overlay on: every control from
// the top edge down to just above the keys, laid out from the measured table in PanelDimensions.h at one
// scale, and clickable. A control sends what the hardware's sends. There are no keys in the picture.
//   left click = press; right click = hold (to build combinations); drag or wheel on a knob; drag a strip.
// Drawing: everything that never changes (body, recesses, overlay, shadows, knob bodies) is drawn once
// into an image per display scale; pads, button caps, knob marks, strip marks and the small display are
// drawn over it, and only the part that changed is repainted.
class VirtualControllerComponent : public juce::Component,
                                   public juce::TooltipClient,
                                   private juce::Timer
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
    static constexpr int PAD_CHANNEL = 1;
    static constexpr int PAD_TOP_NOTE = 0x60;    // 96-103
    static constexpr int PAD_BOTTOM_NOTE = 0x70; // 112-119
    static constexpr int PAD_VELOCITY = 100;
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

    // One scale for the whole picture. 4 px per mm: the body is 1354 px wide, the smallest label 10 px.
    static constexpr float PX_PER_MM = 4.0f;
    static constexpr int MARGIN = 12;                       // window colour left, right and above the body
    static constexpr int CTRL_W = 1354 + 2 * MARGIN;        // 338.5 mm
    static constexpr int CTRL_H = 380 + MARGIN;             // 95 mm: the picture ends where the keys begin

    // Millimetres on the panel to pixels in this component.
    static float mmX(double mm) { return (float) MARGIN + (float) mm * PX_PER_MM; }
    static float mmY(double mm) { return (float) MARGIN + (float) mm * PX_PER_MM; }
    static juce::Rectangle<float> mmRect(const PanelDim::Rect& r)
    {
        return { mmX(r.x), mmY(r.y), (float) r.w * PX_PER_MM, (float) r.h * PX_PER_MM };
    }

    VirtualControllerComponent()
    {
        setSize(CTRL_W, CTRL_H);
        setOpaque(true);
        buildLayout();
    }

    ~VirtualControllerComponent() override { stopTimer(); }

    // The small display window shows the live picture, one screen pixel per display pixel.
    void setDisplay(const DisplayRenderer* renderer)
    {
        display = renderer;
        if (display != nullptr) shownPixels = display->getPixels(); else shownPixels = {};
        if (display != nullptr) startTimerHz(30); else stopTimer();
        repaint(displayArea().toNearestInt());
    }

    // Pad LED colour, 0..255 per channel (0, 0, 0 = unlit). Only a change repaints, and only that pad.
    void setPadColor(int padIndex, int r, int g, int b)
    {
        if (padIndex < 0 || padIndex >= 16)
            return;
        const PadColor c { juce::jlimit(0, 255, r), juce::jlimit(0, 255, g), juce::jlimit(0, 255, b) };
        if (c.r == padColors[padIndex].r && c.g == padColors[padIndex].g && c.b == padColors[padIndex].b)
            return;
        padColors[padIndex] = c;
        repaintElement(padElement[(size_t) padIndex]);
    }

    // Press / release a control by its name, and move a strip, as the mouse does (also for tests).
    int elementNamed(const juce::String& label) const
    {
        for (int i = 0; i < (int)elements.size(); ++i)
            if (elements[(size_t)i].label == label)
                return i;
        return -1;
    }
    int elementAt(float x, float y) const { return hitTestElement(x, y); }
    juce::Rectangle<float> elementBounds(int idx) const
    {
        return idx >= 0 && idx < (int) elements.size() ? elements[(size_t) idx].bounds : juce::Rectangle<float>();
    }
    int numElements() const { return (int) elements.size(); }
    juce::String elementName(int idx) const { return idx >= 0 && idx < (int) elements.size() ? elements[(size_t) idx].label : juce::String(); }

    void press(int idx)
    {
        if (idx >= 0 && idx < (int)elements.size())
        {
            downElements.insert(idx);
            pressElement(idx);
            repaintElement(idx);
        }
    }
    void release(int idx)
    {
        if (idx >= 0 && idx < (int)elements.size())
        {
            downElements.erase(idx);
            releaseElement(idx);
            repaintElement(idx);
        }
    }
    // Hold a control down until it is let go (the right click): Shift held, then a pad, is Shift + pad.
    void setHeld(int idx, bool held)
    {
        if (idx < 0 || idx >= (int) elements.size() || elements[(size_t) idx].type == ElemType::Encoder
            || elements[(size_t) idx].type == ElemType::Strip || (heldElements.count(idx) > 0) == held)
            return;
        if (held) { heldElements.insert(idx); pressElement(idx); }
        else      { heldElements.erase(idx); releaseElement(idx); }
        repaintElement(idx);
    }
    bool isHeld(int idx) const { return heldElements.count(idx) > 0; }

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
        repaintElement(idx);
    }

    // The pitch strip returns to its centre when let go; the mod strip keeps its value.
    void releaseStrip(int idx)
    {
        if (idx == pitchStripIdx)
        {
            pitchPosition = 0.5f;
            if (onPitchBend) onPitchBend(8192);
            repaintElement(idx);
        }
    }

    // The words printed on the overlay, in drawing order (for the test that compares them with the overlay's source).
    static juce::StringArray overlayWords()
    {
        juce::Image scratch(juce::Image::ARGB, 8, 8, true);
        juce::Graphics g(scratch);
        juce::StringArray words;
        PanelOverlay(g, 1.0f, 0.0f, 0.0f, &words).drawArt();
        return words;
    }

    // What a control is, for the pointer resting on it (the caps carry no printed names).
    juce::String getTooltip() override
    {
        if (hoveredElement < 0)
            return {};
        const auto& el = elements[(size_t) hoveredElement];
        switch (el.type)
        {
            case ElemType::Pad:     return el.label;
            case ElemType::Encoder: return "Knob " + juce::String(el.data1 - 0x15 + 1) + " (drag up / down, or scroll)";
            case ElemType::Strip:   return el.label + " strip (drag)";
            case ElemType::Button:  break;
        }
        return el.label + " (right click to hold)";
    }

    void paint(juce::Graphics& g) override
    {
        const float sf = juce::jlimit(1.0f, 4.0f, (float) g.getInternalContext().getPhysicalPixelScaleFactor());
        if (! staticLayer.isValid() || std::abs(sf - staticScale) > 0.01f)
            rebuildStaticLayer(sf);
        g.drawImage(staticLayer, getLocalBounds().toFloat());

        const auto clip = g.getClipBounds().toFloat();
        for (int i = 0; i < (int)elements.size(); ++i)
        {
            const auto& el = elements[(size_t) i];
            if (! clip.intersects(el.bounds.expanded(REPAINT_MARGIN)))
                continue;
            const bool held = heldElements.count(i) > 0;
            const bool down = held || pressedElement == i || downElements.count(i) > 0;
            const bool hovered = hoveredElement == i;
            switch (el.type)
            {
                case ElemType::Button:  drawButton(g, el, i, down, held, hovered); break;
                case ElemType::Pad:     drawPad(g, el, down, held, hovered); break;
                case ElemType::Encoder: drawEncoderMark(g, el, hovered || dragEncoder == i); break;
                case ElemType::Strip:   drawStripMark(g, el, i, hovered || dragStrip == i); break;
            }
        }
        if (clip.intersects(displayArea()))
            drawDisplay(g);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        int hit = hitTestElement(e.position.x, e.position.y);

        if (e.mods.isRightButtonDown())
        {
            if (hit >= 0)
                setHeld(hit, ! isHeld(hit));
            return;
        }

        if (hit >= 0 && elements[(size_t) hit].type == ElemType::Encoder)
        {
            dragEncoder = hit;
            dragLastY = e.y;
            repaintElement(hit);
            return;
        }

        if (hit >= 0 && elements[(size_t) hit].type == ElemType::Strip)
        {
            dragStrip = hit;
            moveStrip(hit, stripPositionAt(hit, e.position.y));
            return;
        }

        if (hit >= 0)
        {
            pressedElement = hit;
            pressElement(hit);
            repaintElement(hit);
        }
        else if (!heldElements.empty())
        {
            releaseAllHeld();
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isRightButtonDown())
            return;

        if (dragEncoder >= 0)
        {
            const int was = dragEncoder;
            dragEncoder = -1;
            repaintElement(was);
        }

        if (dragStrip >= 0)
        {
            const int was = dragStrip;
            dragStrip = -1;
            releaseStrip(was);
            repaintElement(was);
        }

        if (pressedElement >= 0)
        {
            const int was = pressedElement;
            pressedElement = -1;
            releaseElement(was);
            repaintElement(was);
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragStrip >= 0)
        {
            moveStrip(dragStrip, stripPositionAt(dragStrip, e.position.y));
            return;
        }

        if (dragEncoder < 0)
            return;

        int dy = dragLastY - e.y;
        if (dy == 0)
            return;

        dragLastY = e.y;

        int steps = dy / 2;
        if (steps == 0)
            steps = (dy > 0) ? 1 : -1;
        turnEncoder(dragEncoder, steps);
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        int hit = hitTestElement(e.position.x, e.position.y);
        if (hit != hoveredElement)
        {
            const int was = hoveredElement;
            hoveredElement = hit;
            repaintElement(was);
            repaintElement(hit);
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hoveredElement >= 0)
        {
            const int was = hoveredElement;
            hoveredElement = -1;
            repaintElement(was);
        }
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        int hit = hitTestElement(e.position.x, e.position.y);
        if (hit >= 0 && elements[(size_t) hit].type == ElemType::Encoder)
        {
            int steps = (int)std::round(wheel.deltaY * 5.0f);
            if (steps == 0)
                steps = wheel.deltaY > 0 ? 1 : -1;
            turnEncoder(hit, steps);
        }
    }

    // Time for `count` whole repaints with the static layer already built, in milliseconds each (for the snapshot hook).
    double measurePaintMs(int count, float scale = 2.0f)
    {
        juce::Image target(juce::Image::ARGB, (int) (CTRL_W * scale), (int) (CTRL_H * scale), true);
        { juce::Graphics g(target); g.addTransform(juce::AffineTransform::scale(scale)); paint(g); }
        const double start = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < count; ++i)
        {
            juce::Graphics g(target);
            g.addTransform(juce::AffineTransform::scale(scale));
            paint(g);
        }
        return (juce::Time::getMillisecondCounterHiRes() - start) / juce::jmax(1, count);
    }

private:
    enum class ElemType { Button, Pad, Encoder, Strip };

    struct Element
    {
        juce::Rectangle<float> bounds;
        ElemType type;
        juce::String label;
        int channel = 1;
        int data1 = 0;
        bool isNote = false;
    };

    struct PadColor { int r = 0, g = 0, b = 0; };

    std::vector<Element> elements;
    std::set<int> heldElements;      // held by a right click
    std::set<int> downElements;      // pressed through press() / release()
    int pressedElement = -1;         // pressed by the mouse
    int hoveredElement = -1;
    PadColor padColors[16];
    std::array<int, 16> padElement {};
    int encoderSteps[8] = {};        // endless: the mark simply turns with the steps sent

    int octDownIdx = -1, octUpIdx = -1;
    int shiftIdx = -1, funcIdx = -1, arrowUpIdx = -1, arrowDownIdx = -1;
    int dragEncoder = -1;
    int dragLastY = 0;
    int pitchStripIdx = -1, modStripIdx = -1;
    int dragStrip = -1;
    float pitchPosition = 0.5f;
    float modPosition = 0.0f;

    const DisplayRenderer* display = nullptr;
    DisplayRenderer::PixelBuffer shownPixels {};

    juce::Image staticLayer;
    float staticScale = 0.0f;

    static constexpr float REPAINT_MARGIN = 9.0f;     // a lit pad's halo reaches this far
    static constexpr float CAP_RADIUS = 1.5f * PX_PER_MM;

    void timerCallback() override
    {
        if (display == nullptr)
            return;
        const auto& now = display->getPixels();
        if (now != shownPixels)
        {
            shownPixels = now;
            repaint(displayArea().toNearestInt());
        }
    }

    void repaintElement(int idx)
    {
        if (idx >= 0 && idx < (int) elements.size())
            repaint(elements[(size_t) idx].bounds.expanded(REPAINT_MARGIN).getSmallestIntegerContainer());
    }

    float stripPositionAt(int idx, float y) const
    {
        auto b = elements[(size_t)idx].bounds;
        return juce::jlimit(0.0f, 1.0f, (b.getBottom() - y) / juce::jmax(1.0f, b.getHeight()));
    }

    void turnEncoder(int idx, int steps)
    {
        const auto& el = elements[(size_t) idx];
        const int encIdx = el.data1 - 0x15;
        if (encIdx < 0 || encIdx >= 8)
            return;
        encoderSteps[encIdx] += steps;
        for (int s = 0; s < std::abs(steps); ++s)
        {
            int ccVal = (steps > 0) ? 1 : 127;
            if (onCC) onCC(el.channel, el.data1, ccVal);
        }
        repaintElement(idx);
    }

    int addElement(const PanelDim::Rect& mm, ElemType type, const juce::String& label,
                   int channel, int data1, bool isNote)
    {
        int idx = (int)elements.size();
        elements.push_back({ mmRect(mm), type, label, channel, data1, isNote });
        return idx;
    }

    // Every control at its measured place (PanelDimensions.h).
    void buildLayout()
    {
        namespace D = PanelDim;
        elements.clear();
        elements.reserve(48);

        pitchStripIdx = addElement(D::STRIP_PITCH, ElemType::Strip, "Pitch", 1, 0, false);
        modStripIdx   = addElement(D::STRIP_MOD,   ElemType::Strip, "Mod", 1, 1, false);

        shiftIdx = addElement(D::BTN_SHIFT, ElemType::Button, "Shift", SHIFT_CHANNEL, CC_SHIFT, false);
        addElement(D::BTN_SETTINGS, ElemType::Button, "Settings", BUTTON_CHANNEL, CC_SETTINGS, false);
        addElement(D::BTN_PLAY, ElemType::Button, "Play", BUTTON_CHANNEL, CC_PLAY, false);
        addElement(D::BTN_REC,  ElemType::Button, "Rec", BUTTON_CHANNEL, CC_RECORD, false);
        // Octave down / up are handled inside the hardware and send nothing; so do these.
        octDownIdx = addElement(D::BTN_OCT_DN, ElemType::Button, "Oct -", 1, 0, false);
        octUpIdx   = addElement(D::BTN_OCT_UP, ElemType::Button, "Oct +", 1, 0, false);

        addElement(D::BTN_ARP,   ElemType::Button, "Arp", BUTTON_CHANNEL, CC_ARP, false);
        addElement(D::BTN_SCALE, ElemType::Button, "Scale", BUTTON_CHANNEL, CC_SCALE, false);
        arrowUpIdx   = addElement(D::BTN_UP,   ElemType::Button, "Up", BUTTON_CHANNEL, CC_UP, false);
        arrowDownIdx = addElement(D::BTN_DOWN, ElemType::Button, "Down", BUTTON_CHANNEL, CC_DOWN, false);

        for (int i = 0; i < 8; ++i)
            addElement({ D::encCx(i) - D::encClear, D::encCy - D::encClear, 2 * D::encClear, 2 * D::encClear },
                       ElemType::Encoder, "E" + juce::String(i + 1), ENCODER_CHANNEL, 0x15 + i, false);

        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 8; ++col)
                padElement[(size_t) (row * 8 + col)] =
                    addElement(D::pad(row, col), ElemType::Pad, "Pad " + juce::String(row * 8 + col + 1), PAD_CHANNEL,
                               (row == 0 ? PAD_TOP_NOTE : PAD_BOTTOM_NOTE) + col, true);

        addElement(D::BTN_BANK_UP, ElemType::Button, "Bank Up", BUTTON_CHANNEL, CC_BANK_UP, false);
        addElement(D::BTN_BANK_DN, ElemType::Button, "Bank Down", BUTTON_CHANNEL, CC_BANK_DOWN, false);
        addElement(D::BTN_PATTERN, ElemType::Button, "Pattern", BUTTON_CHANNEL, CC_PATTERN_KEY, false);
        funcIdx = addElement(D::BTN_FUNC, ElemType::Button, "Func", BUTTON_CHANNEL, CC_FUNC, false);
    }

    int hitTestElement(float x, float y) const
    {
        for (int i = (int)elements.size() - 1; i >= 0; --i)
        {
            if (elements[(size_t) i].bounds.contains(x, y))
                return i;
        }
        return -1;
    }

    void pressElement(int idx)
    {
        auto& el = elements[(size_t) idx];

        if (idx == octDownIdx || idx == octUpIdx)
            return;

        if (el.isNote)
        {
            if (onNoteOn) onNoteOn(el.channel, el.data1, PAD_VELOCITY);
        }
        else if (el.type == ElemType::Button)
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
        auto& el = elements[(size_t) idx];

        if (idx == octDownIdx || idx == octUpIdx)
            return;

        if (el.isNote)
        {
            if (onNoteOff) onNoteOff(el.channel, el.data1);
        }
        else if (el.type == ElemType::Button)
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
        const auto held = heldElements;
        for (int idx : held)
        {
            releaseElement(idx);
            repaintElement(idx);
        }
        heldElements.clear();
    }

    // ═══ Drawing ═══════════════════════════════════════════════════════════════════════════════
    // One light, above the panel and a little toward its far edge: top edges catch it, shadows fall
    // toward the keys, recesses are dark under their upper lip and lit on their lower one.

    static juce::Colour grey(int v, float alpha = 1.0f) { return juce::Colour((juce::uint8) v, (juce::uint8) v, (juce::uint8) v).withAlpha(alpha); }
    static juce::Colour white(float alpha) { return juce::Colours::white.withAlpha(alpha); }
    static juce::Colour black(float alpha) { return juce::Colours::black.withAlpha(alpha); }

    static juce::Rectangle<float> displayArea() { return mmRect(PanelDim::DISPLAY); }
    static juce::Rectangle<float> bodyArea()
    {
        // The body runs on under the keys; the component ends at the keys, so only its top corners show.
        return { mmX(0.0), mmY(0.0), (float) PanelDim::bodyW * PX_PER_MM, (float) PanelDim::keysY * PX_PER_MM + 60.0f };
    }

    static void fillVertical(juce::Graphics& g, juce::Rectangle<float> r, float radius, juce::Colour top, juce::Colour bottom)
    {
        g.setGradientFill(juce::ColourGradient(top, 0.0f, r.getY(), bottom, 0.0f, r.getBottom(), false));
        g.fillRoundedRectangle(r, radius);
    }

    void rebuildStaticLayer(float sf)
    {
        staticScale = sf;
        staticLayer = juce::Image(juce::Image::RGB, juce::roundToInt(CTRL_W * sf), juce::roundToInt(CTRL_H * sf), false);
        juce::Graphics g(staticLayer);
        g.addTransform(juce::AffineTransform::scale(sf));
        drawStatic(g);
    }

    void drawStatic(juce::Graphics& g) const
    {
        namespace D = PanelDim;
        g.fillAll(juce::Colour(0xFF1A1A1A));

        // ── the body: matte, a shade lighter toward the far edge ──
        const auto body = bodyArea();
        const float bodyRadius = 5.0f * PX_PER_MM;
        juce::Path bodyPath;
        bodyPath.addRoundedRectangle(body, bodyRadius);
        juce::DropShadow(black(0.75f), 14, { 0, 3 }).drawForPath(g, bodyPath);
        fillVertical(g, body, bodyRadius, juce::Colour(0xFF2C2D31), juce::Colour(0xFF1B1C1F));
        {
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(bodyPath);
            // a fine grain, so that the plastic is not a flat computer gradient
            g.setTiledImageFill(grainTile(), 0, 0, 1.0f);
            g.fillRect(body);
            // the rounded-over far edge catches the light; the sides fall away
            g.setGradientFill(juce::ColourGradient(white(0.10f), 0.0f, body.getY(), white(0.0f), 0.0f, body.getY() + 9.0f, false));
            g.fillRect(body.withHeight(9.0f));
            g.setGradientFill(juce::ColourGradient(black(0.30f), body.getX(), 0.0f, black(0.0f), body.getX() + 7.0f, 0.0f, false));
            g.fillRect(body.withWidth(7.0f));
            g.setGradientFill(juce::ColourGradient(black(0.0f), body.getRight() - 7.0f, 0.0f, black(0.30f), body.getRight(), 0.0f, false));
            g.fillRect(body.withLeft(body.getRight() - 7.0f));
            g.setGradientFill(juce::ColourGradient(white(0.34f), 0.0f, body.getY(), white(0.03f), 0.0f, body.getY() + 26.0f, false));
            g.strokePath(bodyPath, juce::PathStrokeType(1.6f));
            // the drop to the keys, where the picture ends
            const float lip = mmY(D::keysY) - 5.0f;
            g.setGradientFill(juce::ColourGradient(black(0.0f), 0.0f, lip, black(0.7f), 0.0f, lip + 5.0f, false));
            g.fillRect(body.withTop(lip));
        }

        // ── recesses: the two strips and the display window ──
        drawWell(g, mmRect(D::STRIP_PITCH), 2.0f * PX_PER_MM, true);
        drawWell(g, mmRect(D::STRIP_MOD), 2.0f * PX_PER_MM, true);
        drawWell(g, displayArea(), CAP_RADIUS, false);

        // ── the overlay: six pieces of printed film lying on the body ──
        const PanelOverlay overlayGeometry(g, PX_PER_MM, (float) MARGIN, (float) MARGIN);
        for (int piece = 0; piece < PanelOverlay::NUM_PIECES; ++piece)
        {
            auto p = PanelOverlay::piecePath(piece);
            p.applyTransform(overlayGeometry.toPixels());
            juce::DropShadow(black(0.55f), 3, { 0, 1 }).drawForPath(g, p);
            g.setColour(PanelOverlay::PANEL);
            g.fillPath(p);
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(p);
            g.setTiledImageFill(grainTile(), 0, 0, 0.45f);
            g.fillRect(p.getBounds());
            // the cut edge of the film: lit above, dark below
            const auto b = p.getBounds();
            g.setGradientFill(juce::ColourGradient(white(0.20f), 0.0f, b.getY(), white(0.05f), 0.0f, b.getBottom(), false));
            g.strokePath(p, juce::PathStrokeType(1.2f));
        }
        PanelOverlay(g, PX_PER_MM, (float) MARGIN, (float) MARGIN).drawArt();

        // ── what the controls throw onto the panel, and the gaps they sit in ──
        for (const auto& el : elements)
        {
            if (el.type == ElemType::Button || el.type == ElemType::Pad)
            {
                juce::Path cap;
                cap.addRoundedRectangle(el.bounds, CAP_RADIUS);
                juce::DropShadow(black(0.55f), el.type == ElemType::Pad ? 6 : 5, { 0, 2 }).drawForPath(g, cap);
                g.setColour(black(0.85f));
                g.fillRoundedRectangle(el.bounds.expanded(1.0f), CAP_RADIUS + 1.0f);
                // the lower lip of the opening catches a little light
                g.setGradientFill(juce::ColourGradient(white(0.0f), 0.0f, el.bounds.getCentreY(), white(0.10f), 0.0f, el.bounds.getBottom() + 1.5f, false));
                g.drawRoundedRectangle(el.bounds.expanded(1.5f), CAP_RADIUS + 1.5f, 0.8f);
            }
        }
        for (int i = 0; i < 8; ++i)
            drawKnobBody(g, { mmX(D::encCx(i)), mmY(D::encCy) }, (float) D::encR * PX_PER_MM);
    }

    // A tile of faint light and dark specks.
    static const juce::Image& grainTile()
    {
        static const juce::Image tile = []
        {
            juce::Image img(juce::Image::ARGB, 128, 128, true);
            juce::Random random(20261111);
            juce::Image::BitmapData data(img, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < 128; ++y)
                for (int x = 0; x < 128; ++x)
                {
                    const float v = random.nextFloat();
                    data.setPixelColour(x, y, v < 0.5f ? black(0.10f * random.nextFloat()) : white(0.035f * random.nextFloat()));
                }
            return img;
        }();
        return tile;
    }

    // A recess with a glossy black floor: dark under its upper lip, a line of light on its lower one.
    static void drawWell(juce::Graphics& g, juce::Rectangle<float> r, float radius, bool withGloss)
    {
        // the chamfer round the opening
        g.setGradientFill(juce::ColourGradient(black(0.75f), 0.0f, r.getY() - 2.0f, white(0.16f), 0.0f, r.getBottom() + 2.0f, false));
        g.fillRoundedRectangle(r.expanded(1.6f), radius + 1.6f);
        fillVertical(g, r, radius, juce::Colour(0xFF050506), juce::Colour(0xFF101013));

        juce::Path floor;
        floor.addRoundedRectangle(r, radius);
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(floor);
        // the shadow of the upper lip and of the two sides
        g.setGradientFill(juce::ColourGradient(black(0.85f), 0.0f, r.getY(), black(0.0f), 0.0f, r.getY() + 9.0f, false));
        g.fillRect(r.withHeight(9.0f));
        g.setGradientFill(juce::ColourGradient(black(0.5f), r.getX(), 0.0f, black(0.0f), r.getX() + 4.0f, 0.0f, false));
        g.fillRect(r.withWidth(4.0f));
        if (withGloss)
            drawGloss(g, r);
    }

    // The reflection of a soft light in black glass: one faint slanting band that fades out along its length.
    // The caller clips to the glass.
    static void drawGloss(juce::Graphics& g, juce::Rectangle<float> r)
    {
        const float w = r.getWidth(), h = r.getHeight();
        const float run = juce::jmin(h, w * 1.6f) * 0.9f;          // how far down the band reaches
        const float lean = run * 0.55f;                            // and how far it leans to the left on the way
        const float x0 = r.getX() + w * 0.52f, x1 = r.getRight() + w * 0.25f;
        juce::Path band;
        band.startNewSubPath(x0, r.getY());
        band.lineTo(x1, r.getY());
        band.lineTo(x1 - lean, r.getY() + run);
        band.lineTo(x0 - lean, r.getY() + run);
        band.closeSubPath();
        g.setGradientFill(juce::ColourGradient(white(0.060f), 0.0f, r.getY(), white(0.0f), 0.0f, r.getY() + run, false));
        g.fillPath(band);
    }

    // A knob from above: its shadow, the knurled flank, the flat top with a lit rim. Its mark is drawn live.
    static void drawKnobBody(juce::Graphics& g, juce::Point<float> c, float radius)
    {
        juce::Path foot;
        foot.addEllipse(c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f);
        juce::DropShadow(black(0.70f), 10, { 0, 6 }).drawForPath(g, foot);
        juce::DropShadow(black(0.60f), 3, { 0, 1 }).drawForPath(g, foot);

        // the flank: lit from above, in shade below
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xFF4A4B50), 0.0f, c.y - radius, juce::Colour(0xFF0E0F10), 0.0f, c.y + radius, false));
        g.fillPath(foot);
        const int ridges = 44;
        for (int i = 0; i < ridges; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / (float) ridges;
            const float dx = std::sin(a), dy = -std::cos(a);
            g.setColour(black(0.42f));
            g.drawLine(c.x + dx * radius * 0.80f, c.y + dy * radius * 0.80f, c.x + dx * radius * 0.985f, c.y + dy * radius * 0.985f, 1.3f);
        }
        g.setColour(black(0.55f));
        g.drawEllipse(c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f, 0.8f);

        // the top
        const float top = radius * 0.80f;
        const juce::Rectangle<float> face(c.x - top, c.y - top, top * 2.0f, top * 2.0f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xFF37383C), 0.0f, face.getY(), juce::Colour(0xFF232427), 0.0f, face.getBottom(), false));
        g.fillEllipse(face);
        g.setGradientFill(juce::ColourGradient(black(0.0f), c.x, c.y, black(0.16f), c.x, c.y + top, true));
        g.fillEllipse(face);
        g.setGradientFill(juce::ColourGradient(white(0.42f), 0.0f, face.getY(), white(0.02f), 0.0f, face.getCentreY() + top * 0.3f, false));
        g.drawEllipse(face.reduced(0.5f), 1.0f);
    }

    void drawEncoderMark(juce::Graphics& g, const Element& el, bool active) const
    {
        const int encIdx = el.data1 - 0x15;
        const auto c = el.bounds.getCentre();
        const float radius = (float) PanelDim::encR * PX_PER_MM;
        const float angle = juce::degreesToRadians(9.0f * (float) (encIdx >= 0 && encIdx < 8 ? encoderSteps[encIdx] : 0));
        const float dx = std::sin(angle), dy = -std::cos(angle);
        juce::Path mark;
        mark.startNewSubPath(c.x + dx * radius * 0.34f, c.y + dy * radius * 0.34f);
        mark.lineTo(c.x + dx * radius * 0.68f, c.y + dy * radius * 0.68f);
        g.setColour(black(0.5f));
        g.strokePath(mark, juce::PathStrokeType(3.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(juce::Colour(0xFFE9E7E1).withAlpha(active ? 1.0f : 0.82f));
        g.strokePath(mark, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (active)
        {
            g.setColour(white(0.10f));
            g.drawEllipse(c.x - radius * 0.80f, c.y - radius * 0.80f, radius * 1.6f, radius * 1.6f, 1.2f);
        }
    }

    // A low rubber cap: a faint bevel, sunk a little when down; its backlight shows through while it is held.
    void drawButton(juce::Graphics& g, const Element& el, int idx, bool down, bool held, bool hovered) const
    {
        auto b = el.bounds;
        if (down)
            b = b.reduced(0.6f).translated(0.0f, 0.9f);
        juce::Colour top(0xFF3A3B40), bottom(0xFF2A2B2F);
        if (down)         { top = top.darker(0.25f); bottom = bottom.darker(0.15f); }
        else if (hovered) { top = top.brighter(0.07f); bottom = bottom.brighter(0.07f); }
        fillVertical(g, b, CAP_RADIUS, top, bottom);

        if (held)
        {
            // light from under the cap, in the overlay's colour for that modifier
            const juce::Colour light = idx == shiftIdx ? PanelOverlay::SHIFT : idx == funcIdx ? PanelOverlay::FUNC : juce::Colour(0xFFF4F1EA);
            juce::Graphics::ScopedSaveState save(g);
            juce::Path cap;
            cap.addRoundedRectangle(b, CAP_RADIUS);
            g.reduceClipRegion(cap);
            const float reach = juce::jmax(b.getWidth(), b.getHeight()) * 0.62f;
            juce::ColourGradient glow(light.withAlpha(0.80f), b.getCentreX(), b.getCentreY(), light.withAlpha(0.10f), b.getCentreX() + reach, b.getCentreY(), true);
            glow.addColour(0.45, light.withAlpha(0.42f));
            g.setGradientFill(glow);
            g.fillRect(b);
            g.setColour(light.withAlpha(0.55f));
            g.drawRoundedRectangle(b.reduced(0.6f), CAP_RADIUS, 1.0f);
        }

        if (down)
        {
            juce::Graphics::ScopedSaveState save(g);
            juce::Path cap;
            cap.addRoundedRectangle(b, CAP_RADIUS);
            g.reduceClipRegion(cap);
            g.setGradientFill(juce::ColourGradient(black(0.40f), 0.0f, b.getY(), black(0.0f), 0.0f, b.getY() + 7.0f, false));
            g.fillRect(b.withHeight(7.0f));
        }
        else
        {
            g.setGradientFill(juce::ColourGradient(white(0.22f), 0.0f, b.getY(), white(0.0f), 0.0f, b.getY() + b.getHeight() * 0.55f, false));
            g.drawRoundedRectangle(b.reduced(0.5f), CAP_RADIUS, 1.0f);
            g.setGradientFill(juce::ColourGradient(black(0.0f), 0.0f, b.getCentreY(), black(0.30f), 0.0f, b.getBottom(), false));
            g.drawRoundedRectangle(b.reduced(0.5f), CAP_RADIUS, 1.0f);
        }
    }

    // A pad of milky silicone over an LED: unlit it is grey rubber; lit, the colour comes up through it,
    // brightest over the lamp, and spills a little onto the panel. Pressed, it sinks.
    void drawPad(juce::Graphics& g, const Element& el, bool down, bool held, bool hovered) const
    {
        int padIdx = -1;
        if (el.data1 >= PAD_TOP_NOTE && el.data1 < PAD_TOP_NOTE + 8) padIdx = el.data1 - PAD_TOP_NOTE;
        else if (el.data1 >= PAD_BOTTOM_NOTE && el.data1 < PAD_BOTTOM_NOTE + 8) padIdx = 8 + (el.data1 - PAD_BOTTOM_NOTE);
        const PadColor pc = padIdx >= 0 ? padColors[padIdx] : PadColor {};

        const int peak = juce::jmax(pc.r, pc.g, pc.b);
        const float level = (float) peak / 255.0f;
        const float glow = peak > 0 ? std::pow(level, 0.68f) : 0.0f;          // dim LEDs still read as lit
        const juce::Colour hue = peak > 0 ? juce::Colour((juce::uint8) (pc.r * 255 / peak), (juce::uint8) (pc.g * 255 / peak), (juce::uint8) (pc.b * 255 / peak))
                                          : juce::Colours::black;

        auto b = el.bounds;
        if (glow > 0.02f && ! down)
            for (int i = 1; i <= 4; ++i)
            {
                g.setColour(hue.withAlpha(glow * 0.20f / (float) i));
                g.drawRoundedRectangle(b.expanded((float) i * 1.7f - 0.6f), CAP_RADIUS + (float) i * 1.7f, 1.9f);
            }
        if (down)
            b = b.reduced(1.0f).translated(0.0f, 1.0f);

        juce::Colour top(0xFF5B5C61), bottom(0xFF44454A);
        if (down)         { top = top.darker(0.22f); bottom = bottom.darker(0.12f); }
        else if (hovered) { top = top.brighter(0.06f); bottom = bottom.brighter(0.06f); }
        fillVertical(g, b, CAP_RADIUS, top, bottom);

        juce::Path cap;
        cap.addRoundedRectangle(b, CAP_RADIUS);
        {
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(cap);
            if (glow > 0.0f)
            {
                const float boost = down ? 1.0f : 0.88f;
                const float reach = b.getWidth() * 0.78f;
                juce::ColourGradient lamp(hue.interpolatedWith(juce::Colours::white, 0.38f * glow).withAlpha(glow * boost),
                                          b.getCentreX(), b.getCentreY(),
                                          hue.withMultipliedBrightness(0.70f).withAlpha(glow * boost * 0.80f),
                                          b.getCentreX() + reach, b.getCentreY(), true);
                lamp.addColour(0.55, hue.withAlpha(glow * boost * 0.92f));
                g.setGradientFill(lamp);
                g.fillRect(b);
            }
            // a soft sheen on the far half of the surface, and shade toward the near edge
            g.setGradientFill(juce::ColourGradient(white(down ? 0.02f : 0.10f), 0.0f, b.getY(), white(0.0f), 0.0f, b.getY() + b.getHeight() * 0.5f, false));
            g.fillRect(b.withHeight(b.getHeight() * 0.5f));
            g.setGradientFill(juce::ColourGradient(black(0.0f), 0.0f, b.getCentreY(), black(0.20f), 0.0f, b.getBottom(), false));
            g.fillRect(b.withTop(b.getCentreY()));
            if (down)
            {
                g.setGradientFill(juce::ColourGradient(black(0.50f), 0.0f, b.getY(), black(0.0f), 0.0f, b.getY() + 11.0f, false));
                g.fillRect(b.withHeight(11.0f));
                g.setGradientFill(juce::ColourGradient(black(0.30f), b.getX(), 0.0f, black(0.0f), b.getX() + 6.0f, 0.0f, false));
                g.fillRect(b.withWidth(6.0f));
            }
        }
        if (! down)
        {
            // the rounded edge of the rubber
            g.setGradientFill(juce::ColourGradient(white(0.36f), 0.0f, b.getY(), white(0.0f), 0.0f, b.getY() + b.getHeight() * 0.6f, false));
            g.drawRoundedRectangle(b.reduced(0.6f), CAP_RADIUS, 1.2f);
        }
        g.setGradientFill(juce::ColourGradient(black(0.0f), 0.0f, b.getCentreY(), black(0.38f), 0.0f, b.getBottom(), false));
        g.drawRoundedRectangle(b.reduced(0.6f), CAP_RADIUS, 1.2f);
        if (held)
        {
            g.setColour(white(0.75f));
            g.drawRoundedRectangle(b.reduced(1.6f), CAP_RADIUS - 1.0f, 1.2f);
        }
    }

    // Where the finger is: the pitch strip rests in the middle, the mod strip where it was left.
    void drawStripMark(juce::Graphics& g, const Element& el, int idx, bool active) const
    {
        const auto b = el.bounds;
        const bool isPitch = idx == pitchStripIdx;
        const float pos = isPitch ? pitchPosition : modPosition;
        const float inset = 7.0f;
        const float y = b.getBottom() - inset - pos * (b.getHeight() - 2.0f * inset);
        const bool resting = isPitch && std::abs(pos - 0.5f) < 0.001f && ! active;
        const juce::Colour light(0xFFDCE9F5);
        const float strength = resting ? 0.35f : (active ? 1.0f : 0.8f);
        for (int i = 3; i >= 1; --i)
        {
            g.setColour(light.withAlpha(strength * 0.09f));
            g.fillRoundedRectangle(b.getX() + 6.0f - (float) i, y - 1.5f - (float) i * 1.6f, b.getWidth() - 12.0f + 2.0f * (float) i, 3.0f + (float) i * 3.2f, 3.0f);
        }
        g.setColour(light.withAlpha(strength * 0.95f));
        g.fillRoundedRectangle(b.getX() + 7.0f, y - 1.25f, b.getWidth() - 14.0f, 2.5f, 1.25f);
    }

    // The display window: the live picture at one pixel per pixel behind the glass, then the reflection.
    void drawDisplay(juce::Graphics& g) const
    {
        const auto r = displayArea();
        if (display != nullptr)
        {
            const float x0 = std::round(r.getCentreX() - DisplayRenderer::WIDTH * 0.5f);
            const float y0 = std::round(r.getCentreY() - DisplayRenderer::HEIGHT * 0.5f);
            g.setColour(juce::Colour(0xFFCCDDEE).withAlpha(0.92f));
            for (int y = 0; y < DisplayRenderer::HEIGHT; ++y)
            {
                int runStart = -1;
                for (int x = 0; x <= DisplayRenderer::WIDTH; ++x)
                {
                    const bool on = x < DisplayRenderer::WIDTH && shownPixels[(size_t) y][(size_t) x];
                    if (on && runStart < 0)
                        runStart = x;
                    else if (! on && runStart >= 0)
                    {
                        g.fillRect(x0 + (float) runStart, y0 + (float) y, (float) (x - runStart), 1.0f);
                        runStart = -1;
                    }
                }
            }
        }
        juce::Path glass;
        glass.addRoundedRectangle(r, CAP_RADIUS);
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(glass);
        drawGloss(g, r);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VirtualControllerComponent)
};
