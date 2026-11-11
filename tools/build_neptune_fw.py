#!/usr/bin/env python3
"""Build the current Neptune firmware: stock + marker strings + small patches.
Every patch is checked against the stock bytes it replaces.

Usage: build_neptune_fw.py [TAG]   TAG = 3-char USB HID marker (default NP3)
Output: firmware/neptune-fw-<TAG>.syx / .bin
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk4_syx_tool import decode_syx, encode_syx, FirmwareInfo, crc32
P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG = (sys.argv[1] if len(sys.argv) > 1 else "NP3").encode()
assert len(TAG) == 3

def b16(src, dst):
    off = dst - (src + 4); assert off % 2 == 0 and -2048 <= off < 2048
    return struct.pack('<H', 0xE000 | ((off >> 1) & 0x7FF))

# (file offset, stock bytes, new bytes, description)
PATCHES = [
    (0x0B16A, b"HID", TAG, "USB HID interface string = build tag (host-readable marker)"),
    (0x26BD4, b"Octave", b"NEPTUN", "on-screen marker"),
    (0x26BDB, b"Transpose", b"NeptuneFW", "on-screen marker"),
    (0x26C6A, b"Settings", b"NEPTUNE!", "on-screen marker"),
    (0x2A06B, b"Entering Upgrade", b"Neptune Upgrade!", "on-screen marker"),
    # Display lock: compositor always picks layer 0 (host bitmap) or blank
    (0x0A748, bytes.fromhex("06f041f8"), b16(0xA748, 0xA77A) + b"\x00\xbf", "display lock: bl get_f3 -> b 0xA77A; nop"),
    # Display ready reply (F0 00 20 29 02 13 09 F7) is sent on the main-loop pass after a host frame is latched,
    # instead of waiting until the display command list is finished (0x10786 -> 0xA3F8). The reply context is
    # still cleared after the one reply. The host can stream the next frame while this one is written to the OLED.
    (0x10D6A, bytes.fromhex("fff70cfd"), bytes.fromhex("012000bf"), "early display ready reply: bl display_idle -> movs r0,#1; nop"),
    # Layer 1 (firmware pop-ups / host temporary display) is never accepted by the display manager's layer check
    # (0x10530), so encoder / control pop-ups no longer mark the screen dirty. With the display lock each of those
    # caused a full 8-page redraw of the host frame (35 ms of I2C per event), which starved host frames.
    (0x1053A, bytes.fromhex("60b9"), b16(0x1053A, 0x10556), "no pop-up redraws: cbnz r0 -> b 0x10556 (layer 1 never accepted)"),
    # Hands-free update: once the 3 s prompt delay has passed (73 01), confirm without the right-arrow press
    (0x0C692, bytes.fromhex("a8b9"), b16(0xC692, 0xC6C0), "auto-confirm update: cbnz r0 -> b 0xC6C0"),
    # Shift module (0x0E) refresh at 0x1AC5E: never claim pads 0x00-0x0F / 0x28-0x2C / keys for the Shift layer.
    # Shift still owns its own button, so it still sets its flag and sends CC 63 ch7 to the host.
    (0x1AC6E, bytes.fromhex("b8b1"), b16(0x1AC6E, 0x1ACA0), "no Shift layer: cbz r0 -> b 0x1ACA0 (skip control claims)"),
    # Settings module (0x10) refresh at 0x1C360: never claim the Settings button, so the firmware menu cannot open.
    (0x1C366, bytes.fromhex("01f029fa"), b"\x00\xbf\x00\xbf", "no Settings menu: bl claim(0x1A, 0x10) -> nop nop"),
]

# Debug command (neptune-patches/dbg.S): F0 00 20 29 00 7D <addr:5><a0:5><a1:5> F7 -> calls addr(a0,a1), replies r0.
# Replaces the stock "43 00" handler (erase external flash + reboot) and reuses its erase routine as code space.
def blob(name, off, maxlen, desc):
    data = open(os.path.join(P, "neptune-patches", name), "rb").read()
    assert len(data) <= maxlen, f"{name} is {len(data)} bytes, max {maxlen}"
    return (off, None, data, desc)
PATCHES += [
    blob("handler.bin", 0x29052, 44, "debug command handler (replaces 43 00 handler)"),
    blob("cave.bin",    0x29866, 94, "debug command body (in dead erase routine)"),
]

# Host CCs for Settings / Arp / Scale (neptune-patches/ccbtn.S). CC 0x6C / 0x6D / 0x6E, status 0xB0, 127 on press, 0 on release.
def bw(src, dst):
    off = dst - (src + 4); assert off % 2 == 0 and -(1 << 24) <= off < (1 << 24)
    s = (off >> 24) & 1; i1 = (off >> 23) & 1; i2 = (off >> 22) & 1
    j1 = (~(i1 ^ s)) & 1; j2 = (~(i2 ^ s)) & 1
    return struct.pack('<HH', 0xF000 | (s << 10) | ((off >> 12) & 0x3FF), 0x9000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7FF))
import subprocess
# Hook targets come from the linked ELF so they cannot drift when ccbtn.S changes (file offset = address - 0x08008000).
SYM = {l.split()[2]: int(l.split()[0], 16) - 0x08008000
       for l in subprocess.run(["arm-none-eabi-nm", os.path.join(P, "neptune-patches/ccbtn.elf")],
                               capture_output=True, text=True, check=True).stdout.splitlines() if len(l.split()) == 3}
assert 0x29020 <= SYM["btn_press"] < 0x29052 and 0x29020 <= SYM["btn_release"] < 0x29052
PATCHES += [
    (0x1B7FE, bytes.fromhex("01f0ddff"), b"\x00\xbf\x00\xbf", "no firmware Arp toggle: bl claim(0x1E, 0x01) -> nop nop"),
    (0x195C8, bytes.fromhex("04f0f8f8"), b"\x00\xbf\x00\xbf", "no firmware Scale toggle: bl claim(0x42, 0x0F) -> nop nop"),
    blob("h43.bin", 0x29020, 50, "43 01 handler disabled; button press/release -> CC lookup"),
    blob("snd.bin", 0x1A8A8, 36, "send CC on the DAW port list (in dead 43 01 helper)"),
    blob("pat.bin", 0x1A794, 116, "DAW-pad helpers: aftertouch enable + red on press, host colour on release (in dead 43 01 prompt UI)"),
    (0x1A808, bytes.fromhex("10b5"), b"\x70\x47", "43 01 prompt refresh callback -> bx lr (body reused)"),
    (0x1A862, bytes.fromhex("80b5"), b"\x70\x47", "43 01 prompt release callback -> bx lr"),
    blob("pat2.bin", 0x1A80C, 86, "pressure rainbow listener + colour table"),
    (0xe0ac, struct.pack('<I', 0x08015E75), None, "pressure-listener registration (list 0x2000F048): stock 0xDE74 -> pad_value, which chains to 0xDE74"),
    (0x1A8CC, bytes.fromhex("38b5"), b"\x70\x47", "43 01 prompt press handler -> bx lr (its UI code is reused)"),
    (0x1A974, bytes.fromhex("0a4a"), b"\x70\x47", "43 01 prompt timer -> bx lr"),
    # Press listeners are called in registration order. Swap two registration literals so the Neptune stub runs
    # FIRST (before the generic press handler at 0xDC6C copies the pad descriptor) and stock 0x18E96 runs in its old slot.
    (0x19160, struct.pack('<I', 0x08020E97), struct.pack('<I', 0x080226DD), "register Neptune press stub first"),
    (0x1A700, struct.pack('<I', 0x080226DD), struct.pack('<I', 0x08020E97), "register stock 0x18E96 press listener in the stub's old slot"),
    (0x1A6DC, None, bw(0x1A6DC, SYM["btn_press"]), "press-list stub -> btn_press"),
    (0x1A6E0, None, bw(0x1A6E0, SYM["btn_release"]), "release-list stub -> btn_release"),
]

stock = decode_syx(bytearray(open(os.path.join(P, "firmware/launchkeyminimk4-firmware-1563.syx"), "rb").read()))
fw = bytearray(stock.binary)
for off, old, new, desc in PATCHES:
    if new is None: new = struct.pack('<I', (0x08008000 + SYM["pad_value"]) | 1)
    if old is None: old = bytes(stock.binary[off:off + len(new)])
    assert len(old) == len(new), desc
    assert fw[off:off + len(old)] == old, f"{desc}: stock has {bytes(fw[off:off+len(old)]).hex()} at {off:#x}, expected {old.hex()}"
    fw[off:off + len(new)] = new
    print(f"  {off:#07x}: {desc}")
info = FirmwareInfo(manufacturer=stock.manufacturer, model=stock.model, build=stock.build,
                    payload_size=len(fw), crc=crc32(bytearray(fw)), binary=bytes(fw))
stem = os.path.join(P, f"firmware/neptune-fw-{TAG.decode()}")
open(stem + ".syx", "wb").write(encode_syx(info)); open(stem + ".bin", "wb").write(fw)
assert decode_syx(bytearray(open(stem + ".syx", "rb").read())).binary == bytes(fw)
print(f"wrote {stem}.syx  ({sum(a != b for a, b in zip(fw, stock.binary))} bytes differ from stock, crc32={info.crc:#010x})")
