# Native sound engine without H8 or JUCE

Build the existing `sc55::NativeSynth` interface as a standalone C++20 consumer.
The `sc55-native-engine` CMake target exposes the control headers and links only
the existing PCM DSP and patch-data decoder. It does not compile H8, MCU interrupts/timers,
sub-MCU, LCD, JUCE, or the plug-in host adapter. This is not a replacement PCM
renderer and does not change the plug-in's Projucer/Xcode projects.

```sh
cmake -S tools/native-engine -B /tmp/sc55-native-engine-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/sc55-native-engine-build -j4
/tmp/sc55-native-engine-build/sc55-native-engine-check "/path/to/SC-55 v1.21"
```

Optionally set `-DSC55_ROM_DIRECTORY="/path/to/SC-55 v1.21"` at configure time,
then run `ctest --test-dir /tmp/sc55-native-engine-build --output-on-failure`.
ROMs stay outside the repository. Loading, waveform decoding and sound-data
import run before rendering; this consumer writes no cache or audio files.

The check instantiates the same default native controller/reference PCM used by
the product, sends MIDI Note On/Off, renders silence/odd/zero-size spans, and
checks the existing product fixture's audio checksum. Link success proves these
operations do not need an H8 implementation; it does not prove every control
feature or real-song output matches H8. Existing integration/host checks remain
necessary. The explicit alternative PCM simulation stays available in the core
but is not selected by this check.

To embed the core, include `sc55_synth.h`, construct `NativeSynth` with imported
sound data and decoded ROM buffers off the audio thread, then call `push`,
`applyCommand`, `render` and `state` on one serialized audio owner. Interpret
panel actions and render display snapshots on the message thread. The caller
splits render spans at MIDI timestamps; control time persists across spans.
