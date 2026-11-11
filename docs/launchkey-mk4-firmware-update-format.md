# MK4 SysEx Firmware Encoder Research

## Summary

The Novation Launchkey Mini MK4 firmware SysEx format has been fully reverse-engineered
and a roundtrip-verified encoder/decoder tool created at `tools/mk4_syx_tool.py`.

## Key Findings

### MK4 Model ID: 0x13

The MK4 uses model ID **0x13** (decimal 19) in its firmware SysEx headers. For reference:
- Launchkey MK3: 0x0F
- FLKey: 0x11
- **Launchkey Mini MK4: 0x13** (new discovery)

### SysEx Message Structure

The firmware .syx file consists of 6040 SysEx messages in this order:

1. **START** (1 message) — identifier `00 71`
   - Header: `F0 00 20 29 00 71`
   - Novation internal manufacturer ID: `02`
   - Model ID: `13` (MK4)
   - Build field: 6 raw bytes (not ASCII, not decimal digits — format differs from MK3/FLKey)
   - Trailer: `F7`
   - Total: 15 bytes

2. **METADATA** (1 message) — identifier `00 7C`
   - Header: `F0 00 20 29 00 7C`
   - Null terminator: `00`
   - Build field: same 6 raw bytes as START
   - Payload size: 8 nibble-encoded bytes (4 bytes split high/low → 8 MIDI-safe bytes)
   - CRC32: 8 nibble-encoded bytes (same encoding)
   - Trailer: `F7`
   - Total: 30 bytes

3. **DATA** (6037 messages) — identifier `00 72`
   - Header: `F0 00 20 29 00 72`
   - Encoded chunk: 37 bytes (32 raw bytes → 37 via 7-bit encoding)
   - Trailer: `F7`
   - Total: 44 bytes each
   - Contains binary chunks starting from offset 32 (chunk 1 onward)

4. **END** (1 message) — identifier `00 73`
   - Same structure as DATA (44 bytes)
   - Contains chunk 0 (first 32 bytes of the binary)

### Binary Reassembly Order

The first chunk (offset 0) of the firmware binary is stored in the **END** message,
not the first DATA message. Reassembly: `END_chunk + DATA_chunks_in_order`.

### 7-bit Encoding

MIDI SysEx can only carry 7-bit values (0x00–0x7F). Novation packs 7 bytes of 8-bit
data into 8 bytes of 7-bit data, distributing the high bits. A 32-byte chunk becomes
exactly 37 bytes when encoded. The codec is bit-level — see `encode_7bit()` / `decode_7bit()`.

### Partial Chunk Padding

The firmware binary (193,203 bytes) is not an exact multiple of 32:
- 193,203 = 6037 × 32 + 19
- The final 19-byte chunk is **padded with 0xFF** to 32 bytes before 7-bit encoding
- This produces a full 37-byte encoded chunk in the last DATA message
- The METADATA payload_size field records the true size (193,203), and on decode the
  reassembled binary is trimmed to that size

### Nibble Encoding (Metadata Fields)

The payload size and CRC32 in the METADATA message are stored as "split nibbles":
each byte is split into its high and low nibbles, each stored as a separate byte.
This doubles the width but keeps all values ≤ 0x0F (MIDI-safe).

### CRC32

Uses the ITU-T V.42 polynomial `0x104C11DB7` with initial value `0xFFFFFFFF`.
Computed over the entire raw binary payload. Verified: computed CRC matches the
value stored in the METADATA message.

### Build Field

The stock MK4 firmware 1563 has build bytes: `01 03 0C 1B 0D 35`.
This does NOT follow the MK3/FLKey encoding (which stores decimal digits as individual
bytes, e.g., `001563` → `[0,0,1,5,6,3]`). The MK4 build encoding appears to be a
different format — possibly a timestamp or hash. For roundtrip safety, the tool
preserves these bytes exactly as-is rather than interpreting them.

### Stock Firmware Stats

- File: `launchkeyminimk4-firmware-1563.syx`
- SysEx file size: 265,717 bytes
- Binary payload: 193,203 bytes
- CRC32: 0xA1817A85
- Model ID: 0x13
- SysEx messages: 6,040 (1 START + 1 METADATA + 6,037 DATA + 1 END)

## Tool Verification

The roundtrip test (`mk4_syx_tool.py roundtrip`) confirms:
- Decode the stock .syx → re-encode → **byte-for-byte identical** to original
- CRC32 validates on decode
- Decoded .bin matches xkey's decode output exactly

## Comparison with xkey

The existing `xkey` tool (AGPL-licensed) supports MK3 and FLKey but not MK4 because:
1. It doesn't have model ID 0x13 in its MODEL_IDS dict
2. Its build number encoding assumes decimal digits — incompatible with MK4's build format
3. It doesn't pad partial chunks with 0xFF (it would produce shorter output)

Our `mk4_syx_tool.py` is written from scratch (no xkey code), handles all MK4 specifics,
and passes the roundtrip verification.
