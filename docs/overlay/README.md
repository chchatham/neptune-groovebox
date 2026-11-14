# Neptune panel overlay

A cut-out paper skin for the Launchkey Mini 25 MK4 running Neptune. Six pieces lie beside and between the
controls; no button cap is covered.

| File | Paper | Skin |
|---|---|---|
| `overlay-letter.pdf` | US Letter, landscape | dark (matches the panel) |
| `overlay-a4.pdf` | A4, landscape | dark |
| `overlay-letter-light.pdf` | US Letter, landscape | light (saves ink; use it for the test fit) |
| `overlay-a4-light.pdf` | A4, landscape | light |
| `overlay.html` | source of all four | add `?paper=a4`, `?skin=light` or both |

Sheets 1 and 2 hold the pieces. Sheet 3 is for plain paper: where to put the ruler, and the few marks that are not words.

**Not yet checked on the unit.** The control positions come from a product picture, not from a ruler
(see "Measure first"). Nobody has laid a print on the hardware.

## Print

1. Open the PDF in Preview or Acrobat (not the browser's own viewer if it only offers "Fit").
2. Orientation: landscape. Scale: **100 %** / "Actual size". Switch off "Fit to page", "Scale to fit",
   "Shrink oversized pages" and borderless enlargement.
3. Dark skin: best quality, and "print backgrounds" on if you print `overlay.html` from a browser.
4. Lay a ruler on the print. The two red marks on every ruler are 100 mm apart; the inch ruler reads 8 in.
   Check both the horizontal and the upright ruler. If either is off, the scaling is still on.

## Paper

- **Test fit:** the light version on plain paper. Cut, lay it on the unit, mark what is off.
- **Final:** matte full-sheet label stock (white, permanent or removable), or 160–200 g/m² matte card held
  with thin double-sided tape. Matte matters: the panel is under lights.
- Laser prints and pigment inks resist fingers. A sheet of matte laminate or two coats of matte fixative
  spray over a dye inkjet print keeps the black from rubbing off.
- A self-adhesive vinyl print from a copy shop works from the same PDFs.

## Cut

- Dashed grey lines are the cuts. On the dark skin the colour runs 1.5 mm past them (bleed); the short
  black marks outside each corner give the same lines for a ruler and knife.
- Faint grey outlines round each piece are the unit's own controls, drawn to show what the piece sits
  against. They fall away with the offcuts.
- **K** has eight knob holes, 13.4 mm across. A 1/2 in (12.7 mm) hole punch is close if the knobs allow it;
  otherwise a knife, or pull the knob caps to lay the piece. If you would rather not cut holes, cut along
  the red dotted line marked **K'**: what is left is a plain strip with the screen names, the grid and the
  lower edge of the two rings as brackets.
- **T** is L-shaped and its upright is only 4.6 mm wide. Cut it last and trim to fit.
- The circled letter beside each piece is its place on the small panel drawing on sheets 1 and 2.

| Piece | Where | Size (mm) |
|---|---|---|
| K | round the knobs and down to the top pads | 178.6 × 28.1 (K': 178.6 × 11.1) |
| F1 | between the strips / buttons and the keys, left part | 125.3 × 15.8 |
| F2 | between the bottom pads and the keys; butts against F1 | 209.3 × 15.8 |
| R | right of the knob-page buttons, > and Func | 18.2 × 73.9 |
| T | bar under the display and Scale; upright between the display and Arp / Scale | 59.2 × 31.2 overall |
| M | between the mod strip and the six buttons | 11.6 × 41.5 |

## Measure first

Every position is drawn from the table at the top of `overlay.html` (`DIM`). Those numbers were taken from
the maker's product picture and scaled by the published width, so the edges of dark buttons are good to
about ± 0.7 mm at best. Before cutting good stock, measure these with a ruler to 0.5 mm (the numbers match
the red callouts on sheet 3), change the table where your reading differs, reload and print again.

| # | Measure | Drawn | `DIM` entry |
|---|---|---|---|
| 1 | left edge of the body → left edge of the Shift cap | 67.3 | `clX0` |
| 2 | Shift cap, width × height | 19.2 × 12.0 | `clBtnW`, `clBtnH` |
| 3 | top edge of the body → top of the Shift cap | 35.3 | `clY0` |
| 4 | left edge of the body → left edge of the Func cap | 306.5 | `rightBtnX0` |
| 5 | left edge of the body → left edge of pad 1 | 129.9 | `padX0` |
| 6 | left edge of pad 1 → right edge of pad 8 | 173.3 | `padPitch`, `padW` |
| 7 | clear height between the knob skirts and the top pads | 12.4 (K' is 11.1) | `kCutY`, `kY1` |
| 8 | bottom of the bottom pads → the keys | 18.6 (F1, F2 are 15.8) | `fY0`, `fY1` |
| 9 | right edge of the mod strip → left edge of the Shift cap | 13.2 (M is 11.6) | `mX0`, `mX1` |
| 10 | bottom of the display window → top of the Shift cap | 5.8 (T's bar is 4.4) | `tBarY0`, `tBarY1` |
| 11 | right edge of the display window → left edge of the Arp cap | 6.0 (T's upright is 4.6) | `tSpineX0`, `tSpineX1` |
| 12 | right edge of the Func cap → right edge of the body | 20.3 (R is 18.2) | `rX0`, `rX1` |
| 13 | widest diameter of a knob, skirt included | hole 13.4 | `encClear` |

Also unknown: whether the panel is flat right of Func (there is an embossed mark near its top), left of the
strips (F1 is empty there but for the NEPTUNE mark and can simply be cut short) and above the knobs; and how tall the knob skirts
are. 10 and 11 decide whether T fits at all: it is the tightest piece.

## How to read it

Colour says which key is held; a label belongs to the control it sits beside. Labels are words. The few
drawn marks are ones that need no explaining.

| Mark | Meaning |
|---|---|
| white (black on the light skin) | the control by itself |
| amber, solid diamond | with **Shift** held. The big diamond sits over the Shift key; a row or zone that is all Shift carries one diamond |
| blue, ring | with **Func** held. The big ring sits beside the Func key |
| diamond and ring together | with Shift and Func held |
| ▶ ● | Play, Record |
| curled arrows | undo, redo |
| ‹ › | back, on |
| two bars, one solid | the top or the bottom row of pads |

### Piece by piece

**K — knobs and top pads.** A line rings knobs 1–4 and runs into the top row of the little 4 × 2 grid;
another rings knobs 5–8 and runs into the bottom row: on grid screens knobs 1–4 turn the top row of cells,
5–8 the bottom row. Over each top pad, in amber: the screen Shift + that pad opens. The line along the pads
marks the beats of the sixteen steps (1, 5; on F2: 9, 13).

**F2 — bottom pads.** Under each bottom pad, in amber: the screen Shift + that pad opens (pads 7 and 8 open
nothing).

**R — right margin.** ‹ › beside the two small buttons: KNOB PAGE back / on; with Func: JUMP (a section, or
the first / last page). Beside >: held, the pads choose the PATTERN; Func + > = PASTE. Beside Func: the
ring; knobs go in COARSE steps; Func + a top-row pad = that PAGE of the screen.

**T — display corner.** Over Shift: the diamond; knobs go in FINE steps. Over Settings: Func + Settings =
TAP TEMPO. Over Up (and so for Down): Shift = previous / next TRACK. Beside Arp: Shift + Arp = LATCH.
Beside Scale: Shift + Scale = the scale SCREEN.

**M — Play and Record.** Under ▶: Shift = REWIND and SILENCE (stop, back to the start, every note and
effect tail cut); Func = CLEAR; Shift + Func = undo. Under ●: Shift = FILL while held; Func = COPY;
Shift + Func = redo.

**F1 — left foot.** Under Oct − / +: Shift + Oct = KEY SPLIT. Under Down: a tap of Up / Down steps the
PAD PAGE (whatever the pads show); with Up or Down held (HOLD), the top row picks a TRACK and the bottom
row is MUTE.

### Left to the help cards

Hold Shift, Func, >, Up / Down or Record alone and the display shows what that key does on the current
screen. The skin leaves these to the cards: hold a step + turn = lock (and > to remove it); > + Record /
Play = keep / reload the pattern; Record + Play = record from the top; Func + Up / Down = pressure range;
Func + bottom pad on TRACKS = solo; Shift + held pad = pin; held Up / Down + knob = every track; four
clicks to carry out a destructive cell; Arp pressed again = arp on / off; Scale = scale lock.

### Words and short forms

Whole words, except these everyday short forms:

| On the skin | Full | On the display |
|---|---|---|
| PARAMS | parameters | PARAM |
| TRACK FX | track effects | TRK FX |
| AT ROUTE | aftertouch routing (pad pressure) | AT ROUTE |
| CHORDS | chord pads | CHORD |

(Shift + bottom pad 3 was ARP until 2026-11-14; the arpeggiator is on the Arp button, which carries its own cap.)

Printed whole: TRACKS, STEP (top pad 4 and bottom pad 1), PRESET, SONG, SETTINGS, DRUMS, SCALE, WAVEFORM
(display: WAVE), FILTER (display: FLTR), KNOB PAGE, PAD PAGE, PAGE, JUMP, PATTERN, PASTE, COARSE, FINE,
TAP TEMPO, TRACK, LATCH, SCREEN, REWIND, SILENCE, CLEAR, FILL, COPY, KEY SPLIT, HOLD, MUTE, and the mark
NEPTUNE.

## Sources

Every function shown was read from `docs/launchkey-mk4-control-map.md` ("CONTROL GRAMMAR" and the Shift +
pad table) and checked in `src/ui/ScreenManager.cpp` and `src/main.cpp`. Where they differ the code was
followed:

- Shift + Play is REWIND and SILENCE. The Shift help card says only `STOP, REWIND`
  (`ScreenManager.cpp`, `buildShiftButtons`); the silence is in `src/main.cpp`, `stopPressed()`.
- Key split (Shift + Oct) is in `src/controller/KeyboardSplit.h` and on the Shift help card, not in the
  grammar table.
- JUMP stands for what Func + a knob-page button does: a section jump on PARAM, TRACK FX and FILTER, first /
  last page on TRACKS, SETTINGS and ARP, and a plain page step on the other screens.

## Regenerate

```
CH="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
D="file://$PWD/overlay.html"
"$CH" --headless --disable-gpu --no-pdf-header-footer --print-to-pdf=overlay-letter.pdf       "$D"
"$CH" --headless --disable-gpu --no-pdf-header-footer --print-to-pdf=overlay-a4.pdf           "$D?paper=a4"
"$CH" --headless --disable-gpu --no-pdf-header-footer --print-to-pdf=overlay-letter-light.pdf "$D?skin=light"
"$CH" --headless --disable-gpu --no-pdf-header-footer --print-to-pdf=overlay-a4-light.pdf     "$D?paper=a4&skin=light"
```
