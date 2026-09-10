# Mono Note On attack loss

## Reproduction and cause

GATCHA55.MID part16, eighth Note On: 17.518091 s, key74, velocity100.
Replay all MIDI normally; after initial setup (3 s), use the firmware panel
MUTE/PART buttons to mute parts1..15. Native playback receives the corresponding
semantic panel commands. Both engines' mute flags are checked. No MIDI tracks
are removed. The comparison uses the product-default integer PCM renderer.

Before the fix the native attack started at `cd960` and the product of its
two PCM gain readbacks stayed zero over the first128 samples. H8 started at
`cd940`, with peak gain product99090432. This also occurred without muting.

Native `startMonoOrSource` used `prepareGroupReuse` (H8 0b1a..0b64, the
Note Off held-key replacement path) for new Note On too. This selected continuing
envelopes based on allocator status. The actual new Note On path is 0e8b..0f32:

- Test previously held keys *before* adding the incoming key (added at0e5f).
- No held keys: restart envelopes even if the mono group still has a release tail.
- Legato: the live C8B3 high bit selects restart; do not substitute allocator status.
- Note Off replacement retains its separate0b1a rules.

The fix separates those decisions and publishes the new held key only after
admission finishes (including valid rejected admissions), so deferred preparation
does not mistake its own incoming key for a previously held key.

## Validation

Diagnostic target: `tools/firmware-oracle/cpu-roles`.

```
sc55-cpu-roles <v1.21-ROM-directory> --song-part16 <GATCHA55.MID>
sc55-cpu-roles <v1.21-ROM-directory> --song-allocation <MIDI-file>
```

The first command replays18 seconds and asserts the eighth note's sample start
and nonzero first128-sample peak gain against H8 after the panel-mute sequence.
After the fix both start at `cd940` with peak gain product99090432. Their key-on
times/pitch evolution are not asserted equal. It is a GATCHA55-specific regression,
not a generic arbitrary-song test.

The second command observes the first60 seconds. GATCHA55 and55KTIZKE both
complete with an empty native MIDI queue. Allocation timing/count differences
remain; this check does not establish full-song audio equivalence. Silent attack
windows alone are not failures (delayed envelopes and terminated notes can be
silent). This fix establishes the reported part16 eighth-note regression, not
that every earlier report of drum/stealing differences is resolved.

No ROM or MIDI assets are copied into the repository.
