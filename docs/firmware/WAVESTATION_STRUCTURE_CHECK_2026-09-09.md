# Reference structure checked against current source

## Current acceptance scope (2026-09-10, user clarification)

The goal is complete semantic replacement of H8 CONTROL, using the existing
PCM DSP. A PCM-free synth or promotion of `independentSignal` is NOT a completion
requirement. Earlier migration notes that imply otherwise are superseded here.
Optional renderer experiments remain optional; do not delete them or switch
the product default as a side effect of this clarification.

Reinspection of `CWsTGEngine.cpp` confirms named per-voice state and separate
`UpdateEnvelope`, `UpdateVoice`, and `TG` operations. The first two actually use
per-voice counters (`14*31` and `42*31` respectively). These are Wavestation's
semantics, NOT timing constants or scheduling rules to copy into SC-55.
The reference supplies a structural example; SC-55 H8/PCM execution supplies
behavioral evidence.

Required end state:

Latest user priority (2026-09-10): defer hidden features and extended LCD menus;
sound-engine control comes first. Deferred UI is not claimed implemented.
Commit each completed work unit; push only when separately requested.

User thread-ownership constraint: panel input, interaction/selection logic and
display presentation belong to the message thread, never processBlock. Audio
applies resolved sound commands and publishes immutable sound/event state.
SOLO's target and sound parameters are audio state, not reads of UI widgets.

- NativeSynth owns MIDI/control/configuration state and exposes a small MIDI,
  render and state interface. Its normal path executes no H8 instructions,
  program counter, stack or emulated kernel tasks.
- H8 responsibilities are implemented as connected C++ behavior: reception,
  routing, configuration/reset, melodic/rhythm/mono admission, allocation and
  stealing, EG/LFO/pitch/filter/level updates, effects control, device events,
  panel commands, display data and incoming firmware configuration protocols.
  Unknown/reachable firmware branches are not declared complete by omission.

User scope exclusion: MIDI output (SysEx replies/settings dumps and their UI
connection) is not required at present. Keep incoming SysEx configuration and
reset handling. Existing optional reply diagnostics may remain, but do not
implement host MIDI output or make output-only paths a completion gate.
- The existing PCM engine continues oscillator/filter/mixing/ramp DSP. A PCM
  interface and references to PCM-owned state are not by themselves failures.
  Startup/reuse readiness and common PCM clock semantics must be preserved.
- The product remains native by default without environment variables; H8 is
  an explicit reference option. No arbitrary waits, removed failing assertions
  or changes to the H8 reference are allowed to manufacture parity.
- Compatibility must be demonstrated through actual MIDI/control/render paths,
  including the user-confirmed mono/drum fixes. Allocation/reserve rules and
  event ordering must hold. A different capacity survivor is diagnostic evidence,
  not by itself proof of a functional bug or a requirement for sample parity.

Latest user clarification: reproducing the H8 emulator's instruction durations
or matching its PCM-write sample is not the goal. Run semantic updates on the
common control cadence, retaining actual PCM activation/reuse handshakes and
causally required ordering. Investigate fine timing only when it explains a
functional failure (missing note, attack loss, reserve violation, etc.). Do not
weaken or delete differential diagnostics to claim parity; label their scope.

The next work is missing or mismatching H8 behavior, not further independent
PCM cleanup. In particular, see the current capacity/scheduling investigation
and the reception/bulk/RQ1 coverage in CPU_SERVICES_AND_ORDER.

Inspected local `/Users/ring2/Documents/src/KLC/WAVESTATION/common` sources,
not a guessed interpretation of the requested end state:

- `CWsTGEngine.cpp:111`: `UpdateEnvelope` owns envelope updates.
- `CWsTGEngine.cpp:482`: `UpdateVoice` owns control/modulation updates.
- `CWsTGEngine.cpp:1227`: `TG` owns signal generation; its voice loop skips
  inactive voices before interpolation/filter work (around1248).
- `CWsTG.cpp:1627,1756`: host Process/ProcessReplacing entry points.
- `CWsTGVAlloc.cpp`: separate voice-allocation implementation.

The SC-55 C++ controller now owns MIDI/part/allocation/EG control, and the product
selects it by default. NativeSynth exposes MIDI ingestion and frame rendering;
no H8 instruction execution is required on that path. The inactive reference-PCM
optimization is aligned with the reference's active-voice traversal, with extra
SC-55 state preservation established by differential tests.

Independent link evidence (2026-09-10): `tools/native-engine` builds the existing
NativeSynth/control headers with PCM, patch decoder and SHA, without Emulator,
MCU, sub-MCU, LCD, JUCE or host adapter sources. Its setup-time ROM-loader
consumer built and ran on macOS arm64, producing the normal product fixture's
checksum `3b54320560580fd3` from MIDI Note On/Off and zero/257-frame render calls.
The executable links only libc++/libSystem dynamically. This verifies core
independence, not complete firmware behavior or other platforms/host formats.

Historical observation, not a requirement to replace PCM: NativeMelodicPlayer
still owns a pcm_t reference, directly reads cycles/key mask/envelope RAM/IRQ,
and calls PCM_Update. Lifecycle helpers still expose raw control-word caches and
byte-register fallbacks. Independent PCMSimVoices/VoiceEnvelopes/PCMEffects
implementations exist but are not a complete independent default product path.

Migration acceptance requires a controller that owns semantic control state and
schedules semantic operations, not merely renamed H8 register transactions.
PCM-owned signal state may remain in the existing PCM engine. Current audio
differences, missing protocol paths and UI functionality remain correctness work.

Do not equate green narrow tests, fewer PCM_Write calls, a new facade, or measured
CPU reduction with completion of the requested architecture.
