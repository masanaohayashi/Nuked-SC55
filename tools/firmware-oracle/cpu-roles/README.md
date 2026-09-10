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
# Kernel event ordering audit

Song identity observation: with the diagnostic target's `SC55_NATIVE_IO_AUDIT`
enabled, set `SC55_SONG_IDENTITIES=1` for `--song-allocation`, `--song-part16`
or `--song-first-kick`. Each PCM key-on records GS part, allocator group key and
sample start. Reports count differences without comparing exact start times,
plus per-part poly/mono/rhythm counts. This is not a MIDI-note completion verdict:
partials, mono reuse and changed group keys can differ from input note identity.
The observer reads H8 state only; it does not patch it. Collection uses allocating
diagnostic containers, is opt-in and must not be used for CPU measurements.
No observer is compiled into the normal product-check target or plug-in.
It also compares mono retained keys when native queued work is empty, H8 RX and
command rings are empty, and H8 pending voice operations are clear. A zero count
of mismatches proves only these observations, not every intervening audible note.

For a targeted trace, also set `SC55_SONG_TRACE_PART=4` (GS part index 0..15,
not display numbering). `IDENTITY_MIDI` records messages on that part's current
receive channel; `IDENTITY_START/ATTACK/RESTART` record PCM starts and early gain,
and `IDENTITY_MUTE` records observed H8/native mute changes. Mute observations
are at replay boundaries (up to128 frames), not exact button-consumption times.
Use this to separate notes received under different mute states from missing
unmuted notes. The part16 regression's physical H8 buttons and immediate native
commands do not have equal UI latency. Do not add artificial audio delays to
make their key-on counts match. This trace is diagnostic-only, not a benchmark.

To distinguish allocation from actual PCM start, add
`SC55_SONG_TRACE_WINDOW=33.18,38.60` (start,end in song seconds). Within that
window, `IDENTITY_OWNER` records changed slot/group/key, allocation/release state,
and PCM key/latch bits at PCM sample callbacks. It reports slots entering or
leaving the selected part as well. A group-key change alone is **not** a new
audible note: the slot can still contain the previous PCM voice. Match it with
`IDENTITY_START` before concluding that the new owner sounded. This observer
does not see every intervening H8 instruction and never changes H8 state.

Pass start/end replay observations use the actual instruction-entry hook, not
the pre-Step PC (interrupt dispatch can otherwise duplicate an opportunity).
After this correction, `end` and `end-clock-start` still pass; `end-unit` and
`end-clock` still fail the original admission40 survivor assertion. Earlier
pre-Step logs contain three extra opportunities and are not exact pass counts.

`SC55_REPLAY_CAPACITY_PASSES=end-clock-start` consumes the native clock at
observed pass entry and holds that elapsed count until observed completion.
Unlike `end`, it does not feed H8 elapsed counts into native control. It currently
passes48 admissions and rejection/recovery, but still replays START/END times
and runs native work at completion: neither default timing nor sample-level
equivalence is established. The existing product already has captured-count
ownership; this diagnostic identifies execution scheduling as the missing link.

`SC55_REPLAY_CAPACITY_PASSES=end-clock` uses those completion opportunities but
derives elapsed counts from a separate production `ControlTaskClock` advanced by
native frames. Its own zero epoch is retained; `emptyClock` counts opportunities
skipped because no timer event is pending. It is not the same count distribution
or necessarily the same dispatch set as `end`. It currently fails admission40
despite the same26-period total in that window. This diagnostic is not a proposal
to collect elapsed counts at completion; the firmware captures them at entry.

`SC55_REPLAY_CAPACITY_PASSES=end-unit` with `--native-capacity-stealing` is a
counterfactual variant of `end`: identical observed completion times, but one
elapsed period per notification. Compare it with `end` to isolate elapsed-count
batching from the completion schedule. Both are diagnostic-only; neither is a
product scheduler or an audio-equivalence test. The survivor assertion remains
enabled (currently `end` passes while `end-unit` fails at admission40).

`sc55-cpu-roles ROMDIR --native-boundary-reception` extends the full-voice
startup-wake fixture with two explicit PCM device notifications during a reuse
wait. It checks immediate acknowledgement, retained per-voice notifications,
deferred handling, and zero/1/127/257-frame audio equality. These injected device
inputs are not evidence of a naturally observed H8 interrupt/reuse overlap.
`SC55_TRACE_KERNEL_EVENTS=1` with `--native-capacity-stealing` reports read-only
H8 PCM acknowledgement/coalescing/wait-mask observations through admission 40;
the known capacity mismatch is not waived by this diagnostic.

`SC55_TRACE_CONTROL_ROUTINES=1` also reports `[DEBUG-pass-partition]` for
complete5af9..5b70 periodic intervals. Task work, hardware interrupts and
non-IRQ scheduler execution are mutually exclusive; `unobserved` is the
remaining device time, not silently attributed to task8. Buckets use allocated
voice count at pass entry. These are emulator-cycle costs, not host CPU times.

`sc55-cpu-roles ROMDIR --native-command-control-order` checks product dispatch
when Note Off and the periodic voice event are both ready, including a 64-command
backlog crossing the per-service command budget. The first control pass must
observe the release rather than update the old held-note state. The explicit
control event is an order fixture, not a recorded H8 timing schedule.

`sc55-cpu-roles ROMDIR --native-panel-solo` compares physical H8 ALL+MUTE
operations with native solo, MIDI admission and sounding-part counts. It covers
selected-part changes, ignored MUTE during solo, per-part/global mute restoration
and ALL selection. GS reset during selected-part solo and GM reset during ALL
solo are followed by notes and solo exit, checking retained selection/mute
behavior as well as drained voices (27 comparisons). This does not exercise
mouse/touch gestures in the plug-in UI.

`sc55-cpu-roles ROMDIR --panel-options` observes standby option entry through
physical H8 POWER/ALL/MUTE/INSTRUMENT-right switches. It asserts that fast-scroll
can be enabled and disabled and survives return to the playing page. No H8 RAM
is patched. It also drives NativeSynth's semantic standby and fast-scroll
operations, comparing standby mode, option retention, sounding-part counts and
CC7 state. Notes/CC during standby are discarded; new notes work after resume.
This does not validate every standby option or the plug-in GUI mapping.

`sc55-cpu-roles ROMDIR --kernel-notify-sites` inventories raw TRAPA2 byte
matches and their preceding immediate arguments. It separately lists ROM1's
direct interrupt jumps into the notification handler at 0419 (including the
LCD notification omitted by a trap-only inventory). Other-bank matches are counted
separately. This is not proof against branches bypassing argument setup, direct
event writes, or timer notifications.

`sc55-cpu-roles ROMDIR --kernel-events` runs the existing `--events` sequence
with read-only observations at actual H8 instruction entry. It reports explicit
notifications, timer registration/expiration, wait masks, event consumption and
task5/6 entry/return counts. `contextTask` is not necessarily the notification
sender: an ISR retains its interrupted task's current-task field. This is not a
native scheduling replay or proof that unobserved firmware paths are unreachable.

`sc55-cpu-roles ROMDIR --configuration-readers` requires configuring this
diagnostic target with `-DSC55_ORACLE_CONFIG_READS=ON` (default OFF). It reports
actual H8 reads of `UninterpretedSystemSettings` fields during boot, notes,
program change, all CC numbers and GS reset. The observer is enabled only inside
the execution loop, not during diagnostic state dumps. A missing read is not
proof that a field is unused. Do not use this profiling build for H8 CPU-cost
comparisons; the hook adds overhead. Normal product builds contain no hook.
# Normal product control runtime

`sc55-native-product-check` is an explicitly built target without
`SC55_CONTROL_TIMING_ORACLE`. A compile-time check rejects any instruction-time
API in its VoiceControlRuntime. It reuses the existing synth and song checks;
it does not change their assertions or turn diagnostic timing on for playback.

```sh
cmake --build /tmp/sc55-cpu-roles-build --target sc55-native-product-check -j4
/tmp/sc55-cpu-roles-build/sc55-native-product-check "$ROM_DIR" synth
/tmp/sc55-cpu-roles-build/sc55-native-product-check "$ROM_DIR" part16 "$GATCHA_MID"
SC55_KICK_ALL=1 /tmp/sc55-cpu-roles-build/sc55-native-product-check "$ROM_DIR" kick "$KTIZKE_MID"
```

The part16 fixture mutes parts1..15 through the H8 panel after initialization,
not by filtering MIDI tracks. The kick fixture with `SC55_KICK_ALL` replays the
first60seconds under full-song voice pressure. These test the native control
runtime, not Logic/AUv3 registration or every sample of complete audio parity.
