#!/usr/bin/env python3
"""Flash firmware .syx to Novation Launchkey Mini MK4 via bootloader.

Usage:
  1. Hold Oct+ and Oct- while connecting USB to enter bootloader
  2. Run: python flash_firmware.py <firmware.syx>
  3. Wait for completion
  4. Press Play on the Launchkey to exit bootloader

Optionally pass --list to see available MIDI ports.
"""

import sys
import time
import mido

CHUNK_DELAY = 0.002  # 2ms between SysEx chunks


def list_ports():
    print("Available MIDI output ports:")
    for name in mido.get_output_names():
        print(f"  {name}")
    print()
    print("Available MIDI input ports:")
    for name in mido.get_input_names():
        print(f"  {name}")


def find_bootloader_port():
    for name in mido.get_output_names():
        if "Launchkey" in name and "MIDI" in name:
            return name
    for name in mido.get_output_names():
        if "Launchkey" in name:
            return name
    return None


def read_syx_file(path):
    with open(path, "rb") as f:
        data = f.read()

    messages = []
    i = 0
    while i < len(data):
        if data[i] == 0xF0:
            end = data.index(0xF7, i)
            messages.append(data[i : end + 1])
            i = end + 1
        else:
            i += 1

    return messages


def flash(syx_path, port_name=None):
    messages = read_syx_file(syx_path)
    print(f"Loaded {syx_path}: {len(messages)} SysEx messages, {sum(len(m) for m in messages)} bytes total")

    if port_name is None:
        port_name = find_bootloader_port()
        if port_name is None:
            print("ERROR: No Launchkey MIDI port found. Is the device in bootloader mode?")
            print("       Hold Oct+ and Oct- while connecting USB.")
            list_ports()
            return False

    print(f"Sending to: {port_name}")
    print()

    with mido.open_output(port_name) as port:
        for i, msg_bytes in enumerate(messages):
            msg = mido.Message.from_bytes(msg_bytes)
            port.send(msg)
            time.sleep(CHUNK_DELAY)

            pct = (i + 1) * 100 // len(messages)
            if (i + 1) % 10 == 0 or i == len(messages) - 1:
                print(f"\r  Sending: {i+1}/{len(messages)} ({pct}%)", end="", flush=True)

    print()
    print(f"Done. Sent {len(messages)} SysEx messages.")
    print("Press Play on the Launchkey to exit bootloader.")
    return True


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] == "--help":
        print(__doc__)
        sys.exit(0)

    if sys.argv[1] == "--list":
        list_ports()
        sys.exit(0)

    syx_path = sys.argv[1]
    port_name = sys.argv[2] if len(sys.argv) > 2 else None

    success = flash(syx_path, port_name)
    sys.exit(0 if success else 1)
