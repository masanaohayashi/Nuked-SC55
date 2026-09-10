# Missing mono partial replenishment

## Evidence

Full55KTIZKE.RCP playback, all initialization/parts retained,48 physical voices:
channel12 is mono. At155.365844 and155.853656 seconds (and the corresponding
163-second phrase), only one partial remained. The native destination plan
selected the same slot for both new partials. PCM output was not strictly zero:
the weaker component overwrote the main sound, explaining apparent skipped notes.

The minimized fixture retains prior SysEx/channel12 configuration and the four
notes at155..156 seconds. Pre-fix, the third note selected slots44,44 and failed
`Mono restart lost a partial despite available voices`. Its post50ms PCM peak
was171275; after replenishment it was632060, with two occupied/sounding slots.
These are diagnostic integer mix values, not normalized output or loudness.

The unchanged H8 prepared22/23,21/22,20/21,19/20 for the same four-note fixture.
ROM execution and the listing identify the omitted path:

- 0ee9..0f26: no held keys, one surviving slot, two requested partials ->186d.
- 0f8c..0f9d: the corresponding non-portamento check also tests held keys.
- 186d: request capacity for one extra physical voice.
- 188b..18d7: retain the group, update key/status/minimum, allocate and attach a
  new tail, move the surviving slot to the second destination, increment count.

This is independent of the earlier stale completion mailbox fix (`be18fae`).

## Implementation boundary

The voice engine applies replenishment only to a new non-legato mono admission.
It uses existing capacity/retirement policy and retries after capacity work;
no extra voice is taken for legato or held-key returns. The admission records
its newly attached slot across preparation waits. The runtime accepts a missing
prior DSP owner only for that explicitly added slot and initializes it through
the ordinary installation/startup path. The allocator transaction preserves the
existing group and firmware destination order.

All work remains in the audio-owned voice engine. No UI, allocation, logging,
PCM-renderer change or H8 production-path change is introduced. Existing user
Projucer/generated-project edits are not part of this fix.

## Regression checks

`tools/native-engine/README.md` documents short/native/full-song and H8 commands.
Allocator coverage tests replenishment through24..128 capacities, distinct slot
ownership, free/part counts, updated key, and rejection of already-full groups.
The short native fixture is registered as `native-mono-replenishment` when the
private song/ROM paths are supplied. ROM/MIDI contents are not committed.

Validation: the full48-voice replay passed all eight observations and the
orphan check through254.146 seconds. The short excerpt also passed at24 and128
voices. CTest passed voice-capacity, native-without-h8, native-mono-completion
and native-mono-replenishment. The H8 check observed four distinct-slot starts
and three calls to186d. No Logic listening test was performed by the agent.
