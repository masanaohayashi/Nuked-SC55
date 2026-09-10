# Native note-group ownership

## One note continuation spans sample and DSP preparation

`resumeNotePreparation` now advances the complete admitted note, not just its
two sample installations. After installation it retains the dispatch requests
and pitch-history update while the owned DSP operation advances. Only final
completion returns `started` with requests/prepared voices/history together;
earlier steps return `preparing` without a partial result to commit.

The default fresh, reused and rhythm path drains the same bounded sequence
(at most five resumes). Direct lower-level preparation remains available to
its existing callers; the player no longer needs separate handoff storage.
No device time is introduced between these steps until the timing scheduler
is implemented. No key-on or gain-wait predicate changed.

The end-to-end staged test exercises every yield, rejection of a competing
note/control pass, input lifetime, delayed result publication and final PCM
write equality against the normal path. Product build, PCM regressions,
release111 and startup8/otherEG88/block partitions pass.
Logs: `/tmp/sc55-full-preparation-*.log`.

## DSP preparation is an owned operation through activation

`NormalVoiceDspPreparation` retains validated records, controller-derived
inputs, second-envelope setup and prepared voice values. Its resume order is
shared first-LFO initialization, then each voice's second-LFO, envelopes,
spatial/level and pitch setup, in the original dispatcher order. Continuing
voices are copied before suspension; no borrowed `continuing` pointer survives.

`VoiceControlRuntime::beginNormalPreparation` reserves this operation;
`resumeNormalPreparation` advances one semantic portion and hands the completed
batch directly to PCM startup. MIDI/installation/control remain deferred while
preparation owns the state. The normal `prepareAndBeginNormalStart` drains these
same steps, so there is no alternate product DSP algorithm or new timing delay.

Validation: staged versus whole PCM write sequence, caller-input destruction,
second-begin rejection, no replay after completion and existing PCM tests pass.
Release integration111 and startup8/otherEG88/block partition tests pass.
Logs: `/tmp/sc55-dsp-preparation-*.log`. Work-duration and event arbitration
remain incomplete; resumability alone is not timing parity or a CPU-speed claim.

## Receive identity and admission progress stay with the voice engine

The engine owns `VoiceCommands` and `PendingAdmission`, including original
receive key/tone, held-key-return origin, source-reuse decision and once-only
retirement progress. The GS receiver still assembles receive-time requests and
live configuration; it no longer owns a second voice-admission lifetime.

`previewMelodicAdmission` checks fresh melodic eligibility on a copy, retires
same-key groups once for that admission, then recomputes the preview against
the updated allocator. Rejected notes do not retire. High-note mapping retains
the original received key for retirement. Mono/source fresh admission shares
`retireAdmission`; capacity/PCM retries cannot repeat its marking/reclamation.

GS reset explicitly retains the received command FIFO and active admission
when rebuilding sounding owners, preserving the prior reset behavior. No new
event timing, capacity selection or PCM arithmetic is introduced.

Validation: `/tmp/sc55-admission-progress-*.log` (release111, reserve48 and
protected mono, startup8/otherEG88, bulk/reset8 transfers1864 bytes).

## Device activation and waveform events belong to the engine

`NativeVoiceEngine::serviceActivation` now owns the next common-kernel-tick
deadline after an unsuccessful reuse poll. The player asks for that deadline
when splitting render spans; it no longer owns an independent reuse-wait flag.
Key-latch protection remains a per-PCM-pass check, not a kernel-tick delay.
Resetting the voice engine also resets its activation deadline.

`handlePcmBoundary` owns the waveform-end pitch/stop operation and publication
to lifecycle and first/second LFO stages. The player acknowledges device events
only outside startup, then delivers the event without editing voice internals.
The existing five calculation-phase interruption tests now exercise this engine
entry rather than calling the low-level helper directly.

Startup-wake preserves eight busy reuses and 88 unrelated envelope updates.
Native audio checksum remains `3b54320560580fd3`, with block-partition equality.
Bulk/reset passes eight transfers and full 1864-byte H8 readback equality.
Logs: `/tmp/sc55-activation-owner-*.log`. These are behavioral/ownership checks,
not proof of a plugin CPU improvement or completion of execution-time scheduling.

## An admitted note owns its preparation through the DSP handoff

Fresh melodic, mono/source reuse and rhythm now all use
`VoiceControlRuntime::beginAllocatedNote`. Its fixed-size `NotePreparation`
owns the selected note, both DSP inputs and the sample-installation continuation.
The normal product path drains the same steps; a partial slice can retain the
operation after the first partial without retaining caller-local references.

While installation owns the admitted note, another admission, reclamation,
stop-task dispatch, completion consumption and periodic control cannot mutate
its owners. A suspended periodic pass and pending clock event are retained.
Finishing installation transfers directly to the existing DSP/startup operation.
This protection is not extended across the PCM reuse wait: unrelated envelopes
still run during gain decay, preserving the drum-attack fix.

This is an ownership handoff, not execution-time emulation. Default activation
times, PCM arithmetic and reuse predicates are unchanged. The remaining
input-dependent scheduler timing and capacity-survivor mismatch are not solved.
Validation logs: `/tmp/sc55-owned-preparation-*.log`.

## Prepared-note history has one commit

`NativeVoiceEngine::PreparationHistory` owns the per-part partial keys,
preparation reference, per-slot previous pitch and drum map/key provenance.
The player no longer owns separate arrays for these. Melodic, mono/source and
rhythm admissions all call `rememberPreparedNote` after acceptance instead of
three independent copies of history-update code.

Deferred/rejected results and incomplete DSP batches are rejected before any
history mutation. Prepared-only outcomes still update the sample-derived key
history even when no DSP owner was produced. Melodic reuse invalidates the drum
map but retains the old key byte, as before. Reset retains the old part-key
history/reference until the existing tone-seeding logic runs, while slot pitch
and drum provenance reset with the engine. No new reset semantics are inferred.

The product translation unit builds; actual MIDI/H8 release integration passes
111 comparisons. Bulk/reset validation passes8 transfers and1864 readback bytes.
Logs: `/tmp/sc55-preparation-owner-{build,release,reset}.log`.

## Mono performance belongs to the voice engine

The 16 mono performance states now live in `NativeVoiceEngine`: held keys,
current note, preparation velocity, glide rate, explicit source note,
portamento enable and selected tone. Resetting the engine resets these with
the voice owners, without a second player-owned mono array.

`monoReuseFlags` owns the restart decision against the actual group and live
EG completion latch. No held keys means a fresh attack even with a linked
release tail; legato and held-key return retain their existing distinct rules.
`releaseMonoNote` clears the held key, decides highest-key replacement or group
release and publishes the release to DSP owners. The player only schedules
the returned replacement with the current tone and retained velocity.

Actual MIDI/H8 release integration passes 111 comparisons; mode changes during
preparation/reuse also pass. Logs: `/tmp/sc55-mono-owner-{release,mode}.log`.
This moves existing control behavior into its owner; it does not alter PCM,
add timing estimates or resolve the open capacity scheduling difference.

## Admission operations owned by the voice engine

`NativeVoiceEngine` now owns repeated-note retirement, capacity reclamation,
single-group stopping and part-wide group stopping. `NativeMelodicPlayer` no
longer calls `RetireRepeatedNote`, `EnsureVoiceCapacity` or
`StopAndReclaimGroup` directly. The pending MIDI admission still owns the
once-only retirement flag and the retry/discard decision.

The engine selects the current lifecycle from the live EG owner, except when
an installation/stop request is pending: that published handoff must survive
until its consumer runs. Melodic capacity (including mono/source reuse),
repeated-note retirement, rhythm admission and group stops use this same
selection. This is ownership of actual reclamation, not a new PCM renderer or
an emulated H8 task layer. No timing constants were introduced.

Product translation-unit build and `--native-startup-wake` pass. The latter
covers eight busy reuses, 88 unrelated envelope updates and zero/1/127/257-frame
block equivalence. `--native-reserve-stealing` passes 48 admissions/survivors
and protected mono rejection. Logs: `/tmp/sc55-admission-owner-*.log`.
Full control scheduling parity and the known alternate-capacity survivor
difference are separate unfinished work. No Xcode/Logic or CPU-load claim.

`VoiceAllocator` now owns an array of `NoteGroup`, replacing seven independent
RAM-shaped group arrays. Note creation, release, pedal retention, repeated-note
retirement, capacity selection and final return operate on the same group object.
Physical voice links remain separate: one note may own two physical voices.

| Native field | Reference RAM row | Meaning |
| --- | --- | --- |
| next / previous | A240 / A258 | Part-list links; next also links free groups |
| status | A270 | Sounding/released/free state byte |
| key | A2E8 | Group's note key |
| retirementFlags | A288 | Hold-deferred bit0, repeated-note mark bit2 |
| noteClass | A2D0 | Melodic80/high81 or rhythm exclusion class |
| releaseFlags | A300 | Bit0 permits Note Off; other bits retained |

The H8 address mapping belongs to the oracle adapters, not to the object's
layout. Tests load/compare individual members against the same reference rows;
no packed RAM alias or compatibility array is kept in the native controller.

`acceptsNoteOff`, `deferRelease`, `releaseHold`, `held` and `markRepeated` keep the
shared state decisions with the group. In particular, clearing hold preserves
the repeated-note mark and unknown flag bits; marking a repeat returns the old
mark, so the first encounter does not accidentally become the second encounter.
The group creation request also names noteClass/releaseFlags rather than scratch
RAM addresses.

## Physical allocation state

`VoiceAllocation` now replaces the six parallel physical-allocation rows. Each
slot owns its part, noteGroup, status, releaseRequested, releaseCommand and
nextFree link. The former A360 byte is the retained release-request marker;
A3E0 is the command published to the voice controller. `requestRelease` changes
both together; `clearRelease` is used when returning the slot. Installation
still clears only the command, and table initialization still clears only the
request marker: these are intentionally not collapsed into a generic reset.

Logical allocation freedom is not PCM reuse readiness. The separate voice
runtime still owns gain-drain/startup protection, EG/pitch/filter state and PCM
completion. The native controller's activity array remains a distinct control
measurement used by both the scheduler and allocation policy. It is not a
second copy of the slot's allocation status.

Oracle adapters map named fields to H8 rows individually. Tests requiring a
before/after release-command or status array use `SnapshotAllocationField` in
the test directory; no such projection or compatibility copy is added to the
audio engine. Tests still compare the same fields, not a weakened subset.

After this second representation change, both diagnostic targets and the
envelope-runner build/check pass; release integration111, reserve admissions48,
envelope-PCM and NativeSynth checksum/block-partition checks also pass.
Logs: `/tmp/sc55-voice-allocation-*.log`.

This changes representation and ownership, not MIDI ordering, initialization
values, free-list order, group lifetime, PCM behavior or control timing.

Validation: both diagnostic targets build; the envelope-runner and envelope-PCM
checks pass; actual MIDI/H8 release integration passes111 group comparisons;
reserved capacity passes48 admissions/survivors and protected mono rejection.
NativeSynth still produces checksum3b54320560580fd3 with matching
zero/1/127/129/257-frame partitions. Logs are `/tmp/sc55-note-group-*.log`.
The unrelated piano capacity-timing discrepancy remains open. The Xcode product
build recorded earlier predates this representation change; the product emulator
translation unit was rebuilt as part of the diagnostic target, not installed.
