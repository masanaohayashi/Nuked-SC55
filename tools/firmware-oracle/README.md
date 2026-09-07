# Firmware replacement oracle

## Native prepared-start ownership

`VoiceControlRuntime::beginPreparedStart/pollPreparedStart` now owns the
prepared batch copy, PCM activation continuation and atomic publication of
both DSP owners. The 36-program paired sweep uses this path instead of
installing owners and continuing their envelopes in test-local glue.
Duplicate slots, concurrent starts, DSP/release publication while pending,
bounded waiting on nonzero PCM levels, cancellation with no replay, and
idempotent completed polling are checked. Real PCM rendering and reclamation
still run for 12 two-partial notes per program, using the MD14 data asset and
waveform ROMs without loading/executing the control ROM.

This is an intermediate serialized startup boundary, not the product's MIDI
scheduler. While a start is pending, the caller advances PCM and polls later;
it must preserve queued events and elapsed DSP ticks rather than dropping
them. Firmware yields during reuse waiting but masks interrupts during the
post-enable wait. Scheduling other voices during the former remains to be
integrated; the current API rejects whole DSP passes in both phases. Failure
can follow device mutation and requires engine recovery, not blind retry.
The product processor still renders through the H8 emulator.

`VoiceControlRuntime::serviceControl` connects the existing device-cycle
`ControlTaskClock` to periodic DSP. It distinguishes idle, deferred, updated
and failed without consuming an event during startup or after failure. One
call executes at most one aggregated pass, including when the firmware byte
counter wraps to zero with its event flag still set. It does not replay 256
individual passes or widen the firmware counter into a different algorithm.
The paired sweep now advances that same clock during PCM startup polling,
services the retained event after each completed start, and uses the scheduled
entry for subsequent DSP. Previously those startup cycles were omitted from
the test clock. Tests check retained counts/phase, wrapped notifications,
no-device-I/O idle/deferred/failed calls, and no duplicate event consumption.
This is not a model of interrupt latency: other voices remain deferred during
reuse waiting, and the product engine scheduler is not yet connected.

## Serialized timed MIDI ingress

`MidiEventQueue<Capacity>` owns decoded events and their absolute PCM-cycle
timestamps. It uses the existing streaming decoder and fixed storage (minimum
two events); there is no allocation or cross-thread synchronization. `push`
returns the exact byte prefix consumed. On full, callers must retain the suffix
and retry it at its original timestamp before sending later input. Decoding is
transactional per byte, including SysEx abort plus restart emitted by one byte.
This is explicit backpressure, not a drop-oldest policy or firmware UART-size
emulation. The eventual host adapter still has to implement the suffix policy.

`VoiceControlRuntime::serviceMidi` dispatches at most one due event, retaining
it during startup. A receiver accepts exactly once (and may start activation),
defers without state mutation, or fails with no automatic replay. Callback
failure also latches the runtime failure. Future events cannot be delivered
early; equal-time events retain FIFO order. A failed engine/queue must be
reconstructed at an engine-reset boundary; it cannot safely discard a partial
GS transaction and carry on.

Tests compare direct and capacity-two decoding under fragmented input,
running status, realtime, SysEx abort/restart and a 64 KiB streamed SysEx.
They cover full-prefix retry, time rejection and non-replay after failure.
The real-PCM paired sweep queues hold on/off during every startup and delivers
them in order afterward; all twelve Note Off messages per program also pass
through this queue and the runtime to actual release/reclamation. Note On
preparation, MIDI-to-part configuration and GS interpretation are still not a
complete native MIDI engine, and the product processor still uses H8.

The paired sweep now also queues one Program Change followed by twelve Note
Ons before starting any voice. A delivered Note On performs real velocity/
sample selection, allocation, installation and task dispatch, then calls
`VoiceControlRuntime::prepareAndBeginNormalStart`. That runtime entry couples
DSP preparation to startup reservation so retries during an active start do
not repeat random reads or mutate modulation peers. Once preparation begins,
failure latches the runtime; duplicate-slot/busy preflight has no I/O.
The returned batch is only a preparation snapshot for inspection/retaining
pitch metadata, not the running voice owner.

The integration asserts twelve deliveries, distinct allocation of all24
voices, no delivery of the next queued Note On during startup, and complete
release/reclamation across all36 paired programs. Invalid preparation is
checked for non-replay. Sample preparation/installation orchestration is
still in the test receiver: this proves the queued Note On handoff but does
not yet provide the complete product MIDI receiver, GS defaults, voice
stealing or mono/reentry scheduling. The independent pending-pedal queue in
this fixture tests startup gating; it is not a multi-source timestamp merger.

`AllocateMelodicNote` now owns tone/velocity selection, transactional group
reservation and ordered partial destination planning for one already-routed
melodic Note On. The caller supplies effective bank/rhythm, part, velocity
accumulator and group metadata explicitly. It derives the required voice
count from the actual candidates, including single-partial dispatch/fallback.
Only an `allocated` result changes the allocator. `needsCapacity` requires the
engine's capacity policy; it is not an accepted/dropped note and must not
become an unbounded retry loop. Mono reuse, rhythm and fan-out remain separate.

The paired waveform receiver uses this owning operation. An additional MD14
matrix compares it with the established selection/createGroup/dispatch
operations for128 programs ×127 velocities ×2 soft-pedal states ×3 free-voice
capacities: 97,536 cases (46,736 single and9,144 paired allocations,41,656
capacity shortages). Full allocator-byte comparison is compile-time guarded
against padding. Zero-velocity Note On, invalid part, rhythm input and corrupt
free head are rejected without allocator mutation. The current capital-bank
matrix does not exercise a valid zero-candidate velocity rejection.

`PrepareAndInstallMelodicSamples` now owns ordered sample selection and the
restart/install handoff for an allocated melodic note. Key-resolution mode
and installation sample mode are separate explicit inputs. Both partials'
plans and installation transactions are validated before any PCM operation;
the queued paired receiver no longer implements those steps itself. It still
owns prior-key caching, task dispatch and construction of the DSP inputs.
The new operation does not wait, key on, or infer tuning/GS defaults.

The receiver rejects an invalid second key without first-partial PCM writes.
A separate256-case check (128 capital programs, restart off/on) compares the
composed operation with direct sample selection plus RestartAndInstallVoice:
complete PCM write sequence/read count, allocator bytes, installation metadata
and lifecycle fields must agree. Reads in this composition check are
deterministic fixtures; the existing paired integration also exercises real
PCM/waveform output. Special sample IDs delegate the existing negative-sample
return path and report no newly installed metadata; that path and sentinel
destinations are not covered by this capital-program composition matrix.
Product host integration and removal of control-ROM loading remain incomplete.

`DispatchNormalVoiceInputs` now composes installed samples/velocity results
with explicit per-partial DSP policy and previous pitch state. It previews
the firmware task selector, verifies that the selected one/two distinct slots
belong to this installed note, and commits task consumption only on a match.
Entries follow dispatcher order, not partial order. Malformed metadata, a
different pending task, non-restarted preparation and an empty owner set leave
lifecycle/activity untouched. These outcomes require the engine to handle
other work/reentry; they are not permission to discard a queued note.

The paired receiver now uses this boundary instead of assembling requests and
swapping entries locally. The128-program installation matrix also checks
single/paired task order, preserved source key/history/previous pitch and
control inputs, and rejection of the non-restarted branch without consuming
its task. Separate paired checks preserve unrelated prepare and finish-stop
tasks and reject mismatched sample metadata. This does not yet supply the
engine-wide pending-task registry or schedule other voices during reuse waits.

`VoiceControlRuntime::startRoutedMelodicNote` now provides the normal restarted
Note On entry, composing allocation, sample installation, task-ordered DSP
inputs, preparation and activation reservation. The engine must supply the
resolved part/bank, sample/key policy and previous DSP state explicitly.
`started` means the event is accepted once and PCM activation is pending, not
that the voice is already audible. `deferred`, `needsCapacity` and
`invalidInput` do not allocate or touch PCM. Pending task2/4 work defers the
new note; a corrupt task code fails instead of causing endless deferral.

After successful allocation, later failure latches the runtime and requires
recovery: it does not undo PCM writes or pretend the note can safely retry.
The result includes preparation snapshots for retained metadata/inspection.
This entry does not yet handle mono/reentry, the no-normal-owner special
sample outcome, routed fan-out, capacity reclamation or prior-key-cache policy.
Those must not be silently mapped to success by a product adapter.

The wave/PCM integration now runs all36 paired programs through both the
stepwise and composed entries without resetting PCM between programs or
paths. Both must sound and reclaim24 voices per program. Composed-entry
checks cover zero-velocity misuse, insufficient free voices, pending stops,
invalid second-partial key, corrupt task codes and non-replay after failure.
This verifies the normal Note On execution seam, not a control-ROM-free
product: the plug-in still uses the H8 emulator.

`VoiceControlRuntime::serviceStopTask` now consumes at most one task4 and
publishes its lifecycle to the periodic DSP owner. It follows the existing
descending dispatcher: a higher preparation task returns `needsPreparation`
without clearing it or skipping to a lower stop. Startup defers this service.
Missing owners/corrupt dispatch fail with the task unconsumed and latch the
runtime; repairing a pointer alone does not silently replay the operation.
This entry has no PCM I/O and does not return a voice merely because the
task was consumed. Periodic stop-level polling still determines reclamation.

Tests cover both18->14 and20->16 branches on all24 slots, one-task-per-call
priority, higher preparation work, missing-owner non-replay and startup
deferral. In the composed entry's first paired program, all24 sounding voices
also receive actual StopPreparedVoice writes and queued task4 transitions
after Note Off. Free count must remain zero immediately after task service,
then the ordinary PCM/control cadence must return every voice. Subsequent
programs continue reusing that PCM and allocator without resetting them.
The scheduler still needs ownership of lifecycle snapshots/pending preparation
records and the capacity-policy path; this is not yet the product engine.

`NativeVoiceEngine` now owns the native runtime, note allocator/state,
installation records, physical lifecycle snapshots, key masks and control
clock. It has no implicit boot policy and does not own the host/PCM or GS
router. The composed Note On entry, startup polling, task4 service and periodic
control now use those same owned objects in the paired waveform fixture.
The fixture retains its explicit program-boundary initialization while the
engine key mask and clock persist across both36-program entry sweeps.

After successful periodic control, updated DSP lifecycle fields are published
back to engine state. `requestStop` takes its snapshot from the current DSP
owner, performs the physical stop and queues task4; repeating the request
before task consumption has no I/O. Control service retains its clock event
while any preparation/stop task is pending, so stale DSP cannot overwrite a
queued stop. This is a serialized ownership boundary, not recovered firmware
interrupt latency or scheduling of unaffected voices during reuse waits.

Tests check lifecycle agreement during real waveform rendering, deliberately
stale progress/PCM cache snapshots at stop time, duplicate-stop deferral and
retention of an elapsed control event. Product processor integration, MIDI
receiver policy, complete pending preparation management and control-ROM-free
boot remain incomplete.

`NativeVoiceEngine::receiveReleaseMidi` now owns the Note Off/hold/sostenuto
transaction: route into a staged PartNoteState, validate/publish the complete
DSP release snapshot, then commit the note state. During activation it defers
before changing pedal or group state. Missing DSP owners reject publication
without committing either side; an event-queue receiver must report that
failure rather than accepting/dropping the event. Unsupported messages/mono
branches report invalid input, not a guessed fallback. This method performs
no PCM access and introduces no routing or GS defaults. Matched-result bits
identify parts with matching note groups, not group indices.

The paired waveform fixture uses this engine boundary for held pedal events
and all queued Note Off messages. Additional checks cover missing-owner
rollback, velocity-zero Note On equivalence, unsupported CC rejection and
hold/sostenuto ON -> Note Off -> OFF against the established receiver plus
release-publication operations. This completes that receive seam, not the
full MIDI engine: Note On fan-out, GS/config handling and host integration
still remain, and the product still loads/executes control ROM.

`NoteOnFanout` retains one admitted Note On and its common receive-part mask.
It visits15..0 one part per call, matching20f2/20fe..2126. A visitor accepts
once, defers without state mutation, or fails without replaying earlier parts.
Routing is snapshotted at admission; configuration changes must be serialized
until completion. Rhythm/key-range and other per-part gates after2126 remain
visitor responsibilities; this is not a complete GS routing implementation.

The already-routed melodic allocation path now applies `AcceptNoteKeyRange`
before tone/sample/voice allocation. This translates2150..215e: original MIDI
key compared inclusively with raw part+0c/+0d, unless the part's bit in the
BYTE atAC0E is set. Parts8..15 therefore do not bypass, and the adjacentAC0F
byte must not be read as a16-bit bypass mask. Inverted bounds are not repaired.
`MelodicAllocationInputs` carries the bounds and rawAC0E value; its default
full range is an explicit compatibility default, not a recovered GS boot state.
Out-of-range notes return `keyRangeRejected` through `VoiceControlRuntime`
without allocating, preparing PCM, or latching failure; a fan-out visitor
should consume that part and continue. Common fan-out itself does not apply
this gate to unsupported rhythm paths or Note Off. Rhythm tone allocation and
GS configuration producers remain to be integrated.

The H8 differential probe executes2150 up to2160/2191 for393,216 cases,
covering all part positions, byte bypass masks, raw bound values, boundary
keys and inverted ranges (147,756 accepted). Allocation tests additionally
check endpoint inclusion and bypass/rollback for all16 parts; the paired
PCM startup fixture checks rejection with zero PCM I/O and unchanged voice
allocation. These are receive-gate checks, not evidence of product ROM-free
boot or complete MIDI semantics.

`AdjustPartNoteVelocity` translates21be..2207 for nonzero MIDI velocities
and7-bit part+0a/+0b configuration. Depth0 substitutes1 and depth64 skips
scaling. Otherwise the multiply consumes the big-endian WORD containing
both depth and offset, divides by64, and retains the quotient's high byte.
The offset therefore also participates in fractional scaling. Negative
offset adjustments clamp to1..127; nonnegative adjustments clamp only the
upper limit and can return0. At214c that zero discards the Note On rather
than generating Note Off. The native allocation path performs this adjustment
before the key-range gate and passes the effective velocity into patch
selection and sample/voice installation. Parameters above127 reject as invalid;
they are not silently masked into a valid GS value. Defaults64/64 preserve
the existing fixture behavior; GS boot/config import remains caller-owned.

The differential oracle executes the complete routine through its register
restore for all127*128*128 =2,080,768 input combinations, including254 zero
results. It verifies R1/R2/R4 and stack preservation as well as output velocity.
Native allocation checks compare nonneutral settings with explicit effective
velocity events. In the36-program paired startup waveform sweep, the combined
entry now receives velocity98 with depth64/offset65 and must install velocity100
in both voices. Separate no-I/O checks cover adjusted-zero rejection before
an inverted key range and invalid adjustment settings. MIDI input velocity0
continues to belong exclusively to the release receiver.

`SelectPartNoteOn` now composes the entire part-specific receive branch
2126..2160, including21be. After common channel/mode routing, an original
velocity0 selects release before any rhythm-key enable, adjustment or key-range
check. With rhythm bit4 in part+5, bit5 selects the first of two per-key flag
maps (8b48+key versus8fd4+key); bit4 in that selected byte enables Note On.
Other bits do not grant permission. Nonzero events then undergo velocity
adjustment, adjusted-zero rejection and key-range rejection in firmware order.
`PartNoteOnInputs` carries the already-indexed raw bytes from both maps,
avoiding H8 memory pointers or invented drum presets in the native core.
The caller still owns per-key configuration and common receive routing.

The melodic allocator uses this composition, replacing its separate adjustment
and range checks. It still rejects rhythm allocation: a `prepare` result from
this receive selector is not proof that drum tone/sample construction exists.
`release` identifies a branch, not a completed release transaction; rejected
parts must not prevent fan-out from visiting other eligible parts.

The H8 probe executes2126 through2160/218e/2191, including real subroutine
call/return, for524,288 cases:131,072 release,98,304 rhythm rejection,
24,576 adjusted-zero rejection,115,392 range rejection and154,944 prepare.
Output velocity on preparation and R1/R2/R4/stack preservation are checked.
Native tests cover all256 part flags and all four combinations of the two
rhythm enable bits, including release despite disabled keys or irrelevant
invalid adjustment fields. Existing waveform tests exercise the composed
melodic path. Product boot, drum allocation and host integration remain open.

`sc55_rhythm.h` translates the subsequent0c3c..0c9f/0ccc tone mapping,
before patch velocity, group release and allocation. `RhythmKeyMap` owns128
tone words plus raw grouping/flag bytes. The firmware has two mutable maps
at8748 and8bd4: tone words at+000, grouping at+200, flags at+400. The caller
selects the map; defaults are unassigned tones, not a GS reset or preset.
The original key is retained. Part program127 takes the initial velocity
accumulator from a128-byte table at03:d168; other programs preserve the
incoming accumulator (normally cleared by0bc1 note entry).

`RhythmToneSelection` preserves the raw tone, key, flags, group and accumulator
even for high-bit/absent tones. Present tone IDs0..223 select bank1 directly;
IDs224..32767 select bank2 with index=tone-224. This is address arithmetic,
NOT proof that every resulting index exists in the sound-data set. Consumers
must validate imported patch bounds; bank()/bankTone() require present().
There is no silent fallback from absent or unavailable drum patches.

The differential test executes the ROM prefix for all65,536 tone words,
both maps and program0/127 (262,144 cases, half absent), checking the
configuration/key side effects and bank/index selection. It uses the local
03:d168 data table, without modifying control ROM. Native tests cover all
128 keys/programs, bank/sentinel boundaries and neighboring-key isolation.
The existing MD14 asset still holds only224 bank1 patches. Bank2 patches,
drum preset/config producers, this mapping's runtime connection, exclusive
group handling and drum DSP installation remain unfinished; this helper is
not yet an end-to-end native drum instrument.

### Bank2 data-only patch extraction

The second patch bank contains162 name-first216-byte records at ROM2 file
offset20000..288af (CPU02:0000..88af), followed by zero-filled bytes. It
contains supplemental melodic tones as well as drums, not162 drum instruments.
The v1.21 program-to-rhythm-set table03:8000 references sets0..8 and13;
their128-entry tone maps at03:8080+set*048c reach global tone385=224+161.
Every present tone in these maps fits the two224/162-record banks. This
checks the upper boundary against actual data consumers, not just printable
names. H8 execution at0fbe..0fd3 additionally verifies the bank-relative
216-byte stride, common flags and first-partial address for all162 records.

Extract only the second bank (local copyrighted data, not distributed):

```sh
python3 tools/sc55patches.py /path/to/sc55_rom2.bin --bank 2 --export /tmp/sc55-v121-bank2.patches
/tmp/sc55-firmware-oracle-build/sc55-patch-layout-test /tmp/sc55-v121-bank2.patches
```

The raw output is exactly34,992 bytes, accepted by
`SC55PatchTable::loadRecords(bytes,0,162)`; it includes names/common/partials,
not instructions, waveform ROM or drum configuration maps. The standalone
patch-layout test links no MCU or ROM loader and verifies every common/partial
byte against this extracted file, then destroys the source buffer to check
ownership. Extraction requires the known v1.21 SHA-256 for either bank,
refuses to overwrite output, and never modifies the source ROM. Listing bank1
retains the earlier heuristic fallback for unknown revisions; exporting from
that heuristic is deliberately rejected.

This does not yet extend MD14 or enable native drum playback. The supplemental
patches refer to137 distinct sample groups (IDs7..243), so the melodic-only
sample export must also be expanded and checked before those patches can be
installed. Rhythm preset/config extraction and bank-aware runtime patch lookup
are still required; silently resolving bank2 IDs against bank1 would be wrong.

### Supplemental sample closure

`--export-all-sample-bank` now collects the union of groups used by both
224/162 patch banks, then every nonnegative descriptor selected by those
groups for keys0..127. It exports the existing SB01 format, not a new layout:
244 groups and992 descriptors,32,996 bytes. Negative sample IDs remain
sentinels, never descriptor indices. The import requires the hashed v1.21
ROM set and validates encoded/decoded ownership before output.

```sh
/tmp/sc55-firmware-oracle-build/sc55-firmware-oracle /path/to/rom-directory /tmp/sc55-v121-all-samples.sbank --export-all-sample-bank
/tmp/sc55-firmware-oracle-build/sc55-sample-zone-test /tmp/sc55-v121-all-samples.sbank /tmp/sc55-v121-bank2.patches
```

Before writing this export, the tool executes H8 lookup14f0..1523 for all
31,232 group/key pairs and descriptor address selection through1557 for
nonnegative IDs, comparing descriptor bytes at the actual firmware-selected
addresses. This checks both bank transitions and offset wrapping. The native
sample-zone executable, which links no emulator or ROM loader, then verifies
all24,064 supplemental partial/key lookups using only the two extracted files.

The older `--export-sample-bank` remains melodic-only (161 groups/758 samples,
23,638 bytes). All919 old records are byte-identical in the enlarged bank;
83 groups and234 descriptors are added. MD14 serialization and its existing
waveform fixtures are unchanged. These files now provide the additional patch
and sample definitions, but bank-aware SoundData/runtime integration, drum
configuration producers and end-to-end drum DSP remain unfinished. Waveform
ROM bytes are still separately required; SB01 contains definitions, not audio.

### Unified MD15 sound data

The plug-in now automatically generates this asset when the verified v1.21
ROM pair is first prepared. No manual export or placement is required.
`src/backend/sc55_sound_data_import.h` extracts data only (no H8 execution);
`Plugins/Source/NativeSoundDataCache.h` stores it under the existing user/app-group
settings directory at
`NativeSoundData/mk1-v1.21-md15/sc55-native.sdata`.
Both ROM hashes are checked before importing. The deterministic MD15 output
digest validates cache hits; missing, corrupt and older files are regenerated
through a temporary file. Other ROM versions retain the existing emulated path.
Import failures are reported by ROM initialisation. No generation or file access
is added to `processBlock`.

`sound-data-cache-test.cpp` is a focused JUCE-core test accepting a ROM directory
and a not-yet-existing cache directory. It verifies first import, the exact
184480-byte MD15 digest, reuse without rewriting, corrupted/stale cache repair,
unsupported-ROM rejection and write failure reporting. This test and the macOS
arm64 Debug shared-code build passed. This change prepares the asset only:
product playback still runs through the existing H8 emulator.

`SoundData` MD15 now includes the supplemental162 patch records immediately
after MD14's pitch-timing curves and before the SB01 sample payload. MD01..14
remain readable with their original layout and224-tone behavior. The getter
`patch(id)` uses global IDs:0..223 in bank1,224..385 in bank2;386 and negative
sentinel IDs return null. `patchCount()` reports0/224/386. Additional storage
is allocated only during MD15 loading, outside the synthesis callback.
Every used supplemental partial must reference an imported sample group;
invalid names, missing groups or truncation reject the whole import while
preserving the previous generation and its pointers.

```sh
/tmp/sc55-firmware-oracle-build/sc55-firmware-oracle /path/to/rom-directory /tmp/sc55-v121-all-15.sdata --export-all-sound-data
/tmp/sc55-firmware-oracle-build/sc55-sound-data-test /tmp/sc55-v121-all-15.sdata
```

The v1.21 export is184,480 bytes,386 patches/244 groups/992 descriptors and
the existing owned DSP tables. Tests now enumerate `patchCount()` rather than
stopping at224:62,388 successful sample-to-pitch preparations and normal pitch
starts,124,928 completed composed envelopes, and98,816 velocity plans across
both banks. Unsupported/absent sample preparations are excluded from those
successful-preparation counts; this is not proof that all drum modes work.
The waveform fixture also runs with MD15 to check that bank1's existing
audio path remains valid; it does not yet play bank2 through drum MIDI routing.
Drum configuration, bank-aware Note On expansion/allocation and product native
boot remain outstanding. MD15 is a data integration step, not ROM-free product
completion or an audio-equivalence claim for drums.

### Mapped rhythm velocity expansion

`PrepareRhythmNoteVelocity` now connects `SelectRhythmTone` to the shared
`PrepareMappedNoteVelocity` using MD15 global patch IDs. `PreparedNoteVelocity`
uses a16-bit tone field (the existing `MelodicNoteVelocity` name remains an
alias), so bank2 IDs above255 are not truncated. The melodic entry reuses the
same expansion after its preset selection. Rhythm input is already received,
part-adjusted and map-selected; this step does not apply those gates twice.

The result retains the rhythm key flags/group and initial accumulator, plus
the selected patch's ordered partial velocity plan. A negative mapped tone
returns a valid absent result; an unavailable nonnegative tone rejects as
invalid, not an alternate instrument. Zero candidate count is velocity
rejection. Original velocity-zero MIDI remains a release event, not an input
to this expansion. Later exclusive-group handling and allocation remain open.

`--export-all-sound-data` now additionally executes the actual0c3c mapping
and0fbe velocity subroutine through0ca2 for all386 imported tones,127 positive
velocities, soft-pedal off/on and program0/127. All196,088 cases and247,396
accepted partial outputs match, including flags/counts, the shared accumulator,
per-partial amplitude/secondary values and balanced stack. Native data-only
tests exercise the same public expansion with both banks and explicitly test
absent versus missing tone data. The MD15 bytes remain unchanged.
This reaches patch-velocity preparation, not PCM startup or complete drum
MIDI rendering; configuration ownership, group choke behavior, allocation and
product native boot still need integration.

### Rhythm exclusive-group forced stop

`StopRhythmExclusiveGroups` translates0ca5..0cb3 and16a4..16cc. Selector0
bypasses traversal; otherwise every group of the selected part with matching
A2D0 is stopped through `StopAndReclaimGroup`. This is not ordinary Note Off:
note, group release status and hold flags do not protect matching groups.
Reclaimed slots append to the free list immediately, while PCM stop ramps and
task4 remain pending. Allocation must still respect the existing reuse wait.

The full H8 oracle covers256 exclusive-stop cases (16 parts, one/two voices
per group, eight seeds), including interleaved matches/nonmatches, another
part, selectors0/60/61/62 and varied hold/status values. Together with512
existing append/prepend stops, all768 cases match allocator bytes, all24
lifecycle records and PCM RAM. The full13,314,400-frame trace remains
byte-identical to the prior bank2 baseline. Native tests also corrupt a later
group tail, successor or cycle and require no I/O or state mutation, despite
an earlier valid matching group; selector0 and invalid parts are checked.

`NativeVoiceEngine::requestRhythmChoke` connects the helper to owned allocator
and lifecycle state. It preflights links and affected DSP owners before PCM I/O,
defers while startup or an affected lifecycle task is pending, and uses current
DSP lifecycle values rather than stale installation snapshots. Task4 delivery
publishes the stopped state through the existing dispatcher. The integration
test clones a running24-voice engine and checks missing-owner rejection,
pending-task deferral, selector0, stale-progress replacement, part reclamation
and task consumption. These new owner-boundary checks use counted mock PCM I/O;
the separate H8 comparison above checks real PCM stop behavior.

Complete native rhythm admission is still open. Its continuation must consume
the accepted choke exactly once before capacity/startup waits; this engine
operation alone does not retain or start the incoming drum note.

### Repeated-key retirement before capacity

The rhythm caller at0cb3 enters17b8, not capacity1737 directly. Its prefix
17b8..185c is now `RetireRepeatedNote`: when part+5 bit7 is enabled, mode0
(low two bits) stops the first same-key group and prepends its slots, without
requiring the same exclusive selector. Mode1 skips keys in the16-byte retained
row (first negative byte terminates it), then scans same-key/same-selector
groups. It sets A288 bit2 on each match and stops/appends the first match
whose bit was already set. Modes2/3 and disabled bit7 do nothing. This is
separate from both exclusive choke and ordinary Note Off.

The H8/real-PCM matrix adds1,024 cases across16 parts, one/two-voice groups,
all four modes enabled/disabled, marked/unmarked matches and retained-key
sentinel variants. All allocator bytes, lifecycle words/flags and PCM RAM
match (1,792 total group-stop cases including the earlier768). Native corrupt
tail/successor/cycle checks additionally verify that tentative marks do not
leak and no PCM I/O occurs on rejected traversal. Admission still must wire
this step between exclusive choke and capacity/allocation exactly once.

### Owned rhythm group admission

`RhythmGroupAdmission` now composes0ca5..0cb6: exclusive choke, repeated-key
retirement, capacity preparation and group creation, in that order. It owns
the already-expanded positive-candidate group request, part flags, retained
keys and capacity policy. The force-oldest policy bit comes from the same
part+5 byte used for repeated-key retirement. Each instance processes one note
exactly once and retains its terminal result and allocated slot order.

Capacity rejection is terminal, not a PCM wait: H8 returns nonzero at0cb6
and abandons the incoming note while preserving earlier stop side effects.
An internal failure is likewise non-replayable. Only input validation before
processing promises no side effects. A later sample-install/startup owner
must retain the admission result across physical reuse waits.

The full H8 path (including calls to1737 and1bab) matches192 saturated-voice
cases:184 allocated and8 capacity-rejected. Allocator bytes, selected group,
ordered slots and real PCM RAM match. Native tests separately cover allocated,
capacity-rejected, invalid-input and corrupt-state terminal outcomes and verify
that repeated calls change neither allocator/lifecycle nor I/O counts. The
admission fixture supplies prepared inputs; this is not yet connected to the
engine's rhythm tone expansion, DSP ownership or complete drum PCM startup.

`RhythmNoteAdmission` bridges `PrepareRhythmNoteVelocity` to that group
admission and the existing `MelodicAllocationResult` sample-install handoff
(a historical name, not another melodic preset lookup). It preserves global
16-bit tone IDs, uses the mapped key/group/flags for the group request, and
derives ordered partial destinations from the selected patch and allocated
slots. Absent tones and zero candidates bypass admission; missing or
inconsistent prepared data is invalid. Outcomes and the handoff are retained
across repeated calls, with no second stop or allocation. SoundData must stay
in the same immutable generation until downstream startup is finished.

The MD15 data-only test now connects all196,088 prepared rhythm variants to
this entry, checking group metadata, free count, tone identity, dispatch
order and replay suppression. It loads only the exported data asset and does
not link an MCU. These fixtures start with free voices and forbid PCM I/O;
they complement, rather than replace, the saturated H8 admission matrix.
Missing-data, absent-tone, mismatched-tone and invalid-part cases also reject
before allocation. Sample key/mode configuration and consuming this handoff
through actual drum DSP/PCM startup remain to be integrated.

### Rhythm initial pitch keys

`RhythmKeyMap::pitches` owns the128 pitch bytes at map+180, separately from
tone IDs, exclusive groups and flags. `PrepareRhythmInitialKeys` translates
11fe..1215: transpose the selected pitch byte by part+6 and the signed AB46
offset, then use the same result for both initialKey(A1B3) and sourceKey(A1B4).
Do not apply master transpose here; do not substitute the incoming MIDI key
for sourceKey. Default map storage is not a GS-reset image.

131,072 H8 comparisons execute the actual11fe entry through1215 over both
maps, all128 incoming keys and16 parts, eight pitch/shift/offset patterns and
four master settings. R0, both destination bytes, unchanged A1B2 and balanced
stack match. Native checks cover a mapped pitch distinct from the incoming
key, signed offset, saturation and an invalid key. This establishes the input
to shared partial pitch preparation, not full drum sample installation.
The later1334 step can replace A1B2 with the raw mapped pitch; subsequent
partial scale-tuning inputs must follow those ordered state changes rather
than reuse the first partial's original-note input blindly.

`PrepareRhythmSampleInputs` now creates the two ordered sample-install inputs
from an admitted note, selected map, part transpose and scale. Both partials
start at the initial mapped/transposed key; sourceKey stays fixed. A prepared
first partial changes the second's originalNote to the raw mapped pitch,
except selectors80/81, while a skipped first partial makes no such change.
Fresh rhythm minimum-key slots areff, as written at0cb9. The shared installer
now accepts high-bit minimum-key sentinels rather than treating them as invalid
MIDI keys. Sample modes and preparation flags still require caller policy.

The all-data export oracle now runs11fe through the actual partial preparation
path with negative destination slots (no voice installation), checking the
inputs at132b in order.487 partial entries across both patch banks match with
non-neutral scale tuning. The MD15 data-only sweep connects admission, this
input builder, sample selection and installation for all386 tones at velocity100
with synthetic PCM I/O; every tone succeeds, preserving global IDs and the
updated original note in installed metadata. This is not waveform fidelity or
full engine startup. The parser's separate zero-filled synthetic fixture has
unsupported tracking0 and is required to reject all224 plans before I/O;
the imported asset test instead requires zero rejected plans.

### First native bank2 drum PCM startup

`VoiceControlRuntime::beginInstalledNote` is the shared post-install entry for
melodic and rhythm notes: dispatch the installed normal samples, prepare DSP,
reserve asynchronous PCM activation and publish owners on completion. The
existing melodic entry delegates to it. Busy startup preserves task flags and
returns deferred; other preparation failures latch rather than replaying
earlier device writes. Admission/installation still belong to the caller.

The MD15 voice/PCM fixture maps note46 to global tone385 (Open Hi Hat2), with
explicit pitch60, group1, flags90 and neutral research part/control settings.
It runs rhythm preparation, admission, ordered sample-input generation,
installation, the shared DSP/start entry, PCM polling and512 scheduled control
passes. PCM is freshly initialized and only the three wave ROM files are
loaded, excluding earlier voices/effect tails and control ROM execution.
The check requires active selected-slot levels, emitted frames, nonconstant
wave output with the real waves, and zero MCU PC/cycles. With no wave data,
the same control/PCM path must output zero. Busy repeated startup is checked
for no I/O or lifecycle mutation.

This proves one explicitly configured drum's native PCM path, not complete
drum-kit behavior or sample-accurate agreement with H8. Product boot, GS map
defaults, complete rhythm control derivation, shared-engine choke/reuse and
broader drum audio coverage remain open. The default CTest asset is MD14;
run the MD15 fixture explicitly to include this additional bank2 case.

`NativeVoiceEngine::beginNoteOn/serviceNoteOn/serviceMidi` integrates that
continuation with activation. Later MIDI stays queued while fan-out remains;
the next part waits for PCM activation to complete. Fan-out failure also
blocks the engine's processing entries. Capacity shortage still requires an
explicit capacity policy rather than accepting/dropping or endlessly retrying.
The old directly-routed entry remains available for component tests.

All65,536 common receive masks are tested for descending order, per-part
deferral, exact-once acceptance and completion; failure/zero velocity are
checked separately. A real-PCM/waveform scenario reuses the running engine
after both program sweeps: one Note On is delivered to12 parts on channel0,
allocates24 voices, holds a following Note Off, then releases/reclaims every
voice with no control-ROM execution. This proves the fan-out continuation
for that explicit melodic configuration, not product boot or complete GS
receive behavior.

Fan-out failure is now checked at the engine boundary after the first part
has completed actual PCM activation. A copied engine fails part10 after
part11 was accepted. Its runtime alone is still healthy, so this explicitly
tests the engine's fan-out failure gate rather than a pre-existing DSP error.
Every public processing entry must refuse subsequent work: no visitor/MIDI
callbacks, PCM reads/writes, allocator or key-mask changes, queue consumption,
or control-event consumption. The original engine continues the remaining
parts and completes the24-voice wave/release test. This is failure containment,
not recovery or a guarantee that already-started PCM voices become silent;
the future host adapter still needs an explicit failed-engine reset policy.

## Partial-to-voice dispatch

`PlanPartialVoiceDispatch` models1219..132a's gates and physical slot choice.
An enabled candidate whose multisample group is notffff undergoes sample
preparation even when no physical voice is present. The second partial falls
back to A3D6 when A3D7 is negative. Both partials may target the same slot in
sequence; callers must not deduplicate them. Invalid nonnegative slots are
rejected only on an otherwise prepared partial. Skipped partials have no target.

2048 H8 dispatch cases cover candidate bits, missing groups, both slot
sentinels and distinct/shared slots. Delegated132b/1357 preparation and113a
installation are deliberately stubbed in this check: it proves dispatch, not
their side effects, and uses restart bit7 clear. ROM-free checks cover the
second-to-first fallback, prepare-without-install and invalid/unused slots.
The native wave fixture now selects its isolated partial's channel through
this plan rather than directly choosing allocated.voices[0]. It still renders
partials separately; full sequential two-partial installation and mono restart
remain incomplete. Previous-key writes and preparation routines are not
implemented by this plan.

## Existing mono voice selection

`VoiceAllocator::prepareMonoReuse` translates0b1a..0b64 only, the existing
group branch after the A1CE part-bit test. It clears A1B1bits7/5, setsbit6,
selects the first group's tail into A3D6 and a distinct head into A3D7, and
setsbit7 if either selected valid voice has status94. If head equals tail,
the prior second-slot byte is preserved. Negative head values are copied but
not dereferenced; malformed native indices are rejected. Allocator state and
PCM remain unchanged: this is an input plan for the11d0 preparation stage.

4096 H8 cases compare slots/flags and all unchanged allocator fields, including
all256 tail statuses, head status permutations, equal/missing heads and prior
flag bits/second-slot values. Native tests explicitly distinguish active versus
status94 voices, preserved second slots and invalid links. The byte compare
at0b36/0b57 uses the emulator's immediate16 encoding with byte operand size;
the linear disassembly is misaligned there, so it is not treated as valid
instruction-boundary evidence. A1CE allocation/steal path and the0b71 variant
remain separate, as do11d0 preparation,1d55 publication and audible restart.

## Native mono replacement velocity preparation

`PrepareMonoReplacementVelocity` connects the replacement decision to the
existing whole-patch calculation: it requires a replacement action, accepts
the explicitly supplied current preparation velocity, and starts the shared
partial accumulator at0 as0a74 does. It does not perform another MIDI preset
lookup or retrieve the replacement key's original velocity. No candidates is
a valid rejected plan, requiring the caller's mono release path; unsupported
actions/invalid inputs return no plan. Soft-pedal input remains explicit.

`--mono-probes` now imports patch/velocity data off-thread and calculates the
native plan at the observed0fbe entry. At0a8a it compares candidate flags/count,
shared accumulator and both accepted partials' amplitude/secondary outputs.
The actual60/40 ->64/80 ->67/110 ->release sequence matches H8. Its soft pedal
is explicitly off; this is not multi-part preparation-velocity ownership proof.
256 ROM-free velocity/soft combinations and rejected/invalid plans are tested.
`/tmp/sc55-native-mono-velocity-1.csv` matches the prior live probe trace.
The native plan still needs to feed the existing-voice replacement/commit
path: this is not yet native audible mono retrigger or production integration.

## Live mono preparation probe

`--mono-probes` extends the pedal probe with actual CC126 mono mode, program0,
note60/velocity40,64/80,67/110, then releases64,67,60 and restores CC127.
It compares the native held-key/release decision at the live0a64 entry and
the three H8 branch boundaries, checking every held-key word. This observes
all three branches once, rather than relying only on synthesized CPU state.

`/tmp/sc55-live-mono-preparation-1.log` records the replacement60 entering0fbe
with A3D3=110, A080[part]=110 and accumulator A1B7=0. At0a8a, accumulator is10
and velocity remains110. The original note60 arrived with velocity40: its
original per-key velocity must NOT be substituted for the current preparation
velocity.0fbe is the already-translated whole-patch velocity calculation;
the mono preparation caller resets its accumulator before calling it. The
probe currently observes those inputs/outputs, not a native end-to-end mono
voice restart. Frame count582400, writes88829, nonzero287673 at32000Hz are
H8 output, not ROM-free audio fidelity or CPU measurements.

## Monophonic release decision

`MonoHeldKeys::release` translates0a64..0a74: clear the incoming key even if
it is not the current A070 key; in that case leave the voice unchanged.
Releasing the current key chooses the highest remaining key for replacement,
or requests group release if none remain. It does not update the caller's
current key before replacement preparation succeeds. Duplicate releases are
not silently discarded: current-key comparison still determines the branch.
4096 H8 fixtures compare the actual branch boundary, replacement key, every
bitmap word and unchanged A070 across all16parts/all128keys. All three branch
outcomes are required. Native tests cover a67/64/60 sequence and composition
of the final release decision with `releaseMonoGroup` for a two-voice group.
This is a decision result for the mono dispatcher, not completed audible
replacement:0a74..0aab and the note-on/current-key owner remain to be connected.

## Monophonic group release

`VoiceAllocator::releaseMonoGroup` translates0aab..0aec, reached when mono
note-off has no replacement key or replacement preparation fails. It acts on
the first group of the part without matching its key/selector/old status. With
a valid tail it sets group status2; Hold defers through A288bit0 and exits.
Otherwise it requests tail and distinct head release (A360=1/A3E0=ff). Unlike
ordinary release, it uses group head rather than the tail's previous link and
does not inspect the sostenuto row. A negative tail skips status/hold handling
but still releases a distinct valid head. Invalid native indices roll back.

2048 H8 fixtures cover all16parts, one/two voices, prior status/deferred bits,
Hold, missing tail and equal head/tail, comparing all allocator fields.
ROM-free tests distinguish head from previous and check deferred/invalid
behavior. This primitive is not yet the full0a64 dispatcher: current-key and
highest-held-key decisions plus replacement preparation must be connected
before enabling arbitrary monophonic MIDI playback.

## Monophonic held-key bitmap

`MonoHeldKeys` translates0984..099f (set),09a0..09bb (clear) and09bc..09f6
(highest set key). The original9f40+part*16 row is eight big-endian16-bit
words, one bit per MIDI key. The native owner stores words directly; no ROM
or byte-addressed H8 memory is needed. Repeated presses are idempotent and
one release clears the bit. Highest-key priority is not last-note priority.
Empty maps return no key instead of the firmware'sffff sentinel. Out-of-range
keys are rejected without mutation. Each native part now reserves this state,
but MIDI mutation/retrigger is not yet connected: unsupported mono release
still rolls back instead of pretending to implement0a64.

All16parts/all128keys exercise set/set/clear/clear against H8 (8192 updates),
plus10240 highest-key comparisons over those transitions, singleton and mixed
maps. ROM-free tests cover priority, repeated input, ascending/all-held and
descending releases, empty state and invalid keys. Remaining work includes
current-key comparison at0a67, replacement preparation and existing-voice
retrigger/stop; the bitmap alone does not make monophonic playback complete.

## Note receive gate

`AcceptNotePart` mirrors the common note-off2076..209c and note-on20fe..2126
receive gate. Channel equality is always required. In ordinary mode, CDF5bit0
blocks reception and part receive word bit9 enables it. With CDCCbit3 set,
CDCCbit1 accepts every channel-matching part; otherwise CDF4 selects one part,
bypassing the ordinary receive gates. Raw mode field names are deliberate:
their UI/GS producers are not yet mapped. All inputs are owned configuration,
not runtime ROM/RAM addresses.32768 fixtures compare BOTH original H8 gates;
ROM-free tests add explicit mode precedence, channel and range checks.
The native wave fixture now uses the gate before note preparation/release.

`SelectNoteRelease` now translates the09f7 consumer's0a19..0a5d decision:
part+5 bit4 selects the rhythm branch (selector0), keys125..127 use81 in the
non-rhythm branch, and bit7 uses80; the remaining branch identifies0a64
monophonic key-stack/retrigger handling explicitly.32768 key/flags pairs are
checked against H8 up to the group-release or mono entry, including selector
and unchanged key. This does not execute or implement the mono branch.

`PartNoteState::receiveNoteOff` composes receive gates and selector selection
in descending part order, returning the mask of parts with matching groups.
Unsupported mono or corrupt state rolls back the whole event; it never uses
selector0 as a fallback. Both8n and9n/velocity0 are accepted. ROM-free checks
cover simultaneous rhythm/poly receivers, differing group selectors for the
same key, high-key precedence over mono and late unsupported-path rollback.
The real-wave fixture now uses configured poly mode and group selector80
instead of bypassing selector matching with0. These remain explicit fixture
settings, not a completed note-on configuration producer. Full note-on
allocation ownership, mono retrigger and instrument integration remain pending.

## Native part note-state owner

`PartNoteState` now owns the allocator plus16 retained-key rows and sostenuto
bits. Its routed `applyPedal` and `noteOff` select the same part-owned row;
callers no longer construct a fresh row per voice. Construction initializes
rows to ff and bits to false; allocator initialization is still explicit at
instrument preparation. There is no runtime reset shortcut that discards
pending releases. Routing, receive permissions and batch publication remain
external, and the allocator is exposed for existing group/start/stop work.

The native real-PCM/wave consumer uses one owner across its24 partial runs,
replacing per-run pedal state. A separate ROM-free state test keeps16 same-key
groups alive on16parts together, overlaps CC64/66 on odd parts, disables
receive before release, and checks every part after each release for isolation.
This exercises concurrent note-management state, not concurrent audio: the
wave fixture still resets PCM between its isolated partial runs. Full native
instrument scheduling, GS routing and production integration remain pending.

`receivePedal` now resolves CC64/66 fan-out using16 explicit `PartMidiReceive`
configuration records (receive channel and flags). It mirrors268a/2702:
descending parts15..0, channel equality, common bit11 plus bit5 forCC64 or
bit7 forCC66. Multiple matching parts receive the same event; no match is
consumed without mutation. It uses bounded stack storage and rolls back the
whole fan-out if a selected part has corrupt allocator links. Configuration
is supplied by the instrument; GS SysEx configuration loading is still absent.
The real-wave fixture now calls this entry with explicit one-to-one fixture
routing instead of giving `applyPedal` a destination part per message.

Extended `--hold-probes` temporarily remaps the emulator's16 part receive
records, testing same-channel fan-out, absent/common/controller permission
bits, disabled channel255 and no matching channel:24 MIDI messages match all
native hold/sostenuto part states against H8. Original configuration is restored
after the probe. ROM-free tests additionally validate populated retained rows
and rollback after higher-index parts have already been processed. This covers
pedal routing only, not note-on/program/GS routing or firmware queue latency.

## Retained-key capture (04:05f1..063e)

`VoiceAllocator::captureRetainedKeys` now produces the caller-owned 16-byte
retained-key row previously supplied only by fixtures. It traverses the part's
groups, considers status0 only, preserves existing keys, and inserts at the
first high-bit sentinel without adding duplicates. Matching/inserting slot15
ends traversal; scanning a full nonmatching row advances to the next group.
It does not clear the row first. Corrupt links are bounded and roll back the
row. The matching initialization fills rows with ff at04:05e4..05ea.

The oracle compares 2048 cases against executing the original bank4 routine,
including empty through24-group parts, duplicate keys, released groups,
prepopulated and full rows. It also checks allocator fields remain unchanged.
ROM-free tests cover duplicate suppression and corrupt-cycle rollback.
The routed CC66 adapter is now connected (see below); GS routing remains
the caller's responsibility.
`releaseRetainedKeys` implements the corresponding release/clear routine
04:0640..06b5. Status2 groups whose key is retained receive A360=1/A3E0=ff
on the tail and optional preceding voice, except when A288bit0 defers release
for hold. Other fields are preserved and the row is finally filled with ff.
Crucially, exhausting all16 entries without a match exits the entire traversal
at0679; encountering an earlier sentinel only skips that group. This behavior
is retained rather than substituting an ordinary set-membership predicate.
Invalid links roll back requests and row clearing. Another2048 H8 fixtures
compare every mapped allocator byte and the cleared row, spanning all16parts,
row lengths0..16, both voice counts, held and nonheld groups and status gates.
ROM-free checks exercise capture -> note release -> retained release with
hold overlap, both voices of a group, full-row early exit and rollback after
an earlier valid group's request. All10 CTests pass. MIDI receiver/routing
integration beyond the routed adapter and production ROM-free operation remain incomplete.

`ApplyRoutedSostenutoController` connects fragmented decoded CC66 to capture
and release. ROM228b/228d selects2702;2715 compares64 and queues commands8/10,
which dispatch through087d/0882. There is no edge-only gate: another high
message captures newly pressed keys as well. The caller supplies resolved
part/receive permission (part word+2 bits11 and7), owns the AB00-equivalent
enabled bit and retained row, and publishes release requests at batch end.
Disabled receive consumes without mutation; invalid input is rejected.
4096 native part/value/permission cases also test repeated capture after a
new group. Extended `--hold-probes` sends64 actual CC66 messages on all16
channels, including63/64 and repeated high after another note, comparing rows
and all16 AB00 bits against H8. Default routing is a fixture, not runtime policy.
The real-PCM/native-wave fixture routes CC66 for6 of24 isolated partial runs:
capture at tick240, note-off252, release256, verifying retained requests reach
the voice lifecycle with no H8 execution.12 other runs still exercise CC64.
This does not establish concurrent polyphony or whole-instrument audio fidelity.

`sc55_voice_control.h` composes a serialized physical-voice control step:
PCM synchronization/release -> second modulation -> amplitude -> second
envelope -> pitch -> TVA/pan/sends, matching the dispatch order at 3196..3362.
`VoiceControlState` holds the native owners rather than H8 RAM snapshots.
First modulation is updated separately and supplied by the instrument owner.
Successful output is passed to UpdateVoicePcm; stopped results still require
the outer stop/allocator machinery. Callbacks must not reenter or replace slots.
This composition deliberately has no internal MIDI/PCM clock advancement.
The MD13-only consumer executes 64 continuous updates with synthetic prepared
sustain states and PCM callbacks, then checks finished-entry preservation and
early termination when amplitude finishes before later envelope/output work.
Missing data and invalid channels are rejected before I/O. Late invalid
prepared inputs can leave earlier stages advanced; this is not a transaction.
The separate consumer and eight CTests pass. The composed path has NOT yet
been paired end-to-end against H8 or rendered audio; note preparation, actual
PCM timing, live inputs and production integration remain necessary.

MD13 appends the 129-byte PanTable (v1.21 6c8f..6d0f) after MD12 modulation
preparation and before the sample bank. Export/import compares every byte;
the asset at `/tmp/sc55-v121-melodic-13.sdata` is 131,938 bytes. Older formats
remain readable with a null pan getter, without synthesizing a substitute.
Round-trip, truncation, unknown-version, dependency and downgrade tests pass
with all eight CTests. The separate sound-data consumer loads only this asset
and performs 57,344 native modulation/TVA/pan/send updates, checking one-step
pan motion and lookup encoding against the owned table. Volume/controller and
PCM seed inputs remain synthetic; this is not audible playback or host CPU
measurement. MD12 still passes its existing consumer checks without pan.
Full regression trace `/tmp/sc55-pan-data-13.csv` equals
`/tmp/sc55-native-spatial-1.csv` byte for byte.

`SpatialState` implements 3734..37fb: sequential centered-pan adjustments,
master-pan zero bypass, one-step position movement, and left/right lookup from
the 129-byte PanTable (6c8f..6d0f). Pan ffff preserves both pan fields while
effect sends continue their one-step movement. `VoiceOutputState` composes TVA
and spatial processing over the whole 36db..37fb routine with stopped/invalid
results and rollback on invalid pan input. Pan controls accept 0..127; current
pan accepts 0..128 or ffff. The output PCM adapter now takes PCM12/14 from
this owner, avoiding stale preparation-cache copies. 16,384 H8 cases compare
TVA, pan, encoded pan and sends together, including tone scaling, master bypass,
locked pan and stopped stages. ROM-free checks cover one-step motion, locked
pan with moving sends, invalid-input rollback and PCM12/14 mapping.
Eight CTests and the existing MD12 consumer pass. PanTable is caller-provided
data and is now included in MD13; focused boundary tests still use a synthetic
table. This implements output control, not the reverb/chorus
audio algorithms, production scheduling or complete ROM-free audible playback.
Full trace `/tmp/sc55-native-spatial-1.csv` equals `/tmp/sc55-native-tva-1.csv`
byte for byte.

`TvaState` owns the output-level ramp (+06), PCM32 readback/last level (+18)
and PCM16 command (+1a). `advance` composes ComputeLevel and TvaWord for
36db..3734, increments the ramp by 2000 hex per invocation (not per elapsed
tick), saturates at ffff and reports the 3363 stop branch for first stage >12.
The BeginVoiceUpdate overload synchronizes directly into this owner; the
UpdateVoicePcm overload takes its current command without maintaining another
persistent cached16 copy. 16,384 H8 cases compare ramp, level, command and stop
branch, chained after modulation-to-level input mapping. ROM-free tests cover
eight-step saturation, holds, stopped preservation, readback baseline and PCM16
write mapping despite a stale legacy cache. The MD12-only consumer performs
57,344 TVA updates from native modulation (synthetic volume/PCM input).
Eight CTests pass. Pan/effect processing after 3734, whole-voice scheduling,
initial-state production and ROM-free audible playback remain incomplete.
Full trace `/tmp/sc55-native-tva-1.csv` is byte-identical to
`/tmp/sc55-modulation-amplitude-4.csv`.

The three-consumer `ApplyVoiceModulationOutputs` overload now maps output[0]
and wave.output into LevelInputs (30ff..3115), alongside pitch and second
envelope. It preserves expression, velocity, master/tone scale, bias and live
controller depths. 16,384 H8 comparisons run the whole 309b level calculation
with two independently varied blocks, both tone-scale branches and silence.
This exposed old sc55_level.h arithmetic differences: subtraction must clamp
to zero after EACH modulation, addition wraps as a word, same-sign negative
magnitudes add as a word, and mixed signs bypass the 7f00 depth cap. The minimal
zero-minus-one and signed-boundary fixtures failed before correction; native
and H8 checks now agree. The instruction-counted mcu_native.h shortcut is a
separate implementation and was not changed here.
The MD12-only consumer now feeds modulation into all three calculations over
57,344 updates (24,835 amplitude changes with synthetic volume/PCM inputs).
Eight CTests pass. This validates control-value calculations, not PCM audio,
host CPU load or a complete control-ROM-free instrument.
Full regression trace `/tmp/sc55-modulation-amplitude-4.csv` equals
`/tmp/sc55-first-modulation-periodic-1.csv` byte for byte.

`FirstVoiceModulationUpdate` replaces the 3985 periodic entry using the same
24 first-block states as initialization. Its shared transfer now delegates to
`UpdatePairedFirstModulation`, the3d44 tail also required by the periodic pair
scheduler. This helper has no identity/stage/source-link eligibility gate:
the scheduler has already selected the pair. It retains destination timing,
waveform selector, adjusted rate, source link and base rate, copies dynamic
wave/ramp/rate-modifier/sharing state, and derives output using destination
depths. 4096 direct H8 tail executions verify the copy/recompute phase even
when the preceding ordinary sharing task detached. Native tests cover local
configuration preservation, full-attack depths, delayed zero output, self-copy
and invalid depth control. All10 CTests pass. The ordered multi-voice scheduler
and complete product integration still remain unfinished.
The full regression `/tmp/sc55-paired-first-lfo-1.csv` matches
`/tmp/sc55-selected-controllers-1.csv` byte for byte (923053 PCM writes).

`FirstVoiceModulationUpdate` operates on the same
24 first-block states as initialization. A valid shared source copies dynamic
wave/ramp state and recomputes destination depths, but preserves local timing,
waveform selector, raw rate and source link (the firmware enters 3d44, not
3d1a). An invalidated source detaches and redirects every other matching link
to the current voice, regardless of those voices' sharing flags. Only detached
updates check for a stage change across the 39e1..39f5 interrupt window;
direct local updates apply current rate/depth controllers and advance normally.
`begin`/`resume` represent that window without a lock or allocation. The owner
must keep the array and slot lifetimes stable; the stage check is not a
generation guarantee. 4,096 H8 cases compare all block fields and 24 links,
covering identity mismatches, completed sources, local/shared paths and injected
stage changes. These are individually seeded cases, not long MIDI playback.
ROM-free tests cover preservation, redirection, pending/repeated calls and the
different stage-gate behavior. Production scheduling and audible ROM-free
integration remain incomplete.
Eight CTests and the MD12-only consumer pass. Full trace
`/tmp/sc55-first-modulation-periodic-1.csv` equals
`/tmp/sc55-second-modulation-gated-1.csv` byte for byte.

`PrepareVoiceSecondModulation` connects the complete 2a66..2bac entry to
`VoiceStopState::flagMinus3B`, the same flag already consumed by activation's
branch (not cleared here). With bit 7 clear it preserves all modulation state,
source links and cached partial byte 8. Otherwise it captures that byte in
`VoiceModulation::fieldA2` and performs shared/local initialization. The 1,128
shared H8 comparisons now start at 2a66, including source search, and the 4,096
paired PCM local cases also start there with all 128 enabled flag combinations.
ROM-free checks cover all 128 disabled flags, no PCM I/O on skipped calls,
fieldA2 preservation/update and the unconsumed activation flag. Eight CTests
pass. Caller-owned identity/stage inputs and outer production scheduling still
need integration; this is not ROM-free audible playback.
The MD12-only consumer passes; full trace `/tmp/sc55-second-modulation-gated-1.csv`
equals `/tmp/sc55-second-modulation-selection-1.csv` byte for byte.

`InitializeSecondVoiceModulation` connects second-block source selection to
shared/local setup (2a7b..2bac), reusing the same 24 voice states and source
indices as periodic `RouteVoiceModulation`. Selection compares partial bank,
identity and field9b, scans 23..0, excludes self and requires stage <=12.
Shared initialization preserves destination depths but copies outputs, adjusted
rate, modifier, timing and oscillator state; it points directly to the chosen
voice and sets sharing=255. It does not reuse first-block depth recomputation.
4,032 H8 selection comparisons include 1,128 shared-transfer comparisons of
every block field and link/flag. ROM-free composed checks cover shared no-I/O,
local seeding/link reset and invalid-channel no-mutation. Eight CTests pass.
The earlier -3b bit-7 gate and partial[8] to voice+a2 assignment are now handled
by PrepareVoiceSecondModulation. Production scheduling and audible
control-ROM-free synthesis remain incomplete.
Full regression trace `/tmp/sc55-second-modulation-selection-1.csv` is
byte-identical to `/tmp/sc55-first-modulation-selection-1.csv`.

`SelectFirstModulationSource` implements 38a5..38df: mode bit 4 enables
the descending 23..0 search, excluding the destination. Eligible voices have
first stage <=12 and equal field9b, common bank and common identity. 4,032 H8
cases cover every destination/candidate pair with disabled sharing, stage 13,
each mismatched identity field, self exclusion and multiple matching sources.
`InitializeFirstVoiceModulation` composes this with the native local/shared
initializers and their metadata reset, using one stable array of 24 voices.
ROM-free checks verify highest-source priority, no PCM access on shared paths,
the local seed read and metadata reset, and invalid-source rollback. The caller
still supplies live common-record/controller inputs and owns voice lifetimes;
the composed entry has unit coverage, not a full H8 end-to-end comparison yet.
Production scheduling and the second block's initialization selection remain
unconnected. This is not a control-ROM-free audible instrument.
Eight CTests pass. Full regression trace `/tmp/sc55-first-modulation-selection-1.csv`
matches `/tmp/sc55-first-modulation-shared-initialize-1.csv` byte for byte.

`InitializeSharedFirstModulationBlock` replaces the block arithmetic at
3d32..3e2d. It copies waveform/ramp state and rate modifier, but preserves the
destination's adjusted rate and amplitude/second-envelope depths. Before delay
completion all three outputs are zero and pitch depth stays untouched. After
delay completion, pitch depth is recalculated with the destination controller;
at full attack only the first two outputs copy exact depths, while pitch still
uses the high product word. No PCM access or allocation is needed.
32,768 H8 comparisons cover every pitch parameter/controller pair with varied
source state and signed depths, plus ROM-free checks of delay gating, full
attack asymmetry, negative depth, self-copy and invalid-input preservation.
`InitializeSharedFirstModulation` adds the source-link/raw-rate/sharing
metadata transfer, covering the whole 3d1a..3e2d routine in the paired cases.
Native links are voice indices (24 means no source), not firmware RAM pointers;
invalid indices/controllers are rejected before mutation. Source selection and
lifetime validation remain the caller's responsibility, and are not yet wired
to the production scheduler.
All eight CTests and the MD12-only consumer pass. Full regression trace
`/tmp/sc55-first-modulation-shared-initialize-1.csv` equals
`/tmp/sc55-second-modulation-initialize-2.csv` byte for byte.
Production still executes H8; passing this check is not audible native playback.

`InitializeSecondModulation` implements the local, unshared 2b26..2bac path:
partial bytes 04..07 select waveform/phase, rate, delay and attack; unlike the
first block, no live controller offset is applied. Prepared depths and the rate
modifier survive. Every waveform seeds from PCM channel 30 before its initial
one-tick update. 4,096 H8/PCM comparisons cover every parameter byte, followed
by independently varied combinations, checking all depths/outputs, oscillator
and ramp state, PCM latch/channel and global-clock/stack preservation.
ROM-free unit checks cover ordered reads and the disabled-delay/raw-rate limits.
The MD12-only consumer also initializes 448 actual patch/partial second-block
configurations, repeated across the first-block variants with synthetic PCM.
All eight CTests and the MD12-only consumer pass. The complete regression
trace `/tmp/sc55-second-modulation-initialize-2.csv` is byte-identical to
`/tmp/sc55-modulation-depth-prepare-1.csv` (13,314,400 frames, 923,053 writes).
Sharing selection, shared initialization and production scheduling are still
not replaced; this is not ROM-free audible playback.

MD12 adds modulation preparation tables after MD11's rate table: 256 BE16
timing entries (7112), 128 BE16 second-envelope depth entries (7212) and 128
BE16 pitch-depth entries (7312), 1,024 bytes total before the sample bank.
The depth definitions now live in sc55_modulation_tables.h, shared by the runtime
and data importer. Older assets expose a null preparation pointer; malformed
imports preserve the prior valid asset. Round-trip, truncation, dependency and
downgrade checks pass with all eight CTests. Exported v1.21 MD12 is 131,809 bytes
at `/tmp/sc55-v121-melodic-12.sdata`, with exact table import equality checked.
The independent sound-data test loads only that asset and exercises 114,688
first-block initializations (224 patches x 2 partials x 256 mode variants), using
actual patch depths but synthetic mode/rate/timing-controller and PCM inputs.
No MCU or code-ROM execution occurs in this test. This does not establish full
patch initialization or audible equivalence; source selection, shared-block
initialization and whole-instrument scheduling remain incomplete.

`PrepareModulationDepths` converts partial bytes 48/49 (amplitude depth),
2a/2b (second-envelope depth table at 7212), and 0f (second-block pitch depth
table at 7312) into the two native blocks, matching 3800..3890. Byte 0e is
returned unchanged for first-block controller adjustment; that block's pitch
depth is not overwritten prematurely. All parameters use sign/magnitude,
including negative zero. 65,536 H8 cases compare every depth and the preserved
phase/counter fields. ROM-free tests check sign/magnitude, independent state
preservation and the later controller handoff. Eight CTests pass; full trace
`/tmp/sc55-modulation-depth-prepare-1.csv` equals first-modulation-initialize-1.
Depth/timing tables still require SoundData export; initialization sharing,
shared-block setup and production scheduling remain incomplete.

`InitializeFirstModulation` composes local preparation, unconditional PCM30
random seeding, controller rate/depth mapping and one-tick update (38e2..3984,
unshared path). All controller inputs validate before any mutation or I/O.
The caller's prepared depths[0..1]/rate modifier survive, while pitch depth is
recomputed. The native routine never changes the global control clock. 4,096
H8/PCM cases compare outputs, phase/counters, seed/smoothing, timing rates,
PCM latch/channel and H8 clock/stack restoration. Eight CTests pass, including
ROM-free ordered initial reads on a non-random waveform and invalid-input
no-I/O/no-mutation. Full trace `/tmp/sc55-first-modulation-initialize-1.csv`
equals first-modulation-controls-1. Source-sharing initialization and preparation
of the supplied depth/timing data remain outside this local routine; production
still runs H8 and this is not a ROM-free audible-instrument test.

`PrepareFirstModulationControls` implements 39f6..3a71: base-rate/controller
byte arithmetic and signed-magnitude pitch-depth/controller lookup (128 words
at 7312). Only rateIndex and depth[2] change; phase, ramps, other depths and
rate modifier survive. Controller values outside 0..127 reject before mutation.
32,768 H8 cases cover every raw pitch-depth byte and depth-control value, while
also covering each base-rate byte against all rate-controller values. ROM-free
tests verify negative-depth reversal, preservation, invalid-input rejection and
handoff to the block updater. Eight CTests pass; full trace
`/tmp/sc55-first-modulation-controls-1.csv` equals first-modulation-prepare-1.
The depth table still requires data export; complete initialization, MIDI
controller production and native whole-instrument scheduling are not complete.

`PrepareFirstModulation` implements local first-block setup at 38e2..3950,
after the source-sharing decision: mode-to-waveform mapping, initial phase,
counter/output reset and delay/attack rate selection. It preserves prepared
depths, rate index/modifier and held random sample. The generated mapping of
the low mode nibble matches the 7493 table. Delay control is validated as
7-bit, high-bit delay parameters disable its increment, and the full-byte
attack parameter indexes the caller-owned 256-word timing table (7112..7311).
That timing table is not yet exported in SoundData. 65,536 H8 cases cover every
mode/delay byte with varying attack/control values; ROM-free tests cover
preservation, saturation and invalid-control rollback. All eight CTests pass,
and `/tmp/sc55-first-modulation-prepare-1.csv` equals the modulation-consumers-1
baseline. Source selection, initial random read, live rate/depth preparation
and complete first-block initialization remain to be connected.

`ApplyVoiceModulationOutputs` connects the two native blocks to pitch and
second-envelope inputs: output[2] supplies pitch, output[1] the second envelope,
and wave.output feeds both. It preserves additional controller depths, offsets,
tuning and base/controller inputs. The existing 16,384-case H8 pitch modulation
and 16,384-case H8 second-envelope output comparisons now include this mapping,
with unrelated block outputs deliberately different. ROM-free unit tests check
all mapped fields and preservation of independent inputs. MD11 data-only tests
also consume the generated modulation in both output calculations for 57,344
updates (one block active, synthetic PCM reads). Eight CTests pass; full trace
`/tmp/sc55-native-modulation-consumers-1.csv` equals the modulation-task-1
baseline. Modulation setup, controller-depth production, amplitude integration
and the outer whole-instrument scheduler remain incomplete.

`VoiceModulationUpdate` connects sharing to local modulation updates through
an explicit begin/resume task. Only detached sharing captures/rechecks the raw
first-envelope stage (3b11..3b25); direct local updates have no such gate.
Shared results are terminal and do not advance the copied oscillator again.
Repeated begin while pending and repeated resume are rejected. No wait, heap
allocation or hidden callback is used for the interrupt window. The outer
owner must keep the same voice array and prevent slot reuse; the firmware stage
comparison is not a generation check. Existing 3,456 sharing H8 cases now
also compare local continuation or simulated stage-change cancellation with
the H8 path, using waveform 3. Seven-waveform block tests remain separate.
Eight CTests pass. Full trace `/tmp/sc55-native-modulation-task-1.csv` equals
the modulation-sharing-1 baseline. This is still not a whole-voice scheduler
or ROM-free production instrument.

`RouteVoiceModulation` implements second-block sharing (3a7a..3b11), using
24 fixed native voice slots and a 24=no-source index instead of RAM pointers.
Valid matching sources copy the three current outputs, rate modifier, waveform
state and delay/attack counters, preserving local depths, rates and waveform
selection. Invalidated sources clear the current link/sharing flag and reparent
all other links to that old source onto the current voice. Invalid indices are
rejected without mutation. The later interrupt/lifetime recheck is not included.
3,456 H8 cases cover all destination/source slot pairs and six sharing/mismatch
conditions, including self-reference. ROM-free tests additionally check unrelated
links remain unchanged and invalid-index behavior. Eight CTests pass; full trace
`/tmp/sc55-native-modulation-sharing-1.csv` equals the modulation-block-1
baseline. This helper is not yet wired into a native whole-voice scheduler.

MD11 adds 256 big-endian modulation-rate words (512 bytes, source v1.21
ROM1 7012..7211) after MD10's second-envelope preparation tables and before
the sample bank. The exporter verifies exact round-trip equality. MD01..10
remain readable and expose no modulation-rate table; missing data is not
silently replaced with neutral values. Truncation preserves the prior asset,
and encoding rates without their preceding MD10 fields is rejected.

`/tmp/sc55-v121-melodic-11.sdata` exports 130,785 data-only bytes. The separate
sound-data executable loads it without MCU execution or either control ROM
and runs 57,344 modulation updates (256 selectors x 7 waveforms x 32 ticks).
PCM read values in this data-only test are synthetic; it tests data consumption
and depth-ramp completion, not audible fidelity. All eight CTests pass.
The earlier 32,768 paired H8/PCM block checks validate the update algorithm.
Native scheduling, modulation sharing/setup and full instrument integration
remain incomplete. ROM-derived assets are not stored in this repository.

`ModulationBlock` connects all three signed depth ramps, delay/attack counters,
rate adjustment and waveform generation (3b2c..3d19). Oscillator phase advances
even during depth delay. Reaching attack ffff still uses high-word scaling on
that call; the following call copies the exact depths. A caller-owned 256-word
rate table is used without code-ROM access. It is not yet exported in SoundData.
Validation: 32,768 persistent H8/PCM updates (1,024 initial states x 32 ticks),
all seven waveforms, all byte rate selectors, signed modifiers and variable PCM
source values. ROM-free boundary tests and all eight CTests pass. Full trace
`/tmp/sc55-native-modulation-block-1.csv` equals the native-lfo-waveforms-1
baseline. The voice-level routing/sharing at 3a7a..3b26, setup, data export and
outer scheduling still need connection; production continues to execute H8.

`LfoWaveformState` implements 3bee..3d19 with persistent phase, held sample,
smoothed sample and output. All seven waveforms are paired against H8 in
28,672 cases, including arbitrary 32-bit increments and variable PCM channel-30
readback (not the old observed ffff constant). Random sampling selects 30,
reads 34 to latch, then reads 3a/3b, only when the doubled increment plus phase
has a nonzero high word. Both smooth-random entries use 0x50: 3cd0 falls
through the immediate overwrite at 3cd3. The generated sine table's 129 used
entries match ROM; the new runner uses a caller-owned table prepared off-thread.
ROM-free tests check invalid selector/no I/O, conditional ordered reads and
signed smoothing endpoints. Eight CTests pass. Full trace
`/tmp/sc55-native-lfo-waveforms-1.csv` equals `sc55-voice-update-entry-1.csv`.
This is the waveform portion only: modulation preparation, ramp dispatch and
the native voice scheduler still need connection. Production remains H8-driven.

`BeginVoiceUpdate` now connects the native PCM synchronization and release
owners in firmware order (3196..32f0/3363). Finished voices retain pending
release requests; delayed cancellation returns stopped; active release uses
the synchronized amplitude. The 768 H8/PCM synchronization cases now run
through the no-request update entry, including pending-request preservation
for finished voices. Separate ROM-free tests cover active readback-before-release,
delayed start/cancel and invalid-channel no-I/O behavior; the existing 96 H8
release cases continue to test release itself. The combined active-release
entry is not yet paired with H8. Eight CTests pass, and
`/tmp/sc55-voice-update-entry-1.csv` equals the activation-handoff-1 baseline.
The subsequent modulation/envelope dispatcher and outer scheduler remain
unconnected; this is not yet a ROM-free production instrument.

Goal: replace the H8 firmware with C/C++ so that runtime does not load either
H8 code ROM. This tool is the reference side of that comparison, not the native
implementation. It still requires the original complete ROM set.

Build without JUCE, SDL, or changes to the production build:

```sh
cmake -S tools/firmware-oracle -B /tmp/sc55-firmware-oracle-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/sc55-firmware-oracle-build -j 4
/tmp/sc55-firmware-oracle-build/sc55-firmware-oracle '/path/to/SC-55 v1.21' /tmp/reference.csv
```

The output path must not already exist. The fixed scenario records boot, idle,
program 16, note 60 at velocity 100, expression 64, and note-off on MIDI channel 1.
Every CPU write to PCM is recorded, including unchanged values. The same native
PCM implementation and default simulation settings as the emulator are used.
Environment flags `SC55_SIM`, `SC55_SCALAR`, `SC55_FXSIM`, and the home-directory
`.sc55_no_sim` switch affect results; keep them identical across comparisons.

The CSV includes phase, source frame, emulated cycle, PC at the device access,
register, and byte value. **PC is not necessarily the instruction start.**
Startup frames include PCM output before firmware configures its registers;
do not infer boot wall-clock duration from the nominal sample rate alone.
No LCD backend is installed, so this is not a GUI integration test.

## Verified on 2026-09-07

- SC-55 v1.21 (hash registry identifies `mk1-v1.21`), default PCM simulation.
- 307,200 source frames, 4,972 PCM writes, 111,596 nonzero stereo frames.
- Two fresh invocations produced byte-identical CSVs (`cmp` exit 0).
- Writes by scenario: boot 731, note-on 2,801, expression 1,364, note-off 76.
- Voice update writes at CPU access PCs around `00:586c`–`00:5894` occur in
  this version. This independently supports the existing firmware notes.

## Remaining work toward ROM-independent runtime

1. Establish the complete sound-data layout. `SC55PatchTable` currently finds
   names in **control ROM2**, not wave ROM2. Its existing comments are ambiguous,
   its original alignment was wrong. The name-first layout is now corrected;
   `--patch-probes` verifies Organ 1's live partial pointer against it. Other
   field meanings still require verification. Merely removing H8 execution would
   leave this data dependency intact.
2. Implement native MIDI/system state, patch/variation/drum selection, voice
   allocation, and note/release/controller handling.
3. Generate pitch, envelope, filter, pan and effect control updates from native
   state. Compare against the oracle using broader input scenarios.
4. Integrate with PCM, effects and the plugin UI without the firmware scheduler.
5. Verify complete runtime operation with both control ROM files absent:
   startup, presets, all supported MIDI/GS handling, polyphony, release tails,
   effects, UI and saved-state restore. Wave ROMs remain the sample source.

No native engine has passed those requirements yet. A trace replay or a captured
set of note samples would not satisfy the goal. Generated ROM-derived traces
stay outside the repository.

## Native MIDI front end

`src/backend/sc55_midi.h` implements a firmware-independent, allocation-free
byte decoder. Channel/system-common events retain their original data bytes;
note-on velocity zero remains a note-on event for the controller to interpret.
SysEx is streamed, with explicit abort events, rather than stored in a bounded
packet. GS checksum/address/transaction handling and the native controller are
not implemented by this decoder.

`ctest --test-dir /tmp/sc55-firmware-oracle-build --output-on-failure` checks
fragmented messages, running status, real-time interleaving, cancelled partial
messages, SysEx interruption and a 64KiB bulk message.

Pass `--native-midi` to route the fixed oracle scenario through the decoder
before forwarding its events to the existing emulator. This scenario's PCM
trace is byte-identical to the direct-input reference. This establishes input
transport equivalence for that scenario; it does not establish native voice
control, GS compatibility, or ROM-independent synthesis. Boot GS reset still
uses the emulator's own reset path.

## Native channel state

`src/backend/sc55_channel.h` stores per-channel volume, pan, expression, reverb
send and chorus send without firmware RAM or ROM access. Program change is
retained as a requested program number; resolving it to variations/drum sets
is still pending. Unhandled events return false for another handler to process.

`--controller-probes` checks boot defaults, then sends every value 0–127 for
CC7, CC10, CC11, CC91 and CC93 on each of the 16 channels. After each update it
compares all 80 fields against the running v1.21 firmware, including channels
that should not change. Result: **10,240 updates; 819,280 field comparisons;
zero mismatches**. The test uses default GS channel-to-part routing, without
SysEx reassignment, and allows 25ms of emulated source frames per update.

CC10=0 is stored as 1 by this firmware; the native CC handler matches that.
GS SysEx pan is a separate path and must not automatically inherit this rule.
These checks prove stored parameter values, not voice allocation, audible
controller update timing, smoothing, CC121 reset behavior or native synthesis.

## Native capital-tone selection

`src/backend/sc55_preset.h` resolves bank-MSB 0 melodic programs to the corrected
v1.21 patch table indices without interpreting H8 code. Unsupported banks and
out-of-range programs return no selection; rhythm-set and variation fallback
semantics remain unfinished rather than being guessed.

`--program-probes` tests all 128 programs on MIDI channel 1, with bank defaults,
note 60 and velocity 100. For each program it observes the common-patch pointer
at `00:2d01`, verifies alignment against the name-first table, and compares that
index to the C++ resolver fed by the native MIDI decoder/channel state.
All 128 selections matched. The 95,412-write PCM trace also matched the original
direct-MIDI sweep byte for byte. This does not verify velocity/key-dependent
partial selection, variation banks, percussion or waveform address decoding.
Patch data itself is still read from the control ROM by the reference engine.

## Native sample address generation

`src/backend/sc55_voice_setup.h` computes encoded start/end/loop addresses from
a 10-byte descriptor with 24-bit carry/borrow semantics, then emits the sample
address portion of the PCM startup transaction. Firmware locations:
`00:2bb4..2bec` (arithmetic), `00:5784..57a7` (device writes).

`--sample-probes` runs the 128-program sweep and checks both operations against
live firmware. Result: 165 descriptor-to-address calculations and 1,815 emitted
bytes matched, with the complete 95,412-write reference trace unchanged.

The oracle still supplies the selected descriptor, start-mode flag and PCM mode
from firmware state. Thus this proves the arithmetic and serialization only;
descriptor lookup from patch/key/velocity, slot selection, envelopes and key-on
remain to be replaced. This is not a ROM-free playback implementation.

## Native multisample zone selection

`SelectSampleZone` selects a sample ID from a 60-byte multisample record:
16 inclusive key limits at offset 12, followed by 16 big-endian IDs at offset 28.
The corresponding firmware search is `00:1516..1523`. The implementation accepts
a data record, not a firmware memory reader. High-bit sample IDs are retained;
their special handling is not implemented as normal melodic samples.

For the v1.21 reference adapter, partial offset +2 contains the multisample
group number. Groups below 144 reside at bank 1, `0xbd00 + group*60`; subsequent
groups at bank 2 with the index reduced by 144. `V121MultisampleOffset` now
performs this import-time mapping in C++; the oracle reads the ROM byte array
directly instead of accessing it through MCU_Read. Group selection,
preprocessing of the lookup key, and permanent ROM-free data assets are pending.

The 128-program/key-60/velocity-100 sweep checks 165 selections against H8 r4 at
`00:1523`. All match, and the full 95,412-write trace remains unchanged. Synthetic
tests cover all 128 keys at split boundaries, absent matches, high-bit IDs and
24-bit address carry/borrow. Full key/velocity sweeps against firmware remain.

Observation caveat: `MCU_Step` can service an interrupt before executing the
observed PC. Returning from it can expose the same entry twice. The selector
probe permits an identical pending selection on re-entry instead of treating
it as a nested call. It still rejects a changed unfinished selection.

## Native descriptor lookup

`V121SampleDescriptorOffset` maps the selected sample ID to an import-time
ROM2 offset. IDs below 532 use bank 1, subsequent IDs use bank 2 after
subtracting 532; records are 16 bytes at `0xdec0`, with 16-bit bank-local wrap.
IDs with bit 15 set return no descriptor (the firmware uses a special path).
This is a v1.21 mapping, not a format validator or proof that any arbitrary
positive ID denotes valid sample data.

The sample probe predicts the descriptor from native zone selection and
compares it with EP:R5 at `00:1557`, before the firmware restores EP.
All 165 lookups in the 128-program/key-60/velocity-100 scenario matched;
the full 95,412-write trace is byte-identical to the previous reference.
Unit tests cover both bank transitions, offset wrap and negative/sentinel IDs.

These offsets belong at a future sound-data import boundary, not in the audio
callback. Runtime-owned sound data and native partial/key processing are still
needed. Production playback continues to execute H8; this does not remove its
control-ROM dependency yet.

## Native partial key arithmetic

`TransposePartialKey` replaces `00:1378..138f` (partial +0x0a, centre 64).
`MakeSampleLookupKey` replaces `00:1361..1374` (partial +1 reference key).
Both preserve the original byte wrap and N/borrow clipping, including inputs
outside the normal MIDI range. They do not yet implement key tracking at
`00:1390`, scale tuning at `00:141f`, or the preceding key override.

The sample probe executes each actual ROM instruction slice in an isolated MCU
with a synthetic SRAM partial, for all 256*256 input pairs per operation.
131,072 comparisons matched. This checks numerical outputs, not peripheral
timing or whole-note equivalence. The regular 128-program sweep also passed
and retained the identical 95,412-write trace. Sample probes now reject ROM
sets other than the hash-identified `mk1-v1.21`.

Partial presence now uses the firmware's +2 word != 0xffff check
(`00:1232` and `00:12b3`), replacing the former first-eight-byte heuristic.
This is only presence, not the complete note/velocity selection policy.

Resolved import issue: the heuristic scanner reported 225 records, misreading
ROM2[0x1bd00] (multisample group zero, `PIANO1`) as a patch. The C++ loader and
Python listing tool now identify the exact v1.21 ROM2 SHA-256 and import 224
records at 0x10000, ending at 0x1bcff. Other images still use an unverified
heuristic, not this version-specific boundary.

`SC55PatchTable::loadRecords` provides a bounded, data-only import path for
known layouts and extracted patch assets, including assets starting at zero.
It validates record count, buffer extent and every name before exposing any
records. Tests cover a printable following multisample name, truncated final
records, invalid counts/offsets and failed-load state. The stored patches own
their data; the importer retains no span into the supplied buffer. This is a
building block for a sound-data asset, not a complete control-ROM-free engine.

## Native key tracking

`TrackPartialKey` replaces the numerical operation at `00:1390..141e`,
returning both the byte key and the fractional word formerly at RAM 0xa1ca.
It generates the 21 coefficients at 00:14c6 with
`n == 0 ? 0 : (n * 65536 - 1) / 20`, rather than loading control-ROM data.
Remainders use the firmware's 65000 threshold and 999/65000 conversion;
negative fractions can be represented as 1000 with an integer borrow.
Byte-add N/V clipping is preserved, even when it differs from a wide clamp.

An isolated real-instruction sweep compares all 128 keys, 128 source keys
(RAM 0xa1b4 input), and tracking bytes 44..84: 671,744 exact key/fraction
matches. Unsupported input ranges return no result rather than indexing
beyond the verified coefficient table. Unit tests retain the rounding cases
that exposed incorrect 65535-based and ordinary 65536-based coefficients.
The usual 128-program PCM trace is unchanged. This is arithmetic equivalence,
not a timing or production replacement test: source-key derivation, scale
tuning integration and note-specific override paths still remain.

## Native scale tuning

`ApplyScaleTuning` translates `00:141f..14c4` after the caller selects the
part's tuning byte for the original note modulo 12. It uses the generated
tracking coefficient, the 65000 rounding threshold, and integer division by
6500. Tuning 64 and tracking 0 bypass the operation, including normalization.
Other paths perform at most one fractional carry/borrow; keys wrap as bytes
instead of clamping to the MIDI range. The input fraction range is 0..1000;
extreme output fractions may be outside this range, as in the firmware.

193,536 isolated instruction comparisons passed: keys 0/60/127/255,
fractions 0/1/9/10/499/500/990/999/1000, every tuning byte 0..127,
and tracking 0 plus 44..84. Both key and fraction are compared. This is not
an exhaustive sweep of every possible fraction or original-note value.
Unit tests cover carry, borrow, bypass and rejected inputs. The regular
128-program PCM trace remains byte-identical. Connecting the native pitch
steps to note/part state and replacing the note-specific override path are
still pending; production playback still runs H8.

## Composed partial pitch stage

`PreparePartialPitch` connects transpose, tracking and scale tuning. It takes
a partial data record, twelve scale-tuning bytes, and explicit key/source-key/
original-note inputs. The tuning pitch class comes from the original note,
not the transformed key. Unit tests distinguish these paths with nonuniform
tuning and verify unsupported tracking is propagated as failure.

The live sample probe predicts at `00:132b`, then compares key and fraction
at `00:1334`: all 165 invocations in the 128-program sweep matched. The PCM
trace remains identical. Unlike separate arithmetic tests this validates the
three-stage composition on real partial/part data, but input state is still
supplied by the reference engine, not by a native note-state implementation.

Next boundary: bytes `15 a3 d1 05 00 81` at 00:1334 and the corresponding
sequence ending 0080 at 00:133d are byte-memory comparisons against 0x81/0x80.
The linear disassembler splits them incorrectly. The emulator's
MCU_Opcode_MOVG_Immediate byte-operand/register-5 case consumes a 16-bit
immediate and compares its low byte. Mode 0x81 returns directly; 0x80 stores
the adjusted key at a1b3; other modes also replace a1b2 from [a1c6]+0x180.
The caller at 00:123d/12be then loads the partial-specific minimum from
a1cc/a1cd, replacing both R1 and the N flag before calling 00:1357.

## Native mode correction through sample selection

`ResolvePartialKey` returns the lookup key and optional original/adjusted-note
state writes. Mode 0x81 writes neither field; mode 0x80 writes only adjusted
key; other modes also write the remapped note. The following minimum-key gate
uses the sign of the caller-reloaded minimum, not flags from mode correction.
The comparison uses wrapped byte subtraction, not a normal C++ max.

The isolated test includes the real caller reload/BSR and checks 1,835,008
combinations: all byte modes and keys, seven minimum values, four reference
keys and a remapped note distinct from the minimum. Both state writes and
lookup key are checked. An earlier isolated test incorrectly omitted the
caller reload; the live combined probe caught the error at program 86.

The live probe now feeds C++ pitch -> mode correction -> sample-zone selection
-> descriptor lookup, using partial/part inputs captured at 00:132b. It no
longer feeds zone selection with H8's calculated key. All 165 selections and
intermediate key/state checks matched in the 128-program sweep, and the
95,412-write PCM trace remains identical. Native generation of the initial
note/part inputs, minimum keys and voice allocation remains to be implemented.

## Native initial melodic note key

`TransposePartKey` implements 00:1e1b..1e39 using part byte +6 (centre 64)
and the already-signed per-part offset at ab46. `TransposeMasterKey` implements
00:1e3a..1e58 using global byte 8005. Master value zero bypasses the add;
64 performs a zero-offset add, which differs for an out-of-range byte key.
Both preserve byte wrap and sign-based saturation.

524,288 isolated comparisons passed: part transpose covers all byte keys and
shifts with seven signed-offset boundary values; master transpose covers all
byte keys and shifts. The live probe captures original note and part/global
inputs at 00:11d0, computes the initial key, and checks R0/a1b3/a1b4 at 00:1219.
All 129 notes in the scenario matched. The native initial key/source key now
feed the composed partial pitch instead of reading H8's calculated R0/a1b4.
All 165 resulting sample selections still match; the PCM trace is unchanged.

This covers the melodic 11d0 entry only; the probe explicitly rejects 11ea and
11fe to avoid stale-input comparisons. Other note-entry semantics, generation
of per-part offsets/minimum keys, MIDI/GS state ownership and voice allocation
remain. Production playback still executes the control firmware.

## Native RPN coarse tuning

`ChannelControls` now tracks CC101/100 RPN selection, distinguishes subsequent
NRPN selection (CC99/98), and applies CC6 for RPN 0,2. v1.21 04:0bea..0c03
clamps the data-entry value to 40..88 then subtracts 64, storing signed
semitones at ab46+part. Native state stores this directly as `coarseTuning`.
Unsupported data-entry/NRPN operations still return false for another
dispatcher; observing NRPN selection nevertheless updates the selection mode.
Data increment/decrement, fine tuning, other RPNs and CC121 are not implemented.

`--rpn-probes` verifies all 128 values on all 16 default-routed channels,
checking all channels after every update. It also checks null-RPN and NRPN
selection prevent coarse-tuning changes. 201,312 state-field comparisons
passed against hash-identified v1.21 firmware. Unit tests cover clamp edges,
channel isolation, deselection and native reset defaults.

Initial melodic pitch now consumes the native channel's coarse tuning instead
of reading ab46 from firmware RAM. The default-tuning 128-program sample probe
continues to pass with an identical PCM trace. End-to-end nonzero coarse tuning
with note playback, GS channel reassignment and the remaining RPN semantics
still need verification/implementation.

## Tuned-note integration sweep and high-note mapping

`--tuned-sample-probes` adds 128 programs * five coarse-data values
(0/40/64/88/127) * five keys (0/24/60/103/127), velocity 100: 3,200
note-ons. All 2,560 ordinary note-ons reached native initial-key comparison.
The 640 key-127 notes use the alternate mapping at 00:0ccd..0ce8 instead.
`MapHighNote` decodes patch-common +8/+10/+12 big-endian tone IDs and
+14/+15/+16 replacement notes for keys 125/126/127. The high-bit tone means
absent. All 640 mappings were checked against H8 and were absent; no missing
initial-key observation is silently counted as a pitch match. Unexplained
entry paths cause the sweep to fail. Synthetic tests cover all three entries,
positive/negative IDs and invalid keys. Positive mappings are not yet played
by the native pipeline.

Including the earlier program sweep and final scenario, 3,445 pitch/zone
checks, 3,442 descriptor/address checks and 37,862 sample-address write bytes
matched. A repeat produced the same 923,374-write reference trace. This still
observes firmware playback, not native audio output. The wider sweep also
exposed interrupt re-entry at address calculation; the observer now accepts
identical pending inputs while rejecting changed pending calculations.

## Owned, data-only sample bank

`sc55_sample_bank.h` owns groups and 16-byte sample descriptors indexed by
logical IDs. Its API has no emulator, ROM address or source-buffer lifetime.
Setup-time loading sorts IDs and rejects duplicates, reserved descriptor IDs,
uncovered MIDI keys and missing playable-sample references. Failed validation
preserves the prior bank. Queries use allocation-free binary search; reloading
must occur outside playback and invalidates previously returned pointers.

The oracle's v1.21 import adapter gathers group IDs from all 224 patches and
the playable sample IDs reached across every key in those groups. It imports
161 groups and 758 descriptors, then zone selection reads only the owned bank.
At the firmware descriptor-selection boundary, all 16 descriptor bytes are
compared with the independently imported bank. The 3,200-note sweep passes:
3,445 zones and 3,442 descriptors checked, with the same 923,374-write trace.
Unit tests cover independent ownership, failure preservation, duplicate groups,
missing references, special IDs and uncovered keys.

This bank is not yet a complete sound-data asset: drum-only groups, all other
control-ROM data tables, serialization and production engine wiring remain.
The research importer still requires control ROM at setup. Playback without
that ROM is therefore not achieved merely by adding this owned-data boundary.

## Sample-bank serialization (research format v1)

`SampleBank::encode/loadEncoded` stores only group/sample records, not CPU
instructions or a control-ROM image. Layout: ASCII `SC55SB01` (8 bytes),
big-endian uint16 group/sample counts, then groups (uint16 ID + 60 bytes)
and samples (uint16 ID + 16 bytes). No padding or trailing bytes are accepted.
Encoding is deterministic in ID order. Counts and exact lengths are checked
before allocation; reference/duplicate validation happens before replacing
the current bank. Tests cover every truncation of a fixture, extra bytes,
unknown version, exaggerated count, missing sample references and failed-load
preservation. This format has no cryptographic integrity/authenticity check.

Export once from the supplied v1.21 ROM set (output must not exist):

```sh
/tmp/sc55-firmware-oracle-build/sc55-firmware-oracle \
  '/path/to/SC-55 v1.21' /tmp/sc55-v121-melodic-samples.sbank --export-sample-bank
/tmp/sc55-firmware-oracle-build/sc55-sample-zone-test \
  /tmp/sc55-v121-melodic-samples.sbank
```

The second executable links no emulator or ROM loader and loads only the
23,638-byte artifact: 161 groups and 758 descriptors. The oracle also round
trips through this representation before using the bank for sample probes.
The derived artifact stays outside the repository; no ROM-derived binary is
added to source control. This is NOT a complete instrument asset: patches,
drum coverage, other tables and wave-ROM audio data are not in this file.
Product playback still needs the existing H8 engine/control ROM.

## Native velocity gate and partial candidates

`PartialVelocityCurveIndex` translates 00:1034..1082 for MIDI-range inputs.
Partial +0x41/+0x43 are inclusive endpoints; reversed endpoints reverse the
index. Normalization uses byte-DIV overflow saturation, then halves the result.
All 128^3 = 2,097,152 velocity/endpoint combinations were compared with actual
firmware instructions, including accepted/rejected outcomes and curve index.
Equal endpoints expose an existing emulator limitation: divide-by-zero calls
MCU_ErrorTrap but leaves the dividend and clears V, yielding index zero here.
The native function preserves that emulator result; physical hardware behavior
for this case is unverified. This does not establish the later amplitude curve.

`SelectPartialCandidates` uses patch-common +6 (patch +0x12) enable bits and
the velocity intervals, preserving high bits while returning the surviving
count. The later absent-group check is separate. In the live 3,200-note sweep,
all 2,689 candidate-mask/count decisions matched RAM a1b0/a3d4 at 00:1033.
The complete 923,374-write PCM trace is unchanged. Unit tests cover reversed
and equal bounds, rejected velocities, split-boundary flags/counts.

Remaining in 00:1034..1119: the two velocity-curve lookups and their amplitude
arithmetic/state writes (a1b7/a1b8/a1bb), plus native ownership of incoming
velocity and the allocator consuming the candidate mask. No native-only
playback claim follows from candidate selection equivalence.

## Full native partial velocity values

`EvaluatePartialVelocity` now includes interval/index selection, the first
curve lookup, accumulator/amplitude interpolation and the second curve lookup
through 00:1119. It returns optional accepted values for a1b7/a1b8/a1bb.
`InterpolateVelocityAmplitude` preserves byte-signed differences, accumulated
offset wrap, the rounded 16-bit multiply/shift, and zero-to-one output handling.
87,808 isolated interpolation cases matched firmware (seven boundary values
for accumulator/increment/endpoint, every curve byte). Unit tests also check
the secondary-curve index reduction `velocity*91/127`.

The reference adapter imports sixteen 256-byte curve windows using the table
at ROM1 0x111a and ROM2 bank 3. The wider-than-128 windows preserve byte-index
wrap for reversed intervals; overlapping windows are intentional. Runtime
native evaluation has no ROM accesses. All 3,445 live velocity operations in
the tuned-note sweep matched acceptance and the three output values, with the
same 923,374-write PCM trace. The sweep still uses velocity 100; broad
live velocity/reduction tests remain beyond the isolated arithmetic tests.

The curve data is not yet included in the serialized sample bank. The probe
still supplies initial accumulator, input velocity and the part reduction bit
from firmware state. Whole-patch sequencing/native note-state ownership and
PCM voice initialization remain before native-only playback is possible.

## Whole-patch velocity preparation

`PreparePatchVelocity` evaluates enabled partials in order, carrying the
accumulator only across accepted partials. It returns candidate flags/count,
final accumulator and optional per-partial amplitude/secondary values.
Disabled/rejected partials do not advance state. Tests distinguish shared
accumulation from two independent evaluations and preserve upper flag bits.

The oracle now prepares both partials at 00:0fbe from imported patch data,
native accumulated state and the received MIDI velocity. It compares every
called partial at 00:1119 and final a1b7/a1b9/a1ba/a1bc/a1bd at 00:1033.
The previous per-partial reads of firmware a1b7 and partial bytes are removed.
Native accumulation resets when the reference enters the note handler 00:0bc1,
matching the reset at 00:0bcd. Received velocities are retained per MIDI
channel/key and checked at that entry, rather than sourced from a3d3.

This is still an observer driven by firmware entry timing and default channel
routing. The received-value table is not a complete timestamped note queue or
overlapping-note model. Native scheduling, GS reassignment, reduction-controller
state and voice allocation are not implemented by this preparation function.

## Native soft pedal; long-sweep verification

CC67 now owns `ChannelControls::softPedal` (threshold 64). This matches the
00:273e..2762 handler and its ab02 bitmask under default channel routing.
The controller probe covers all 128 values on all channels, including other
channel isolation: 1,376,368 total state comparisons passed. Unit tests cover
the full threshold range and native reset defaults. Whole-patch velocity
preparation now consumes this native state instead of reading ab02.

`--soft-sample-probes` toggles CC67 during the tuned-note sweep. All 3,200
notes now pass (640 absent high-note mappings, zero unverified entry paths),
with 3,445 velocity/pitch/zone comparisons and 37,862 sample-address bytes
matched. The trace contains 923,053 writes over 13,314,400 frames.

The initial failure at program 101/coarse data 88 exposed an emulator UART
race, not a native soft-pedal discrepancy. A peripheral event arriving between
SSR read and write was erased by the final unconditional register assignment,
while its interrupt request remained pending. The firmware then repeatedly
received the same 0x40 byte roughly every 600 cycles. Its 512-byte receive ring
overflowed at 00:0601, injected an internal 0xfe sentinel, and reset all parts
via 00:2013 -> 04:08b8 -> 04:0844. This cleared RPN/coarse tuning.

`mcu.cpp` now preserves RX/TX completion flags unless the existing
read-then-clear checks acknowledge them. The ROM-free `uart-status` CTest
uses the actual peripheral functions to reproduce read/event/stale-write
ordering (failed before the fix), checks RX and TX, proper acknowledgement,
and that writing ones cannot fabricate completion. No test pacing, firmware
RAM correction or native-state reset was added. Sample probes now reject
receive-ring overflow directly. Temporary instruction/reset tracing is gone.

The default baseline still reports 4,972 writes, 307,200 frames and 111,596
nonzero frames after the fix; these counts alone are not audio equivalence.
This verification does not establish native scheduling or ROM-free playback.

## Data-only melodic asset

`sc55_sound_data.h` owns the 224 patch records, sixteen 256-byte velocity
curve windows and the validated sample bank. Its research format `SC55MD01`
is exactly: eight-byte magic, 224 × 216 patch bytes, 16 × 256 curve bytes,
then one complete `SC55SB01` sample bank. Integers inside those records retain
their existing big-endian representation. Invalid sizes, names, sample banks
and missing patch-to-group references fail without replacing prior valid data.
The format has no checksum/authentication and is not a complete SC-55 asset.

Export and consume in a separate ROM-free process:

```sh
/tmp/sc55-firmware-oracle-build/sc55-firmware-oracle '/path/to/SC-55 v1.21' /tmp/new-melodic.sdata --export-sound-data
/tmp/sc55-firmware-oracle-build/sc55-sound-data-test /tmp/new-melodic.sdata
```

The consumer links only native preparation, patch parsing and SHA code, not
the emulator, ROM loader, JUCE or PCM. The v1.21 asset is 76,126 bytes. It runs
57,344 preparations (224 tones × 128 velocities × soft off/on), accepting
76,800 partials. This is execution/coverage evidence, not an audio comparison.
Generated data stays outside the repository; extraction still needs the
user's ROMs once, and derived data is not independent of their provenance.

All sample-mode oracle runs now deserialize the complete melodic asset and
use its patches for whole-patch velocity/high-note preparation, its curves
and sample records for native evaluation. They verify every imported common
and partial byte against the extraction source. The soft-pedal 3,200-note
sweep passes with a byte-identical 923,053-write trace to the prior version.
The reference side still executes control ROM instructions; the standalone
consumer does not synthesize audio. Drums, remaining control tables, native
voice allocation, envelopes, effects and scheduling are still outstanding.

## Composed partial sample preparation and voice binding

`sc55_note_setup.h::PreparePartialSample` connects pitch/scale tuning, key
resolution, multisample selection and descriptor address decoding. Its inputs
are an owned partial, sample bank and explicit note/part values; the returned
plan contains pitch, optional note-state writes, sample ID and both start-mode
address triples. High-bit sample IDs remain valid special results without
fabricated descriptor addresses. Missing groups, absent partials or unsupported
pitch inputs fail explicitly. This operation allocates no storage.

The oracle no longer reads partial bytes from control ROM at 00:132b or
descriptor bytes at 00:2bb4 to supply these native calculations. It uses the
deserialized sound asset and compares the result at each firmware boundary.
Plans are bound to logical voices at 00:114f: one latest-plan variable was
insufficient because the firmware can prepare a second partial before
initializing the first. The 24-entry binding retains each voice's selected
sample and addresses until reused. At 00:5784, native address writes are
generated from that retained plan, not the firmware's calculated address RAM.

This is **not yet native voice allocation**: binding IDs, timing, the
unoffset-start flag, PCM mode word and several note/part inputs (scale tuning,
minimum-key/remapping state, initial transpose settings) still come from the
reference. They remain explicit replacement work rather than hidden defaults.
Unit tests cover composition, minimum-key behavior, stored-note modes, absent
and unsupported partials, missing groups, and special sample IDs.

## PCM sample-control word

`DecodeSampleControl` replaces the arithmetic at 00:2bef..2c13. It combines
the loop address bank, descriptor byte 10 bit 0, voice ID, and an explicit
four-bit history value. Descriptor bit 1 is returned separately. Native
sample-address writes now use this generated mode word rather than reading
the finished word at firmware voice-10.

Do not omit the history input: the H8 byte operations retain R3's upper byte,
and its low nibble becomes mode bits 12..15. A first zero-history-only test
passed in isolation but failed on sample 100/voice 1: 0x0201 versus firmware
0x1201. The scalar PCM implementation actually uses those bits as `old_nibble`;
masking the mismatch would hide state, not prove equivalence. The oracle
currently captures the incoming R3 nibble at 00:2bb4 for each voice. Deriving
that state natively remains outstanding, along with the previously documented
start-mode flag and allocator/timing inputs.

Verification includes 786,432 isolated bank/flags/voice/history combinations,
explicit unit regressions for the nonzero-history example and bank boundaries,
and the complete soft-pedal sweep: 3,444 control-boundary observations (PC
re-entry after an interrupt can repeat an observation), 3,442 sample-address
calculations, and 37,862 emitted setup bytes. The 923,053-write trace is
byte-identical to the preceding composed-partial version. This still does not
establish native-only playback.

## Native envelope initialization

`sc55_envelope_setup.h` generates five stage value/flag pairs from partial
bytes 0x4e..0x52 and two velocity scales from bytes 0x59/0x5a. Scale input is
the prepared partial amplitude, not MIDI velocity. The 21 coefficients at
ROM1 0x679a are exactly `(magnitude * 128 + 10) / 20`, so that table is no
longer a data dependency for this operation. Sensitivities outside the proven
44..84 domain are rejected. The arithmetic preserves byte subtraction and
16-bit denominator wrap; it is not floating-point interpolation.

The oracle supplies owned patch data and the already-native per-partial
amplitude, binds the resulting envelope plan alongside the sample plan, and
compares voice-30/-28, voice-8..-4 and voice+0x4f..0x53 at 00:2f52. All
20,992 isolated amplitude/sensitivity/operation combinations and 3,447 live
initialization observations pass. The full soft sweep's 923,053-write trace
is unchanged. Unit tests include the non-obvious scale 255 at amplitude 127
with negative sensitivity, clamping, range rejection, and all stage-byte values.

The old `SC55Partial::envelope[3][5]` remains a legacy dump interpretation,
not the runtime source: its claimed three equally encoded groups were not
supported by this instruction trace. Its comments now distinguish that from
the verified map. This work does not implement advancing envelope stages,
release behavior, controller modulation or the native scheduler.

## Normal-duration envelope advancement

`AdvanceEnvelopeProgress` in `sc55_envelope.h` replaces 00:3599..35c9.
For duration > 8 it computes the fixed-point increment, advances position,
and retains whole ticks left after the endpoint for the following segment.
It preserves the firmware's 16-bit elapsed-tick wrap. Duration 0 and 1..8
are rejected here: firmware routes them to hold/immediate paths, not this
division/advancement path. (The earlier description of zero as hold was
incorrect; see the immediate-duration verification below.)

`StepEnvelopeSegment` composes that advancement with the existing native
`EnvelopeSegment` interpolation and `EncodeEnvelope` PCM-word encoding.
The oracle checks progress/deferred ticks/interpolated level at 00:365d and
the stored level/PCM word at both return paths. 327,635 isolated combinations
cover every normal duration 9..65535 with five boundary-state/tick cases.
The soft sweep verifies 34,496 composed updates, retaining the same 923,053
PCM-write trace. Unit tests exercise carry into a following segment, tick
wrap, linear rise/fall, exponential endpoint, hold encoding, and unsupported
duration dispatch.

This is not an independently clocked envelope yet. The oracle still supplies
the current stage's input state, duration and elapsed ticks from firmware;
native stage selection, duration calculation, special-duration paths and
continuously owned per-voice state remain required. No reference memory is
patched to force an update match.

## Immediate and short-duration envelope paths

`StepEnvelopeSegment` now also implements duration 0 and 1..8, completing
the duration branch at 00:358e. Zero takes 00:36c6: set the target level,
force rate 0xaf, mark progress 0xffff, and leave deferred ticks unchanged.
It is **not hold**. Durations 1..8 take 00:36ae: set target/progress directly
and encode the difference with exponent limits 10,9,9,8,8,8,7,7 respectively.
An unchanged target in that second path still emits the 0xff00 hold word.
The existing `EncodeEnvelope` accepts an optional limit; its default remains
7, so prior callers keep their original behavior.

16,128 isolated cases cover all nine short/zero durations, all 256 target
levels and seven prior-level boundaries. These compare progress, deferred
ticks, level and PCM word against the actual instructions. The complete
soft sweep now checks 36,895 composed updates (previously only 34,496 normal
updates), and its 923,053-write trace remains byte-identical. All five CTests
pass. This fills the update's duration dispatch, not stage selection or
duration generation; those and the per-voice clock/state ownership still
depend on the reference engine.

## Native envelope duration generation

`PrepareEnvelopeDuration` combines the current seven-bit stage value, a
center-64 controller adjustment (two index steps per unit), an owned 128-word
time table and two scaling coefficients. `ScaleEnvelopeTime` preserves the
two separate narrowing operations and the firmware's saturation threshold
at product high-word 255, which differs from a generic 16-bit clamp.

The oracle checks all 128 stage × 128 controller values with five coefficient
pairs (81,920 isolated cases), then compares 36,898 live duration observations
across the three calculation entry paths. Its envelope update consumes the
native duration instead of the resulting H8 R6. The velocity scale is taken
from the retained native envelope initialization plan. Current stage value,
controller value and key-dependent scale still come from reference state;
native selection/modulation of those inputs remains to be implemented.

Data format `SC55MD02` adds 128 big-endian 16-bit time entries between the
velocity curves and sample bank. The exporter extracts ROM1 0x6f12 once;
runtime duration calculation uses the deserialized table. MD01 remains
readable but `times()` returns null, never fabricated defaults. Unit tests
cover both versions, version changes, missing/truncated data, saturation
boundaries and controller index clipping. The v1.21 MD02 asset is 76,382 bytes.
It remains an incomplete, user-ROM-derived research asset, not a full native
engine or a permission to redistribute the underlying ROM data.

## Envelope stage transitions

`AdvanceEnvelopeStage` implements the transition at 00:33f4..3445 using
native stage/state types. If progress is unfinished, it preserves state.
At the endpoint it resets position, retains deferred ticks, advances the
stage, and for attack2/decay1/decay2 loads the next parameter/flag/target,
moving the former target to the new start. Release completion returns a
terminal native stage. Sustain does not itself synthesize a note-off event.

7,168 isolated stage/progress/value combinations match the instructions,
including the terminal branch to 00:3472. Unit tests exercise a sequence of
transitions, preserved carry, the unfinished case and terminal idempotence.
The soft sweep compares 34,015 live transitions; its 923,053-write trace
is unchanged and all five CTests pass.

The observer still imports the incoming stage/current parameters/targets
from reference RAM for this check. The pure function can retain state in a
native engine, but the oracle is not yet independently owning that state
across ticks, note-offs and voice reuse. Transition correctness does not
establish native scheduling, sustain/release event dispatch or ROM-free audio.

## Envelope release request

`ReleaseEnvelope` implements the first envelope's part of 00:3220..328b,
after the pending release request is consumed and PCM level is synchronized.
Ordinary release sets the release stage, takes the synchronized level's high
byte as start, targets zero, clears progress/deferred ticks, and loads the
fifth stage parameter/flag. Repeated requests at release or later preserve
state. During delay, a nonzero delay increment terminates the voice; zero
switches to attack1 while preserving the envelope fields, matching the
separate firmware branch rather than treating every request identically.

5,376 isolated stage/level/delay cases match the reference. Unit tests cover
ordinary release, idempotence and both delay branches. The soft-sweep trace
remains byte-identical. This is only the first envelope's state transition:
MIDI note matching/sustain deferral, PCM level synchronization, the other two
envelopes' resets, voice termination and native scheduling are not implemented
by this helper. Its current live probe imports the incoming state from RAM.

## Persistent first-envelope runner

`sc55_envelope_runner.h` now owns the segment, level, PCM word and delay
accumulator across control ticks. It composes transitions, duration selection,
segment updates, sustain and release without importing firmware RAM on each
tick. Delay advances once per control invocation (not by elapsed audio samples);
completed segments transition on the next invocation. Zero sustain/release
levels signal the terminal stage. Setup still requires prepared targets and
key scales, and release takes an explicitly synchronized PCM level.

The isolated oracle initializes both implementations once per trajectory and
then injects only clock values, changing controllers and a release event.
384 trajectories (all 128 stage parameters, alternating curve types, delay
and zero-sustain variants) match for 17,612 ticks, including every persisted
field. Comparisons stop at the firmware's voice-cleanup branch; they do not
verify the other envelopes, PCM cleanup or actual native audio output.
The ROM-free `envelope-runner` CTest also runs complete note lifetimes,
long sustain, delay cancellation, invalid controllers and deferred-time carry.

The runner is not yet integrated into production voice allocation or the live
sample-sweep observer. Those remain H8-driven. Control-rate scheduling,
initial-state preparation, other envelopes and voice termination still need
native ownership before this can replace the plug-in's control CPU.

`EnvelopeRunner::start` now reproduces 00:2f5a..2fcc: delay counter/increment
preparation, conditional clearing of the final sustain target, zeroed progress
and level, then one attack tick irrespective of the allocated stage. An initial
PCM rate byte af is replaced by ba. Delay-time addition wraps at 16 bits before
division by eight; a high-bit delay parameter takes the separate zero-increment
branch, not an ordinary table lookup. Initialization uses the owned MD02 time
table and native stage plan, not CPU execution.

32,768 isolated delay/attack-parameter combinations match the actual routine,
including both curve types, all persisted output fields and restoration of the
firmware's elapsed-tick global. ROM-free tests also cover the synthetic time
overflow case. Allocation's stage, target preparation and key-dependent scales
are still explicit inputs; this is not yet a complete native note allocator.

## Envelope target preparation and MD03

`PrepareEnvelopeTargets` implements 00:2d14..2d67 using partial bytes 4a..4d
and a pre-attenuated base. It subtracts the parameter attenuation with a zero
floor, then maps each resulting index through the level curve. The earlier
base computation uses different clipping and is not replaced by this helper.
All 256 bases and 128 rotated parameter combinations (32,768 four-target
cases) match the actual instructions. Seven-bit parameters are validated.

SC55MD03 adds the 128-byte attenuation and 256-byte level tables after MD02's
time table. The one-time exporter reads ROM1 6b0f and 6b8f; runtime queries use
owned data and no control ROM. MD01/MD02 remain readable, with `levels()` null
when absent. Tests cover round trips, truncation, preserving valid data after
failed imports, missing required times, and switching back to older versions.
The exported v1.21 MD03 asset produces 76,800 target plans in the ROM-free
test executable. The complete soft sweep's PCM trace remains unchanged.

This does not yet eliminate base-amplitude/key-scaling computation by the H8
or integrate these targets into production voice allocation. No claim of
native-only audio output follows from the isolated target comparison.

`PrepareEnvelopeBase` now covers 00:2cda..2d14: prepared partial amplitude,
the voice +a0 record's byte zero, and patch common[0] each subtract their
attenuation from the key-adjusted base. Every subtraction floors at one,
including equality; this deliberately differs from segment target clipping.
98,304 isolated combinations match the instructions. The identity/selection
of the +a0 record remains outside this helper; it is not labeled as a MIDI
controller without evidence.

`EnvelopeRunner::fromPartial` composes that base calculation with target and
timing-plan preparation and the primed initial state. Its remaining inputs
are explicit note-allocation data (key-adjusted base, prepared amplitude,
record/patch levels, key scales, allocated stage and attack control). It does
not retain firmware pointers. With MD03, the ROM-free sound-data executable
creates, ticks, releases and completes 76,800 envelopes using actual melodic
partials and synthetic allocation inputs. This is a composition test, not a
physical note-allocation or PCM/audio-equivalence test. All six CTests pass;
the full soft-sweep reference PCM trace is byte-identical to the prior run.

## Key-dependent envelope times

`PrepareEnvelopeKeyScales` covers 00:2d67..2e30: partial bytes 55/56 select
one of 16 attack/release curves, the prepared key byte indexes that curve,
and sensitivities 57/58 produce two time multipliers. Sensitivity 64 and the
center curve point return 256 directly. Negative sensitivity reverses the
unsigned curve point, specially mapping zero to 255; coefficient arithmetic
and byte rounding match the original. Values outside sensitivity 44..84 or
selector 0..15 are rejected rather than reading beyond known tables.

The isolated comparison now includes ROM2 for these actual curve lookups.
16 selectors × 256 key bytes × 41 sensitivities (167,936 paired cases)
match both resulting multipliers. SC55MD04 stores the 256 multiplier words
and two sets of 16 × 256 curve bytes after MD03's level data. One-time import
reads ROM1 67c6 and ROM2 bank03 dc92/dcb2 pointer tables; runtime consumes
owned arrays without control-ROM addresses. MD01..03 remain readable, with
`keys()` null. Missing prerequisites and truncated imports are tested.

The MD04 data-only test feeds these native scales into `fromPartial`, then
updates/releases 76,800 envelopes to completion. Allocation inputs other than
the scales remain synthetic. Neither this test nor the isolated CPU comparison
establishes a native note allocator, control clock or audible PCM output.

## Key-dependent levels and composed initialization

`PrepareEnvelopeKeyLevel` covers 00:2c6a..2cda. Partial 45 supplies the
starting attenuation, 46 selects one of 16 key curves, and 47 controls the
direction/depth of its level bias. Results saturate to 1..255. The isolated
oracle compares 167,936 selector/key/sensitivity combinations against H8
instructions. SC55MD05 adds 129 adjustment bytes and 16 × 256 curve bytes
after MD04's key-time data. The offline importer reads ROM1 69c6 and ROM2
bank03 dc72; these ROM addresses are absent from the native calculation.
MD01..04 remain readable with `keyLevels()` null, and malformed/truncated
MD05 imports preserve the previously loaded asset.

`EnvelopeRunner::fromKey` now computes the key-adjusted base and both time
scales before invoking the existing base/target/timing/initialization chain.
The MD05 data-only test runs 76,800 composed envelopes to completion. The
remaining note inputs (prepared key/amplitude, selected record level, patch
level, allocated stage and attack controller) are still supplied by callers.

The live oracle compares the complete initialization-fields chain from
00:2c5d to 2fcc: 3,442 observations match during the 3,200-note soft sweep,
including progress, deferred ticks, level, PCM word, delay state, target levels
and key-time scales. The 923,053-write reference trace is byte-identical.
At this entry point, the allocator has not necessarily assigned an active
envelope stage. The test therefore separately verifies that the raw stage
code is unchanged across this routine; it uses a neutral native stage solely
for field preparation. This does NOT establish correct voice activation.
No initialized runner is yet driving production audio or the live update
scheduler. Native allocation, activation, other envelopes and PCM integration
remain required for control-ROM-free sound generation.

## New-voice envelope activation

`activateNewVoice` implements the first envelope's activation at 57ca..57f3
(the new-voice flag branch only). A nonzero initialized delay accumulator
selects attack1 and preserves the primed position/PCM word. Zero selects
delay, clears position and changes the PCM word to 00b6; cached level and
deferred ticks are not reset. This does not implement the alternate reused-
voice restoration at 57f6, the other envelopes, or the pitch-register write.

2,304 isolated accumulator/progress/word cases match the instructions before
their PCM writes. The live oracle retains the independently prepared runner
per logical slot from 2fcc and activates it at 57ca without re-importing its
state. 3,442 activations match stage, progress, deferred ticks, level, PCM word
and delay accumulator at 57f5. The full soft-sweep trace is unchanged.
MD05's data-only test now includes activation before ticking/releasing all
76,800 envelopes. Production and live control-tick scheduling are still H8-
driven; retaining activation state alone does not replace the control CPU.

## Persistent live envelope trajectory

The live oracle now ticks the per-slot runner retained from initialization and
activation, rather than constructing a segment from H8 RAM at each update.
It injects elapsed control ticks and part controllers at 33f4, and release
events at 3220. Only the PCM readback level (observed after the firmware's
PCM read/half-scale conversion at 31dc) is synchronized through
`synchronizePcmLevel`; that setter cannot alter stage, targets or progress.
Interrupt re-entry at an unexecuted start PC is guarded against double ticking.

34,015 live control updates match stage, progress, deferred ticks, cached
level, PCM word, delay accumulator, segment endpoints and parameter/flag.
All 3,442 initialized/activated runners are prepared natively from MD05 data;
no stage/progress/target corrections from H8 are applied during their lifetime.
The full 3,200-note soft sweep's 923,053 PCM writes remain byte-identical.
All six CTests and the MD05 data-only lifetime test pass.

This is still a shadow trajectory: H8 supplies scheduling, release dispatch,
initial note inputs and the bridge to PCM readback, and H8 writes the actual
PCM registers. Production is not using this runner. Native event scheduling,
voice stealing/reuse, other envelopes, PCM I/O and audio equivalence remain
unproven and required; matching one envelope's state is not native synthesis.

## Envelope PCM synchronization

`sc55_envelope_pcm.h` implements the register transaction at 31c7..31e5.
For a moving envelope it writes ff00 to PCM 18/19, reads 34 to latch the
current amplitude, then reads 3a/3b and doubles the 16-bit value with wrap.
For a held envelope it writes cached level >> 1 to 34/35 without reading.
The caller must select the physical channel and serialize access to PCM.
The native runner's progress/stage/PCM command remain untouched by readback.

The live observer now supplies raw readback R5 at 31d7, not the H8-converted
RAM level at 31dc, and the native conversion feeds the persistent trajectory.
Native synchronization plans predict all 68,110 byte writes in the soft sweep;
34,015 persistent updates still match, and the full trace is byte-identical.
Unit tests verify exact callback order, no reads in the hold branch, preserving
the command word, odd cached levels and all 65,536 raw readback values.

This bridge is ready for native PCM callbacks, but the live test still uses
H8's actual I/O and timing. It checks plans rather than replacing the device
transactions. No production CPU removal or native-only audio is claimed.

## Direct native-to-PCM register integration

`sc55-firmware-oracle --native-envelope-pcm-test` is also registered as the
`envelope-pcm` CTest. It creates a PCM device and a configuration-only MCU
object without loading ROMs or executing MCU instructions, then connects
`SynchronizeEnvelopePcm` directly to the real `PCM_Read`/`PCM_Write` functions.
16,384 channel/value/moving-or-held cases verify converted levels, command
registers, half-scale writes and isolation of the other 31 channels.

`WriteEnvelopePcm` emits a runner's command to PCM 18/19. The integration test
uses it across attack/decay/sustain/release and verifies real register values,
including af endpoint commands, ff00 hold and zero/af release. MCU PC and
cycle count remain zero. All seven CTests pass.

This test intentionally does not run `PCM_Update`, advance physical sample
time, load wave ROMs, allocate polyphonic notes or generate audio frames.
It establishes direct register interoperability, not the complete native
scheduler or a control-ROM-free sounding instrument.

The same test now additionally advances `PCM_Update` on the original slot
pipeline with 24 slots and a single enabled voice. A synthetic control clock
issues native envelope commands, synchronizes actual interpolated PCM levels,
and releases the envelope after 160 control ticks. The run completes in 192
control ticks with 24,580 output callbacks, nonzero rising PCM envelope level
and a falling release. MCU PC/cycles remain zero throughout. All seven tests
pass. Wave memory is zero-filled; these callbacks are not evidence of a
correct SC-55 tone, and the synthetic clock is not the recovered scheduler.

The test exposed an important lifecycle distinction: the terminal envelope
stage can be reached while the PCM readback is still nonzero (272 in this
fixture). The last synchronization must preserve that actual value, not
fabricate zero. Physical voice termination/muting remains a separate owner
responsibility to implement and verify against the firmware's cleanup path.

## PCM termination ramp

`WriteEnvelopeTermination` implements 33c9..33d0: select the physical channel
and write 00b6 to PCM 18/19. This is a ramp command, not an immediate zero of
the current-level register or a voice-enable-mask clear. Invalid physical
channels are rejected without issuing any write.

The timed native-to-PCM test now issues that command after logical envelope
completion. It asserts the nonzero residual remains immediately after the
write and reaches zero after 128 more pipeline passes. Tests cover channel
selection/isolation across all 32 slots. The live firmware sweep matches
10,329 termination bytes (3,443 transactions), with its complete PCM trace
unchanged. All seven CTests pass.

Only the PCM part of termination is implemented here. The preceding logical
voice/link cleanup and following firmware event dispatch remain outside this
helper, as do native polyphonic allocation and the actual control scheduler.

## Voice-link detach

`sc55_voice_links.h` owns the two 24-entry link tables and implements
339b..33c9. It chooses the first link if present, otherwise the second, then
clears both fields on the terminating voice and that selected partner. With
no partner it leaves the tables unchanged. It does not walk a chain, erase
the other partner's entry or terminate another envelope. Invalid selected
partners/voice indices are rejected without partial mutation.

All 24 voices × 25 first-link possibilities × 25 second-link possibilities
(15,000 cases, including absent and self links) match the firmware, comparing
all 48 table bytes. ROM-free tests also verify precedence, self links and
invalid inputs. The live observer separately compares detach operations from
incoming link-table snapshots; creation and continued ownership of those links
by the actual note allocator is not yet native. PCM termination and the
following queued event remain separate from this pure metadata operation.
The soft sweep matches 3,443 live detaches; its PCM trace is byte-identical
to the preceding termination run. All seven CTests pass.

### Pitch envelope completion dispatch (4fdb..50cf)

`PitchEnvelopeRunner` now owns the segment, raw even stage code, future
destinations/increments and pre-modulation output. Completion advances exactly
one stage per call, resets progress but retains deferred time for interpolation,
and retargets stages 4/6/8. Stages 10/22 hold the exact target and discard deferred
time; inactive stages leave output unchanged. Invalid stage codes are rejected
before mutation. Runtime processing uses fixed-size state and integer arithmetic.

The isolated H8 comparison covers all twelve even stages, sixteen starting
variants and sixty-four consecutive calls each (12,288 updates), checking both
output copies, endpoints, direction, increment, progress and deferred ticks.
`pitch-runner` additionally tests ROM-free multi-stage traversal, interpolation
rounding, holding, inactive states and invalid-input preservation. All eight
CTests pass. `/tmp/sc55-pitch-runner-2.csv` is byte-identical to
`/tmp/sc55-pitch-retarget-1.csv` (923,053 PCM writes). This trace still runs H8:
it is regression evidence, not native instrument or host performance evidence.

Re-entry dispatch at 4f9e is now implemented by `reenter(ticks)`. It leaves the
stage and progress intact, selects endpoints from the saved future targets,
then dispatches interpolation/holding/inactive behavior without completion
advancement. Stage 4 starts from the previous target; stage 6 from future target
0; stages 8/10 from future target 1; stages 12..22 from base pitch to the release
target. The latter includes inactive stages, whose endpoints change even though
their output does not. Re-entry is not itself a MIDI note-off operation.

The differential suite now checks 24,576 persistent updates, with re-entry on
every fourth call in half the scenarios. ROM-free tests verify preserved
progress, changed release direction, base-pitch origin and invalid rejection.
All eight CTests pass; `/tmp/sc55-pitch-reentry-1.csv` is byte-identical to the
previous stage-runner trace.

The initial preparation branch at 4f60 and the note-off stage producer remain
to be connected. The production instrument still needs its
control ROM; this helper does not establish ROM-free playback.

### Post-envelope pitch modulation (50cf..5175)

`PrepareModulatedPitch` applies the signed voice offset, two calls to
`ApplyPitchModulation` (5368..53e4), master tuning centered on 1024, and signed
part tuning in firmware order. Inputs are owned numeric values, not ROM/RAM
pointers. The unmodulated envelope output remains separate.

The offset clamps negative underflow by borrow, but positive offsets wrap to
24 bits before clamping at 127000. Modulation combines two signed depths:
same-sign magnitude sums wrap to 16 bits before saturation at 6000; mixed-sign
sums do not saturate. The absolute waveform doubles with 16-bit wrap before
rounded multiplication. Negative modulation and final tuning use the wrapped
24-bit sign clamp, including a negative modulation branch whose rounded amount
is zero. These distinctions are covered by ROM-free boundary tests.

The complete 50cf..5175 H8 path matches 16,384 boundary/seeded cases, including
both internal subroutine calls and the preserved unmodulated output copy. All
eight CTests pass. `/tmp/sc55-pitch-modulation-1.csv` is byte-identical to
`/tmp/sc55-pitch-reentry-1.csv`. Waveform/depth producers and production voice
integration are still missing; this does not demonstrate a ROM-free instrument
or reduced host CPU usage.

### Composed per-voice pitch update

`VoicePitchRunner` owns the envelope, glide/correction state and final PCM pitch
word. Its update composes 4fdb (or 4f9e re-entry) through 5367, resetting the
working pitch to the current envelope output before each modulation/glide pass.
Only glide decay and correction cache persist, not prior modulation additions.
Inactive envelope paths bypass modulation, glide and conversion while preserving
any endpoint changes made by re-entry. Invalid stages or an active glide with an
invalid rate index are rejected without partial state changes.

The full H8 path matches 3,072 persistent updates (48 starting variants, 64
updates each), including changing modulation, correction source and rate,
positive/negative glide, all twelve stages and periodic re-entry. Comparisons
cover both pitch copies, envelope state, glide residual, cache and final word.
ROM-free tests cover non-accumulation, invalid-input preservation, inactive
bypass and zero-glide rate bypass. All eight CTests pass; the complete H8 PCM
trace `/tmp/sc55-voice-pitch-1.csv` matches `/tmp/sc55-pitch-modulation-1.csv`.

This is the runtime pitch calculation owner, not a native note scheduler. It
still requires prepared envelope data and current modulation/control values;
production integration and ROM-free audible playback remain unproven.

`VoicePitchRunner::installEnvelope` now connects prepared target/timing results
to that owner. It installs the initial pair, two later targets, base-pitch
destination and separate release destination, with all five increments. It
sets both unmodulated and working pitch to the first target but preserves
stage/progress, glide residual and correction cache. Initialization versus
re-entry remains the subsequent 4f51 decision, not an implicit reset here.

512 complete H8 preparations (4a5a..4f51) match the installed fields, varying
key, velocity, depth, random input, target offsets and timing parameters.
ROM-free tests also install into an existing release stage and continue with
the retained progress. All eight CTests pass; `/tmp/sc55-pitch-install-1.csv`
matches `/tmp/sc55-voice-pitch-1.csv`. Full note initialization, scheduler and
production connection are still required for control-ROM-free operation.

`VoicePitchRunner::initialize` implements the 4f60 branch through its return at
4f90: clear progress/deferred ticks, clear only the correction-cache source,
then interpolate and run modulation/glide/conversion for exactly one tick.
It bypasses stage dispatch and leaves the stage unchanged. The caller supplies
the current part's glide rate instead of retaining the H8 pointer to AB26[part].
Invalid active-glide rate indices fail before any mutation.

The persistent 3,072-update H8 suite now includes 96 initialization calls from
4f51 to 4f90, mixed with normal/re-entry updates and all twelve stages. It also
checks restored scheduler elapsed time, balanced stack and the selected H8 rate
pointer. ROM-free tests check inactive-stage bypass, one-tick glide decay,
retained cache offset when the incoming source is zero, and invalid preservation.
All eight CTests pass. This initializes pitch calculation, not the full note:
the flag producer, voice scheduler and remaining synthesis paths still require
native integration before control-ROM-free playback is possible.

### Pitch handoff at voice activation

`PrepareVoicePitch` transfers the pitch runner's stage and calculated word into
the existing voice-commit owner. `ContinueVoicePitch` transfers the resulting
stage back after activation, preserving pitch progress, deferred ticks, glide,
cache and calculated word. In the delayed-start branch, PCM10 receives zero but
the cached +48 word remains unchanged; only amplitude progress is cleared by
57e2, not pitch progress. Do not infer pitch state from that temporary PCM write.

The existing full commit differential now uses converted pitch words and checks
the return handoff in 6,144 H8/PCM cases across all 24 channels, including both
activation flags and delay branches. The paired PCM memories match. ROM-free
tests verify ordered 18/19/10/11 byte writes, retained pitch state and invalid
stage rejection. All eight CTests pass. The full H8 observer trace remains
unchanged (`/tmp/sc55-pitch-commit-1.csv` vs `pitch-initialize-1.csv`). This
connects research owners; the product still runs H8 and has no native note
scheduler or demonstrated control-ROM-free playback.

### Periodic voice register commit

`UpdateVoicePcm` implements 5855..5898. The first envelope's raw stage gates
the entire transaction: zero or >=14 emits nothing; otherwise channel select
3e is followed by words 18,16,12,14,1a,1c,10, high byte first. Pitch stage is
not the gate. The 1a command comes from voice+26, not the different prepared
1a value used during activation. Invalid channels >=24 produce no I/O.

768 paired H8/native PCM cases cover all 24 channels and raw stages 0..31;
both PCM RAM banks and write counts match. ROM-free tests verify the complete
15-byte ordering/values, first-envelope gating, separate 1a source and invalid
channel handling. All eight CTests pass. The H8 observer trace
`/tmp/sc55-periodic-pcm-1.csv` matches `/tmp/sc55-pitch-commit-1.csv`.
This supplies the periodic commit operation to the native scheduler work;
production scheduling and remaining control-value producers are not replaced.

### Active pitch note-off setup

`PitchEnvelopeRunner::release` implements the active branch's stage store at
3269 and pitch setup at 32a3..32dc. It starts from the current unmodulated
output, not base pitch (the latter is used by re-entry), chooses the release
target/increment, sets direction with equality choosing zero, and clears both
progress and deferred ticks. Output and downstream glide/cache remain intact.
The caller must first resolve the amplitude-stage/delay acceptance branch;
this method is not a complete MIDI note-off dispatcher.

64 active release setups are now mixed into the 3,072 persistent full-pitch
updates, comparing setup fields and subsequent H8 results. ROM-free tests cover
boundary endpoints, equality, retained initial output and release completion
into stage22. All eight CTests pass. The observer trace
`/tmp/sc55-pitch-release-1.csv` matches `/tmp/sc55-periodic-pcm-1.csv`.

`SelectVoiceReleaseAction` now shares the 3220..3269 decision between the
first-envelope release helper and `PitchEnvelopeRunner::requestRelease`.
The caller passes the first envelope's pre-release raw stage: >=12 is a no-op;
zero selects stage2 when delay increment is zero or stage22 otherwise, retaining
pitch progress/endpoints; other stages take the active pitch release path.
This must not be gated by the pitch's own current stage.

1,152 H8 executions from 3220 to 32f0/3363 cover first stages0..31, all twelve
pitch stages and three delay increments. Pitch state and cancellation exit
match. ROM-free tests cover independent stages and repeated release preserving
progress. All eight CTests pass, including the existing amplitude release tests.
The observer trace `pitch-release-gate-1.csv` matches `pitch-release-1.csv` in
`/tmp`. Pending request consumption, PCM-level synchronization, second-envelope
state and the final modulation flags remain the voice scheduler's work; this
is not yet a complete MIDI-to-native note-off route.

`ConsumeVoiceRelease` now composes 3212..32f0/3363 after PCM synchronization:
consume the pending flag even when already released, use the pre-release first
envelope stage for all three owners, apply amplitude and pitch setup, prepare
the second envelope's release fields and replace zero modulation-release words
with ffff. Delay cancellation clears activity and returns `cancel`, allowing
the scheduler to skip normal updates; no pending flag returns nullopt.

96 composed H8 cases vary amplitude stage, pending flag, delay and release-word
values. They check all three stages, progress, second-envelope endpoints and
parameter, amplitude endpoints, pending/activity and modulation words. ROM-free
tests cover no request, active release and repeated-request consumption. All
eight CTests pass; `/tmp/sc55-voice-release-1.csv` matches the previous release
gate trace. The second envelope now has release-state ownership, not a complete
tick implementation. MIDI request production, PCM synchronization of every
envelope and full native scheduling remain outstanding.

### Pre-release PCM synchronization

`SynchronizeVoicePcm` now composes the three PCM ramp synchronizations before
request consumption: PCM32, amplitude34, activity update, PCM36. Delayed and
finished voices skip the operation, preserving activity. The activity byte is
the amplitude high byte with ff mapped to fe. The additional PCM levels are
distinct from the second envelope's internal +20 level. `SynchronizePcmRamp`
shares the existing 34 behavior with 32/36, retaining half-scale conversion,
latch/read ordering and the ff00 direct-level-write branch.

768 paired H8/PCM cases cover all 24 channels, eight command combinations and
delay/active/release/finished stages; level fields, activity and both PCM RAM
banks match. ROM-free tests exercise all three reads followed by release,
confirming the freshly synchronized amplitude becomes the release start and
the pending flag is not consumed prematurely. All eight CTests pass. The trace
`/tmp/sc55-voice-sync-1.csv` matches `/tmp/sc55-voice-release-1.csv`.
Production scheduling and the second envelope's continuous calculation remain
unimplemented; these tests do not establish ROM-free playback.

### Second-envelope interpolation

The second-envelope state now advances a prepared-duration segment via
`advanceSegment(duration,ticks)`, matching 45b6..4662. It retains signed-word
endpoints, caps cross-sign distance at 32767, uses high-word interpolation,
and preserves H8's wrapped elapsed/deferred time. Duration<=8 snaps to target
without consuming deferred time; overflow snaps and carries excess ticks,
whereas exactly ffff progress still interpolates. The downstream step-time
word is retained (duration for <=8, otherwise8).

4,800 persistent H8 updates cover signed boundary endpoints and six durations,
with small ticks followed by a large final tick to cover both interpolation
and overflow. ROM-free tests distinguish exact completion from overflow,
cross-sign clipping and short-duration deferred-time preservation. All eight
CTests pass. Duration preparation, stage dispatch and processing after4662
remain to be connected; a segment helper is not a full second-envelope runner.

`SecondEnvelopeTiming::duration` now implements the three duration entries
44a3/44ff/4564..45b6 with owned time data. It reuses the verified controller
adjustment and two-stage scaling: attack control applies only with voiceA2
bit4, decay always applies, release selects its separate key scale, and
attack uses a different velocity scale from decay/release. `advanceInterval`
connects this to segment interpolation and rejects unsupported inputs before
state mutation.

65,536 H8 cases compare all 128 parameters and 128 controller values across
attack-disabled/enabled, decay and release, varying scales and comparing both
duration and subsequent segment state. All eight CTests pass. ROM-free tests
verify scale selection, attack gating and invalid-input preservation. Stage
dispatch and post-interpolation processing remain outstanding.

The second-envelope state's `advance` now composes 4443..4662, before
post-processing: voice65 bypass, completion dispatch through 7896, parameter
and target installation, timed attack/decay/release, and target holding at
stages10/22. Stage0 clears the separate cached PCM36 level and step-time word,
using the start value without clearing deferred ticks. Inactive stages14..20
skip output processing. Invalid stages/selected timing inputs preserve state.

1,536 persistent H8 updates cover all twelve even stages, completed/incomplete
starting progress and intermittent bypass. ROM-free tests cover the complete
short-duration stage sequence, hold, delayed PCM-level reset and transactional
failure after a prospective stage change. All eight CTests pass. The trace
`/tmp/sc55-second-runner-1.csv` matches `second-timing-1.csv`. Processing after
4662 and initial preparation are still missing; native instrument integration
and control-ROM-free playback have not been demonstrated.

`PrepareSecondEnvelopeOutput` now implements 4662..46e0 using caller-owned
inputs: base/controller adjustment, optional positive-controller suppression,
signed internal-level addition, signed offset, then two modulation sources.
`ApplySecondEnvelopeModulation` preserves the 47fb..4856 word arithmetic,
including the unusual two-negative-depth branch which uses only the first
depth magnitude, unlike pitch modulation. Waveform doubling wraps to 16 bits;
rounding precedes subtraction/overflow saturation. No ROM lookup or allocation
is used by these helpers. Base/controller values outside 0..127 are rejected.

16,384 H8 cases compare the entire output chain with varying internal levels,
offsets, controllers and both modulation sources. ROM-free regression cases
cover negative-depth asymmetry, modulation sign, depth cap, waveform wrap,
output clamps, controller gating and invalid inputs. All eight CTests pass.
`/tmp/sc55-second-output-1.csv` is byte-identical to `second-runner-1.csv`;
the full trace still executes H8 and is regression evidence only. Processing
from 46e0 onward, initial preparation, modulation producers, native scheduling
and control-ROM-free instrument playback remain outstanding.

The next portion, 46e0..478e, is implemented by
`AdvanceSecondEnvelopeControl` and `ConvertSecondEnvelopePcmLevel`. The former
moves the controller byte by one toward the adjusted request, applying the
previous PCM level's ceiling only on a changed request. The latter interpolates
the level curve, doubles with word wrap, enforces control >=8, and clamps by
the selected ceiling and 0xe600. Tables are caller-owned: 256 smoothing bytes,
129 curve words (including the interpolation successor), and 128 output bytes.
No executable bytes beyond the output table are needed. Non-musical control
inputs (>127) and levels outside the preceding output stage's 0..32767 domain
are rejected before mutation.

The oracle compares smoothing state and converted PCM level over 65,536 cases,
covering every valid level twice, all base/controller combinations, and varied
previous PCM levels. ROM-free cases exercise unchanged-request ceiling bypass,
ceiling indexing at word overflow, minimum control, interpolation rounding,
doubling wrap, output caps and invalid-input preservation. Ramp command
generation from 478e onward and data-asset export for these tables remain
unimplemented; these helpers do not yet drive the production instrument.

Validation: all eight CTests pass; `second-pcm-level-2.csv` is byte-identical
to `second-output-1.csv` (both in `/tmp`). This trace still runs H8 and does not
prove ROM-free playback or reduced host CPU load.

`EncodeSecondEnvelopePcmCommand` implements 478e..47fa, reusing the established
`EncodeCutoff`/`EncodeRate` arithmetic. It preserves hold-before-zero-time
dispatch, uses the original level delta for rate, rounds an increasing target
to the next byte only when its high byte did not change, and reapplies the
controller and 0xe600 ceilings. The cached PCM level is not rounded up.
Step times 0..8 select the same verified short-duration shift limits used by
the amplitude segment; larger step times are rejected (the dispatcher emits
at most eight). 589,824 H8 comparisons cover all nine times, equal levels,
one-unit rises and varied previous levels. Eight CTests pass, including
same-byte target advancement, ceiling reapplication and invalid arguments.

`SecondEnvelopePcmState::advance` composes the complete post-processing path
4662..47fa with caller-owned tables and inputs, committing output, controller,
cached PCM level and command only after validation. The oracle compares this
chain in the 16,384 modulation cases. This still needs connection to the
segment scheduler and PCM register writer; table export, input preparation,
and full control-ROM-free instrument execution remain outstanding.

The successful full trace `/tmp/sc55-second-pcm-chain-1.csv` is byte-identical
to `second-pcm-command-1.csv`, which matches `second-pcm-level-2.csv`.

`AdvanceSecondEnvelope` now joins the segment dispatcher and output chain
(4443..47fa), retaining separate segment ownership for release handling and
PCM state ownership for synchronization. Stage0 clears the previous PCM level
before conversion/command generation; bypass and inactive stages skip the
output chain. Invalid active inputs roll back both state objects, including
prospective stage changes and stage0's level reset.

The persistent dispatcher oracle now also executes the complete output chain
on every active update, varying modulation and controllers over all twelve
stages and bypass combinations (1,536 updates). Both cached PCM state and the
existing segment comparisons match H8. ROM-free tests cover rollback, bypass,
inactive-stage preservation and stage0 reset-before-command ordering. All
eight CTests pass. `/tmp/sc55-second-integrated-1.csv` matches the previous
`second-pcm-chain-1.csv` trace byte for byte. This is a native periodic
calculation, not yet the production scheduler or the PCM register handoff;
initial preparation, table export and ROM-free instrument playback remain.

The PCM handoff now accepts `SecondEnvelopePcmState` directly. The native
`SynchronizeVoicePcm` overload updates its level from PCM36 while preserving
output/control/command; a local legacy adapter avoids persistent duplicate
level ownership. The periodic `UpdateVoicePcm` overload takes command1a from
the same state and replaces PCM1c's high byte with its controller (voice68),
preserving the prepared low byte (voice66). Activation's separate pcm1a and
the prepared object are untouched. Existing first-envelope stage/channel gates
and ordered PCM I/O are retained.

768 paired H8/PCM synchronization cases and 768 periodic-write cases now use
these native-state handoffs over all 24 channels. PCM RAM and synchronized
levels match; ordered ROM-free tests verify control-byte replacement and
preservation, invalid-channel no-I/O and source-state ownership. All eight
CTests pass. `/tmp/sc55-second-handoff-1.csv` matches `second-integrated-1.csv`.
These are verified entry points, not a running ROM-free scheduler: the actual
product still executes H8, and table export/input preparation/native scheduling
remain required before instrument playback or CPU improvements can be claimed.

MD09 adds the second-envelope PCM tables after the MD08 pitch block and before
the sample bank: 256 smoothing-ceiling bytes, 129 big-endian curve words, then
128 output-ceiling bytes (642 bytes total). The independent table type lives in
`sc55_second_envelope_tables.h`; asset loading does not depend on voice lifecycle
code. `SoundData::secondEnvelope()` returns owned optional data. MD01..08 still
load with this getter absent; encoding MD09 requires the preceding tables.
Truncated imports preserve the previously loaded object.

The v1.21 exporter copies these bounded data ranges and verifies exact equality
after decoding. `/tmp/sc55-v121-melodic-9.sdata` is 112,609 bytes. The separate
sound-data test reads only this asset (no control ROM or MCU implementation)
and executes 32,768 second-envelope output updates, in addition to the existing
note/pitch/amplitude preparation checks. Synthetic round-trip, truncation,
dependency and downgrade tests pass along with all eight CTests. ROM-derived
assets remain outside the repository. This does not yet include second-envelope
initial preparation tables or prove an audible ROM-free instrument.

`InitializeSecondEnvelope` implements 4403..4435 AFTER target/timing input
preparation. It evaluates the start value with zero previous PCM level and
zero step time, captures the activation level and command (low byte forced
to ba), clears progress/deferred ticks, then performs one attack tick and its
output chain. Raw stage is preserved even for inactive stages; unlike normal
periodic dispatch this entry does not gate on stage. Caller elapsed ticks are
not modified. Failed validation commits neither segment nor PCM state.

96 H8 initialization cases are interleaved with the 1,536 persistent periodic
updates, checking activation fields, both output states, progress, preserved
stage and the H8 elapsed-clock/stack restoration. ROM-free tests cover the
zero-duration start/attack sequence and rollback after initial output preparation.
All eight CTests pass; `/tmp/sc55-second-initialize-1.csv` matches
`second-handoff-1.csv`. The preceding target/timing preparation (before 4403),
activation handoff and production scheduling are still incomplete; this is not
proof of control-ROM-free instrument playback.

`PrepareSecondEnvelopeTarget` implements the release-target calculation
410d..418c from a prepared signed start/depth, scale and raw parameter byte.
It uses the already-owned pitch curve at 79f2, truncates after each multiply,
and preserves branch-specific saturation (7fff/8001) and word wrap when
crossing zero. It does not replace those rules with a final signed clamp.
65,536 H8 comparisons vary all parameter bytes and word inputs; ROM-free
tests cover direction reversal, neutral parameter, both saturation limits and
zero crossings. All eight CTests pass and `/tmp/sc55-second-target-1.csv`
matches `second-initialize-1.csv`. Start/depth/scale production and installation
of the full target set remain separate work; no native instrument playback
or CPU improvement follows from this primitive alone.

`SecondEnvelopeReleaseState::installTargets` connects the common target
calculation to all five partial parameter bytes 2d..31, installing the current
target, three subsequent targets and the release target from a prepared start,
depth and scale. It also installs time bytes 32..36 masked to seven bits,
matching 43d1..4403. Current stage, level, progress/deferred ticks and step time
are preserved; initialization remains a separate entry.

4,096 H8 cases execute 3f13..418c and 43d1..4403, comparing the entire target
and time-parameter set against the native installation. ROM-free tests cover
parameter ordering, mask semantics and preservation of running state. All
eight CTests pass; `/tmp/sc55-second-install-1.csv` is byte-identical to
`second-target-1.csv`. The start/depth/scale producers preceding 3f13, initial
controller ceiling preparation and full native instrument integration remain.

`PrepareSecondEnvelopeTargetInputs` implements 3e98..3f16 from an already
selected key-table word, amplitude and partial parameters. Start sensitivity
uses bits 8..23 of the product, with direction determined by both the key's
4000 center and parameter's 64 center. Velocity sensitivity keeps the product's
low word before subtraction/optional negation; parameter64 retains depth7fff.
Scale is selected by raw byte2c. `SecondEnvelopeTargetTables` owns the three
bounded lookup arrays (74d2/74fc/7512); it is not yet exported in MD09.

65,536 H8 cases cover all combinations of the two sensitivity bytes with varied
key/amplitude/scale selectors. ROM-free tests check sign reversal, key center,
neutral velocity sensitivity and low-word overflow. All eight CTests pass;
`/tmp/sc55-second-inputs-1.csv` matches `second-install-1.csv`. Key-table lookup,
new-table asset export, initial controller ceiling and full native scheduling
remain outstanding. No control-ROM-free instrument playback is yet proven.

`SecondEnvelopeKeyTables` owns sixteen 256-word key curves selected through
the original ROM2 directory at 03:dcd2. Runtime selection uses the low nibble
of partial28 and the prepared key byte. `prepareTargets` now composes this
selection with start/depth/scale calculation and all five target/time-field
installations, preserving current progression state.

65,536 H8 cases cover every selector byte/key byte (including selector high
bits). Another 4,096 execute 3e79..418c plus 43d1..4403 end-to-end and compare
the complete installed target set. ROM-free tests exercise selector masking,
key255 and state preservation. Eight CTests pass; the full trace
`/tmp/sc55-second-key-targets-1.csv` matches `second-inputs-1.csv`. These key
curves and the target lookup tables still need data-asset export. Initial
controller ceiling, timing-scale preparation, bypass initialization and native
instrument scheduling remain incomplete.

`PrepareSecondEnvelopeInitialControl` implements 418c..4253: select the signed
maximum of start and all five targets, reuse neutral-output base/controller
adjustment, round up to the curve index, double with word wrap, then round up
again for the smoothing-ceiling lookup. The initial controller is assigned
directly from its adjusted base capped by this limit, without one-step
smoothing or the later minimum-eight adjustment. Modulation/offsets do not
participate in this initialization path.

65,536 H8 cases vary all six signed levels and the two controller paths,
including positive-controller suppression. ROM-free checks cover all-negative
targets, release maximum, round-up, high-end saturation, direct clamping and
invalid inputs. Eight CTests pass; `/tmp/sc55-second-initial-control-1.csv`
matches `second-key-targets-1.csv`. Timing-scale preparation, bypass setup,
target-table export and full native scheduler integration remain incomplete.

`PrepareSecondEnvelopeTiming` implements 425b..43d1, mapping partial39/3a
selectors and 3b/3c sensitivities to separate normal/release key scales, then
3e/3f sensitivities to attack and decay/release velocity scales. It reuses
`EnvelopeKeyScale` and `EnvelopeVelocityScale` with independent caller-owned
`SecondEnvelopeTimingKeyCurves` (ROM2 directories03:dcf2/03:dd12), not amplitude
or pitch curves. Selectors >=16 and sensitivities outside44..84 are rejected;
controller values and attack gating remain caller-managed neutral defaults.

167,936 H8 cases span all sixteen selectors, 256 prepared keys and 41 key
sensitivities, with varied velocity inputs/sensitivities. All four scale words
match. ROM-free tests cover independent curve selection, extreme sensitivities,
neutral controller defaults and invalid inputs. Eight CTests pass; the full
`/tmp/sc55-second-timing-prepare-1.csv` trace matches `second-initial-control-1.csv`.
The new timing curves still need data-asset export. Bypass setup and composing
the whole initialization path remain, as does native instrument scheduling.

`ConfigureSecondEnvelopeMode` implements the mode gate 3e3b..3e69/4435.
Modes0/1 continue normal preparation with PCM mode bytes1/3; every other byte
bypasses, clears PCM level/command and activation level/command, sets control64
and PCM mode3, and preserves the computed output. It does not own or reset
the segment state. All 256 raw mode bytes match H8. ROM-free tests cover both
active modes, bypass clearing and re-enabling without inventing new state.
Eight CTests pass; `/tmp/sc55-second-mode-1.csv` matches
`second-timing-prepare-1.csv`. Full initialization composition, remaining data
export and native instrument scheduling are still outstanding.

`SecondEnvelopeSetup::prepare` composes 3e3b..4435: mode gate, partial base
installation, key-derived targets, initial controller limit, four timing scales,
activation output and the first attack tick. It preserves caller-owned live
controllers, attack gating and modulation inputs. Active failure rolls back
setup/segment/PCM together; bypass skips validation of unused parameters and
preserves unrelated fields. Segment/PCM owners remain separate for release
and hardware synchronization, not duplicated inside the setup object.

4,096 H8 executions now compare this entire entry-to-exit path, varying modes,
key/amplitude, partials, offset and controller flags. Checks cover mode, base,
limit, activation words, PCM output/level/command/control and segment progress.
ROM-free tests cover rollback, bypass with otherwise invalid parameters and
re-enabling into initialization. All eight CTests pass; the full trace
`/tmp/sc55-second-full-setup-1.csv` matches `second-mode-1.csv`. This stops before
the voice handoff at4435; new-table export and production scheduling still
remain, and audible control-ROM-free instrument playback is not demonstrated.

MD10 adds `SecondEnvelopePreparationTables`: sixteen 256-word key tables,
192 start-sensitivity words, 192 velocity-sensitivity words, 256 scale words,
and two sets of sixteen 256-byte timing curves. The 17,664-byte block follows
MD09's PCM tables and precedes the sample bank. Words are big-endian; header
version parsing accepts exact decimal01..10 and rejects other encodings.
`secondPreparation()` is absent on older files. MD10 requires MD09 and its
dependencies; failed/truncated loads preserve the last valid asset.

The exporter verifies exact round-trip equality of all new tables. The
130,273-byte `/tmp/sc55-v121-melodic-10.sdata` passes the separate data-only test:
57,344 full second-envelope initializations (224 tones ×2 partials ×128 keys,
fixed prepared amplitude100, neutral live controllers/modulation), without
opening a control ROM or linking MCU execution, plus existing preparation
checks. Eight CTests pass, covering synthetic table round-trip, truncation,
unknown versions, dependency validation and downgrade. This demonstrates
data-only initialization, not audible playback, fidelity under live modulation,
or completed native instrument scheduling. ROM-derived files remain in `/tmp`.
# Second-envelope activation handoff

`PrepareVoiceSecondEnvelope` maps the initialized native second envelope into
the existing prepared-voice commit/post-enable owners. PCM1c combines current
control and prepared mode; PCM1a at activation uses the captured initial command,
while post-enable restores the periodic command after installing the captured
initial level. `ContinueVoiceSecondEnvelope` reflects only the stage changed by
activation, retaining the initialized segment level and progress.

Validation: 6,144 paired H8/PCM prepared-commit cases include the second-envelope
handoff; ROM-free envelope-pcm tests check distinct initial/periodic values,
preserved progress and invalid-stage rejection. The full oracle trace at
`/tmp/sc55-second-activation-handoff-1.csv` equals
`/tmp/sc55-second-full-setup-1.csv`. This is a native preparation connection,
not a production scheduler or a ROM-free audible synthesizer.

## Composed voice control with the real PCM pipeline

`--native-voice-control-pcm-test PATH_TO_MD13` loads only a data-only sound asset,
before any control ROM loader or H8 execution. It advances 24 prepared sustain
voices through `AdvanceVoiceControl` and `UpdateVoicePcm`, then clocks the real
integer PCM pipeline. It checks pitch/pan/send/envelope command register mapping,
adjacent-slot isolation, nonzero PCM amplitude/TVA levels, and zero MCU cycles/PC.
The fixture completes 1,536 voice updates and 8,196 output callbacks.

To include this local-asset test in CTest:

```sh
cmake -S tools/firmware-oracle -B /tmp/sc55-firmware-oracle-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSC55_SOUND_DATA_TEST_ASSET=/tmp/sc55-v121-melodic-13.sdata
cmake --build /tmp/sc55-firmware-oracle-build -j4
ctest --test-dir /tmp/sc55-firmware-oracle-build --output-on-failure
```

The asset is not distributed. This fixture uses synthetic sustain initialization
and cadence, no wave ROM, and no MIDI scheduler. Output callbacks do not imply
audible instrument output or firmware-equivalent audio. Production still needs
native note preparation, scheduling, waveform playback and end-to-end fidelity
validation; this check establishes the composed control-to-device connection.

The same test now also activates the first 24 eligible normal-sample partials
from the local MD13 patch set, using note 60 / velocity 100, neutral scale,
record level 127, and neutral controllers. It connects patch velocity selection,
sample addresses, key-scaled amplitude and second-envelope initialization,
prepared-voice commit/key-latch polling, continuation, periodic voice control,
and release requests through the real PCM implementation. All 24 must develop
nonzero amplitude and consume release. This extends the synthetic sustain check;
it does not remove its independent register-isolation coverage.

The input mapping follows 116f..117b: envelope key C8FC comes from original note
A1B2, not adjusted sample key A1B3/C98C. Amplitude uses the velocity amplitude
A1B9/C944; the second envelope uses the secondary curve A1BC/C95C (1283..128f).
Partial byte 08 supplies second-envelope control/attack gates. Pitch remains a
fixed sustain input, modulation remains a fixture, wave memory remains empty,
and the cadence is still synthetic. This is patch-to-envelope/device integration,
not a complete native note implementation or audio equivalence measurement.

## MD14 pitch-envelope timing and native pitch startup

MD14 adds 8,192 bytes after MD13's pan table: 16 attack curves followed by
16 release curves, each 256 bytes. Import reads bank-03 directories DD42/DD62,
with 16-bit bank-local address wrap. `SoundData::pitchTiming()` owns the result;
MD01..13 return null rather than substituting neutral curves. Encoding requires
the MD13 sections. Tests cover exact round-trip, truncation preserving the old
asset, missing dependencies, unknown version and downgrade. The local v1.21
export is 140,130 bytes; no derived asset is checked in.

With MD14, the real-PCM patch activation test replaces its fixed pitch with
sample/part base pitch, key-table and fine/random preparation, pitch-envelope
targets and all five timing increments, initial interpolation/modulation-to-PCM
conversion, then activation-stage continuation. The 24 selected partials all
use native pitch initialization. MD13 retains the explicitly reported pitch
fixture for compatibility. The data-only consumer also uses real timing curves
for its 38,362 sample-to-pitch cases when MD14 is supplied.

Use the previous commands with `/tmp/sc55-v121-melodic-14.sdata` for MD14.
This does not establish the firmware control clock, LFO preparation, waveform
audio fidelity or production ROM-free operation. Neutral note/part controls and
empty wave memory remain deliberate integration-test inputs.

### Patch LFO connection to real PCM

The patch-activation test now prepares both LFOs from owned patch data and
modulation tables. Inactive slots are marked stage22 before source selection,
so a zero-filled inactive voice cannot accidentally qualify as a sharing source.
Each test case still has one live voice, exercising the local initialization
route; cross-voice sharing is covered by the separate primitive tests only.

The second block is initialized before depth preparation, then first-block
initialization uses patch offsets0e..11 (`common[2..5]`, because the H8 pointer
includes the 12-byte name). Initial modulation outputs feed second-envelope and
pitch initialization. Each synthetic control interval updates the first block,
then the composed voice update advances the second block and applies both to
amplitude, second-envelope output and pitch. Both phase-advance counters must be
nonzero across the selected patches. Neutral part controllers and the fixture's
cadence remain explicit; this is not proof of production clock/interrupt ordering
or audible fidelity. No control ROM is loaded or executed by this test path.

The first 24 eligible v1.21 partials all have second-LFO rate zero, so expecting
both oscillators to advance on that selection was an incorrect test assumption.
The selection now reserves its final two slots for nonzero second-LFO rate
patches (v1.21 tone23). The first22 retain the disabled-rate coverage. No runtime
rate is forced or patched to make the phase check pass.

## Native control driving actual waveform ROMs

```sh
/tmp/sc55-firmware-oracle-build/sc55-firmware-oracle \
  --native-wave-output-test /tmp/sc55-v121-melodic-14.sdata \
  '/Users/ring2/Documents/roms/Synths/Roland SC-55/SC-55 v1.21'
```

This early CLI path opens only `sc55_waverom1.bin` through `sc55_waverom3.bin`
(exactly 1 MiB each), reuses the existing loader's unscrambling operation, and
copies them into the PCM engine before each isolated note case. Neither control
ROM is opened or loaded; MCU PC/cycles must remain zero. An optional CMake cache
path `SC55_WAVE_TEST_DIRECTORY` adds this check to CTest alongside the required
`SC55_SOUND_DATA_TEST_ASSET`.

The waveform check exposed two missing startup connections: zero pan gains
despite center position, and PCM3d's wave-bank selector. `SpatialState::initializePan`
implements 304c/3067..307d's resolved-position gain encoding, separately from
periodic smoothing. It matches 258 H8 cases, including frozen positions; full
pan selection/random choice is not claimed. The neutral center fixture installs
its gains before commit. PCM3d now uses boot-trace value b7 rather than just the
slot count17. PCM3c uses undithered output00, deliberately not firmware boot f8.

The pitch input now carries the selected adjusted key/sub-semitone fraction and
sample reference into initial and periodic conversion, instead of placeholder
tune1024/reference0. Part controllers and portamento inputs remain fixture values.

All24 selected partials produce varying stereo PCM output (range greater than
65536 in the engine's int32 output units). The otherwise identical empty-wave
check requires every output sample to remain exactly zero. This distinguishes
wave playback from a DC offset/noise-only signal. Both checks pass with the
existing eight core tests. It is not a subjective listening test, pitch/fidelity
comparison, native MIDI scheduler, or production ROM-free instrument. The outer
control cadence, complete startup state, effects setup and host integration
still need completion and validation.

## Recovered voice-task clock

`ControlClockProbe` observes the completed AC5A store at 5af5..5af9, avoiding
pre-instruction observations that can be interrupted/revisited. On the full
v1.21 soft-sample trace it sees task8, period8 kernel ticks, 51,370 dispatches:
51,345 consume one expiration, 24 consume two, and one consumes five. Actual
dispatch gaps range from6,900 to685,224 MCU cycles: the cooperative scheduler's
latency is not constant. The probe leaves the PCM trace byte-identical.

Timer configuration is checked on every observation: FRT2 TCR22, OCRA0138,
clear-on-A enabled, prescaler mask31. In this emulator TIMER_Clock uses MCU/2,
so a kernel tick is313*32*2=20,032 MCU cycles and a task period160,256 cycles.
This is a statement about the current emulation clock, not a hardware-frequency
measurement. The old80,000-cycle audio-test cadence was approximately twice as
fast and has been replaced.

`sc55::ControlTaskClock` accumulates these cycles without allocation, accepts
arbitrary block partitions, and hands off elapsed expiration counts. The kernel
increments a BYTE at03cd and sets a separate ready bit; TRAPA B02ee..02fe returns
and clears that byte and bit, followed by zero-extension at5af3. Accordingly,
256 pending expirations yield a ready event with value0, not no event and not
a saturated255.512 paired H8 tests cover this wrap and consumption. The isolated
CPU fixture enables internal RAM while testing kernel addresses, as normal MCU
reset does. Unit checks also cover partition invariance and exact boundaries.

The native patch/audio test advances an absolute PCM deadline per nominal
expiration, then supplies the consumed count to LFOs/envelopes/pitch. It does
not repeatedly add the period to PCM's rounded-up cycle counter, avoiding drift
from the chip pipeline's frame quantization. The epoch remains the end of each
test's activation, not reconstructed kernel startup. It currently dispatches
each expiration promptly: contention, multi-voice service timing and exact H8
task/interrupt latency remain unmodeled. Passing this test is not audio-fidelity
proof or completed production scheduling.

## Sample-specific amplitude and initial spatial controls

`PreparePartialAmplitude` ties amplitude setup to the selected normal sample's
descriptor byte0 (2ce6..2cf9), in addition to partial/key/velocity and common
level. The waveform test previously supplied a neutral127 instead. The helper
rejects absent/special samples, and the data-only consumer exercises38,362
sample plans against the existing envelope runner with descriptor-derived input.
The sample/key generation remains caller-owned; this helper does not allocate
a voice or resolve drum/special branches.

`SpatialState::initialize` covers2fcc..3080: direct initial send levels,
sequentially clamped part/tone/base/master pan, and the PCM random-pan path when
part pan is0 or the enabled tone pan scale is0. It validates controls before I/O,
reads the complete PCM random word but uses its high byte divided by2, and
stores the frozen sentinel separately from the encoded gains. Initial sends
are not the periodic one-step smoother.4096 H8 cases compare allthree output
words and native random-transaction counts; unit tests check unchanged state
and zero I/O on rejected controls. The H8 fixture explicitly supplies its PCM
device and restores the original pointer afterward.

The actual-wave test now supplies partial byte09 as base pan (2f54..2f57), uses
the initial pan/send words during commit, and retains these same inputs for
periodic output control. Part/master controls remain neutral fixture choices;
drum tone scaling is exercised by the primitive comparison, not by an end-to-end
native drum instrument. Full native note ownership/MIDI scheduling, effects
and production integration still require work.

## Initial TVA and key-on transfer

`TvaState::initialize` implements the normal new-voice branch (-3b bit7):
2c5d installs rampffff;3080..308d computes the level without multiplying by
the ramp and stores `(level & 0xff00) | 0xba` as the initial command.
This differs from periodic36db, which applies the ramp and usesb4/ff00.
It must not be used for the voice-reuse fade or every control tick.
`PrepareVoiceTva` transfers only the command to the activation batch's cache;
periodic output continues to read the live TVA owner directly.

The real-wave integration now initializes TVA after mapping both LFO outputs,
passes its command through the actual key-on batch, and checksPCM16 before the
first control tick. Part expression/volume/master remain fixture values100,
not yet complete native MIDI control state. Unit checks cover silence (00ba),
replacement of stale state, full initial ramp, key-on register mapping, and a
subsequent periodic hold without an artificial eight-tick ramp-up.
16,384 randomized H8 cases compare ramp/level/command at both initialization
and periodic update. This establishes the primitive and its connection, not
native-versus-H8 audio fidelity or a host CPU reduction.
Validation: all10 CTests and the MD14 data-only consumer pass. Full H8 regression
`/tmp/sc55-native-initial-tva-1.csv` is byte-identical to
`/tmp/sc55-native-initial-spatial-2.csv`:13,314,400 frames,923,053 PCM writes.

## Native normal pitch-start composition

`PrepareNormalVoicePitch` moves the sample-descriptor pitch, key correction,
random pitch, glide setup, pitch-envelope targets/timing and first interpolation
step from the integration fixture into `sc55_note_setup.h`. It consumes owned
MD14 tables and explicit note/control inputs, not ROM addresses or CPU state.
Part key, envelope key and fractional tuning remain separate. The previous
per-voice prepared pitch/glide is supplied explicitly and is never modified;
the result contains a preparation snapshot and the live pitch runner. A future
note owner must use the current runner's glide, not a stale preparation snapshot.
This covers the normal initialization branch, not the 4f51 reuse/re-entry path.

The data-only test checks38,362 normal sample plans with nonzero previous
pitch/glide/correction state, four flag combinations and source60/255. Results,
PCM transaction counts, and16 subsequent control updates match the existing
primitive composition. Invalid partials, special sample IDs and out-of-domain
glide rates are rejected before PCM I/O. This composition test is not an
independent H8 oracle; its underlying primitives have the separate H8 checks.

`ContinueVoiceControlAfterStart` transfers one activation batch result to all
three native envelope owners and the lifecycle snapshot. Validation precedes
all writes to that state, so an invalid pitch stage cannot partially restart
the amplitude envelope. The real-PCM integration uses this adapter and checks
that pitch progress and glide increment survive the transfer.

All10 CTests, the MD14 data-only consumer and the MD13 legacy integration pass.
The H8 regression `/tmp/sc55-native-normal-pitch-start-1.csv` equals
`/tmp/sc55-native-initial-tva-1.csv` byte for byte. The actual-wave fixture now
calls the backend pitch-start composition; its note selection, portamento
flags/source and most part controls are still fixtures. A complete native
MIDI/voice owner, effects, drums, startup and host integration remain unfinished.

## MIDI channel output-control connection

`ApplyChannelOutputControls` maps the established v1.21 CC fields into the
native level/spatial inputs: CC7 volume, CC11 expression, CC10 pan and CC91/93
sends. `LevelInputs::velocity` is a legacy name for part+08 (CC7), not note-on
velocity; note velocity remains in envelope preparation. Master values, tone
scales, LFO contributions and partial base pan are preserved. The caller must
resolve MIDI-channel-to-part routing and refresh the mapping each control step.
This helper does not implement GS part routing or the remaining controllers.

The actual-PCM integration now feeds fragmented MIDI CC messages through
MidiDecoder/ChannelControls before key-on and during playback. CC11=0 at tick64
produces exactly768 zero-level TVA updates across24 partials before restoration
at tick96. CC10=0 goes through the firmware-compatible clamp to1, not GS random
pan. At tick128 CC91/93 targets64/32 begin from0101 and reach4020 after64 updates,
exercising the existing one-unit send smoother. This checks control state, not
instantaneous audible silence or implemented reverb/chorus audio algorithms.
The ROM-free MIDI test covers2048 value/channel combinations, packet splitting,
running status, channel isolation and preservation of non-channel fields.

The full H8 regression `/tmp/sc55-native-midi-output-1.csv` is byte-identical to
`/tmp/sc55-native-normal-pitch-start-1.csv` (923,053 writes). Native note allocation,
program/bank dispatch, master/GS controls, effects and production integration
are still incomplete; this is no longer a fixed output-control fixture, but it
is not yet a playable native instrument.

## MIDI program/note-to-velocity dispatch

`PrepareMelodicNoteVelocity` connects a decoded nonzero Note On and the routed
channel's Program Change/soft-pedal state to the v1.21 capital-tone map and
whole-patch velocity calculation. It returns owned scalar/partial results,
not a patch pointer. An effective bank and rhythm flag are explicit caller
inputs: this helper does not guess GS routing, latch bank-select messages or
implement variation-bank fallback. Note Off, zero-velocity Note On, malformed
data, rhythm parts, unsupported banks and missing assets return no plan.
A successful plan with zero velocity candidates remains distinguishable from
those failures. Sample existence/high-note remapping/key transposition and
physical voice allocation are later stages, not hidden substitutions here.

The sound-data test feeds fragmented Program Change, CC67 and Note On messages
for128 programs,127 nonzero velocities and both soft-pedal states (32,512 cases),
varying the shared starting accumulator and checking both partial results
against the existing arithmetic. Negative cases cover routing/data/message
rejection. These are composition tests; the capital map and velocity arithmetic
have their separate H8 comparisons.

The actual-PCM test now selects22 partials through MIDI program/note dispatch,
then uses the decoded note/velocity for sample and pitch preparation. Two
explicit variation-tone23 fixtures retain second-LFO coverage; they are not
presented as a bank-selection implementation. All24 actual-wave outputs vary,
and all24 empty-wave outputs remain exactly zero. Existing CC and release
checks still pass. The H8 regression `/tmp/sc55-native-note-dispatch-1.csv`
is byte-identical to `/tmp/sc55-native-midi-output-1.csv` (923,053 writes).
Native allocation/stealing, note-off ownership, bank/GS/drum handling, effects,
and production host integration are still unfinished.

## Allocator-selected PCM slots and release publication

The actual-PCM integration no longer chooses `channel = started`. It reserves
one slot through `VoiceAllocator::createGroup` for each isolated partial run,
uses the returned slot throughout sample/oscillator/envelope/key-on/control
work, verifies all24 slots were selected exactly once, checks pool exhaustion,
then returns the reservations and verifies24 free voices and zero per-part
counts. These are24 isolated PCM runs, NOT simultaneous polyphonic playback.
Each run still requests one voice; whole-note two-partial allocation remains
to be connected. The two opaque group metadata bytes are still fixture inputs.

The first version failed the final free-count check. A one-slot reproduction
showed why: `createGroup` reserves/attaches but deliberately leaves status94;
`returnVoice` therefore treats it as already free. Firmware113a clears the
status in the later preparation phase. The integration now performs that
separate phase instead of changing the H8-verified allocation primitive.
The minimal regression preserves the distinction between reservation and use.

`VoiceAllocator::publishReleaseRequests` implements1e04..1e1a: copy A3E0 into
the per-voice pending request in descending slot order, including zero, without
clearing the source.256 H8 batches/6144 slot writes match. ROM-free checks cover
ordering, overwriting stale nonzero requests, preservation of source/activity,
and the all-zero publication. This must run at the note-management boundary,
not automatically every control tick (which would republish consumed requests).
The PCM test uses it for the release request, but the producer that matches a
MIDI note-off to A3E0 is still a fixture and has not been presented as complete.

All10 CTests pass after fixing the missing preparation-phase status update.
The H8 trace `/tmp/sc55-native-allocated-output-1.csv` matches
`/tmp/sc55-native-note-dispatch-1.csv` byte for byte (923,053 writes). Native
MIDI note matching/sustain, shared two-partial ownership, runtime retirement,
stealing under concurrent playback, effects and host integration remain.

## First-match native note-off selection

`VoiceAllocator::requestNoteRelease` implements155a..1597 plus1633..1687.
It walks only the routed part's group chain, selecting the first group with
status0, matching note, optional A2D0 selector match (selector0 bypasses), and
A300 bit0 enabled. Selection changes group status to2. A230 bit0 defers release
via A288 bit0; otherwise the supplied16-byte A090 retained-key row is searched
until its first high-bit sentinel. A retained note produces no voice request.
The remaining path sets A360=1 and A3E0=ff for the tail and optional predecessor.
It does not release every same-pitch group or walk an unbounded voice chain.
Bad part/group/tail/predecessor references and cycles fail without mutating
the native state. This safety rejection is beyond the firmware's trusted-RAM
contract, not a claim of matching arbitrary corrupt firmware memory accesses.

`RequestMelodicNoteOff` routes explicit Note Off and zero-velocity Note On to
that operation. It does not publish pending requests itself. The real-PCM test
now enables the group gate, feeds fragmented MIDI note-offs, checks that a
different note has no effect, then publishes the actual selection flags before
control processing. This replaces the former direct A3E0=1 fixture.
Routing/selector, retention rows, the A300 gate producer and pedal release
handling still need the complete native part owner; they remain explicit
inputs rather than guessed MIDI semantics.

2,048 H8 fixtures (320 matching, including deferred/retained paths) compare
all allocator tables after the routine. ROM-free regressions exercise duplicate
notes, two-voice groups, selector bypass/mismatch, both note-off encodings,
retained-key termination, deferred-bit preservation and invalid-input rollback.
All10 CTests, the MD13 PCM integration and MD14 data-only consumer pass.
The intermediate H8 trace `/tmp/sc55-native-note-off-1.csv` equals
`/tmp/sc55-native-allocated-output-1.csv` byte for byte.
The live observer additionally snapshots the real call at155a and compares
all allocator fields at1597:3,329 live calls match. Its final trace
`/tmp/sc55-native-note-off-live-1.csv` is also byte-identical to
`/tmp/sc55-native-allocated-output-1.csv` (923,053 writes). Neither this parallel
oracle nor the isolated PCM tests establish complete native polyphonic playback.

## Part hold-off and deferred releases

`VoiceAllocator::setPartHold` implements16cd..16d1 and16d2..1736. Enabling
sets only A230 bit0. Disabling an already-clear part is a no-op; otherwise it
clears that bit, walks every linked group, clears each A288 deferred bit and
processes previously deferred groups regardless of their current group status.
The supplied A090 retained-key row can still suppress a group's request.
Other groups set A360=1/A3E0=ff on the tail and optional predecessor. Source
flags are published to the control owners separately, as with ordinary note-off.
Validation failures roll back all changes, including earlier group flag clears.

2,048 paired H8 hold-off/on cases compare every allocator field with varied
part flags, deferred bits, group statuses, one/two-voice groups and retention
rows. ROM-free tests chain hold-on, multiple note-offs and hold-off; they check
retained notes, unrelated flag preservation, repeated off as a no-op, and late
invalid-link/cycle rollback. The real-PCM test runs12 of its24 isolated partial
cases through a deferred note-off at tick252 and hold-off at256. The remaining
12 use ordinary note-off at256; all24 requests are consumed as before.

All10 CTests pass, including actual-wave and empty-wave checks. The MIDI pedal
controller's routing/receive rules and retained-key list producer are not yet
connected: the test invokes the native part-hold operation directly. This is
not yet a complete sustain/sostenuto implementation or simultaneous polyphonic
native instrument.
The full H8 regression `/tmp/sc55-native-part-hold-1.csv` is byte-identical to
`/tmp/sc55-native-note-off-live-1.csv` (923,053 writes); the MD13 PCM and MD14
data-only consumers also pass.

## Routed MIDI Hold1 (CC64)

`ApplyRoutedHoldController` connects decoded CC64 to the native part-hold
owner. Values0..63 release and64..127 hold; an explicitly disabled receiver
consumes the message without state changes. The destination part and receive
permission are supplied by routing, not inferred from the status channel.
Other controllers/malformed events/invalid parts are rejected; pending-request
publication remains with the note-management batch. ChannelControls still
returns false for CC64 so the caller can dispatch it to this voice-aware owner.

`--hold-probes` is a separate v1.21 hardware-reference scenario: after boot,
send0,1,63,64,65,127,127,0 on each MIDI channel, compare all16 part flags after
each message and report128 matches. This verifies default GS routing (channel9
to part0, channels0..8 to parts1..9, other channels unchanged), not arbitrary
GS reassignment or the firmware's receive-mask address handling. Native code
does not hard-code this default map. The observer leaves its reference trace
unchanged: `/tmp/sc55-midi-hold-routing-2.csv` equals the original observation
`/tmp/sc55-midi-hold-routing-1.csv` byte for byte.

ROM-free tests cover4096 combinations of destination part/value/receive enable,
deliberately using a different source MIDI channel, with fragmented messages,
other-part isolation and non-hold-bit preservation. The actual-PCM integration
now sends CC64=127 before the deferred note-off and CC64=0 to release it,
replacing direct part-hold calls in all12 held cases. All10 CTests pass.
The retained-key list producer, receive-mask/GS routing owner and CC66 remain
unfinished; this is not a complete pedal/part-management subsystem.
The full H8 trace `/tmp/sc55-native-midi-hold-1.csv` equals
`/tmp/sc55-native-part-hold-1.csv` byte for byte (923,053 writes). MD13's legacy
PCM integration also passes; no production plug-in CPU claim is made.

## Live partial-dispatch comparison

### Voice installation (113a..11cf)

The real-patch wave fixture now retains each partial's actual PRE-commit DSP
state, sample address, LFO state, activation entry and per-voice inputs. After
the isolated tests it replays these24 prepared states onto ONE freshly reset
PCM, loading only the three waveform ROMs. Each voice uses the normal native
`PreparedVoiceBatch` key-latch path, preserving earlier enabled slots, then
`ContinueVoiceControlAfterStart`. `VoiceControlRuntime` performs all periodic
updates on the shared PCM; no per-voice PCM reset occurs during this run.

The original concurrency run completed256 nominal-clock passes and65888 audio frames. All24 slots
are updated in the first pass; at least one pass has all24 PCM amplitude levels
nonzero simultaneously. Every published pitch register matches its owning
voice. The waveform-ROM run has varying summed audio; the empty-wave negative
control is exactly silent. All10 CTests pass and MCU instruction cycles remain
zero. This is concurrent actual-patch/sample rendering from captured prepared
state, not yet concurrent MIDI note startup, paired real-patch preparation,
reallocation coverage, waveform equivalence or product integration.

The concurrency run now also maintains an independent `PartNoteState` allocator
with the same24 allocated note groups as the prepared replay, rather than
reusing the isolated tests' already-released group state. Byte-fragmented MIDI
CC64 holds all parts at tick120; note-offs at128 remain deferred; CC64 off at144
creates24 release requests. `VoiceControlRuntime::publishNoteReleases` transfers
the complete allocator snapshot to existing voice owners at event boundaries.
It rejects nonzero requests for missing owners before any mutation and rejects
publication after a runtime failure. Zero requests for absent slots are safe.
Do not publish every DSP tick: consumed requests must not be continually reset.

All24 deferred requests are delivered and consumed during512 shared-PCM passes
(131680 frames); all24 simultaneous nonzero levels and waveform/empty-wave
checks still pass. All10 CTests pass. This proves MIDI release delivery and
consumption, not that every envelope tail is finished, every slot is reclaimed,
or that concurrent MIDI note-on preparation/product integration is complete.

`VoiceControlRuntime` (`sc55_voice_runtime.h`) now owns24 optional prepared
physical-voice states, per-slot control/first-LFO inputs, both modulation arrays,
source links and computed controller outputs. Its bounded `advance` connects
`PeriodicVoiceUpdatePass` to real controller refresh, first modulation,
unconditional paired first-modulation transfer, voice DSP and PCM writes.
Stage changes are republished to the sharing/selection arrays during the pass.
The returned mask and per-slot result/write records identify processed slots.
Missing selected owners reject before their DSP/device calls. A failure latches
the runtime to prevent replay; the caller must reconstruct/reprepare after
recovery. Installation/MIDI/PCM access must stay serialized with this owner.

The prepared-sustain PCM fixture now uses this runtime for24 concurrent slots
in12 reciprocal pairs, across64 passes (1536 voice updates, ticks0..4). It checks
every slot's pitch/pan/effects/TVA/envelope registers, copied first-LFO phase and
retained destination rate setting. Tests also cover an empty runtime, missing
pair owner and latched failure without PCM access. This is concurrent prepared
DSP state, not yet concurrent real-patch/sample audio or complete native note
activation. The separate24 isolated waveform cases still exercise actual
patch/sample preparation; combining those lifetimes remains required.
All10 CTests pass. The waveform run still reports24/24 varying outputs,
72 pitch-counterfactual differences and7179 isolated real-DSP passes, without
H8 instruction execution. Product control-ROM independence remains incomplete.

`PeriodicVoiceUpdatePass` owns the serialized5b11..5c1d iteration cursor and
24 visited bytes. A step selects a single/pair, invokes controller preparation,
first modulation, optional paired first-modulation transfer, then ALL selected
voice updates before ANY final PCM writes. Each update marks visited255 before
its stage gate (the3190 instruction is verified directly). The originally
scanned slot determines the next cursor. Pass completion clears all visited
bytes; repeated completion calls have no side effects until explicit reset.
A failed delegate poisons the pass rather than replaying partially advanced DSP
or PCM writes. The owning runtime must recover/reset its state before restarting.
Callbacks must be bounded, serialized and non-reentrant; voice lifetimes remain
the caller's responsibility. Interrupt latency and top-level task timing are
not represented by this pass runner.

256 full H8 orchestration traces compare delegate order across24 slots with
single/pair/self-link cases and inactive stages. DSP delegates are stubbed in
this particular test (3188's visited write is retained); this proves scheduling
order, not combined DSP equivalence. Native tests cover exact paired ordering,
visited timing, completion clearing and no replay after a late write failure.
Connecting this pass runner to every native DSP owner, then simultaneous
multi-voice rendering and product integration, remains required.
All10 CTests pass. The complete regression `/tmp/sc55-periodic-pass-1.csv`
matches `/tmp/sc55-paired-first-lfo-1.csv` byte for byte (923053 PCM writes).

The isolated-partial native wave path now executes its real DSP through
`PeriodicVoiceUpdatePass`: selected controller refresh, first LFO update,
`AdvanceVoiceControl` (second LFO/envelopes/pitch/TVA/spatial), then final
`UpdateVoicePcm`. Each pass is driven by the existing nominal control clock,
completed before restarting, and checked for cleared visited flags. Controller
refresh now occurs on every pass; the old event-only manual refresh is removed.
Termination is a nonlocal exit:33e7..33f1 discards3188's return address and
resumes the descending scan at5b5c. `UpdateResult::skipRemaining` preserves
that behavior: neither remaining paired DSP nor final PCM writes execute.
There are256 normal orchestration fixtures plus256 executing this real H8 tail
for selected stubbed DSP exits. Before the fix the latter fail with `Periodic delegate order
differs from H8`; native tests also cover termination in either pair member.
That change corrected pass ordering; the termination integration below adds
the previously missing device/allocator side effects.
Validation: all10 CTests pass; the full `--soft-sample-probes` run completes
with13,314,400 frames and923,053 writes. `/tmp/sc55-termination-512.csv` is
byte-identical to `/tmp/sc55-periodic-pass-1.csv` (the H8 baseline is unchanged).

`VoiceControlRuntime` now takes the mutable allocator (including its PCM links
and activity), and completes two distinct termination paths:

- Stop stages0e/10: `PollEnvelopeTermination` reads PCM32/34 respectively;
  only exact raw zero permits termination. Nonzero activity uses the firmware's
  wrapping word-doubling/high-byte conversion, with255 capped to254.
- Natural envelope completion at3472: `VoiceControlResult::finished` invokes
  `FinishEnvelopeTermination` directly, without polling. It clears activity,
  sets stage22, detaches PCM links, writes18=00b6 and notifies the allocator.

Both paths use `returnVoice` for the07d0..07e6 notification consumer. The runtime
serializes that event inline; firmware task-switch latency is not modeled.
The natural path previously lost its notification by conflating it with the
already-stopped stage gate. A new24-voice release-tail test failed before this
fix. The concurrent fixture also now performs installation's active-status
publication: `createGroup` alone intentionally does not mark slots active.

H8 comparisons cover131,120 poll cases (all16-bit levels on both selected
registers plus zero on every channel),24 natural exits with nonzero hardware
levels, and the existing return-bookkeeping matrix now through the complete
notification consumer rather than only1cbd. Native real-patch playback returns
all24 slots after189 additional tail passes beyond the512-pass MIDI fixture.
It then reallocates all24 without duplicate slots. This is allocator reuse,
extended by the second-generation playback check below; full MIDI note-on
startup is still incomplete.
The full `--soft-sample-probes` run `/tmp/sc55-termination-natural-1.csv`
is byte-identical to `/tmp/sc55-periodic-pass-1.csv` (13,314,400 frames,
923,053 writes); these new native paths do not alter the H8 production baseline.

The native real-patch fixture runs a second generation after those24 slots
have returned. The earlier prepared-DSP snapshot replay has been removed.
It retains the actual PCM instance, allocator, clock and key-mask state. Each
new allocator-selected slot goes through `RestartAndInstallVoice`, pending-task
dispatch, reuse readiness, key removal/programming/re-enable and post-enable
continuation. `PrepareNormalVoiceDsp` now recomputes amplitude, spatial controls,
both modulation blocks, TVA, second envelope and pitch from explicit note/part
inputs and MD14 data. It sees the live modulation peers and previous pitch/glide
state; it does not invent GS defaults, run H8, allocate or advance PCM time.
Initial prepared-stage/level/PCM/phase outputs are compared with the original
24 isolated preparations. Invalid slot, sample-plan mismatch and non-restarted
inputs are rejected without device I/O. Later failures may have side effects
and are not automatically retryable.

Startup-order correction:55f2 computes controllers before37fc/3891 and2a66.
The composed preparation had applied rate modifiers only after the LFOs'
initial one-tick update. The focused native regression failed with `Startup
LFO missed controller rate on its first tick`. Controllers now precede both
initializers;37fc depths precede second-LFO initialization, and first-LFO
initialization precedes second-LFO/spatial work. Controllers are not reapplied
after a shared initializer, which would overwrite its copied source rate.
The test covers nonzero initial controller rates for both LFOs. The H8 oracle's
existing4096 first-LFO cases cover preloaded rate modifiers, and its full trace
`/tmp/sc55-startup-order-1.csv` matches `/tmp/sc55-periodic-pass-1.csv` exactly.

Preparation also publishes sharing identities instead of leaving every voice
at zero: first LFO uses(part,tone), second LFO uses(part,tone,partial). Owned
IDs replace H8 bank/pointer identity, not PCM addresses. Native checks accept
a same-identity peer and reject another part, tone or partial. Both identities
refer to the caller's single sound-data generation.

Paired startup at5542..559e performs metadata for both voices,
first-controller computation plus second-controller copy, depths for both,
first-LFO initialization for the first voice plus3d1a transfer to the second,
then56b5 DSP preparation for each. It cannot be implemented faithfully by
calling the complete single-voice preparer twice. `PrepareNormalVoicesDsp` now
executes these phases for one or two dispatcher-ordered entries; the old
single-voice API delegates to that same implementation. Both records are
validated before mutation/I/O. Duplicate slots and a bad second sample plan
are rejected without changing the first voice or touching PCM.

Prepared voices retain their pre-DSP stage during first-LFO source selection,
so another pending member is not mistaken for an already running source. The
second member takes the full3d1a shared initialization (including timing and
sharing metadata), preserving its old rate index, rather than the periodic
3d44 transfer. Then each member's second LFO, amplitude/spatial/TVA, second
envelope and pitch are prepared. The first controller snapshot supplies both
members as5c20/5ff5 requires; copied source rates are not overwritten afterward.

The native wave fixture now scans all128 capital programs and exercises every
program selecting two partials atvelocity100:36 programs in v1.21. For each,
MIDI keys60..71 produce24 allocator-selected voices (432 notes/864 voices total).
Both partials are installed, dispatched
in actual link order, DSP-prepared together and activated in one two-entry
PCM batch. The shared first phase and preserved destination rate index are
checked. Every program reaches24 simultaneously nonzero voice level gates and
returns all24 voices after note-off. Waveform output varies for each program;
the empty-wave negative control stays silent. The PCM, key mask, clock and
returned allocator tables survive from one program to the next rather than
being reset. `/tmp/sc55-paired-sweep-1.log` records the direct waveform run.
This covers paired capital tones at this velocity/key range, not other
velocities, variation/drum banks, mixed restart/re-entry modes, full firmware
task timing, host integration or waveform equivalence.

Second-generation notes are byte-decoded Program Change/Note On messages at
key64 (first generation was key60). The22 capital-bank fixtures recalculate
tone/velocity selection; two variation fixtures retain their explicit tone
selection. Each then reruns sample selection and native DSP preparation, not
a stored sample plan or prepared envelope. This remains a selected single
partial per note, not full candidate-count allocation/two-partial preparation,
mono/re-entry preparation, firmware task timing or the product MIDI entry point.

All24 reactivated voices publish real PCM pitch and both nonzero level gates.
A fresh byte-decoded MIDI note-off set at144 triggers another complete release
and all24 slots return in701 passes. The waveform-ROM run produces varying
audio; the empty-wave run stays exactly silent. Audio measurement is reset
after32 control passes to exclude activation-time residual output. PCM itself
is not reset. All10 CTests pass, including both generations.

The MD14 plus waveform-ROM test completes7179 passes across24 isolated partials,
with7156 continuing updates,72 changed PCM pitch counterfactuals,768 muted
updates,12 hold releases and6 sostenuto releases. All24 wave outputs vary;
the empty-wave negative control stays silent. No control-ROM instructions
execute, and all10 CTests pass. This connects the single-live-voice DSP path,
not the pair callback (which deliberately rejects in this isolated fixture).
That isolated check alone does not validate simultaneous voices or paired
ownership (covered separately above). Task latency and product integration
remain outstanding; these results do not prove audio equivalence
or CPU savings versus the original firmware.

`SelectNextVoiceUpdate` implements the separate periodic selection at5b11,
stopping at the single (5b73), paired (5bb4), or completed-pass (5942) boundary.
It scans downward, skipping stage>=18 and nonzero voice[-26] visited flags.
CAC4 has precedence: a first link yields [partner,current]; otherwise CADC
yields [current,partner], and no link yields a single voice. The partner is
not independently filtered by its stage/visited flag, and self links are not
deduplicated. The resume cursor follows the originally scanned slot, not the
possibly swapped first member. Native255 means resume after slot0 and complete
the pass. Exhaustion clears all24 visited bytes, including slots above the
starting cursor. Invalid cursors/selected links reject without mutation.

The caller must still execute the ordered delegates:5c20 then (for pairs)5ff5
computes/copies controller words,3985/3d44 updates first modulation,3188 updates
voices,5855 writes PCM. Selection itself does not set visited flags or derive
timer cadence. Thus it is not yet a complete native scheduler. The initial
stage comparison at5b19 is misaligned in the linear listing; tests execute real
ROM instructions rather than relying on that decoded text.

2048 H8 comparisons cover start cursors (including resume after slot0), stage
and visited gates, both link directions/precedence, pair order, resume position
and all visited bytes. Native tests include malformed selected links versus
ignored lower-priority links, self pairing, invalid cursor and end-of-pass.
All10 CTests pass. The full regression `/tmp/sc55-periodic-selector-2.csv`
matches `/tmp/sc55-pitch-bend-1.csv` byte for byte (923053 PCM writes).

`RefreshSelectedVoiceControllers` composes the controller phase after selection:
5b73..5b89 for a single voice,5bb4..5bdb for a pair. Installed part and original
key of the first selected slot supply the current part/key controller input.
Its eleven computed words are stored in the24-slot native controller owner and
copied to the second selected slot, without recalculating from partner metadata.
It validates all selected slot indices and the source part/key before mutation;
empty selections are no-ops. It does not touch LFO/envelope state, visited flags
or the cursor. Those belong to the remaining ordered update phases.

1024 complete H8 phase comparisons cover single/pair/self-pair cases with
varying source and partner part/key identities and controller inputs. Native
tests additionally verify preservation on malformed source/destination input,
empty selection and ignoring partner metadata for controller calculation.
The native wave fixture now goes through periodic selection and this owned
controller phase for its72 live bend refreshes, retaining its real-PCM pitch
and counterfactual checks. All10 CTests pass. Timing and multi-voice DSP update
ordering are still unfinished; this is not product control-ROM independence.
The full regression `/tmp/sc55-selected-controllers-1.csv` matches
`/tmp/sc55-periodic-selector-2.csv` byte for byte (923053 PCM writes).

`PartControllerState` now owns16 per-part128-key pressure rows,11 sensitivities
and five contribution rows. `receivePolyPressure` implements2218..223b: An
messages update the addressed key in every part whose receive channel matches
and whose receive bit10 is set. Note-routing gates do not apply. Input bounds
are checked before mutation; disabled/unmatched parts and other keys survive.
`inputs(part,originalKey)` assembles the existing controller calculator input
without emulated memory access. Storage defaults are not a GS reset image.

2048 paired H8 routing cases compare all2048 pressure bytes for each event,
covering every channel/key with matched/mismatched and enabled/disabled parts.
Native tests cover running status fragmented byte-by-byte, fan-out, pressure
zero, disabled receivers, invalid input, and a nonzero pressure-to-pitch-offset
calculation. The data-only wave test now routes actual An messages through this
owner before controller preparation. Initially these were neutral fixtures;
the nonneutral native composition test described below now exercises them.
GS input-table configuration and reset behavior remain
unfinished. All10 CTests pass; `/tmp/sc55-poly-pressure-1.csv` matches
`/tmp/sc55-controller-preparation-live-3.csv` byte for byte (923053 PCM writes).

Channel pressure now has its own native producer: `receiveChannelPressure`
implements2346..2378 (Dn, channel match, receive bit13), updating contribution
row2 without applying note or CC gates. The owner stores five compact source
sensitivity rows; these are explicit configuration, not inferred GS defaults.
`PrepareControllerContribution` implements2666..2689 and its28a8/28c6/28e8
delegates: centered pitch multiplication, centered half products, unsigned
quarter products, and negative magnitude truncation before sign restoration.
The unused fourth parameter byte is omitted from the compact representation.

2048 H8 comparisons execute the complete routing and arithmetic, checking all
16 parts and all five eleven-word contribution rows, including untouched rows.
Native tests additionally cover byte-fragmented running status, fan-out,
disabled receivers, invalid messages, zero release and pressure-to-pitch input
composition. All10 CTests pass. `/tmp/sc55-channel-pressure-1.csv` matches
`/tmp/sc55-poly-pressure-1.csv` byte for byte. These are primitive/trace regression
checks, not proof of host integration, complete native synthesis or CPU savings.

`receiveControlContributions` adds the Bn contribution side: CC1 at25ac..25dd
requires channel match and receive bits11+1; the subsequent22e9..22f6 dispatch
updates the two assignable rows for CC numbers below121, requiring bit11 only.
The part owner stores the two assignment numbers (+26/+27) and the existing
five compact sensitivity rows. A CC1 event can update modulation and both
assignments; disabling modulation reception does not disable assignments.
This method is not a complete CC handler: scalar controls, pedals, RPN and
channel-mode side effects must still be dispatched by their respective owners.

Important v1.21 behavior: the second assignment's mismatch branch at2646 goes
to2623, the first assignment loop's decrement. After an enabled/channel-matched
part has a mismatched second assignment number, lower parts resume the FIRST
assignment scan. Channel/receive-gated skips do not switch scans. Native code
preserves this behavior, verified by executing the original instructions rather
than correcting the apparent firmware bug. The first scan already updated
matching first-assignment rows, so repeating those writes is idempotent here.

2048 comparisons execute CC1 (when selected) and the complete auxiliary dispatch
through22f6, with all five eleven-word rows of all16 parts compared. Native
tests lock down the mismatch transition, overlapping CC1 assignments, separate
modulation/CC gates, zero values, fragmented running status, invalid values and
the CC121 exclusion. All10 CTests pass. This does not implement GS assignment
configuration/reset or establish a CPU-load improvement in the product.
The complete soft-sample regression `/tmp/sc55-cc-contributions-1.csv` matches
`/tmp/sc55-channel-pressure-1.csv` byte for byte (923053 PCM writes).

`receivePitchBend` implements2390..23f7 and the2401/2441/2481 arithmetic,
without control-ROM reads. En LSB/MSB assemble a fourteen-bit value; zero is
clamped to one before subtracting8192. Matching parts require receive bit14,
independently of note/CC reception. Only contribution row1 changes. The absolute
bend is multiplied by4 and the sensitivity magnitude; the product loses its
lowest byte before the second multiplication. The returned high word uses
coefficients fe16 (pitch),7f00 (centered envelope/level/rates),3f81 (depths).
Centered sensitivities combine their own sign with bend direction; uncentered
depths have reversed bend polarity. This is not the seven-bit CC calculator.

All16384 input values are paired with actual H8 execution, varying channels,
receive gates and all-byte sensitivities. Every case compares all five rows of
all16 parts, checking preservation as well as computed results. Native tests
cover fragmented running status, matched/disabled parts, both extremes, zero
versus one equivalence, midpoint clearing and malformed data. All10 CTests pass.
Configuration still supplies explicit sensitivities: GS/RPN updates, reset,
active-voice controller scheduling and product integration are not established
by these tests. Product control-ROM independence is still incomplete.
The complete regression `/tmp/sc55-pitch-bend-1.csv` matches
`/tmp/sc55-cc-contributions-1.csv` byte for byte (923053 PCM writes).

The data-only native wave path now composes all five contribution producers:
CC1, pitch bend, channel pressure, assigned CC16/17, plus poly pressure for the
installed original key. Explicit sensitivities produce nonzero pitch offsets;
these are test configuration, not a substitute for GS initialization. Startup
pitch inputs are carried into the ongoing `VoiceControlInputs` rather than
being lost when the per-tick structure is initialized. CC contribution handling
also runs alongside scalar/pedal handling rather than short-circuiting it.

The integration test changes bend at ticks32/48/56 and refreshes controller
inputs of the live voice while retaining envelope/LFO state. All72 refreshes
(three for each of24 isolated partials) change the resulting PCM pitch word
relative to a zero-controller-offset counterfactual from identical envelope,
LFO and glide state. Every written pitch register10 is checked against RAM2[0].
The waveform test renders varying audio from all24 partials with no control-ROM
execution; the empty-wave negative control stays silent. All10 CTests pass.
This remains an isolated-partial integration harness, not24 concurrent voices,
a firmware-accurate controller update schedule, host integration, or a waveform
equivalence/CPU-load claim. The product still requires H8/control ROM.

Paired preparation at5565 invokes5ff5..6049, copying exactly11 controller words
from first voice to second. `VoiceControllerState` owns these: pitch/envelope
offsets, level bias, two rate modifiers and two controller contributions for
each of amplitude/second-envelope/pitch modulation. `CopyPairedVoiceControllers`
copies that state, not the oscillator phase, waveform, patch depths or envelope.
Its adapter writes only the corresponding consumer fields and rate modifiers;
signed word conversion is explicit.

`PrepareVoiceControllers` now computes all11 outputs of5c46..5ff4. Inputs are
the selected per-key byte,11 part sensitivity bytes (4c..57 excluding4f) and
five11-word contribution rows. Centered sensitivities use signed magnitude
with truncation before negation; other depths use an unsigned quarter-product.
Five additions wrap as16-bit words before sign selection, saturation, shifting
and high-word multiplication. Output-specific caps and coefficients reproduce
pitch, envelope, level and rate scaling. No control-ROM lookup or CPU execution
is required. GS configuration/reset of these input tables remains unfinished.

4096 full H8 producer cases compare every output across byte extremes, small
and arbitrary wrapping contributions. Unit tests include neutral input,
wrapping cancellation and distinct low-amplitude scale results. The native
wave fixture initially called this calculation with explicit neutral input
tables; it now uses the nonneutral MIDI composition described below. All10 CTests pass.
`/tmp/sc55-controller-preparation-1.csv` matches the paired-controller baseline
byte for byte. A live observer also snapshots producer input at5c20 and checks
all11 results at5ff4 without mutating the H8 execution.
The initial live observer failed because `MCU_Step` handles interrupts before
executing the observed instruction: an unexecuted5c20 entry was counted twice.
Entry snapshots are now committed only when the next PC is5c22; interrupted
entries discard their provisional expectation. This does not skip a completed
calculation or ignore an output mismatch. The full rerun
`/tmp/sc55-controller-preparation-live-3.csv` has32510 complete11-field matches
and2 interrupted entry snapshots, and equals
`/tmp/sc55-paired-controllers-1.csv` byte for byte (923053 writes).
The first two live logs are failed observer diagnostics, not passing baselines.

2048 H8 transfer cases cover distinct and aliased source/destination and compare
all512 destination bytes, including fields that must survive. Unit tests check
signed values, exact consumer mapping and phase/depth preservation. All10 CTests
pass. `/tmp/sc55-paired-controllers-1.csv` equals
`/tmp/sc55-preparation-context-1.csv` byte for byte. Complete paired startup is
not yet integrated: the two-voice control transfer is one required step, not
proof of simultaneous native two-partial sound or complete controller support.

`StopPreparedVoice` now composes the complete53e6 state/PCM transaction and is
shared with group reclamation. `RestartAndInstallVoice` applies the preparation
flag7 gate, stops the old voice when requested, and installs/returns the selected
sample. It validates the return path before any PCM I/O. Reclamation sets CAF4=4;
restart itself preserves CAF4, and normal installation sets2. Native real-PCM
tests cover192 combinations across24 slots, both ramp branches, restart/bypass
and normal/negative samples; invalid links produce no device calls.

Complete stop compares384 H8 state/device cases. This comparison requires an
explicit PCM peripheral: the instruction-only CPU has none. Its initial missing
connection caused a test crash, fixed by a dedicated test PCM and restoring the
previous peripheral afterward. `/tmp/sc55-restart-install-2.csv` equals
`/tmp/sc55-voice-installation-2.csv` byte for byte.

**Task-dispatch integration:** forwarding installation's CAF4=2 directly to
`PreparedVoiceBatch` previously failed its readiness gate. `DispatchNextVoiceTask`
now implements54bf..5542/55de: highest numbered nonzero task first; task2 clears
the selected slot and its selected PCM partner. CAC4 has priority and places its
partner first; otherwise CADC places its partner second; exactff means absent.
Task4 clears activity and changes all three stages to0e if the first was12,
otherwise10. Invalid task/link values roll back, rather than following an unsafe
firmware jump-table entry. Idle preserves all state. Batch-mask initialization
at54b4 and detailed preparation at5639 and later are outside this function.

3072 H8 executions compare all24 flags, activity bytes and three stage words,
plus selected preparation order. Unit tests cover idle, competing requests,
first-link priority, ignored second link, corrupt input and task4 transition.
The native wave fixture now dispatches the actual installed lifecycle before
preparation, removing its independent post-scheduler lifecycle fixture. All10
CTests pass. `/tmp/sc55-task-dispatch-1.csv` equals
`/tmp/sc55-restart-install-2.csv` byte for byte (923053 writes).
Paired preparation and complete native mono retrigger remain unfinished; the
isolated-partial fixture does not prove these paths.

`PrepareVoiceContext` now expands5639..56b4 using installed tone/partial/sample
IDs, part, slot and flags. Firmware bank/pointer copies become data identity,
while the+30 pointer becomes a per-key control-table selection: mode0 selects
the first RAM table, positive mode the second, high-bit mode no table. These
tables' musical semantics and contents are not invented. Flag7 sets activityff;
without it the previous activity is preserved. Invalid native bounds and
negative samples fail without changing activity; caller validates data IDs and
keeps the matching immutable generation alive.

4096 H8 cases compare identity translation, slot, part-control pointer, flags,
key-table selection and activity across all mode/flag byte values. The native
wave fixture consumes context flags rather than independently forcing flag7.
Its explicit new/restarted normal-pitch fixture now supplies flags a0 at
installation, so the restart gate is exercised; this is not yet MIDI-derived
mode ownership. All10 CTests pass. `/tmp/sc55-preparation-context-1.csv` equals
`/tmp/sc55-task-dispatch-1.csv` byte for byte (923053 H8 PCM writes). No complete
native/H8 audio-fidelity or production CPU-load claim follows from that trace.

`VoiceInstallationState::install` owns the startup metadata and pending-release
initialization, using tone/partial/sample IDs into the caller's immutable sound
data instead of H8 bank/pointer pairs. The normal path marks allocator status0,
preserves/sets preparation flag7 according to restart, records keys, velocity,
mode and sample lookup key, sets task state2, and clears both release source and
pending request. Caller validates data IDs and retains the matching generation.

A negative sample ID instead clears status and executes native `returnVoice`;
old metadata, pending request and preparation flags remain untouched. Invalid
return links roll back the whole allocator update. This is not PCM key-on or
the earlier53e6 restart operation, and boot/default-part ownership is unfinished.

The2048 paired H8 executions run113a..11cf without stubbing its return delegate:
1024 normal and1024 negative samples, both group slots, all256 preparation flag
values and both restart states. Allocator tables, target metadata, pointer/bank
translation, preparation flags and pending request are checked. Unit coverage
adds invalid bounds/links and metadata preservation. Native wave integration
now obtains envelope key and pitch-start keys/velocity/flags from this installed
record, replacing its former direct `allocator.status=0` fixture write. It still
renders isolated partials; this does not establish concurrent native polyphony.
All10 CTests pass. The final full H8 regression
`/tmp/sc55-voice-installation-2.csv` equals `/tmp/sc55-partial-staging-1.csv`
byte for byte (923053 PCM writes). Production still executes the control ROM;
these checks do not establish native/H8 whole-render fidelity or CPU savings.

`PartialDispatchState` now owns the previous-key cache (16 parts x2 partials)
and24 slot inputs (fractional pitch, amplitude/secondary velocity factors and
previous key). Its `stage` models the post-delegate handoff at1244..129b and
12c5..1323: a prepared partial updates its cache even with a negative slot;
only a valid slot receives inputs, and a second partial sharing that slot
overwrites the first. Disabled preparation leaves both unchanged. Invalid
part/partial/slot inputs fail without mutation. Initial values are not claimed
to be GS boot state, and restart/installation are not performed by this API.

The2048 isolated H8 dispatch cases additionally compare every cache entry and
every slot input, including untouched entries, with varying byte/word values.
Delegated calls remain stubbed in this particular check. Unit tests explicitly
exercise shared-slot overwrite, no-slot cache updates and invalid-input rollback.
The native data-only wave fixture now consumes amplitude, secondary velocity and
fractional pitch through this owned handoff; previous-key history remains an
explicit fixture rather than a complete mono owner.
All10 CTests pass after this integration. The full H8 regression
`/tmp/sc55-partial-staging-1.csv` matches
`/tmp/sc55-live-partial-dispatch-full-1.csv` byte for byte (923053 writes).
This is not a full native audio comparison or a production CPU measurement.

The mono and sample probes now compare `PlanPartialVoiceDispatch` at1219
against preparation calls at123a/12bb and installation calls at129b/1323,
finishing at132a. Unlike the 2048 isolated cases, no delegated call is skipped:
sample preparation, restart and installation execute normally in H8. Native
inputs are the imported patch, candidate bits and two allocated slots captured
at entry. Observations do not mutate emulated state. Nonempty coverage and no
pending comparison are required; sample mode also requires dual preparation.

`/tmp/sc55-live-partial-dispatch-1.csv` (`--mono-probes`) matches the previous
mono-velocity-2 trace byte for byte: 37 dispatches, none dual/shared.
`/tmp/sc55-live-partial-dispatch-full-1.csv` (`--soft-sample-probes`) matches
`/tmp/sc55-native-partial-dispatch-2.csv`: 2689 dispatches, 756 dual preparations,
zero shared slots; 923053 PCM writes. Shared-slot fallback remains covered only
by the isolated tests. All10 CTests pass. This validates selection in context,
not a native replacement for the delegates or complete native polyphony.

## Correction: channel-to-voice effect-send byte order

The initial channel-output adapter incorrectly connected CC91 to the low byte
and CC93 to the high byte. H8 observation at2fcc establishes that voice+2e
points to the same part block (80b8 for default MIDI channel0), whose+0e is
chorus/CC93 and+0f is reverb/CC91. The resulting effects word has reverb in the
high byte and chorus in the low byte. The arithmetic routines already matched
the firmware addresses; the bug was the new semantic MIDI adapter, not those
raw-word routines. `ApplyChannelOutputControls` now swaps into their legacy
reversed field names. Comments explicitly identify those names as low/high;
existing primitive callers and owned scale layouts are unchanged.

A minimal default-state test failed before the fix (native40/0 versus H8's
0/40). The new `--output-control-probes` scenario also failed against the live
H8 voice pointer before the fix. It sends distinct CC91/CC93 values across all
16 channels and compares volume, expression, pan and both send inputs when
each H8 voice reaches2fcc. All16 parts now match. This closes the former gap:
the channel adapter tests had merely restated the adapter's own assumptions.
The PCM integration's target-word assertion is corrected to4020 for CC91=64,
CC93=32, while the raw-byte primitive comparisons are unchanged. All10 CTests
and the MD13 PCM integration pass. Temporary send-mapping logs were removed.

Reference artifacts: `/tmp/sc55-midi-output-controls-red-1.csv` is the failed
pre-fix observation, `/tmp/sc55-midi-output-controls-green-1.csv` is the passing
live scenario. This corrects earlier notes that called+0e reverb and+0f chorus;
it does not implement the reverb/chorus audio algorithms or complete native
polyphonic/host integration.
The full H8 regression `/tmp/sc55-native-send-mapping-1.csv` remains byte-identical
to `/tmp/sc55-native-midi-hold-1.csv` (923,053 writes). This trace is an H8
regression check, not native audio-fidelity proof.
