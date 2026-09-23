# LCD rendering regression

Exercises the editor's `SC55LcdRenderer` without ROMs or an audio device.
The renderer follows TX81Z: floating-point dot rectangles and JUCE
`fillRectList`, transformed directly into the destination, with no glyph image
resampling or custom downsampling filter.

```sh
cmake -S tools/lcd-rendering -B /tmp/sc55-lcd-rendering-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/sc55-lcd-rendering-build
ctest --test-dir /tmp/sc55-lcd-rendering-build --output-on-failure
```

Generate a 244x88 Piano 1 fixture using the real background and font:

```sh
/tmp/sc55-lcd-rendering-build/sc55-lcd-rendering-check /tmp/sc55-lcd.png
```

Checks the mean brightness of a six-pixel gap pattern reduced sixfold across
all six phases (the old bilinear image path fails all six); edge coverage at
50–400% effective scales; equivalent direct/host transforms; repeat painting;
display-off/on and changing glyph content. Allows up to 2/255 colour rounding
between equivalent transforms and 3/255 for fractional coverage of the stripes.

The PNG is a synthetic display state, not a live app screenshot. Native window
and host checks on Windows, Linux and macOS remain separate validation.
