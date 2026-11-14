#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include "PanelDimensions.h"

// The printed overlay ("skin"), drawn on the on-screen panel: the six pieces of docs/overlay/overlay.html
// in their places, with the same words, colours and marks. This is a port of that file's script (the
// PIECES and ART sections): every label, size and position is the script's, in millimetres. Change the
// overlay there first, then here; the "Panel Dimensions" test checks the numbers and the words.
//   white = the control alone; amber with a diamond = with Shift; blue with a ring = with Func.
class PanelOverlay
{
public:
    static inline const juce::Colour INK   { 0xFFF4F1EA };
    static inline const juce::Colour SHIFT { 0xFFFFB000 };
    static inline const juce::Colour FUNC  { 0xFF55C6F5 };
    static inline const juce::Colour DIM   { 0xFF8B8E92 };
    static inline const juce::Colour PANEL { 0xFF131416 };

    enum class Anchor { Start, Middle, End };
    enum class Face { Label, Number };

    // Millimetres on the panel to pixels in the component: x' = ox + x * scale.
    PanelOverlay(juce::Graphics& graphics, float pxPerMm, float originX, float originY, juce::StringArray* wordSink = nullptr)
        : g(graphics), s(pxPerMm), ox(originX), oy(originY), words(wordSink) {}

    // The six pieces as outlines in millimetres; the knob plate has its eight holes.
    static juce::Path piecePath(int piece)
    {
        namespace D = PanelDim;
        juce::Path p;
        auto box = [&p](double x0, double y0, double x1, double y1)
        { p.addRectangle((float) x0, (float) y0, (float) (x1 - x0), (float) (y1 - y0)); };
        switch (piece)
        {
            case 0:   // K
                box(D::kX0, D::kY0, D::kX1, D::kY1);
                for (int i = 0; i < 8; ++i)
                    p.addEllipse((float) (D::encCx(i) - D::encClear), (float) (D::encCy - D::encClear),
                                 (float) (2 * D::encClear), (float) (2 * D::encClear));
                p.setUsingNonZeroWinding(false);
                break;
            case 1: box(D::fX0, D::fY0, D::fSplitX, D::fY1); break;       // F1
            case 2: box(D::fSplitX, D::fY0, D::fX1, D::fY1); break;       // F2
            case 3: box(D::rX0, D::rY0, D::rX1, D::rY1); break;           // R
            case 4:                                                       // T
                p.startNewSubPath((float) D::tSpineX0, (float) D::tSpineY0);
                p.lineTo((float) D::tSpineX1, (float) D::tSpineY0);
                p.lineTo((float) D::tSpineX1, (float) D::tBarY0);
                p.lineTo((float) D::tBarX1, (float) D::tBarY0);
                p.lineTo((float) D::tBarX1, (float) D::tBarY1);
                p.lineTo((float) D::tBarX0, (float) D::tBarY1);
                p.lineTo((float) D::tBarX0, (float) D::tBarY0);
                p.lineTo((float) D::tSpineX0, (float) D::tBarY0);
                p.closeSubPath();
                break;
            case 5: box(D::mX0, D::mY0, D::mX1, D::mY1); break;           // M
            default: break;
        }
        return p;
    }
    static constexpr int NUM_PIECES = 6;

    juce::AffineTransform toPixels() const { return juce::AffineTransform::scale(s).translated(ox, oy); }

    // Everything printed on the six pieces.
    void drawArt()
    {
        artK(); artF2(); artR(); artT(); artM(); artF1();
    }

private:
    juce::Graphics& g;
    float s, ox, oy;
    juce::StringArray* words;

    static constexpr double SW1 = 0.35;                       // one stroke weight for every drawn line
    static constexpr double BIG = 3.3, LAB = 2.9, SUB = 2.6;  // label sizes in mm
    static constexpr double PI = 3.14159265358979323846;

    float X(double mm) const { return ox + (float) mm * s; }
    float Y(double mm) const { return oy + (float) mm * s; }

    // ---- the script's drawing helpers (L, P, F, R, FR, CI, DOT, TX, WD) ----
    void line(double x1, double y1, double x2, double y2, juce::Colour c, double w, bool round = true)
    {
        juce::Path p;
        p.startNewSubPath(X(x1), Y(y1));
        p.lineTo(X(x2), Y(y2));
        g.setColour(c);
        g.strokePath(p, juce::PathStrokeType((float) w * s, juce::PathStrokeType::curved,
                                             round ? juce::PathStrokeType::rounded : juce::PathStrokeType::butt));
    }
    void strokeMm(const juce::Path& mmPath, juce::Colour c, double w)
    {
        juce::Path p(mmPath);
        p.applyTransform(toPixels());
        g.setColour(c);
        g.strokePath(p, juce::PathStrokeType((float) w * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    void fillMm(const juce::Path& mmPath, juce::Colour c)
    {
        juce::Path p(mmPath);
        p.applyTransform(toPixels());
        g.setColour(c);
        g.fillPath(p);
    }
    void fillRect(double x, double y, double w, double h, juce::Colour c, double r = 0)
    {
        g.setColour(c);
        g.fillRoundedRectangle(X(x), Y(y), (float) w * s, (float) h * s, (float) r * s);
    }
    void strokeRect(double x, double y, double w, double h, double r, juce::Colour c, double sw)
    {
        g.setColour(c);
        g.drawRoundedRectangle(X(x), Y(y), (float) w * s, (float) h * s, (float) r * s, (float) sw * s);
    }
    void ring(double x, double y, double r, juce::Colour c, double sw)
    {
        g.setColour(c);
        g.drawEllipse(X(x - r), Y(y - r), (float) (2 * r) * s, (float) (2 * r) * s, (float) sw * s);
    }
    void dot(double x, double y, double r, juce::Colour c)
    {
        g.setColour(c);
        g.fillEllipse(X(x - r), Y(y - r), (float) (2 * r) * s, (float) (2 * r) * s);
    }

    static juce::String faceName(Face face)
    {
        // The overlay's own font stacks; the first one installed is used.
        static const juce::StringArray installed = juce::Font::findAllTypefaceNames();
        static const auto pick = [](std::initializer_list<const char*> names)
        {
            for (auto* n : names)
                if (installed.contains(n))
                    return juce::String(n);
            return juce::Font::getDefaultSansSerifFontName();
        };
        static const juce::String label = pick({ "DIN Condensed", "Avenir Next Condensed", "Arial Narrow" });
        static const juce::String number = pick({ "DIN Alternate", "Helvetica Neue", "Arial" });
        return face == Face::Label ? label : number;
    }

    // y is the baseline; `sp` is the letter spacing in mm; `quarterTurn` stands the text upright (reading upward).
    void text(double x, double y, const juce::String& str, juce::Colour c, double size, Anchor anchor = Anchor::Middle,
              Face face = Face::Label, double sp = 0.14, bool quarterTurn = false)
    {
        const juce::Font font(juce::FontOptions(faceName(face), "Bold", 12.0f)
                                  .withPointHeight((float) size * s)
                                  .withKerningFactor((float) (sp / size)));
        const float w = juce::GlyphArrangement::getStringWidth(font, str);
        juce::GlyphArrangement ga;
        ga.addLineOfText(font, str, 0.0f, 0.0f);
        const float dx = anchor == Anchor::Start ? 0.0f : anchor == Anchor::Middle ? -w * 0.5f : -w;
        auto t = juce::AffineTransform::translation(dx, 0.0f);
        if (quarterTurn)
            t = t.rotated((float) (-PI / 2));
        g.setColour(c);
        ga.draw(g, t.translated(X(x), Y(y)));
    }
    // a word on the skin: counted, as the overlay counts it
    void word(double x, double y, const juce::String& str, juce::Colour c, double size, Anchor anchor = Anchor::Middle,
              double sp = 0.14, bool quarterTurn = false)
    {
        if (words != nullptr)
            words->addTokens(str, " ", {});
        text(x, y, str, c, size, anchor, Face::Label, sp, quarterTurn);
    }

    // ---- the icon vocabulary ----
    void iShift(double x, double y, double r = 1.0)
    {
        juce::Path p;
        p.startNewSubPath((float) x, (float) (y - r));
        p.lineTo((float) (x + r), (float) y);
        p.lineTo((float) x, (float) (y + r));
        p.lineTo((float) (x - r), (float) y);
        p.closeSubPath();
        fillMm(p, SHIFT);
    }
    void iFunc(double x, double y, double r = 1.0) { ring(x, y, r * 0.78, FUNC, r * 0.44); }
    void iPlay(double x, double y, double sz, juce::Colour c)
    {
        juce::Path p;
        p.addTriangle((float) (x - sz * 0.36), (float) (y - sz * 0.44), (float) (x + sz * 0.46), (float) y,
                      (float) (x - sz * 0.36), (float) (y + sz * 0.44));
        fillMm(p, c);
    }
    void iRec(double x, double y, double sz, juce::Colour c) { dot(x, y, sz * 0.42, c); }
    // undo (dir -1) and redo (dir +1): three quarters of a circle over the top, ending in an arrow head
    void iUndo(double x, double y, double sz, juce::Colour c, int dir = -1)
    {
        const double r = sz * 0.34, bx = x + dir * r, by = y;
        // The script's arc runs from 55 degrees below the horizontal, over the top, to the far side.
        const double from = dir < 0 ? 145.0 : 215.0, to = dir < 0 ? -90.0 : 450.0;   // clockwise from 12 o'clock
        juce::Path arc;
        arc.addCentredArc((float) x, (float) y, (float) r, (float) r, 0.0f,
                          (float) (from * PI / 180.0), (float) (to * PI / 180.0), true);
        strokeMm(arc, c, SW1);
        juce::Path head;
        head.addTriangle((float) (bx - sz * 0.2), (float) (by - sz * 0.02), (float) (bx + sz * 0.2), (float) (by - sz * 0.02),
                         (float) bx, (float) (by + sz * 0.28));
        fillMm(head, c);
    }
    void iChev(double x, double y, double sz, int dir, juce::Colour c)
    {
        juce::Path p;
        p.startNewSubPath((float) (x - dir * sz * 0.2), (float) (y - sz * 0.4));
        p.lineTo((float) (x + dir * sz * 0.2), (float) y);
        p.lineTo((float) (x - dir * sz * 0.2), (float) (y + sz * 0.4));
        strokeMm(p, c, SW1);
    }
    // the two pad rows as two bars: the one meant is solid
    void iRows(double x, double y, double w, bool top, juce::Colour c)
    {
        const double h = 1.25, gap = 0.7;
        const double ys[2] = { y - gap / 2 - h, y + gap / 2 };
        for (int row = 0; row < 2; ++row)
        {
            if ((row == 0) == top)
                fillRect(x - w / 2, ys[row], w, h, c, 0.2);
            else
                strokeRect(x - w / 2 + SW1 / 2, ys[row] + SW1 / 2, w - SW1, h - SW1, 0.1, DIM, SW1);
        }
    }

    // ---- words ----
    // Screens reached by Shift + pad: the display's short name -> what the skin prints.
    static const char* screenWord(const char* name)
    {
        struct Pair { const char* key; const char* value; };
        static constexpr Pair WORD[] = {
            { "PARAM", "PARAMS" }, { "TRACKS", "TRACKS" }, { "TRK FX", "TRACK FX" }, { "STEP", "STEP" },
            { "PRESET", "PRESET" }, { "SONG", "SONG" }, { "SETTINGS", "SETTINGS" }, { "AT ROUTE", "AT ROUTE" },
            { "DRUMS", "DRUMS" }, { "ARP", "ARP" }, { "CHORD", "CHORDS" }, { "SCALE", "SCALE" }, { "WAVE", "WAVEFORM" }, { "FLTR", "FILTER" } };
        for (const auto& w : WORD)
            if (juce::String(w.key) == name)
                return w.value;
        return name;
    }
    static constexpr const char* SHIFT_TOP[8] = { "PARAM", "TRACKS", "TRK FX", "STEP", "PRESET", "SONG", "SETTINGS", "AT ROUTE" };
    static constexpr const char* SHIFT_BOT[8] = { "STEP", "DRUMS", "CHORD", "SCALE", "WAVE", "FLTR", nullptr, nullptr };

    void padName(double x, double yMid, const char* name) { word(x, yMid + 1.17, screenWord(name), SHIFT, BIG, Anchor::Middle, 0.2); }

    // the pad rows as a ruler of sixteen steps: a long mark on every beat
    void beatRuler(double y, int dir, int first)
    {
        namespace D = PanelDim;
        const double xe = D::padX0 + 7 * D::padPitch + D::padW;
        line(D::padX0, y, xe, y, INK, SW1, false);
        for (int i = 0; i < 8; i += 4)
        {
            const double x = D::padX0 + i * D::padPitch + 0.3;
            line(x, y, x, y + dir * 1.5, INK, 0.6, false);
            text(x + 0.9, dir < 0 ? y - 0.45 : y + 2.15, juce::String(first + i), INK, 2.4, Anchor::Start, Face::Number, 0.0);
        }
    }

    // ---- K: knobs 1-4 = the top row of cells, 5-8 = the bottom row; Shift + top pad = a screen ----
    void artK()
    {
        namespace D = PanelDim;
        const double r = 8.0, yT = D::encCy - r, yB = D::encCy + r, gx = (D::encCx(3) + D::encCx(4)) / 2;
        const double cw = 1.7, ch = 1.9, gap = 0.4, gw = 4 * cw + 3 * gap, gx0 = gx - gw / 2, gyT = 26.3, gyB = gyT + ch + 0.6;
        const float fr = (float) r, cy = (float) D::encCy, pi = (float) PI;
        {
            juce::Path p;
            p.startNewSubPath((float) gx0, (float) (gyT + ch / 2));
            p.lineTo((float) (gx0 - 0.85), (float) (gyT + ch / 2));
            p.lineTo((float) (gx0 - 0.85), (float) yB);
            p.lineTo((float) D::encCx(0), (float) yB);
            p.addCentredArc((float) D::encCx(0), cy, fr, fr, 0.0f, pi, 2.0f * pi, false);
            p.lineTo((float) D::encCx(3), (float) yT);
            p.addCentredArc((float) D::encCx(3), cy, fr, fr, 0.0f, 0.0f, pi, false);
            strokeMm(p, INK, SW1);
        }
        {
            juce::Path p;
            p.startNewSubPath((float) (gx0 + gw), (float) (gyB + ch / 2));
            p.lineTo((float) (gx0 + gw + 0.85), (float) (gyB + ch / 2));
            p.lineTo((float) (gx0 + gw + 0.85), (float) yB);
            p.lineTo((float) D::encCx(7), (float) yB);
            p.addCentredArc((float) D::encCx(7), cy, fr, fr, 0.0f, pi, 0.0f, false);
            p.lineTo((float) D::encCx(4), (float) yT);
            p.addCentredArc((float) D::encCx(4), cy, fr, fr, 0.0f, 0.0f, -pi, false);
            strokeMm(p, INK, SW1);
        }
        for (int i = 0; i < 4; ++i)
        {
            fillRect(gx0 + i * (cw + gap), gyT, cw, ch, INK, 0.25);
            fillRect(gx0 + i * (cw + gap), gyB, cw, ch, INK, 0.25);
        }
        iShift(D::kX0 + 2.6, 29.2, 1.3);
        for (int i = 0; i < 8; ++i)
            padName(D::padCx(i), 29.2, SHIFT_TOP[i]);
        beatRuler(34.05, -1, 1);
    }

    // ---- F2: Shift + bottom pad = a screen ----
    void artF2()
    {
        namespace D = PanelDim;
        beatRuler(78.25, 1, 9);
        iShift(D::kX0 + 2.6, 86.2, 1.3);
        for (int i = 0; i < 8; ++i)
            if (SHIFT_BOT[i] != nullptr)
                padName(D::padCx(i), 86.2, SHIFT_BOT[i]);
    }

    // ---- R: knob pages, the pattern key, Func ----
    void artR()
    {
        namespace D = PanelDim;
        const double yu = D::BTN_BANK_UP.cy(), yd = D::BTN_BANK_DN.cy(), xl = D::rX0 + 2.2, ym = (yu + yd) / 2;
        iChev(xl + 1.0, yu, 3.4, -1, INK);
        iChev(xl + 1.0, yd, 3.4, 1, INK);
        word(xl + 3.4, ym - 1.0, "KNOB PAGE", INK, LAB, Anchor::Start);
        iFunc(xl + 4.3, ym + 2.1, 1.0);
        word(xl + 6.4, ym + 3.15, "JUMP", FUNC, LAB, Anchor::Start);
        const double yp = D::BTN_PATTERN.cy();
        word(xl, yp - 1.6, "PATTERN", INK, LAB, Anchor::Start);
        iFunc(xl + 0.9, yp + 3.4, 1.0);
        word(xl + 3.0, yp + 4.45, "PASTE", FUNC, LAB, Anchor::Start);
        const double yf = D::BTN_FUNC.cy();
        iFunc(xl + 1.6, yf - 4.6, 2.0);
        word(xl, yf + 1.6, "COARSE", FUNC, LAB, Anchor::Start);
        iRows(xl + 2.4, yf + 5.6, 4.8, true, FUNC);
        word(xl + 6.2, yf + 6.65, "PAGE", FUNC, LAB, Anchor::Start);
    }

    // ---- T: Shift, Settings, Up (bar); Arp and Scale (upright) ----
    void artT()
    {
        namespace D = PanelDim;
        const double y = (D::tBarY0 + D::tBarY1) / 2, xs = D::BTN_SHIFT.cx(), xt = D::BTN_SETTINGS.cx(), xu = D::BTN_UP.cx();
        const double sx = (D::tSpineX0 + D::tSpineX1) / 2;
        iShift(xs - 4.4, y, 1.6);
        word(xs - 1.8, y + 1.05, "FINE", SHIFT, LAB, Anchor::Start);
        iFunc(xt - 7.6, y, 1.0);
        word(xt - 5.6, y + 1.05, "TAP TEMPO", FUNC, LAB, Anchor::Start);
        iShift(xu - 4.6, y, 1.0);
        word(xu - 3.0, y + 1.05, "TRACK", SHIFT, LAB, Anchor::Start);
        const double ya = D::BTN_ARP.cy(), yc = D::BTN_SCALE.cy();
        iShift(sx, D::tSpineY0 + 1.9, 1.0);
        word(sx + 1.05, ya + 1.3, "LATCH", SHIFT, LAB, Anchor::Middle, 0.14, true);
        word(sx + 1.05, yc + 0.6, "SCREEN", SHIFT, LAB, Anchor::Middle, 0.14, true);
    }

    // ---- M: Play and Record with Shift, with Func, with both ----
    void artM()
    {
        namespace D = PanelDim;
        const double xm = D::mX0 + 1.3, xw = D::mX0 + 3.2;
        double y = D::mY0 + 3.4;
        enum Mark { None, WithShift, WithFunc };
        auto row = [&](Mark mark, const char* str, juce::Colour c)
        {
            y += 3.75;
            if (mark == WithShift) iShift(xm + 0.3, y - 0.95, 0.8);
            if (mark == WithFunc)  iFunc(xm + 0.3, y - 0.95, 0.8);
            word(xw, y, str, c, SUB, Anchor::Start, 0.1);
        };
        iPlay(xm + 1.0, y, 3.2, INK);
        row(WithShift, "REWIND", SHIFT);
        row(None, "SILENCE", SHIFT);
        row(WithFunc, "CLEAR", FUNC);
        y += 3.9;
        iShift(xm + 0.3, y - 0.9, 0.85); iFunc(xm + 2.5, y - 0.9, 0.85); iUndo(xm + 6.3, y - 0.9, 4.0, FUNC, -1);
        y += 5.2;
        iRec(xm + 1.0, y, 3.2, INK);
        row(WithShift, "FILL", SHIFT);
        row(WithFunc, "COPY", FUNC);
        y += 3.9;
        iShift(xm + 0.3, y - 0.9, 0.85); iFunc(xm + 2.5, y - 0.9, 0.85); iUndo(xm + 6.3, y - 0.9, 4.0, FUNC, 1);
    }

    // ---- F1: the key split, Up / Down ----
    void artF1()
    {
        namespace D = PanelDim;
        const double yc = (D::fY0 + D::fY1) / 2;
        word(D::fX0 + 3.0, yc + 1.0, "NEPTUNE", DIM, SUB, Anchor::Start, 0.6);
        const double xo = (D::BTN_OCT_DN.x + D::BTN_OCT_UP.x + D::BTN_OCT_UP.w) / 2;
        iShift(xo - 7.2, yc, 1.3);
        word(xo - 5.0, yc + 1.05, "KEY SPLIT", SHIFT, LAB, Anchor::Start);
        // Up / Down tapped: the page of whatever the pads show. Held: top row = pick a track, bottom row = mute
        const double xd = D::BTN_DOWN.x;
        word(xd + 0.3, 82.3, "PAD PAGE", INK, LAB, Anchor::Start);
        word(xd - 1.0, 89.2, "HOLD", DIM, SUB, Anchor::End, 0.1);
        iRows(xd + 2.7, 86.5, 4.8, true, INK);
        word(xd + 6.0, 87.5, "TRACK", INK, SUB, Anchor::Start, 0.1);
        iRows(xd + 2.7, 90.4, 4.8, false, INK);
        word(xd + 6.0, 91.4, "MUTE", INK, SUB, Anchor::Start, 0.1);
    }
};
