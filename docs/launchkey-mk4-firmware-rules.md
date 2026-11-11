# Launchkey Mini MK4 — firmware rules: facts, flashing, patching, tools

Rules learned while mapping and patching the Launchkey Mini MK4 firmware. Each one is here because breaking it
cost something. Read them before flashing anything. The map itself is
[launchkey-mk4-firmware-map.md](launchkey-mk4-firmware-map.md).

## FIRMWARE: FACTS (verified on hardware — full map: `docs/launchkey-mk4-firmware-map.md` section 0)
- The stock app image loads at **0x08008000** (reset handler at file 0x2B480 sets VTOR = 0x08008000). `true VMA = 0x08008000 + file offset`. Any note that uses base 0x08030000 is 0x28000 too high
- The bootloader is 32 KB (0x08000000–0x08007FFF), is not in the image and must NEVER be modified
- **The OLED is on I2C3** (PA8/PC9, 400 kHz, DMA TX, slave 0x78 = 0x3C 7-bit). I2C1 (PB8/PB9, 100 kHz, slave 0x10/0x11, PB4 reset) is a sensor read in 2-byte registers — almost certainly the touch strips. File 0x867E is I2C1 *init* (re-run on bus error), not a display poller
- Firmware modules claim controls via `claim(id, module)` (file 0x1D7BC); ownership table 0x2000A134. A module only handles and paints what it owns
- Two USB port lists: [0x2000EE24+0x1C] = DAW, [+0x20] = MIDI. Host-control messages belong on the DAW list only
- Running build is **NPE** (`tools/build_neptune_fw.py`; NPD + early display ready reply + no pop-up redraws; rollback image `firmware/neptune-fw-NPD.syx`): display lock, hands-free update, debug SysEx `7D`, no firmware Shift layer / Settings menu / Arp / Scale modes, Settings/Arp/Scale = CC 108/109/110 ch1 on the DAW port (127 press, 0 release), pad aftertouch in the DAW layout, pressure rainbow. The old Arp/Scale scheme (feature-control suppression, "re-suppress per press", CC 73/74 ch7) is obsolete on this firmware
- **Display timing (NPE, measured 2026-11-11):** a host frame costs about 38 ms of USB receive (1,216-byte SysEx, at most 2 packets per main-loop pass) and about 33 ms of I2C push (8 page transactions at 400 kHz). The ready reply `F0 00 20 29 02 13 09 F7` now means "frame received and latched", sent about one loop pass after the SysEx ends; the picture is complete on the glass about 35 ms later. The host sends the next frame on the reply (latest frame wins): 43-50 ms per frame, 21-23 frames a second, also while encoders turn. A screenshot or "is it shown" check must wait at least 40 ms after the last reply
- One frame buffer: a frame latched during a push can tear at 8-row page granularity for one push; the newer frame is pushed in full straight after. Never corruption
- Firmware pop-ups (display layer 1, and the host "temporary" target) no longer touch the display on NPE. On NPD every encoder or control event marked the screen dirty and, with the display lock, forced a full redraw of the host frame (about 35 ms of I2C per event); the old ready reply waited for an idle display, so it was withheld for as long as a knob kept turning (the "laggy filter sweep")
- USB receive is now the bottleneck of the display; overdriving the I2C clock gains nothing. Speeding USB up means servicing the USB task more often per loop pass (next to no-touch areas: only with someone at the device); partial updates need about 100-140 bytes of code that the caves do not have
- Display lock (file 0xA748): the compositor only draws layer 0 (host stationary bitmap) or blank. The host "temporary" display target 0x21 is suppressed too
- Octave (module 0x0B) is untouched and **must stay in firmware** (user explicit) — never move octave handling into the app
- Stock DAW pad layout sends NO pad aftertouch. NPB+ enables it per pad press; the host must also set feature 0x6C = 2 (`B6 6C 02` on the DAW port; non-volatile). Result: `A0 note pressure` ch1 on the DAW port
- **MIDI clock:** the device setting "MIDI Clock Out" (feature 0x6E, non-volatile, stock default ON) makes the firmware send `F8` on the MIDI port all the time, host or no host. The app sends `B6 6E 00` on the DAW port at every connect (`LaunchkeyController::neptuneConnectMessages()`); the Settings menu that could change it is disabled on Neptune builds. The app never takes clock or transport from the controller's ports and never sends its clock to them. A passive recording that shows `F8` from the Launchkey means the setting is back on (settings reset, stock reflash, or the app has not connected since)
- `43 00` (`F0 00 20 29 00 43 00 F7`) on stock firmware erases external-flash 0x6000–0x7FFF and 0x60000–0x7FFFF, then resets. It is NOT needed for updating and does nothing on NP5+ (its handler is now the debug command)

## FIRMWARE: FLASHING
- **The user will NOT do manual bootloader entry.** All flashing must be software-triggered and verified
- Command: `.tools/venv/bin/python -u -I tools/flash_confirmed.py firmware/<file>.syx --wait 16 --retries 3`, then check `ioreg -l -w0 | grep kUSBString` for the new 3-character TAG
- A flash counts only with per-packet acks (~6038 of `72 <lo> <hi> 00`) + final `73 7F 7F 7F` + the new tag in the USB HID interface string. A build is only "running" if that host-readable marker proves it
- Sequence: host sends the `71` + `7C` header ONCE (a second header resets the state) → device `73 00 7F 7F`, then `73 01 7F 7F` at +3 s → builds with the auto-confirm patch (file 0xC692) confirm themselves → device sends `72 01 00 00` → data packets (2 ms spacing worked), each acked → `73 7F 7F 7F`. Data sent before `72 01 00 00` is ignored
- Touching any control during the 3 s prompt delay cancels the update (`73 10`). On firmware without auto-confirm, someone must press the button right of the top pad row within ~10 s of `73 01` or the device gives up (`73 11 7F 7F`)
- The update is staged and installed only after a verified transfer; a cancelled or corrupt transfer leaves the running firmware intact. The version on the upgrade screen is the INSTALLED one
- **A build that transfers correctly but cannot run USB + the update handler CANNOT be recovered remotely** (it needs Oct+/Oct- held while connecting USB, by hand)
- **No-touch areas when nobody is at the device:** USB, the SysEx handler chain (file 0x28300–0x29900), the update code (file 0xC5F4–0xC730, 0x13890–0x13AE2), boot code. Keep patches tiny
- **NEVER flash neptune-displaylock (v7) or any scorched / carpet-bomb / region-fill build**: they overwrite the USB strings, the display pump and the I2C3 driver tail
- Always keep the stock image `firmware/launchkeyminimk4-firmware-1563.syx` as the rollback; never delete it
- Flash and MIDI tools need `mido`: always use `.tools/venv/bin/python -I`, not bare `python3`. `system_profiler SPUSBDataType` returns empty on this macOS — use `ioreg`
- Wait ~12 s after a flash before running MIDI tools (a script that opens ports within ~10 s can hang); wrap long tests with a kill timer

## FIRMWARE: PATCHING RULES
- Every patch lives in the single list in `tools/build_neptune_fw.py` and is checked against the stock bytes it replaces
- To stop a firmware mode from taking a control, stop its `claim` — do not NOP its drawing or LED code
- Hook addresses come from `arm-none-eabi-nm` on `neptune-patches/ccbtn.elf`. Never hardcode a label's address in the build list: adding code moved `btn_release`, and a hardcoded value would have branched into the middle of an instruction on every button release
- After changing any `.S` file: re-run the gcc/ld/objcopy steps for ALL its sections, then the build script, then disassemble or check the branch targets before flashing
- Any hook on the pressure listener list (0x2000F048) must chain to stock 0xDE74 or aftertouch stops
- Free code space is nearly used up: `43 00` cave 88/94, `43 01` handler 50/50, prompt UI 72/116, prompt callbacks 84/86, helper 32/36. New features need new dead code or an appended region

## CLOSED-LOOP TESTING TOOLS
- `tools/nepdbg.py` — debug SysEx `F0 00 20 29 00 7D <addr:5><a0:5><a1:5> F7` → device calls addr(a0,a1) and replies `F0 00 20 29 00 7D <r0:5> F7` (5×7-bit, MSB first; addr needs the Thumb bit). `call`, `peek32` (gadget file 0xA190), `poke32` (file 0x196), `read_mem`. **A bad address faults and the device resets**
- `tools/nepinput.py hold|tap|combo` — uses the firmware's own input simulation: `0x13660(1/0)` switches it, `0x1362E(id, raw12)` sets a control's raw reading (0xFFF = pressed). While it is on, physical controls are ignored
- Simulation side effects: on entry every raw input becomes 0, so the host sees pitch bend −8192, CC 1 = 0 and encoder jumps (they return on exit); a simulated key's note-off is only sent when simulation is switched off; simulated encoder quadrature does not decode direction reliably. Don't leave simulation on, and expect those messages in logs
- Simulated pad pressure behaves like a real finger (verified against the user's presses)
- `tools/oled_dump.py` — reads the screen buffer back; reliable only while the app is NOT running (see DISPLAY AND UI THREADS)
- `tools/snap.sh` / `tools/snap_oled.sh [name]` — Mac camera still / enlarged OLED crop in `.ralph/snaps/` (git-ignored). Crop coordinates are fixed for the 2026-11-10 framing (OLED at about x 395–665, y 30–200 of 1920×1080); if the device or laptop moves, re-take a full photo and adjust the `sips --cropOffset`
- Camera limits: large shapes, borders and pad/button LEDs are readable; 5×7 pixel text is NOT. Red, green, blue, white, cyan, orange read true; **yellow shows as pale lime, magenta as purple** — do not read that as a wrong colour. Unlit pads look plain white/grey
- `tools/pad_colors.py pattern|off` — known pad colour pattern for camera checks; `tools/listen.sh` — microphone level check for sound
- Capture scripts that open mido inputs with callbacks can hang on exit and block later scripts: end them with `os._exit(0)`, and check `pgrep -f "ython.*-I"` if a tool stalls

## FIRMWARE: WHAT NOT TO DO
- No blind flashing (sending an image without checking acks). The only flashing tool is `tools/flash_confirmed.py`
- Do not rely on anything "learned" from the firmware builds of iterations 44–49 (hotswap, scorched earth, carpet bomb, displaylock v5–v7, "approaches exhausted"): those results are untested
- Do not send `43 00` to stock firmware
- Do not build on address-based notes that assume base 0x08030000, on "OLED on I2C1", or on the "I2C flush gate" idea
- The from-scratch custom firmware (`neptune-firmware/`) is shelved. If revisited it must be linked at 0x08008000 (the old 0x08030000 link address is the likely reason it never enumerated), and any wait for a clock-ready flag (e.g. HSE) needs a timeout with a fallback

