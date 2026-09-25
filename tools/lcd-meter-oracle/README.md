# Model45 meter layout against the original ROM

This test boots the user's hash-verified SC-55 v1.21 ROMs with H8 native
shortcuts disabled. A headless LCD backend retains the real LCD writes.
Each test sends a Model45 `10 01 00` DT1 message through the emulated UART,
waits for the firmware's transfer to complete, then calls the existing
`LCD_Render`. All 55,296 meter-cell pixels are compared with the native
renderer used by the JUCE adapter. No ROM or song is distributed here.

```sh
cmake -S tools/lcd-meter-oracle -B /tmp/sc55-lcd-meter-oracle-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/sc55-lcd-meter-oracle-build -j 6
/tmp/sc55-lcd-meter-oracle-build/sc55-lcd-meter-oracle '/path/to/v1.21-ROMs' '/path/to/55KTIZKE.RCP'
```

The optional song is decoded with the product's MIDI/RCP parser. Every meter
payload is replayed in a canonical Roland envelope, allowing enough time for
a complete LCD update. This verifies each image's layout, not the song's
original timing, extended manufacturer-ID handling, or a host window.

## ROM findings (2026-09-25)

- `04:737c..738f`: copies the 64 received bytes to `00:ff00..ff3f`, then
  sets `CF34` to 300. The payload is not reordered.
- `04:2dcb..2dd5`: sets CCE0 bit 6 while the bitmap timer is active.
- `04:3065..3072`: selects `FF00` instead of the normal meter bank at `FEC0`.
- `04:30b9..3106`: transfers bytes in address order to LCD CGRAM, with the
  existing change cache and optional meter inversion flag. LCD CGRAM retains
  the low five bits of each byte.
- Actual LCD DDRAM after boot and after receiving a bitmap is
  `20..23 = 00 02 04 06` and `60..63 = 01 03 05 07`. Glyphs are therefore
  **upper/lower pairs for successive five-part groups**, not one upper bank
  followed by one lower bank. Only the first column of glyphs 6/7 is visible.

The faulty upper `0,1,2,3` / lower `4,5,6,7` mapping disagreed on 56 of 256
cells for input bytes `00..3f`. A nonuniform fixture disagreed on 121 cells.
Its ROM-derived row masks are frozen in
`tools/firmware-oracle/lcd-meter-render-test.cpp` for a ROM-free regression.

The corrected renderer passed 451 synthetic frames (the nonuniform fixture,
448 individual MIDI data bits, all-off, all-on) and all 109 meter images in
the user-provided `55KTIZKE.RCP`, with zero differing cell pixels. The bit
walk includes the unused columns and upper data bits, as well as every
visible cell. Other meter modes/inversion are outside this test's scope.
