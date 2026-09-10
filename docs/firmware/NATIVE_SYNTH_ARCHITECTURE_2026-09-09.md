# Native sound generator: current implementation

> Scope correction, 2026-09-10: the user requires full H8 CONTROL replacement,
> retaining the existing PCM DSP. A PCM-free NativeSynth or a default switch to
> the experimental independent renderer is not an acceptance requirement.
> Earlier entries below describe experiments and historical constraints, not
> additional work the user must accept. Follow the current acceptance scope in
> [WAVESTATION_STRUCTURE_CHECK](WAVESTATION_STRUCTURE_CHECK_2026-09-09.md).
> Existing H8/native behavior differences remain unresolved requirements.

## 2026-09-10: output frame generation owns update/effect progression

SignalRenderer::nextFrame produces the shared integer stereo frame directly,
owning the initial non-updating pass and effect-enable progression. The normal
adapter no longer supplies PCM flags to every voice/effect render call or scales
the output itself. Legacy state is adopted during dirty setup; typed chorus
configuration enables effects directly. AudioFrame is now a standalone value
header so this renderer does not import chip arithmetic or require C++20.

The adapter still performs compatibility imports/publication and is still the
NativeSynth call path. This is not yet a PCM-free NativeSynth main loop. The
owned output path is exercised without PCM, and lifecycle comparison retains
exact stereo/buses/delay/clock behavior with zero runtime state imports.

## 2026-09-10: note installation uses owned oscillator history

SignalRenderer installs resolved waveform geometry, selects its pitch source
and prepares the next key transition using its own preceding active-pass phase,
direction and boundary state. Native installation no longer reads those states
or envelope levels back from PCM RAM. Initial ramp commands arrive through the
existing native voice update; their levels remain in the envelope owner.

The PCM-free startup/reuse test now uses this installation interface, retaining
the non-updating-pass and first-active-EG readiness checks. Six standalone tests,
three-program lifecycle parity and unchanged NativeSynth checksum/partition
tests pass. The compatibility adapter still publishes histories before install
to preserve legacy readback semantics, and resolves ROM windows; neither is
claimed removed. The independent renderer's installation no longer consumes
that published history. Product default and startup timing are unchanged.

## 2026-09-10: envelope segment handoff uses owned live levels

VoiceEnvelopes now implements segment synchronization directly: a held stage
restores its saved level, while a moving stage captures the current level and
holds until the next command. Independent native control uses that owned state
instead of publishing all voice histories to PCM RAM and reading them back.
Ordinary voice and ramp-command updates likewise no longer request a full voice
readback. Compatibility latches are still mirrored for legacy callers.

All eight held/moving combinations are covered without PCM storage, alongside
the existing ramp arithmetic reference test. Six standalone tests, three-program
lifecycle parity (including zero runtime imports) and NativeSynth's unchanged
checksum/variable-block checks pass. Installation/key commits still synchronize
legacy histories and the controller still holds pcm_t: those dependencies are
not removed by this change. Product renderer selection remains unchanged.

## 2026-09-10: shared pitch routing owned by signal

SignalRenderer now owns the 32 shared pitch values, 24 voice source selections
and chorus source selection. Note installation establishes the source; voice
updates change the source value and resolve its consumers inside that owner.
The independent native update no longer traverses compatibility mode words to
rebuild every voice's pitch. Legacy import adopts routing only when initial or
explicit compatibility state is dirty; reference/native-slot adapters retain
their existing routing implementation.

The PCM-free standalone test covers two voices and chorus sharing a source,
updating a voice whose source is another voice, relinking to a previously written
source, and separating chorus onto source 31. All six standalone tests pass.
Three-program lifecycle parity still passes with zero runtime voice/effect
imports; NativeSynth checksum and variable frame-partition equivalence are
unchanged. This is ownership migration, not a product renderer default switch.
Controller readback, waveform bank resolution and initialization still need
their remaining pcm_t dependencies removed before the architecture is complete.

## 2026-09-10: direct effects and pitch updates without runtime state imports

Typed reverb/chorus setup no longer invalidates every simulated voice. Their
changes to shared pitch sources update only dependent phase increments, without
reconstructing waveform, envelope, filter or key state from compatibility RAM.
Voice updates likewise update a linked chorus increment directly instead of
invalidating the entire effect owner. Integer reference rendering is unchanged;
its compatibility invalidation remains available when simulation is disabled.

The diagnostic-only audit now counts actual independent voice/effect imports,
not just byte-register transactions. After initial setup, the three-program
131072-frame lifecycle exercise (24 voices, effect changes, mono/reuse, all sound
off and GS reset) has zero voice imports and zero effect imports. Stereo, buses,
delay memory, gates, notifications and clock still match the former slot pipeline
using the same native controller. Independent NativeSynth retains checksum
`6f5711f995a12277` and zero/1/127/129/257-frame partition equivalence.

This removes runtime reconstruction on that tested native path, not the whole
compatibility adapter. Pitch-link selection, controller readback and initialization
still depend on pcm_t; moving those semantic states to their owners remains
necessary. Product renderer selection is unchanged. This check is not a new
H8-vs-native song fidelity result or a host CPU measurement.

## 2026-09-10: voice gates and startup readiness owned by signal

SignalRenderer now owns enabled/latched/initial-EG-completed voice sets and
prepared restart positions. Native note installation prepares a restart and
key commits update the owner's key set. Each render handles gate selection,
oscillator restart and latch/initial-EG progression itself. The independent
path no longer uses PCM_PrepareSimulatedVoices to make those decisions each
frame. Compatibility imports still reconstruct dirty control/waveform state;
removing those imports remains work, not a completed architectural migration.

Native post-enable readiness now queries SignalRenderer's completed active
EG pass rather than its mirrored PCM mode word or a PCM cycle-delay heuristic.
Reference-chip rendering retains the verified two-pass startup protection.
NativeSynth's independent active-key snapshot also comes from the signal owner.

A standalone test starts/reuses a voice with no PCM object, including a
non-updating pass, key latch and first active EG pass; readiness stays false
until initial gain commands have taken effect. This exposed a NEON gather of
uninitialized ROM pointers in silent lanes adjacent to a live voice. Those
lanes now skip waveform reads, matching the scalar path. Scalar/vector tests,
six standalone checks and the three-program lifecycle comparison pass.
The independent NativeSynth preserves checksum6f5711f995a12277 and variable
frame-partition equivalence. This is same-controller renderer parity, not
full H8 fidelity or proof that the product default can yet change.

## 2026-09-10: frame generation owns the audio clock

SignalRenderer now owns phase, the shared random word and rendered frame count.
Its render operation advances these together with voices/effects; callers can
no longer advance the clock separately, provide an arbitrary phase per frame,
or forget the advance. Initialization adopts the existing phase/random/count;
MIDI reset does not reset that continuing audio timeline. Random reads are pure.

PCM_RenderIndependentFrame now prepares compatibility imports and receives one
completed frame, then publishes clock/random readback mirrors. Those mirrors
are outputs, not the clock source. NativeSynth state reads the owned frame count.
This completes the clock ownership work interrupted by the audible-bug fixes.

Standalone signal testing verifies32768 frames from a nonzero adopted clock,
including wraparound, repeated random reads, voice/effect output and delayed
boundary acknowledgement. Three integrated programs48/80/120 retain exact
audio/mix/delay/event parity against the previous pipeline using the same C++
controller. These comparisons are not H8-controller fidelity claims.

The user confirmed the separate mono admission and PCM initial-EG race fixes:
see MONO_NOTE_ON_ATTACK_FIX_2026-09-09.md and PCM_STARTUP_ATTACK_FIX_2026-09-10.md.
Their startup protection is retained. The product default remains referenceChip
with the C++ controller; the independent signal path is not silently promoted.

Remaining structural work is still substantial: NativeMelodicPlayer retains
pcm_t and ControlWriter, waveform installation/control still mirrors compatibility
fields, and signal preparation still imports dirty slots. Removing those data
dependencies and validating the resulting independent path against real-song
H8 behavior are required before the WAVESTATION-style goal is complete.

## Runtime byte-I/O audit and shared random source

An opt-in SC55_NATIVE_IO_AUDIT CMake option on cpu-roles counts PCM_Read/Write;
the instrumentation is absent from product builds. Before migration, the
expanded lifecycle scenario produced reads/writes579/163,612/146,360/90 for
programs48/80/120. All were shared-random reads used by modulation/pan/pitch.

Those consumers now request randomWord from the native owner. The existing
shared audio-clocked source is unchanged; reads do not advance it or create
per-voice sequences. Legacy transaction ordering (including two pitch reads)
is retained in the oracle fallback. After this change the audited scenario
has0 reads and0 writes for all three programs, and now fails on any fallback.
Audio/allocation/event comparisons and checksum6acecde1d41c6db6 still pass.

Scope: counters reset before the first note, so this proves the tested running
lifecycle (including GS reset), not initial construction or every MIDI feature.
The native target still reads/writes pcm_t compatibility fields internally.
Removing those fields/dependencies remains necessary; zero byte I/O alone is
not completion of the WAVESTATION-style architecture goal.

## Waveform-event response updates pitch directly

HandleVoicePcmBoundary now submits its loop-transition pitch through a typed
operation. The renderer updates the affected pitch source and linked consumers
without dirtying/rebuilding waveform, filter or envelope state. Stop-at-end
continues through the typed gain/ramp stop path. The reference adapter retains
its existing register behavior; native event handling no longer emits this
remaining three-byte pitch transaction.

--direct-voice-updates now also checks4096 loop-pitch updates against byte writes,
including linked consumers and unchanged histories/no waveform invalidation.
Expanded three-program lifecycle audio/event comparisons pass; independent
NativeSynth retains checksum6acecde1d41c6db6. Other compatibility state still
remains and the default renderer selection is unchanged.

## Gain consumers read the renderer owner

Stop/reuse gain readback and UI-meter snapshots now read current native ramps
without materializing all voices' oscillator/filter register mirrors. Pending
legacy control writes remain authoritative until rendering consumes them;
PCM_PeekVoiceGainLevels and the single-ramp read enforce that ordering. Blanket
readback synchronization at controller-service and UI-snapshot entry was removed.
Actual legacy byte reads/writes still synchronize at their compatibility seam.

--direct-voice-stop includes a stale mirror/new renderer case and a newer pending
control-write case, checking both values and absence of unnecessary writeback.
--independent-synth retains6acecde1d41c6db6 and block partition parity. The expanded
three-program lifecycle comparison (including All Sound Off, mono replacement,
GS reset and new note) retains exact audio, allocation and event results.
This does not imply that pcm_t itself has been removed from the controller.

## Direct control now covers stop/reset/reuse callers

Removed remaining anonymous byte-writer adapters from NativeMelodicPlayer's
stopPartGroups, GS/GM reset draining, All Sound Off, invalidated mono reuse and
mono capacity preparation. They now pass the same ControlWriter as ordinary
note creation and periodic control, so semantic stop/readiness/ramp operations
are not silently bypassed by these call sites. Some lower-level compatibility
operations still exist; this does not claim that ControlWriter is PCM-free.

The integrated comparison now runs131072 frames per program48/80/120:24-note
input, effect/pitch/pan changes, All Notes Off, All Sound Off, mono legato,
program-change replacement, GS reset and a fresh note. It checks exact output,
delay memory, notifications, free-voice counts and reset completion, including
all24 voices being available before restart. This extends previous short
playback tests to the actual stop/reuse/reset flow.

## Prepared waveform installation and direct envelope handoff

Native voice installation now resolves sample/exponent windows from its prepared
start/loop/end/mode and supplies PCMSim_SetWaveform immediately. It no longer
invalidates every voice for a later full register import. Linked pitch updates
remain part of ApplyVoiceUpdate; actual oscillator restart still belongs to the
paired key-enable transition, not the earlier geometry installation.

Post-enable cutoff and three-ramp synchronization now update renderer-owned
envelopes directly instead of dirtying the entire voice waveform/filter state.
The compatibility mirror and pending unrelated legacy updates remain intact.
--direct-renderer-start compares4096 prepared starts with the original register
commit + slot import: geometry, controls, envelope levels and retained oscillator/
filter history match, with no deferred import for the installed slot. Independent
and native-voice synth hashes remain6acecde1d41c6db6 and9b31ad6ba8b312dc.
Control still retains pcm_t and some initial/readback state; this is not yet a
fully independent default product engine.

## Waveform notifications are renderer-owned events

SignalRenderer owns one outstanding voice-boundary event and exposes presence/
take operations to its audio-thread controller. The independent path no longer
raises a PCM IRQ or reads status register3e to acknowledge it. Other voices
remain eligible while an event is pending, preserving the original arbitration
and frame-end scheduling. Reference chip rendering retains its existing IRQ
adapter. No cross-thread queue or audio-thread allocation was introduced.

The standalone signal test now enables waveform notifications and delays
acknowledgement for19-frame intervals while comparing against the existing
boundary selector; pending/consume behavior, output and tails pass. Integrated
three-program/24-note comparisons retain exact audio and notification timing.
Independent synth checksum remains6acecde1d41c6db6 with block partition parity.
This does not remove the other remaining pcm_t control/initialization state.

## Voice stop and termination use ramp operations

The native lifecycle reads gain levels and sets a specific renderer-owned ramp
command instead of selecting a PCM channel and assembling byte transactions.
This covers choosing the quieter gain for forced stop, reuse preparation and
normal termination. PollEnvelopeTermination retains its exact-zero requirement
before detaching voice links and notifying allocation; no hard mute or premature
reclaim has been introduced. The old transaction branches remain the oracle.

--direct-voice-stop exercises all24 slots with unequal/equal/zero gain levels,
including link detachment and activity/status changes. Its native target has no
byte I/O operations. --native-reserve-stealing still matches H8 for48 admissions
across three parts and rejection at fully protected capacity. Independent synth
checksum remains6acecde1d41c6db6. These checks do not prove every lifecycle path
has migrated: startup and other prepared-state compatibility remains.

## Complete periodic effects control uses semantic operations

NativeMelodicPlayer now calls EffectsControl::advance once for the effects
control tick. Its target exposes prepared chorus/reverb setup, coefficient/mix
updates and beginReverbDrain; it has no register reader, byte writer or address
writer. The legacy readback/retry lambdas and effectChorusResume state have been
removed from the native player. The original transactions remain available only
through the compatibility branches used by oracle callers.

PCMEffects applies input-filter/output/feedback/spread updates directly without
reloading its running state. Drain changes network settings and lets history
decay; it does not clear the delay memory. A pcm_t compatibility mirror remains
in the adapter for the reference renderer and other remaining control paths.

--direct-effect-transitions drives the same advance interface as the product
through all8 macros, fade-out/drain/setup/fade-in and differential changes:
1600 control ticks and204800 audio frames match old transactions, latch/state,
stereo output and delay histories. The native test target cannot accept raw
register operations, making an accidental fallback a compile error.
--independent-synth retains checksum6acecde1d41c6db6 and block partition parity.
This completes this control interface migration, not the overall native synth:
voice lifecycle/register dependencies and full H8 audio parity remain open.

## Prepared reverb/delay network

ReverbSetup carries resolved diffusion/tail taps, diffusion/comb/damping
coefficients, output routing and spread command. EffectsControl resolves ROM
programs and the two delay-character time formulas before submission.
PCMEffects::configureReverb installs this directly, preserving delay memory,
filter histories and the current spread level. Native setup no longer emits
the original bank-selected byte sequence. The legacy adapter mirrors the
configuration for the remaining control code and reference renderer.

--direct-reverb-setup compares all8 ROM characters at times0/1/64/127 against
the retained byte path. Configuration/latches and subsequent32768 effect frames
match; the test asserts audible residual output so silence cannot pass it.
Fades, drain phases and differential coefficient updates remain register-based;
neither this operation nor the chorus setup constitutes complete effects-control
migration. No generated project or default renderer selection was changed.

## Prepared chorus setup reaches the effects owner directly

ChorusSetup carries resolved begin/end/position, phase increment and four return
coefficients. EffectsControl submits it as one operation; PCMEffects installs
the geometry/routing without discarding delay/filter histories or prematurely
advancing interpolation. Native service no longer performs byte writes and
readback polling for this setup. The compatibility adapter maintains the old
control view; remaining fades and reverb control still use word operations.
This is a real prepared-setting interface rather than exposing register
addresses on the new effects-owner operation.

--direct-chorus-setup compares128 parameter sets against the retained original
byte-write/readback path, including subsequent effect frames. The independent
three-program/24-note comparison now includes chorus macro changes5 and7 and
still matches stereo, mixes, delay memory and control state exactly. Independent
NativeSynth retains checksum6acecde1d41c6db6; all six standalone DSP tests pass.
These checks validate the refactor, not original-H8 whole-engine audio parity.

## Renderer state is no longer copied back every sample

The independent path retains envelope levels, wave positions and filter state
in SignalRenderer::voices across frames. PCM_SynchronizeNativeReadback supplies
the remaining register-shaped controller only at a control read/write or UI
snapshot boundary. All byte-level and typed compatibility operations synchronize
before mutation so a later import cannot overwrite newer renderer history.
Key-enable latching and waveform notifications still happen at their original
frame boundaries; lazy conversion does not delay either event.

--independent-synth retains checksum6acecde1d41c6db6 with zero/1/127/129/257-frame
partitions. --independent-signal explicitly requests readback for comparison and
retains exact stereo/mix/delay/control results for programs48/80/120. The block
partition test also covers batches without per-frame diagnostic readback.
Single headless measurements changed from about5.68/20.13ms to4.77/18.16ms per
audio second (idle/24-note independent path); these are not repeated controlled
benchmarks or Logic CPU measurements. Default product renderer is unchanged.

## Synth-owned rendering and controller deadlines

NativeSynth's independent path now writes directly into the caller's span.
It does not call PCM_Update or retain an output pointer for a PCM callback.
NativeMelodicPlayer supplies bounded batches ending at the shared control
deadline (or one frame during pending transitions). The renderer returns the
number completed, yielding early for a waveform boundary; only then does the
controller advance its own elapsed time and service MIDI/control work.
MIDI timestamps no longer read the renderer's pcm.cycles counter.

The PCM compatibility adapter still prepares/publishes legacy control state
per frame. It is explicitly transitional, not an independent controller yet.
The old renderer uses the same scheduler through a compatibility overload.
--independent-synth retains checksum6acecde1d41c6db6 and block partition parity;
--native-synth passes with checksumab27a7692f6bdaf1. --independent-signal retains
exact stereo/delay/control results for programs48/80/120 and24-note input.
No product default, generated project or host parameter was changed.

## Independent complete signal renderer

`sc55_signal_renderer.h` composes owned PCMSimVoices and PCMEffects with the
four-bus mixer. It owns the pending mix, uses its previous sends to feed effects,
and returns the previous output while computing the next frame, preserving the
existing one-frame pipeline. Waveform-boundary events are returned as voice
indices. No PCM/H8/JUCE dependency is linked by its standalone target.

`sc55-signal-test` compares32768 frames against the existing separately driven
voice/effects/mixer operations, including held clock passes and shutting all
voices off after8192 frames. Output and delay memory match; post-key-off tail
energy remains nonzero. All six independent DSP tests pass.

The renderer is now connected to NativeSynth's real MIDI controller through
VoiceRendering::independentSignal. The compatibility adapter prepares dirty
voices, publishes readbacks and forwards waveform events. Rendering bypasses
the chip slot pipeline: SignalRenderer owns voices, effects and bus history.
NativeSynth owns its lifetime; construction remains off the audio thread.

Validation: cpu-roles --independent-signal compares 65792 frames per program
(48,80,120), with 24-note input, pitch/pan/send changes, note-off and all-notes-off.
Against the existing native-voice + floating-effects path, stereo samples,
mixes, delay memory, gates, IRQ and control/readback match exactly.
--independent-synth exercises the actual NativeSynth constructor and checks
zero/1/127/129/257-frame partitions (checksum 6acecde1d41c6db6).
The existing --float-effects-audio checksum remains e873da9a8e735b83.

This is not the default renderer yet. The floating effects algorithm's parity
with original integer effects is not proved by these comparisons. Control still
uses pcm_t compatibility registers and the adapter advances its clock; removing
that control-state dependency remains required for the requested architecture.
These headless checks do not establish Logic CPU usage or AUv3 host behavior.

## Independent dry/effects bus mix

`sc55_audio_buses.h` owns dry stereo and reverb/chorus return mixing, including
the existing signed20-bit operand semantics, rounding and feedback routing.
The simulation path calls it and only mirrors the resulting four buses into
PCM. No chip object, firmware state or allocation is required by this operation.
65536 randomized comparisons cover half-unit rounding and wrap points against
the previous mixer; the independent DSP suite and integrated audio checksum
9b31ad6ba8b312dc pass. This is not a change to effect algorithms or the default
reference-chip renderer, and does not yet remove PCM scheduling/state entirely.

## Renderer-owned waveform boundary events

PCMSimVoices now owns boundary-enabled and boundary-latched state.
PCMSim_CollectBoundary emits the first eligible voice, honors an already-pending
notification, clears inactive latches and preserves non-updating-clock behavior.
The PCM simulation adapter imports explicit register changes, mirrors the owned
state and forwards the event; it no longer calculates waveform-boundary crossing.
Typed control publication updates boundary enable directly.
The standalone test compares4096 randomized32-voice states to the former
adapter predicate, including held notifications/reverse playback/non-updating
passes. DSP tests and integrated waveform/EG readback pass; audio hash remains
9b31ad6ba8b312dc. Default reference-chip rendering is unchanged.

## Broad renderer comparison

`--voice-renderer-programs` compares128 capital programs at note60/velocity100;
`--voice-renderer-polyphony` sends24 Note Ons (48..71, velocity100) per program,
allowing normal voice allocation/stealing. Both use identical C++ controllers
with reference PCM vs native voice rendering, default effects and note-off tails.
Neither is an H8-vs-native control comparison or a perceptual-equivalence gate.

Observed relative waveform RMS error: single-note worst program121=.011211,
one program above1%; 24-note worst program121=.009097, none above1%. Program
121 single-note level ratio=.999922. The1% count is descriptive, not an approved
fidelity threshold. All cases produced audio without engine failure. This expands
earlier four-program evidence but does not cover every velocity/key/variation,
rhythm or clipping scenario. Default renderer selection is unchanged.
Logs: `/tmp/sc55-program-scan.log`, `/tmp/sc55-poly-scan.log`.

`--voice-renderer-stages` isolates worst-case program121 using the same native
controller and checks waveform position, pitch, EG commands/readback and random
clock every frame. They match. DPCM reference state RMS difference is exactly0;
filter low/band differences are1.117516/.910716 in20-bit units. Bus signal RMS
is168.453 and error RMS1.889; bus relative error=.011212 and post-DAC=.011211.
Thus the discrepancy already exists in filter computation, not MIDI scheduling,
waveform reconstruction or the final DAC stage. The relative metric is magnified
by low signal level. This does not establish perceptual equivalence, but does not
justify program-specific corrections or changing control timing. Evidence:
`/tmp/sc55-render-stages.log`. No product sound algorithm changed in this probe.

## Shared-clock renderer entry

`PCMSim_RenderFrame(voices,clock,buses)` now owns all voice EG updates followed
by signal generation. The PCM simulation adapter publishes gates/control changes
and mirrors the resulting readback, rather than advancing individual envelopes
itself. Signal-only diagnostics explicitly call PCMSim_RenderSignals.
The clocked entry/EG loop are inline: an initial out-of-line implementation
increased sampled benchmark cost to4.72/20.21ms idle/24notes; making optimization
across the caller available restored3.13/16.50ms. These are diagnostic timings,
not Logic results or a claim of further product speedup. Audio checksum and
waveform/EG readback remain unchanged. The default reference renderer is untouched.

## Waveform and restart ownership

PCMSimWaveform carries resolved sample/exponent windows and playback geometry.
PCMSim_SetWaveform changes controls without restarting phase/filter history;
PCMSim_RestartVoice owns oscillator/reference/filter reset separately from EG
and control values. Both operations live in pcm_sim.cpp and require no pcm_t.
The real simulation adapter now decodes registers into these operations instead
of implementing history reset itself. Standalone renderer tests exercise control
publication mid-note and explicit restart against manual scalar state setup.
Five independent DSP tests pass; integrated waveform/EG readback and audio hash
9b31ad6ba8b312dc remain unchanged. Default reference PCM rendering is unchanged.
This is a step toward native renderer ownership, not removal of the remaining
controller/PCM state dependency or a new product-default renderer selection.

## Partial reserve wiring

The capacity algorithm already accepts reserves, but product melodic/mono and
rhythm callers supplied an empty policy. They now pass the player's policy,
initialized from imported system defaults8018..8028 and restored on reset.
`--native-reserve-defaults` confirms all16 reserves and starting priority match
H8 after120 million boot cycles. This verifies default configuration wiring,
not complete multi-part stealing fidelity. SysEx reserve changes are now wired:
40 01 10 requires all16 values with total<=24; invalid lengths/totals leave the
previous table unchanged.40 01 20 sets starting priority, clamped to15.
`--native-reserve-sysex` failed before this wiring and now matches H8 on12
messages including valid/oversized totals, shifted addresses, short/long/empty
payloads and priority clipping. Remaining policy fields and multi-part voice
stealing outside the following scenario still need verification.

`--native-reserve-stealing` now sends the same reserve SysEx (8 voices each for
three melodic parts), program80, and48 successive Note Ons to H8 and native.
Each message is followed by200ms of rendering. After each admission, all16 part
voice counts and the per-note multiplicities of surviving voices for the three
parts match. This exercises full capacity and replacement without depending on
physical slot numbering. Other programs, drum protection, release/hold cases and
precise transient timing are not proven by this test.

The same test now fills a part with24 protected voices, attempts a mono note
on another part, then frees capacity and plays another note. Before the fix,
the mono/source admission path treated the valid `capacity=false` result as a
fatal engine error. It now drops that admission only (nullopt still indicates
an internal failure). Normal melodic admission likewise no longer counts this
valid resource rejection as an unsupported feature. H8 part counts match on
rejection and subsequent playback; the MIDI queue drains and the synth stays
healthy. Evidence: `/tmp/sc55-reserve-protected-before.log` and `-after.log`.

## Inactive reference-PCM voice bypass

Default PCM rendering now bypasses waveform decoding/interpolation/filter/gain
work for mk1 slots0..23 with key disabled on an updating pass. Key-on and
non-updating passes retain the full path. Cutoff advances normally; the shared
epilogue clears filter/history/gain readback exactly as before. All effect return
mixing remains at its original slot boundaries and shared clocks keep running.
`pcm_t::skip_inactive_voices=false` retains the old pipeline for differential
testing; this switch is not used by normal product startup.

`--inactive-voice-equivalence` compares8192 randomized mixed-key frames, including
nonzero initial histories and pending key-on, against the full pipeline. Voice
RAM, effect memory, buses, IRQ and clock match. `--native-synth` passes with the
default renderer and variable blocks. Updated Release idle run:1000 audio seconds
in5.714041 wall seconds; single-second check5.662ms idle and31.236ms with24 notes.
Earlier28.077650 seconds used sampling, so it is not a strictly controlled speed
ratio. Logic performance and the next Xcode-built app remain to be checked.

## Idle profile with the current default renderer

User's Logic measurement: maximum polyphony around3.3%, idle around2.6%.
`--profile-native-idle` now provides a persistent sampling target using the
actual default C++ controller/reference PCM renderer,10 audio seconds warmup
then1000 audio seconds in256-frame spans. This excludes JUCE/Logic overhead.
Release run took28.077650 wall seconds, with sampling enabled (not a clean
benchmark). A3-second macOS sample collected2163 main-thread samples:2161
under PCM_Update and2 under serviceWork/control updates. Hot lines include
wave ROM fetches and interpolation in the voice loop, despite no MIDI notes.
The earlier ~3ms/audio-second idle results used the experimental native voice
renderer and must not be presented as current-default product performance.

Next performance target is inactive PCM voice work, not finer C++ control
refactoring. Preserve all readback, clocks and effect tails; a zero voice mask
alone is not proof that arbitrary PCM state or audio can be discarded.
Reproducer: `sc55-cpu-roles <v1.21-rom-dir> --profile-native-idle`.
Evidence: `/tmp/sc55-idle-profile-sample.txt` and
`/tmp/sc55-idle-profile-run.log`. No runtime optimization is claimed yet.

## Default product engine

Normal builds and launches now select the C++ controller without environment
variables, for both Standalone and AUv3. `NUKED_SC55_USE_H8=1` explicitly selects
the H8 comparison path. The old `NUKED_SC55_NATIVE_PREVIEW` variable is no longer
used. C++ mode requires v1.21 ROMs and reports an error for unsupported ROMs;
it never silently switches back to H8. PCM rendering remains unchanged unless
the separate experimental `SC55_SIM=1` switch is selected.

Validation: actual adapter `--product-engine-selection` confirms C++ with the
selection variables unset, and H8 with `NUKED_SC55_USE_H8=1`.
`--native-adapter` passes44.1/48/96kHz without the former preview variable.
Xcode Release Standalone (including embedded AUv3) builds successfully with
the project's signing settings, overriding only CONFIGURATION_BUILD_DIR to
`/tmp/sc55-native-default-products`. Logic discovery/playback remains unverified.

## Native startup lifecycle operations

The native writer now exposes key-mask commit, gain-level snapshot and enable
completion operations. Prepared batches use these instead of byte-register
callbacks for key removal/enable, reuse readiness and post-enable envelope setup.
Pending enable still waits for the renderer's ready bit; no arbitrary delay or
immediate key-on is substituted. Reserved key-mask bits, shared latches and
renderer dirty state retain the reference transaction semantics.

`--direct-voice-lifecycle` passes4096 comparisons covering key masks, gain levels,
pending/ready enable and complete register/latch state. This narrows the control
layer's device interface but does not remove the PCM mirror or its renderer.

## Voice installation transaction

`VoiceRenderStart` groups sample start/loop/end, playback mode and initial
controls. Native melodic, reused/mono and rhythm startup writers now support
`PCM_InstallVoice`; pending startup uses the same writer. Activation state is
computed by `ActivateVoiceState`, separately from publishing DSP controls.
Paired key enable and readiness polling remain separate scheduler transitions.
The PCM adapter still owns the register mirror and subsequent waveform import;
this does not yet constitute a chip-free native signal chain.

`--direct-voice-start` passes4096 comparisons against the byte-register commit,
including sample/control registers, latch state and activation branches.
Release diagnostic build and `--native-synth-fast` pass, retaining checksum
9b31ad6ba8b312dc and variable-block equivalence. No Xcode GUI/Logic verification
was performed for this change.

## Direct envelope readback

The native periodic control writer now synchronizes all three voice envelopes
through `PCM_SynchronizeVoiceEnvelopes`, without channel/byte-register callback
dispatch. Running ramps are held and their current levels returned; already-held
ramps receive the software level. Delay/finished gates and release consumption
order are unchanged. The PCM mirror and latch state remain compatible with the
still-register-based startup/termination paths; renderer import uses the existing
dirty flag. This is not yet removal of the complete PCM bridge.

`sc55-cpu-roles --direct-envelope-readback` passes4096 transactions covering all
held/running combinations against the original register synchronization, including
returned levels, full ram2, latches, channel selection and dirty flags.
`--native-synth-fast` retains audio checksum9b31ad6ba8b312dc and matching variable
block partitions. These checks establish refactor equivalence, not H8/native
audio parity or Logic host performance. Latest app has not been rebuilt here.

## Bounded host output

The product adapter now divides large host spans into source-FIFO-sized
segments, consuming each before generating the next. Previously a200000-frame
host span at44.1kHz requested more source frames than the65535-frame usable
FIFO capacity. The native producer could never satisfy that request and looped
forever. This is independent of H8/native synthesis parity.

Regression --native-large-block runs the actual product adapter and compares
one200000-frame request with257-frame requests. Before the change the command
hit its3-second process alarm; after the change it completes in under a second
with identical stereo samples. --native-adapter also passes44.1/48/96kHz after
the shared PCM-envelope changes. No callback allocation or FIFO expansion was
added. Nonfinite preparation sample rates are rejected before chunk arithmetic.

## Verification boundary (latest)

### Controlled isolation of large waveform differences

The direct H8/native comparison now has explicit **diagnostic-only** environment
switches. They do not change product initialization or implement a sound-engine
fix, and their results must never be reported as normal waveform parity:

- `SC55_COMPARE_SYNC_RANDOM=1`: copy the H8 LFSR state once immediately before
  note-on. Program48 held error remains1.096957 (baseline0.972252); a common
  initial seed does not give equal later reads at different execution times.
- `SC55_COMPARE_PIN_RANDOM=1`: pin channel30's random state in both PCM sample
  callbacks. Program48 attack/held error becomes0.005161/0.003444. This isolates
  random-input dependence as the dominant difference in this Strings case;
  it does not justify replacing the product random generator with a constant.
- Program80 with pinned random still has attack/held error0.670905/0.712461.
  Both voices' entire attack pitch-value sequences match (1 and6 distinct
  values), but slot23's successive changes occur at247/305/551/899/1058 frames
  after H8 key-on versus52/308/565/821/1078 in native. Both use32kHz here.
- `SC55_COMPARE_REPLAY_PITCH=1` additionally replaces native PCM pitch each pass
  with the recorded H8 pitch schedule, using the one measured key-on offset.
  With pinned random, program80 attack/held error drops to0.003156/0.002276.
  Release error remains0.195355. No gain fit, per-window time fit, or product
  timing change was made. The experiment identifies pitch-write timing as the
  dominant cause of this held-waveform difference, not a wrong pitch-value
  sequence. The override affects diagnostic PCM input, not native control state.

Evidence: `/tmp/sc55-h8-difference-current.log`,
`/tmp/sc55-h8-difference-synced.log`, `/tmp/sc55-h8-difference-pinned.log`,
`/tmp/sc55-h8-square-pitch-sequence.log`, `/tmp/sc55-h8-square-replay-pitch.log`.
All use `--compare-native-audio <cache> <romdir> --program <48-or-80> --dry`.
The switches print diagnostic-perturbation notices in the current comparator.
The next sound-engine boundary to resolve is the semantic common-control
transaction and its PCM write times, rather than rewriting correct pitch
arithmetic or inserting a fixed note-age delay. Randomized modulation must be
compared with controlled inputs separately from ordinary audio output.

For program120, pinning random makes both held AC RMS values zero; the existing
amplitude gate still reports FAIL because it requires nonzero reference energy.
This is not evidence of a missing sustained voice in a decaying sound. Its
baseline H8 noise floor and native undithered output remain different.

The behavioral reference for the migration is execution of the existing H8
emulator with the v1.21 ROM. Hardware measurements are not a prerequisite for
that comparison, and native-controller/PCM-renderer comparisons alone do not
prove that the H8 replacement is correct.

The current source was rebuilt and directly compared with:
`sc55-firmware-oracle --compare-native-audio <generated-cache> <v1.21-rom-directory> --program 80 --dry`.
Log: `/tmp/sc55-h8-direct-p80.log`. The amplitude-only gate passes, but key-on
occurs at frame123 in H8 and frame1 in C++. After that measured122-frame offset,
attack relative waveform error is0.670895 and held error0.712449. This is NOT a
waveform-parity pass. Initial pitch/address/commands match for the two allocated
voices, but subsequent command times differ. The startup random states also
differ (33cb/d9b1), so this result alone does not isolate a single cause.

Separately, PCM fast-renderer EG approximation has been removed. Both PCM paths
now use the existing envelope integrator, including controller-visible readback,
and the filter uses the previous cutoff as the reference path does. The SIMD
waveform/filter renderer remains. Program80 renderer-only relative waveform
error fell from0.046854 to0.000542. Per-frame envelope commands/readbacks now
match in --voice-renderer-envelope. This fixes a PCM-path discrepancy, NOT the
independent H8/native-controller difference above.

The direct comparator now records the completed AC5A store (5af5 -> 5af9)
and the first executed32f0 entry in task8, alongside PCM command transitions.
It does not count instruction entries interrupted before execution. In the
program80 dry attack window,64 dispatches consumed64 expirations, each with
elapsed1; dispatch intervals ranged79824..226884 cycles (nominal160256).
Examples: counter reads at frames249/377/623, first voice entries259/389/634.
The existing command trace then shows slot23's hold command at258 and its next
parameter set at370. Thus timer notification, task entry, and the actual PCM
parameter write are distinct boundaries. This observation is not permission
to insert arbitrary matching delays in the native engine. Kernel scheduling,
voice computation and PCM waits must be distinguished before changing timing.
Evidence: `/tmp/sc55-h8-control-dispatch.log`, same command as above. No native
clock period or audio scheduling was changed for this diagnostic.

The direct MIDI comparison now asserts initial allocation, sample address,
pitch and all three ramp commands, not just sample address. Programs0/48/80/120
all passed these initialization assertions with note60/velocity100 and dry
CC91/93 settings. This is one note per program, not all-preset coverage.
Whole-test results remain mixed: held amplitude passes0/80, fails48/120.
Program0's onset-aligned held waveform error is0.007604; program48's is0.972252.
Program120's held window has zero native output versus H8 AC RMS28301.3, so
the amplitude-only failure must be distinguished from sustained-tone failure
before drawing a conclusion. No threshold was relaxed to turn these green.
Logs: `/tmp/sc55-h8-initial-{0,48,80,120}.log`.

After the inactive-voice shortcut, one Release timing run measured native
idle2.795ms and24-note16.983ms per audio second, versus H8 with the same renderer
19.370/55.152ms. The exact EG has a cost versus the former approximation; these
are diagnostic wall times, not Logic measurements. Native remains opt-in.

The target is a semantic sound generator, not a set of faster H8 opcodes.
`sc55::NativeSynth` now owns sound data, waveform storage, PCM synthesis and
the MIDI/voice/control state. Its audio-thread interface is `push(midi)` and
`render(frames)` at 32 kHz. Host resampling remains in NukedSC55Emulator.

## What changed

- Native initialization no longer creates Emulator, MCU, peripheral timers,
  sub-MCU or LCD. The native branch loads the generated data-only cache and
  constructs the sound generator directly.
- The host feeds MIDI at render-segment boundaries and requests frame spans.
  It no longer steps the native player once per source sample.
- PCM device identity, sample output and IRQ notification no longer dereference
  an MCU. The legacy MCU adapter lives in emu.cpp. The PCM object file has no
  MCU function dependencies (checked with nm).
- The legacy mcu pointer in pcm_t is retained for existing oracle probes, not
  used by synthesis. NativeSynth leaves it null.
- Voice initialization remains event-driven. The shared control clock and
  persistent per-voice EG/LFO states remain inside the sound generator.
  This extraction does not change the current 160256-device-cycle control
  period or pretend that the unresolved physical clock conversion is settled.

## Verification performed

Release target: `/tmp/sc55-cpu-roles-build/sc55-cpu-roles`.
ROM set: mk1-v1.21. No ROM bytes added to the repository.

`--native-synth` checks identical integer audio across 257-frame spans and
0/1/127/129-frame partitions, including note-on and note-off. It also measures
one audio second of native and H8 rendering with real PCM synthesis on both.

Observed run (wall-clock milliseconds, not Logic CPU meter):

| Workload | Native | H8 |
|---|---:|---:|
| Idle | 29.396 | 38.835 |
| 24 Note On messages | 32.480 | 67.855 |

These are single-run diagnostic timings, not a statistical benchmark or proof
of audio parity. Import, initialization and firmware boot are outside timing.

`NUKED_SC55_NATIVE_PREVIEW=1 SC55_TEST_CACHE=<temporary directory>` with
`--native-adapter` exercises actual product adapter initialization, cache
generation, repeated release/prepare and variable render lengths at 44100,
48000 and 96000 Hz. All produced finite, non-silent audio.

`--midi-timing` output was identical before/after this extraction, including
periodic task dispatch, note-on PCM key masks and pitch-bend PCM writes.

## Still incomplete

- Native is now the product default at the user's request. This does not establish
  H8 audio parity or completion of the architectural migration.
- NativeMelodicPlayer still retains firmware-shaped startup/stop handshakes.
  Stable rendering now batches PCM passes as described below; unfinished
  handshakes still yield after one PCM pass.
- Previously observed independent H8/native audio differences remain; block
  partition equivalence is not a substitute for resolving those differences.
- Basic native LCD fields, envelope activity meters and part controls are now
  connected as described below. ALL-mode instrument/channel operations and
  firmware-specific display features remain incomplete.
- No Logic/AU/VST3 host validation was performed in this change. Generated
  projects, Projucer settings and unrelated user UI changes were not edited.
- Idle cost is still dominated by PCM rendering; removing H8 alone has not
  achieved the requested idle CPU target.

## Common-clock block scheduling

NativeSynth now requests a frame span from the voice controller. It renders
until the next common control deadline, the end of the span, or a PCM IRQ.
PCM_Update's optional interrupt yield happens after the complete pass that
raises the IRQ, so acknowledgement and voice-state updates precede the next
pass. Legacy H8 calls retain deadline-only behavior.

The controller sleeps between incoming MIDI, shared-clock expiration and PCM
notifications once transitions are settled. It no longer scans 24 voice stop
states and the MIDI/effects services twice for every stable PCM frame. Pending
startup/stop/reset/effect readback retains one-pass scheduling; events are not
quantized to the control tick to obtain this saving.

Before/after scheduling change, the note-on/off partition fixture's integer
audio FNV-1a checksum is `ab27a7692f6bdaf1`. New native timings in one Release
run were idle 25.972 ms and 24 notes 29.094 ms per audio second (previous native
run 29.040/32.262 ms). H8 comparison was 38.387/67.726 ms. Product adapter
checks at 44.1/48/96 kHz also passed. These measurements do not establish
H8/native waveform parity or completion of the migration.

## Semantic state handoff to the editor

SynthState contains copies of the 16 GS-ordered parts (MIDI channel, tone,
bank, volume/expression/pan/sends/key shift, allocated voice count and rhythm
mode), the actual active PCM voice mask, rendered frame count and failure flag.
No view or pointer into live sound state is returned. Voice counts describe
allocation, not per-part audio amplitude.

NativeSynthStateExchange uses three fixed buffers and a lock-free unsigned
atomic exchange. One audio producer owns the back buffer; one message-thread
consumer owns the front buffer; the middle buffer transfers ownership. Slow
editors drop intermediate states. No allocations, mutex or UI call is added
to audio rendering. Initialization may publish while the audio owner is stopped.

NukedSC55Emulator.getNativeState requests a new snapshot, returns the last
complete one and is message-thread-only. PluginProcessor.getUiStatus now
includes this state. Native debug publication reports actual voice mask/time
and explicitly identifies the native engine, rather than reading a missing MCU.

Verified with 100000 concurrent producer/consumer publications (complete,
monotonically advancing snapshots), actual adapter note-on/CC7/readback/release
at 44100/48000/96000 Hz, and unchanged integer audio checksum. This is a focused
handoff test, not a ThreadSanitizer run or host validation. The headless target
builds the product adapter; the full PluginProcessor/format targets were not
built here. Native LCD rendering from this model is now connected as below.

## Native front panel and LCD

The message thread enqueues part selection, program, volume, pan, sends,
key shift and MIDI-channel adjustments in a fixed 64-entry SPSC queue.
The audio owner drains it at render boundaries and applies semantic operations
to NativeSynth, without H8 button pulses. Part/global MUTE and ALL-mode master
volume/pan/key shift and effects levels are now connected. Other ALL-mode edits remain unsupported instead of
accidentally modifying the selected part.

--panel-semantics executes actual H8 button presses and compares a separate
NativeSynth at each settled operation. Part MUTE clears the0x0200 note receive
bit and stops existing voices; unmute does not restart them. Global MUTE gates
note reception for all parts and stops their voices without overwriting each
part's receive flag. ALL+LEVEL adjusts master volume, not16 part volumes.
The native implementation uses the existing stop lifecycle, retaining stop
requests while activation is pending. Panel commands, LED state and LCD ALL/
master-volume readback are connected in the product adapter. No H8 is executed
by NativeSynth. Tested part/global mute, notes arriving while muted, unmute,
ALL toggle and master-volume decrement against H8; --native-adapter verifies
the actual queued-button path at44.1/48/96kHz. Evidence:
`/tmp/sc55-native-panel-parity.log`. The extended test also compares master pan
(8006), key shift (8005), reverb level (802d) and chorus level (8034) after
ALL-panel increments against H8: `/tmp/sc55-all-controls-native-parity.log`.
These are master/effects settings, not changes to all16 part parameters.
Effects edits request the existing audio-owned effects controller; LCD reads
the same semantic snapshot. This verifies settled settings, not sample-exact
audio or transition timing. Reset interactions, limits and the remaining
ALL-mode controls are not covered by this test.
The extended queued-button adapter test passes at44.1/48/96kHz, including
readback of all four new controls (`/tmp/sc55-all-controls-adapter.log`).
The existing Xcode Shared Code scheme also builds successfully in Release
after this integration (`/tmp/sc55-native-mute-shared.log`); no resave was run.
Standalone validation found a dangling `build/Release/SC-55.app` symlink to
an old Archive installation directory. The default build failed at MkDir,
before compilation (`/tmp/sc55-native-standalone-release.log`). The existing
outputs were preserved; validation uses the command-line override
`CONFIGURATION_BUILD_DIR=/tmp/sc55-native-preview-products` without resaving
or changing the Xcode project.
This isolated Release Standalone build succeeded, including its AUv3 dependency
(`/tmp/sc55-native-standalone-release-isolated.log`). The app is at
`/tmp/sc55-native-preview-products/SC-55.app`; signing was disabled for the
build. GUI launch and Logic-host validation are not yet performed.

The message thread renders the existing LCD glyph mask from a copied SynthState:
selected part, program/name, controls and 16 envelope-activity bars. The bars
are peak stereo envelope sums, not audio RMS. Rhythm names are currently generic.
No LCD allocation or mutex is added to audio rendering. Native 2X display merging
now sums per-part envelope activity and voice counts from both sound generators
while retaining coherent selected-part fields and instrument names. It does not
interleave instrument-name characters using MIDI-channel parity. Summing actual
activity also covers voices still sounding after a channel assignment changes.

Release --native-adapter passed at 44100/48000/96000 Hz, including repeated
panel operations, selection/routing readback and a nonempty LCD mask. The
generated 44100 Hz mask was also visually inspected. A two-instance check with
different selections/tones verifies unchanged primary text and visible secondary
voice activity. This target compiles the
actual product adapter, not the complete plug-in or a Logic host session.

Product integration was subsequently compiled with the existing Projucer-owned
Xcode project, without resaving or modifying generated files:
`xcodebuild -project Plugins/Builds/MacOSX/SC-55.xcodeproj -scheme 'SC-55 - Shared Code' -configuration Release -destination 'generic/platform=macOS' build CODE_SIGNING_ALLOWED=NO`.
Result: BUILD SUCCEEDED (log `/tmp/sc55-native-shared-release.log`). This covers
PluginProcessor/editor integration as well as the adapter, but does not establish
format-wrapper linking, installation, signing or Logic-host behavior.

## Native voice computation versus chip-slot reproduction

### Direct periodic voice-control publication

The real NativeMelodicPlayer control pass now publishes `VoiceRenderUpdate`
transactions: phase increment,three ramp commands,signed pan/send gains and
filter parameters. Its writer passes these directly to the renderer through
`PCM_ApplyVoiceUpdate`/`PCMSim_ApplyVoiceUpdate`; it no longer performs15 byte
writes for each periodic publication. The compatibility adapter mirrors the
seven words and selection/write latch for remaining transactions. Linked pitch
sources are updated directly without invalidating every waveform/filter.
The byte-writer fallback remains for oracle/reference callers.

This is used by `serviceWork` -> `serviceControl` -> `UpdateVoicePcm`, not just
an unused alternative interface. Startup,stop,PCM-boundary handling and control
readback still retain legacy operations. The overall native engine has not
become register-free yet.

`--direct-voice-updates` compares4096 varying transactions to the old byte-write
sequence: all register words,latches,linked renderer pitch,pan/sends,filter
coefficients and all envelope commands/readback match. Nonzero filter/EG
history is preserved. The standalone renderer test also uses this transaction
interface for periodic pitch updates, checking it against scalar state updates.
Actual NativeSynth output retains checksum9b31ad6ba8b312dc and exact block
partition equivalence (`/tmp/sc55-direct-voice-update-integration.log`).
This does not resolve the previously measured H8/native control-write timing
difference, and no clock period or arbitrary delay was changed here.

### Independent optional floating-point effects

`PCMEffects::process` now owns the complete float effect frame: persistent
chorus oscillator, interpolation readback, spread ramp, previous delay heads,
delay memory and coefficients. It consumes a common `EnvelopeClock`, active
state and two send inputs, then produces bus contributions. Ramp/interpolation
updates precede the delay network; read heads advance afterward. Silence does
not stop modulation. No PCM register view is needed by this operation.

The actual float PCM path calls this owner. Its adapter imports state only on
control edits (including chorus address edits and shared pitch-source writes)
and mirrors resulting readback. The former per-frame PCM modulation/ramp work
is skipped in this path, avoiding double advancement. The integer effect path
and default selection are unchanged.

`sc55-effects-test --owned` compares the complete operation, including held
clocks/inactive motion and reserved-phase/interpolation state, against the
frozen original effects, ramp and chorus implementations.65536 frames match
exactly in output, delay memory and motion/ramp state. All5 standalone CTest
tests pass in about1second in the observed Release run.

An actual H8-driven before/after run with `SC55_FXSIM=1` and
`--float-effects-audio` starts two notes, changes all8 reverb/chorus macros,
releases notes and renders tails. Both versions produce262400 frames and hash
`e873da9a8e735b83`; final phase/spread/taps are0000/2000/3ffa,3afc.
Logs: `/tmp/sc55-effects-owner-before.log`, `/tmp/sc55-effects-owner-final.log`.
The normal native sound check also retains checksum9b31ad6ba8b312dc
(`/tmp/sc55-effects-owner-native.log`). These prove preservation of the existing
float path, not equivalence between float and integer effect algorithms.

The chorus sweep calculation is now `sc55::ChorusOscillator`: position,
fractional phase, interval, rate and traversal direction, with complementary
left/right delay taps. It advances independently of signal silence. The PCM
adapter imports/exports the legacy fields, preserving the reserved phase bit;
both integer and float effects use the same independent calculation. The
effect spread ramp also uses `EnvelopeRamp`, rather than calling chip-bound
`calc_tv`. This removes the calculations' PCM dependency but does not yet make
the complete native effect controller own their persistent state directly.

`sc55-chorus-test` compares2097152 state transitions with a frozen original
address-generator implementation: interval endpoints and wraparound, random
rates/phases, ping-pong/wrapping, reverse traversal, inactive and held clocks.
Position,phase and both taps match exactly. Product audio checksum remains
9b31ad6ba8b312dc; block partitions and renderer comparisons remain unchanged
(`/tmp/sc55-effects-motion-integration.log`). No fixed note-age delays or clock
period changes were added.

The existing optional float reverb/chorus renderer no longer takes `pcm_t` or
reads chip registers. It owns a `PCMEffectsSettings` block containing decoded
input/feedback/output gains and fixed delay routing. Each sample receives the
common delay position and `PCMEffectsModulation`: moving chorus taps, fractional
read weights and reverb spread. The spread is a ramp output (old30:9), not a
fixed coefficient; chorus heads (old29:10/11,31:9/10) also remain audio-rate.
The legacy settings adapter is invalidated by effect-setting/address edits and
shared pitch-source writes; fixed coefficient decoding is not repeated every sample.

`sc55-effects-test` links no PCM/H8/JUCE implementation. It compares65536 frames
against a frozen copy of the old float renderer, changing coefficients and taps,
injecting impulses, wrapping the delay clock and changing modulation each frame.
All12 bus contributions and sampled complete delay-memory states match exactly;
reset clears delay memory. The test uses synthetic parameters, not ROM data.
The H8-driven float path also passes83 GS effect-setting transactions with
`SC55_FXSIM=1 ... --native-effects-settings`
(`/tmp/sc55-independent-effects-adapter.log`). That integration check verifies
settings operations/execution, not float-versus-integer audio fidelity.

No default renderer selection changed. The float effects algorithm already
differs from fixed-point PCM clipping/packing, and this separation does not
establish its equivalence to H8+integer PCM. Normal NativeSynth still uses the
integer effects path; directly connecting its effect controller to this owner and
validating the complete direct signal chain remain unfinished.

### Renderer-owned envelopes

`sc55_envelope_ramp.h` adds a PCM-independent ramp owner. Each voice owns three
commands/readback levels; `VoiceEnvelopes::advance` receives the same
`EnvelopeClock` for every voice and returns the new amplifier gains plus the
previous cutoff value. The two gain stages' target-crossing behavior remains
distinct. No note-on resets the common clock. The independent renderer test now
drives envelope attack/release, pitch changes, and voice gates without PCM RAM.

The legacy adapter imports commands and readback only when its existing dirty
flag says registers changed. It advances the owned ramps and mirrors readback
back to the register view. The original chip renderer still uses its original
`calc_tv` implementation, giving a separate implementation for comparison.
The first integration copied all registers and made extra passes every sample;
that measured5.129/20.262ms idle/24notes per audio second. Removing that redundant
work measured3.029/15.387ms. These are single-run diagnostic timings, not a host
performance claim or a controlled before/after benchmark.

`sc55-envelope-test` compares the independent arithmetic against a frozen
test-only copy of the original `calc_tv`: all65536 commands,14 clock phases,
seven levels,three stages,active/inactive and update/hold. All77070336 exact
gain/readback comparisons pass. The two independent CTest tests finish in under
one second in the observed Release run.
Product integration keeps checksum9b31ad6ba8b312dc and identical block partitions;
`--voice-renderer-envelope` still reports matching random clock,pitch,wave
position,EG commands and readback. Logs:
`/tmp/sc55-owned-envelopes-dirty-integration.log`,
`/tmp/sc55-owned-envelopes-dirty-readback.log`.

This moves ramp integration out of the chip-dependent path for native voices.
The higher-level envelope segment controller, its timing/handshakes and the
effects network still need migration; it does not resolve the H8/native write
timing differences above or make the entire NativeSynth PCM-independent.

The waveform/filter/mix renderer now compiles and links independently of the
PCM chip, H8, ROM loader and JUCE. `PCMSim_RenderFrame(voices, clock, buses)` consumes
waveform windows, integer phase, filter state, per-frame envelope outputs and
bus gains directly. Register-to-state conversion and key-on adoption remain
in `pcm.cpp` as the legacy adapter; they no longer live in `pcm_sim.cpp` or its
public interface. The immutable interpolation table is shared through
`pcm_interpolation.h`, not an external symbol owned by the PCM implementation.
The scalar/SIMD environment selection now occurs during `PCMSim_Init`, removing
function-local static initialization and `getenv` from the first audio frame.

An independent executable drives this same renderer with synthetic waveform
data and no firmware objects:

```
cmake -S tools/voice-renderer -B /tmp/sc55-independent-voice-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/sc55-independent-voice-build -j4
/tmp/sc55-independent-voice-build/sc55-voice-renderer
```

8192 frames exercise24 voices, pitch edits, four buses and whole/partial silent
SIMD groups. Scalar/SIMD phase and address states match exactly; bus relative
error is0.000000087. The renderer object has no unresolved PCM/H8 symbols.
The actual NativeSynth integration still produces checksum9b31ad6ba8b312dc,
with exact0/1/127/129/257-frame partition equivalence and unchanged per-program
renderer errors (`/tmp/sc55-independent-renderer-integration.log`).

This is a real rendering-module separation, not completion of the full engine
migration: NativeMelodicPlayer still feeds the PCM adapter, higher-level envelope
segment control and effects are still in that path, and H8/native control timing differences
described above remain. No performance improvement is claimed for this split.

NativeSynth can now use the existing PCM simulation's per-voice signal state
and vectorized rendering, instead of reproducing every voice slot's chip
pipeline. The C++ MIDI/control owner, common control clock, effects and IRQ
handling remain the same. Setup selects VoiceRendering::nativeVoices; the
product's explicit comparison selection is NUKED_SC55_NATIVE_PREVIEW=1 plus
SC55_SIM=1. Native preview without that explicit selection retains reference
chip rendering. Normal product startup is still H8.

PCMSim_Init's shared interpolation/envelope tables now initialize exactly once
with std::call_once, during setup only. Previously a plain shared boolean
allowed concurrent instances to write tables while another instance rendered.

Release diagnostic --native-synth-fast (one observed run):

| Workload | Native control + native voices | H8 control + native voices |
|---|---:|---:|
| Idle | 1.969 ms | 17.007 ms |
| 24 Note On messages | 10.728 ms | 49.115 ms |

Times are wall-clock milliseconds per audio second, excluding setup. This is
not a Logic CPU measurement. Both sides use the same voice renderer. Reference
chip native-control rendering previously cost approximately 26/29 ms.

The voice-rendering comparison feeds identical MIDI to two fresh NativeSynth
instances, one using reference chip slots and one using native voice math.
There is no lag fit, gain fit or time stretch. Measured relative RMS error and
output/reference RMS ratio were:

| Program (zero based) | Relative RMS error | Level ratio |
|---|---:|---:|
| 0 | 0.001746 | 1.001131 |
| 48 | 0.696569 | 1.009872 |
| 80 | 0.046854 | 1.023609 |
| 120 | 0.002512 | 1.001373 |

These are pre-fix waveform differences, not perceptual ratings. Program48's
large difference was subsequently fixed below; the high-speed renderer is
not yet promoted to the default. Both renderers preserve block partitioning in the
tested note-on/off sequence, but that does not resolve their mutual difference.

## Shared random clock correction

The simulation skipped DAC dithering and inadvertently skipped the update of
PCM channel30's LFSR too. The modulation controller reads that same state. In
the string fixture, the first pitch command diverged 3808 frames after MIDI,
then waveform phase at3809 and sample address at3958. This was a control input
bug, not an intrinsic failure of vectorized waveform rendering.

The LFSR now advances independently of voice-renderer selection, preserving
the configured one/two DAC-clock advances per PCM pass. Dither remains absent
from simulation output, while modulation receives the same moving state.
This also corrects H8+simulation, which previously read the frozen generator.

Regression --voice-renderer-clock initially failed with differing LFSR state.
After the fix it verifies shared RNG, pitch command, waveform address and phase
for a sounding string voice, including an oversampling configuration change.
Temporary first-divergence logs were removed; the retained source is
cpu-roles/voice-renderer-clock.h. --native-synth-fast also asserts a1% relative
waveform error gate for the string regression, through the sound generator's
MIDI/render interface.

The original comparison now gives program48 relative RMS error0.003536 and
level ratio1.001014 (previously0.696569/1.009872). Program80 remains0.046854;
its difference is not explained away by this fix. Last observed native timing:
idle2.011ms, 24 notes10.651ms per audio second. EG approximations and remaining
product integration still require work; neither the migration nor whole-bank
waveform compatibility is declared complete.
