# Native system integration work log

## Objective and completion gate

Complete the H8 replacement, not just opcode acceleration or the melodic preview.
Normal product rendering must use native MIDI/part/voice/control/FX/system behavior
without executing H8, retaining PCM sound generation and UI ownership boundaries.
Current goal remains active: that gate has NOT been reached.

## Implemented in this step

- `sc55_sysex.h`: bounded, engine-owned SysEx staging behind MidiDecoder. No allocation,
  H8 state or firmware access. Framing supports model42/45, RQ1/DT1, GM on/off recognition.
  Only complete validated packets reach the dispatcher; realtime, interruption,
  device ID, checksum policy, overflow draining and recovery are explicit.
- `MasterControls`: native tune/volume/key-shift/pan values and GS master writes.
  NativeMelodicPlayer applies them to existing voice inputs and subsequent note setup.
  Master transpose uses the already translated part/master key arithmetic and retains
  original source key separately from transposed initial key.
- Scalar multi-setting messages follow table-record order. Master tune is different:
  ROM1511 requires complete command/address/data length8, so a tune followed by further
  settings in one packet is rejected. This distinction was caught by the H8 comparison,
  not inferred from a generic Roland protocol rule.

Recognition of a packet is NOT implementation of its effect. General GS/GM reset,
part/drum settings, bulk, readback and model45 display dispatch remain to be connected;
the player continues to count those recognized but unimplemented requests explicitly.
The default product mode was not switched to the incomplete native player.

## Verification

- `sc55-sysex-test`: single-byte fragments, realtime, aborted packet, bad checksum and
  explicit checksum override, device mismatch, oversize/recovery, GM framing, master
  bounds and scalar table continuation/tune length rejection.
- `cpu-roles --native-master`: 12 actual MIDI SysEx transactions on v1.21 H8 compared
  with native values; all four master fields compared after each request. Includes
  out-of-range values, non-nibble bytes, scalar continuation and rejected tune suffix.
  Passed; `/tmp/sc55-native-master.log`.
- `native-player`: H8-free PCM test extended with master write/rejection and audible
  live-note mute/unmute. Existing sustain, saturation/steal/reclaim remain covered.
  The test checks MCU PC/cycles remain zero. This is not full GS audio equivalence.

Commands:

```sh
cmake -S tools/firmware-oracle -B /tmp/sc55-firmware-oracle-build
cmake --build /tmp/sc55-firmware-oracle-build --target sc55-sysex-test sc55-firmware-oracle -j 4
ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^(sysex|native-player)$' --output-on-failure
cmake --build /tmp/sc55-cpu-roles-build -j 4
/tmp/sc55-cpu-roles-build/sc55-cpu-roles '/Users/ring2/Documents/roms/Synths/Roland SC-55/SC-55 v1.21' --native-master
```

Existing local CMake cache supplies the MD15 asset and wave directory for native-player.
No resave, release build, install, commit or push. Concurrent UI/generated-file edits
in the workspace were left untouched. Plug-in hosts/architectures were not retested.

## Next implementation dependencies (not a reduced goal)

1. Part/system settings owner with ROM-derived boot defaults, GS field mapping and
   side effects; connect receive routing and note inputs rather than fixed preview values.
2. Full note admission including drum/bank and mono/reentry, using existing components.
3. Native FX state machines and reset sequencing, bulk/readback/display services.
4. Replace the normal product H8 path only after integration/compatibility checks;
   compare Release CPU and audio with the reference under the same conditions.

See CPU_NATIVE_READINESS for the complete responsibility matrix and known gaps.

## Part settings and routed native playback

Added `sc55_part_settings.h`, an audio-owned configuration with actual GS part
indices (part0 defaults to MIDI channel10, part1 to channel1). Routing is not
inferred from the low nibble of the MIDI status when installing a voice.
The supported bootstrap fields match all16 parts after reference cold boot;
this does not establish a complete ROM-derived boot/reset image.

Connected settings:

- Receive flags03..12, per-part key shift16, volume19, velocity depth/offset1a/1b,
  pan1c, key bounds1d/1e, sends21/22 and the12-byte scale table40.
- Scalar continuation follows the firmware parameter table; an unsupported later
  record preserves earlier writes. Scale tuning requires the complete expected length.
- Note admission uses the selected part's key range, velocity, scale and key shift.
- MIDI program/scalar CC are routed to every matching enabled part, with per-CC
  receive gates. SysEx pan0 remains0, unlike MIDI CC10's0->1 conversion.
- Native NoteOn uses the existing descending-part fanout and retains each admitted
  part across asynchronous PCM startup/capacity work. NoteOff, pedals, pressure,
  pitch bend and stop selection use the same part routing.

Verification:

- `cpu-roles --native-parts`: 64 comparisons against live v1.21 H8. Each comparison
  checks all supported fields of the target part, not just the field being written.
  Includes all16 bootstrap part rows,16 receive-bit gates off/on, scalar sequences,
  shift clamp, pan0, inverted key bounds and12-byte scales. PASS; log
  `/tmp/sc55-native-parts.log`.
- `ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^(sysex|midi-decoder|native-player)$'
  passes3/3. Native PCM integration includes two parts on one input channel, shared
  program/volume, both-part release/reclaim, GS note gate and inverted key rejection.
  Final MCU PC/cycles remain zero. Previous master mute/unmute and polyphony checks pass.
- No plug-in resave or host installation. The diagnostic build recompiles the
  NukedSC55Emulator adapter against the changed header; this is not an all-format build.

Remaining limitations are NOT completion: bank/tone changes,
fine tuning, mode/rhythm changes and modulation setting rows remain to be connected.
FX sends are stored but preview still suppresses them until native FX integration.
Normal product rendering still uses H8. Full replacement goal remains active.

## Receive-channel reset and channel-mode release

GS receive-channel02 now invokes the native part-reset boundary even when the
incoming channel equals the current value. The reference handler04:1173 calls
04:0844 (not page0:0844), queues controller/pedal reset, then All Notes Off.
Native reset preserves program/volume/tuning while clearing expression, soft
pedal, controller contributions and key pressure; releases hold/sostenuto state
before group release. Portamento state is not yet integrated into this player.

CC121 resets controllers/pedals; CC123/124/125 request group release, not a hard
PCM stop. Channel-mode commands require value0 and route without the ordinary
note/CC receive gates. All Notes Off preserves hold/retained-key release rules.

Verification recorded in `/tmp/sc55-all-notes.log`: seven actual firmware
commands match the native allocator/group state, including duplicate notes,
hold, sostenuto and drums. `/tmp/sc55-native-parts.log` now reports72 supported
field comparisons. The latter's channel-reset callback is configuration-only,
not proof of full reset equivalence. Native-player integration covers part
isolation, channel reassignment, same-channel reassignment, retained-note release
and preserved volume/program. Complete portamento/reset compatibility remains open.

## Engine-owned rhythm start boundary

`NativeVoiceEngine::startRoutedRhythmNote` now owns the previously test-assembled
sequence: rhythm admission/exclusive and repeated-note retirement/capacity,
stop-task publication, sample installation, DSP preparation and async PCM start.
Admission uses live DSP lifecycle state and the part's retained keys. Stop tasks
are serviced before installation so a different choked slot cannot outrank the
new preparation task. No PCM clock advancement occurs inside the entry point.
Busy startup returns deferred before admission or I/O. Once admission has made
side effects, subsequent failure latches the engine rather than allowing replay.

The bank2 loop in `voice-control-pcm-test.h` uses this engine entry instead of
assembling the operations itself. Validation on the local MD15 asset and real
waveforms:

```sh
cmake --build /tmp/sc55-firmware-oracle-build --target sc55-firmware-oracle -j 4
ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^(native-wave-output|native-player)$' --output-on-failure
```

PASS2/2 (28.86s), `/tmp/sc55-rhythm-engine-tests.log`. Exercises bank2 tones
224..385, nonzero PCM output, choke/reclaim/reuse with no H8 execution. Busy-entry
checks assert unchanged allocator/lifecycle and forbid I/O. Tone224 additionally
retriggers during active ownership to exercise the internal choke-to-start path.
This is native integration evidence, not a ROM-vs-native waveform equivalence
claim or a Release CPU measurement.

The supplied rhythm map, pitch/control defaults and receive-adjusted selection
are still explicit research inputs. ROM-derived map initialization and routing
from `NativeMelodicPlayer` remain to be connected; the ordinary product still
executes H8. No generated projects, host installation, commit or push in this step.

## ROM-owned rhythm presets and program resolution

Added `RhythmPresetTable` and setup-only `ImportRhythmPresets`. The latter checks
the exact v1.21 ROM pair using the existing SHA256 gate before extracting the
program lookup at03:8000, complete0x48c-byte records at03:8080, and program127
velocity accumulators at03:d168. No ROM bytes are embedded in source. This is
currently a separate configuration import, not an MD15 cache-format change.

The native resolver follows MIDI handler04:0964..0998: an unmapped program below64
tries its eight-program family then program0; unmapped programs64..126 reject
tone selection while preserving the previous shared map. Program127 selects
its actual indexed record. `decode` produces the existing `RhythmKeyMap` fields
without losing negative tone sentinels; the full record retains the other GS data.

Validation: `cpu-roles --native-rhythm-presets` PASS, logged in
`/tmp/sc55-rhythm-presets.log`. Compares initial map0 plus all128 real MIDI program
changes on each of the two maps (256 transitions): every record byte, all decoded
note rows, and invalid-tone state match the interpreter. Map selection for the
second pass is changed directly in diagnostic RAM; this is not a test of GS
mode-change side effects or shared-part program propagation.

Next integration boundary remains connecting these imported presets and mutable
maps to native MIDI routing, per-key output controls and the new rhythm start
entry. Neither normal product H8 removal nor full reset/FX compatibility is
established by this focused result. No plug-in resave, commit or push.

## Routed native rhythm playback

The preview player now accepts an owned `RhythmPresetTable` at construction.
The plug-in's existing `NUKED_SC55_NATIVE_PREVIEW=1` setup imports it from the
verified ROM pair, outside rendering. MIDI program changes select/copy the
shared map and propagate the requested program to parts using that map;
invalid programs preserve the map but reject subsequent tone admission for
the receiving part. Nonzero-bank drum selection remains unsupported.

Rhythm Note On now follows the existing receive fanout, key receive flag,
velocity adjustment and key-range gates, mapped tone/velocity expansion,
engine rhythm admission and async PCM activation. Per-voice map/key ownership
retains access to key volume/pan/send scales when controls change; melodic slot
reuse clears that ownership. Master transpose does not alter drum initial keys.
FX sends are still suppressed pending native FX setup. No new callback allocation,
wait or H8 execution is introduced in these paths.

Focused validation: native-player CTest PASS (3.83s),
`/tmp/sc55-routed-drum-tests.log`. Existing melodic/master/pedal/channel tests
remain. New cases play a mapped note from each of ten presets through fragmented
MIDI and real wave PCM; verify hi-hat choke, invalid-program silence, matching-map
program propagation, second-map independence, per-key volume0 and reclaim.
Final MCU PC/cycles remain0. This checks integration, not waveform equivalence
against a simultaneous H8 rendering or Release performance.

`cmake --build /tmp/sc55-cpu-roles-build -j 4` also rebuilt the JUCE adapter
successfully (`/tmp/sc55-routed-drum-adapter-build.log`, existing u8path warnings).
No resave, format packaging, host load, commit or push. Normal product mode still
uses H8. Rhythm GS edits/reset, complete initial state, bank variations,
mono/portamento, FX, remaining services and default-mode replacement are open.

## GS rhythm map writes

Connected model42 DT1 `41 mx kk` through the shared map owner. Supported fields:
name00 (12 bytes, key0 only, minimum character32), pitch01, level02, group03,
pan04, reverb05, chorus06, Note Off receive07 and Note On receive08.
Byte rows write sequential bytes from the requested key, including row-boundary
continuation; they do not advance through the GS parameter table. Flag fields
clamp to boolean and consume only the first data value. Name-length/address
failures leave the record unchanged. The player refreshes decoded key rows and
active output controls after successful writes, without changing the ROM presets.
Effects sends remain stored but suppressed until native FX integration.

`cpu-roles --native-rhythm-presets` now additionally verifies54 GS transactions
against both complete live H8 map records (`/tmp/sc55-drum-gs.log`): all fields,
multi-key writes, key127 crossing, flag trailing-data behavior, name clamping and
rejected lengths/addresses. Existing boot/256 program-transition checks also pass.
Native-player CTest PASS (5.95s), `/tmp/sc55-drum-gs-player.log`: GS level restores
audible PCM from a muted key; receive-off prevents allocation; receive-on restores
it; final H8 PC/cycles stay0. This does not establish full SysEx/reset/FX parity.
Diagnostic adapter and player compile successfully; no host installation/resave,
commit or push. Normal-mode H8 replacement remains unfinished.

## Imported melodic bank selection

`MelodicPresetTable` owns the complete03:0000 bank/program tone-ID table, loaded
by setup-only `ImportMelodicPresets` after the existing exact-ROM SHA256 check.
The resolver implements04:09c4..0a24: a defined entry is used directly, including
bank127; an undefined entry rejects bank>=64 or program>=120, otherwise trying
the eight-bank family then bank0. This is data extraction plus native selection,
not H8 execution or ROM data embedded in code.

`cpu-roles --native-melodic-presets` PASS: all16384 combinations match actual
MIDI-driven H8 tone IDs and fallback-bank cache values.128 CC0 changes also
verify that the selected tone is unchanged before Program Change. Log:
`/tmp/sc55-melodic-presets.log`.

The player now separates pending CC0 bank from the bank/tone committed by
Program Change. Allocation accepts an explicit resolved tone instead of forcing
the capital lookup. Rejected selections do not reuse the previous tone, while
unsupported import-less bank selection remains explicit. Drum bank commit also
uses Program Change instead of acting on pending CC0. The plug-in's opt-in
native setup supplies both imported tables; normal mode remains H8.

Native-player CTest PASS (5.25s), `/tmp/sc55-native-bank-player.log`: five
bank/program pairs including bank127 produce real PCM and reclaim their voices;
CC0 alone leaves the committed tone unchanged; an undefined bank64/program0
does not allocate a voice. Existing melodic, drum and GS tests remain passing,
with final H8 PC/cycles0. An initial test wrongly assumed bank127/program0 was
absent; ROM bytes37f00 and the exhaustive H8 probe established tone189, and the
test now explicitly requires playback there. The JUCE adapter diagnostic build
also passes (`/tmp/sc55-native-bank-adapter.log`, existing u8path warnings).
No full host/format test or Release CPU measurement, resave, commit or push.
GS tone assignment, complete reset, mono/portamento, FX/services and normal-mode
H8 removal are still required by the full goal.

## GS part tone assignment

`PartSettings::write` supports `40 1p 00` through an explicit selection callback:
exactly two data bytes (bank/program) are required, with no scalar continuation.
Short/long payloads do not invoke selection. A caller without a selection owner
still reports unsupported rather than only writing configuration bytes.
The player supplies that owner and reuses `commitProgram` for MIDI and GS.
GS stores bank/program before selecting, bypassing MIDI program receive flags;
this also preserves the nonzero-rhythm-bank distinction from MIDI's handler.

The live H8 melodic preset probe now includes eight GS cases with MIDI receive
flags disabled: valid/fallback/absent selections, bank latch, short/long rejection.
PASS along with the existing16384 MIDI combinations (`/tmp/sc55-gs-tone.log`).
Native-player PASS (4.61s), `/tmp/sc55-gs-tone-player.log`: a disabled MIDI
Program Change is ignored, GS still selects and audibly plays a variation;
an incomplete pair preserves selection; GS also selects and plays a drum set.
Existing tests and final zero-H8 assertion pass. The diagnostic build compiles
the JUCE adapter as well. No resave, host/format test, commit or push.

Full reset/initial-state ownership, mono/portamento, remaining controller settings,
FX and service integration still precede normal-mode H8 removal.

## ROM system defaults

Added `SystemDefaults` and exact-ROM-gated `ImportSystemDefaults`: the1848-byte
table03:ca00..d147 copied by04:1e6a is now owned native data. It retains the full
table, while typed projections populate currently implemented master, part and
controller fields. All six modulation-source sensitivity rows are decoded with
the skipped fourth byte preserved in the original data: mod wheel28, bend34,
channel pressure40, poly pressure4c, assigned controllers58/64. Dynamic controller
contributions and key pressure start zero. Unconnected settings remain in the
raw table; merely importing them does not implement their behavior.

`cpu-roles --native-system-defaults` PASS (`/tmp/sc55-system-defaults.log`):
every byte agrees after reference boot and GS reset, allowing only the proven
post-reset receive bit15 overlay; controller row projections also agree. Reset's
typed part projection exposes this distinction instead of reusing boot flags.
Both rhythm maps are separately initialized from03:8080 by04:1fea..2011, matching
the already integrated native rhythm defaults.

The product's opt-in native setup now uses the ROM-backed constructor. This
replaces preview master volume100 with actual127 and replaces placeholder
controller sensitivities (notably mod-wheel depths) with imported values. The
custom PartSettings-only constructor remains for explicit research fixtures.
The complete defaults are retained for the forthcoming reset state machine.

Native-player PASS (7.41s), `/tmp/sc55-defaults-player.log`: the new constructor
applies the ROM master values, produces real PCM with mod wheel input and
reclaims the note; all previous integration cases and zero-H8 assertion pass.
The JUCE adapter diagnostic build also passes. No resave, host install, commit
or push. This step establishes defaults, not the full native GS reset sequence:
voice-stop/drain, configuration restore, FX reset and input-resume ordering are
still to be connected; normal-mode H8 remains unchanged.

## Native reset voice/configuration boundary

`NativeMelodicPlayer` now accepts GS reset and GM On with imported defaults.
The audio-owned requested/draining phases stop all occupied slots, continue
normal periodic control and PCM advancement, and wait for logical reclaim plus
PCM envelope reuse readiness before replacing engine state. The control clock
epoch is preserved. Configuration/controller defaults, both drum maps, bank/tone
selection and note history are restored before subsequent queued MIDI resumes.
The receiver consumes EOX once; no sleep, external task or H8 execution is used.
Importless research callers explicitly count reset as unsupported.

Focused build and `ctest --test-dir /tmp/sc55-firmware-oracle-build -R
'^native-player$' --output-on-failure` PASS (5.10s). Added cases cover:

- sustained melodic plus drum owners; incomplete SysEx cannot start reset;
- post-EOX reset holds the following Program Change/Note On until restore,
  advances real PCM frames while draining, then produces audio;
- old sustain does not retain the new note; drum program and master restore;
- two consecutive idle GM resets complete exactly twice;
- incorrect checksum cannot reset; all 24 occupied PCM slots reclaim on reset;
- the complete native-player test still ends with zero H8 PC/cycles.

The JUCE adapter diagnostic target also builds (`/tmp/sc55-reset-adapter-build.log`).
This proves the tested native voice/configuration boundary, not full firmware
reset equivalence: FX reset/setup is still disabled, GM Off and mode-specific
services remain, and the native reuse wait is not a claim of H8 cycle timing
parity. Normal product mode still uses H8. No host test, resave, commit or push.

## RPN bend range and fine tuning

ChannelControls now implements Data Entry MSB/LSB for RPN0 (bend range) and1
(fine tuning), alongside existing RPN2 coarse tuning. Range clamps0..24 and
updates the bend-source sensitivity used on subsequent bend input. Fine tuning
uses the full14-bit value and truncates its signed conversion to1000 pitch
units per semitone exactly as04:0BC5. MSB clears the old LSB; LSB is ignored for
bend range/coarse tuning. Part routing retains the CC and RPN receive gates.
Fine pitch is refreshed in active voice inputs, not deferred until a new note.

The entry latch is separate from the calculated pitch: boot starts the latch
at0 with pitch0, and04:0844 clears the latch without recalculating AB76. Native
CC121 preserves pitch while clearing the latch. Full system defaults reset
both. NRPN dispatch and Data Increment/Decrement remain separate pending work.

`--native-rpn` PASS:78 real MIDI messages compare range, fine pitch and coarse
tuning to H8, including LSB-first after boot, extrema, MSB/LSB order and ignored
LSBs. `/tmp/sc55-rpn.log`. Native-player PASS (5.41s): changing fine tuning
raises live PCM pitch, accepts24-semitone range, and existing wet effects/reset
tests retain the zero-H8 assertion. Adapter diagnostic target builds. No host
performance/equivalence claim, resave, commit or push; full H8 goal remains open.

## NRPN selectors and drum output controls

ChannelControls now owns both NRPN selector bytes (initial/reset FF, distinct
from received7F). PartSettings switches RPN/NRPN mode after the CC receive gate
but before the family-specific receive gate, matching04:0D74..0E08. Disabled
NRPN receive can therefore change mode without updating the selector.

The player dispatches NRPN1A/1C/1D/1E Data Entry MSB into shared drum key
level/pan/reverb/chorus rows100/280/300/380. It checks matching channel, CC and
NRPN receive gates, rhythm assignment and a valid key; bit6 selects the NRPN
map as at04:0B7C. Active output inputs are refreshed as well as subsequent
notes. CC121 clears both selector bytes, and full reset restores drum records.

`--native-nrpn` PASS (`/tmp/sc55-nrpn.log`):63 real MIDI messages compare
receiver mode/selector values and all four output-row mappings at keys0/38/127,
values0/64/127. Native-player PASS (5.66s): a drum key muted via NRPN produces
no significant output, restoring its level produces audio without unsupported
events, and the final zero-H8 assertion passes. Adapter diagnostic build passes.

Still pending: NRPN melodic vibrato/filter/envelope parameters, drum relative
pitch, broader map/routing tests and the other remaining full-engine duties.
This step does not claim all NRPN or normal-mode H8 replacement is complete.

## NRPN tone controls connected to voice preparation and updates

`ToneControls` implements the eight part10..17 controls: vibrato rate/depth,
filter cutoff/resonance, envelope attack/decay/release and vibrato delay.
NRPN MSB1 selectors08/09/20/21/63/64/66/0A clamp to14..114, except cutoff's
maximum80, matching04:0C05..0CA1. SystemDefaults imports their original bytes.
The player dispatches them before drum-only NRPN routing, retaining receive
gates, and refreshes the active amplitude/second-envelope and first-LFO inputs.

New voices receive the modulation controls before LFO initialization. The
second-envelope preparation previously failed to copy caller timing and
resonance control into its setup; it now takes those controls before deriving
patch/key/velocity scales. Existing prepared base/limit values are retained on
later scalar updates. Vibrato delay therefore affects initialization while
rate/depth also feed ongoing modulation updates.

Validation: `--native-nrpn` PASS,120 real MIDI messages, with every tone field
checked against H8 at six values including clamp boundaries. Log
`/tmp/sc55-tone.log`. Native-player PASS (5.89s): NRPN attack114 versus14 changes
the actual PCM onset amplitude; all eight controls can be updated while a
note runs without an unsupported event, and cutoff clamps80. Existing
effects/reset/drum/RPN tests still pass and final H8 PC/cycles stay zero.
This is not full per-parameter waveform equivalence or a CPU benchmark.

Remaining full-engine work includes drum relative pitch, mono/portamento,
additional GS controls/services and normal-path integration/compatibility.
No resave, commit or push; the full H8 goal remains active.

## Drum relative pitch NRPN18

Implemented ROM-baseline relative pitch (04:0CA4..0CFC) in the native player.
The value is clamp(immutable preset pitch + Data Entry - 64, 0, 127), not an
increment on the edited map. Only the addressed decoded pitch is refreshed;
existing voices retain their initial key, subsequent notes use the new value.

The first comparison exposed an incorrect requested-program assumption:
program1 actually uses program0's baseline. 04:095C first writes the requested
program to CE34+2*part; 04:09A1 overwrites it after successful fallback. Rejected
undefined programs >=64 retain the requested latch, so NRPN18 skips their
undefined record. 04:0A9B/0AAD propagate the resolved low byte to shared-map
peers independently of their displayed/requested program. The native player
now owns a separate committed rhythm-program array, propagates successful
selections and clears it on system reset.

Validation: `--native-nrpn` PASS: 2,808 MIDI CC messages including 1,920 relative
pitch comparisons (all128 programs, keys0/38/127, values0/127/76/76/64), with
every program latch checked against real H8. Log `/tmp/sc55-drum-pitch.log`.
`ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^native-player$'
--output-on-failure` PASS in6.12s. Fresh PCM instances confirm program1 and0
have equal baseline pitch, value76 raises the actual PCM pitch register, and
repeating76 does not accumulate. The final zero-H8-cycle assertion passes.
This does not establish whole-wave equivalence, host CPU improvement or full
GS rhythm-mode transition compatibility. Mono/portamento, further GS/services
and normal-path integration remain. No generated projects/resave/commit/push.

## Mono/portamento pitch re-preparation boundary

`PrepareNormalVoicePitch` now optionally accepts the live VoicePitchRunner and
elapsed control ticks for the installed restart-bit-clear branch at00:4F51.
It takes glide/correction/progress from that live owner, not the old preparation
snapshot, installs the newly calculated targets/rates, then reenters the current
stage. Normal initialization remains the default and still takes one tick.
Invalid stages are rejected before PCM random reads. Legal inactive stages are
not treated as errors; firmware's reentry dispatch can legitimately return idle.

Targeted `voice-control-pcm` and `native-player` tests PASS (24.65s/5.46s).
The added data-only preparation checks cover all12 legal stages with nonzero
progress, a deliberately different live glide and a nonzero cached correction.
Zero elapsed isolates state preservation from interpolation. These assertions
are not an independent full-preparation comparison with H8.

New real-MIDI `cpu-roles --mono-pitch` probe runs the unmodified interpreter:
program80, CC126=1, keys60/64/67 then releases, followed by CC65=127 and60/67.
Log `/tmp/sc55-mono-trace.log` observes12 initialization and4 reentry visits.
Without portamento, the same slots23/22 have installed flagsFF for both new and
return notes. With portamento, first note uses21/20 flagsFF, then legato and
return reuse those slots with flags5F. Reentry stages4 and10 and progress65535
occur in this actual sequence. Return notes use velocity110, not the earlier
key's velocity40/80. This proves the branch is reachable, not audio equivalence.

Consequently mono mode alone must NOT select the new API. The still-pending
native mono transaction must resolve allocation/reuse/restart flags first and
preserve live owners through sample installation and all EG preparations.
The new optional path is not yet called by the product preview: the normal
voice DSP composition still handles restarted notes only. Normal product H8
behavior is unchanged. No claim of complete mono support or CPU improvement.

## Continuing-voice PCM freeze before pitch preparation

Implemented `PrepareReusedVoicePcm` for00:2C18..2C52. The restart-bit-clear
branch skips fresh amplitude/TVA/second-envelope initialization, writes PCM16
00B4, caches the current amplitude/second levels with low byteAF, preserves
the first EG stage in savedStage and temporarily selects stage24. Repeating
the operation at stage24 does not overwrite the saved stage. Its prepared
PCM1A/post-enable fields feed the existing activation transaction. The pitch
reentry added above follows this branch; resetting all other EG owners would
be incorrect. Integration must also synchronize command caches in the live
amplitude/TVA owners rather than just copying VoiceStopState.

The expanded `--mono-pitch` probe independently snapshots real state at2C18
and compares native output at2C52, without modifying/intercepting the H8
execution. `/tmp/sc55-mono-freeze.log`:15 initializations,6 reentries and6
matching freeze transactions. Additional native checks exercise repeat-at24
and activation's restoration of the saved stage. This is a lifecycle-boundary
comparison, not complete native mono rendering.

Added program73(single partial) -> program80(two partials) while a mono note
is held with portamento enabled. Actual firmware changes slots19 ->17/18 and
flagsFF (fresh preparation), then reuses17/18 with5F on return to the held key.
Therefore merely keeping a previous physical group across all program changes
is wrong. The mono selection/configuration invalidation owner and continuing
DSP/startup integration remain required before enabling native mono input.

## Continuing DSP preparation connected to the runtime

The installed-note dispatcher now accepts non-restarted normal samples.
`VoiceControlRuntime::prepareAndBeginNormalStart` binds each continuing entry
to its own live voice and retains the live second-envelope derived settings.
The borrowed pointer is used only during synchronous preparation; the startup
batch still owns copies. A missing live owner is rejected before DSP I/O.

`PrepareNormalVoicesDsp` branches on installed restart flags. The continuing
branch preserves amplitude/second-envelope state and LFO phase, skips fresh
amplitude/TVA/TVF and modulation initialization, calls PrepareReusedVoicePcm,
synchronizes command caches, and performs pitch reentry using the live runner.
The existing activation continuation restores the saved amplitude stage. The
pair's first restart flag controls shared LFO initialization (556F..5590).
Invalid pitch stages are rejected before device writes. Pitch-source255 is
accepted as the no-source sentinel;128..254 remain invalid at dispatch.

Tests now exercise the composed DSP freeze/reentry/activation path with
nonzero amplitude/TVF/pitch progress and distinct first/second LFO phases.
The old sample-dispatch test's unsupported-reuse expectation is replaced by
ordered task consumption and exact installed-flag checks for both branches.
A runtime-level test binds its own live owner, reserves startup and verifies
retention of a nondefault second-envelope limit instead of fresh defaults.

This connects the DSP/runtime boundary, not MIDI mono routing. The player's
mono held-key/current-key/program invalidation/allocation transaction still
needs to select and install the existing group before calling this path.
Whole-note native/H8 waveform equivalence and performance remain unverified;
normal product routing still uses H8. No resave, commit or push.

Validation for this connection: Release headless build succeeded. Final
`ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^voice-control-pcm$'
--output-on-failure` PASS82.76s, including runtime-owned binding/startup
reservation and preserved derived settings. Earlier in the same change,
`voice-control-pcm` and `native-player` both passed (68.85s/19.45s), before the
final runtime derived-setting retention assertion was added. These wall times
are test durations, not a performance comparison. Final build log:
`/tmp/sc55-reuse-dsp-final-build.log`.

## Existing mono group connected through installation/startup

`PrepareMonoReuseAllocation` converts an already-resolved tone/velocity into
the existing group's tail/head destinations, using the established mono reuse
selector and ordered partial dispatch. It does not create a group, consume
free voices or update the part's current key. Zero candidates return explicit
velocity rejection; missing/corrupt selection stays invalid.

`VoiceControlRuntime::startReusedMelodicNote` and the engine entry connect that
selection to sample installation, continuing DSP preparation and the existing
asynchronous PCM startup. Busy lifecycle/startup defers before installation.
Missing live voice owners reject before I/O. After installation begins, failure
latches rather than replaying the note. The caller still owns fresh-group
invalidation, restart flags and commit of current key/preparation velocity.

The added real-PCM regression starts24 voices across12 parts, with zero free
slots, then replaces part0's note60 with67 through the existing-group path.
It checks unchanged group/head/tail/free-count, completes real PCM startup and
checks both installed partials' new key. The existing subsequent release and
PCM sound checks remain in the same test. This deliberately exercises the
allocation-to-render boundary before MIDI mono routing is enabled; it does
not claim full mono MIDI behavior or hardware waveform equivalence.

Validation: Release build succeeded (`/tmp/sc55-reuse-install-build.log`),
`ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^voice-control-pcm$'
--output-on-failure` PASS26.20s. Targeted diff whitespace check passes.
MIDI CC126/127, held-key returns, portamento controls and program invalidation
still need the part-owner transaction that calls this connected engine entry.
No generated projects, normal-product routing, commit or push changed.

## MIDI mono connection in the native player

NativeMelodicPlayer now owns per-part held-key bitmap, current key, preparation
velocity, committed mono tone and portamento switch/rate. CC126 values0..16
clear poly bit7, clear held keys, reset current key60 and force-stop/reclaim
old groups. CC127 only accepts0, sets poly and reclaims groups while retaining
the bitmap, matching2827/2850 and095D/094B. CC5/65 use both CC and portamento
receive gates (040E0C/0026C6); the live control inputs receive the glide rate.
CC121 disables portamento without clearing held keys; full system reset
replaces the complete per-part mono state.

Mono NoteOn applies the part velocity/key gate once and prepares the selected
tone. A compatible existing group calls startReusedMelodicNote; tone changes
reclaim the old group and defer until task4 is serviced before fresh allocation.
Without portamento the same slots restart withFF; with portamento the established
reuse selector derives the restart flags from voice status. New/restarted notes
use the no-source255 pitch sentinel. Successful preparation commits current key,
velocity and previous per-slot pitch snapshots. Zero-candidate return notes
release the first mono group rather than substituting another tone.

NoteOff now has a fixed pending part mask, visited15..0. Poly/rhythm keep their
existing group selection; mono clears the released key, replaces the current
key with the highest remaining key using the current preparation velocity, or
releases its first group. A replacement can suspend for PCM startup while the
remaining part mask is retained. Later MIDI is not consumed until the mask
drains. No heap allocation or new thread/lock was introduced.

Validation: Release headless build and final `native-player` CTest PASS6.98s,
log `/tmp/sc55-mono-midi-final-build.log` for the build. Added real-PCM cases
cover mono with portamentoOFF/ON,60->67->64->release64 returning67 then60,
velocity-zero NoteOff, no extra voice allocation, final release, program73->80,
invalid CC127value1 and valid poly restoration. A second sequence routes two
mono parts to channel0 and verifies both returning keys and four held voices,
followed by hold-off returning all24 slots. Final MCU PC/cycles remain zero.

This is native-preview functionality, not a switch of the normal product path.
Pending compatibility work includes CC84/custom source controller, polyphonic
portamento/source history, special high-key/partial-count transitions and
independent whole-note H8/native waveform comparison. General GS/services and
normal-product integration also remain. No resave, commit or push performed.

### CC84 source preparation: verified native calculation

Added `PreparePortamentoSourceKey` in `sc55_voice_setup.h` for the two
00:1dd0/1df7 call sites. It composes partial transpose and key tracking, returns
only the integral key and deliberately omits scale tuning. The native player
does not call it yet: connecting the controller requires preserving the shared
preparation reference and polyphonic group-selection behavior below.

Extended the real-MIDI `--mono-pitch` probe (interpreter only, no RAM writes).
Mono and poly sequences each set CC84=48, play60, setCC84=60 and play67. Eight
native per-partial source calculations matched H8. Ten reuse-freeze comparisons
also passed. Log: `/tmp/sc55-source-probe.log`; Release CPU-role build passed.

Observed controller semantics in both modes:

- CC84 stores the source latch (MIDI channel0 maps to firmware part1/A051).
- CC65 OFF preserves it; CC65 ON clears it; CC121 clears it.
- Note preparation consumes it once, including when CC65 is OFF.
- Initial source note used fresh flagsFF; the next source note reused the same
  physical slots with flags5F, even with the portamento switch OFF.
- Poly source60->67 also reused the source60 group. Ordinary fresh-poly
  allocation is therefore not a valid replacement for this path.

Important shared state: 1d9b's source calculation calls1390 before11d8 updates
A1B4. The tracking reference is the preceding preparation's A1B4, not the new
note or CC84 value. The probe observed reference60 for source48 as well as
source60. Writes at11d8/11f2 set a melodic original key;120d sets the rhythm
transposed key. Preserve this cross-part preparation history when connecting
the native controller. The current helper accepts the reference explicitly.

### CC84 connected to native mono note preparation

`NativeMelodicPlayer` now owns each mono part's source latch, accepts the
configured controller (`SystemDefaults.bytes[3]`, legacy default84) with the
CC receive gate, preserves it onCC65OFF and clears it onCC65ON/CC121. A normal
mono note consumes it; a held-key replacement does not. Pending startup or
reclaim retries retain it until preparation completes. Source presence selects
the existing-group reuse flags even when CC65 isOFF.

Each prepared partial now receives `PreparePortamentoSourceKey`'s result in
the DSP pitch input. The shared preparation reference is retained across parts;
successful mono/poly preparation publishes its note, rhythm publishes its
transposed map key. Source conversion reads the preceding reference before
the next note replaces it. This adds no allocation, lock, thread or H8 call.

The source controller is still explicitly unsupported on poly/rhythm parts;
poly's source-group search must be connected next. This is not complete CC84
compatibility or normal-product integration. Special descriptors, partial-count
changes and rejected-preparation history still need their remaining paths.

Validation: Release oracle build and final native-player CTest passed7.19s, including
source48->note60 lowering initial PCM pitch, source60->note67 reusing the group,
release67 returning60, source consumption, CC65OFF preservation, CC65ON/CC121
clearing and unchanged unsupported count on mono. Existing tests also passed.
Build log `/tmp/sc55-source-midi-build.log`. No host/CPU benchmark or resave.

### Polyphonic CC84 group reuse connected

`VoiceAllocator::findSourceGroup` implements0d4e..0dca's reverse part-list
search, matching source key and melodic selector80. It returns the newest
matching group, including released-but-linked entries. Traversal is bounded
at24 and malformed links fail. `prepareGroupReuse` and the existing native
installation/DSP entry now accept this explicit group instead of always
using partHead; preparation validates the physical voices' part/group owners.

The native player's shared mono/source start path now handles poly CC84 too:
no match allocates a fresh group with explicit starting pitch; a match reuses
that group and renames its key to the destination, resetting groupStatus.
Poly operation does not alter mono held keys/current key. A tracked tone
change reclaims the matching group and defers fresh allocation until PCM is
ready. Normal poly starts record each group's tone for this decision.

Validation: Release builds of both tools passed. `--mono-pitch` compared three
source searches against the real interpreter (absent, matching, and a middle
group within a three-note chord), ten source-key calculations and twelve
reuse-freeze transactions: all passed. Log `/tmp/sc55-poly-source-probe.log`.
The native-player CTest passed7.57s:60/64/72 -> CC84=64+note67 stays at6voices;
NoteOff64 leaves6, NoteOff67 leaves4; an absent source starts2more voices;
final releases return24free. Existing mono/source/rhythm/reset tests passed.

Remaining: program invalidation's exact part-wide history (group-tone identity
is not a complete substitute), special descriptors/high notes/partial-count
transitions, general poly CC65 source history without CC84, drum controller
behavior and full waveform/host performance validation. Normal product still
uses H8; native preview is not yet a complete replacement. No commit/push/resave.

### Program-change reuse invalidation history

Replaced per-group tone identity as the restart decision with the part-wide
`reuseInvalidation_` mask (A1CE). Accepted melodic tone changes set the bit;
changing back before a note does not undo it. An identical resolved tone does
not set it. Mono/poly mode changes clear it. Mono preparation consumes it and
reclaims old groups; poly CC84 consumes it only after finding its source group,
reclaiming that group before fresh allocation. Ordinary poly allocation does
not consume it. Reset clears the mask. Existing deferred PCM ordering remains.

Real MIDI trace located the set at00:08da and observed the clear at00:0f60
(normal mono),00:0e84 (portamento mono), and00:095d (mode change). The
80->73->80 sequence sets the bit and note67 starts fresh physical slots;
subsequent80->80 leaves it clear and note69 reuses the current slots. Added
explicit oracle assertions for these states. Log `/tmp/sc55-invalidation.log`.
Native-player CTest passed7.72s including corresponding round-trip/same-program
tests and existing mono/poly CC84 cases. Release build passed; log
`/tmp/sc55-invalidation-native-build.log`.

General poly CC65 history, special note/partial paths, wider GS semantics and
full waveform/host validation are still pending; no normal-product switch.

### CC65 per-partial pitch history connected

The player now retains two pitch-history bytes per part (A190/A191). Fresh
poly CC65ON uses them as glide origins and sets the fresh portamento flagsFF,
without reusing an old note group. Mono CC65ON uses the same history when no
CC84 override is present. Note preparation records each prepared partial's
stored adjusted key, not the incoming MIDI key. Rhythm preparation updates
the shared part history too. Program changes seed used partials from middleC
with part/master/partial transpose, matching08f8..0941 (no tracking/scale in
that initialization). Same-program messages do not reseed it; CC121 preserves
history. Reset restores program-derived history.

Also connected the partial source key as sample-selection minimum for both
CC65 and CC84. ROM123d/12be->1357 uses it before multisample lookup; passing
only a DSP glide origin missed this independent effect. Target pitch/history
still use the unclamped adjusted key; minimum affects lookup only.

Validation: real-interpreter probe verifies poly note48 stores3030, note72
stores4848, and CC121 preserves4848. Trace shows note72's input history3030
with fresh flagsFF. Existing source/search/reuse comparisons passed. Log
`/tmp/sc55-history-probe.log`. Release native-player build and CTest passed
8.12s, including a real-PCM CC65OFF/ON comparison:48->72 retains four voices
in both cases, while ON lowers the second note's initial PCM pitch. Existing
CC84, mono, rhythm, reset tests also passed. Build `/tmp/sc55-history-build.log`.

Remaining includes special descriptors/high notes/partial-count transitions,
history writes on skipped/no-destination partials, wider GS/services, full
H8/native waveform comparison and host performance. Product default remains
H8; these tests do not prove complete replacement or target CPU utilization.

### High-note125..127 mapping connected

Added the melodic0ccd branch before mono/CC84 selection (rhythm still wins).
The original note passes the part receive/velocity/key-range gates, then
`MapHighNote` selects the dedicated tone and pitch from the selected patch's
common bytes8..16. Negative tone means no sound, not ordinary melodic pitch.
The mapped pitch bypasses part/master transpose (11ea), sample mode81 keeps
the preparation key unchanged, and fresh flagsFF/sourceFF bypass portamento.
Mono held/current state and CC84 latch are not consumed by this branch.

Added an explicit optional `MelodicAllocationInputs::groupNote`: high-note
allocation uses the original125..127 key and selector81 from the outset,
while velocity/sample/DSP use the mapped tone/key. Thus allocator bookkeeping
and NoteOff agree without a post-allocation key correction.

Validation: new `--high-notes` real-interpreter probe compares `MapHighNote`
against0ce8 for128capital programs times3keys. All384 matched:33present,
351absent. Observed start uses mapped60, original group125, flagsFF/selector81.
Log `/tmp/sc55-high.log`. Release native-player build and final CTest passed
8.32s: program80's absent125/126/127 start no voices, program24's125 starts its
mapped sound even in mono, preserves mono current60 and CC84=48, and releases
by NoteOff125. Existing note/reset/portamento tests also passed.
Build log `/tmp/sc55-high-native-build.log`. No commit/push/resave.

Still pending: special sample descriptors, partial-count/no-destination
transactions, broader bank/GS/services coverage and full waveform/host
validation. The default product path remains H8, not the preview.

### Same-slot partial installation handoff

`PlanPartialVoiceDispatch` already preserves both ordered preparations when
the second partial falls back to the first physical slot. Sample installation
also executed both writes, but `DispatchNormalVoiceInputs` rejected the result
as a duplicate. It now validates both records, then dispatches only the final
installed partial for that slot. Task2 has one owner/PCM voice, not two DSP
starts; both earlier installation operations are retained.

Added a composed fixture using actual program80's two-partial selection and
an explicitly allocated one-slot group. Both installations are present, final
metadata identifies partial1, exactly one task is dispatched with partial1's
sourceKey72 (not partial0's48), and freeCount remains23. This is a constructed
allocation fixture, not a claim that a standard MIDI sequence reproduced it.

The new `--partial-transitions` interpreter probe ran all128capital programs
with mono/CC65ON, velocity1 note60 -> velocity127 note67 -> releases; none
reached this same-slot preparation. Log `/tmp/sc55-partial.log`. Other banks,
velocity combinations and allocation histories remain to examine.

Release build passed (`/tmp/sc55-overwrite-build.log`); voice-control-pcm24.74s
and native-player7.92s passed. Remaining for this path: publishing the earlier
partial's key history independently of the surviving DSP request, zero-voice
preparations and negative descriptors. Full H8 replacement remains unfinished.

### Partial history separated from DSP survivors

Added `PartialPitchHistoryUpdate`, a fixed-size two-partial write set captured
from installed sample plans. Its prepared mask distinguishes a skipped partial
from a prepared partial whose key remains the initial preparation key (mode81).
It deliberately does not depend on physical slot or final installed metadata.
`MelodicStartResult` now returns this write set independently of DSP requests;
mono/source, ordinary/high-note and rhythm player paths apply it after successful
preparation. The old loop over surviving DSP owners no longer publishes history.

The one-slot/two-partial fixture now uses distinct keys60/72. Both histories
survive despite only partial1 reaching DSP. Additional cases remove partial0's
physical destination (both histories still publish), then skip partial0 and
use unchanged-key mode for partial1 (old history77 retained; initial91 used).
These checks exercise the write set; an all-zero-physical-voice transaction
is still not accepted by the runtime and is not claimed as supported.

Release build passed (`/tmp/sc55-partial-history-build.log`), voice-control-pcm
24.71s and native-player7.92s passed. No host testing, commit, push or resave.
Next remaining transaction boundary: no surviving voice/negative descriptors
and ordering of stop tasks before any remaining partial's DSP preparation.

### Prepared-only transaction completion

Added explicit `MelodicStartResult::Status::preparedOnly`. When sample
preparation produced history but every destination is a negative/unassigned
slot and no task is pending, `beginInstalledNote` returns that status with
zero DSP requests, zero prepared voices and the complete history write set.
It performs no PCM I/O or synthetic key-on and reserves no startup. Empty
sample input, invalid nonnegative destinations and pending tasks still fail.
This currently does NOT accept negative sample IDs with returned physical
slots; those need the separate stop/owner-lifetime handling below.

Native mono/poly/high-note callers accept this distinct completion and consume
the MIDI event/history. Poly source does not rename a group when no voice
survived. The rhythm wrapper also passes this completed transaction through,
allowing its caller to retain history without inventing a DSP request.

Focused fixture: two unassigned sample plans return preparedOnly, preserve
history60/72, leave key mask/startup empty, and perform zero reads/writes.
Changing a destination to invalid slot24 fails with no I/O. Release build
passed (`/tmp/sc55-prepared-only-build.log`); voice-control-pcm24.87s and
native-player7.94s passed. No commit/push/resave or host test.

Next: negative sample113e..114c returns an allocated physical slot through
1cbd rather than synthesizing a special waveform. The player still rejects
that path before allocation; it must be connected with the old DSP owner's
stop state and any remaining partial's task2 ordering. Whole H8 replacement
remains incomplete.

### Negative-sample return handoff connected

The native player no longer rejects a valid negative sample ID during melodic
lookup. Installation already executes113e's return-to-free-list operation.
`beginInstalledNote` now accepts the returned physical destination after
checking its negative descriptor, free allocator status and absence of a
pending task. It preserves any old DSP owner and copies the lifecycle caches
and stopped stages produced by53e6, clears the returned release request and
updates the modulation-stage mirrors. No extra PCM stop or key-on is emitted.
Non-restarted installation simply carries its unchanged lifecycle forward.

If no DSP voice survives, the transaction returns preparedOnly with history;
otherwise the surviving partial proceeds through normal task dispatch. An
earlier installed record targeting the same slot retains its own task2 record
instead of being overwritten by the return handoff. Complex same-slot
negative/positive ordering still needs dedicated end-to-end verification.

Focused fixture runs actual `RestartAndInstallVoice` with negativeFFFF against
an allocated/installed slot and a retained amplitude owner: freeCount returns24,
the native handoff returns preparedOnly, keeps the owner with matching stopped
stages/caches, clears its pending release, and performs no additional I/O or
startup. Release build passed (`/tmp/sc55-returned-sample-build.log`);
voice-control-pcm24.68s and native-player7.92s passed. This is a constructed
negative-descriptor transaction, not a full MIDI waveform equivalence result.
No commit/push/resave; default product still uses H8.

### Mixed return/positive task handoff

Fixed the post-install runtime guard: a negative first partial followed by a
positive installation to the same slot clears allocator free status. Checking
that status before checking the surviving installed record incorrectly failed
the whole start. Return-only destinations still require free status; mixed
destinations proceed to the existing metadata validation and task2 dispatch.
This changes no allocation, PCM installation order, or thread ownership and
adds no allocation or locking to the audio path.

Extended the constructed one-slot fixture with an actual positive
RestartAndInstallVoice after the negative return. The runtime now accepts the
final partial1 metadata and prepares exactly one DSP owner. This is a handoff
regression test, not proof that MIDI reaches this combination, nor proof of
firmware free-list equivalence (the return has already changed that list).
Those conditions and whole-note H8/native output parity remain open.

Release research target built successfully (`/tmp/sc55-mixed-return-build.log`).
Focused CTest voice-control-pcm passed25.55s, native-player passed8.19s.
No host test, commit, push, resave, or default-path switch was performed.

### Independent H8/native audio baseline

**Correction:** the original numbers below compared unequal device durations
because H8 emitted two DAC frames/pass and native one. They do not establish
a native amplitude/envelope defect. See the equal-rate correction below.

Added `--compare-native-audio ASSET ROM_DIRECTORY` to the research executable.
Unlike sc55abcompare (shared H8 register stream, different PCM renderers), this
runs independent H8 and NativeMelodicPlayer MIDI/control paths through separate
real PCM instances. Both load the same ROM waves; native imports system,
melodic/rhythm and effects tables. H8 native instruction shortcuts are disabled;
native MCU cycles remain zero. No state is copied from H8 into the native engine.

Program80, key60, velocity100: settle/attack/held/release/tail windows of16384
PCM frames. H8 boots120M cycles; native warms16384frames. MIDI is submitted at
the same measurement boundaries, but UART latency/onset and PCM phase are not
aligned. This is an explicit diagnostic, not a passing waveform parity test.

Measured centered stereo RMS (H8/native): attack16711259.891/21270664.482,
held25725116.165/16250616.528, release5151604.575/3099214.932,
tail1484826.465/276136.173. H8 silent output has DC16777216 while native is0;
pcm.cpp's output quantizer/configuration accounts for a distinct output-stage
boundary that must not be confused with a synthesis amplitude measurement.
Per-channel mean removal still leaves the above substantial amplitude-envelope
difference. Do not infer a single cause from these aggregate metrics.

Build and diagnostic completed successfully. Log:
`/tmp/sc55-native-audio-comparison.log`. Next: compare this note's PCM control
registers/EG evolution and effects contribution before widening MIDI coverage.
Normal product remains H8; full native waveform compatibility is unproven.

### Equal-rate comparison and native output-rate contract

Boundary diagnostic found H8 config3c=f8 versus native00. PCM_Update emits two
frames only when enable_oversampling AND config.oversampling are true. The
original16384-frame windows therefore advanced H8 by5120000device cycles but
native by10240000. This explained the supposed63% held-level discrepancy.

The diagnostic now disables H8 oversampling and asserts equal device-cycle
duration for each window. Native still uses its existing undithered output;
DC offset is reported separately. Equal-duration centered RMS H8/native:
attack21272182.262/21270664.482, held16211290.025/16250616.528,
release2880630.333/3099214.932, tail280375.264/276136.173.
Held amplitude differs0.243%, not37%. A1% held-amplitude gate fails before
the comparison correction and passes afterward. This gate is not full parity;
release/onset alignment and output quantization remain to compare.

The same investigation found a product integration bug: NativeMelodicPlayer
sets3c=0 but retained enable_oversampling=true, while PCM_GetOutputFrequency
uses the latter to report64kHz. The product assigns sourceSampleRate from this
query after constructing the native player. Native initialization now sets
enable_oversampling=false so the query reports the actual32kHz output. Added
that contract assertion to native-player-test. No H8 default path is changed.

Validation: Release research build passed (`/tmp/sc55-native-rate-build.log`),
native-player CTest passed8.38s, independent comparison passed its equal-device-
time assertion and1% held-level gate (`/tmp/sc55-native-rate-comparison.log`).
Temporary boundary debug prints removed. No host/resampler smoke test performed;
whole-waveform parity and full H8 replacement remain incomplete.

### Measured key-on alignment and dry-output isolation

The independent comparison now observes each PCM's active key mask at actual
sample callbacks. Program80 note60 starts at window frame123 in H8 and frame1
in native (122frames difference). A single offset measured at this boundary is
used for attack, held, release and tail. No gain fitting, time stretching or
per-phase offset optimization is performed. DC is measured during silence and
kept fixed, rather than re-estimated from each sounding window.

Default-effects normalized waveform RMS error after that alignment:
attack0.229087, held0.244661, release0.866060, tail0.296344.
Correlations:0.973647,0.970202,0.778375,0.956048.
This does not pass a waveform-equivalence criterion merely because the
separate held-amplitude gate passes.

Added optional `--dry`: sends identical CC91=0 and CC93=0 to both MIDI paths
before the program-settle window, with no register/state copying. The dry
key-on offset remains122frames. Aligned errors: attack0.222292,
held0.233982, release1.037732, tail1.000000. Thus effects alone do not explain
the main discrepancy. Dry native tail is exactly silent while H8 retains a
small signal (centered RMS25050.163); differing release timing still needs
measurement before attributing it to EG arithmetic.

Build passed (`/tmp/sc55-dry-build.log`); default run completed in0.746s,
dry run0.744s. Logs `/tmp/sc55-onset-comparison.log` and
`/tmp/sc55-dry-comparison.log`. Next boundary: per-voice start phase and
release-command/EG completion timestamps. The new measurements change no
product audio processing and do not establish full H8/native compatibility.

### PCM time accounting and missing IRQ handoff identified

Added sample-callback observations for slots22/23: only key-mask, pitch and
envelope-command transitions are recorded, with address/phase/current levels.
Program80 initial pitches match (slot22=3926, slot23=41f6); release commands
also match (slot22=4fb4,0056,1f4a; slot23=4fb4,0056,1e49).
Before the clock fix CPP periodic changes were uniformly257frames apart,
because step charged the requested final sub-pass duration while PCM_Update
rounded it up to625cycles. Changed step to charge actual PCM cycle delta.
Regression checks1024steps against an independent ControlTaskClock, including
multiple160256-cycle deadlines. native-player passed8.29s; Release build passed.

This correction does NOT improve the current waveform comparison: correcting
the epoch drift moves the first native pitch update fromframe128 to53; dry
aligned held error rises from0.234 to0.712. Periods now alternate256/257 as
required by160256/625, but H8 service latency/epoch remains different. Do not
claim waveform compatibility or tune the clock back to an incorrect period.
Logs: `/tmp/sc55-clock-accounting-build.log`,
`/tmp/sc55-clock-accounting-comparison.log`, pre-fix transitions
`/tmp/sc55-voice-observation.log`.

Higher-priority missing connection: native service never acknowledges/routes
PCM IRQ into the2928 voice handler. ROM0752 reads e03e, masks channel31,
sets cb30[channel]=ff and posts task2.2928 branches on stage>=0e and the
voice's byte[-17]; one branch chooses the lower PCM ramp, writes00b6,
sets stages0e/10 and detaches peers. The other updates sample-position-derived
pitch through29dc and writes PCM10. Existing native lifecycle only clears
fieldCB30; no producer/consumer is connected. Implement this semantic event
handler before claiming whole-note signal-path completion. The periodic
control path is not a substitute for this PCM event.

### PCM IRQ semantic handler connected

Native service acknowledges e03e only outside pending startup, then processes
the reported physical slot. Installed voices now retain descriptor0a bit1 and
the alternate pitch reference. Handler2928 ignores stages>=0e; one-shot ends
choose the quieter PCM ramp, cache00b6, set all stages0e/10 and detach PCM
peers. Loop events switch the active pitch reference, invalidate correction
source A4 while retaining A6, and recompute/write PCM10 without an EG tick.
Runtime lifecycle/modulation stage mirrors are published in the same owner.
No threads, locks, ROM execution or allocations added to the audio path.

Added `--native-pcm-boundary-test ROM_DIRECTORY` and CTest native-pcm-boundary.
It executes original ROM2928 through54ae independently, checking24 constructed
cases across physical channels, stop/loop/already-stopped branches, signed
pitch offsets and both relative PCM level orderings. RAM state, PCM words and
peer links match C++. The separate sample-install fixture covers duplicate
stopped IRQ no-op and verifies the exact pitch writes.

Validation: voice-control-pcm24.03s and native-player7.89s passed. Subsequent
coverage run native-player8.16s observed80 real PCM IRQ events from MIDI and
zero H8 cycles; ROM boundary test0.05s passed24cases. Logs:
`/tmp/sc55-pcm-event-coverage.log`, `/tmp/sc55-boundary-oracle-build.log`.
Program80 dry waveform metrics did not change with this handler; do not
attribute that residual error to the newly connected IRQ path. Full replacement
and waveform compatibility remain unfinished. No resave, commit or push.

### Fresh-note descriptor start corrected at shared preparation

Expanded independent comparison with `--program N [--dry]`. Program0 H8
started PCM slot23 at1c160 but native at21973; program48 H8 started8000 but
native ae74. These were not timing differences: native selected descriptor
base+offset04 even on restarted notes. ROM2bc3 tests installed flag bit7;
shared PrepareNormalVoicesDsp incorrectly used a separate caller boolean.
It now derives unoffset start from installed.flags&128 for all melodic/rhythm
callers, including reuse (bit7clear retains the offset path).

After correction program0 dry aligned error: attack0.002673, held0.007604,
release0.045834 (correlations0.999996,0.999971,0.998950). Previously held error
was1.354732. Program48 improves from1.637218 to0.624614 held error but still
fails the1% amplitude gate; remaining modulation/timing differences are real
open work. Source-address assertions at observed key-on were added to the
comparison so future starts cannot silently pick a different sample region.

voice-control-pcm24.05s and native-player7.82s pass after the shared fix.
Logs `/tmp/sc55-piano-start-fixed.log`, `/tmp/sc55-strings-start-fixed.log`,
build `/tmp/sc55-start-address-build.log`. Next investigation: PCM channel30
random-source initialization, used by native LFO and tuning preparation.

### PCM random source and ordinary poly source ownership

Native construction now seeds channel30 register34/35 withffff, matching
04:2643..264b. Previously the zero LFSR never advanced. Native-player verifies
the seed and advancement after1024 PCM passes. This is a boot operation,
not a GS-reset reseed. Reference/native boot durations differ, so their random
phases differ; do not mistake equal random output for a compatibility contract.
After seeding, program48 dry held waveform error is0.972252 (correlation
0.522533), still failing the amplitude gate. Log `/tmp/sc55-strings-seeded.log`.

Ordinary poly DSP preparation now passes255 for the absent glide-source key,
matching A1CC initialized at0f51 and copied into C974 at1293. CC65 continues
to use partial pitch history; CC84/mono retains its separate path. This change
does not improve the measured program48 waveform error. Fresh-note fine-tuning
is checked at the initial PCM pitch write, in addition to existing live RPN,
CC65 and CC84 coverage. Native-player8.19s and PCM-boundary0.05s pass;
`/tmp/sc55-poly-source-test.log`.

The short15-program dry comparison uses `/tmp/sc55-compare-program-N.log`.
Initial sample addresses match in each tested program. Held waveform error
is0.004528 for16,0.005176 for24,0.712449 for80 and1.393650 for88. This is not
evidence of complete sound compatibility. Program120 has a roughly5.9M AC RMS
attack in both engines but only~28K H8 residual after the one-shot has stopped;
its held relative error1.0 must not be reported as an entirely silent note.

### Model45 receive state connected

DisplayData owns validated10 00 00 text (1..32 bytes, control characters become
spaces) and10 01 00 bitmap (exact64 bytes), following04:7321..73a6. Native MIDI
dispatch now stores these without H8 RAM, allocations or UI calls. Bad length
or address does not change the previous payload. Checksum/framing still gates
all mutations. Separate revisions allow repeated identical commands to restart
a future display controller's timeout. Access is audio-owner only.

This is reception/state only: native LCD rendering, scrolling, expiry and
thread-safe publication remain unconnected. Do not claim display completion.

GM Off now accepts without resetting settings/voices. ROM04:1dc7 clears the
transient fe7e bit1, already cleared by0746 before the serial native reset
releases later MIDI; it is not a persistent GM mode switch. Integration tests
verify Model45 reaches the player and GM Off does not request another reset.
CTest sysex/native-player/native-pcm-boundary passed8.31s in total, log
`/tmp/sc55-display-gm-test.log`. Product emulator translation unit also built
in the CPU-role target (no Projucer resave). Its `--extended-tx` diagnostic
compares native text/bitmap values with actual ROM receive results and passed;
`/tmp/sc55-display-native-oracle.log`. TX-ready IRQ supplementation remains
diagnostic-only as documented in CPU_SERVICES_AND_ORDER, not a product fix.

### GS tone/controller/fine-tune input owners connected

PartSettings now accepts30..37 tone controls with the d7dc table's14..114
limits (cutoff32 has upper80), and1f/20 assigned-controller numbers through
the audio owner's contribution configuration. Multi-record writes follow table
order, including37->40. PartControllerState accepts40 2p and all six11-column
sensitivity groups from d982. Column0 is limited40..88; xA advances to the
next group's x0. Poly-pressure uses its separate sensitivity row; the other
five groups map to existing contribution-source rows. Configuration writes
preserve latched MIDI contributions, as the scalar handler has no recompute
side effect; subsequent MIDI calculates contributions using the new settings.

GS part fine tune17 is a separate stored byte from RPN fine pitch. It requires
exactly two encoded bytes, combines them with byte truncation, clamps8..248,
and is supplied as VoiceControlInputs.correctionSource for527c..5367. Boot/reset
import now retains part+07 (neutral128). This fixes a missing input, not a new
pitch algorithm. Fresh onset and live PCM pitch tests cover both directions.

Master7e owns the configurable portamento-source CC number (default84).
Receive dispatch now reads current master state instead of the immutable boot
defaults. Pan6->7e scalar continuation is supported. Master reset restores
the imported value. Collisions with other special MIDI controllers are not
exhaustively verified by this change.

Independent ROM tests:102 supported-part comparisons (including all8 tone
fields, fine-tune extremes/non-nibble input and six controller rows) pass;
14 master transactions including CC-number writes/continuation pass.
Logs `/tmp/sc55-gs-fine-oracle.log`, `/tmp/sc55-gs-final-master.log`.
Native live test verifies GS bend sensitivity after a new pitch-bend event,
fine-tune updates on an already sounding note and GS fast attack after NRPN
slow attack; passed8.14s before the CC-number extension.
The source128 neutral correction does not change program48 waveform metrics
(`/tmp/sc55-gs-fine-strings.log`); residual timing/random-phase differences and
other missing system paths remain open. No resave, commit/push or default-mode
switch. Native LCD, panel, bulk/RQ1 and remaining GS mode/reset details are
still required before claiming full replacement.

### GS note modes and shared rhythm-map selection connected

Part13 now uses an audio-owner mode operation: a changed poly bit stops/reclaims
the part's groups and resets the mono current-key/reuse latch, using the same
operation as MIDI126/127. Unlike those MIDI commands, an identical GS value
does not stop a sounding note (04:144c..14b8). Part14 updates only the two low
note-assignment bits, clamped0..2. Part15 follows1553..15c6: bank/program and
coarse-tuning latches reset, rhythm flags select map1/map2 or melodic. This
operation does not stop existing physical voices or reload the shared map.

Native now retains the two raw rhythm-map program latches AC10/11, separate
from per-part fallback-resolved program values. Program changes update these;
joining a map inherits its current program and edited key data. Reset clears
the latches. Returning to melodic selects bank0/program0 for future notes.
The configured assignable CC numbers are also used when classifying handled
MIDI, rather than marking non-default assignments19/etc as unsupported after
successfully applying their contribution.

New `--compare-native-modes ASSET ROM_DIRECTORY` uses independently booted
H8/native engines, real MIDI and real PCM. Eighteen sequence checkpoints match
all16 parts' bank/program/mode bits and allocated voice counts: same-value
poly/mono writes while sounding, changed mode, assignment limits, shared-map
program inheritance, map switching and switching away with a drum voice alive.
Registered as CTest native-gs-modes. It is not a waveform-parity assertion.

Native-player additionally proves a muted edited drum key stays muted when
another part joins that map, while the second map remains audible, and accepts
a non-default assigned controller. Tests native-player8.54s and
native-gs-modes0.71s passed (`/tmp/sc55-gs-modes-final-test.log`). No resave,
commit, push, default-native switch or host CPU claim. Bulk/RQ1, native panel/
LCD publication, remaining reset/system behavior and audio compatibility still
prevent claiming full H8 replacement.

### Configurable source-controller dispatch priority (2026-09-10)

The native MIDI dispatcher previously intercepted the configured source CC
before bank/volume/pan/pedals, but after mono/poly mode. H8 disagrees: fixed
controllers0/1/5/7/10/11/64/65/66/67 take priority, then the source CC takes
priority over RPN/NRPN and channel-mode commands. The native order now follows
the measured H8 order. Rhythm parts also store the source latch at00:08b0;
they no longer report it unsupported. Rhythm note preparation remains rhythm
preparation, not melodic portamento.

`--native-source-controller` runs actual MIDI in H8 and NativeMelodicPlayer:
all128 configured numbers × values0/48/127 × melodic/rhythm =768 cases PASS.
It compares source latches and bank/volume/pan/chorus/reverb side effects.
CC0 is compared to the pending bank latch (AB66+part), not committed GS memory.
Before the fix, value48 alone exposed ten melodic dispatch mismatches and
rhythm source writes being dropped. No H8 timing or reference code was changed.
Log `/tmp/sc55-source-controller-test.log`; Release diagnostic build succeeds.
`--native-synth` PASS with checksum3b54320560580fd3 and variable-frame equality.
This does not prove all receive-flag combinations, all mapped modulation side
effects, or complete portamento audio parity. No Xcode/Logic validation here.

### Pending bank and committed tone share an explicit part model (2026-09-10)

PartSettings::Part now distinguishes `bankSelect` (CC0 pending latch) from
`bank` (committed GS tone bank). Removed NativeMelodicPlayer's duplicate
selectedBank_ array. Note admission, normal display, RQ1 and bulk readback now
refer to the same committed bank. Defaults, GS tone assignment and rhythm-mode
changes initialize both fields. CC0 alone changes only bankSelect.

Before this change, CC0=8 without Program Change made native RQ1 report bank8
while H8 reported bank0. Rendering used another array and stayed on the old
tone, so state readback and actual sound selection disagreed.
Program selection now takes an explicit bank: MIDI uses bankSelect, panel and
bulk reapplication use the stored bank. Selection updates both bank fields.
H8 readback and AB67 latch observations confirm bulk and instrument-panel
actions do not accidentally commit a pending CC0 bank.

`--native-parameter-replies` passes423 complete packets, including pending bank,
Program Change, fallback bank127, bulk reapply and a physical instrument-button
press followed by readback. The panel test waits for scanning before issuing
RQ1; it does not assert a reply sent before key scanning already contains the edit.
`--native-bulk-system` passes full1864-byte readback/reapply/reset.
`--native-source-controller` still passes768 cases (pending bank checked against
AB66+part). `--native-synth` passes with unchanged3b54320560580fd3 checksum.
Release diagnostic target builds; no Projucer resave, commit/push or host test.
