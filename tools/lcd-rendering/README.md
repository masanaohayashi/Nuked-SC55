# LCD rendering regression

Exercises the editor's `SC55LcdRenderer` without ROMs or an audio device.
Background and glyphs are composited at the native 741x268 resolution. The
complete image is then drawn with JUCE's high-quality image resampling, with
no conversion of pixels to rectangles and no separately scaled glyph layer.

```sh
cmake -S tools/lcd-rendering -B /tmp/sc55-lcd-rendering-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/sc55-lcd-rendering-build
ctest --test-dir /tmp/sc55-lcd-rendering-build --output-on-failure
```

Generate a 244x88 Piano 1 fixture using the real background and font:

```sh
/tmp/sc55-lcd-rendering-build/sc55-lcd-rendering-check /tmp/sc55-lcd.png
```

Checks agreement with scaling a single composited image at 50–400% effective
scales, repeat painting, display-off/on, and changing glyph content.
The old rectangle renderer fails the single-image comparison at all scales.

The PNG is a synthetic display state, not a live app screenshot. These tests
use software images; native window and host checks remain separate validation.
