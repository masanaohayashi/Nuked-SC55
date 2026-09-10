# GS rhythm-mode changes: configuration versus program selection

The normal native MIDI path now preserves the distinction between assigning a
part to a shared drum map and issuing a Program Change. Neither operation is a
generic reset of the part. Existing PCM rendering is unchanged.

## Firmware evidence

The actual GS handler at04:1553..15c6 changes routing, resets the bank/program
display latches and selects the map's shared program number.04:15c2 clears the
part's rejected-tone latch atAB06 unconditionally. It does not write the RPN
coarse-tuning byte atAB46+part and does not reload the editable drum record.

This differs from Program Change04:0937..09ac:

- An unassigned program>=64 rejects that receiver and retains the existing map.
- A valid program reloads the shared map and updates peer program/display
  latches in04:0a81..0ab7, but does not clear peers' rejected-tone latches.
- Assigning a GS rhythm mode, including the same mode, clears the addressed
  receiver's rejection even if the shared program latch is still invalid.

## Reproduced native omissions

`--native-shared-rhythm` sends actual MIDI to the H8 interpreter and native
controller, without writing the reference's RAM or replaying recorded timing.
Two independently routed parts share each of the two maps.

1. Part1 receives rejected PC65. Part2 is assigned that same map again, then
   receives NoteOn57. Before the fix H8 allocated key57 but native had no group.
   `changeGsMode` incorrectly recomputed rejection from the shared program.
   It now clears rejection as the GS operation requires.
2. RPN2 sets part2 coarse tuning to+12 before assigning the map. Before the fix
   native returned0 while H8 retained12. Removed the unrelated tuning reset.

The test also compares group state after peer Program Changes and after leaving
and rejoining a map, then checks recovery through the receiver's own valid PC.
Both complete editable map records are compared at each mode assignment,
including a GS pitch edit followed by reassigning the same mode. This protects
the distinction between map membership and map reinitialization.

Follow-through coverage also starts a sustained melodic voice, then changes the
part through map1, map2 and melodic mode while issuing new notes and Note Offs.
It compares groups at every change and after final release, with no pedal, hold
and sostenuto. This exercises old and new admission identities sharing one part;
it does not infer correctness from configuration readback alone.

## Validation scope

Release headless target `sc55-cpu-roles` builds, including the product emulator
translation unit. The new mode passes42 semantic group comparisons across both
maps, plus routing/program/tuning and complete-map comparisons. This is not
sample-exact H8 audio parity or an Xcode/Logic host test.

Reproduction (substitute the local ROM directory):

```sh
/tmp/sc55-cpu-roles-build/sc55-cpu-roles ROM_DIRECTORY --native-shared-rhythm
```

Failure logs: `/tmp/sc55-shared-rhythm.log` and
`/tmp/sc55-shared-rhythm-tuning.log`. Passing result:
`/tmp/sc55-shared-rhythm-final.log` (initial12 comparisons) and
`/tmp/sc55-mode-live.log` (42 comparisons including sounding mode transitions).
