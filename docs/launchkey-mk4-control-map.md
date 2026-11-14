# Controls Specification — Neptune Groovebox

Hardware messages below are for Neptune firmware **NPD**, **NPE** and **NPF**, the running build (the same messages, except that NPF also sends encoder turns while Shift is held; measured on NPD; source: `docs/launchkey-mk4-firmware-map.md` section 0.7).
What every control does is the last section, **CONTROL GRAMMAR**; what each screen's knobs and pads are is the table
in the owner's manual (`docs/Neptune-Groovebox-Manual.pdf`). The sections before the grammar are protocol and implementation facts only.

## HARDWARE → HOST MAP (NPD)
| Control | Message | Port |
|---|---|---|
| Pads, top row L→R | notes 96–103 ch1, velocity from onset pressure; polyphonic aftertouch `A0` ch1 (needs feature 0x6C = 2) | DAW |
| Pads, bottom row L→R | notes 112–119 ch1; polyphonic aftertouch likewise | DAW |
| Encoders 1–8 | relative: CC 0x55–0x5C ch16, value 64 ± steps (needs feature 0x45 = 127) | DAW |
| Shift | CC 63 ch7 (127 / 0) | DAW |
| Settings | CC 108 ch1 (127 press / 0 release) | DAW |
| Arp | CC 109 ch1 (127 / 0) | DAW |
| Scale | CC 110 ch1 (127 / 0) | DAW |
| Func (right of bottom pad row) | CC 105 | DAW |
| Button right of top pad row (">", the pattern key) | CC 104 | DAW |
| Up / Down (left of pads) | CC 106 / 107 | DAW |
| Encoder-bank up / down (small pair right of encoders) | CC 51 / 52 | DAW |
| Play / Record | CC 115 / CC 117 | DAW |
| Octave − / + | handled inside the firmware (shifts the keybed); sends nothing | — |
| Keys | notes 48–72 at octave 0, on the channel set by feature 0x64 (the app sets channel 1) | MIDI |
| Pitch strip | pitch bend; snaps back to centre on release | MIDI |
| Mod strip | CC 1; holds its last value on release | MIDI |

- Shift, Settings, Arp and Scale no longer trigger any firmware mode. With Shift held, pads keep their host colours and send their normal notes.
- A held pad shows the firmware's pressure rainbow; the host colour returns on release.
- There is no left arrow on this device.
- The app tags notes from the MIDI (keybed) port with `KEYS_PORT_CHANNEL`, so a key can never be read as a pad even when octave shift brings it to note 96 or above.
- Shift + Up / Down: `ScreenManager` treats CC 102 / 103 (ch1), and CC 106 / 107 received while Shift is held, as
  track −1 / +1 (measured on NPD: Shift + Down → CC 103).

## BUTTON MAPPING POLICY (user explicit)
- **Buttons must match their physical MK4 overlay labels.** Do not repurpose labeled buttons for unrelated functions.
- Octave − / + stay in the firmware.
- Shift + pad = go to a screen, following the labels printed over the pads (set in `main.cpp`, `setShiftPadScreen`):

| Pad | Top-row label | Screen | Bottom-row label | Screen |
|---|---|---|---|---|
| 1 | Plug-in | PARAM | DAW | STEP |
| 2 | Mixer | TRACKS | Drum | DRUMS |
| 3 | Sends | TRK FX | User Chord | CHORD |
| 4 | Transport | STEP | Chord Map | SCALE |
| 5 | Custom 1 | PRESET | Custom 1 | WAVE |
| 6 | Custom 2 | SONG | Custom 2 | FLTR |
| 7 | Custom 3 | SETTINGS | Custom 3 | — |
| 8 | Custom 4 | AT ROUTE | Custom 4 | — |

Screen indices (`main.cpp`, `ScreenIndex`): 0 TRACKS, 1 STEP, 2 PARAM, 3 DRUMS, 4 PRESET, 5 SETTINGS, 6 FLTR, 7 ARP,
8 SCALE, 9 SONG, 10 AT ROUTE, 11 WAVE, 12 TRK FX, 13 CHORD. ARP is reached by the Arp button only.

## ARP, SCALE AND LATCH (app side)
- Internally the app still identifies the Arp and Scale buttons as (channel 7, CC 0x49) and (channel 7, CC 0x4A); the Mac window's keyboard shortcuts and the tests use that identity. `ScreenManager::controlChangeReceived` translates the hardware messages CC 109 / CC 110 (ch1) to it and opens SETTINGS on CC 108.
- Key latch: `addPhysicalNote()` on note-on, `removePhysicalNote()` on note-off. Latch suppresses note-off for latched notes; the first key after all keys are up starts a new chord; switching latch off sends note-off only for notes not physically held. Max 16 latched notes (`MAX_HELD_NOTES`).
- The feature is called **"latch"** (not "hold").

## CONTROLLER PROTOCOL
- The OLED bitmap SysEx is exactly 1,216 bytes: 19 bytes × 64 rows, 7 pixels per byte
- Never queue frames — wait for the ready reply (`F0 00 20 29 02 13 09 F7`; about 44 ms after a frame on NPE, 75 ms on NPD) before sending the next bitmap. The Launchkey holds only one bitmap at a time. Static screens must not be resent
- Only the stationary display target is shown on NPD (the temporary target and the firmware's encoder pop-ups are suppressed by the display lock)
- Keys are believed to have NO aftertouch (not verified)

## FEATURE CONTROLS
- Feature controls are CC messages on **channel 7** sent to the DAW port (`B6 <cc> <value>`); enabled with NoteOn ch16 note 11 vel 127 (`9F 0B 7F`)
- At connect the app sends, in order (`main.cpp`): `enableFeatureControls()`, `setPadMode(PAD_MODE_DAW)` (0x1D = 2), `disableFirmwareArp()` (0x49 = 0), `disableFirmwareScale()` (0x4A = 0), `setEncoderMode(ENCODER_MODE_PLUGIN)` (0x1E = 2), `configureNeptuneFirmware()` (0x64 = 0 keys on channel 1, 0x6C = 2 polyphonic pad aftertouch, 0x6E = 0 device MIDI clock output off, 0x45 = 127 relative encoders)
- 0x45 is volatile and must be sent on every connect; 0x64, 0x6C and 0x6E are non-volatile
- **MIDI clock:** the device's "MIDI Clock Out" setting (0x6E) is on by default; it then sends `F8` on the MIDI port all the time (24 a beat at its own tempo, 120 BPM), app or no app. The app switches it off. The controller's ports are never a clock or transport source for the app: `LaunchkeyController` drops system messages (`F1`–`FF`) from them, and clock from a MIDI input goes through `MIDIClock::handleMidiMessageFromPort`, which refuses a Launchkey port in every CLK mode (nothing calls it yet: the app opens no MIDI input but the controller, so CLK = EXT / AUTO has no clock source). The app's own clock goes to the first MIDI output that is not a Launchkey port. Play is CC 115; in DAW mode the device sends no Start / Stop
- **Clock in:** the app makes a virtual MIDI input, "Neptune Clock In". With CLK = EXT or AUTO the sequencer follows clock sent there (Start / Stop, tempo, position). No physical MIDI input is opened for clock, and the Launchkey's ports never count as one
- **Button lights:** after the connect messages the app lights Up, Down, > and Func white: the debug SysEx on the MIDI port calling the firmware's `led_set(id, 0x3F3F3F)` for ids 0x22, 0x23, 0x10, 0x11 *(not yet seen on the device)*
- **Octave and split:** the app reads the firmware's octave every 2 s while idle (debug SysEx) and writes it, with the split, when a preset or project that stores the keyboard is loaded (`KeyboardSplit.h`)
- Full list of registered feature controls: `docs/launchkey-mk4-firmware-map.md` section 0.9 and the Programmer's Reference

## ENCODER VALUE CONVENTION
- Device format (NPD, feature 0x45 on): CC 0x55–0x5C ch16, value 64 + steps (65 = one step up, 63 = one step down)
- `LaunchkeyController` translates that to the app's internal step format on CC 0x15–0x1C: **1–63 = positive (clockwise), 65–127 = negative (counter-clockwise)**
- All screens use the same delta formula: `int delta = (value > 64) ? (value - 128) : value;`
- In tests, use value=1 for increment and value=127 for decrement — NOT 65/63

## ENCODER LABELS
- Encoder-bar labels are 2 characters (8 columns × 16 px); grid-cell labels are up to 4 characters.

## setPadRGB NOTE MAPPING
- `setPadRGB(padIndex, r, g, b)` expects **MIDI note numbers**, NOT 0-15 indices
- Top row pads: notes 96–103 (`PAD_TOP_ROW_START + 0..7`)
- Bottom row pads: notes 112–119 (`PAD_BOTTOM_ROW_START + 0..7`)

## PARAMETER ID SPACE
Parameter id ranges in use (engine ids 0–149, 158–189 and 400–655, send types 150–157, mixer 190–199, insert effects
200–239, track effect blocks 240–263, arp locks 270–272, insert ON / WET / SYNC / division 280–311, screen-only cells
900–909, pressure ranges 1000+ / 2000+). Do not add a range that collides with them.

## VIRTUAL CONTROLLER
- `VirtualControllerComponent.h` draws the panel in the Mac window as it looks with the printed overlay on: from the top edge down to just above the keys (no keys in the picture), laid out at 4 px per mm from `src/ui/PanelDimensions.h`. That table and the `DIM` table of `docs/overlay/overlay.html` must hold the same numbers, and `PanelOverlay.h` the overlay's words ("Panel Dimensions" test): change the overlay first, then the two headers
- Its buttons send the hardware's messages (Func CC 105, Settings 108, Arp 109, Scale 110 on channel 1; Shift CC 63 on channel 7); pads send their notes on channel 1, velocity 100; encoders send the app's internal step format (CC 0x15–0x1C, channel 16); Oct − / Oct + send nothing, like the hardware's
- Strips: drag; the pitch strip returns to centre on release, the mod strip keeps its value (`onPitchBend`, `onModStrip`, wired in `main.cpp` to the selected track like the hardware's)
- Right-click = hold element (build combos; Shift held lights amber, Func blue), left-click = press, click-drag or mouse wheel on encoders; the caps carry no printed names, a tooltip names the control under the pointer
- `setPadColor()` (0–255 per channel) lights a pad; `main.cpp` → `mirrorPadsToPanel()` feeds it what the hardware's pads get, with or without a controller. `setDisplay()` shows the live display in the panel's small window, 1:1
- CTRL_W=1378, CTRL_H=392; stacked below the OLED mirror in MainContentComponent. `NEPTUNE_SNAPSHOT_CONTROLLER=<file.png>` on the test binary writes the picture (and `<file>-lit.png`) at 2x

## CONTROL GRAMMAR (approved by the owner 2026-11-10 — this section wins over anything above it)
Shift = where, and how finely. Func = bigger, or secondary. The same gesture means the same thing on every screen.

| Modifier | Turn a knob | Press a pad | Up / Down (beside the pads) | Bank up / down (beside the knobs) |
|---|---|---|---|---|
| plain | edit the shown cell, one step | the screen's pad action | what the pads address: step page (STEP, PARAM), scale page, SONG row, PRESET list | encoder page ±1, on every screen, nothing else; the pages go round (back from the first = the last, on from the last = the first) |
| Shift + | fine; on an envelope's ATK, DEC or REL cell: that stage's curve (below) | top / bottom row: go to screen (an unassigned pad does nothing) | track ±1 | encoder page ±1, as plain; while Shift's help card shows: its cards (**Help cards**, below) |
| Func + | coarse / snap | top row: page key; bottom row on TRACKS: solo; any other pad: the screen's own pad action (also while another screen's pads are pinned) | PARAM, TRK FX, FLTR: set the low (Down) / high (Up) pressure-range end of the knob turned last; again = clear it. Elsewhere: nothing | section jump (PARAM, TRK FX, FLTR), first / last page (TRACKS, SETTINGS, ARP), encoder page ±1 as plain on the other screens; while Func's help card shows: its cards |
| hold a step + | lock on that step (Shift fine, Func coarse); the hold alone shows that step's values (**Held step**, below) | — | — | lock page ±1 |
| hold > + | on a held step: remove that knob's lock | queue pattern 1–16 | — | Record = keep a temporary copy of the pattern, Play = reload it (an undo step) |
| hold Record + | — | — | — | Play = live recording; again = free / quantized |
| hold Up or Down ("Track") + | the same parameter on every track that has it | top row: select track N; bottom row: mute track N | — | — |

**Step sizes.** Plain: 1 % of a continuous range, 1 of a discrete one, on every screen. Fine (Shift): a tenth of that.
Coarse (Func) snaps: unipolar 0 / 25 / 50 / 75 / 100 % and the default; bipolar min / centre / max and quarters; tune,
NOTE and frequencies by octaves; BPM ±10 (Shift = 0.1); LEN and counts by 4; division lists to the straight values;
other choice lists by 4 (LFO and envelope DEST lists: to the next group of destinations). FM operator ratios (RAT1–6,
RATI): plain = the next harmonic (0.5, 1, 2, 3 … 16; from an in-between value the neighbouring harmonic), Shift = 0.01,
Func = an octave (×2, ÷2). A press of Up or Down that had a pad or knob used under it does not also page, and neither does one held long enough to show its help card.

**Envelope stage curves.** Shift + turn on the ATK, DEC or REL cell of an envelope (times in seconds: the MAIN page,
AMP, F ENV, MENV, the FM operator pages, the same cells on FLTR and on a held step's pages on STEP) does not fine-adjust the
time: it bends that one stage. −100 = the move comes early (an attack that bulges upward, a decay or release that
drops at once and levels out), 0 = a straight line, +100 = the move comes late (a hollow attack, a decay or release
that holds and then falls); 5 per click. For a second the cell shows the curve (ACRV / DCRV / RCRV), the name bar
says "ATK CRV +40" (on FLTR the knob readout does) and the envelope picture shows the shape. SUS has no shape: Shift
stays fine there, as on every other cell, and on a MIDI track's ATK / DEC / REL (controller values). Func + turn is
coarse as always, also with Shift held. The curves are parameters like any other (`StageCurveParam`, ids 400–423 of
the engine): saved with the sound, lockable per step (hold a step + Shift + turn; hold a step + > + Shift + turn
removes that lock), and Track + Shift + turn sets them on every track with the same engine. Where the envelope has a
CURV cell, CURV still bends all three stages and each stage's own curve adds to it (limited to ±100).

**Buttons.** Arp: opens the ARP screen; pressed again on that screen it switches the selected track's arp on / off
(an ARP tag in the name bar shows it is on). Shift + Arp: latch — the arp's latch when the arp is on, the key latch
when it is off; LATCH tag in the name bar on every screen (and ROLL, letter O, while pad rolls are on with ROL2's LTCH). Scale: scale lock (keys on pitched tracks play the
nearest note of the active scale page, arps stay in it); the name bar says "SCALE LOCK ON C MAJOR" / "SCALE LOCK
OFF" for 2 s (shortened to fit: "SCALE ON C MAJOR", "SCALE ON") and a SCALE tag stays while it is on; Shift + Scale:
SCALE screen. Play: start / stop; Shift + Play: stop and return to the start (when already stopped: reset the
position), and silence: every note is let go and every effect's tail (inserts, send buses) is cut - the way out
of anything that rings on. Shift + Record held: FILL while held. Func + Settings: tap tempo.

**Copy, clear, paste.** Func + Record = copy, Func + Play = clear, Func + > = paste. What they act on
(`ScreenManager::getEditPlan`, which the action, the Func help card and the confirmation all go by):

| Where | Copy / paste | Clear |
|---|---|---|
| a step held on STEP | that step (notes, velocity, length, condition, locks) | that step |
| a step held on PARAM, TRK FX, FLTR | that step's locks (one clipboard for the three screens) | that step's locks |
| STEP, no step held | the selected track's steps, length and speed | the selected track's steps |
| TRACKS | the pattern: every track's steps, lengths and speeds, and the swing — **paste: hold 0.5 s** | every track's steps — **hold 0.5 s** |
| PARAM, FLTR, no step held | the selected track's whole sound: engine and its parameters, mixer, sends, effects | **the engine's parameters only**, back to their defaults (with their pressure ranges); mixer and effects stay ("RESET T3 ENGINE") |
| TRK FX, no step held | the selected track's **effects**: the DIRT / TALK / MODFX blocks, the insert slots (types, ON / WET / SYNC / division, parameters), the ARPLK values, and pressure ranges set on any of those | the same set: blocks to their defaults (off), no insert slots, arp locks off ("CLEAR T3 FX") |
| any other screen | nothing | nothing |

TRK FX never touches the TRACK page's VOL, PAN, SND1 / SND2, BUS1 / BUS2, LEN or SPD (mix and sequencing, not
effects), nor the engine. The effects have their own clipboard, separate from the whole-sound one.

**Whole pattern = hold.** A plan that replaces a whole pattern from an edit chord (clear and paste on TRACKS) is
carried out only when its button (Play, >) has been held for 0.5 s (`ScreenManager::HOLD_MS`, timed by `UiClock`
and the 30 Hz refresh's `ScreenManager::tick()`; a release after 0.5 s also acts if no refresh came). While it is
held the name bar shows it filling ("CLEAR >>>.."; on the Func card, if it is showing, the PLAY line: "PLAY >>>..
PATTERN 5" — the held button does not remove the card). Let go early (the button or Func): nothing changes and the
name bar says "HOLD TO CLEAR PATTERN" / "HOLD TO CLEAR" / "HOLD TO PASTE". Every smaller scope acts at the press, and
so do copy, an undo, and a paste with nothing copied. > + Play (reload the kept copy) also replaces a whole pattern
but stays instant: it is a performance gesture that has to land in time; it is an undo step instead.

**Undo, redo.** Func + Shift + Play = undo, Func + Shift + Record = redo (Shift + Play is "back to the start";
both chords are ones the hardware is known to send with Shift held). The history (`src/project/UndoHistory.h`,
filled and applied by `ScreenManager`) holds up to 15 actions; the sixteenth pushes the oldest out; a new action ends
what could be redone. An action stays undoable until it is undone or pushed out — knob turns, pads, keys, the
transport, pages and screens do not void it. What is an undo step:
- every clear, paste and reset above (not copy: it changes only the clipboard);
- > + Play (reload);
- a screen's own one-click destructive actions, wrapped in `Screen::UndoMark`: FX TYPE on TRK FX (one scroll through
  the list within 2 s = one step, back to the effect and values it replaced), LCKS on a held step's TRIG page (and
  hold step + > + turn LCKS), ARP → MUTA EVOL anticlockwise (the mutation and its ancestor), >PAT;
- a recording pass: one step from the moment recording runs (armed and playing) to the moment it stops, holding the
  tracks it changed. Undo while it runs takes back the take so far and recording goes on; an edit during a take
  splits it (take so far, the edit, the rest).

Not undo steps: knob turns (parameters, locks made by hold + turn, trigs tapped on STEP), copy, engine change on
TRACKS, preset load, project load / save / delete, SONG edits, ARP → ANC. What an entry holds: steps entries hold the
other state of exactly what the action replaced (one step, one track's steps / length / speed, or a pattern's tracks
and swing) and swap it with the pattern; a sound entry holds the track's parameter map before and after and puts
back only the parameters the action changed (the whole sound and engine when the action changed the engine type),
so knobs turned since stay. Memory per level is bounded by one pattern's steps or two parameter maps; nothing of it
is touched by the audio thread, and whatever replaces steps or rebuilds an engine holds `AudioEngine::structure()`.
An entry whose track no longer has the engine it had (its locks and parameters are that engine's ids) is passed over
and dropped; a project loaded or started empties the history (`ScreenManager::projectLoaded`).

The older shortcut stays: the same copy / clear / paste chord again, with no other pad, knob or button in between,
takes that edit back (for clear and paste it is the history's undo of that entry, so redo brings it back; for copy
the clipboard returns to what it was).

The name bar names what was done for 2 s, in the longest wording that fits beside the screen's name ("CLEAR T3
STEPS", "CLEAR PAT 5", "CLEAR STEP 7", "CLEAR S7 LOCKS", "RESET T3 ENGINE" / "RESET T3 ENG", "CLEAR T3 FX", "COPY T3
SOUND", "PASTE T3 SND"); what an undo or redo did may take the whole bar, the screen's name included ("UNDO CLEAR T3
STEPS", "REDO PASTE PAT 5", "UNDO REC TAKE PAT 1", "UNDO SET T3 FX1 TYPE", "NOTHING TO UNDO"). Where a chord does
nothing it says "NO CLEAR HERE" / "NO COPY HERE" / "NO PASTE HERE"; a paste with nothing copied says "NOTHING COPIED"
and changes nothing. Known gap: "again" after copying a held step or its locks says UNDO COPY, but the step
clipboard keeps the copy.

**Help cards.** Every key that changes what other controls do while it is held has help cards: **Shift, Func, >
(the pattern key), Up / Down (either button: the Track layer) and Record**. Hold one of them alone and, after
0.35 s, the whole display becomes a card that says what that key does on this screen (the screen is not drawn
under it; the pad lights are the key's usual ones). SETTINGS → SETUP → HELP sets when: OFF (never), 0.35S (the
default) or INST (at the press). A hold key pressed within 2 s of a knob turn, pad press or button press is taken
for the start of a chord: in every mode its card then waits 2 s (and any control used meanwhile cancels it for the
hold, as always). The choice is kept between sessions (`Projects/help.txt`), not in a project. One
rule for all five (`ScreenManager::getHelpCardKey`, `helpKeyChanged`):
- The card appears only if no other pad, knob or button was pressed since the key went down, and not if another of
  these keys, Play or Scale was already held (`holdDownBesides`): two keys held together show no card, whichever
  went down first. A held pad does not prevent it (hold a step, then Func: the card says what copy / clear / paste
  would do to that step; hold a step, then >: the card says a turn removes that step's lock).
- Any other pad, knob or button removes the card for the rest of that hold **and acts exactly as always** (Shift +
  turn is fine from the first click, Shift + pad goes to the screen, Func + Play clears). Keys, the two strips and
  pad pressure neither remove it nor act differently. Letting the key go brings the screen back unchanged.
- The one exception: **while a card shows, the two bank buttons page through that key's cards** (lower = next,
  upper = previous, round and round, "n/N" at the top right) and do nothing else. Pressed before the card shows, a
  bank button keeps its meaning (Func + bank = jump).
- Each key remembers the card it was on until the screen changes; a new screen starts at the first card. A key
  with one card (Record) shows no "n/N" and the bank buttons then do nothing.
- **A hold that showed a card has no other effect when the key is let go.** That matters for Up / Down, whose tap
  acts on release: a quick press pages as always (also with HELP on INST, where the card flashes up at the press:
  a press shorter than 0.35 s is still a tap), a hold that showed the card does not page — the same as a hold that
  had a pad or knob used under it. With HELP OFF every press pages, however long. The one exception is in the
  table (`releaseActsAfterCard`): **Record still switches recording on / off when it is let go**, card or no card.
- No card: Play, Settings, Arp and Scale (they act at the press and are not layers; their Shift / Func forms are on
  the Shift and Func cards), a held step (on STEP the hold itself shows that step's pages; what else it accepts is
  on the > and Func cards while it is held), the octave buttons (they never reach the app), and any two keys
  held together (Shift + Func is not a layer: Func's coarse turn wins, Shift's pads win).

Shift's cards, the same on every screen: **PADS** (the screen map in the pad columns, four letters each, the
current screen inverted, a pinned pad screen marked "*", on DRUMS / SCALE "HOLD THIS PAD: PIN" / "UNPIN", and a
pointer "COPY CLEAR PASTE:FUNC"), **BUTTONS** (Oct − / + key split, Arp latch, Scale screen, Up / Down track, Play
stop and rewind, Record held fill) and **KNOBS** (fine; "ATK DEC REL: CURVE" on a page that has such cells; bank).
Func's cards follow the screen: **EDIT** (REC / PLAY / > with the verb and what it acts on right now, e.g. "PLAY
CLEAR T3 STEPS", "PLAY RESET T3 ENGINE", "PLAY CLEAR T3 FX", "> PASTE (EMPTY)"; a whole pattern reads "PLAY CLEAR
PAT 5 HOLD" and, while the button is held, "PLAY >>>.. PATTERN 5"; while the same-again undo is armed that line
reads "PLAY UNDO  CLEAR", inverted; on a screen without the chord "NOT ON THIS SCREEN" and where to go. Under them, on
every screen: "WITH SHIFT: PLAY, REC", then "UNDO: CLEAR T3 STEPS" or "NOTHING TO UNDO", then "REDO: ..." or, when
there is nothing to redo, "KNOB TURNS: NO UNDO"), **PADS** (the page keys by
name with the current one inverted; "BOTTOM ROW: SOLO" on TRACKS) and **KNOBS ETC** (Settings tap tempo, coarse,
what bank does here, pressure range where it exists).

>'s cards: **> PATTERNS** (the sixteen patterns in the pad columns as the pad lights show them: the current one
inverted, the queued one marked "*", a pattern with trigs by its number, an empty one a dot; "PATTERN 1  NEXT 7*";
"PAD: GO TO PATTERN" when stopped, "PAD: NEXT AT LOOP END" while playing) and **> BUTTONS, KNOBS** ("REC KEEP A
COPY OF PATTERN n"; "PLAY RELOAD IT" with where from: "FROM THE KEPT COPY", "AS IT WAS LOADED" or "(NOTHING TO
RELOAD)"; the knob line: "KNOB REMOVE LOCK ON THE HELD STEP" while a step is held, "HOLD A STEP + KNOB: REMOVE THAT
LOCK" on STEP, PARAM, TRK FX and FLTR, "KNOB NO CHANGE" elsewhere).
Up / Down's cards: **UP DOWN: TRACKS** (tracks 1–8 over the top pad columns with their engine letters, "1VA 2DR …",
the selected one inverted; under each "ON", "MUTE" or "SOLO" as its bottom pad shows; "TOP: SELECT A TRACK",
"BOTTOM: MUTE ON / OFF") and **UP DOWN: KNOBS** ("KNOB ALL TRACKS THAT HAVE THIS CELL" on TRACKS, PARAM, TRK FX and
FLTR — "KNOB NO CHANGE (THIS TRACK ONLY)" elsewhere, on TRACKS → ENG and while a step is held; "TAP" with what a
quick press moves here: STEP PAGE, SCALE PAGE, SONG ROW, PRESET LIST or NOTHING HERE; and where solo is, which is
not in this layer: the TRACKS screen, Func + bottom pad). More than eight tracks: the layer and its card reach
tracks 1–8.
Record's card: **RECORD** ("LET GO RECORD ON" / "RECORD OFF" and what follows; "+ PLAY RECORD NOW FROM THE TOP",
"AGAIN FREE / QUANT", "NOW: REC FREE"; while recording "+ PLAY FREE / QUANT").

The keys are a table in `ScreenManager::helpKeyDef`, one row per hold key: its name, the delay, whether its release
still acts after a card, and its cards (`helpCardsFor`). A new hold layer needs a `HelpKey`, a row, and a
`helpKeyChanged()` call where the key goes down and comes up. Each screen supplies its own facts through
`Screen::getEditScope`, `getPageKeyName` and `getHelpFacts` (`tapUpDown`, `allTracksTurn`, `stepLocks` for the new
cards). The screen audit holds every key on every screen (`ScreenAudit::auditHelpCards`).

**CHORD (chord pads).** Shift + bottom pad 3. Keys held (physically, or kept by the key latch; with the arp on and
latched, the arp's notes) + a pad: the pad takes that chord, replacing what it had (undo step "STORE CHORD PAD n");
nothing is played by the press. A pad pressed with no key held plays its chord on the selected track through the
note-pad path (`notePadDown`, one pad slot per note: recording, arp, rolls), pad velocity scaling the chord and pad
pressure going to every note. Four banks of sixteen (BK, Up / Down), TR / OC move all pads, VL = stored levels or
the pad's velocity, CL (four clicks) clears the bank. Pinnable. Saved in the project (`chordPads`,
`ChordPads::toFlat`). Not built: strum, copy / clear / paste of one pad.

**Presets.** SV saves the track's own sound read from the engine, mixer and effects (`ProjectBridge::captureTrackSound`,
with the insert chain's types); while a preset is auditioned, the remembered sound. A preset that has the insert
type ids sets the track's insert chain when auditioned or loaded; presets without them leave the track's effects.
A preset also holds the track's arpeggiator (`Preset::arp`, the list `ProjectBridge::captureArp` makes: settings, on /
off, tapped rhythm); one that has it sets the track's arp, one without leaves it; leaving PRESET without LD restores
the track's own.

**ARCH.** The synth's ARCH cell loads an architecture's starting sound the first time it is chosen. From then on
each architecture keeps what was dialled in on it: turning ARCH away and back returns the sound as it was left (per
track, until that track's sound is replaced by a preset, a project or another engine).

**Session.** The app starts in the state it was last running in: every sound, pattern and setting a project holds
is written to `~/Music/Neptune/Projects/session.json` when the app quits and every 15 s while it runs, and put back
at the next start on top of the project last saved or loaded (SAVE on SETTINGS still writes that project's slot).

**Stop and latch.** When the transport stops (Play, or Shift + Play, which also cuts effect tails), notes that only
a latch is holding are let go: the key latch's and those of every track's latched arp. The latch stays on for the
next keys, and keys that are physically down keep sounding (`ScreenManager::releaseLatches`).

**Recording knob movements.** While recording is on and the transport runs, a lockable knob turned on PARAM, TRK FX
or FLTR (no step held) is written into the selected track as parameter locks: its value on every step the track
passes, from the step of the first turn for one lap of the track (a knob left alone keeps writing its value until
the lap is full; the lap never overwrites its own start). A step without a trig becomes a trig without notes that
carries the lock. When the lap is full or recording ends, the track's own value is again what it was before the
first turn. `AudioEngine::motionTouch` / `motionPoll` (the 30 Hz refresh); part of the recording pass's undo step.

**Recording.** Record alone: recording on / off, acting when the button is let go (the transport is not
touched; with recording on, Play records); held alone for 0.35 s it shows its help card, and still acts when let go. Hold Record + press Play: live recording — recording on and the transport
started from the top; when it is already playing, recording just switches on. Record + Play again while recording:
free <-> quantized (REC FREE: nearest step, timing kept as micro-timing; REC QUANT: nearest step, on the grid); the
mode is kept for the next recording. Play pressed a moment before Record counts as Record + Play when that Play
started the transport. Play (or Shift + Play) stops and ends recording; Record alone ends recording and keeps
playing. When recording ends and something was recorded, STEP opens with the track recorded last selected, on the
step page of its first recorded step (a pad pin is kept: unpin to get the step pads). The name bar shows a REC FREE
/ REC QUANT tag on every screen while recording is on ("R" / "Q" when room is short). While recording runs, a red
light on the pads follows the selected track's step (step modulo 16), drawn over the screen's own pad colours
without changing what the pads do (white on a pad that is already red); hold layers are not drawn over; STEP
shows its playhead.

**Metronome** (SETTINGS → CLICK, the fourth page; no gesture of its own). CLCK: OFF, REC (a click on every beat
while recording is on and the transport runs: the default) or PLAY (whenever the transport runs). The first beat
of each four-beat bar is higher and louder (1760 Hz against 880 Hz; a 40 ms sine burst made in code,
`src/engine/Metronome.h`). CLVL: its level, 0–100, default 50; every turn plays one click (also when stopped and
with CLCK off, so the knob always answers). CNT: count-in, OFF (default), 1BAR or 2BAR. With CNT set, a start of
the transport with recording on (Record + Play, or Record then Play) counts that many bars first: the name bar
says "COUNT IN 1 BAR" / "COUNT IN 2 BARS", the count-in clicks whatever CLCK says, and the transport and the
recording start on the sample the count-in ends (`SequencerCore::requestStart`). During the count-in
`isPlaying()` is false: no step, pattern or song row moves, nothing is recorded, the MIDI clock has not started;
Play (or Shift + Play) calls it off and switches recording off. Recording switched on while the transport already
runs, and Play without recording, never count in. The beats come from the sequencer's clock, sample-exact, and
follow tempo changes; a click still sounding when the transport stops is faded in 3 ms. The click is added to the
output after the master EQ, compressor, limiter and MVOL and after the resampler's tap: the master chain never
reacts to it, MVOL does not change it, and it is never in a resample of a track or the master (while it sounds the
output sum is kept within full scale). The three settings are app-wide (`Projects/metronome.txt`, like HELP):
no project stores or changes them.

**Destructive actions** need four clicks clockwise on their knob, spread over at least 300 ms, progress shown as
">>>"; another knob or an anticlockwise click cancels. Nothing destructive is on a pad, a paging button or a
modified turn. The edit chords are the other family: instant and undoable, a whole pattern held for 0.5 s (above).
The one-click destructive cells that have no confirmation (FX TYPE, LCKS, EVOL back) are undo steps.

**Pads.** Every screen sets its own pad colours. While Shift is held the pads show the screen map: current screen
bright, other assigned pads dim, unassigned off. A trig is added on press; an existing trig is removed only by a
release within 350 ms with nothing edited — a held step is never removed.

**Drum tracks on STEP.** The pads show one drum of the kit, the pad selected on DRUMS, and a step can hold any
number of the sixteen drums. A press puts the selected drum on the step beside the ones already there; a tap on a
step where it hits takes that drum away and leaves the others (the step goes off with its last drum). Pad lights:
lit where the selected drum hits, dim yellow where only other drums do; the display fills the cell where it hits
(with the number of drums on the step when more than one) and underlines the step number where only others do.
Hold a step + a key adds that key's drum. On TRIG, NOTE and VEL belong to the selected drum; LEN, MICR, COND and the
locks belong to the step. Recording was always polyphonic. Other tracks: a step is one trig, tapped on and off whole.

**Held step.** On STEP a press that lasts longer than those 350 ms is a hold, and the hold alone (no knob, no
movement) changes the display to that step's pages: the track's PARAM pages as locks, opening on MAIN (the engine's
first page where there is no MAIN: a MIDI track; a drum track shows the DR page of the pad selected on DRUMS, which the hold has put
on that step; on PARAM, where a hold adds nothing, the selected pad if it hits there, else the step's first drum), with TRIG (NOTE VEL LEN MICR | COND PROB / A B LCKS) one bank button back. Every cell
shows the value that step will play: its lock, with the lock mark, or else the track's own value. The name bar reads
"MAIN T1 S5 LOCK" ("TRIG T1 S5" on TRIG). A step whose notes carry recorded pad pressure (a dark dot at the lower
right of its cell on STEP) has one more page after the lock pages, PRES (PRES >LCK CLR, the recorded curves below):
one bank button back from TRIG. PRES = amount played, 0–200 % (hold > + turn: 100 %); >LCK and CLR act after four
clicks clockwise and the page then gives way to TRIG. A tap never shows the pages; a turn or a bank button before the 350 ms
opens them at once. The pages close when the last held step is let go, and only then. The page last shown is where
the next hold opens, until the track changes or STEP is opened again (then MAIN). Several steps held: the pages show
one step, the one on the lowest pad (top row left to right, then the bottom row), and every held step is edited
with it: what a knob gives the shown step (a lock on the sound pages; NOTE, VEL, LEN, MICR, COND, PROB, A, B on TRIG)
is written to all of them as the same value, a key sets every held step's note, > + turn removes that lock (or
restores that TRIG cell) on all, and LCKS clears the locks of all as one undo step. PRES and Func + Record / Play / >
still act on the shown step alone. Letting the shown step go moves the pages to the next. The same holds for
several pads held on PARAM, TRK FX and FLTR (lock and unlock all; all go white). On PARAM, TRK FX and FLTR a held step switches the cells to that step's values at once, the pad
goes white and the name bar puts the step first ("S12 FILTR"; when room is short the step is what stays).

Pad pin: Shift + **hold for half a
second** the bottom-row pad of the screen you are already on (DRUMS, CHORD or SCALE) pins those playing pads; they keep
playing with pressure on every other screen until the same gesture unpins them. A quick press does nothing (it was
too easy to do by accident). A PIN tag shows in the name bar.

**Screens.** Shift + top row: 1 PARAM (the engine's own pages only; a drum track shows one DR page that follows the
selected pad), 2 TRACKS (paged mixer: VOL, PAN, SND1, SND2, ENG — knob N = track N; on ENG the knob chooses a pending
engine and that track's top pad commits it), 3 TRK FX (TRACK, ARPLK, DIRT, TALK, MODFX, FX — the same six pages on
every track), 4 STEP, 5 PRESET (pads play the track; with AU on, scrolling auditions and the previous sound returns
unless LD is used), 6 SONG, 7 SETTINGS, 8 AT ROUTE (per track; destinations from that track's engine). Bottom row:
1 STEP, 2 DRUMS, 3 CHORD, 4 SCALE, 5 WAVE, 6 FLTR. The name bar shows "T n" on every screen. Bar screens show the full
name and value of the knob just turned in the name bar. The 4 × 2 grid stays.

**Filter picture.** While a filter parameter is being turned on any page, the response curve is drawn over the half
of the grid that does not hold that knob. FLTR shows the curve for every engine that has a filter, for every model.

**Pressure.** A pad's pressure moves its parameter from the knob's value toward the range end(s) and returns on
release (one end is enough).

**Vocabulary.** Filter: TYPE (model), MODE (response), CUT, RESO, ROUT. Every destination cell: DEST. Master volume:
MVOL. Always-trig: ALL.
