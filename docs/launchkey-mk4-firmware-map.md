# Firmware RE Map — Novation Launchkey Mini MK4

**Binary**: `firmware/launchkeyminimk4-firmware-1563.syx.bin` (stock 1563, 193,203 bytes) · **MCU**: STM32F4 (ARM Cortex-M4, Thumb-2)

> The whole document uses **file offsets**. True load base is **0x08008000** (VMA = file offset + 0x08008000).
> Only values written as full 32-bit addresses (`0x0800xxxx`/`0x0803xxxx` flash, `0x2000xxxx` RAM, `0x4000xxxx` peripherals)
> are absolute. Older versions of this map (sections 1–15, wrong base) and the older per-topic notes are in git history.

## 0. Verified Map (iteration 50) — file offsets, true base 0x08008000

Evidence level: everything here was read from disassembly of stock 1563 with the correct base **and**
confirmed on hardware (verified flash + camera/MIDI/memory readback), unless marked *(code only)*.

### 0.1 Image layout
| Item | Value |
|---|---|
| Load address | 0x08008000 (reset handler at file 0x2B480 writes VTOR = 0x08008000) |
| Image size | 0x2F2B3 bytes → 0x08008000–0x080372B3 |
| Bootloader | 0x08000000–0x08007FFF (32 KB, not in the image) |
| Vector table | file 0x0000, 101 entries; first code at file 0x194 |
| Fault handlers | file 0x2AA8A–0x2AAA8: each loops calling reset-with-magic (file 0x13AEE) |
| TIM1_UP / TIM2 ISRs | file 0x90DC / 0x7C4C (in the image — earlier notes wrongly said "in bootloader") |
| I2C1/I2C3 EV/ER, DMA1_S4 vectors | `b .` / `bx lr` stubs → those buses are polled, not interrupt-driven |
| Font tables | glyph index at 0x0802DF94 (file 0x25F94), glyph data base 0x0800E600 (file 0x6600), count at file 0x2EA6C (=640) |
| USB strings | file 0xB080… ("Launchkey Mini MK4 @@", "… HID" at 0xB154; "@@" is replaced at runtime) |
| On-screen strings | file 0x26A00–0x27200 (menus), "Entering Upgrade" at file 0x2A06B |

### 0.2 Buses and peripherals
| Bus | Pins / config | Device | Driver |
|---|---|---|---|
| **I2C3** 0x40005C00 | PA8 SCL, PC9 SDA, 400 kHz, DMA TX (DR literal 0x40005C10) | **OLED**, slave 0x78 (0x3C 7-bit) | init 0x86F8; bus object ctor 0x9E62 (addr 0x78 passed at 0x81F2); start-transfer 0x9F48(obj, buf, len); state machine 0x9F64 |
| **I2C1** 0x40005400 | PB8/PB9, 100 kHz, PB4 = reset line | sensor, slave 0x10/0x11, 2-byte register reads into 0x2000E494 — almost certainly the touch strips | init 0x867E (re-run on bus error from main loop); state machine 0xF154 / 0xF310; getters 0xF0FA–0xF112 |
| External SPI NOR flash | driver object at [0x2000F4F0]; cmds 0x03 read, 0x02 program, 0x20/0x52/0xD8 erase | settings / staged update | 0x9AB0 (erase range), 0x9B74 (read), 0x9BD2 (busy poll) |

### 0.3 Main loop
Body at file 0x83FA–0x8542. Per pass it calls, among others: I2C1 error check/re-init (0xF0E8 → 0x867E),
display pump 0xA44C via the trampoline 0x8576 (many times per pass), upgrade UI task 0xC5F4 (call at 0x8460),
display commit 0xA414 (call at 0x84FE), contrast path 0xF65E, compositor 0xA682 (when 0xE254 says so).

### 0.4 Display pipeline (OLED)
```
draw primitives 0xA194..0xA3C0  → push opcode bytes (0x8A94) into FIFO at dev+4
commit 0xA414 (once per loop)   → moves FIFO to dev+0x14, sets dev[0x2DD/0x2DE]
pump 0xA44C                     → interprets opcodes (tbb at 0xA572), renders via 0x102E0,
                                  sends pixels with I2C3 start-transfer (calls at 0x10470 / 0x104EE)
contrast 0xF65E                 → 4-byte command 80 81 80 <level> via 0x9F48
```
Display device object: base+0x145C, pointer at [0x2000F4D8]; `dev[0x2E0]` = "direct UI owns the screen" flag
(set by 0xF652; used by the upgrade UI at 0xC734).

**Display manager** (state at 0x2000035C): flag bytes [0]..[4],[6] = layer active, [5] = dirty, [8] = display on,
[0xC] popup timer, [0x10] popup id. `show(layer, type)` = 0x105C0; `popup(id, type)` = 0x1061C;
per-layer content type at 0x2000ED54+0x28+4·layer.

**Compositor** (0xA682; chooser at 0xA744–0xA786) picks by priority:
flag3 → layer 3, flag1 → layer 2, flag4 → layer 5, popup timer → layer 1, flag0 → layer 4, flag2 → **layer 0**, else blank.
Layer 0 = host stationary bitmap; layer 1 = host temporary bitmap and firmware timed popups.

**Host bitmap path:** SysEx handler at 0x28A34 (DAW header check 0x202EA; cmd byte [6]: 04 configure → 0x285BC,
06 text → 0x22902, **09 bitmap → 0x10CAC**, 6F). 0x10CAC unpacks 19×64 7-bit bytes into the page buffer at
[0x20000028] (128 bytes per 8-row page) then `show(0 or 1, 8)`.

**Timing and the ready reply (static analysis + device probes, 2026-11-11):**
- A host frame is one draw opcode pushed as 8 I2C transactions (17 command bytes + 128 data bytes per page; the renderer at 0x102E0 has a 148-byte buffer). DMA carries the data; a polled state machine (0x9F64, about 8 polls per main-loop pass) handles START, address and STOP. The push takes about 33 ms at 400 kHz (I2C3 CCR 0x8023, TRISE 0x0D; the literal 400000 is at file 0x890C). No display tick or deliberate delay exists anywhere in the path
- The controller is SSD1306-class (init `A1 C8 8D 14 AF`, addressing commands 20 / 21 / 22; oscillator and multiplex at reset defaults)
- Bitmap SysEx handler 0x28A34 (command 09) → 0x10CAC: target must be 0x20 or 0x21 and length at least 1216; unpacked bit by bit into the single page buffer at [0x20000028]; identical frames are not detected
- The ready reply is sent from 0x10D5A (called once per pass at 0x8492). Stock condition: 0x10786 → 0xA3F8 (staging FIFO empty and dev[0x2DD] == 0), i.e. the whole push finished. NPE replaces the test (section 0.8)
- Layer check 0x10530; dirty flag 0x2000035C+5; any dirty flag redraws the whole chosen layer. Encoder and control pop-ups call show(1, type) (0x105C0); the pop-up timer (0x2000035C+0xC) reloads to 5000 ms on every step and its expiry still causes one redundant push 5 s after the last turn (a NOP of `bl 0x107F6` at 0xA6BE would remove it; not applied)
- USB: TinyUSB on the OTG FS core (ISR 0x11D7C, device task 0xB360, MIDI class driver around 0xF90C-0xFB94). 64-byte MIDI RX FIFO, re-armed only when empty, from the device task, which runs twice per pass (0x8434, 0x84AE); a main-loop pass is about 2.5-3 ms; a 1,216-byte SysEx (26 packets) takes about 38 ms to receive
- The MIDI clock (F8, 48 a second) seen on the MIDI port in recordings up to 2026-11-11 was the device's own "MIDI Clock Out" setting, on by default; see section 0.9 (MIDI clock and transport). It is off now

### 0.5 Firmware update (in-app, staged)
State machine at 0x2000F184 (state in [+0x10]); SysEx handler 0x28E00 (cmd byte [5]):
| Msg | Handler action |
|---|---|
| `71` | 0x13918: state = 1, remember reply context |
| `7C` | 0x1392C: store header, state = 2 (awaiting confirmation) |
| `72` data | 0x139F6: write 0x20 bytes to staging via 0x8CA6, ack with `72 <ctr>` (0x13894) |
| `73` final | last block; then 0x13A70 verifies (0x2194E) → state 8 (reply `73 7F`) or 9 (`73 7E`) |
Upgrade UI task 0xC5F4: sends `73 00` on entry; for 3000 ms any pressed control cancels (`73 10`); then sends
`73 01` and waits for control 0x10 (button right of the top pad row) → `0x139B8(1)` confirm → state 3 →
device sends `72 01 00 00`. No press within ~13 s → `73 11`, cancel. After success the device resets
(0x13AEC → plain NVIC reset) and the bootloader installs the staged image. A cancelled or corrupt transfer
leaves the running image untouched. The version shown on the upgrade screen is the installed one.
The separate "Launchkey Mini MK4 Bootloader" USB device seen in the official capture is a different path
(real bootloader); it has not been needed.

`43 00` (stock handler 0x29052 → 0x29866): erases external-flash 0x6000–0x7FFF and 0x60000–0x7FFFF, then
reset-with-magic. Not needed for updating. **Removed in NP5+** (space reused for the debug command).
`43 01 xx` (handler 0x29020 → 0x1A8A8): 15 s scheduled-reboot prompt using controls 0x16/0x17 *(code only)*.

### 0.6 SysEx handler chain
Dispatcher around 0x28340–0x28490 calls each handler as `h(r0 = reply ctx, r1 = data incl. F0, r2 = len)`
after `bl 0x2833A`; a handler returns 1 to stop the chain. Header checks: 0x202D6 (00 20 29 …),
0x202EA (DAW header 00 20 29 02 13 …). Reply helpers: 0xC078(ctx) emits F0 00 20 29; 0xBF8E(ctx, byte); 0xC092(ctx) emits F7.
Host port lists (linked lists walked by `0xBF6E(list, status, d1, d2)`, the 3-byte MIDI send): **[0x2000EE24+0x1C] = DAW port**, **[0x2000EE24+0x20] = MIDI port** (measured). `0xE0C4(id, vel)` sends a note on both.
Known handlers: 0x28A34 display (04/06/09/6F), 0x28E00 update (71/72/73/7C), 0x29020 `43 01`, 0x29052 `43 00`
(NP5+: debug `7D`), 0x2907E `78` (LED colour batch, DAW header), 0x291C6 `0A`, 0xF4EC `05`,
0x284DA/0x2921A/0x29284/0x2939A/0x29588/0x295CE/0x296F8/0x29764 = `70 xx` sub-commands (factory/test) *(code only)*.

### 0.7 Controls
**Control ids** (measured on hardware with the user pressing each control):
| id | Control | Host MIDI (DAW mode) |
|---|---|---|
| 0x00–0x07 | pads, top row L→R | note 0x60–0x67 ch1; NPB with feature 0x6C = 2: poly aftertouch ch1 |
| 0x08–0x0F | pads, bottom row L→R | note 0x70–0x77 ch1; NPB: poly aftertouch likewise |
| 0x10 | button right of top pad row | CC 0x68 (104) ch1, DAW port |
| 0x11 | Func (right of bottom pad row) | CC 0x69 (105) ch1, DAW port |
| 0x19 | Shift | CC 0x3F ch7 (127/0) |
| 0x1A | Settings | stock: none (opens firmware menu). **NP9: CC 0x6C (108) ch1, DAW port, 127/0** |
| 0x1E | Arp | stock: none (toggles firmware arp; module 0x01 then claims encoders + bank buttons, module 0x02 claims pads, 0x10, 0x11; host sees CC 0x1D ch7 = 13). **NP9: CC 0x6D (109) ch1, DAW port, 127/0** |
| 0x20 / 0x21 | Oct − / Oct + | handled in firmware (module 0x0B); no host message seen |
| 0x22 / 0x23 | Up / Down | CC 0x6A / 0x6B (106/107) ch1, DAW port *(channel inferred from Func)* |
| 0x24 / 0x25 | encoder bank down / up (small buttons right of encoders) | CC 0x34 / 0x33 |
| 0x38–0x3F | encoders 1–8 | default: CC 0x15–0x1C (21–28) on ch16, DAW port, running absolute value. With feature 0x45 on: CC 0x55–0x5C, value 64 ± steps (what the app uses) |
| 0x42 | Scale | stock: none (toggles firmware scale mode; module 0x0F claims encoders + bank buttons, module 0x02 claims pads). **NP9: CC 0x6E (110) ch1, DAW port, 127/0** |
| 0x48 / 0x4A | Play / Record | CC 0x73 / 0x75 |
| 0x28–0x30 | 9 controls mapped 0x0B–0x13 (not yet identified) | |
| 0x80–0x98 | the 25 keys (note = id − 0x80 + 48 at octave 0), read via 0xFEB8; rest of 0x80–0xFF unused on the Mini | notes on the MIDI port |
Control type per id: byte table at file 0x2A17C, bits 4–6 (1 button, 2, 3 encoder).

**Raw readings** (`0x135D4(id)` → bits 12–15 = sensor type, bits 0–11 = value; measured with the user operating each control):
| Control | Type nibble | Raw value |
|---|---|---|
| Pads 0x00–0x0F | 1 | 12-bit pressure. Rest ≈ 0x003–0x00B (noise), hard press ≈ 0xEE0–0xF30. Pads are pressure sensors, not switches |
| Buttons | 0 | 0 released / non-zero pressed (0xFFF works for simulation) |
| Encoders 0x38–0x3F | 4 | 2-bit quadrature phase 0–3 (changes by one per detent) |
| Keys 0x80–0x98 | 3 | 12-bit key travel. A 1 Hz poll saw no change while keys were played (too slow to catch it); simulation with 0xFFF triggers the note |

**Keybed ids:** keys are control ids **0x80–0x98** (note = id − 0x80 + 48 at octave 0), raw type 3. Simulating one (`0x1362E(0x80+k, 0xFFF)`) sends the note-on; the note-off is only sent when simulation mode is switched off. Feature 0x64 = Keys Channel (0-based; verified 0 → ch1, 2 → ch3).

**Keybed scanner:** separate scanner (velocity code around file 0x126F6–0x12AF0, tables 0x2000D410 / 0x2000D510 / 0x2000DD04) *(code only)*.
Measured output: lowest key = note 48, highest = note 72 (25 keys, octave shift 0), on the MIDI port (not the DAW port). The channel is feature 0x64: channel 3 as found (0x64 = 2); the app sets 0x64 = 0, channel 1.

**Touch strips** (I2C1 sensor, state struct 0x2000EBE4; upper 16 bits of each word are a timestamp):
| Field | Meaning (low 12 bits) | Host MIDI |
|---|---|---|
| +0x10 | pitch strip position 0x000–0xFFF; **snaps back to 0x800 (centre) on release** | pitch bend, MIDI port, ch3 as found (−8192 … +8191, returns to 0) |
| +0x14 | mod strip position 0x000–0xFFF; **holds its last value** on release | CC 1, MIDI port, ch3 as found (0–127) |
| +0x18 | pitch strip touched (0xFFF while a finger is on it, else 0) | |
| +0x1C | mod strip touched (0xFFF / 0) | |
Getters: 0xF0FA (+0x10), 0xF102 (+0x14), 0xF10A (+0x18), 0xF112 (+0x1C).

**State tables (RAM):** 0x2000D110[id] halfword (0xFFFF = pressed); 0x2000E214[id] byte (3 = held);
0x20008170[id] 32-bit host MIDI mapping word (e.g. 0x20E00060 = pad note 0x60; 0 = no host mapping);
per-control descriptor (0x14 bytes) via 0x161AA(id); 0x2000A134[id] = owning module number.

**Scan and dispatch:** raw read 0x135D4(id) → hardware 0x9414(id). Scan loops (buttons ~0x1301C–0x13168,
pads 0x12D5E–0x12F6C) call listener lists: 0x13530 press (list 0x2000F024), 0x1354C (0x2000F048),
0x13574 release (0x2000F06C), 0x13592 value (0x2000F090). Each list = count byte + up to 8 function pointers.

**Listener polarity:** list 0x2000F024 (via 0x13530, r1 = value) fires on **press**; list 0x2000F06C (via 0x13574) fires on **release**. Stock registers two no-op stubs there (file 0x1A6DC press, 0x1A6E0 release; both `b.w 0x1E858`, a getter) — NP9 uses them as hooks.

**Mapping word format** (0x20008170[id]): top 3 bits 001 = valid; bits 16–23 = class (0xE0 note, 0xE2 drum-layout note, 0xE4 encoder, 0xE6 button CC, 0xF1 14-bit, 0x10–0x60 CC families); bits 0–15 = number. Value-change generators: 0x1809A (class E4), 0x182EA (class F1), 0x18716 (classes 0x10–0x60), all called from the generic value listener 0xDEC4.

**Host mapping at runtime:** an unowned control whose mapping word (0x20008170[id]) and descriptor are filled in sends MIDI through the generic path. Proven by copying Func's descriptor to Settings and changing byte 0 (the CC number). The mapping words are built by the layout engine (file ~0x17A00–0x18C50), not stored as constants.

**Input simulation (stock, factory-test style):** `0x13660(on)` sets flag 0x2000F516 and clears the override
table 0x20009170; `0x1362E(id, raw12)` sets a control's raw value (0xFFF = pressed). While on, real controls
read as released. Side effect on entry: every raw value becomes 0, so the host sees pitch bend −8192, CC 1 = 0 and the encoder CCs jump; they return on exit.

**Module ownership:** `claim(id, module)` = 0x1D7BC, `claim_list` = 0x1D7CC, `release(id, module)` = 0x1D770,
`owned_by(id, module)` = 0x1D7A6. A module only handles and paints controls it owns.
| Module | What it is | Owns when idle (DAW mode) |
|---|---|---|
| 0x01 | Arp — refresh 0x1B7E4 | 0x1E (and 0x60); when arp is on: encoders, 0x24, 0x25 |
| 0x02 | pad-mode layer used with Arp/Scale | when active: pads, 0x10, 0x11 |
| 0x04 | DAW | 0x10, 0x11, 0x22, 0x23, 0x40, 0x41 |
| 0x0B | Octave | 0x20, 0x21 |
| 0x0E | **Shift** — refresh 0x1AC5E, release 0x1ACA2, paint 0x1AA88, held flag 0x2000F514 | 0x19; while held also pads 0x00–0x0F, 0x28–0x2C, 0x2D–0x30, keys 0x80+ |
| 0x0F | Scale — refresh ~0x195B8 | 0x42, 0x12; when scale mode is on: encoders, 0x24, 0x25 |
| 0x10 | **Settings** — refresh 0x1C360, exit 0x1C3AC, active flag 0x200010E8+8 | 0x1A; while open also pads, 0x10, 0x11, 0x22, 0x23, 0x28–0x30, encoders, 0x40, 0x41 |
| 0x05 | `43 01` reboot prompt | 0x16, 0x17 |
| 0x07, 0x09, 0x0A, 0x06 | other modes (not yet mapped) | 0x12, 0x13, 0x43 … |

**Keyboard notes, octave and transpose (measured on NPD, 2026-11-10; used by `src/controller/KeyboardSplit.h`):**
octave = signed word at 0x2000F440 (copy 0x20001038); transpose in semitones = 0x2000F444 (copy 0x2000103C, and byte 1
of the word at 0x200015BC). Shift + Oct −/+ is the firmware's transpose. A key's note is the low byte of its mapping
word (0x20008170 + 4·(0x80 + key) = 0x21000000 | note); the firmware rewrites all of them on an octave or transpose
press from the per-key records (20 bytes each from 0x2000371C, note as a word) and the 128-byte note table at
0x2000E194, and again from the records when the host connects. Poking a mapping word changes the note a key sends.

### 0.8 Neptune patches (all in `tools/build_neptune_fw.py`; running build = NPE)
| File offset | Change | Effect | Verified |
|---|---|---|---|
| 0xB16A | "HID" → 3-char tag | build id readable from host (`ioreg … kUSBString`) | yes |
| 0x26BD4, 0x26BDB, 0x26C6A, 0x2A06B | menu strings → NEPTUN / NeptuneFW / NEPTUNE! / Neptune Upgrade! | on-screen marker (hidden once display lock is in) | yes (NP1) |
| 0xA748 | `bl 0x107CE` → `b 0xA77A; nop` | display lock: only layer 0 or blank is drawn | yes (user + camera) |
| 0xC692 | `cbnz r0,0xC6C0` → `b 0xC6C0` | update auto-confirms after 3 s (hands-free flashing) | yes |
| 0x29052 (40 B), 0x29866 (88 B) | debug SysEx `7D`: call addr(a0,a1) → r0 (`neptune-patches/dbg.S`) | peek/poke/call from host | yes |
| 0x1AC6E | `cbz r0,0x1ACA0` → `b 0x1ACA0` | Shift never claims pads: host pad colours stay, Shift+pad sends the pad note, Shift still sends CC 63 | yes (camera + MIDI) |
| 0x1C366 | `bl claim(0x1A,0x10)` → `nop nop` | Settings menu cannot open | yes (camera) |
| 0x1B7FE | `bl claim(0x1E,0x01)` → `nop nop` | Arp button no longer toggles the firmware arp | yes (ownership unchanged on press) |
| 0x195C8 | `bl claim(0x42,0x0F)` → `nop nop` | Scale button no longer toggles firmware scale mode | yes |
| 0x1A794 (72 B incl. `daw_pad` and `pad_release`), 0x19160 ↔ 0x1A700, 0x1A8CC, 0x1A974 | `ccbtn.S` `pad_at`: on every pad press, if the pad's mapping class is 0xE0 (DAW note), write `10 40` to descriptor +0x10. The two registration literals are swapped so the Neptune press stub is the FIRST press listener. Code sits in the dead `43 01` prompt UI; its press handler (0x1A8CC) and timer (0x1A974) are stubbed to `bx lr` | pad aftertouch in the DAW layout, from the first press | yes (simulation + user's finger, NPB) |
| 0x1A80C (84 B), 0xE0AC, 0x1A808, 0x1A862 | `ccbtn.S` `pad_value`: the pressure-listener registration literal (0xE0AC) points at `pad_value`, which paints the pad red→orange→yellow→green→blue→indigo→violet by aftertouch value (index = value·7 >> 16) and then always chains to stock 0xDE74. `pad_at` paints red on press; `pad_release` (from the release stub) calls `led_mode(id, 3)`. DAW-layout pads only. Code sits in the stubbed `43 01` prompt callbacks | pressure rainbow on held pads, host colour restored on release, aftertouch unaffected | yes (camera: all seven colours + restore; MIDI: polytouch still sent) — NPD |
| 0x29020 (50 B), 0x1A8A8 (32 B), 0x1A6DC, 0x1A6E0 | `neptune-patches/ccbtn.S`: stock `43 01` handler disabled; press/release stubs → send CC 0x6C/0x6D/0x6E (Settings/Arp/Scale), status B0, on the DAW port list | those three buttons report to the host | yes (MIDI log, NP9) |
| 0x10D6A | `ff f7 0c fd` (`bl 0x10786`) → `01 20 00 bf` (`movs r0,#1; nop`) | early display ready reply: sent the pass after a host frame is latched instead of when the display is idle; the reply context is still cleared after the one reply | yes (NPE: reply 78 → 46 ms idle; two frames back to back give two replies) |
| 0x1053A | `60 b9` (`cbnz r0,0x10556`) → `0c e0` (`b 0x10556`) | layer check never accepts layer 1: firmware pop-ups no longer set content or the dirty flag | yes (NPE: 40 encoder steps/s, 0 of 52 frames timed out; 12 of 13 on NPD) |

### 0.8b LEDs (pad and button colours)
| Item | Value |
|---|---|
| `led_set(id, 0xRRGGBB)` | file 0x22612 → sets mode 0 (0x13C92(id, 0)) then stores the converted colour via 0x13C6E into 0x2000ADA4[id]. Shows immediately. Verified by camera with seven colours |
| `led_mode(id, mode)` | file 0x13C92, mode byte table 0x2000DF94[id]: 0 = solid firmware colour, 1 = 0x10, 2 = 0x20 (flash / pulse, *code only*), **3 = back to the host-set colour** (verified: host pattern returns) |
| Module LED bitmap | 0x2000F34C, set/cleared by 0x13D28(id, on) — not needed for `led_set` to show |
| Raw LED write | 0xF6C8(led_index, value) with shadow 0x20009570 (used by the upgrade animation) *(code only)* |
| Host colours | SysEx `F0 00 20 29 02 13 01 43 <pad note> <r> <g> <b> F7` (0–127 each) and note-on palette messages on the DAW port |

**Pressure listener list** 0x2000F048 (dispatched by 0x1354C from the pad scan): one stock entry, 0xDE74, which generates
aftertouch. Its r1 is NOT the raw pressure: it is the aftertouch-scaled 16-bit value, 0 at the aftertouch threshold
(internal pressure ≈ 0x5600, raw ≈ 0x5C0) rising linearly to 0xFFFF at raw ≈ 0xC80. No event is dispatched below the threshold.
Measured: raw 0x600 → AT 7, 0x700 → 26, 0x800 → 45, 0x900 → 64, 0xA00 → 82, 0xB00 → 101, 0xC00 → 120, ≥0xC80 → 127.

### 0.9 Feature controls and settings (host: `B6 <cc> <value>` on the DAW port)
Parameter store dispatch: get 0xEC50(class, id, &out), set 0xEC88; class 0xD0 = feature controls → get 0x1ED14, set 0x1ED64.
Tables (index = CC number 0–127): flags 0x2000E094 (bit 0 registered, bit 1 non-volatile, bit 2 = 14-bit pair with cc+0x20);
values 0x2000E014 (volatile) or 0x2000DD8C (non-volatile shadow, saved to external flash). Validators: list at 0x2000EA88
(5 functions: 0x15524, 0x1ECE4, 0x1F1BC, 0x1F59C, 0x1F6B0 as file offsets).

Registered on this unit (read from RAM on NP9, DAW mode; value in brackets):
- Volatile: 0x02–0x07 (14-bit; 0x03 = 60, 0x04 = 15), 0x1B [0], 0x1C [0], **0x1D pad layout [2 = DAW; 13 seen when firmware arp took the pads]**,
  0x1E [2], 0x1F [6], 0x3C [0], 0x3D [0], 0x3E [1], **0x3F Shift state [0/127, also reported to host]**, 0x44–0x47 [0],
  **0x49 / 0x4A (arp / scale feature switches used by the old Neptune code)** [0], 0x4B, 0x4C, 0x4D [0], 0x4E [60], 0x53 [21],
  0x54 [127], 0x55 [0], 0x56 [4], 0x57, 0x58 [0], 0x59 [50], 0x5C [0], 0x7A [1], 0x7B [1], 0x7C [0], 0x7D [0]
- Non-volatile (device settings): 0x4F [0], 0x64 [2], 0x65 [1], 0x66 [2], 0x67 [9], 0x68 [1], 0x69 [1], 0x6A [120], 0x6B [127],
  0x6C [0], 0x6D [1], 0x6E [127], 0x6F [99], 0x70 [99], 0x71 [40], 0x72 [0], 0x73 [127], 0x74 [1], 0x75 [6], 0x76 [6], 0x77 [0], 0x79 [1]

Settings menu order in the firmware strings (file 0x26C6A…): Keys Channel, Part A Channel, Part B Channel, Chords Channel,
Drums Channel, Arp Note Source, Vel Curve (Keys), Vel Curve (Pads), Fixed Velocity, Arp Velocity, Pad Aftertouch,
Pad AT Threshold, Pot Pickup, Fader Pickup Type, MIDI Clock Out, LED Brightness, Vegas Mode, Power On Modes,
External Feedback, Screen Brightness, Display Timeout, Encoders Curve.
**Verified so far:** 0x64 = Keys Channel (0-based), 0x6C = Pad Aftertouch (0/1/2), 0x6E = MIDI Clock Out (0 / 127). Likely: 0x67 = Drums Channel (9 ↔ channel 10). Novation's Programmer's Reference lists the feature controls by CC number (0x64–0x67 channels, 0x68 / 0x69 velocity curves, 0x6A fixed velocity, 0x6B arp velocity, 0x6C pad aftertouch, 0x6D aftertouch threshold, 0x6E MIDI clock output, 0x6F LED brightness, 0x70 screen brightness, 0x71 display timeout, 0x72 Vegas mode, 0x73 external feedback, 0x74–0x76 power-on modes, 0x77 fader pick-up); only the three named first were checked on the device. Host writes are accepted (0x6C read back 2, 1, 0 after `B6 6C xx`).

**MIDI clock and transport (disassembly of stock 1563 + device, 2026-11-11):**
- Tempo engine: state 0x20000104, task 0xBC60 (called once per main-loop pass at 0x8438); running 24-per-beat tick counter at 0x20000124 (getter 0xBBC6 returns its address). Internal tempo 120 BPM unless changed (feature 0x03:0x23, tempo control)
- Clock sender: 0xC492, called every pass at 0x843C; state 0x2000F35C: byte +0 = output enabled, byte +1 = held off (set by 0xC48C from the main loop while 0xE254 / 0xC5F0 / 0xE874 return non-zero; *code only*), +4 = pointer to the tick counter, +8 = port list, +0xC = last tick count seen. It sends one `F8` per elapsed tick through 0xBF6E on its port list, which is the **MIDI port list** (0x2000FE98 = [0x2000EE24+0x20]; init 0xC46C, called at 0x8382). Nothing goes to the DAW port (measured)
- The enable byte is written only by 0xC486, called only from the feature-control apply switch (case 0x6E at 0x16E82: non-zero → 1, value stored as 127; zero → 0). The settings defaults routine (the run of `set(0xD0, cc, &value)` calls through 0x17112; 0x6E at 0x1707A) writes **127 = on** for 0x6E. So a stock or freshly reset device sends clock from power-on, with or without a host; Neptune builds inherit that and their disabled Settings menu cannot change it
- **`B6 6E 00` on the DAW port switches it off** (measured on NPE: 96 `F8` in 2 s before, none in 5 s after; value table 0x2000DD8C+0x6E read 0x7F before, 0x00 after; enable byte 0x2000F35C read 0). Flag table says non-volatile and Novation documents it as kept across power cycles; the power cycle itself was not tried. The app sends it on every connect
- Stand-alone transport: 0x1A710 (press listener; state 0x2000F480: +0 active, +1 playing, +4 port list = MIDI port list). When active, Play (control 0x48) sends `FA` and `FC` in turn on the MIDI port. 0x1A752 makes it active only while the getter 0x1E858 returns 0; with the app connected in DAW mode the active byte read 0, so Play sends CC 0x73 only *(the FA / FC path is code only: not pressed on the device)*
- The MIDI message code around 0x25A30 knows the system bytes F1 / F2 / F3 / F8 / FA / FB / FC *(code only; direction not worked out)*; pad flashing and pulsing follow a received clock (Programmer's Reference). The app sends the device no clock

**Feature 0x45 = relative encoder output** (verified by simulation): with `B6 45 7F` the encoders stop sending absolute
positions on CC 0x15–0x1C and send **CC 0x55–0x5C on channel 16, value 64 + steps** (63 = one step down, 65 = one up).
Volatile, so the host must send it on every connect. Features 0x44–0x47 are on/off toggles (any non-zero value reads back 127):
0x44 on → absolute encoder CCs report 0; 0x47 on → extra CC 21 on channel 15 with 127/0 around each turn (looks like encoder
touch/activity reports); 0x46 no visible effect. Feature 0x1E (encoder layout): 2 = plugin (CC 21–28 ch16); 4 and 5 produced no CC 21.

**Feature 0x6C = Pad Aftertouch** (verified): 0 = off, 1 = channel pressure, 2 = polyphonic. Non-volatile. Host sets it with `B6 6C 02`.

**Pad velocity / aftertouch (verified by simulation AND by the user's finger):**
- Note-on velocity follows the pressure at onset (raw 0x180 → 3, 0x200 → 7, 0x300 → 14; firm real presses 21–28).
- Aftertouch is gated per control by **descriptor byte +0x10** (descriptor = 0x20002D1C + id·0x14): high nibble 1 = pressure
  becomes aftertouch. The Drum pad layout sets bytes +0x10/+0x11 to `10 40`; the **DAW pad layout leaves them `00 00`, so stock
  sends no pad aftertouch in DAW mode** whatever 0x6C says. The generic press handler (0xDC6C) copies the descriptor into a
  per-note slot (0x2000BE60 + slot·0x14) at press time, so the byte must be set before that handler runs.
- Drum layout for reference (feature 0x1D = 1, reads back 15): pads send notes 36–51 region (pad 0 = note 40) on channel 10.
- NPB result, DAW layout, 0x6C = 2: `A0 <note> <pressure>` on channel 1, DAW port. Real finger: values 0–127, 38–60 distinct
  values per press, sent on change, up to ≈75 messages/s while pressure moves. Raw→value: 0x700 → 26, 0xB00 → 101, 0xFFF → 127.

### 0.10 Host tools built on this map
| Tool | Purpose |
|---|---|
| `tools/build_neptune_fw.py TAG` | builds stock + all patches, tag in USB HID string |
| `tools/flash_confirmed.py file.syx --wait 16 --retries 3` | ack-verified update (hands-free on NP3+) |
| `tools/nepdbg.py` | call / peek32 / poke32 / read_mem over the debug SysEx |
| `tools/nepinput.py` | simulated button hold / tap / combo |
| `tools/oled_dump.py` | reads the page buffer at [0x20000028] and prints the 128×64 screen exactly |
| `tools/snap.sh`, `tools/snap_oled.sh` | camera still of the device / OLED crop |
| `tools/pad_colors.py pattern\|off` | known pad colour pattern for camera checks |
| `tools/listen.sh` | microphone level check |
| `tools/mk4_syx_tool.py` | .syx decode / encode / round-trip (format: `docs/launchkey-mk4-firmware-update-format.md`) |

### 0.11 History
- Before iteration 50 this map assumed load base 0x08030000, so every VMA was 0x28000 too high (file offsets were right), and it described a 192 KB bootloader (it is 32 KB).
- It put the OLED on I2C1 (it is on I2C3), so the "I2C flush gate" display work targeted the wrong bus; file 0x28E00 was taken for the bitmap handler (it is the update handler).
- Flashing was blind (`flash_cycle.py`, `43 00` as "bootloader entry", no ack checking), so results reported for the builds of iterations 44–49 are untested.
- The custom skeleton firmware was linked at 0x08030000; it must be linked at 0x08008000.

---

## Appendix A — unverified leads from earlier analysis (file offsets; not re-checked)

Carried over from the pre-iteration-50 notes (`firmware-re-clock/-usb/-pads/-encoders.md`, `firmware-research.md`,
`firmware-recovery-research.md` — deleted, in git history). Everything here was read from the image or from the web
before the correct base was known; addresses have been converted to file offsets. None of it is contradicted by
section 0, and none of it has been re-checked. Treat each item as a lead to confirm, not as fact.

- **MCU**: probably STM32F401 (84 MHz, 64 KB SRAM: initial SP 0x2000FFF8). Not confirmed by a teardown.
- **Reset literal pool** at file 0x13B00–0x13B10 (belongs to the reset-with-magic routine at 0x13AEE):
  0x2000FFF8 (magic location, also the initial SP), 0x9669A55A (boot magic 1, written to 0x2000FFF8),
  0x3CC3F00F (boot magic 2, written to 0x2000FFFC), 0xE000ED0C (AIRCR), 0x05FA0004 (VECTKEY | SYSRESETREQ).
- **Clock setup** function at file 0x857C: HSE 8 MHz → PLL (M=8, N=336, P=4, Q=7) → SYSCLK 84 MHz, USB 48 MHz;
  flash 2 wait states + caches (cache/prefetch init at file 0x8E3C); voltage scale 2; AHB /1, APB1 /2, APB2 /1.
  Main peripheral init function at file 0x7C90.
- **USB descriptors**: VID 0x1235; product-info block (VID, PID, bcdDevice 0x0180, string indices) at file 0x2F14E; no
  raw device descriptor in flash (built at run time). Descriptor analysis read PID 0x0356 but the running device
  enumerates as PID 0x0141 — not reconciled. Configuration 1 at file 0x29F54 (128 bytes: AudioControl + MIDIStreaming
  with 2 virtual cables on bulk EP1 IN/OUT, 64 bytes + HID on interrupt EP2 IN); a second configuration at file 0x29FD4
  adds Mass Storage ("Easy Start Drive", EP2 OUT / EP3 IN). Strings (ASCII, "@@" replaced at run time): manufacturer
  0xB068, product 0xB080, "… MIDI" 0xB098, "… MIDI Int" 0xB0B4, "… MIDI In" 0xB0D4, "… MIDI Out" 0xB0F4, "… DAW In"
  0xB114, "… DAW Out" 0xB134, "… HID" 0xB154 (section 0.1), serial template "Novation LK: …" at 0xAC18.
- **Pad / key sensing**: GPIO matrix scan on GPIOC (8 rows × 12 columns = 96 positions; scanner at file 0xFE34, scan
  init 0x92E4, state buffer 0x2000F1D0); pad pressure through ADC1 with DMA2 stream 0 and an analog multiplexer
  cycling 8 groups of 2 channels (mux step 0xFCFC, DMA read-back 0xFD40, setup 0xFD78, scaled read 0xFDDA; DMA buffer
  0x2000E414). Key velocity from two-contact timing (0xFEB8, consistent with section 0.7). This agrees with the
  measured behaviour in section 0.7 (pads are pressure sensors) but the code itself was not re-read.
- **SPI1** (PA5/6/7, DMA2 streams 2/3) and **SPI2** (PB13/14/15, DMA1 streams 3/4) are set up as masters with DMA in
  the init function at file 0x7C90. The old note guessed the encoders are read over SPI; that was never established
  (section 0.7 shows them as 2-bit quadrature readings through 0x135D4). What the SPI buses serve (external flash,
  LEDs, …) is open.
- **Other peripherals referenced by literals in the image** (purpose not established): USART1 0x40011000,
  USART2 0x40004400, TIM1–TIM5, DMA1 0x40026000, DMA2 0x40026400.
- **Source file names in assert strings** (useful for orientation): `ui_mode.cpp`, `ui_menumgr.cpp`,
  `inputmidi_listener.cpp`, `io_listener.c`, `midiobj_feature.cpp`, `midiobj_backend.cpp`, `feature_arp.cpp`,
  `feature_chord.cpp`, `feature_scale.cpp`, `scale.cpp`, `feature_settings.cpp`, `displayproto.cpp`, `hui_state.cpp`,
  `layout_channel.cpp`, `layout_devconf.cpp`, `confstore.cpp`, `confstore_sec.cpp`, `extdata_upgrade.c`,
  `muxio_hw_data.c`, `inputrelative.cpp`, `serstor_hw.c`.
- **Host queries seen in the Novation Components capture** (`launchkeyofficial/launchkey-midi-capture.json`):
  `F0 00 20 29 00 42 02 F7` → ASCII serial number; `F0 7E 7F 06 01 F7` → identity reply;
  `F0 00 20 29 00 70 F7` → 50-byte version/capability structure.
- **Manual recovery (Novation's documented procedure; never exercised on this unit, and the user will not do it):**
  hold Octave + and Octave − while connecting USB → bootloader ("Easy Start" status and version numbers on the OLED);
  reinstall with Novation Components ("Reinstall firmware"); Play leaves the bootloader, Record toggles Easy Start.
  The STM32 ROM (DFU) bootloader is not expected to be usable (read-out protection is probably set; no BOOT0 or SWD
  access known; no MK4 teardown found).
- **Update file format** (.syx: `71` start, `7C` metadata, `72` data, `73` end; model id 0x13; 7-bit packing; CRC32):
  verified, documented in `docs/launchkey-mk4-firmware-update-format.md`, implemented in `tools/mk4_syx_tool.py`.
- **External references**: KernelPicnic "Reverse engineering a Novation FLKey" (update protocol on related hardware);
  `xkey` (PyPI; decodes Novation update files, its encoder does not support the MK4);
  `anthonyhfm/launchpad-injection-cfw` (binary patching of stock Novation firmware, the closest precedent to the
  Neptune patches). No other Launchkey MK4 firmware modification was found (search of 2026-11-09).
