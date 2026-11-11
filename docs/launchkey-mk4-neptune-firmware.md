# Firmware Specification — Neptune Groovebox

Current build: **NPE**. Verified map: `docs/launchkey-mk4-firmware-map.md` section 0. Constraints: `docs/launchkey-mk4-firmware-rules.md`.
All addresses below are **file offsets** into the stock image (true VMA = file offset + 0x08008000).

## Purpose
The Neptune firmware is Novation's stock Launchkey Mini MK4 firmware (1563) with a small set of verified patches. Its job is
to make the controller a transparent surface for the Neptune app:
- the host owns the OLED (the firmware never draws over a host frame);
- no firmware mode takes controls away (no Shift layer, Settings menu, firmware Arp or Scale mode);
- every front-panel button reaches the host, except Octave ±, which the firmware keeps handling itself (user requirement);
- pads send polyphonic aftertouch in the DAW pad layout;
- firmware can be updated from the host with nobody touching the device, and inspected/driven from the host for testing.

The stock bootloader (0x08000000–0x08007FFF, 32 KB) is never modified. A from-scratch replacement firmware
(`neptune-firmware/`) was tried earlier and is shelved; if revisited it must be linked at 0x08008000.

## Build
```
tools/build_neptune_fw.py <TAG>      # TAG = 3 characters, e.g. NPD → firmware/neptune-fw-<TAG>.syx / .bin
```
- One patch list; every patch is checked against the stock bytes it replaces. Input: `firmware/launchkeyminimk4-firmware-1563.syx`.
- Assembly sources are `neptune-patches/dbg.S` and `neptune-patches/ccbtn.S`; their binaries (`handler.bin`, `cave.bin`,
  `h43.bin`, `snd.bin`, `pat.bin`, `pat2.bin`) are placed into dead stock code. Hook targets are read from
  `neptune-patches/ccbtn.elf` with `arm-none-eabi-nm`.
- `.syx` packing/unpacking: `tools/mk4_syx_tool.py` (MK4 model ID 0x13, round-trip verified).

## Flash
```
.tools/venv/bin/python -u -I tools/flash_confirmed.py firmware/<file>.syx --wait 16 --retries 3
ioreg -l -w0 | grep kUSBString        # the HID interface string must end with the new TAG
```
- Hands-free: host sends the `71` + `7C` header once; the device confirms itself after a 3 s prompt delay, sends
  `72 01 00 00`, acks every data packet (`72 <lo> <hi> 00`, ~6038 of them) and ends with `73 7F 7F 7F`.
- A flash counts only with the acks, the final status and the new tag. The image is staged and installed after a verified
  transfer; a cancelled or corrupt transfer leaves the running firmware intact.
- `43 00` is not part of the procedure and no longer exists in NP5+.
- Wait ~12 s after a flash before running other MIDI tools.

## Patches in NPE
| File offset | Change | Effect |
|---|---|---|
| 0xB16A | "HID" → 3-character tag | build id readable from the host (USB HID interface string) |
| 0x26BD4, 0x26BDB, 0x26C6A, 0x2A06B | menu strings → NEPTUN / NeptuneFW / NEPTUNE! / Neptune Upgrade! | on-screen marker (hidden once the display lock is in) |
| 0xA748 | `bl 0x107CE` → `b 0xA77A; nop` | **display lock**: the compositor draws only layer 0 (host stationary bitmap) or blank |
| 0xC692 | `cbnz r0,0xC6C0` → `b 0xC6C0` | **update auto-confirms** after 3 s (hands-free flashing) |
| 0x29052, 0x29866 | stock `43 00` handler and its erase routine replaced (`dbg.S`) | **debug SysEx `7D`**: call addr(a0, a1) → r0 |
| 0x1AC6E | `cbz r0,0x1ACA0` → `b 0x1ACA0` | Shift never claims pads: host pad colours stay, Shift+pad sends the pad note, Shift still sends CC 63 |
| 0x1C366 | `bl claim(0x1A,0x10)` → `nop nop` | Settings menu cannot open |
| 0x1B7FE | `bl claim(0x1E,0x01)` → `nop nop` | Arp button no longer toggles the firmware arp |
| 0x195C8 | `bl claim(0x42,0x0F)` → `nop nop` | Scale button no longer toggles firmware scale mode |
| 0x29020, 0x1A8A8, 0x1A6DC, 0x1A6E0 | stock `43 01` handler disabled; press/release stubs → `ccbtn.S` | Settings / Arp / Scale send CC 0x6C / 0x6D / 0x6E on the DAW port |
| 0x1A794, 0x19160 ↔ 0x1A700, 0x1A8CC, 0x1A974 | `pad_at` in the dead `43 01` prompt UI; registered as the first press listener | pad aftertouch enabled for DAW-layout pads from the first press |
| 0x1A80C, 0xE0AC, 0x1A808, 0x1A862 | `pad_value` pressure listener (chains to stock 0xDE74), `pad_release` | pressure rainbow on held pads; host colour restored on release |
| 0x10D6A | `bl 0x10786` → `movs r0,#1; nop` | **early display ready reply** (NPE): the reply is sent on the main-loop pass after a frame is latched, not after the push to the OLED; the host streams the next frame meanwhile |
| 0x1053A | `cbnz r0,0x10556` → `b 0x10556` | **no pop-up redraws** (NPE): the layer check never accepts layer 1, so encoder / control pop-ups no longer force a redraw of the host frame |

Full detail (stock bytes, sizes, verification evidence) is in `docs/launchkey-mk4-firmware-map.md` section 0.8 and in the build script.

## Host Protocol Additions (relative to stock)
- **Debug SysEx:** `F0 00 20 29 00 7D <addr:5><a0:5><a1:5> F7` → the device calls addr(a0, a1) and replies
  `F0 00 20 29 00 7D <r0:5> F7` (5×7-bit, MSB first; addr needs the Thumb bit). A bad address faults and the device resets.
  Host tool: `tools/nepdbg.py` (`call`, `peek32`, `poke32`, `read_mem`).
- **Buttons:** Settings = CC 108, Arp = CC 109, Scale = CC 110, channel 1, DAW port, 127 on press / 0 on release.
- **Pad aftertouch:** with feature 0x6C = 2, DAW-layout pads send `A0 <note> <pressure>` on channel 1, DAW port
  (0–127, sent on change, up to ≈75 messages/s).
- **Pressure rainbow:** while a DAW-layout pad is held its LED goes red→orange→yellow→green→blue→indigo→violet with the
  aftertouch value (red on press); the host-set colour returns on release. Always on.
- **Removed:** `43 00` and `43 01` SysEx commands; the host "temporary" display target (0x21) no longer shows; firmware
  pop-ups, menus and the Shift pad layer never appear.
- Everything else is the stock DAW-mode protocol (Programmer's Reference): bitmap SysEx to the stationary display, pad
  colours, DAW-mode handshake. Full button/CC map: `docs/launchkey-mk4-control-map.md`.

## Feature Controls the App Relies On
Sent as `B6 <cc> <value>` (channel 7) on the DAW port.
| CC | Meaning | Value the app uses | Notes |
|---|---|---|---|
| 0x45 | relative encoder output | 127 | encoders send CC 0x55–0x5C ch16, value 64 ± steps. Volatile — send on every connect |
| 0x64 | Keys Channel (0-based) | 0 (channel 1) | non-volatile |
| 0x6C | Pad Aftertouch | 2 (polyphonic; 0 = off, 1 = channel pressure) | non-volatile |
| 0x6E | MIDI Clock Out | 0 (off; stock default is on) | non-volatile |
| 0x1D | pad layout | 2 (DAW) | reads 13 when the stock firmware arp takes the pads; 1 = Drum layout |
| 0x1E | encoder layout | 2 (plugin: CC 21–28 ch16 when not in relative mode) | |

## Safety Rules
See `docs/launchkey-mk4-firmware-rules.md` (flashing, patching rules, what not to do). In short: verified
flashes only; never flash scorched / displaylock-v7 builds; a build that breaks USB or the update handler cannot be
recovered remotely and the user will not do manual bootloader entry; stay out of the no-touch areas when nobody is at the
device; hook addresses come from the linked ELF; pressure-listener hooks must chain to stock 0xDE74.

## Open Items
- On/off switch for the pressure rainbow.
- Free code space is nearly used up (`43 00` cave 88/94, `43 01` handler 50/50, prompt UI 72/116, prompt callbacks 84/86,
  helper 32/36); new features need new dead code or an appended region.
- Map gaps: settings menu → feature-CC mapping (only 0x64 and 0x6C verified), controls 0x28–0x30, modules
  0x06/0x07/0x09/0x0A, encoder modes / bank buttons, LED driver, settings storage.
- Encoder direction and feel in relative mode not yet confirmed by hand.
