#!/usr/bin/env python3
"""Shared helpers for the Launchkey tools: find a MIDI port by name, read a .syx file."""
import mido


def find_port(keyword, direction='output'):
    names = mido.get_output_names() if direction == 'output' else mido.get_input_names()
    for n in sorted(names):
        if "Launchkey" in n and keyword in n:
            return n
    return None



def read_syx_messages(syx_path):
    """Parse a .syx file into a list of mido Message objects."""
    with open(syx_path, 'rb') as f:
        raw = f.read()
    messages = []
    i = 0
    while i < len(raw):
        if raw[i] == 0xF0:
            end = raw.index(0xF7, i)
            messages.append(mido.Message('sysex', data=list(raw[i+1:end])))
            i = end + 1
        else:
            i += 1
    return messages


