#!/usr/bin/env python3
"""MK4 SysEx Firmware Tool

Decode, encode, roundtrip-verify, and patch Novation Launchkey Mini MK4 firmware
SysEx files. Written from clean-room analysis of the SysEx format.

Usage:
    python mk4_syx_tool.py decode  <firmware.syx>                  # .syx -> .bin
    python mk4_syx_tool.py encode  <firmware.bin> [--build B]      # .bin -> .syx
    python mk4_syx_tool.py roundtrip <firmware.syx>                # decode+encode, verify match
    python mk4_syx_tool.py patch   <firmware.bin> <patches.json>   # apply binary patches

The roundtrip command is the critical safety check: it decodes a stock .syx to a
temporary .bin, re-encodes it, and verifies byte-for-byte identity with the original.
If roundtrip fails, the tool is NOT safe for firmware modification.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import tempfile
from ctypes import c_int8, c_uint8
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SYSEX_SOX = 0xF0
SYSEX_EOX = 0xF7

# MIDI manufacturer ID for Novation (3-byte extended)
NOVATION_MIDI_MFR = bytearray([0x00, 0x20, 0x29])

# Novation internal manufacturer ID (inside SysEx payload)
NOVATION_INTERNAL_MFR = bytearray([0x02])

# MK4 model ID -- discovered by analyzing the stock firmware .syx header.
# For reference: MK3 = 0x0F, FLKey = 0x11, MK4 = 0x13
MK4_MODEL_ID = bytearray([0x13])

# SysEx message type identifiers (2 bytes following the Novation manufacturer ID)
IDENT_START    = bytearray([0x00, 0x71])
IDENT_METADATA = bytearray([0x00, 0x7C])
IDENT_DATA     = bytearray([0x00, 0x72])
IDENT_END      = bytearray([0x00, 0x73])

# Chunk size for binary data (before 7-bit encoding)
CHUNK_SIZE = 32

# Encoded chunk size: 32 bytes -> 37 bytes in 7-bit encoding
ENCODED_CHUNK_SIZE = 37

# Build field length (6 bytes)
BUILD_FIELD_SIZE = 6

# CRC32 polynomial -- ITU-T V.42 (same as used by Novation firmware)
CRC32_POLY = 0x104C11DB7

# Padding byte for partial final chunk
PAD_BYTE = 0xFF


# ---------------------------------------------------------------------------
# CRC32 (ITU-T V.42 polynomial)
# ---------------------------------------------------------------------------

def crc32(buffer: bytearray, crc: int = 0xFFFFFFFF) -> int:
    """Compute CRC32 with ITU V.42 polynomial, matching Novation's implementation."""
    for byte in buffer:
        crc ^= byte << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = (crc << 1) ^ CRC32_POLY
            else:
                crc = crc << 1
    return crc & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# 7-bit codec (Novation SysEx encoding)
# ---------------------------------------------------------------------------
# MIDI SysEx can only carry 7-bit bytes (0x00-0x7F). Novation packs 7 bytes of
# 8-bit data into 8 bytes of 7-bit data by distributing the high bits across
# the group.

def encode_7bit(buffer: bytearray) -> bytearray:
    """Encode 8-bit bytes into 7-bit Novation SysEx format."""
    last = 0x0
    byte = last
    encoded = bytearray()

    for offset in range(len(buffer)):
        last = byte
        byte = buffer[offset]

        byte_shift = (offset % 7) + 1
        mask_shift = 7 - (offset % 7)

        shifted = c_uint8(byte >> byte_shift).value
        masked = c_uint8(last << mask_shift).value & 0x7F

        if offset > 0 and offset % 7 == 0:
            encoded.append(c_uint8(last).value & 0x7F)
            encoded.append(shifted)
            continue

        encoded.append(shifted ^ masked)

    # Final carry
    encoded.append(c_uint8(byte << (offset % 7)).value & 0x7F)

    return encoded


def decode_7bit(buffer: bytearray) -> bytearray:
    """Decode 7-bit Novation SysEx format into 8-bit bytes."""
    decoded = bytearray()

    for start in range(0, len(buffer) - 1, 8):
        for index in range(0, 7):
            offset = start + index

            if (offset + 1) >= len(buffer):
                break

            byte = buffer[offset]
            next_ = buffer[offset + 1]

            byte_shift = (index % 7) + 1
            mask_shift = 6 - (index % 7)

            masked = (
                c_int8(next_ >> mask_shift).value & c_uint8(0x7F >> mask_shift).value
            )
            shifted = c_int8(byte << byte_shift).value

            decoded.append(c_uint8(shifted | masked).value)

    return decoded


# ---------------------------------------------------------------------------
# Nibble codec (for metadata fields -- size and CRC)
# ---------------------------------------------------------------------------

def bytes_to_nibbles(buffer: bytearray) -> bytearray:
    """Split each byte into two nibble bytes (for MIDI-safe 4-bit encoding)."""
    output = bytearray()
    for b in buffer:
        output.append((b >> 4) & 0x0F)
        output.append(b & 0x0F)
    return output


def nibbles_to_bytes(buffer: bytearray) -> bytearray:
    """Recombine nibble pairs into full bytes."""
    output = bytearray()
    for i in range(0, len(buffer), 2):
        output.append(((buffer[i] & 0x0F) << 4) | (buffer[i + 1] & 0x0F))
    return output


# ---------------------------------------------------------------------------
# SysEx message builders
# ---------------------------------------------------------------------------

def build_sysex_header(identifier: bytearray) -> bytearray:
    """Build the common 6-byte SysEx header: F0 00 20 29 <ident_hi> <ident_lo>."""
    msg = bytearray([SYSEX_SOX])
    msg.extend(NOVATION_MIDI_MFR)
    msg.extend(identifier)
    return msg


def build_start_message(
    manufacturer: bytearray,
    model: bytearray,
    build: bytearray,
) -> bytearray:
    """Build a START SysEx message."""
    msg = build_sysex_header(IDENT_START)
    msg.extend(manufacturer)
    msg.extend(model)
    msg.extend(build)
    msg.append(SYSEX_EOX)
    return msg


def build_metadata_message(
    build: bytearray,
    payload_size: int,
    crc_value: int,
) -> bytearray:
    """Build a METADATA SysEx message."""
    msg = build_sysex_header(IDENT_METADATA)
    msg.append(0x00)  # null terminator field
    msg.extend(build)
    msg.extend(bytes_to_nibbles(bytearray(payload_size.to_bytes(4, byteorder="big"))))
    msg.extend(bytes_to_nibbles(bytearray(crc_value.to_bytes(4, byteorder="big"))))
    msg.append(SYSEX_EOX)
    return msg


def build_data_message(encoded_chunk: bytearray) -> bytearray:
    """Build a DATA SysEx message wrapping an already-encoded chunk."""
    msg = build_sysex_header(IDENT_DATA)
    msg.extend(encoded_chunk)
    msg.append(SYSEX_EOX)
    return msg


def build_end_message(encoded_chunk: bytearray) -> bytearray:
    """Build an END SysEx message wrapping an already-encoded chunk."""
    msg = build_sysex_header(IDENT_END)
    msg.extend(encoded_chunk)
    msg.append(SYSEX_EOX)
    return msg


# ---------------------------------------------------------------------------
# Parsed firmware representation
# ---------------------------------------------------------------------------

@dataclass
class FirmwareInfo:
    """All information extracted from a decoded .syx firmware file."""
    manufacturer: bytearray
    model: bytearray
    build: bytearray
    payload_size: int
    crc: int
    binary: bytearray


# ---------------------------------------------------------------------------
# Decode (.syx -> binary)
# ---------------------------------------------------------------------------

def parse_sysex_messages(syx_data: bytearray) -> List[bytearray]:
    """Split raw SysEx data into individual messages (F0 ... F7)."""
    messages = []
    start = None
    for i, b in enumerate(syx_data):
        if b == SYSEX_SOX:
            start = i
        elif b == SYSEX_EOX and start is not None:
            messages.append(bytearray(syx_data[start : i + 1]))
            start = None
    return messages


def decode_syx(syx_data: bytearray, verbose: bool = False) -> FirmwareInfo:
    """Decode a .syx firmware file into its constituent parts.

    Returns a FirmwareInfo with the binary payload trimmed to the declared size.
    """
    messages = parse_sysex_messages(syx_data)
    if len(messages) < 3:
        raise ValueError(f"Expected at least 3 SysEx messages, got {len(messages)}")

    # -- Parse START --
    start_msg = messages[0]
    if start_msg[4:6] != IDENT_START:
        raise ValueError(f"First message is not START (got {start_msg[4:6].hex()})")
    manufacturer = bytearray([start_msg[6]])
    model = bytearray([start_msg[7]])
    build = bytearray(start_msg[8 : 8 + BUILD_FIELD_SIZE])

    if verbose:
        print(f"START: manufacturer=0x{manufacturer[0]:02x} model=0x{model[0]:02x} "
              f"build={build.hex(' ')}")

    # -- Parse METADATA --
    meta_msg = messages[1]
    if meta_msg[4:6] != IDENT_METADATA:
        raise ValueError(f"Second message is not METADATA (got {meta_msg[4:6].hex()})")
    # Fields: nullterm(1) + build(6) + payload_size_nibbles(8) + crc_nibbles(8)
    meta_build = bytearray(meta_msg[7 : 7 + BUILD_FIELD_SIZE])
    size_nibbles = bytearray(meta_msg[13:21])
    crc_nibbles = bytearray(meta_msg[21:29])

    payload_size = int.from_bytes(nibbles_to_bytes(size_nibbles), byteorder="big")
    crc_value = int.from_bytes(nibbles_to_bytes(crc_nibbles), byteorder="big")

    if verbose:
        print(f"METADATA: payload_size={payload_size} crc=0x{crc_value:08x}")

    # -- Parse DATA and END chunks --
    data_chunks: List[bytearray] = []
    end_chunk: Optional[bytearray] = None

    for msg in messages[2:]:
        ident = msg[4:6]
        payload = bytearray(msg[6:-1])  # between header and EOX

        if ident == IDENT_DATA:
            decoded = decode_7bit(payload)
            data_chunks.append(decoded[:CHUNK_SIZE])
        elif ident == IDENT_END:
            decoded = decode_7bit(payload)
            end_chunk = decoded[:CHUNK_SIZE]
        else:
            raise ValueError(f"Unexpected message type: {ident.hex()}")

    if end_chunk is None:
        raise ValueError("No END message found")

    if verbose:
        print(f"Decoded {len(data_chunks)} DATA chunks + 1 END chunk")

    # Reassemble: END chunk is first, then DATA chunks in order
    binary = bytearray(end_chunk)
    for chunk in data_chunks:
        binary.extend(chunk)

    # Trim to declared payload size
    binary = bytearray(binary[:payload_size])

    # Verify CRC
    computed_crc = crc32(binary)
    if computed_crc != crc_value:
        raise ValueError(
            f"CRC mismatch: computed 0x{computed_crc:08x}, expected 0x{crc_value:08x}"
        )

    if verbose:
        print(f"CRC32 verified: 0x{computed_crc:08x}")

    return FirmwareInfo(
        manufacturer=manufacturer,
        model=model,
        build=build,
        payload_size=payload_size,
        crc=crc_value,
        binary=binary,
    )


# ---------------------------------------------------------------------------
# Encode (binary -> .syx)
# ---------------------------------------------------------------------------

def encode_syx(info: FirmwareInfo) -> bytearray:
    """Encode a FirmwareInfo back into a .syx bytearray.

    The binary payload is split into 32-byte chunks. The first chunk (offset 0)
    goes into the END message. All subsequent chunks go into DATA messages, in
    order. The last chunk is padded with 0xFF to a full 32 bytes before encoding.
    """
    binary = info.binary

    # Compute CRC and size from the actual binary
    payload_size = len(binary)
    crc_value = crc32(binary)

    # Build START message
    start = build_start_message(info.manufacturer, info.model, info.build)

    # Build METADATA message
    metadata = build_metadata_message(info.build, payload_size, crc_value)

    # Build DATA messages (chunks starting at offset CHUNK_SIZE)
    data_section = bytearray()
    offset = CHUNK_SIZE
    while offset < len(binary):
        chunk = bytearray(binary[offset : offset + CHUNK_SIZE])
        # Pad partial final chunk with 0xFF
        if len(chunk) < CHUNK_SIZE:
            chunk.extend(bytearray([PAD_BYTE] * (CHUNK_SIZE - len(chunk))))
        encoded_chunk = encode_7bit(chunk)
        data_section.extend(build_data_message(encoded_chunk))
        offset += CHUNK_SIZE

    # Build END message (chunk 0)
    chunk0 = bytearray(binary[0:CHUNK_SIZE])
    encoded_chunk0 = encode_7bit(chunk0)
    end = build_end_message(encoded_chunk0)

    return start + metadata + data_section + end


# ---------------------------------------------------------------------------
# CLI commands
# ---------------------------------------------------------------------------

def cmd_decode(args: argparse.Namespace) -> int:
    """Decode .syx -> .bin."""
    syx_path = Path(args.syx_file).resolve()
    out_path = Path(args.output) if args.output else syx_path.with_suffix(".syx.bin")

    print(f"Reading: {syx_path}")
    syx_data = bytearray(syx_path.read_bytes())

    info = decode_syx(syx_data, verbose=True)
    out_path.write_bytes(info.binary)
    print(f"Wrote {len(info.binary)} bytes to {out_path}")
    return 0


def cmd_encode(args: argparse.Namespace) -> int:
    """Encode .bin -> .syx."""
    bin_path = Path(args.bin_file).resolve()
    out_path = Path(args.output) if args.output else bin_path.with_suffix(".syx")

    print(f"Reading: {bin_path}")
    binary = bytearray(bin_path.read_bytes())

    # Parse build bytes if provided, otherwise use the MK4 stock firmware's build
    if args.build_hex:
        build_hex = args.build_hex.replace(" ", "")
        if len(build_hex) != BUILD_FIELD_SIZE * 2:
            print(f"Error: --build-hex must be exactly {BUILD_FIELD_SIZE} bytes "
                  f"({BUILD_FIELD_SIZE * 2} hex chars)")
            return 1
        build = bytearray.fromhex(build_hex)
    else:
        # Default: stock MK4 firmware build bytes
        build = bytearray([0x01, 0x03, 0x0c, 0x1b, 0x0d, 0x35])
        print(f"Using default build: {build.hex(' ')}")

    info = FirmwareInfo(
        manufacturer=NOVATION_INTERNAL_MFR,
        model=MK4_MODEL_ID,
        build=build,
        payload_size=len(binary),
        crc=crc32(binary),
        binary=binary,
    )

    syx_data = encode_syx(info)
    out_path.write_bytes(syx_data)
    print(f"Wrote {len(syx_data)} bytes to {out_path}")
    print(f"Payload: {info.payload_size} bytes, CRC32: 0x{info.crc:08x}")
    return 0


def cmd_roundtrip(args: argparse.Namespace) -> int:
    """Decode a .syx, re-encode, and verify byte-for-byte match."""
    syx_path = Path(args.syx_file).resolve()

    print(f"Reading: {syx_path}")
    original = bytearray(syx_path.read_bytes())

    # Step 1: Decode
    print("Step 1: Decoding .syx -> binary...")
    info = decode_syx(original, verbose=True)

    # Step 2: Re-encode
    print("Step 2: Re-encoding binary -> .syx...")
    reconstructed = encode_syx(info)

    # Step 3: Compare
    print(f"Step 3: Comparing ({len(original)} bytes vs {len(reconstructed)} bytes)...")

    if len(original) != len(reconstructed):
        print(f"FAIL: Size mismatch: original={len(original)}, "
              f"reconstructed={len(reconstructed)}")
        return 1

    if original == reconstructed:
        print("PASS: Byte-for-byte identical. Roundtrip verified.")
        return 0

    # Find first difference
    for i in range(len(original)):
        if original[i] != reconstructed[i]:
            print(f"FAIL: First difference at byte {i}: "
                  f"original=0x{original[i]:02x}, reconstructed=0x{reconstructed[i]:02x}")
            context_start = max(0, i - 8)
            context_end = min(len(original), i + 8)
            print(f"  Original:      ...{original[context_start:context_end].hex(' ')}...")
            print(f"  Reconstructed: ...{reconstructed[context_start:context_end].hex(' ')}...")
            return 1

    return 1  # unreachable


def cmd_patch(args: argparse.Namespace) -> int:
    """Apply binary patches from a JSON file to a .bin firmware."""
    bin_path = Path(args.bin_file).resolve()
    patch_path = Path(args.patch_file).resolve()
    out_path = Path(args.output) if args.output else bin_path.with_name(
        bin_path.stem + "-patched" + bin_path.suffix
    )

    print(f"Reading binary: {bin_path}")
    binary = bytearray(bin_path.read_bytes())

    print(f"Reading patches: {patch_path}")
    with open(patch_path, "r") as f:
        patches = json.load(f)

    if not isinstance(patches, list):
        print("Error: patches JSON must be a list of {offset, old_byte, new_byte} objects")
        return 1

    # Validate all patches before applying any
    for i, patch in enumerate(patches):
        offset = patch.get("offset")
        old_byte = patch.get("old_byte")
        new_byte = patch.get("new_byte")

        if offset is None or old_byte is None or new_byte is None:
            print(f"Error: patch {i} missing required field (offset, old_byte, new_byte)")
            return 1

        if not (0 <= offset < len(binary)):
            print(f"Error: patch {i} offset {offset} out of range (binary is {len(binary)} bytes)")
            return 1

        if binary[offset] != old_byte:
            print(f"Error: patch {i} at offset {offset}: expected old_byte=0x{old_byte:02x}, "
                  f"found 0x{binary[offset]:02x}")
            return 1

    # Apply patches
    for i, patch in enumerate(patches):
        offset = patch["offset"]
        old_byte = patch["old_byte"]
        new_byte = patch["new_byte"]
        binary[offset] = new_byte
        desc = patch.get("description", "")
        desc_str = f" ({desc})" if desc else ""
        print(f"  Patch {i}: offset 0x{offset:06x} "
              f"0x{old_byte:02x} -> 0x{new_byte:02x}{desc_str}")

    out_path.write_bytes(binary)
    print(f"Wrote {len(binary)} bytes to {out_path}")
    print(f"Applied {len(patches)} patch(es)")
    return 0


# ---------------------------------------------------------------------------
# Argument parser
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Novation Launchkey Mini MK4 SysEx Firmware Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # decode
    p_decode = subparsers.add_parser("decode", help="Decode .syx to .bin")
    p_decode.add_argument("syx_file", help="Input .syx firmware file")
    p_decode.add_argument("-o", "--output", help="Output .bin file (default: <input>.bin)")

    # encode
    p_encode = subparsers.add_parser("encode", help="Encode .bin to .syx")
    p_encode.add_argument("bin_file", help="Input .bin firmware file")
    p_encode.add_argument("-o", "--output", help="Output .syx file (default: <input>.syx)")
    p_encode.add_argument(
        "--build-hex",
        help="Build field as 12 hex chars (default: stock MK4 1563 build)",
    )

    # roundtrip
    p_rt = subparsers.add_parser(
        "roundtrip",
        help="Decode then re-encode .syx, verify byte-for-byte match",
    )
    p_rt.add_argument("syx_file", help="Input .syx firmware file")

    # patch
    p_patch = subparsers.add_parser("patch", help="Apply binary patches to .bin")
    p_patch.add_argument("bin_file", help="Input .bin firmware file")
    p_patch.add_argument("patch_file", help="JSON file with patch list")
    p_patch.add_argument("-o", "--output", help="Output .bin file (default: <input>-patched.bin)")

    args = parser.parse_args()

    commands = {
        "decode": cmd_decode,
        "encode": cmd_encode,
        "roundtrip": cmd_roundtrip,
        "patch": cmd_patch,
    }
    return commands[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
