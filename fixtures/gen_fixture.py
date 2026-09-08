#!/usr/bin/env python3
"""Generate the self-authored playback fixture retrovert_selftest.ixs.

The .ixs container is a proprietary wrapper the iXalance tracker no longer
exists to produce, but the player has a second loader: anything that is
not "IXS!" is parsed as an Impulse Tracker module, and that path is
plain IT 2.14. This writes such a module from scratch -- one looped
triangle sample behind one instrument, and two 64-row patterns of an
arpeggiated figure across four channels -- and gives it the .ixs
extension the plugin routes on.

Deterministic output — the committed fixture and its sha256 in
harness.toml must match what this script emits.
"""

import struct
from pathlib import Path

OUT = Path(__file__).parent / "retrovert_selftest.ixs"

CHANNELS = 4
ROWS = 64
SAMPLE_LEN = 128  # bytes, one triangle cycle

# IT note numbers: 60 is C-5, the sample's own C5Speed pitch.
FIGURE = [[60, 64, 67, 72], [62, 65, 69, 74]]

IT_SMPL, IT_LOOP = 0x01, 0x10

INSTRUMENT_SIZE = 0x22A
SAMPLE_HEADER_SIZE = 0x50


def sample_data():
    # Signed 8-bit triangle, full scale, one cycle over SAMPLE_LEN bytes.
    q = SAMPLE_LEN // 4
    out = bytearray()
    for i in range(SAMPLE_LEN):
        if i < q:
            v = i * 127 // q
        elif i < 3 * q:
            v = 127 - (i - q) * 254 // (2 * q)
        else:
            v = -127 + (i - 3 * q) * 127 // q
        out.append(v & 0xFF)
    return bytes(out)


def name(text, length):
    return text.encode("ascii").ljust(length, b"\0")[:length]


def instrument():
    ins = bytearray(INSTRUMENT_SIZE)
    ins[0x00:0x04] = b"IMPI"
    ins[0x04:0x11] = name("selftest.iti", 13)
    ins[0x11] = 0  # NNA: cut
    ins[0x14:0x16] = struct.pack("<H", 256)  # fadeout
    ins[0x18] = 128  # global volume
    ins[0x19] = 32  # default pan, high bit clear so the channel's pan wins
    ins[0x1C:0x1E] = struct.pack("<H", 0x0214)
    ins[0x1E] = 1  # number of samples
    ins[0x20:0x3A] = name("Retrovert self-test", 26)
    # Note/sample keyboard table: every note plays sample 1 at its own pitch.
    for note in range(120):
        ins[0x40 + note * 2] = note
        ins[0x41 + note * 2] = 1
    # All three envelopes stay disabled (flags byte zero), which leaves the
    # instrument at full volume for as long as the note is held.
    return bytes(ins)


def sample_header(data_offset):
    smp = bytearray(SAMPLE_HEADER_SIZE)
    smp[0x00:0x04] = b"IMPS"
    smp[0x04:0x11] = name("selftest.wav", 13)
    smp[0x11] = 64  # global volume
    smp[0x12] = IT_SMPL | IT_LOOP
    smp[0x13] = 64  # default volume
    smp[0x14:0x2E] = name("triangle", 26)
    smp[0x2E] = 0x01  # convert flags: sample data is signed
    smp[0x2F] = 32  # default pan, high bit clear so it is not applied
    struct.pack_into("<IIII", smp, 0x30, SAMPLE_LEN, 0, SAMPLE_LEN, 8363)
    struct.pack_into("<III", smp, 0x40, 0, 0, data_offset)
    return bytes(smp)


def pattern(figure):
    """Pack one 64-row pattern; each voice restates the figure every 8 rows."""
    packed = bytearray()
    for row in range(ROWS):
        for channel in range(CHANNELS):
            if row % 8 != channel * 2:
                continue
            note = figure[(row // 8 + channel) % len(figure)]
            packed.append((channel + 1) | 0x80)  # channel, with a new mask
            packed.append(0x07)  # mask: note, instrument, volume
            packed.append(note)
            packed.append(1)  # instrument 1
            packed.append(48)  # volume column, 0..64
        packed.append(0)  # end of row
    head = struct.pack("<HHI", len(packed), ROWS, 0)
    return head + bytes(packed)


def build():
    patterns = [pattern(f) for f in FIGURE]
    ins = instrument()
    wave = sample_data()

    order = bytes([0, 1])
    header_size = 0xC0 + len(order) + 4 * (1 + 1 + len(patterns))

    ins_offset = header_size
    smp_offset = ins_offset + len(ins)
    pat_offset = smp_offset + SAMPLE_HEADER_SIZE
    pat_offsets = []
    cursor = pat_offset
    for p in patterns:
        pat_offsets.append(cursor)
        cursor += len(p)
    wave_offset = cursor

    header = bytearray(0xC0)
    header[0x00:0x04] = b"IMPM"
    header[0x04:0x1E] = name("Retrovert self-test", 26)
    header[0x1E:0x20] = bytes((4, 16))  # highlight: rows per beat / measure
    struct.pack_into("<HHHH", header, 0x20, len(order), 1, 1, len(patterns))
    struct.pack_into("<HHHH", header, 0x28, 0x0214, 0x0200, 0x000D, 0)
    # flags 0x0D: stereo, use instruments, linear slides
    header[0x30] = 128  # global volume
    header[0x31] = 48  # mixing volume
    header[0x32] = 6  # initial speed
    header[0x33] = 125  # initial tempo
    header[0x34] = 128  # panning separation
    for channel in range(64):
        header[0x40 + channel] = 32  # centre
        header[0x80 + channel] = 64  # full channel volume

    out = bytearray(header)
    out += order
    out += struct.pack("<I", ins_offset)
    out += struct.pack("<I", smp_offset)
    out += b"".join(struct.pack("<I", o) for o in pat_offsets)
    assert len(out) == header_size, (len(out), header_size)
    out += ins
    out += sample_header(wave_offset)
    for p in patterns:
        out += p
    out += wave
    return bytes(out)


def main():
    data = build()
    OUT.write_bytes(data)
    print(f"wrote {OUT} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
