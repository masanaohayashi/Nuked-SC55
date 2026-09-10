# H8 capacity-selection control: missing native connections

The target is complete semantic H8 control replacement using the existing PCM
renderer as the sound-generation reference. Independent PCM renderer cleanup is
not counted as progress toward that target.

## Current recheck and attribution limitation

Latest user clarification supersedes the timed-product descriptions below:
the normal player now drains semantic control passes on the common cadence.
It does not turn H8 instruction counts into PCM-time waits. Actual PCM reuse
and key-latch handshakes remain unchanged. Timed calculations remain available
in the runtime for diagnostics, not as the normal product scheduler.
The last timed-product capacity run still differed at admission40 (66/67),
`/tmp/sc55-lfo-timed-capacity.log`. This is not by itself evidence of a reserve
rule violation or audible failure. The diagnostic assertion is retained, and
neither exact parity nor functional equivalence is claimed from that result.

### First and second LFO bodies connected, including the random latch boundary

The product's timed path now uses `ModulationCalculation` for both local LFO
blocks. Routing, follower relinking, shared copies and detach-stage checks still
belong to the existing voice-modulation owners. Their `prepareLocal` boundary
separates that responsibility from oscillator calculation without repeating it.
Shared blocks do not spuriously run a new local oscillator.

For random shapes, the calculation first waits to the PCM latch instruction:
3cbf/3ce9 reads E034, capturing channel30 RAM2[10]. The subsequent E03A word read
uses that latch. It is NOT a fresh sample at3cc3/3ced and must not be taken at
calculation entry. The input-derived work model now provides this prefix; at its
deadline the product samples `ReadControlRandom` once, calculates the result and
remaining work, then stays protected until completion. No IRQ/MIDI service runs
at this internal latch point. The result is published only at completion.

Actual-instruction H8 observation verifies10982 block state/work comparisons,
597 random-latch positions, and no extra/missing random reads. Observed shapes
are0,1,3,4,5; this is NOT dynamic H8 coverage of2 or6. The native PCM suite checks
all seven shapes against the existing immediate mathematical operation across
zero/small/large increments and positive/negative random values, including early
read rejection and single consumption. Logs `/tmp/sc55-lfo-latch-oracle.log`,
`/tmp/sc55-lfo-calculation-oracle.log`, `/tmp/sc55-lfo-timed-pcm.log`.

Product startup8/otherEG90/variable-block equality, release111, reserve48/protected
mono, boundary reception, command priority and MIDI during preparation pass in
`/tmp/sc55-lfo-timed-*.log`. The priority fixture observes the first readback of
the sole externally supplied pass; it no longer assumes LFO work finishes in one
frame. H8 observer/core/ROM and audio expectations were not changed.

This still is not the complete execution-time scheduler. Shared-routing work,
controller refresh, caller/gap instructions, PCM readback/publication, admission
and other CPU services remain uncharged. PCM-frame quantization of deadlines
also remains. No Xcode/Logic runtime or CPU-performance measurement is claimed.

### Default product now advances device time for four protected calculations

`ControlSlice::timedPhase` retains each parameter result for its input-derived
body instruction count times12 (the reference interpreter's cycle convention).
`NativeMelodicPlayer` advances that work with actual rendered PCM cycles and
includes completion in its next-service deadline. Before accepting IRQ/MIDI/
voice-control work it commits any completed protected calculation. While it is
unfinished, PCM and timer counters progress but control handlers remain deferred.
Zero-duration surrounding phases drain synchronously rather than adding one
invented sample delay per phase. The product default uses this path.

This is a PARTIAL execution-time connection, not a complete scheduler. It covers
amplitude/filter/pitch/level calculation bodies only. Caller instructions, LFO,
controller refresh, readback/publication and other CPU services are not yet
charged. PCM still advances in whole625-cycle passes, so completion can overshoot
a body deadline to the next PCM frame; that quantization is not claimed to match
H8 instruction timing. No measured average, recorded event schedule or change to
PCM DSP/key-latch protection was introduced. Earlier counterfactual replay logs
describe the preceding product and are not new results for this default.

The timed-runtime test checks all four bodies at duration-1 and duration, no I/O
before completion, and separately accumulated next-period events. PCM385tone,
release111, reserve48/protected mono, startup8 (101 other-EG updates), boundary
reception (100 other-EG updates), variable-block audio, and command-priority
fixtures pass. Normal capacity still fails admission40, retaining67 instead of66.
Logs `/tmp/sc55-timed-calculation-*.log`. CPU/Logic performance is unmeasured.

Two old test assumptions were updated, without changing voice/audio expectations:
startup may resume AFTER its timer deadline if protected work blocked dispatch;
the test now rejects early resumes and requires protected work for late dispatch.
The simultaneous-command fixture first drains warmup work and checks the first
scanned voice, rather than requiring the whole paired pass to finish in one frame.
The MIDI-during-preparation fixture waits for the actual reuse-wait entry before
sending CC, matching its H8 trigger instead of assuming entry after one sample.

### Product calculation results separated from publication

Runtime connection: `VoiceControlRuntime` now owns a pending slot/stage/result/
work record. Phase service calculates once, retains the result, and commits on
resumption. Whole-pass service drains the same phases; its bounded phase count
includes four calculate/commit pairs per voice. Preparation entry points defer
while a result is pending, rather than allowing its destination to be reassigned.
Runtime copies used by diagnostics copy value-owned results into independent
voice owners; they do not share a pending result pointer.

The phased PCM test now explicitly advances both halves. It changes input ticks
after calculation and compares with an independent unchanged snapshot at commit,
verifying no recalculation. Existing five pre-calculation stop gates, PCM output,
and pair publication assertions remain. Both targets build and PCM385tone,
release111, startup8/otherEG88/variable-block audio tests pass; logs
`/tmp/sc55-runtime-calculation-*.log`. Default service does not yet advance device
time between calculate and commit, so this is not the completed scheduler.

`VoiceParameterCalculation` in `sc55_voice_control.h` now evaluates the four
pure parameter operations (amplitude, filter, pitch and level) without mutating
the live voice. Its move-only variant contains only that operation's result;
commit updates only the owned fields and associated lifecycle cache. It holds no
caller pointers or CPU/PCM state. A move consumes the source, repeat commit is
rejected, and a destination that has stopped discards the old result. Destination
reassignment while pending remains prohibited by the audio owner's contract.

The real `CalculateVoiceControlStage` uses this path, replacing the former
in-place calculation branches rather than computing both paths. Shared LFO
updates retain their existing separate continuation/device contract. The normal
caller still commits immediately: no execution duration or inferred delay was
introduced, and the capacity timing discrepancy is not claimed fixed.

The four result evaluators and their cutoff-conversion dependency subsequently
moved from diagnostic headers into `src/backend/sc55_{amplitude,filter,pitch,
output,envelope}_work.h`. Diagnostic headers now import those implementations;
there is no separate product copy of the math. `VoiceParameterCalculation` uses
each evaluator once and retains its `referenceInstructions` beside the result.
Move transfers both, and committed/discarded results expose no pending work.
These counts cover the documented calculation bodies, not caller/IRQ/scheduler
cost. In particular the amplitude delay exit excludes its unmask/return tail.
They have NOT been installed as whole-voice delays.

Validation after that connection: `/tmp/sc55-work-owner-oracle.log` contains the
full value/work comparisons; `/tmp/sc55-work-owner-release.log` and
`/tmp/sc55-work-owner-startup.log` pass111 release comparisons and8 reuse waits/
88 unrelated EG updates/variable-block audio equality respectively. The separate
PCM suite is `/tmp/sc55-work-owner-pcm.log`, including result/work move ownership.
This does not establish host performance; unused work metadata may be optimized
away by the compiler, but no performance improvement is claimed.

Builds of both diagnostic targets pass (the CPU-roles target includes the product
emulator TU). The added pending-result tests plus existing PCM385tone, release111
and startup8/otherEG88/zero-1-127-257 block checks pass. Logs
`/tmp/sc55-parameter-calculation-{build,pcm}.log`,
`/tmp/sc55-parameter-{product-build,release,startup}.log`.
No host/format runtime or CPU-performance claim follows from these checks.

### Input-derived calculation work revalidated before product connection

The current diagnostic binary's `--native-controller-work` completed with exit0
(`/tmp/sc55-controller-work-current.log`). Full state/work comparisons include:
amplitude5802 (21 ends, no delay-stage coverage), filter5781 (87 budgets),
pitch5781 (661 moving-glide cases), level/output5781, modulation10982,
controller scaling5238. These verify the existing semantic work calculators on
this workload, not every possible input or the entire control-pass duration.

The product's five calculation stages are already distinct in
`CalculateVoiceControlStage`; the runtime owns readback, calculation continuation,
linked-voice traversal and publication. The existing diagnostic work calculators
evaluate a copy and return the resulting typed state. A product connection must
use that result once, not run the old calculation plus a second timing copy.
Likewise, mutating live state immediately and merely sleeping afterward is not
equivalent to an atomic protected calculation: device time must advance while
the completion result remains pending, with higher-priority control work admitted
only at verified interruptible boundaries. There must be no audio-thread sleep.

The existing stage-work observation distinguishes the five protected calculations
from the gaps between them; in the capacity trace each calculation's own time
equals its wall time, whereas the gaps include other control responsibilities.
This is a reason NOT to promote the five local budgets as the complete scheduler.
Controller refresh, first-LFO routing, entry/readback/publication, admission,
receive, device notification and display-service work remain part of the full
connection. None of these results authorizes a fitted whole-pass delay or changes
the default scheduling behavior. No work calculator was moved into the product
in this check.

### Replay observation corrected to actual instruction entry

The pass start/end collectors now run from `Oracle_H8Fallback`, after interrupt
dispatch, rather than sampling the next PC before `Step`. The observer is scoped
to each send with automatic cleanup and reads only the selected reference MCU.
This matters: the previous `end-clock-start` log counted three extra completion
opportunities (c2/00, the later90/31, and protected91/48). All three disappear at
actual entry. Do not use the old per-window pass counts as exact evidence.

Rebuilt and reran the four counterfactuals without changing H8 or product state:

| Mode | Result with actual-entry observation |
| --- | --- |
| end |48 admissions, protected rejection/recovery PASS|
| end-clock-start |48 admissions, protected rejection/recovery PASS|
| end-unit |Original admission40 survivor mismatch, exit134|
| end-clock |Original admission40 survivor mismatch, exit134|

The former failing window remains9 opportunities: observed-count end totals26,
native entry capture totals25, native completion capture totals26, unit totals9.
Thus the preceding conclusions survive removal of duplicate observations.
Logs `/tmp/sc55-{entry,end,unit,clock}-observer.log`; successful build
`/tmp/sc55-entry-observer-build.log`. These remain diagnostic schedules, not
product execution-time implementation or a measurement of host CPU performance.

### Native timer counts captured at entry pass the capacity fixture

`end-clock-start` extends the preceding `end-clock` diagnostic by consuming the
same native timer at observed5af9 entry, retaining that count until the observed
5b70 completion. New expirations remain in the clock for the next entry.
It never uses AC5A/H8 elapsed values as native control inputs. The shadow clock
and optional captured count persist across MIDI windows; empty entry events are
not manufactured, and overlapping nonempty captures fail the diagnostic.

With the native zero epoch retained, all48 admissions, protected-capacity
rejection and subsequent playback pass (exit0). In the former failing window
there are9 completion opportunities,25 elapsed periods,0 coalescing and0 empty
captures. Log `/tmp/sc55-elapsed-entry.log`; build
`/tmp/sc55-elapsed-entry-build.log` (exit0).

This removes dependence on observed H8 elapsed values for this fixture. It still
uses observed START and END opportunities, and computes the whole native pass at
the latter. It does not demonstrate within-pass device-read/write timing, full
audio parity, or default scheduler correctness. Do not ship the replay.

Source reinspection also corrects an earlier progress description: the product
ALREADY captures the clock before effects in `serviceWork` (`effectPassClock_`),
and `VoiceControlRuntime::serviceControl` retains `controlTicks_` for a pending
pass. The common clock continues accumulating separately. Do not add another
snapshot/accumulator or rename this existing behavior as new implementation.
The missing connection is input-dependent execution time and the start/resume/
completion of those existing semantic operations, not the counter arithmetic
or the existence of a captured-count owner.

### Native elapsed accumulation at completion is not sufficient

`end-clock` keeps observed H8 completion opportunities but removes the H8
elapsed-count input. A separate instance of the production `ControlTaskClock`
advances by the rendered native frames (625 device cycles each), persists across
MIDI windows, and consumes only at those opportunities. Its epoch starts at zero
like the native external-control setup. An opportunity with no expiration is
skipped, not converted to zero (which represents byte wrap, not absence).
Thus its actual dispatch set can differ from `end`; `emptyClock` reports this.

The first prototype rejected an opportunity before the native clock expired;
that was a diagnostic epoch assumption, not a product failure. The final
counterfactual preserves the native epoch and skips such empty opportunities.
It builds successfully but fails the original survivor assertion at admission40.
In that input window there are9 opportunities,26 elapsed periods,0 coalescing,
and0 empty opportunities: the same totals as successful `end`, yet H8 retains66
and native67. Earlier windows and per-pass count distribution need not match.
Log `/tmp/sc55-elapsed-clock.log`, build `/tmp/sc55-elapsed-clock-build.log`.

This rules out treating a window's total elapsed count, consumed at completion,
as sufficient. It does not identify a unique cause: initial phase and previous
history differ too. H8 captures the count at the control-pass entry (5af1/5af9),
before executing its work; the semantic continuation must retain that captured
count while subsequent expirations accumulate separately. Completion-time
collection is not a proposed implementation. Product behavior remains unchanged.

### Elapsed-count isolation on the same completion schedule

After the PCM notification reception changes, the same binary was run with
`SC55_REPLAY_CAPACITY_PASSES=end` and the new diagnostic-only `end-unit`.
Both select the same H8 completion observations at5b70 and render the same
intervals. Only the count supplied to `signalControlPassAudit` changes:
the observed byte versus one per observed completion. H8 state and input are
unchanged. The original survivor assertion is retained.

- `end`: all48 admissions, protected-capacity rejection and recovery pass (exit0).
- `end-unit`: fails admission40, H8 retains66/native67 (exit134).
- In the failing input window both schedules contain9 passes and no coalesced
  notifications. Their elapsed totals are26 versus9 respectively.

Logs `/tmp/sc55-elapsed-observed.log`, `/tmp/sc55-elapsed-unit.log`;
build `/tmp/sc55-elapsed-isolation-build.log` (exit0).

This establishes that elapsed-count batching matters even with the completion
schedule held fixed. Matching completion times alone, with one EG update unit
per completion, does not repair the survivor result. It does NOT establish that
timing is irrelevant, that an arbitrary larger count is correct, or that counts
alone at native default times would pass. The default remains uncorrected.
The semantic scheduler must retain timer expirations independently of pending
work and consume their accumulated count at the appropriate control boundary.
No recorded schedule, fitted delay, or forced unit count belongs in the product.

### Whole-pass execution time, after command-priority corrections

The current default still fails admission40 (H8 retains66, native67).
`SC55_REPLAY_CAPACITY_PASSES=end` on the same current code passes all48
admissions, reserve rejection and subsequent playback. It supplies observed
periodic completion times **and elapsed counts**, not native timing. This
supports investigating scheduling, not changing the allocator to choose66.
It does not prove whole-render parity or isolate timing from elapsed counts.

`ControlPassWork` partitions the complete5af9..5b70 interval at actual H8
instruction entry. It tracks hardware frames in all task contexts, not just8.
The idle scheduler's stack replacement at04af abandons task9's old frame;
other suspended task frames remain tracked until their RTE. No CPU/ROM/PCM
state is modified. Instruction cost12 is the existing emulator's assumption.

For146 completed passes starting with24 allocated voices:

| Executing responsibility | Device cycles | Share |
| --- | ---: | ---: |
| Voice control/task8 |46,106,796|68.60%|
| Display-related task7 |8,055,012|11.99%|
| Display-related task4 |2,158,464|3.21%|
| Hardware interrupt execution |6,983,460|10.39%|
| Non-IRQ scheduler execution |2,374,716|3.53%|
| Other task execution |1,529,328|2.28%|
| Unobserved remainder |0|0%|
| Total |67,207,776|100%|

Mean elapsed pass460,327 cycles; mean task8-own work315,800 cycles.
This is reference **device time**, not host CPU usage or the native plugin's
profile. The existing sampling estimate was close but could not establish
this exclusive partition. The new observer accounts for the whole interval.

Consequently, a voice-only work-duration model would still omit about31% of
the observed pass interval. The next scheduler implementation must connect
semantic voice operations with timer/receive/device/display service work and
their interruption boundaries. It must not replay these totals, add an average
delay, run JUCE GUI code on the audio thread, or reintroduce an H8 instruction
interpreter/kernel-task emulator into the normal path. Display-related
firmware work is not the same thing as painting the JUCE editor.

Commands used (ROMDIR is the local v1.21 directory):

```sh
SC55_REPLAY_CAPACITY_PASSES=end /tmp/sc55-cpu-roles-build/sc55-cpu-roles "$ROMDIR" --native-capacity-stealing
SC55_TRACE_CONTROL_ROUTINES=1 /tmp/sc55-cpu-roles-build/sc55-cpu-roles "$ROMDIR" --native-capacity-stealing
```

Logs `/tmp/sc55-current-capacity-end.log` (exit0),
`/tmp/sc55-pass-partition.log` (original capacity assertion, exit134),
`/tmp/sc55-pass-partition-build.log` (exit0). The unobserved remainder is zero
for every reported voice-count bucket. This turn changes diagnostics only;
it does not claim that product timing or audio was repaired.

### Later readback failure is an activation overlap, not an LFO failure

The readback replay adapter previously converted every non-advanced result to
`failed`, hiding whether the product actually failed or deferred. It now
preserves the original status; the assertion still requires `advancedPhase`.
The observer snapshots H8 owners at the readback event, not just group entry.

Re-running the stop/common-phase-aligned probe reaches90/42, frame167:

- Native `failed()==false`; actual result is `deferred` (5), not failed (4).
- Periodic phase remains firstModulation, selected slot20 stage4 on both sides.
- Native is in waitingForKeyLatch, reserved mask010000 (slot16).
- At this same readback event H8 slot16 is stage18, part1/key66; native's old
  runtime owner is stage14 while its new activation owns the key-latch wait.

This is another overlap caused by activation timing, not evidence of corrupt
first-LFO input. H8 has not reached the same activation point when it reads
slot20. Removing the native global key-latch guard would conceal the timing
discrepancy and risk the confirmed drum-attack regression: the initial PCM gain
commands must survive until the chip consumes them. That guard is unchanged.

Logs: `/tmp/sc55-readback-cause{,-owner}.log`, build
`/tmp/sc55-readback-cause-build.log`. Both diagnostic runs intentionally fail
the original readback assertion. No audio behavior or expected value changed.
Next timing work must cover subsequent admissions too; aligning only92/39 is
a controlled attribution experiment, not a replacement for the scheduler.

### Common phase is owned separately from pending control events

`ControlTaskClock` now exposes the next common-kernel tick and tick count over
a device-time span, derived from its stored phase. Reset accepts an explicit
validated phase; consuming an aggregated control event never resets it.
Native activation uses this phase rather than taking absolute PCM cycles modulo
the kernel period. Bulk-transfer spacing uses the same tick source.

Observed-event diagnostics continue advancing the phase while suppressing
automatic control events. Their explicit replay signal advances whole periods,
leaving the common phase intact. The activation-specific audit offset was
removed: the existing alignment diagnostic now changes the common phase itself.
Default zero-phase operation is preserved; no measured H8 boot phase or guessed
dispatch latency was installed as a product constant.

`/tmp/sc55-common-phase-{build,startup,reset,synth}.log`: product TU build,
eight reuse wakes/88 otherEG updates, bulk8 transfers1864-byte reset equality,
checksum3b54320560580fd3 and block partitions pass. The aligned diagnostic still
passes the earlier92/39 target and fails the same later90/42 readback. This
connects timer ownership but does not complete the execution-time scheduler.

### Controlled admission delay separates timer phase from execution latency

After the ownership changes, normal `--native-capacity-stealing` still fails
at admission40: H8 retains66, native67. Log:
`/tmp/sc55-capacity-after-ownership.log`. No allocator assertion was weakened.

New diagnostics hold only the target92/39 note's voice-command service, while
UART input, PCM and the existing observed control-phase replay continue.
`SC55_REPLAY_ADMISSION=entry` releases it at observed0bc1 (frame22);
`stop` releases at the observed slot8 stage18 transition (frame30).
These are diagnostic counterfactuals, not proposed product waits.

| Target92/39 case | Native zero gain | Reuse poll | Sounding stage | Readback at216 |
| --- | ---: | ---: | ---: | --- |
| UART/holds replay only |160|187|189|slot7/8 mismatch|
| Admission entry aligned |176|187|189|same mismatch|
| Stop transition aligned |176|187|189|same mismatch|
| Stop + common-tick phase aligned |176|203|205|passes this event|
| H8 |178.944|207.072|208.243|reference|

The previous comparison of post-zero waits (27 versus28 frames) did not prove
clock alignment. At this window, H8 cycles268000296 and native cycles148000000
have different epochs. H8 FRT index1 hasFRC209/OCRA312/TCR34; observed wraps
occur at frames10.598,42.547,74.592,106.752,138.701,170.746,202.906.
Native's modulo-kernel phase is3584 cycles, so its corresponding opportunities
are about16 frames later modulo the period. H8's actual poll at206.842 follows
the202.906 tick by about3.94 frames: timer phase and task dispatch latency are
distinct. Neither discrepancy authorizes adding a constant product delay.

`SC55_ALIGN_REUSE_TICK=1` aligns the diagnostic native activation phase to the
first observed H8 wrap. Together with stop alignment it removes the target
slot8 selection failure. The same unmodified readback assertion later fails
on90/42, frame167, slot20/native255, phase1: this is NOT full parity.
The matched earlier target therefore supports a causal timing explanation,
but does not validate a full scheduler or eliminate later ownership/timing work.

Logs: `/tmp/sc55-admission-timing-{base,entry,stop,epoch,aligned}.log`.
All experimental controls are compiled under `SC55_NATIVE_IO_AUDIT`; normal
activation arithmetic is unchanged. Default `--native-synth` passes with
checksum3b54320560580fd3 (`/tmp/sc55-admission-timing-default.log`).
Next implementation must make the common timer epoch explicit and account for
semantic execution/dispatch latency across admission and periodic work, rather
than replaying recorded H8 frames or adjusting a PCM readiness predicate.

### Product completion notification separated from allocator consumption

`VoiceControlRuntime` now retains the completed physical slot in a single
mailbox after `FinishEnvelopeTermination` / successful stop-level polling.
This corresponds to33d0/33dc publishing the slot and07d0..07e6 consuming it.
The envelope calculation no longer directly calls allocator.returnVoice.
Explicit phase execution returns with stage22/link detachment visible while
the allocation is still active; its next resume consumes the mailbox before
scanning another group. Whole-group execution drains the same notification
before returning, preserving the normal product's immediate behavior.

`/tmp/sc55-return-phase-pcm.log` exits0. A real allocated-slot fixture proves
freeCount23 and active allocation after natural EG completion, then freeCount24
only after consumption, with no duplicate return on pass completion. Existing
PCM control and stop/continuation tests pass. Product emulator TU build succeeds
(`/tmp/sc55-return-phase-product-build.log`, existing u8path warnings), and
`--native-startup-wake` exits0 (`/tmp/sc55-return-phase-startup.log`).

Correction: completion does NOT have priority over queued commands. ROM task1
drains event0's command ring before selecting event1's completion mailbox.
The product now retains the mailbox while commands/admission/fanout remain;
the earlier unconditional consumption at MIDI/command entry was removed.
Full elapsed notification latency remains unresolved. No product wait, PCM
algorithm, Xcode project, install or host validation changed.

### Product second-LFO continuation ownership

`CalculateVoiceControlStage` accepts an optional retained modulation operation.
Only the shared-source detach path suspends it; normal local/shared paths keep
their existing behavior. `VoiceControlRuntime` owns that continuation, refreshes
the source's live stage before routing, and holds the calculation stage until
resume. Release-stage changes skip the old LFO update and proceed to amplitude;
a stop clears the continuation and follows the existing termination path.
The immediate helper still drains the same operation without suspension.

Normal group execution has a bounded20-step maximum including up to one detach
for each LFO owner; no PCM time or synthetic wait is inserted between steps.
The model is ready to retain these decisions but the complete execution-time
and event policy remains unfinished.

`/tmp/sc55-second-phase-pcm.log` exits0, with explicit detach/relink then
stage12/release and stage18/stop cases proving no stale depth, counter or
oscillator update. Existing whole/phase equivalence, PCM hold, five stop gates
and scheduled stop/reinstall cases also pass. The product emulator TU builds
(`/tmp/sc55-second-phase-product-build.log`, existing u8path warnings only),
and `--native-startup-wake` exits0 (`/tmp/sc55-second-phase-startup.log`).
No Xcode/Logic test, CPU improvement or repaired capacity-survivor claim.

### Product first-LFO continuation ownership

`PeriodicVoiceUpdatePass` now separates selection/controller refresh,
first modulation and paired first-modulation transfer before readback. The
native runtime retains `FirstVoiceModulationUpdate` across the first-LFO
detach window instead of constructing and consuming it in one stack frame.
The first-modulation phase refreshes live lifecycle stages on phase reentry;
if the saved stage changed, resume skips the old oscillator update and follows
the firmware's return behavior rather than failing the entire native synth.

Normal product group execution drains exactly the same phases synchronously
(maximum18 semantic steps). No emulated PC/registers, allocation, wall-clock
wait or fitted timing constant was introduced. The explicit phase entry can
now retain the detach decision until the next scheduler call. Second-LFO
detach continuation and the complete time/event policy still remain open.

Validation: `/tmp/sc55-first-phase-detach.log` exits0, including the existing
phase/whole-pass I/O comparison, real PCM hold, five stop gates and scheduled
stop/reinstall tests. A new runtime-level case detaches a dead first-LFO source,
stops the destination between calls, and verifies no depth/counter/oscillator
state advances on resume. This is a constructed scheduling regression, not
a claim that the earlier real-MIDI fixture reached first-LFO sharing.
Product emulator TU build succeeds (`/tmp/sc55-first-phase-product-build.log`;
two existing u8path deprecation warnings). `--native-startup-wake` exits0
(`/tmp/sc55-first-phase-startup.log`). No Xcode/Logic or CPU benchmark claim.

### First-LFO routing and paired state comparison

`FirstModulationRoutingProbe` captures3985 routing and standalone3d44 paired
transfers through the real task8 path. It invokes the product
`FirstVoiceModulationUpdate::begin` / `UpdatePairedFirstModulation`, compares
all destination modulation state and all24 source links. Local output depths
are recomputed, and local rate/timing/waveform configuration must survive.
Local routing is checked before39f6 controls, detach before39e1's interrupt
window, and shared/paired copying at the actual caller return.

The MIDI fixture's waveform search previously read partial[14] as first-LFO
mode; the product prepares it from patch.common[2]. The search now uses the
actual preparation source. This is a fixture correction, not a product change.
`/tmp/sc55-first-routing-chord.log` exits0 with local5238/paired573 state
comparisons. Shared/detached remain zero even with the capital-tone sharing
search and distinct-key overlap; they are not claimed verified or unreachable.
The earlier broader search gave local10282/paired1243 but used the incorrect
mode selector (`/tmp/sc55-first-routing.log`); it is not the final coverage gate.
First-routing instruction work and interrupt-window behavior remain open.

### Second-LFO routing, sharing and detach verification

`ModulationRoutingProbe` captures the semantic24-voice/source table at3a7a,
runs `RouteVoiceModulation`, and compares destination block state and every
source link at the local entry3b26, shared return3aea, or detach boundary3b11.
It also verifies input-dependent routing work. Shared copying preserves local
depths/rate/timing/waveform settings; detach redirects all former followers.
The following interrupt window and lifetime recheck are outside this boundary.

The original controller/capacity cases used only local routes. The fixture now
selects an actual capital tone with second-LFO sharing enabled, sends overlapping
same-key notes, releases one and lets the remaining voices continue through
real MIDI control traffic. `/tmp/sc55-modulation-routing-shared.log` exits0:
local11435,shared58,detached2 state/work comparisons PASS. No reference RAM was
patched. First-LFO routing and product scheduling remain outstanding.

### Whole modulation-block state comparison

The observer now also compares input-derived instruction work from
`modulation-work.h`, including depth delay/attack, rate limiting, waveform
generation and conditional random reads. The model consumes the same typed
block/rates/ticks and external random input; it never consumes a PC or observed
duration. It covers the local block only, not shared-owner selection/copying.

The original controller fixture passes6755 state/work comparisons,13 budgets
(`/tmp/sc55-modulation-work.log`). The fixture then selects actual capital
tones whose partial data names unobserved waveforms and sends real Program
Change/Note On/Off, rather than patching H8 waveform state. Expanded run
`/tmp/sc55-modulation-shapes.log` exits0:20539 state/work comparisons,38 budgets,
804 random reads. Observed waveform0/1/3/4/5 counts are15399/91/70/4757/222.
Waveforms2/6 remain unobserved in this run, not implicitly verified.
All other enabled control probes also pass. Product scheduling is unchanged.

`ModulationControlProbe` now captures real task8 calls at3b2c, including both
block owners, and compares all mutable depth outputs, delay/attack counters,
phase, held/smoothed values and waveform output at the actual return address.
It runs the existing native `ModulationBlock::advance` on captured inputs.
For random waveforms the external PCM word is captured at3cc3/3ced immediately
after the real read, not reconstructed from the expected output. The native
operation must request exactly the same number of random words (zero or one).
This checks the sampling decision but not native versus H8 PCM clock alignment.

`/tmp/sc55-modulation-control.log`, controller fixture, exit0:
6755 state comparisons PASS, sine3528 and sample/hold3227,461 random reads.
Existing amplitude/filter/pitch/output comparisons also remain enabled.
Other waveform shapes, sharing/selection logic and LFO instruction work are
not proven by this run. No product timing or PCM changes were made.

### Amplitude operation: stage, value, work and completion decision

`cpu-roles/amplitude-control-work.h` uses the native `EnvelopeRunner` with its
typed setup/state, controls, elapsed ticks and times. It computes reference
work for stage transition, attack/decay/release duration and scaling, progress
with deferred ticks, linear/exponential interpolation and PCM command encoding.
The result separates updated, delay and finished decisions. No CPU/PC or
recorded timings are model inputs, and no product timing or PCM change is made.

The observer captures real MIDI-driven H8 at33f4 and checks all mutable EG
fields at331c (normal return),3393 (completion service entry), or346b (delay
decision before its stack/unmask/outer-return tail). H8 still holds its
pre-return stage until the completion service writes it: the probe compares
that stage separately from the C++ semantic finished state instead of requiring
an artificial intermediate lifecycle write in the product.

- `/tmp/sc55-amplitude-work-variable.log`:3377 value/work comparisons,
  35 budgets,8 natural completions, exit0. Stage2/4/6/8/10/12 counts are
  135/300/1523/1/88/1330. The fixture now requires release and completion.
- `/tmp/sc55-amplitude-work-capacity.log`:7471 comparisons,19 budgets PASS;
  the original capacity survivor mismatch at admission40 remains.

Delay was not reached by either MIDI sequence and is not claimed verified.
The completion service's allocation/notification work and the delay exit tail
are outside this operation. Modulation work and the remaining event scheduling
still must be connected before enabling timed phase delivery in the product.

### Complete output-control value and work model

`cpu-roles/voice-output-work.h` evaluates the complete output owner36db..37fb:
level composition (including both modulation sources), startup ramp/TVA command,
bounded pan motion and effect-send smoothing. It calls the product's
`VoiceOutputState::advance` for values and calculates reference instruction work
from the same typed inputs. No CPU, PC, stack, device writes or measured timing
are model inputs. This is diagnostic metadata, not a product waiting policy.

`VoiceOutputProbe` captures actual MIDI-driven H8 inputs at36db and compares
all six output state words plus complete instruction count at335e (or3363 for
the stopped exit). Normal controller/glide case:2417 comparisons,16 budgets
PASS; capacity:7471 comparisons,2 budgets PASS, original admission40 survivor
mismatch unchanged (`/tmp/sc55-output-work-{variable,capacity}.log`).

Those initial cases had no pan/send motion or tone scale, so the MIDI fixture
now sends CC7/11/10/91/93=0/64/127, fresh notes, GS part pan0 and drum notes
49/51/41. The first coverage assertion correctly failed: CC10=0 is stored as1
by SC-55, so it cannot select random/frozen pan. GS part pan0 uses the distinct
receiver path. No reference RAM or clock was changed to create coverage.

Final run `/tmp/sc55-output-work-coverage.log` exits0:
3369 full state/work comparisons,41 budgets, frozen pan124, tone scaling150,
pan motion305 and send motion218. The test requires all four categories.
Filter and pitch comparisons remain enabled and pass in this same run.

This establishes the exercised output operation, not complete native/H8 audio
equivalence. Amplitude EG, modulation and event work/scheduling remain open.
No product delays, PCM changes, Projucer resave or host/install validation were
performed for this diagnostic-only addition.

### Complete periodic pitch value and work model

`cpu-roles/pitch-control-work.h` now evaluates the whole periodic pitch operation
from native `VoicePitchRunner`, modulation inputs, elapsed ticks, glide rates,
reference and correction source. Values use the existing native state owner.
The diagnostic work model covers envelope dispatch/interpolation, offset,
both modulation sources, master/part tuning, glide decay, rate conversion and
source-keyed correction. No H8 registers, PC, captured schedule, average wait
or product PCM changes are involved.

The real-MIDI observer captures input at4fdb and compares the complete mutable
pitch state and whole instruction count at3348. The controller fixture adds
CC5=100, CC65 on/off and48/84/36/72 Note On/Off, not direct firmware RAM writes.
Results:

- Variable-controller/release/glide:2417 value/work comparisons,58 budgets,
  including520 moving-glide calls, PASS (`/tmp/sc55-pitch-work-variable.log`).
- Capacity:7471 value/work comparisons,26 budgets PASS; the original survivor
  mismatch at admission40 remains (`/tmp/sc55-pitch-work-capacity.log`).
- Final coverage-gated run also exits0: positive glide299, negative glide221,
  correction-source refresh7, same2417 value/work comparisons. The fixture
  fails if either glide direction is absent (`/tmp/sc55-pitch-work-coverage.log`).

These are complete-operation comparisons on the exercised inputs, not proof
of all possible pitch branches, audible equality or CPU improvement. Product
timed phase delivery is still disabled. Amplitude, modulation, level and event
costs remain separate outstanding work; this does not justify inserting only
filter/pitch delays into the product.

### Complete filter-control value and work model

`cpu-roles/filter-control-work.h` composes the native second-envelope state
advance with the previously checked output conversion. The diagnostic model
covers4443 through return: bypass, segment transition, controller-adjusted
duration with both saturating scales, progress/overflow/deferred ticks,
signed interpolation, then smoothing/level conversion/command encoding.
Inputs are typed envelope/timing/output values and tables. It has no CPU,
PC, stack, recorded timestamps or host-speed input. Its work count is reference
interpreter metadata; it is **not installed as a product waiting policy**.

At the real entry, the observer captures the full filter state and inputs.
At3332 it compares stage, parameter, start/target, progress/deferred ticks,
internal level, step time, output, cached PCM level/command and control byte,
as well as the complete instruction count. Nested output/conversion checks
remain independent, preventing compensating prefix/output cost errors.

The initial probe caught a one-instruction undercount on a transition to
sustain, with values already equal. Inspection of the actual transition table
at7896 showed the reload at448b for these transitions; transition22 instead
jumps directly to4553. The model now follows those distinct paths rather than
adding a blanket correction to all calls.

- Capacity:7471 complete value/work comparisons,42 distinct budgets PASS;
  original survivor FAIL40 remains (`/tmp/sc55-filter-work-capacity.log`).
- Original variable-controller case:1223 comparisons,55 budgets PASS
  (`/tmp/sc55-filter-work-variable.log`).
- The controller fixture now also sends CC72=0/64/127 and real Note Off/On
  messages. Expanded run:1713 comparisons,73 budgets PASS; entry stages
  2/4/6/8/10/12/22 have4/176/112/159/923/338/1 observations. The fixture
  explicitly requires release coverage, rather than reporting a held-note
  test as complete release verification (`/tmp/sc55-filter-work-release.log`,
  exit0). Bypass and inactive-stage branches are not claimed covered by this
  expanded run. No H8 RAM/clock was patched to produce the cases.

The diagnostic target builds. Product scheduling and PCM DSP are unchanged.
Filter was prioritized because it had the largest measured calculation total;
the other calculation stages and separate event-execution costs still need
complete input-derived models before timed phase delivery can be enabled.

### Calculation work versus interruptible gaps (current timing evidence)

`ControlStageWork` observes the actual five calls3303/3319/332f/3345/335b
and their returns, separately from the interruptible gaps beginning at the
preceding ANDC. It accounts instructions only for task8 outside hardware IRQ
frames/kernel execution, but wall time includes all intervening work. Nested
or missed interval boundaries fail the probe; natural amplitude completion
through3393 and a stopped gate through3363 have explicit exits.

Capacity sequence through admission39, reference emulated cycles:

| Calculation | Calls | Total exclusive work | Per-call range | Following entry gap convention |
| --- | ---: | ---: | --- | --- |
| Second modulation | 7472 | 4752192 | 636..636 | Gap0 precedes this call |
| Amplitude EG | 7471 | 11299572 | 816..1716 | Gap1 precedes this call |
| Filter EG/output | 7471 | 18412896 | 1416..2844 | Gap2 precedes this call |
| Pitch | 7471 | 13933512 | 1812..2484 | Gap3 precedes this call |
| Level/output | 7471 | 11564040 | 1536..1560 | Gap4 precedes this call |

For **each observed calculation**, wall time equals its own work: no other
task/IRQ runs within the calculation bodies in these cases. The calculation
total is59962212 cycles. The five gaps total27744108 wall cycles, only3586272
of which are task8 work. Each gap's own work is96 cycles; the rest is
intervening execution, with a maximum gap135540 cycles. The stopped amplitude
entry contributes a gap but no amplitude call, as expected.

The variable-controller sequence also has wall==own for all five calculation
bodies (1223 each). Its ranges are480..732,180..1716,1464..2868,1560..2424,
1476..1692. All entry gaps again have96 own cycles, with wall time up to85860.
Thus the constant636 modulation cost in the first sequence is **not** a valid
general model. Neither case exercises natural amplitude-completion3393 in this
probe, so that exit is not claimed dynamically validated here.

Commands: `SC55_TRACE_CONTROL_ROUTINES=1 ... --native-capacity-stealing`
(known FAIL40 preserved) and `... --native-controller-work` (exit0).
Logs: `/tmp/sc55-stage-work-capacity.log`, `/tmp/sc55-stage-work-variable.log`.
This is not host CPU profiling or physical hardware timing. No product waits
were added. The next time model must keep input-derived calculation work
separate from eligible event execution in the gaps: charging average wall
time as calculation cost would count external work twice and hide its order.

### Observed stop source and scheduled lifecycle handoff

Read-only instruction-entry observation across **all** tasks identifies the
previous amplitude-entry stop. During slot7's hold, task1 calls53e6 at age11712
cycles (old stage6), calls it again at15804 (stage18), and enters113a at16188
to install the replacement. Task8 then reaches its amplitude gate with stage18
and skips the remaining calculation. This is a capacity/replacement operation,
not the PCM sample-end case used to first test the seam. In the same fixture,
task2 enters2928 while slots22 and17 are held; those intervals still complete
normally. Log: `/tmp/sc55-held-event-capacity.log`. Event flags1/2/4 mean
PCM-boundary/physical-stop/installation respectively. Observations do not
change the reference and must not become a recorded product schedule.

`NativeVoiceEngine::serviceControl` now accepts a semantic `ControlSlice`:
normal whole-pass execution or one phase. Both use the same scheduled runtime
and lifecycle/request handoff. A phase returns `working`, with only that
call's changed-owner mask; completion remains `updated`. The pass consumes
the clock once, retains its elapsed count, and leaves later timer expirations
pending. Stop/replacement requests are imported before each phase, so the live
stage gate sees the request without prematurely executing its preparation or
stop-consumer work. The default remains whole-pass execution; there is still
no guessed duration or normal render-path switch to timed phases.

Two engine-level real-PCM cases stop slot23 between modulation and amplitude,
one also using `RestartAndInstallVoice` to publish a new part/key and task2
request. Both retain the new stopped lifecycle and request, skip the obsolete
calculation, complete with the original elapsed1, and preserve two later
expirations. The full PCM suite and startup/block-size regression exit0:
`/tmp/sc55-scheduled-phase-pcm.log`, `/tmp/sc55-scheduled-phase-startup.log`.
Diagnostic and product-emulator translation-unit builds succeed. No Xcode
host playback or timing-fidelity improvement is claimed. Normal capacity
remains the known FAIL40; phase timing and event delivery are still required.

### Interruptible calculation stages and live stop gates

Reinspection of the executed H8 entry points shows that calculation itself is
not atomic:32f0/3306/331c/3332/3348 enable interrupts, and
32fc/3312/3328/333e/3354 check the live stop stage before respectively second
modulation, amplitude EG, filter EG, pitch and level/output control. The
actual-instruction observer confirms gate entry counts7472/7472/7471/7471/7471
in the capacity fixture. One stop is detected at the amplitude entry, before
that EG calculation; the later three gates are consequently skipped. This is
an observed control flow, not just a possible interpretation of a listing.

The initial three-phase native implementation could overwrite a sample-end
stop arriving after readback: calculation reconstructed lifecycle stages from
the still-running amplitude runner. A real-PCM regression invoked the existing
sample-boundary handler at that seam and failed before the fix (exit134,
`/tmp/sc55-control-stop-red.log`, then line224). This is a defect exposed by
the new resumable seam, **not proof of a current audible product regression**;
the product still drains groups without interleaving events there.

The calculation now has five named semantic stages and checks live lifecycle
before each. `VoiceControlRuntime` retains only the next calculation stage;
the ordinary path drains the same code without introducing a delay. A stop
returns through the existing termination handler and skips remaining paired
calculation/publication. It must not be mistaken for natural EG completion.
The ordinary group loop remains bounded (selection plus at most two voices,
each with readback, five calculations and publication:15 steps).

The revised regression delivers a sample-end stop at **each of the five**
calculation seams and checks the stop survives. Full real-PCM suite exits0,
including768 paired-group I/O comparisons; startup reuse and variable-block
audio checks exit0. Logs: `/tmp/sc55-control-stop-green.log` and
`/tmp/sc55-control-stop-startup.log`. Diagnostic/product-TU builds succeed.
`/tmp/sc55-control-stop-capacity.log` still fails at40 with the same66/67
survivors; no mismatch assertion was changed. No host playback, CPU improvement,
or complete scheduling fidelity is claimed. The remaining execution model
must deliver eligible events between these semantic stages, not impose an
atomic whole-calculation prohibition on the final product.

### Native readback/calculation/publication phases implemented

`PeriodicVoiceUpdatePass` now retains semantic phases: select/control inputs,
readback, calculation, publication. Selection and pair index belong to the
pass; there is no retained PC, stack, reference trace, heap allocation or
duration estimate. For linked voices the order is read/calculate first voice,
read/calculate second voice, publish first, publish second. Early termination
still abandons the remaining pair operations and resumes the scan.

`VoiceControlRuntime::resumeControlPhase` advances one phase; the ordinary
`resumeControlPass` drains the **same implementation** for a whole group.
`ReadVoiceControl` owns ramp hold/readback and release entry, publishing
allocator activity at that point. `CalculateVoiceControl` advances EG,
modulation, pitch and output controls without repeating readback. Existing
`UpdateVoicePcm` remains the output publication. Ordinary rendering constructs
the group context once, rather than re-scanning24 owners for every phase.

This is connected product code, but **normal execution still drains the
phases without advancing PCM between them**. There is no claim that the
capacity mismatch is fixed or that host CPU load improved. Time attribution,
PCM-boundary events and voice-command interleaving within a pending group
still need an authoritative execution model. Until then, the phase interface
requires stable voice ownership within a group; it must not be used to install
or reclaim a voice halfway through that group's calculation.

The real-PCM test alternates whole-group and phase execution over768 paired
groups, checking identical I/O and results against uninterrupted execution.
A separate test advances PCM32 frames after readback and another32 after
calculation, checking that the amplitude andff00 command stay held and that
no amplitude command is written until publication. These are **synthetic
test intervals**, not proposed SC-55 timing constants. Startup reuse/block-size
checks pass; capacity remains FAIL40 with unchanged66/67 survivors.

### PCM amplitude is held during control calculation (executed reference)

The actual-instruction observer now measures3212 (after the three PCM
readbacks) through586f (before the new amplitude command is written). It
records physical slot, PCM amplitude, command and the down-counting14-bit
PCM frame counter. It discards an unmatched interval when the same slot
enters a new update: termination can bypass normal publication. It does not
modify the CPU, chip, input stream or native scheduler.

At31ce the firmware writesff00 to the amplitude ramp command, reads back its
current level, computes the new envelopes, and only at586f publishes the next
command. The measured interval is a **lower bound** on that hold, since3212
is later than31ce. Delay-stage updates are excluded.

| Reproduction | Published intervals | Mean/min/max emulated cycles | PCM frames elapsed | Amplitude/command changes during interval |
| --- | ---: | --- | ---: | --- |
| Capacity, through admission39 | 7471 | 12589 / 7632 / 195216 | 150497 | 0 / 0 |
| Variable controllers | 1223 | 9377 / 6012 / 98016 | 18352 | 0 / 0 |

At the emulator's20MHz timebase the capacity mean is0.62945ms; this is **not
physical hardware timing**. Long intervals include intervening task/IRQ time.
One capacity interval bypassed publication, versus zero in the controller
case. All matched intervals retained physical amplitude and commandff00 while
PCM continued running. Logs: `/tmp/sc55-ramp-hold-capacity.log` and
`/tmp/sc55-ramp-hold-variable.log`.

The native `AdvanceVoiceControl` performs readback/hold and calculation, and
`resumeControlPass` immediately publishes with `UpdateVoicePcm`, without
advancing PCM between them. Thus a future semantic execution scheduler must
distinguish readback/hold from publication, not merely distribute complete
atomic voice groups across a pass. This observation does **not** establish
that the hold alone causes the66/67 survivor mismatch; capacity still fails
at40, unchanged, while the variable-controller comparison exits0. It rules
out treating calculation time as only CPU overhead with no chip-side effect.
No fixed hold duration, recorded schedule or PCM DSP change was added.

### Product configuration and simultaneous-priority recheck

After the panel-transfer work, the normal admission40 failure was reproduced
again. Inspection found that this fixture omitted ImportEffectsTables, unlike
NativeSynth. The fixture now supplies the complete product configuration; the
survivor assertion is unchanged and still fails at40 (66/67 swapped).
Log: `/tmp/sc55-capacity-product-setup.log`. This corrects the test setup; it
does not prove that missing effects caused any user-visible defect.

A temporary diagnostic-only experiment processed queued admission before a
ready control pass, retaining all startup/reset/transfer readiness gates. It
counted simultaneous eligible admission/control conflicts: zero in this exact
reproduction. The same failure remained. Log: `/tmp/sc55-capacity-priority.log`.
Thus swapping same-instant priority cannot explain this fixture. It does not
exclude scheduling differences while an H8 pass is already executing.
The experimental switch, fields and product-path branch were removed.

The next substantive work remains control execution across advancing PCM time,
using input-derived work and semantic publication stages. The existing native
instantaneous whole-pass execution and H8's long pass are not made equivalent
by the parameter/transfer changes. No guessed per-voice delay or survivor-rule
exception was added, and no failing gate was weakened.

### Input-derived second-envelope output work

`cpu-roles/envelope-conversion-work.h` models the complete4662..47fa output
calculation from typed input values and the existing native table/math functions:
base/controller offsets, two modulation sources, smoothing/ceiling, interpolation
and command encoding. It does not interpret instructions, inspect a PC, retain
CPU registers or consume a recorded schedule. Counts are diagnostic metadata
for the reference interpreter, not physical H8 timings or host CPU costs.

The observer captures real entry inputs and checks the resulting output,
cached level, controller byte, PCM command and exclusive instruction count at
the actual return. The nested473c conversion is checked separately so errors
in its cost cannot be hidden by compensating errors in the preceding work.

- Capacity sequence:7471 full-output and7471 conversion comparisons pass.
  Full output has18 distinct budgets (104..160 instructions); conversion has11.
  The original admission40 survivor failure remains unchanged (exit134).
- Variable-controller sequence:1223 full-output and conversion comparisons pass,
  alongside1223 controller-work comparisons. Full output has40 distinct budgets
  (104..166 instructions), conversion24. This invocation exits0 and requires
  at least three budgets for each calculation, rejecting a constant-cost fixture.

Commands are the existing `SC55_TRACE_CONTROL_ROUTINES=1 ... --native-capacity-stealing`
and `... --native-controller-work`; logs `/tmp/sc55-envelope-work-{capacity,variable}.log`.
Only diagnostics changed. The product must not use this partial work budget
before amplitude/pitch/modulation, admission and publication scheduling are
connected into a complete time model. No delay was inserted into NativeSynth.

### Instruction-entry and IRQ-frame accounting (current source)

After the MIDI/SysEx/GS-part receiver split, normal capacity still fails at
admission40 with the same66/67 survivors. The consolidated Release/arm64
Standalone+embedded AUv3 build succeeds; this is not a host playback test.

The diagnostic target now uses the existing instruction-entry observer AFTER
interrupt dispatch and read-only IRQ entry/return hooks. Hardware frames are
tracked by CPU/stack/task, not one global nesting depth: the kernel can suspend
an interrupted task and restore another stack. RTE alone is insufficient;
the timer path at7f8b restores SR explicitly and returns via PRTS at7f90.
Both returns remove their matching frame. Duplicate live-frame reuse aborts
the probe rather than silently accepting stale attribution.

Hooks are compiled only into cpu-roles, and tracking is enabled only for
`SC55_TRACE_CONTROL_ROUTINES` or `--native-controller-work`. No CPU cycles,
reference registers, native waiting policy, PCM or allocation rule changed.
The former pre-Step/page attribution is retained and labelled sampled.

`/tmp/sc55-exclusive-capacity.log` (7472 main calls, original FAIL40 retained):

| Routine | Sampled task cycles | Exclusive calculation cycles | Hardware-frame-associated cycles | Wall cycles |
| --- | ---: | ---: | ---: | ---: |
| 3188 voice control | 70,099,704 | 67,684,824 | 6,631,380 | 95,250,120 |
| 3985 modulation | 7,331,100 | 7,331,100 | 0 | 7,331,100 |
| 5c20 controllers | 18,739,776 | 18,739,776 | 0 | 18,739,776 |
| 5855 publication (7471 calls) | 2,383,452 | 2,330,952 | 75,648 | 2,463,288 |

Exclusive excludes kernel PCs below0752 and instructions inside a tracked
task8 hardware frame. The IRQ column includes such a frame's kernel work;
wall time also includes other tasks. These columns are not sampled-minus-IRQ.
The controller calculation checks both sampled and exclusive counts against
the independently input-derived budget (7472 calls; variable-input fixture1223).
The exclusive3188 page ranking still starts4700,3100,3600,5300,3700,5100;
the timer's7f page is no longer among the leading pages. IRQ contamination therefore is not
the main explanation for the long control pass. Raw stage6/ticks3 at24 voices
still ranges1464..9864 exclusive cycles, so stage/tick/voice count alone is
not a sufficient constant-cost model. This is virtual emulator timing, not
physical H8 timing or host CPU profiling.

The next control-time model must derive work from actual control branches and
publication/reuse ordering. Neither these totals nor their means are suitable
as a fitted delay. The default survivor assertion remains a failing gate.

### Pending preparation: published identity versus DSP state

Reinspection rejects the hypothesis that periodic controllers should retain
an older part/key until DSP preparation completes. The installation producer
113a writes part at116b (`c8e4[slot]`), original key at1173 (`c8fc[slot]`), then
queues preparation at11c2 (`caf4[slot]=2`). Periodic controller preparation
5c22/5c38 reads those same rows, not a copy made by5639. Existing native
`RefreshSelectedVoiceControllers` correctly reads the installed identity.
No second old-identity table was introduced.

The engine now allows a pending preparation request with an existing DSP owner
to participate according to its published lifecycle. RestartAndInstallVoice
has already stopped a restarted voice (stage18/20), so the primary scan skips
it; a non-restarted continuation keeps its EG/LFO and uses the new installed
controller identity. The request remains2 for the preparation consumer. This
does not install a new DSP owner or key it on from periodic control.

The regression calls RestartAndInstallVoice for both cases, with a distinct
part/controller contribution. Before the change the pass incorrectly deferred.
Afterward the continuation updates with the new part/key, the restarted slot
is excluded, other voices progress, and both requests remain pending. Full
voice/PCM tests, release integration111 cases, startup-wake/block partition
tests and13 all-part55KTIZKE kick attacks pass. Logs use
`/tmp/sc55-pending-prepare-*.log`.

A request on a slot without any previous DSP owner still retains conservative
deferral (including linked access), rather than constructing a fake old owner.
Invalid lifecycle/request values also retain deferral. H8 dispatch/computation
timing and the capacity-survivor discrepancy remain separate open work.

### Queued physical stops no longer freeze the whole pass

`NativeVoiceEngine::serviceControl` previously deferred whenever any lifecycle
entry had a pending request. H8's stop producer53e6 instead immediately writes
stage18/20 at5426..542c. The periodic primary scan excludes those stages; the
finish-stop consumer later changes them to14/16. The native stop producer had
the correct stopped state, but only in the admission/stop lifecycle array,
while periodic ownership still held the prior playing state.

The engine now validates pending physical-stop state and publishes it to the
periodic owner before selection. It does not consume the pending request,
advance to14/16, clear activity or return the allocation early. Unrelated
voices can run. For linked voices, selecting a stopped partner uses the
existing3363..33e7 early exit; it is not an indiscriminate bit-mask exclusion.
Effects scheduling uses the same readiness decision. Requests for preparation
still defer the pass until their ownership handoff is understood; invalid or
incomplete stop state retains the previous conservative deferral.

The new real-PCM regression failed before the change at its assertion that
unrelated voices progressed. Afterward both single and linked cases pass,
including retention of the finish-stop request and its later18/20→14/16
transition. The all24-stopped case consumes the clock with zero PCM I/O and
zero updated owners, while preserving all24 pending stop requests.
`--native-startup-wake` still reports8 timer-boundary reuses,88 unrelated EG
updates and block-invariant audio. Release integration111 cases and all13
55KTIZKE kick attacks pass. Normal capacity stealing still differs at
admission40. Logs: `/tmp/sc55-queued-stop-*.log`.

### Product control can progress during a reuse wait

The scheduled product path now uses the resumable periodic pass, not the
all-startup-blocking `advance` entry. A failed reuse poll still sleeps until
the common one-tick event, but that sleep no longer prevents a pending voice
control event from updating unrelated owners. Destinations reserved for
preparation are excluded by the existing stage/slot selection. A linked group
that reaches a reserved partner defers while retaining the pass cursor and
captured elapsed count. Key-latch/two-PCM-pass protection still defers control.
Everything remains serialized on the same audio owner; no thread was added.

`ScheduledResult::updatedMask` describes changes in this call, including a
partially completed pass. NativeVoiceEngine publishes those changes even when
the pass deferred, and does not republish earlier owners on resume. The clock
is consumed only at pass start. Expirations accumulated during a pause belong
to the next pass. Explicitly stepped diagnostic passes retain their own
continuation; the automatic scheduled entry cannot steal them.

Evidence:

- Extended `--native-startup-wake` failed before this connection with
  `One preparing voice froze every other envelope`. Afterward it records
  88 unrelated envelope updates during eight busy reuses, preserves reuse-tick
  boundaries and verifies full-capacity audio across zero/1/127/257-frame blocks.
- A scheduled linked-partner regression checks partial publication, resume
  without replay of slot23, captured elapsed3 and separately pending elapsed2.
  Existing explicit-pass/uninterrupted PCM I/O comparisons still pass.
- `--native-release-integration`111 comparisons and `--native-reserve-stealing`
  48 admissions/protected mono rejection pass.
- `SC55_KICK_ALL=1 --song-first-kick 55KTIZKE.MID` replays all parts for60 seconds:
  all13 kick attacks match H8 first-millisecond gain. This is not whole-song
  sample equality (key-on edges remain2100 H8 /2114 native in this fixture).
- `--native-synth` preserves checksum `3b54320560580fd3` and block invariance.

Logs use `/tmp/sc55-control-during-reuse-*.log`. Normal capacity stealing still
fails at admission40. Queued physical stop tasks still defer the complete
scheduled pass at the NativeVoiceEngine entry, and H8 execution/dispatch costs
are not modeled. This change removes the global reuse-wait freeze, not those
remaining ordering differences. The synchronous diagnostic `advance` entry
still rejects startup; product scheduling no longer uses it for this case.

### Connected reuse-wait scheduling

NativeMelodicPlayer previously called `pollStart` every sample while the old
PCM gain was nonzero. H8's 5710..573c polls, then waits on event80 if neither
gain is zero. Task2 has a one-tick period; this is a shared timer wake, not a
busy poll or a delay measured from each note. Observed key57 preparation polls
at relative frames86.938,88.032,109.018,109.574,141.178,173.914,174.470,206.842;
the final poll returns at207.072. Closely repeated polls can consume an already
pending periodic event. They are not evidence of sample-rate polling.

The native owner now remembers a failed reuse poll and resumes on the next
shared `ControlTaskClock::kernelTickCycles` boundary. While waiting, it exposes
that deadline to render batching; incoming MIDI does not bypass the wait. PCM,
display time and the control clock continue. No H8 PC/task execution, captured
timestamp or arbitrary per-note delay was introduced. This connects the timer
wait, not the H8 task-dispatch/computation latency. The key-latch wait and the
two-PCM-pass startup protection remain unchanged.

`--native-startup-wake` failed before the change with
`Reuse completed between periodic wake events`. It passes after the change:
8 consecutive busy reuses resume on shared ticks; 32 notes through NativeSynth
produce identical audio with zero/1/127/257-frame partitioning, including full
capacity stealing. `--native-release-integration` (111 comparisons),
`--native-reserve-stealing` (48 admissions plus protected mono rejection) and
`--native-synth` also pass. Normal `--native-capacity-stealing` still fails at
admission40. This correction does not claim to solve that whole difference.
Logs: `/tmp/sc55-startup-wake-{before,after,release,reserve,synth,capacity}.log`.

An independent diagnostic, `SC55_REPLAY_MIDI_INGRESS=1`, observes H8 receive-ring
commits and supplies those bytes to native at the observed frames. It validates
that all bytes match the submitted packet, and never changes H8 state. With
`groups-start` replay the mismatch moves earlier to key50/frame185, where native
is still in its protected startup wait. Thus matching UART ingress alone does
not align the entire admission/control pipeline. This experiment remains in
the diagnostic executable only; product MIDI timing was not changed to match
a recorded trace. Log: `/tmp/sc55-capacity-ingress-current.log` (before reuse-wake fix).

Product Release/arm64 Standalone and embedded AUv3 build succeeded with this
reuse-wake change (`/tmp/sc55-native-product-validation-current.log`). Signing,
registration and host playback were not tested. The remaining product-wide
serialization is explicit in `NativeVoiceEngine::serviceControl`: any queued
physical stop defers the entire pass, and the synchronous runtime also defers
during startup. The diagnostic resumable path can skip preparing slots and
visit other groups. Connecting that ownership-aware progression, together with
its correct event ordering, remains necessary; the timer-wake correction alone
does not remove the whole-pass serialization.

### Current-source recheck: group-start replay still differs

The normal capacity test still fails at admission40 after NoteGroup and
VoiceAllocation ownership changes. Diagnostic `SC55_REPLAY_CAPACITY_PASSES=groups-start`
with `SC55_TRACE_REUSE_SCAN=1` also fails at the key57 admission, before that
normal survivor mismatch. Logs: `/tmp/sc55-capacity-current.log` and
`/tmp/sc55-capacity-group-start-current.log`.

For physical slot8, relative to the key57 input window (units: 32kHz frames):

| Event | H8 | Native |
|---|---:|---:|
| Destination enters preparation / reuse wait | 29.626 (stage18) | 1 (reserved) |
| Old PCM gain reaches reuse condition | 178.944 | 144 (key-latch wait begins) |
| New owner installed | 208.243 (stage2) | 146 (stage2) |
| H8 scan tests slot8 | 195.667, still stage18 | Already installed |

Native's frame144 is a startup-state transition, not an instrumented observation
of the exact gain-zero sample; do not equate those timestamps more precisely.
At the group result boundary (frame238), H8 updated slot7 (`000080`) while
native updated slot8 (`000100`). Moving the diagnostic update from group end
to group start therefore does not solve the ownership difference. The preparation
pipeline is already ahead before this scan. This rules out changing only the
group publication boundary as a complete fix for this case; it does not by itself
prove how much of the lead is MIDI dispatch, PCM decay or preparation-task service.

Keep the product free of these recorded timestamps. The next timing work must
cover admission/reuse and periodic control together, preserving the existing
gain-readiness and two-pass attack protection. Changing the allocator tie-break
or EG formula cannot repair this earlier ownership/order mismatch.

After the panel queue and GS rhythm-mode fixes, the normal
`--native-capacity-stealing` still fails at admission40, retaining native key67
instead of H8 key66. Log: `/tmp/sc55-capacity-current.log`. The newer functional
fixes do not resolve the scheduling difference.

`SC55_TRACE_CONTROL_ROUTINES=1` now also summarizes the16 most expensive256-byte
PC pages sampled inside3188. This is read-only diagnostic work, not a scheduling
table for the product. `/tmp/sc55-control-work-pages.log` shows70,099,704 cycles
under the existing task8/non-kernel-PC definition, including:

| Page | Attributed cycles |
| --- | ---: |
| 00:4700 | 8,069,748 |
| 00:3100 | 7,378,692 |
| 00:3600 | 6,592,752 |
| 00:5300 | 4,439,532 |
| 00:3700 | 4,303,296 |
| 00:5100 | 4,213,644 |
| 00:7f00 | 1,959,000 |

The first pages span envelope/output/pitch calculations already represented by
`AdvanceVoiceControl` and its callees. The7f page, however, contains device timer
interrupt code:7f20..7f84 includes the display countdown loops7f53 and7f64 and
returns withRTE. Therefore the old `own` aggregate is **not exclusive calculation
time**, even though its task ID is8 and its PC is outside the low kernel region.
This confirms the limitation already noted above; do not turn that aggregate
into a per-voice delay constant. Sampling occurs beforeStep and may additionally
attribute an interrupt's first instruction to its interrupted PC.

This result narrows the next scheduling work: separate calculation completion
from interrupt/preemption time before deriving any workload-based model. It does
not establish a correct native delay model or fix the original survivor test.

## Input-derived controller work: verified reusable calculation

The existing `ScaleVoiceController` already returns the exact interpreted
instruction count for its arithmetic branches. `TryPrepareControllers` uses
those counts for the legacy H8 accelerator; its CPU-register/stack adapter is
not suitable for use by NativeSynth. The pure count calculation is reusable
without that adapter or a recorded playback schedule.

`ControlWorkProbe` now independently predicts a complete5c20 call from the live
part/key sensitivities and five contribution rows, using the existing scalar
counts. It adds17 setup/call/return instructions and the10 part-pointer reloads,
then multiplies by the existing emulator's12-cycle instruction convention.
The routine masks interrupts, unlike the larger3188 measurement, so actual
call-to-return attributed time can be checked directly in this fixture.

- Original capacity sequence:7,472 predicted/observed calls match. The original
  admission40 survivor assertion still fails; this is not a capacity fix.
- New `--native-controller-work`:1,223 calls match while actual MIDI changes all
  six GS sensitivity rows, poly/channel pressure, pitch bend, modulation and the
  two assignable CCs. Nine distinct budgets occur:2508,2580,2640,2688,2748,2760,
  2820,2868,2928 device cycles. The test rejects a run with fewer than three
  distinct budgets, so a constant timing assumption cannot satisfy it.

Logs: `/tmp/sc55-controller-budget.log` and
`/tmp/sc55-controller-budget-variable.log`. This verifies this one calculation,
not the envelope, preparation, IRQ/preemption or complete-pass scheduling model.
No product timing, PCM code or H8 reference behavior was changed. Do not promote
this partial budget alone to the product scheduler.

## Confirmed omissions and implementation

ROM1 08da..08f0 derives A040[part] from selected patch record+0d bit0:
0 selects FIFO, 2 selects the three-pass candidate search. The native capacity
policy previously left all modes at zero. Program selection now derives the
same value from patch.common[1]. A new real-MIDI/H8 comparison first failed at
program0 (native0/H8 2); after the change all128 capital programs match, with
55 selecting mode2. Bank/program resolution remains the existing implementation.

The three-pass search1961/19af/19f5 compares AC42[voice], produced by envelope
synchronization3196..3212. Native synchronization already calculated that value
as voice.release.activity, but normal control updates did not publish it to
allocator.activity. The connection is now made before termination processing;
termination retains its existing final authority over stopped-voice activity.

## Verification and unresolved fidelity

`--native-reserve-stealing` (program80) still passes48 admissions across three
reserved parts, including survivors and rejection at fully protected capacity.
`--native-capacity-modes` passes128 program selections against booted H8.

`--native-capacity-stealing` exercises the same admission sequence with piano,
and intentionally remains a failing regression: admission40, part2, H8 retains
key66 and steals67; native retains67 and steals66. Before that admission the
physical voice mappings agree. H8 activity for keys66/67 is4/3; native is3/3.
Other observed differences are key69=5/4 and key68=5/4; keys70/71 match5/6.
This proves a remaining envelope/control-state difference that changes ranking,
not a reason to special-case these notes or change the tie rule. Timing versus
envelope arithmetic has not yet been isolated. Do not call capacity fidelity or
the full H8 replacement complete on the basis of the128 program-settings test.

Logs: /tmp/sc55-h8-capacity-before.log, /tmp/sc55-h8-capacity-after.log,
/tmp/sc55-h8-capacity-stealing.log, /tmp/sc55-h8-capacity-inputs.log.

## Follow-up: PCM clock phase is not sufficient to explain the difference

The diagnostic-only SC55_CAPACITY_ALIGN_PCM probe lets the native player finish
idle setup, then advances idle audio until the H8/native PCM envelope phases
agree. At admission40 the clocks differ by only one frame (22e3/22e4), but the
same activity/survivor mismatch remains, with identical native levels to the
unaligned run. This rules out the initial PCM ramp-clock phase as the sole cause;
it does not align note onset or the H8 control-task dispatch epoch.

The first gain command and level agree for slots4/5 (4fb4,2780). Second gain
and cutoff commands differ: slot4 H8=0405,1101 versus native=ff00,ff00;
slot5 H8=0304,1002 versus native=ff00,ff00. Raw second-gain levels are
0200/01e1 and0180/01c2 respectively. The discrepancy therefore exists in the
envelope/control path before candidate selection, not only in the published
activity byte. Next comparison must trace segment progress and command
generation against H8 at matching voice ages/control-pass points. Do not alter
the tie rule or insert a note-specific delay to pass this fixture.

Logs: /tmp/sc55-capacity-eg-input.log and /tmp/sc55-capacity-eg-aligned.log.
The DEBUG-capacity-eg probes are temporary diagnostics for this unresolved case.

## Segment-state probe

H8 bases were observed at actual318c entry (physical channel at base-2), not
guessed from a stride. At the same admission40 checkpoint:

| Voice/key | Raw stage H8/native | Progress H8/native | Deferred ticks | Logical level H8/native |
| --- | --- | --- | --- | --- |
| 4/67 | 6/6 | 17427/17501 | 0/0 | 03c8/03bc |
| 5/66 | 6/6 | 17820/17892 | 0/0 | 038a/037f |

The progress gaps are74 and72, consistent with two increments of37 and36.
Both sides are in decay1; this points to elapsed-update/onset scheduling rather
than a different stage selection. It does not yet prove where the two-update
lead originated. Next probe should capture H8 control-pass timestamps and
native segment transitions from first onset; arbitrary post-hoc time shifts
are not a product fix. Log: /tmp/sc55-capacity-eg-state.log.

## Actual H8 elapsed-event batching, not a fixed two-tick offset

SC55_TRACE_CAPACITY_EG records the key66 voice at H8's5855 control publication
entry, with AC5A elapsed count, and records native segment progression after
single-frame stepping. This retains the original high-polyphony inputs. The
initial observed H8 elapsed counts are2,2,2,3,3,1,3,2,3,1,3,2. Native progression
increments by1278 per nominal expiration in attack2; H8 increments by multiples
of1278 according to those actual elapsed counts.

Matched progress values have matched logical levels in the trace:2556/4bc5,
5112/4b89,7668/4b4d,11502/4af3. PCM commands need not match at these points:
for example progress5112 produces H8 4c03 versus native ff00 because the previous
PCM ramp/current-level history differs when updates are batched. H8's observed
publication intervals include345912,431988 and398184 cycles, not one fixed
160256-cycle interval. This is direct evidence of control-dispatch/event-batching
differences. It supersedes treating the final two-increment gap as a constant
startup offset; no such delay has been added.

The native event clock already supports aggregated expirations when genuinely
deferred, but native execution does not consume emulated H8 instruction time.
Reproducing firmware's workload-dependent dispatch cadence is an unresolved
control scheduling fidelity issue, distinct from translating the envelope
formula. Neither a fixed slower timer nor special-casing the failing notes is
justified. Logs: /tmp/sc55-capacity-eg-time.log and
/tmp/sc55-capacity-eg-ticks.log. Full capacity test remains red.

## Workload origin of the batching (2026-09-10)

The current worktree reproduces the original admission40 mismatch after the
receive-watchdog and bulk48 additions. `SC55_TRACE_CAPACITY_WORK=1` profiles
the SAME test without changing firmware scheduling or native behavior.
Each MIDI packet retains its original4,000,000-cycle observation window.
Complete control passes are measured from5af9 to5b70, with the voice section
starting at5b0b. Partial passes crossing a window boundary are excluded from
the per-pass means; task totals cover the whole window.

| Allocated voices | Complete passes/window | Mean elapsed cycles/pass | Mean task8 non-kernel cycles/pass |
| --- | --- | --- | --- |
| 0 (last program setup) | 25 | 3,428 | 3,343 |
| 1 | 25 | 19,033 | 17,068 |
| 8 | 24 | 154,693 | 114,081 |
| 9 | 21 | 178,589 | 127,996 |
| 17 | 11 | 322,432 | 238,405 |
| 24 (first full window) | 7 | 455,989 | 332,773 |
| 24 (failing admission) | 8 | 460,782 | 326,568 |

The programmed period is160,256 cycles. In this piano sequence, the firmware
falls behind around9 allocated voices. At24 voices even the task8-attributed
work exceeds TWO nominal periods, before counting the other tasks' time.
The attributed column excludes sleep and CP00 PCs below0752; it is a precisely
defined execution attribution, not a claim that all possible nested interrupt
instructions have been identified. This rules out UI/MIDI preemption as the
sole cause of the batching. Almost the entire measured pass is inside the
voice section; FX contributes48 cycles in most of these windows.

Crucially these are EXISTING EMULATOR cycles, not physical SC-55 measurements:
`mcu.cpp` currently adds12 cycles per interpreted instruction with an explicit
FIXME. Do not describe the above as proof that actual hardware misses its
deadline. Do not alter that reference timing to turn the failing test green.

This is a control-scheduling fidelity gap, not evidence for changing the EG
level formula or the allocator tie rule. A native scheduling model must retain
the effects of accumulated elapsed updates and segment transitions; fitting a
constant delay/slower period to this one sequence is not an implementation.
Product scheduling was NOT changed in this investigation. The original failing
assertion remains in place, and the diagnostic-only probes remain tagged until
the discrepancy is resolved.

Reproduction:
`SC55_TRACE_CAPACITY_WORK=1 sc55-cpu-roles ROM_DIRECTORY --native-capacity-stealing`

Latest capture: `/tmp/sc55-capacity-work-profile.log` (exit134 is the deliberately
unresolved survivor assertion, not a successful fidelity test).

## Causality probe: replay complete-pass boundaries (2026-09-10)

The default test was rerun after the bulk transport changes: it still fails
at admission40 with the same key66/key67 survivor discrepancy. This turn changes
diagnostics only, not product scheduling or the envelope/allocator formulas.

`SC55_REPLAY_CAPACITY_PASSES=1` captures H8 00:5af9 timestamps and AC5A elapsed
counts from the unchanged real-MIDI fixture. It disables the diagnostic native
player's automatic control-clock expirations and signals the captured passes.
Times are relative to each existing4M-cycle observation window, rounded up to
the next625-cycle native sample boundary. No fixed shift or fitted period is
used. The H8 boot epoch, note-on dispatch and individual voice visits are not
aligned by this experiment. A counter reports replay signals that arrive while
the previous signal is still pending; do not silently treat these as distinct
native passes. Start-boundary replay encountered this condition during program
setup and the92/3b note window (one each),
so its failure is not by itself a clean proof against every start-based model.

At pass START the original admission40 survivor assertion still fails.
Key67 progress becomes17575 (H8 17427), key66 becomes17964 (H8 17820).
This intervention alone does not remove the failure; the setup coalescing
above is an additional limitation of that variant.

`SC55_REPLAY_CAPACITY_PASSES=end` instead captures 00:5b70, after the voice loop,
with the same pass's AC5A. This variant PASSES all48 admissions/survivors and the
subsequent fully-protected mono rejection/recovery cases without changing those
assertions. All end-replay windows report zero coalesced signals. At the original
pre-admission40 checkpoint:

| Key | H8/native progress | H8/native logical level | H8/native activity |
| --- | --- | --- | --- |
| 67 | 17427/17427 | 03c8/03c8 | 3/3 |
| 66 | 17820/17820 | 038a/038a | 4/4 |

This is strong evidence that control-pass completion timing, rather than a
different EG formula or tie rule, is sufficient to explain THIS allocation
failure. It is not sample-exact synthesis parity: key67's second PCM command
still differs0405/0404 and its cutoff level0825/081f. Key66's inspected PCM
commands and levels match. Per-voice publication times are spread through H8's
pass, not all exactly at the pass end.

The next product work needs a semantic control scheduling/publication model
that accounts for those completion boundaries without running H8. Replaying
H8 in the plugin, using this fixture's recorded timings as a constant schedule,
or declaring the default survivor test fixed would all be incorrect. The
default test remains red and remains the acceptance gate. Further fixtures
are required before generalizing this one successful causal intervention.

Audit hooks are enclosed inSC55_NATIVE_IO_AUDIT and only explicitly enabled by
the diagnostic option. Normal `--native-synth` still passes with checksum
`3b54320560580fd3`; no product environment variable is introduced.
Logs: `/tmp/sc55-capacity-pass-replay.log` (start, expected exit134),
`/tmp/sc55-capacity-pass-end-replay.log` (end, exit0).

## Per-routine control-work probe

`SC55_TRACE_CONTROL_ROUTINES=1 ... --native-capacity-stealing` now measures
actual task8 call/return boundaries without changing CPU state or timing.
It handles interrupts before call execution and the non-returning3188 path
that discards the return address and resumes the scan at5b5c.
The test still reaches the original admission40 survivor failure (exit134).

Across7472 calls,3188 accounts for70,099,704 attributed cycles, versus
18,739,776 for5c20 and7,331,100 for3985;5855 accounts for2,383,452 over7471
calls. Thus3188 comprises about71% of the measured routine work in this
fixture, and is the next boundary to inspect for semantic completion timing.
At24 allocated voices,5c20 consistently costs2508 attributed cycles, while
3188 varies even within a raw-state/elapsed-tick group. A fixed cost based
only on allocated voice count is therefore not established by this probe.

These are emulator virtual cycles, not measured physical H8 cycle costs or
host CPU performance. Attribution excludes sleeping and kernel PCs below0752
but is not complete interrupt call-stack accounting; do not treat its extrema
as intrinsic routine costs. State values are raw words, not independently
verified semantic EG-stage labels. No timing constants were added to product
code. Log: `/tmp/sc55-control-routine-work.log`.

## Native control pass can now resume by voice group

`VoiceControlRuntime` now owns an in-flight pass: captured elapsed count,
voice traversal/stages and cumulative updated mask. `beginControlPass` captures
once; `resumeControlPass` updates a single or linked pair through controller,
LFO, envelope, pitch, level and final PCM publication. The existing `advance`
and scheduled product path use this same implementation synchronously.
Re-entering begin cannot replace the elapsed count. After completion, resume
is idle with no device I/O. A late failure remains latched, not replayed.

MIDI consumption, preparation, releases and stop dispatch defer/reject while
a pass is in flight; a new control-clock event remains unconsumed. Dependencies
and voice owners must remain stable across resumes. This preserves the old
serialized product contract; it does not yet implement H8 preemption within a
voice group or MIDI priority between groups. No product timing is changed and
no guessed per-group delays are installed.

Focused native PCM test:64 passes /768 linked groups /1536 voice updates,
with the exact read/write byte sequence compared against uninterrupted passes.
It checks elapsed-count retention, MIDI held then dispatched once, new clock
events retained, and idle resume without PCM I/O. Existing preparation/reuse,
release and rhythm coverage in that test also passes. Normal `--native-synth`
retains checksum `3b54320560580fd3` and zero/1/127/129/257-frame partition
equivalence. Neither test is proof of H8 audio parity. No Xcode or Logic test
was run for this change.

The next diagnostic can now compare per-group PCM readback/publication times
against H8, instead of moving a complete24-voice pass to one recorded end time.
Completion scheduling and the default capacity-survivor failure remain open.
Logs: `/tmp/sc55-resumable-control-test.log`,
`/tmp/sc55-resumable-native-synth.log`.

## Group replay exposed an incorrect serialization assumption

`SC55_REPLAY_CAPACITY_PASSES=groups ... --native-capacity-stealing` observes
task8's actual voice-pass begin5b0b, group completion5b5c and pass end5b70.
The visited-mask check reads the real per-voice visited bytes through676a's
pointer table; it does not infer visitation from note count. Observation is
read-only, persists across capture windows and replays ordered events rounded
up to the next625-cycle PCM frame. Native executes its actual resumable pass.
Effects advance at the observed voice-pass begin, not their own exact H8
write times. This is a diagnostic intervention, not an exact timing oracle.

With the preceding all-MIDI-until-pass-end guard, the first mismatch was
MIDI90/34 (fifth note), frame215: H8 visited maskf80000, nativef00000 and
native reported complete. Native had held the new MIDI event until pass end,
while H8 had admitted its voice during that pass. Thus the earlier requirement
to keep all voice owners/stages frozen across resumes was too restrictive.

Runtime now retains traversal and elapsed ticks, but reloads live stages when
resuming each group. MIDI reception, preparation, releases, stop dispatch and
PCM boundaries may run between groups on the same serialized owner thread.
Newly installed inputs receive the captured pass count when selected. An
actual pending startup still defers a resume; no group is silently skipped or
given an invented delay. A new clock event cannot overwrite the in-flight pass.
This supersedes the MIDI-deferral contract in the previous section.

The same replay then passed traversal through the first24 admissions, reaching
MIDI92/30 (admission25), frame105. H8 visitedfffff8; native remainedfffff0 and
reported deferred because startup was still pending. The next issue is the
interaction of full-capacity reuse/start readiness and the control pass. It
is not fixed by suppressing startup safety or by ignoring this assertion.
The user-confirmed drum startup protection has not been removed.

The product still runs its passes synchronously. This change makes the
resumable native control model correct for observed between-group admission;
it does NOT yet change product cadence or fix the default admission40 ranking.
Focused regression publishes a new voice owner via a MIDI sink after the first
group and checks that it is visited in the same pass with the captured elapsed
count. The real-MIDI/H8 replay is the separate integration evidence above.

Logs: `/tmp/sc55-capacity-group-replay.log` (before, fifth-note failure),
`/tmp/sc55-capacity-group-midi-replay.log` (after, admission25 startup deferral),
`/tmp/sc55-interleave-control-test.log`, `/tmp/sc55-interleave-native-synth.log`.

## Reuse wait is a yield, not a global control lock

At admission25's failure the native startup state was `waitingForReuse`, not
`waitingForKeyLatch`: reserved slot10, PCM gains000d/09a1, commands00b6/1302,
keysffffff/ffffff, no queued MIDI. The group H8 had just updated was slot3,
unrelated to the reserved destination. This matches5710..573e's explicit
TRAPA0 yield while either old gain has not yet reached zero.

The resumable runtime now permits unrelated groups during reuse wait, while
retaining the cursor and deferring if a selected single/pair includes a
reserved destination. Key-latch wait still blocks control globally. The
two-PCM-pass startup protection in PCM_CompleteVoiceEnable is unchanged.
The synchronous product wrapper retains its original startup deferral; this
is not yet a new product timing model.

`ControlStep.changedMask` identifies only owners updated by that resume. The
group-replay adapter previously republished every owner in the cumulative
pass mask after each resume, which could overwrite a newly prepared lifecycle
with an old owner from an earlier group. It now publishes only changed owners.
The cumulative mask remains available as history, not publication authority.

The comparison now checks each group's actual318c entries, including early
exits, against changedMask. Cumulative visited RAM alone is not a safe proxy
for executed groups when MIDI can reinitialize voice state during a pass.
Both the earlier reuse deferral and the following mismatch remain detectable
with actual-entry comparison; no failing group is skipped or retried late.

Replay now reaches admission34, MIDI92/39, group end frame238: H8 updates
slot7 (key51), native updates slot8 (key57). Both allocate the same notes to
those slots. Slot7 stage/activity match6/33; slot8 is H8 stage2/activity255
(newly prepared) versus native stage4/activity56 (already updated). Thus
this observation is not evidence of a different sample assignment or an
incorrect EG formula; the new voice is admitted to this pass differently.

`SC55_REPLAY_CAPACITY_PASSES=groups-start` runs each native group at H8's
observed5b73/5bb4 entry instead of5b5c completion, and checks actual H8 entries
at group end. It fails at the same admission/slots. Moving a whole group from
end to start is therefore insufficient; selection/startup publication ordering
around reuse remains unresolved. No recorded schedule is used in the product.

Focused regression verifies unrelated group progress during reuse wait,
repeatable no-I/O deferral for the reserved destination, global key-latch
protection, and changedMask excluding previously processed owners. Native PCM
integration passes and normal synth checksum remains3b54320560580fd3 with
zero/1/127/129/257-frame partition equivalence. No Xcode/Logic test was run.
Logs: `/tmp/sc55-group-startup-state.log`, `/tmp/sc55-group-selection-state.log`,
`/tmp/sc55-group-start-replay.log`, `/tmp/sc55-reuse-control-test.log`,
`/tmp/sc55-reuse-native-synth.log`.
# Current scheduler gate after admission/mono ownership consolidation

The current product code was exercised again, rather than assuming the earlier
causality results survived the ownership changes:

- Normal execution still fails at admission40: H8 retains key66, native key67.
  `/tmp/sc55-current-capacity-eg.log` (exit134).
- Replaying actual UART ingress alone still produces that same failure.
  `/tmp/sc55-current-capacity-ingress.log` (exit134).
- Replaying actual complete-pass END times and elapsed counts passes the
  unchanged48-admission/survivor assertions and protected mono cases.
  `/tmp/sc55-current-capacity-end.log` (exit0).
- Replaying whole groups at their end still fails before that checkpoint:
  MIDI92/39, frame238, expected updated mask000080, native000100. Slot8 is
  stage2/activity255 in H8 but stage4/activity56 in native; slot7 remains
  stage6/activity33 on both sides. `/tmp/sc55-current-capacity-groups.log`
  (exit134). Moving a complete group's computation is not a sufficient model.

These results do not fix or newly discover the timing gap. They establish
that the current ownership consolidation preserves the previous causal
isolation. The next implementation must connect selection/readback/calculation/
publication continuations to input-dependent control execution and competing
admission events. Do not spend further turns changing allocation tie rules,
the EG formula, MIDI ingress latency, or a fixed/slower control period to make
this fixture pass. Diagnostic replay remains prohibited in the product.
# First-LFO initialization copy is not persistent sharing

`FirstModulationRoutingProbe` now also has a separate task2 observer for
3d1a..return. It exercises `InitializeSharedFirstModulation` against the actual
source/destination state and compares all block fields plus the source links.
The task2 observer is separate from the task8 observer so a preemption cannot
overwrite an in-progress periodic comparison.

`--native-controller-work` passes with six initialization copies, all having
zero persistent-sharing flag. The periodic observer still sees5238 local and
573 paired updates, with zero shared/detached updates. This verifies the
initialization-copy path without pretending to cover persistent sharing.
ROM38a2 clears the destination flag; 3d68..3d6b copies the source flag. A mode
that selects an initialization source does not itself set that flag nonzero.

This narrows the coverage question: do not keep adding overlapping notes on
the assumption that initial sharing must enter398a. Establish a nonzero flag
producer first. These observations are not a whole-ROM unreachability proof
and do not authorize deleting the existing persistent-sharing implementation.
Product sound generation and scheduling are unchanged. Build and comparison
logs: `/tmp/sc55-lfo-initialization-build.log`, `/tmp/sc55-lfo-initialization.log`.
# Reuse readiness: earlier stop, not a missing post-decay wait

Current hold/selection plus UART-ingress replay was traced at MIDI92/39, slot8.
The diagnostic now records actual native PCM gain readiness as well as stages;
it does not infer readiness from a controller stage number.

| Event (frames since MIDI window start) | H8 | Native |
| --- | ---: | ---: |
| Task1 note admission entry | 21.312 | not separately instrumented |
| Stop transition observed | 29.626 | 15 |
| PCM gain reaches zero | 178.944 | 160 |
| Successful reuse poll | 207.072 | 187 |
| New sounding stage | 208.243 | 189 |

Initial gains are identical2780/0eda. Native gain is already2508 at frame15,
whereas H8 gain is still2780 at its stage18 transition. The native post-decay
wait is27 frames to successful poll, H8 about28.1. Both wait for a common-tick
opportunity; the observation does NOT support adding an extra reuse delay or
changing `PollVoiceReuse`'s zero-gain predicate.

This corrects the overly broad earlier description of a reuse-completion
timing bug. The divergence starts upstream: native allocation/stop/preparation
advance as immediate operations after ingress, while H8 task0/task1/task2
consume execution time. The remaining scheduler must model those semantic
operations together with periodic control; accounting for task8 alone cannot
fix this interleaving. The numeric offsets above are evidence for this input,
not constants to insert into the product.

Log `/tmp/sc55-reuse-readiness.log`; unchanged readback assertion fails at
frame216, expectedslot7/native8. Build succeeds. Product scheduling/PCM have
not been changed in this diagnostic turn.
