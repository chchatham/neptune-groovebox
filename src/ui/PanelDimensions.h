#pragma once

#include <cstddef>

// The measured geometry of the controller's panel, in millimetres from the top-left corner of the body
// (x to the right, y toward the keys). The companion of this table is the DIMENSION TABLE (`const DIM`)
// at the top of docs/overlay/overlay.html, which the printed overlay is drawn from: the two must hold
// the same numbers ("Panel Dimensions" test), so that paper and screen cannot drift apart. Where a
// number comes from, and how far to trust it, is written there.
#define NEPTUNE_PANEL_DIM(X) \
    X(bodyW,      338.5)  /* width of the body */ \
    X(bodyD,      176.8)  /* depth of the body (only the part above the keys is drawn) */ \
    X(keysY,       95.0)  /* y where the keys begin */ \
    /* touch strips (recess outlines) */ \
    X(stripY0,      3.9) \
    X(stripY1,     76.6) \
    X(pitchX0,     20.4) \
    X(pitchX1,     34.9) \
    X(modX0,       39.4) \
    X(modX1,       54.1) \
    /* display window (glossy bezel, not the lit pixels) */ \
    X(dispX0,      67.3) \
    X(dispX1,     108.8) \
    X(dispY0,       3.9) \
    X(dispY1,      29.5) \
    /* the six buttons under the display: Shift Settings / Play Rec / Oct- Oct+ */ \
    X(clX0,        67.3)  /* left edge of the left column */ \
    X(clColPitch,  22.1)  /* left edge to left edge, column to column */ \
    X(clBtnW,      19.2)  /* width of one cap */ \
    X(clY0,        35.3)  /* top edge of the Shift / Settings row */ \
    X(clRowPitch,  14.75) /* top edge to top edge, row to row */ \
    X(clBtnH,      12.0)  /* height of one cap */ \
    /* the narrow buttons: Arp, Scale, pad Up / Down (left), bank up / down, > and Func (right) */ \
    X(leftBtnX0,  114.8)  /* left edge of Arp / Scale / Up / Down */ \
    X(rightBtnX0, 306.5)  /* left edge of bank up / bank down / > / Func */ \
    X(narrowW,     11.7)  /* width of those caps */ \
    X(smallY0,      3.9)  /* top edge of Arp and of bank up */ \
    X(smallY1,     18.2)  /* top edge of Scale and of bank down */ \
    X(smallH,      11.4)  /* height of Arp, Scale, bank up, bank down */ \
    X(tallY0,      35.5)  /* top edge of Up and of > */ \
    X(tallY1,      57.6)  /* top edge of Down and of Func */ \
    X(tallH,       19.1)  /* height of Up, Down, >, Func */ \
    /* pads */ \
    X(padX0,      129.9)  /* left edge of pad 1 (lit area) */ \
    X(padPitch,    22.10) /* left edge to left edge */ \
    X(padW,        18.6) \
    X(padTopY0,    35.6)  /* top edge of the top row */ \
    X(padBotY0,    57.7)  /* top edge of the bottom row */ \
    X(padH,        18.7) \
    /* encoders (centred over the pad columns) */ \
    X(encCy,       16.5)  /* centre line of the knobs */ \
    X(encR,         5.6)  /* radius of the knob cap seen from above */ \
    X(encClear,     6.7)  /* radius of the hole cut for a knob */ \
    /* the overlay pieces: K knob plate, F1 / F2 foot strips, R right margin, T (bar + spine), M column */ \
    X(kX0, 127.2) X(kX1, 305.8) X(kY0, 6.6) X(kY1, 34.7) X(kCutY, 23.6) \
    X(fX0, 3.0) X(fSplitX, 128.3) X(fX1, 337.6) X(fY0, 77.6) X(fY1, 93.4) \
    X(rX0, 319.4) X(rX1, 337.6) X(rY0, 3.0) X(rY1, 76.9) \
    X(tBarX0, 67.3) X(tBarX1, 126.5) X(tBarY0, 30.3) X(tBarY1, 34.7) \
    X(tSpineX0, 109.5) X(tSpineX1, 114.1) X(tSpineY0, 3.5) \
    X(mX0, 55.0) X(mX1, 66.6) X(mY0, 35.3) X(mY1, 76.8) \
    X(bleed, 1.5)         /* print only: colour past the cut lines */

namespace PanelDim
{
    #define NEPTUNE_PANEL_DIM_CONST(name, mm) inline constexpr double name = mm;
    NEPTUNE_PANEL_DIM(NEPTUNE_PANEL_DIM_CONST)
    #undef NEPTUNE_PANEL_DIM_CONST

    // The same table by name, for the test that compares it with the overlay's.
    struct Entry { const char* name; double mm; };
    #define NEPTUNE_PANEL_DIM_ENTRY(name, mm) Entry { #name, mm },
    inline constexpr Entry TABLE[] = { NEPTUNE_PANEL_DIM(NEPTUNE_PANEL_DIM_ENTRY) };
    #undef NEPTUNE_PANEL_DIM_ENTRY
    inline constexpr std::size_t TABLE_SIZE = sizeof(TABLE) / sizeof(TABLE[0]);

    // ---- every control's rectangle, derived as the overlay's script derives it ----
    struct Rect
    {
        double x, y, w, h;
        constexpr double cx() const { return x + w / 2; }
        constexpr double cy() const { return y + h / 2; }
    };

    constexpr double padCx(int i) { return padX0 + i * padPitch + padW / 2; }
    constexpr double encCx(int i) { return padCx(i); }       // the knobs sit over the pad columns
    constexpr Rect pad(int row, int col) { return { padX0 + col * padPitch, row == 0 ? padTopY0 : padBotY0, padW, padH }; }
    constexpr Rect sixBtn(int col, int row) { return { clX0 + col * clColPitch, clY0 + row * clRowPitch, clBtnW, clBtnH }; }

    inline constexpr Rect BTN_SHIFT = sixBtn(0, 0), BTN_SETTINGS = sixBtn(1, 0),
                          BTN_PLAY = sixBtn(0, 1),  BTN_REC = sixBtn(1, 1),
                          BTN_OCT_DN = sixBtn(0, 2), BTN_OCT_UP = sixBtn(1, 2);
    inline constexpr Rect BTN_ARP     { leftBtnX0,  smallY0, narrowW, smallH };
    inline constexpr Rect BTN_SCALE   { leftBtnX0,  smallY1, narrowW, smallH };
    inline constexpr Rect BTN_UP      { leftBtnX0,  tallY0,  narrowW, tallH };
    inline constexpr Rect BTN_DOWN    { leftBtnX0,  tallY1,  narrowW, tallH };
    inline constexpr Rect BTN_BANK_UP { rightBtnX0, smallY0, narrowW, smallH };
    inline constexpr Rect BTN_BANK_DN { rightBtnX0, smallY1, narrowW, smallH };
    inline constexpr Rect BTN_PATTERN { rightBtnX0, tallY0,  narrowW, tallH };
    inline constexpr Rect BTN_FUNC    { rightBtnX0, tallY1,  narrowW, tallH };

    inline constexpr Rect STRIP_PITCH { pitchX0, stripY0, pitchX1 - pitchX0, stripY1 - stripY0 };
    inline constexpr Rect STRIP_MOD   { modX0,   stripY0, modX1 - modX0,     stripY1 - stripY0 };
    inline constexpr Rect DISPLAY     { dispX0,  dispY0,  dispX1 - dispX0,   dispY1 - dispY0 };
    constexpr Rect encoder(int i) { return { encCx(i) - encR, encCy - encR, 2 * encR, 2 * encR }; }
}
