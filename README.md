# Neptune Groovebox

A macOS app (C++ / JUCE) that turns a **Novation Launchkey Mini 25 MK4** into an eyes-free groovebox:
step sequencer with parameter locks, eight sound engines, sampling, effects and an arpeggiator per track,
all driven from the controller's knobs, pads and 128 × 64 display. The Mac screen is optional.

It comes with something that is useful well beyond this app:

## The Launchkey Mini MK4 firmware map

**[docs/launchkey-mk4-firmware-map.md](docs/launchkey-mk4-firmware-map.md)** is a map of Novation's stock
firmware 1563 for the Launchkey Mini MK4 (STM32F4), worked out from disassembly and then **confirmed on
hardware**: every entry in its section 0 was checked with a verified flash plus MIDI capture, memory
read-back or a camera on the device, unless it is marked "code only".

If you want to make this controller do something Novation did not intend, start there. It covers:

| Section | What you get |
|---|---|
| 0.1 Image layout | Load address (0x08008000), 32 KB bootloader, vector table, font and string tables |
| 0.2 Buses and peripherals | OLED on I2C3 (not I2C1), the touch-strip sensor, external flash and their drivers |
| 0.3 – 0.4 Main loop, display pipeline | How frames reach the OLED, the layer compositor, where host bitmaps live in RAM |
| 0.5 Firmware update | The staged in-app update: message sequence, acknowledgements, what cancels it |
| 0.6 SysEx handler chain | Where each manufacturer SysEx command is dispatched |
| 0.7 Controls | Control ids, the `claim(id, module)` ownership model, measured behaviour of every button, pad, knob and strip |
| 0.8 Patches | Each patch in the Neptune build: offset, stock bytes, new bytes, effect, how it was verified |
| 0.8b LEDs | Pad and button colour paths |
| 0.9 Feature controls and settings | The host feature CCs (`B6 <cc> <value>`), which are stored on the device and which are not |
| 0.10 Host tools | The tools below |
| Appendix A | Earlier leads that were **not** re-checked; treat as hints |

Companion documents:

- [docs/launchkey-mk4-firmware-update-format.md](docs/launchkey-mk4-firmware-update-format.md): the `.syx`
  update-file format (model id 0x13, no encryption), decoded and re-encoded byte for byte.
- [docs/launchkey-mk4-neptune-firmware.md](docs/launchkey-mk4-neptune-firmware.md): what the Neptune build changes and why.
- [docs/launchkey-mk4-control-map.md](docs/launchkey-mk4-control-map.md): the full control / MIDI map of the patched device.
- [docs/launchkey-mk4-firmware-rules.md](docs/launchkey-mk4-firmware-rules.md): the rules learned the hard
  way (no-touch areas, how a flash is proven, what cannot be recovered remotely) and the closed-loop test tools.

### What the Neptune firmware build does

Small, individually verified patches on the stock image (`tools/build_neptune_fw.py`, assembly in
`neptune-patches/`):

- **Display lock**: the host owns the OLED; the firmware never draws over a host bitmap.
- **Buttons reclaimed**: no firmware Shift layer, Settings menu, Arp or Scale mode; Settings / Arp / Scale
  report to the host as CC 108 / 109 / 110. Octave ± stays in the firmware.
- **Polyphonic pad aftertouch** in the DAW pad layout (stock sends none there).
- **Hands-free, verified updates**: the update confirms itself, every packet is acknowledged, and the build
  tag is readable from the host in the USB HID string.
- **Debug SysEx `7D`**: call / peek / poke from the host, which the tools below are built on.

### Tools (`tools/`)

| Tool | Purpose |
|---|---|
| `mk4_syx_tool.py` | Decode / encode / round-trip a Launchkey MK4 `.syx` firmware file |
| `build_neptune_fw.py TAG` | Stock image + the patch list (each patch checked against the stock bytes it replaces) |
| `flash_confirmed.py` | Acknowledgement-verified update; a flash only counts with all acks and the new tag |
| `nepdbg.py` | Call a function, read or write memory over the debug SysEx |
| `nepinput.py` | Press buttons through the firmware's own input simulation |
| `oled_dump.py` | Read the display buffer back from the device |
| `screen_audit.py` | Every screen, page and knob of the app, checked on the device (optionally with a camera) |

### Before you flash anything

- **Novation's firmware is not in this repository** and must not be added to it. Get the stock update file
  (version 1563) from Novation and put it in `firmware/`, which is git-ignored. Novation's
  Programmer's Reference Guide for the MK4 is likewise theirs: download it from Novation.
- A build that transfers correctly but cannot run USB and the update handler **cannot be recovered over
  USB**; it needs the hardware bootloader entry (Oct + and Oct − held while plugging in). Read the firmware
  `docs/launchkey-mk4-firmware-rules.md` first. You flash at your own risk, and modified firmware is not
  supported by Novation.
- This project is not affiliated with or endorsed by Novation. "Novation" and "Launchkey" are trademarks of
  their owner, used here only to say what hardware this is for.

## The groovebox

- **Engines** (one per track): a three-oscillator synth whose oscillator slots can each be an analog wave, a
  wavetable, an FM pair, a formant or a harmonic oscillator; six-operator FM; macro oscillator; wavetable;
  wave sequencer; sampler with slicing, time stretch and a granular mode; a 16-voice drum kit with a kick
  machine; and a MIDI track for external gear.
- **Sequencer**: sample-accurate, per-track length and speed, parameter locks on every cell, trig conditions,
  live recording, song mode, MIDI clock.
- **Sound shaping**: five filter models with a live response picture, envelope pictures, per-voice LFOs,
  polyphonic pad pressure routed per voice, three effect blocks plus insert slots on every track, two send
  buses, master EQ / compressor / limiter.
- **Manual**: [docs/Neptune-Groovebox-Manual.pdf](docs/Neptune-Groovebox-Manual.pdf), built from the code's
  own parameter pages and screen bitmaps (`tools/manual/`).

### Build

Needs macOS, CMake and a C++17 compiler. JUCE 9.0.5 is fetched by CMake.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
open "build-release/NeptuneGroovebox_artefacts/Release/Neptune Groovebox.app"

# tests (Debug build)
cmake -S . -B build && cmake --build build --target NeptuneTests && (cd build && ctest)
```

The app expects a Launchkey Mini 25 MK4 running the Neptune firmware build. Without one it starts but has
nothing to play it with.

### Status

Work in progress and unreleased: sounds, project files and the control layout may change without notice.
Much of it is covered by unit tests and an automated on-device screen audit, but a lot has not yet been
judged by ear.

### Repository layout

| Path | Contents |
|---|---|
| `src/` | The app (see `CLAUDE.md` for the architecture; it also refers to the author's private working notes, which are not published) |
| `tests/` | Unit tests, one console binary |
| `docs/` | The firmware map, rules, update-file format, Neptune firmware spec and control map; the owner's manual |
| `tools/` | Firmware build / flash / debug tools, the screen audit, the manual builder |
| `neptune-patches/` | Assembly for the firmware patches |
| `neptune-firmware/` | A from-scratch firmware skeleton that never ran; shelved, kept for reference |

## Licence

Copyright (C) 2026 Chris Chatham.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero
General Public License, version 3, as published by the Free Software Foundation. It is distributed in the hope
that it will be useful, but WITHOUT ANY WARRANTY; see [LICENSE](LICENSE). The firmware map and the other
documents in `docs/` are covered by the same licence.

JUCE, fetched at build time, is used under its AGPLv3 option. Novation's firmware and documentation are not
part of this repository and are not covered by this licence.
