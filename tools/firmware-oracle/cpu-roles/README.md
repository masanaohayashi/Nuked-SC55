# v1.21 CPU role probes

Local research only; does not resave Projucer or build/install the plug-in.
Requires the user's mk1-v1.21 ROM directory. No ROM is distributed here.
The probe disables native H8 shortcuts and runs the firmware interpreter.

```sh
cmake -S tools/firmware-oracle/cpu-roles -B /tmp/sc55-cpu-roles-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/sc55-cpu-roles-build -j 4
/tmp/sc55-cpu-roles-build/sc55-cpu-roles '/path/to/SC-55 v1.21' --effects
```

Modes (one per invocation):

- `--native-controller-work`: compares input-derived controller calculation
  duration to actual H8 calls under changing GS sensitivities, pressure, bend
  and assignable CCs. Also compares native amplitude/filter/pitch/output state and work,
  including CC72 release and CC65/CC5 portamento with ascending/descending
  notes. Output coverage sends CC7/11/10/91/93, GS random pan and drum notes;
  pan/send motion, frozen pan and tone scaling are required. Requires multiple
  distinct timing paths, both glide directions, amplitude release and natural
  completion. Amplitude completion-service work and the delay exit tail are
  separate boundaries, not included in the EG calculation count. This is one
  component of scheduling research, not a complete control-pass budget or a
  product timing change. Uses the emulator's existing12-cycle convention.

- `--native-shared-rhythm`: actual MIDI/H8 comparison of both shared drum maps,
  receiver-local rejected programs, GS map assignment/reassignment, peer program
  changes and subsequent notes. Checks retained RPN coarse tuning and complete
  edited-map records, plus sounding melodic/map1/map2/melodic transitions with
  hold/sostenuto and final Note Off. Does not compare sample-exact audio or all
  mode combinations.

- `--native-parameter-transport`: H8/native normal RQ1 TX waiting, packet
  equality, retained input during TX and following CC/note after explicit
  completion. No host MIDI output, overflow or disconnect recovery test.

- `--native-source-controller`: all128 configured source CC numbers with
  values0/48/127 on melodic and rhythm channels. Compares source latches and
  bank/volume/pan/chorus/reverb side effects through actual MIDI in both engines.

- `--native-display-control`: compares semantic text/bitmap display control to
  actual H8 MIDI reception, timer ticks and scroll services. Includes short-line
  centering, long scrolling, replacement, bitmap refresh and expiration. This
  also checks NativeSynth MIDI/render/snapshot delivery and expiration without
  an editor. Does not assert H8/product cycle parity or LCD pixel rendering.

- `--mono-pitch`: actual mono MIDI notes/releases with and without CC65;
  observes installed restart flags, pitch stage/progress and return velocity
  at4f51. Asserts both initialization/reentry were reached; not waveform parity.
- `--native-system-defaults`: compares the full1848-byte ROM configuration
  image against boot and GS reset (including receive-bit15 difference), plus
  decoded controller sensitivities. Does not validate native reset scheduling.
- `--native-melodic-presets`: compares all16384 bank/program selections and
  selected-bank fallback with actual H8 MIDI handling; verifies CC0 is a latch
  and does not commit a tone before Program Change. Also checks eight GS tone
  assignment/length cases with MIDI reception disabled.
- `--native-rhythm-presets`: extracts verified-ROM drum presets without H8,
  compares boot and all128 MIDI programs on both maps against complete firmware
  records, then compares54 GS key/name/flag writes. Map selection is set directly;
  GS mode-change side effects are not tested.
- `--native-all-notes`: compares seven live H8 All Notes Off commands with native
  group release, including duplicate keys, hold, sostenuto and rhythm flags.
- `--native-parts`: compares supported native part configuration against all16
  H8 bootstrap rows and real receive-channel/flag/scalar/scale-tuning SysEx writes.
  Channel writes here compare configuration only; native reset side effects are
  covered separately by the native-player and all-notes checks.
- `--native-master`: compares native master settings with actual firmware SysEx
  writes, including scalar table continuation and the special tune length gate.
- No mode: boot, idle, note-on/off task accounting.
- `--events`: MIDI/control event dispatch observation.
- `--system`: settings table, GS/GM reset ordering, selected parameter writes.
- `--extended`: display, panel, bulk writes and firmware RQ1 ring generation.
  The current emulator stalls waiting for TX drain; the probe resets between requests.
- `--extended-tx`: explicitly supplies a diagnostic ready-TX interrupt when TIE/TX/TDRE
  are set. This exposes response generation/drain without changing product code.
  It is NOT a test of unmodified UART behavior or host MIDI output support.
- `--effects`: boot and four parameter changes; logs transition states and PCM RAM
  snapshots, asserts both effect state machines eventually return to idle.

Checks are deliberately scoped. PCM RAM changes between snapshots can include
autonomous modulation and are not automatically attributed to the input message.
PCs are sampled before Step, so interrupt entry can affect per-PC accounting.
Emulator cycles are not a claim of hardware-accurate H8 instruction timing.
These are not performance benchmarks, exhaustive protocol tests or native audio
equivalence tests. Findings and remaining boundaries are in docs/firmware/CPU_*.
