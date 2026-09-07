# Voice lifecycle evidence (SC-55 v1.21)

The production backend still executes H8. These native primitives are not yet
a complete allocator or a ROM-free instrument.

Termination at 3393 clears activity, marks envelope finished, detaches CAC4/CADC,
and writes PCM command 00b6. It stores the logical slot in A1D4 and signals via
TRAPA 2 (R0=1, R1=1). The task at 07c3 receives this signal and calls 1cbd with
that slot. This is a task signal; FIFO semantics have not been established.

Routine 1cbd skips already-free slots (A348 bit7). Otherwise it clears A360 and
A3E0, calls 1e7e, appends the slot to the free chain (A378, head A3C3, tail A3C4),
sets status 94, increments A3C1, and may recycle the now-empty note group. The
whole routine, including group removal and part bookkeeping, is now implemented
by `VoiceAllocator::returnVoice`. Production still uses the firmware path.

`VoiceGroupLinks::detach` implements 1e7e through 1ecd. A390/A3A8 are distinct
from the PCM-side CAC4/CADC links. A2A0/A2B8 hold group endpoints. If a successor
exists it becomes the head; otherwise the predecessor becomes the tail. The
firmware tests the sign bit for missing links and preserves a negative
predecessor byte when writing the tail. This is not a generic arbitrary-length
doubly-linked-list erase: do not replace it with one without further evidence.

Validation: 16,224 isolated H8/native comparisons, all six 24-byte arrays checked
after each call, including self-links, successor precedence, and ff/80 sentinel
cases. Unit tests cover invalid indices without partial mutation. Full
soft-sample probe output: `/tmp/sc55-group-detach-1.csv` and `.log`.
No live allocator ownership, host performance improvement, or native note-on
allocation is claimed by these tests.

## Return/recycle implementation

`sc55_voice_allocator.h` owns the byte-sized state touched by 1cbd..1d54 and
both subroutines (1e7e and 1e59). The C++ operation skips already-free voices,
detaches voice links, appends the voice to the free list, recycles an empty
group, and conditionally recomputes the minimum A2E8 value across the part's
remaining groups. A1F0 is decremented with byte wrap, matching firmware.
The musical meaning of A2E8 is not yet asserted. Default construction is not
firmware initialization; allocation, initialization, and stealing remain pending.

All updates are bounded and allocation-free. Bad indices or cycles reject a
temporary state copy, preserving the caller's state. Unit tests cover this
failure path and double return. 24,576 isolated full-routine cases compare all
mapped fields, and 3,446 live calls match through the end of the routine in
`/tmp/sc55-voice-return-2.log`. The corresponding CSV is byte-identical to
`/tmp/sc55-group-detach-1.csv`. Live checks snapshot state at entry: they do not
prove persistent native allocator ownership or native note allocation.

## Free selection and attach

`takeFreeVoice` implements 1ca5..1cbc: remove the free-list head, update the tail
when the next byte is negative, decrement the byte count, and return the selected
slot. It intentionally does not mark the slot active or clear its old links.
Native callers receive an empty optional on an empty/invalid list instead of
allowing the firmware's out-of-range access when its caller contract is broken.

`attachVoice` implements 1c40..1ca4. An empty group clears stale reciprocal PCM
links in firmware write order. A nonempty group retains the old tail as its head
and installs the new voice as tail; this is not arbitrary-list append. Ownership
bytes are updated, but status and the part voice count are the caller's job.
Isolated comparisons cover 624 free selections and 48,672 attaches, including
self-links and negative sentinels. Invalid input tests check rollback.
Live snapshots in `/tmp/sc55-voice-attach-1.log` compare 3,445 selections and
3,445 attaches successfully. The CSV is byte-identical to
`/tmp/sc55-voice-return-2.csv`; all seven CTests pass. As with return/recycle,
these are shadow comparisons, not persistent native allocation ownership.

`createGroup` now implements 1bab..1c3f and calls both primitives for the requested
voice count (A3D4). It takes a group from A3C2, appends it to the part group chain,
copies A3D1/A3D2/A1BF to A2D0/A2E8/A300, and updates the per-part minimum and
count. A270 and A288 are cleared. The minimum update follows N from the byte
subtraction exactly (not a general signed comparison). Selected voices are
returned in reverse acquisition order, followed by the ff terminator. Group
endpoints are not reset here: they retain their prior allocator state, as in H8.

12,288 isolated cases compare the complete operation, including its nested free
selection and attach calls. A ROM-free unit trajectory creates a two-voice group,
returns both voices, and reuses the group and free slots without importing H8
state. This fixture is not a boot initializer or audible native instrument.
Native invalid-capacity and repeated-slot cases fail transactionally. Voice
status activation remains separate, just as it is in the firmware.
Live full-operation snapshots match 2,689 group creations in
`/tmp/sc55-group-create-1.log`; its CSV is byte-identical to
`/tmp/sc55-voice-attach-1.csv`. All seven CTests pass. Live operation still uses
H8, with native results checked at the routine's return boundary.

The caller at 1867 reaches this operation after capacity checks; higher-level
capacity/stealing decisions remain to be recovered before native note-on
allocation is complete.

## Allocator table initialization

`initializeTables` implements the allocator-owned fields in 04:04b9..0569.
For 24 voices, the free-voice chain runs 23 down to 0; the free-group chain runs
0 up to 23. Voice/group statuses become 94. Part counts clear, endpoints become
ff, minima become 7f, A230 bit0 clears (other bits retained), and A200 receives
the preceding part index. Fields not touched by this region remain unchanged.
In particular, PCM links and A3E0 are initialized earlier at 04:0450..0499;
this method does not pretend to reset those or the whole synthesizer.

2,304 isolated comparisons cover all voice/group counts 1..24 and four nonzero
initial-memory patterns. The production firmware uses 24; smaller configurations
only exercise the recovered loop semantics. A ROM-free unit trajectory calls
the native initializer, creates 12 two-voice groups, rejects exhausted capacity,
returns all 24 voices, and allocates again. Activation status is supplied by the
fixture because the activation routine is still outside this allocator.
Live initialization is also compared at the 04:0569 boundary. Results are in
`/tmp/sc55-allocator-init-1.log`; the PCM CSV remains byte-identical to
`/tmp/sc55-group-create-1.csv`, and all seven CTests pass.

## Stealing candidate selection

`selectCandidate` recovers three scans (1961..19ae, 19af..19f4,
19f5..1a3a) called by 18ff. First pass excludes zero-status groups and groups
whose A2E8 equals R4. Second pass only excludes the matching value. Third pass
only includes the matching value. Each considers the group tail then its A3A8
predecessor. Lowest AC42 wins; strict comparison preserves scan order on ties.
Activity ff never replaces the initial no-candidate sentinel. R4 is loaded from
the part minimum by 18ff. No loudness interpretation of AC42 is assumed here.

12,288 seeded isolated scans across all three modes compare the returned voice,
activity, and A3CC and verify allocator tables are unchanged. Unit tests cover
empty parts, ties, filtering, invalid references and cyclic group lists. The
native empty-part result is safe; the original routine assumes a nonempty part.
This is candidate selection only, not the complete stealing policy: 1737 applies
part reserves (8018), a starting-part control (8028), and part order (A200).
The selected group is reclaimed via 1a3b, not normal envelope-completion return.
That reclaim path and the capacity-control loop still need native integration.
Validation run: `/tmp/sc55-candidate-1.log` and `.csv`; no live stealing coverage
is claimed by this scenario.

## Post-stop reclaim bookkeeping

`reclaimStoppedVoice` implements 1a7d..1b0f (append) and 1b24..1baa
(prepend). Unlike normal return, it uses the caller's group/part, does not skip
status 94, clears AC42, and decrements shortage A3C0 with byte wrap. Both share
the verified group detach/recycle and minimum update with normal return. The
prepend variant changes which voice will be reused next, so it is not aliased
to the append path.

49,152 isolated comparisons cover both variants and compare all mapped state.
Unit tests cover insertion order, explicit free-status reclaim and bad-index
rollback. The caller MUST first perform 53e6 and set CAF4=4; this method does
not silence PCM or finish the envelope state machine. The group loop at 1a3b
repeats stopping and reclaiming its tail until empty and preserves next group
in R5. That complete loop is still pending.

Next stop-path evidence: 53e6..542f selects PCM channel, reads both envelope
levels via 32/34 and the 3a latch, compares them, and writes 00b6 to 16 or 18.
It also stores the command at voice+1a or +1e and sets three stage words to
12 or 14. Thus ordinary envelope termination's fixed 18 write is not a complete
substitute. The linear listing is misaligned at 53e6; decode the entry bytes
against the emulator before implementing this path.

## PCM stop selection and I/O

`PrepareVoiceStop` implements the unsigned comparison at 5404. If raw level34
is less than raw level32, command 00b6 goes to PCM16, cache offset1a and stage12;
otherwise (including equality) it goes to PCM18, cache offset1e and stage14.
327,680 boundary/comparison cases execute H8 through the branch and compare
R6 and the selected instruction address. `StopVoicePcm` selects a logical
channel 0..23, latches 32 then reads 3a/3b, latches 34 then reads 3a/3b, and
writes the selected command bytes. No ROM lookup is needed for that operation.

18,432 ROM-free real-PCM register cases verify read/write ordering, command
selection, unchanged current levels, and isolation of other channels. This
proves register interaction, not completed fadeout or audible stealing fidelity.
The plan returns stage/cache metadata to its caller; committing the three stage
words, clearing CB30 (the entry bytes f1 cb 30 13), and setting CAF4=4 remain
integration work. The full stop/reclaim/reuse lifecycle is not complete.

## Correction: stopped stages do not poll for silence

Dispatch at 3195 sends stages >=0e to 3363, bypassing normal envelope ticking.
The bytes at 3363 decode as comparisons with 10 and 0e. Stage10 reads PCM34,
stage0e reads PCM32; every other value (including forced-stop12/14) jumps to
33e7 without reading PCM or updating the voice/activity. The ordinary envelope
table at 6af8 is therefore not a valid dispatch table for these stopped stages.
Do not add a zero-level wait to the normal EnvelopeRunner for forced stops.

768 isolated H8 runs at 3363 with stage12/14 verify the jump to 33e7 and preserve
all 256 tested voice bytes plus AC42. For the separate 0e/10 polling path,
337f tests raw level zero before doubling it; nonzero activity is the high byte
of the doubled (16-bit wrapping) level, with ff replaced by fe. That path is
not yet a native implementation.

Next integration must preserve the immediate allocator reclaim following 53e6
while keeping the hardware ramp and reactivation sequencing. The reuse path
57f6 writes cached command1e to PCM18, writes voice48 to PCM10, restores stage
from voice06, and clears voice06. Its surrounding synchronization still needs
inspection; waiting for silence in stage12/14 would change firmware behavior.

## Native composed group stop and reclaim

`sc55_voice_lifecycle.h` now composes the two group reclaim loops with PCM stop
and metadata updates. It retains the original next-group byte, visits group
tails in firmware order, clears CB30, performs the selected PCM ramp command,
updates only the chosen cached command and all three stage words, sets CAF4=4,
and reclaims the slot. It does not wait for the ramp. A preflight copy validates
the full chain before any irreversible device I/O. Callbacks must be bounded,
nonthrowing and non-reentrant; allocator/device state has one serialized owner.

64 ROM-free real-PCM tests cover all 16 parts, one/two voices and append/prepend
reclaim. They check metadata, shortage/count, retained nonzero PCM levels and
subsequent native allocation. Invalid targets cause no device writes. These
tests compose previously H8-compared primitives, but are not an end-to-end H8
comparison of 1a3b/1a53 and do not prove audible reactivation. The new stop state
is not wired into the production processor or persistent envelope runner yet.
All seven CTests pass. Next: verify the whole group operation against H8 and
connect capacity policy and reactivation, rather than calling slot allocation
proof of complete synthesis.

## Whole-operation H8 comparison

The oracle now executes 1a3b/1a53 through their return instructions, including
53e6 and both nested reclaim paths, against an independent real PCM instance.
512 cases cover all 16 parts, one/two voices, prepend/append and eight level
patterns. Native and H8 results match allocator state, all 24 voices' stop
metadata, original next-group result, balanced H8 stack, and the complete PCM
ram2 register array. This supersedes the earlier lack of whole-operation
comparison; it still does not exercise PCM clock advancement during the routine
or full audible reactivation/host behavior. Native device callbacks run with a
single owner and the isolated H8 run does not deliver interrupts.

Evidence: `/tmp/sc55-group-stop-1.log`, research Release build, seven CTests.
The production H8 path remains unchanged; capacity policy and persistent native
ownership/activation are still required for a control-ROM-free instrument.

## Capacity preparation

`EnsureVoiceCapacity` now composes 1737..17b7 and 18da..1960 with the native
group-stop operation. It leaves shortage untouched if capacity is already
sufficient, otherwise scans the firmware part order, checks per-part reserves,
and applies mode0 (group-chain order), mode2 (three activity-selection passes),
or refuses unsupported modes as the firmware does. Patch bit4 forces mode0.
R4's initial protected value remains fixed across mode2 passes. Final fallback
tries the incoming part when it has at least the outstanding number of voices.
Reclaiming a whole two-voice group can cross a reserve or overshoot shortage;
that behavior is preserved. False means insufficient capacity under valid policy,
not rollback: earlier successful reclaims remain committed. Invalid data returns
nullopt, likewise without undoing earlier groups already stopped.

192 full H8/PCM comparisons start at 1737 and stop at 178c, comparing the
signed-condition success result, complete allocator state and PCM ram2. They
cover full 24-voice fixtures, reserve settings, start-part control, mode choices,
patch override, and one/two-voice requests. Six ROM-free real-PCM fixtures also
exercise capacity preparation followed by native group creation. Evidence:
`/tmp/sc55-capacity-1.log`; seven CTests pass. This is offline, noninterrupting
verification, not production/host polyphony validation. Note-on prechecks,
reactivation, and persistent processor ownership remain unfinished.

## Reuse readiness gate (not the stopped-stage dispatcher)

5710..573e reads PCM32/34 and then checks CAF4. Nonzero CAF4 aborts this attempt
via 56d9. With CAF4 zero, either level zero permits continuation; two nonzero
levels issue TRAPA0 with R0=80 and retry after the task resumes. This wait is
separate from the 12/14 stopped-stage bypass. Immediate allocator reclaim does
NOT imply immediately programming a replacement audible voice.

`PollVoiceReuse` implements one bounded iteration and returns ready, pending,
or cancelled. It neither spins nor invents a delay duration. The native scheduler
must retry pending work later and handle cancellation; neither is wired into the
production renderer yet. 2,400 isolated H8/PCM iterations compare all three
outcomes across 24 channels, four CAF4 values, and level boundary pairs. The H8
test stops before TRAPA rather than faking the task scheduler. ROM-free PCM tests
also exercise pending-to-ready and cancellation precedence. Evidence:
`/tmp/sc55-reuse-ready-1.log`; all seven CTests pass.

57f6 is selected by bit7 of voice-3b being clear; its classification as a generic
"reused voice" path is not established solely by its address. Trace that flag
and caller sequencing before using it for every stolen slot.

## Prepared activation commit

`ActivatePreparedVoice` now implements 57c5..5809, using the already selected
PCM channel. With voice-3b bit7 set, nonzero delay accumulator sets all stages
to2 and writes cached18 and voice48; zero delay clears progress, sets all stages
to0, and writes 00b6 and zero. The latter updates cached18 but preserves voice48.
With bit7 clear, it writes cached18 and voice48, restores only stage0 from
voice06, and clears voice06. Stages1/2, delay and progress remain unchanged.

6,144 H8/real-PCM comparisons cover all 24 channels and 256 field variants,
checking eight state words and all PCM ram2 registers. ROM-free tests cover
both branches and delay cases. Evidence: `/tmp/sc55-prepared-activation-1.log`;
all seven CTests pass. This operation is the final part of 5777, not the whole
note start: readiness scheduling, preceding sample/control register writes,
key-mask updates and provenance of the -3b flag still need integration.

## Full prepared PCM commit

`CommitPreparedVoice` extends that operation back to 5777. It clears the owned
C8B3/CB30 bytes, selects the logical channel, writes PCM1e, the three packed
sample-address fields (05/06, 09/0a, 0d/0e), then PCM12/14/1c/1a/16 before
calling the final activation branch. PCM1c is prepared with voice68 in the high
byte and voice66 in the low byte. Inputs are explicit native values, not ROM
pointers. Invalid logical channels produce neither state changes nor I/O.

The 6,144 isolated comparisons now execute the entire 5777 routine up to its
return instruction, comparing both PCM ram1 and ram2, activation state words,
and both cleared flags. ROM-free real-PCM tests additionally check the exact
26-byte write sequence and invalid-channel rejection. All seven CTests pass;
`/tmp/sc55-prepared-commit-1.log` records the H8 comparisons and its CSV is
byte-identical to `/tmp/sc55-prepared-activation-1.csv`.

This commits already computed values; it does not yet connect their native
producers to a persistent note scheduler. Readiness retries, key-mask updates,
remaining envelope/control tasks and production renderer integration are still
required. No control-ROM-free audible instrument or host CPU improvement is
claimed by these offline tests.

## Prepared voice key masks

`VoiceKeyMask` owns CB24/26 (enabled) and CB28/2a (prepared batch).
`RemovePreparedVoiceKeys` implements 573f..576f: nonzero CAF4 cancels without
state or device changes; otherwise enabled &= ~prepared, four big-endian bytes
are written to PCM00..03, and a read of PCM00 commits the pending hardware mask.
The prepared batch remains intact. `EnablePreparedVoiceKeys` implements
582e..5854: enabled |= prepared, the same write/read transaction, then prepared
is cleared. The firmware cache retains 32 bits even though PCM masks to 28.

6,144 isolated H8/PCM cases exercise both primitives, checking cached bits,
hardware active/pending masks and the update flag. Cancellation is included;
the independent enable test also follows cancelled removal fixtures, not as a
claim that the real caller proceeds after cancellation. ROM-free tests check
the write/read order, cancellation without I/O, and hardware mask truncation.

Caller evidence at 559e..55c1 is: wait on each of two voices, remove the batch
once, commit each voice via 5777, then enable the batch via 582e. This provides
the ordering for a future native batch owner; post-enable 580a, earlier input
generation and task resumption remain to be connected. These primitives alone
are not a native note scheduler or a ROM-free instrument.

Validation: `/tmp/sc55-prepared-key-mask-2.log`, all seven CTests passing, and
the full CSV byte-identical to `/tmp/sc55-prepared-commit-1.csv`.

## Post-enable PCM key latch

`PollVoicePostEnable` implements one iteration of 580a..582d. A nonzero voice65
skips all I/O. Otherwise it selects the voice and reads PCM1e via 3a/3b; until
bit5 is set it returns pending with no control-register writes. Once ready it
writes ff00 to PCM1a, voice-16 logically shifted right one to PCM36, then
voice26 to PCM1a. Invalid logical slots are rejected without I/O. The native
caller must only retry pending work, advance PCM time, and reselect the channel;
unlike the H8 busy loop this function never spins. A future scheduler must
preserve timing/ownership across that suspension, which is not proved here.

PCM confirms bit5 is the key latch set by a processing pass. The ROM-free timed
test now uses `EnablePreparedVoiceKeys`, observes pending, advances the original
PCM pipeline, observes ready, then runs the existing native envelope lifetime.
It still uses a synthetic clock and zero wave memory, not a validated SC-55 tone.
24 register tests additionally cover bypass, pending, retry after selecting a
different channel, exact write order and invalid inputs. 6,144 H8/PCM cases
compare the full ready/bypass path or one pending loop including PCM ram2,
read/write latches and channel selection. Evidence: `/tmp/sc55-post-enable-1.log`;
all seven CTests pass, and its CSV matches `/tmp/sc55-prepared-key-mask-2.csv`.

The batch owner and computed input producers remain disconnected from production;
these tests do not establish control-ROM-independent audio or CPU savings.

## Native prepared-batch continuation

`PreparedVoiceBatch` now connects the verified primitives in the ordering at
559e..55d7 (two voices) and 560e..5632 (one voice). It copies one/two prepared
entries and reserves their mask bits, polls reuse in order with a persistent
cursor, rechecks the first voice for cancellation, removes the batch mask once,
commits each voice, enables the batch once, then runs post-enable polls if the
first voice's -3b bit7 requests that branch. Finished/cancelled tasks do no more
I/O. Duplicate/out-of-range slots, empty/oversized batches, an active task and
an occupied prepared mask are rejected on begin. Cancellation leaves prepared
bits intact, matching the helper; the outer scheduler must own subsequent reset.

The external voice array, mask and PCM must have one owner. In particular,
post-enable suspension replaces an interrupt-masked firmware spin: it must
allow PCM time to advance without letting another task reassign these slots or
change their control state. This ownership requirement is explicit, not yet
implemented in the production scheduler. Entry snapshots also assume prepared
inputs remain stable through this phase. This is not the earlier note-input
generation or 56b5 task dispatch.

ROM-free tests exercise one/two voices, both flag branches, waiting on the second
voice without rechecking a passed first voice, no repeated post-enable writes,
terminal idempotence, input rejection, immediate cancellation and cancellation
of the first voice while waiting on the second. The timed real-PCM fixture now
uses this owner to start two voices, advances 2,500 device cycles, finishes the
key-latch wait, and continues the existing first-voice envelope lifetime. MCU
PC/cycles remain zero. All seven CTests pass. The constituent H8 comparisons
remain available; this newly composed continuation has not yet been compared
against the complete H8 caller across timed suspensions. Wave memory is still
zero-filled and the clock synthetic, so sound/timing fidelity remains unproven.

## Owned sample-plan connection

`PreparedVoicePcm` now embeds the existing `SampleAddressSetup` and calls its
verified writer instead of duplicating split address fields. `PreparePartialSamplePcm`
bridges a native partial/sample plan and owned sample bank to that input, preserving
normal versus unoffset start, bank/history/control bits and the separate loop flag.
Special sample IDs, missing descriptors/addresses and invalid slots are rejected;
their separate firmware path is still not implemented. The caller must keep the
plan and bank from the same sound-data generation and supply history state.

A ROM-free integration fixture now performs partial pitch/key resolution, sample
selection, descriptor decoding, PCM preparation and native batch commit, checking
the real PCM address RAM and mode. Its synthetic descriptor crosses a 16-bit carry
and covers both start modes, nonzero history/bank bits and special-ID rejection.
This still leaves pitch/filter/envelope control producers to connect; it is not an
audible full note test. All seven CTests pass. The refactored commit continues to
match 6,144 H8/PCM cases and the full trace is byte-identical to the previous run:
`/tmp/sc55-sample-commit-1.log` and `/tmp/sc55-post-enable-1.csv`.

## Amplitude continuation across batch commit

`PrepareVoiceAmplitude` transfers the prepared first-envelope stage, position,
delay accumulator and cached PCM18 command into the voice-start state without
overwriting other envelopes, pitch, saved stage or allocator flags.
`ContinueVoiceAmplitude` returns a runner using the actual committed values,
preserving the original setup, current level, deferred ticks and segment
parameters. It accepts ordinary amplitude stage codes 0..12 even, or 22
(finished), and rejects stop/special dispatcher codes. The caller invokes this
handoff once after successful batch completion, not while pending/cancelled.
This is a boundary transfer, not a second long-lived envelope owner.

32 ROM-free cases compare the handoff against existing activation behavior
across the two -3b branches, zero/nonzero delay and all eight runner stages;
five invalid codes are rejected. The real-PCM timed two-voice fixture now seeds
batch amplitude state from its runner and continues the first voice from the
batch result, instead of independently calling activateNewVoice afterward.
The subsequent envelope/release test passes with MCU PC/cycles still zero.
All seven CTests pass. This does not yet supply the production control clock,
other envelopes, or native-only audible rendering; full caller timing fidelity
is still unverified.

## Final pitch correction

The PCM10 producer is the single voice48 write at 5364. Its last stage,
534b..5364, adds the signed voiceA6 correction to the unsigned C8B0 reference
with saturation. `ApplyPitchOffset` now exposes this native calculation, with
explicit sign conversion rather than implementation-dependent narrowing.
ROM-free tests pass its result through `CommitPreparedVoice` into real PCM10
for zero, both sign boundaries and saturation cases.

The older `sc55_tables.h::PitchWord` already computes the C8B0 reference from
24-bit accumulator/reference inputs, but is not the whole producer: 527c..5347
updates the cached A6 correction when the source byte changes. The upstream
accumulator, modulation source and that cache update still require integration.
Do not mistake this final addition or the older trace comparisons for a complete
native pitch scheduler. PitchWord's floating table evaluation and signed-shift
implementation also need review before use in the new persistent audio path.

Validation: all 65,536 correction bit patterns at seven reference boundaries
(458,752 cases) match H8 execution from 534b through the voice48 store.
`/tmp/sc55-pitch-offset-1.log` records the comparison; its full CSV is identical
to `/tmp/sc55-sample-commit-1.csv`. All seven CTests pass.

## Pitch correction quotient

`PreparePitchOffset` implements 5326..534b: (source-128)*65536/divisor,
truncated toward zero, with magnitude clamped to 32767. H8's BGE after DIVXU
handles both a signed-negative 16-bit quotient and unsigned quotient overflow;
both correspond to that clamp. Zero divisor is rejected, since the preceding
firmware branch guarantees a nonzero divisor and a CPU divide trap is not a
native audio operation.

The ROM-free PCM test feeds all 256 source bytes at four divisors through this
calculation, final pitch addition and actual PCM10 write. It includes centered
zero, both signs and saturation. This does not yet calculate the divisor from
the voice's 24-bit pitch reference (528f..5326), or own the source-change cache
at 527c..528f. Those are still needed before connecting the full producer.

Validation: 131,072 H8 comparisons (all source bytes at divisors 1..256 and
65024..65279), `/tmp/sc55-pitch-prepare-1.log`; the complete PCM CSV matches
`/tmp/sc55-pitch-offset-1.csv`, and all seven CTests pass.

## Pitch correction cache and divisor

`sc55_pitch.h` now composes 527c..5367. `PitchConversion` generates the existing
formula-based 47 coarse and 256 fine values on construction (preparation thread),
then converts signed 24-bit deltas using only bounded integer operations. The
divisor path subtracts 81000 (013c68) from the reference with 24-bit wrapping,
performs octave/table conversion, and floors zero to one. No control ROM lookup
is retained. This new path avoids the old PitchWord signed-left-shift expression.

`PitchCorrectionCache` owns A4/A6 and refreshes the quotient only if the incoming
source byte differs. A changed reference alone intentionally does not invalidate
it. The final saturated addition always runs. 6,144 full H8 comparisons cover
all 256 source bytes, hit/miss branches and twelve reference values, including
octave boundaries, 24-bit wrap, extreme downshift and divisor-one saturation.
They compare cached source, cached correction and the resulting voice48 word.
ROM-free tests cover cache retention/refresh and pass the result to actual PCM10.

The earlier 51d5 accumulator update/reference conversion and its upstream
modulation producers are not yet connected to this cache, nor to production
control scheduling. Constructor preparation and immutable table lifetime must
be owned outside the audio callback when integrating it.

Validation: `/tmp/sc55-pitch-cache-1.log`, all seven CTests passing, and the
complete PCM CSV byte-identical to `/tmp/sc55-pitch-prepare-1.csv`.

## Persistent pitch accumulation

`VoicePitch` connects 51d5..5367: add the caller's increment with 24-bit wrap,
convert accumulator-reference-12000 to the unsigned pitch reference, refresh
the source-keyed correction when required, and return the saturated PCM10 word.
The implementation uses unsigned wrapping and explicit signed conversion, not
signed left shifts. The caller still calculates the increment and owns the
control clock, source selection, initial accumulator and reference.

64 persistent fixtures of 256 ticks (16,384 updates) execute the whole H8 range
without resnapshotting native state between ticks. They compare accumulator,
cached source/correction and PCM word, including negative increments, 16/24-bit
carry, sign-boundary crossings and cache retention over repeated source bytes.
ROM-free tests also feed the returned word into the real PCM commit and verify
octave/overflow boundaries. All seven CTests pass. This replaces the downstream
pitch calculation, not the earlier modulation/portamento increment generation;
production still executes H8 and no audio fidelity or speedup is claimed.

Evidence: `/tmp/sc55-pitch-advance-1.log`; complete PCM trace byte-identical to
`/tmp/sc55-pitch-cache-1.csv`.

## Pitch increment decay

`DecayPitchIncrement` implements 519b..51d5. It forms elapsed(AC5A)*rate(R6),
subtracts its low 24 bits from the signed increment's magnitude, clamps if the
wrapped result has bit23 set, then restores sign. This retains H8 wrapping for
oversized products rather than substituting wide signed saturating subtraction.
The caller bypasses this for an already-zero stored increment at 5175..5189;
rate selection through voice-3a and the table at 7a32 is not connected yet.

512 composed H8 fixtures execute 519b..5367 and compare the updated increment,
accumulator and resulting PCM pitch word, across both signs, carry/borrow,
extreme products and zero elapsed/rate. Native code feeds the decayed value
into `VoicePitch::advance`. ROM-free tests cover ordinary decay, zero crossing,
and an oversized product that wraps to a positive remainder. Upstream generation
of the initial increment, rate ownership and production scheduling remain open.

Evidence: `/tmp/sc55-pitch-decay-1.log`, all seven CTests passing; the full PCM
trace is byte-identical to `/tmp/sc55-pitch-advance-1.csv`.

## Glide rate selection and persistent state

The source pointer at voice-3a is set at 4f69..4f71 to AB26 plus the part
index from C8E4. At 5190..5197 its byte indexes the 128-word table at 7a32;
7b32 starts unrelated dispatch data. No analytic replacement of this table is
established. `PitchGlideRates` is owned data supplied to the native operation,
not a firmware pointer. Adding these rates to the standalone sound-data format
is still required; the H8 oracle currently imports them for comparison only.

`PitchGlide` composes 5175..5367 with retained increment and `VoicePitch` state.
A nonzero increment selects the current rate and decays before accumulation;
zero bypasses the lookup. Invalid indices reject nonzero work without mutation,
while zero increment preserves the firmware's no-lookup behavior. 1,536 H8
cases cover all 128 rates, zero/positive/negative increments and four elapsed
values, checking retained increment, accumulated pitch and output. ROM-free
tests use synthetic rate data to exercise persistent decay-to-zero and invalid
inputs. This is not a substitute for importing the actual rate data, nor for
native part-state mapping, initial increment generation or production integration.

Validation: `/tmp/sc55-glide-1.log`, all seven CTests passing, full PCM trace
byte-identical to `/tmp/sc55-pitch-decay-1.csv`.

## Data-only glide rates (MD06)

`SoundData` now supports SC55MD06: MD05 fields followed by 128 big-endian rate
words before the sample-bank payload. MD01..05 remain readable and expose no
glide rates; absent values are not fabricated. Invalid/truncated imports preserve
the previous valid storage. The export boundary imports ROM1 7a32..7b31 once
and verifies the encoded/decoded words. Runtime getters own the complete table
and retain no control-ROM pointer. This is research sound data, not plugin state.

`/tmp/sc55-v121-melodic-6.sdata` was exported (89,951 bytes). The separate
sound-data test executable loads that file without an emulator/ROM loader and
runs 2,048 native glide updates across all 128 actual rates, along with existing
sample/envelope tests. MD05 still loads. All seven CTests pass, including MD06
round-trip, truncated payload, unsupported version and dependency checks.
The asset is ROM-derived and remains outside the repository; this does not make
it freely redistributable. Initial glide generation, native part state and
production integration remain necessary for the full ROM-free instrument.

## Glide preparation at part-pitch changes

`PitchGlide::prepare` implements 49aa..4a47. Flag bit5 clears the old increment;
with a non-ff C974 source key it replaces it by key*1000-currentPartPitch.
With ff, bit7 decides whether to leave zero or take the old/current difference
path. Without bit5, that difference is added to the existing increment. The
bit7-clear difference path also subtracts it from the pitch accumulator; the
bit7-set path preserves the accumulator. Arithmetic wraps at 24 bits throughout.
The earlier calculation of the current part pitch and provenance of the raw
flag/source inputs remain external; these names do not imply generic MIDI
legato behavior without tracing their producers.

8,192 H8 cases execute 49aa..4a47 (all flag bytes, four source-key boundary
values, eight 24-bit field fixtures), comparing increment and accumulator.
ROM-free tests cover replacement, clearing and accumulator adjustment. The
MD06 data-only glide test now initializes through this operation rather than
injecting a precomputed increment, then performs its 2,048 rate-driven updates.
This is still not the complete note-on pitch preparation or production owner.

Validation: `/tmp/sc55-glide-prepare-1.log`, all seven CTests passing; complete
PCM trace byte-identical to `/tmp/sc55-glide-1.csv`.

## Part pitch fine and random adjustments

`ApplyPartPitchFine` implements 4927..4955: subtract 64 from the partial's
fine parameter and apply ten pitch units per step. `ApplyPartPitchRandom`
implements 4966..49aa: signed PCM random byte, absolute magnitude multiplied
by depth, rounded with +128 before dividing by 256, then multiplied by ten.
Both use 24-bit arithmetic. Negative branches clamp a negative wrapped result
to zero; positive branches wrap without that clamp. In the random negative
branch the clamp still runs when depth or rounding makes the adjustment zero.
Using the scaled adjustment's sign loses that distinction (regression fixture:
pitch ffffff, random 80, depth 0; H8 returns zero).

The oracle compares 1,024 fine and 4,096 random cases, including zero depth,
both signed-byte extremes and wrapped initial pitches. ROM-free regression
tests check the zero-magnitude sign branch. All seven CTests pass; full probe
run `/tmp/sc55-part-pitch-fixed.log` succeeds with the PCM trace byte-identical
to `/tmp/sc55-glide-prepare-1.csv`.

The random byte remains an explicit input. The two PCM34 read transactions at
4955..4966 (and the separate cached byte from the second read), upstream scale
tuning, and the production part-state owner are not yet implemented here.
These helpers do not establish a working control-ROM-free instrument.

## PCM-backed part pitch preparation

`PreparePartPitch` now composes 4927..49aa, returning the new part pitch and
the second random byte as owned values. It selects PCM channel 30 once, reads
34/3a/3b twice in that exact order, applies the first high byte as signed random
pitch input, and retains the second high byte for voice -3c. Both low-byte reads
are preserved. The caller copies the pitch to its current/reference fields and
passes it to `PitchGlide::prepare`; no H8 register or ROM pointer is retained.

4,096 oracle cases execute the entire 4927..4a47 sequence using the real PCM
register implementation. Comparisons cover both pitch copies, cached byte,
glide increment/accumulator, selected PCM channel and read latch. Separate
ROM-free tests assert the exact six-read/one-write transaction with different
first/second values, then exercise the real PCM registers and glide connection.

The oracle executes without clock advancement between instructions. It proves
the register transaction and arithmetic, not the eventual native scheduler's
cycle timing. `PCM_Read(34)` latches channel RAM2[10]; the PCM update's original
mixing path advances that shift register. The native bus must own this PCM
state and preserve read ordering; replacing the pair with a host RNG or one
cached read is not equivalent. Upstream scale tuning and production part-state
ownership remain pending.

Validation: all seven CTests pass; `/tmp/sc55-part-pitch-pcm-1.log` completes,
and its full PCM-write trace is byte-identical to `/tmp/sc55-part-pitch-fixed.csv`.

## Part pitch base and table correction arithmetic

`PreparePartPitchBase` implements 486a..48d0 from explicit owned inputs:
partKey*1000+partTune, sampleKey*1000+0400-sampleTune, and that first reference
+0400-alternateTune. Reference arithmetic wraps at 24 bits, including negative
results. The two descriptor tuning words are cumulative for the alternate
reference, not independent corrections of sampleKey. The preceding copy of
the previous part pitch (485c..486a) remains the caller's responsibility.

`ApplyPartPitchKeyCorrection` implements 48f9..491f for a supplied table word
centered at 8000. Negative corrections clamp if the wrapped result is negative;
positive corrections wrap. Selector zero at 48dd bypasses lookup and correction.
This is not yet identified as the user-visible GS scale-tuning parameter.

The linear assembly listing loses instruction boundaries at 48e9. Raw bytes
show 48e9 `ldc #3,ep`, 48ed a word-pointer lookup at dd32+2*selector, 48f1
adds that pointer to the doubled part key, 48f3 reads the correction word, and
48f5 restores EP zero. ROM2 file offset 3dd32 contains the pointer table;
the table's valid selectors/key ranges still need validation and an owned-data
export before the whole preparation path can run without ROM lookup.

4,096 H8 comparisons cover base/reference preparation with all byte keys and
tuning boundaries. 327,680 comparisons cover every correction word at five
initial pitches. ROM-free CTests cover cumulative references, 24-bit underflow,
and signed table corrections. These are arithmetic checks, not a production
ROM-free engine or audio-fidelity result.

Validation: all seven CTests pass; `/tmp/sc55-part-pitch-base-1.log` completes
and its full PCM trace is byte-identical to `/tmp/sc55-part-pitch-pcm-1.csv`.

## Owned part-pitch key tables (MD07)

`PartPitchKeyTables` expands the 40-entry pointer directory at 03:dd32..dd81
into 40 rows of 256 big-endian words. Row zero is unused/zero-filled: selector
zero bypasses the table entirely. `ApplyPartPitchKeyTable` performs 48d8..491f
without ROM pointers, rejects selectors >=40, and preserves all byte-key
addressing. Expanding keys 128..255 reproduces firmware addressing beyond
ordinary MIDI keys; it does not assert those are meaningful musical inputs.
The directory ends where the next data starts at dd82; this boundary is not
a claim that every selector is exposed by a MIDI parameter.

SoundData MD07 appends 20,480 bytes after the MD06 glide rates and before the
sample bank. Older MD01..06 remain readable and report no pitch-key table.
Malformed/truncated imports preserve the last valid owned asset. Export occurs
off the audio thread and resolves bank-3 word pointers once; runtime lookup is
bounded integer arithmetic without allocations or emulator access.

30,720 H8 comparisons execute the complete lookup/correction block for all
40 selectors, all 256 keys and three initial pitches. Synthetic serialization
tests compare every word, test truncation and invalid selectors, and verify
MD06 backward loading. Export `/tmp/sc55-v121-melodic-7.sdata` is 110,431 bytes.
The separately linked sound-data test opens only that file and performs 10,240
native lookups, 2,048 glide updates and 76,800 composed envelope completions.
The prior MD06 real asset also passes. ROM-derived assets remain outside git;
this export is not a redistribution license or a complete ROM-free product.

Validation: all seven CTests pass. `/tmp/sc55-pitch-keys-1.log` completes;
its full PCM-write trace is byte-identical to `/tmp/sc55-part-pitch-base-1.csv`.

## Persistent composed part-pitch preparation

`PreparedPartPitch` now owns the persistent results of 485c..4a47: current
part pitch, both sample references, cached random byte and the glide state.
Its `prepare` composes base/reference calculation, optional owned key table,
fine/random PCM transaction and glide preparation using the previous stored
part pitch. The firmware's duplicate pitch field has the same value and is
not stored twice in native state. Shared C8AE/C8B2 scratch is replaced by the
old owned value. Raw controller/descriptor fields enter through `PartPitchInputs`;
their upstream MIDI and sample-bank producers are still a separate integration.

A nonzero key-table selector requires an available valid table. Missing tables
or selectors >=40 return false before state mutation or PCM access. Selector
zero succeeds without tables. Callbacks must not throw and the caller must
serialize PCM/state access; this is not a thread-safe publication mechanism.

The oracle executes the entire 485c..4a47 instruction sequence for all 40
selectors and 256 flag values, with four consecutive changes per persistent
state (40,960 updates). Native state is not re-seeded from H8 between updates.
It compares both reference fields, both firmware pitch copies, random cache,
glide accumulator/increment, selected PCM channel and read latch. ROM-free
tests exercise actual PCM registers, selector-zero bypass and failure without
callbacks/state mutation. These tests still do not advance the PCM clock
between H8 instructions or demonstrate host audio fidelity.

Validation: all seven CTests pass; `/tmp/sc55-pitch-composed-1.log` completes
and its full PCM trace is byte-identical to `/tmp/sc55-pitch-keys-1.csv`.

## Owned sample-to-pitch input bridge

`PreparePartialPitchInputs` connects a `PartialSamplePlan`, its owning patch
and sample bank to persistent pitch preparation. It reads sample descriptor
bytes 11, 12..13 and 14..15 as key/two tuning words, patch common[7] (record
+13 hex) as key-table selector, and partial[11]/[12] as fine/random depth.
Part key/tune, flags and source key remain explicit controller-state inputs.
Invalid partial indices, unused partials, missing samples and special IDs are
rejected. Plan/patch/bank must belong to the same immutable asset generation;
the helper does not infer generation or resample a different zone.

The live oracle inspects 485c entry and resolves the actual three firmware
pointers to owned patch/partial/sample IDs, then compares all six extracted
fields with the actual firmware source bytes. This verifies pointer ownership
and offsets independently of the synthetic RAM fixtures used previously.
The data-only executable runs real MD07 asset selection through this bridge
and `PreparedPartPitch::prepare` for 38,362 normal-sample cases, using synthetic
part controls and a zero-valued PCM bus. It verifies no H8 or control-ROM read
is needed for that composition, not timing or audible fidelity. Special-sample
handling, controller producers and production integration remain pending.

Validation: 3,442 live input mappings match in `/tmp/sc55-pitch-input-2.log`;
the full PCM trace is byte-identical to `/tmp/sc55-pitch-composed-1.csv`.
All seven CTests pass, as does the separate MD07 sound-data executable.

## Pitch-envelope target arithmetic

`PreparePitchEnvelopeTarget` implements 4ada..4b23. The incoming signed byte
has already been centered/inverted by 4a5a..4aa9; depth comes from 4a86..4ad7.
Lookup uses the absolute byte magnitude in the caller-owned curve, then the
adjustment is `(depth*curve[magnitude] >> 7) & ffff`. Negative offsets subtract
and clamp if the wrapped 24-bit result is negative, including zero adjustment.
Positive offsets add with 24-bit wrap. The initial implementation incorrectly
dropped the low product word's high byte: H8 MOV.B preserves it, and SWAP moves
it into the low byte of the resulting adjustment. The corrected version retains
both bytes. ROM-free regression checks include that fractional-byte boundary.

3,840 H8 cases cover all signed offset bytes, five depth boundaries and three
base pitches. The current oracle supplies the original curve at 79f2; the
helper itself only takes an owned span. Depth generation, curve export and
the remaining pitch-envelope stages are not yet connected to this helper.
This target calculation is not a complete pitch-envelope implementation.

Validation: all seven CTests pass; `/tmp/sc55-pitch-target-2.log` completes
with the PCM trace byte-identical to `/tmp/sc55-pitch-input-2.csv`.

## Pitch-envelope depth and centered levels

`PreparePitchEnvelopeDepth` implements 4a5a..4ada. It centers partial 12..16
at 64 as wrapped signed bytes, inverting all five when partial22 is below64.
The partial10 depth table value is returned unchanged for sensitivity64.
Otherwise factor is the low16 bits of base[abs(sensitivity-64)] plus velocity
times coefficient[abs(sensitivity-64)]. Depth is the high16 bits of the depth
table value times that factor, rounded by adding8000. Carry/wrap is preserved,
not replaced with wide saturation. Input velocity is the C914 slot byte;
its ultimate controller producer is not inferred here.

Owned `PitchEnvelopeDepthTables` currently expands raw byte-parameter accesses
at 78c6,78dc and78f2. The larger arrays preserve out-of-musical-range addressing
for differential tests; their dimensions do not establish supported parameter
ranges or the boundaries of the original logical tables. MD07 does not yet
export these arrays or the target curve. Runtime helpers accept data only.

262,144 H8 cases cover all sensitivity/depth bytes and four velocity boundaries,
compare the depth and all five signed levels, then execute 4ada..4b23 and
compare the first composed target. ROM-free tests explicitly verify negative
sensitivity, neutral bypass and low-word product wrapping/rounding. Full
pitch-envelope timing and the other target/stage connections remain pending.

Validation: all seven CTests pass; `/tmp/sc55-pitch-depth-1.log` completes
and its full PCM trace is byte-identical to `/tmp/sc55-pitch-target-2.csv`.

## Five pitch targets and initial direction

`PreparePitchEnvelopeTargets` composes 4a5a..4cb7: depth/offset preparation,
all five target calculations, first-target random correction and initial
direction. Every target uses the same base pitch, not the preceding target.
The cached second PCM random byte applies only to target0, using partial11;
the existing signed/rounded random helper reproduces 4c53..4c91. Firmware then
copies that first target to the accumulator (2d:46) and segment-start (2c:44).
The returned plan leaves installing those owned state fields to its caller.
Direction is zero when target0 < target1 (unsigned24), otherwise2, including
equality. This does not yet implement segment timing or stage transitions.

The existing 262,144 depth fixtures now continue through 4cb7, checking all
five 24-bit fields, the two initial-state copies and direction. They vary
all sensitivity/depth bytes, four velocities, five independent level offsets
and cached random values. ROM-free tests check that random correction changes
only the first target, that equal targets choose direction2, and that negative
random correction chooses direction0 when appropriate. Tables still need
independent-asset export and the running pitch-envelope owner remains pending.

Validation: all seven CTests pass; `/tmp/sc55-pitch-targets-1.log` completes
and its full PCM trace is byte-identical to `/tmp/sc55-pitch-depth-1.csv`.

## Owned pitch-envelope assets (MD08)

SoundData MD08 appends 1,536 bytes after MD07 key tables and before the sample
bank: 192 base words, 192 velocity words, 256 depth words (all big-endian),
then 256 target-curve bytes. These expanded raw-parameter views preserve the
addressing tested by the oracle; their sizes are not musical parameter limits.
`pitchEnvelope()` is absent in older versions; invalid/truncated imports keep
the previous asset. Encoding requires the preceding table sections. All load
and allocation remains preparation-thread work; runtime targets use owned data.

The v1.21 export `/tmp/sc55-v121-melodic-8.sdata` is 111,967 bytes. Every exported
pitch-envelope word/byte is compared after deserialization with the source
view. Synthetic tests cover whole-table equality, endian-sensitive values,
truncation, section dependencies, prior-table preservation and MD07 loading.
The separate executable reads only MD08 and prepares 38,362 normal-sample
pitch-envelope plans after sample selection and base-pitch preparation, then
installs the first target into its native accumulator. Inputs still use
synthetic controls/zero PCM random reads. No audible fidelity or running
pitch-envelope timing is established by this test. The real MD07 asset also
passes without inventing absent pitch-envelope data. ROM-derived assets stay
outside the repository; export does not imply redistribution permission.

Validation: all seven CTests pass; `/tmp/sc55-pitch-data8-1.log` completes,
with the full PCM trace byte-identical to `/tmp/sc55-pitch-targets-1.csv`.

## Pitch-envelope key-dependent time scales

`PreparePitchEnvelopeKeyScales` implements 4cbb..4d8c using the existing
`EnvelopeKeyScale` arithmetic and multiplier table. Pitch-specific curves
come from directories 03:dd42 and03:dd62, NOT the amplitude directories.
Partial1e selects the first curve with a low-nibble mask; partial1f selects
the second without masking. Partial20/21 provide sensitivities; the supported
44..84 range is explicitly validated. The caller supplies the C8FC slot key.
`PitchEnvelopeKeyCurves` owns both sets separately from amplitude data.

167,936 paired H8 cases execute the complete two-scale block: all256 keys,
16 first curves paired with reversed second curves, all41 sensitivities paired
with their opposite, and varied high nibbles of the masked first selector.
Comparisons cover voice-36 andvoice-34 words. ROM-free tests cover zero-point
negative inversion, neutral curve midpoint, ignored high nibble and invalid
release/sensitivity rejection. These two curve directories are not yet in MD08;
the timing runner and native controller producer remain pending.

Validation: all seven CTests pass; `/tmp/sc55-pitch-keyscale-1.log` completes,
with the PCM trace byte-identical to `/tmp/sc55-pitch-data8-1.csv`.

## Pitch velocity scaling and first progress increment

`PreparePitchEnvelopeFirstIncrement` composes 4d8c..4e2a. It reuses
`EnvelopeVelocityScale` for partial23 and the C914 byte, then looks up
partial17&127 in the existing 6f12 time table. `PreparePitchEnvelopeIncrement`
applies key scale then velocity scale using the same early high-word>=255
saturation as amplitude. A duration<=8 produces incrementffff; otherwise
the integer increment is 80000hex/duration. Saturated duration produces8.
The parameter's high bit does not affect this lookup. This is a prepared
increment, not a scheduler tick or an elapsed-time integration step.

335,872 H8 cases execute velocity scaling through the first increment write:
all41 supported sensitivities, all256 velocity bytes, eight segment parameters
covering high-bit aliases and four key-scale boundaries. Both velocity scale
and voice7c increment match. ROM-free tests cover duration8/9, saturation,
neutral sensitivity and invalid-sensitivity rejection. The real MD08 data-only
test also prepares an increment for all38,362 normal sample plans, using
neutral key scale256 until pitch key curves are exported. That synthetic
key-scale choice is test input only, not a production fallback.

Validation: all seven CTests pass; `/tmp/sc55-pitch-increment-1.log` completes,
with the PCM trace byte-identical to `/tmp/sc55-pitch-keyscale-1.csv`.

## Complete pitch-envelope timing preparation

`PreparePitchEnvelopeTiming` composes 4cbb..4f51 into owned key scales,
velocity scale and five reciprocal progress increments. Partial17..1a use
the first key scale; partial1b alone uses the second/release key scale. All
five use the same velocity scale and low-seven-bit time index. Invalid
curve/sensitivity input returns no plan without state mutation.

The key-scale differential test now executes the complete block through4f51
for167,936 cases, varying all byte keys, curve pairs, supported sensitivities,
velocities and distinct segment parameter bytes. It compares both key scales,
velocity scale and all five increment words at voice7c..84. ROM-free tests
use different key scales to ensure the fifth interval is not accidentally
treated like the first four, and test rejection of invalid plans. This is
timing preparation, not the running phase accumulator or stage transitions.

Validation: all seven CTests pass; `/tmp/sc55-pitch-timing-1.log` completes
with its PCM trace byte-identical to `/tmp/sc55-pitch-increment-1.csv`.

## Persistent pitch segment update

`PitchEnvelopeSegment::advance` implements 5060..50cf. Elapsed ticks plus
deferred ticks wrap16 before multiplying by increment. Progress clamps atffff
and excess progress is converted back to deferred ticks with integer division.
Increment zero cannot enter the division branch and safely holds position.
Interpolation uses a wrapped16-bit start/target distance, multiplied by progress
and shifted16; the result adds/subtracts from the full24-bit start. Any nonzero
direction takes the subtraction path. Even at progressffff the interpolation
may be one unit short: do not force target equality here, as stage dispatch
handles the subsequent transition separately. The returned pitch initializes
both the segment-result and accumulator fields before later modulation.

8,192 H8 updates retain native and firmware state across128 ticks in64 fixtures,
including zero/large/wrapping tick sums, zero increment, both directions and
24-bit-crossing endpoints. Both firmware result copies and position/deferred
ticks are compared. MD08 data-only tests now connect38,362 prepared target plans
to16 updates each (613,792 segment updates) without a control ROM. They keep
the previously documented synthetic controls/neutral key scale and do not
perform stage transitions or establish audio fidelity.

Validation: all seven CTests pass; `/tmp/sc55-pitch-segment-1.log` completes
with its PCM trace byte-identical to `/tmp/sc55-pitch-timing-1.csv`.

## Segment retargeting

`PitchEnvelopeSegment::retarget` implements5019..5040: prior target becomes
start, incoming24-bit target/increment replace the old values, and direction
is2 for descending or0 otherwise. Equal endpoints choose0 here, unlike the
initial preparation's2. Progress/deferred ticks are deliberately untouched:
the completed-stage dispatcher separately clears position and retains deferred
time. Reusing the interpolated current pitch as the new start would lose the
firmware's endpoint snap when progressffff is still one unit short.

196 H8 cases cover equal/cross-word/cross-sign endpoints and four increments,
then compose retargeting with5060..50cf advancement using seven deferred ticks.
ROM-free tests check equality direction and carry-over. The full stage/hold/
release dispatcher remains to be implemented, not replaced by this primitive.

Dispatch evidence for that next step: ROM1 7b32 table entries(index0..11) are
5040,5040,4ff8,5003,500f,5040,5040,5040,5040,5040,5040,5040;
7b62 entries are5367,5060,5060,5060,5060,5049,5060,5367,5367,5367,5367,5049.
The shared6ac8 next-stage codes are2,4,6,8,a,c,16,16,16,16,16,16.
Stage codes are byte offsets (twice these indices). Entry4fdb checks completion
before dispatch; 4f9e is a separate preparation/re-entry table at7b4a.

Validation: all seven CTests pass; `/tmp/sc55-pitch-retarget-1.log` completes
with its PCM trace byte-identical to `/tmp/sc55-pitch-segment-1.csv`.
