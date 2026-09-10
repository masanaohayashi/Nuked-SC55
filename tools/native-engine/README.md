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

The same Note On/Off fixture also checks that a sounding part has a meter and
that the display meter clears after the voice finishes, even when PCM key bits
remain enabled. It does not clear or otherwise modify those DSP bits.

To embed the core, include `sc55_synth.h`, construct `NativeSynth` with imported
sound data and decoded ROM buffers off the audio thread, then call `push`,
`applyCommand`, `render` and `state` on one serialized audio owner. Interpret
panel actions and render display snapshots on the message thread. The caller
splits render spans at MIDI timestamps; control time persists across spans.

Native polyphony is configurable from 24 to 128 physical voices in steps of 4,
via the final `NativeSynth` constructor argument (default 24). Two-partial notes
consume two voices. All voices share the original 32 kHz clock and effect unit;
extra voices have separate PCM state and do not alias effect slots 28..31.
The plug-in's settings slider rebuilds the native engine outside audio processing
when released. Its value is saved in host state; H8/unsupported ROMs stay at 24
and disable the slider.

`sc55-native-engine-check ROM_DIRECTORY polyphony` exercises every supported
limit, single-/two-partial allocation, stealing, release and reset. The 128-voice
case additionally checks mono ownership and drums stealing from a full pool.
With `SC55_ROM_DIRECTORY` configured, this is the `native-polyphony` CTest.

## Expanded-voice diagnostics

- `sc55-pcm-voice-bank-check` checks all 128 logical pitch banks and their
  isolation from the hardware effect banks. Include `-fsanitize=bounds` in
  diagnostic builds: ASan alone misses out-of-range subarrays inside `pcm_t`.
- `sc55-native-engine-check ROM_DIRECTORY capacity-audio` compares exact audio
  at capacities 24/128 for all 128 melodic programs, without voice stealing.
- `sc55-native-engine-check ROM_DIRECTORY song-release MIDI_OR_RCP_FILE` replays
  a local song at capacity 128, releases notes and pedals, and checks that no
  allocated voices remain after 20 seconds. This is not a pitch-fidelity test.
- `sc55-native-engine-check ROM_DIRECTORY slot-audio OCCUPIED` is an exploratory
  comparison using silent organ voices to relocate a test voice. It includes
  bend, modulation and release. At 127 occupied slots, voice stealing changes
  the scenario; exact differences are observations, not a fidelity verdict.
  Program 76 exhibited a difference in this scenario during investigation.

The September 2026 expanded-pitch regression was reproduced by replaying
GATCHA55 with `-fsanitize=address,bounds,shift-exponent` and
`UBSAN_OPTIONS=halt_on_error=1`: the old pitch-consumer helper indexed
`ram2[127]` even though the hardware bank has only 32 slots. Native integer
voices now bypass that simulation/effects helper and read their logical pitch
banks directly. Host-reported sustained pitch corruption still requires a
matching playback reproduction; removing this invalid read alone does not
prove that every reported audible symptom is resolved.

### Mono reuse and pending completion

ANMDLYGS.RCP provided a matching stuck-note reproduction at capacity 128:
at about 12.731 seconds, part 16's slot 4 finished its old envelope, queued
completion, and restarted for the next mono note before that completion was
consumed. The stale completion then returned the new voice to the allocator
while PCM continued sounding. Its later note-offs could not reach that owner.
An accepted restart now invalidates a pending completion for that same slot;
other slots' completions and the envelope/rendering timing are unchanged.

`sc55-native-engine-check ROM_DIRECTORY song-notes ANMDLYGS.RCP` replays all
parts and checks both PCM gain stages against current voice ownership. The
pre-fix run detects an orphan at 13.739 seconds (slot 4, GS part 15 = MIDI
channel 16). The two gains are serial stages: either zero makes the voice
silent. A short final ramp after retirement is allowed, but one second of
nonzero gains without ownership fails. No extra note-offs or resets are sent.

Set `SC55_PROBE_SECONDS=15` for the short reproduction, or omit it to check the
entire song and its natural end. `SC55_PROBE_VOICES=24` selects a comparison
capacity; the default is 128. These variables affect only this diagnostic.
Configure `SC55_MONO_REUSE_SONG` alongside `SC55_ROM_DIRECTORY` to register the
15-second replay as the `native-mono-completion` CTest. ROM/song data are local
fixtures and are not copied into the repository.
