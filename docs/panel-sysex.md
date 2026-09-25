# Full-resolution panel drawing over SysEx

The plug-in accepts an optional private SysEx extension for drawing directly
into the SC-55's 741x268 LCD image. It is separate from Roland Model 45's
standard display text and 64-byte CGRAM messages. Raster data is consumed by
the plug-in UI and is not sent into the synthesizer engine.

## Image

The image is exactly 741x268 pixels. Each pixel is a two-bit palette index:

| Value | Rendering |
| --- | --- |
| 0 | Transparent; show the LCD background |
| 1 | Black |
| 2 | Panel orange (`#c85000`) |
| 3 | White |

Three pixels are packed into one seven-bit MIDI data byte, in left-to-right
order: `pixel0 | (pixel1 << 2) | (pixel2 << 4)`. The image contains exactly
66,196 packed bytes. Pixel rows do not need padding because 741 is divisible by
three.

## Packets

Each packet is a complete SysEx message:

```text
F0 7D 53 43 35 35 01 COMMAND FIELDS... CHECKSUM F7
```

`7D` is MIDI's non-commercial manufacturer ID, `53 43 35 35` is the `SC55`
signature, and `01` is the extension version. The checksum is
`(-sum(bytes from 53 through the last field)) & 7F`; including the checksum,
that sum is zero modulo 128. All data bytes between the `F0` and `F7` framing
bytes must be below `80`.

Commands are:

| Command | Fields | Meaning |
| --- | --- | --- |
| `01` | `frameId` | Begin a new transfer and discard any incomplete transfer |
| `02` | `frameId offsetHi offsetMid offsetLo data...` | Write 1–128 packed bytes at the encoded-byte offset |
| `03` | `frameId` | Atomically display the frame if every packed byte arrived |
| `04` | none | Remove the overlay and return to the emulated LCD |

Offsets use three 7-bit groups, most significant first:
`offset = (offsetHi << 14) | (offsetMid << 7) | offsetLo`. Chunk offsets may
arrive in any order; repeated chunks replace their earlier bytes. The frame ID
must match the active transfer. An incomplete or invalid `COMMIT` leaves the
previously displayed frame intact. The display changes only on `COMMIT` and
remains until another frame is committed, `CLEAR` is received, or the emulator
is reinitialized. Invalid checksums, invalid palette bits, and oversized
packets are ignored. Disabling SysEx reception also disables this extension.

## Creating a `.syx` file

The bundled encoder reads an 8-bit binary P5 PGM whose dimensions are exactly
741x268 and whose maximum value is 3. Pixel values in its binary raster are the
palette values above. It emits a `.syx` file containing `BEGIN`, all required
128-byte-or-smaller chunks, and `COMMIT`:

```sh
python3 tools/panel-sysex/encode.py --input panel.pgm panel.syx
```

To remove the custom image and resume the emulator's screen:

```sh
python3 tools/panel-sysex/encode.py --clear clear-panel.syx
```

Send the file through a MIDI track or a MIDI utility that accepts concatenated
SysEx messages. This is a project-specific extension; real SC-55 hardware does
not implement it.
