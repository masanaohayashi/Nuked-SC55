#!/usr/bin/env python3
"""Create SC-55 private full-resolution panel SysEx from a 2-bit PGM."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


WIDTH = 741
HEIGHT = 268
PIXELS = WIDTH * HEIGHT
RASTER_BYTES = PIXELS // 3
MAX_CHUNK_BYTES = 128
MAX_MESSAGE_BYTES = 142


def read_pgm(path: Path) -> bytes:
    """Read a binary P5 PGM with the SC-55 panel's fixed 2-bit palette."""
    raw = path.read_bytes()
    position = 0

    def token() -> bytes:
        nonlocal position
        while position < len(raw):
            if raw[position] in b" \t\r\n\v\f":
                position += 1
            elif raw[position] == ord("#"):
                newline = raw.find(b"\n", position)
                position = len(raw) if newline < 0 else newline + 1
            else:
                break
        start = position
        while position < len(raw) and raw[position] not in b" \t\r\n\v\f#":
            position += 1
        if position == start:
            raise ValueError("truncated PGM header")
        return raw[start:position]

    if token() != b"P5":
        raise ValueError("input must be a binary P5 PGM")
    try:
        width = int(token())
        height = int(token())
        maximum = int(token())
    except ValueError as error:
        raise ValueError("invalid PGM dimensions or maximum value") from error

    if (width, height) != (WIDTH, HEIGHT):
        raise ValueError(f"image must be exactly {WIDTH}x{HEIGHT}, got {width}x{height}")
    if maximum != 3:
        raise ValueError("PGM maximum value must be 3 (the panel's four palette indices)")
    if position >= len(raw) or raw[position] not in b" \t\r\n\v\f":
        raise ValueError("PGM header has no raster separator")
    if raw[position:position + 2] == b"\r\n":
        position += 2
    else:
        position += 1
    pixels = raw[position:]
    if len(pixels) != PIXELS:
        raise ValueError(f"PGM raster must contain exactly {PIXELS} pixels")
    if any(pixel > 3 for pixel in pixels):
        raise ValueError("PGM raster contains a value outside 0..3")
    return pixels


def packet(command: int, fields: bytes = b"") -> bytes:
    body = bytes((0x7D,)) + b"SC55" + bytes((1, command)) + fields
    checksum = (-sum(body[1:])) & 0x7F
    result = b"\xF0" + body + bytes((checksum, 0xF7))
    if len(result) > MAX_MESSAGE_BYTES:
        raise ValueError("generated SysEx packet exceeds the receiver's 128-byte chunk limit")
    return result


def encode_pixels(pixels: bytes, frame_id: int = 0) -> bytes:
    if len(pixels) != PIXELS:
        raise ValueError(f"expected {PIXELS} pixels, got {len(pixels)}")
    if not 0 <= frame_id <= 0x7F:
        raise ValueError("frame ID must be in 0..127")
    if any(pixel > 3 for pixel in pixels):
        raise ValueError("pixel palette indices must be in 0..3")

    packed = bytearray(RASTER_BYTES)
    for offset in range(0, PIXELS, 3):
        packed[offset // 3] = pixels[offset] | (pixels[offset + 1] << 2) | (pixels[offset + 2] << 4)

    messages = [packet(1, bytes((frame_id,)))]
    for offset in range(0, len(packed), MAX_CHUNK_BYTES):
        address = bytes((frame_id, (offset >> 14) & 0x7F,
                         (offset >> 7) & 0x7F, offset & 0x7F))
        messages.append(packet(2, address + packed[offset:offset + MAX_CHUNK_BYTES]))
    messages.append(packet(3, bytes((frame_id,))))
    return b"".join(messages)


def encode_clear() -> bytes:
    return packet(4)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="output .syx file")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--input", type=Path, help="binary P5 PGM, 741x268, maximum value 3")
    source.add_argument("--clear", action="store_true", help="emit a packet that restores the emulator LCD")
    parser.add_argument("--frame-id", type=int, default=0, help="7-bit frame identifier (default: 0)")
    args = parser.parse_args()

    try:
        data = encode_clear() if args.clear else encode_pixels(read_pgm(args.input), args.frame_id)
        args.output.write_bytes(data)
    except (OSError, ValueError) as error:
        print(f"panel-sysex: {error}", file=sys.stderr)
        return 1
    print(f"Wrote {args.output} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
