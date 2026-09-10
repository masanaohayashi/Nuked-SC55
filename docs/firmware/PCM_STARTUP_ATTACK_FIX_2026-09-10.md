# Native startup / periodic EG race

## Reproduction

Replay all tracks of55KTIZKE from the beginning. The first kick is not a
reliable failure. In a deterministic60-second replay, kicks starting at native
times53.200000 and54.420500 reproduce reduced attacks. The first kick and the
other11 kick attacks are normal. All use wave start6eca0.

At age32 native32-kHz frames, the product of the two PCM gain readbacks is:

| Kick | H8 | Before fix | After fix |
| --- | ---: | ---: | ---: |
| 53.2s | 58392576 | 2813126 | 58392576 |
| 54.42s | 286654464 | 13238240 | 286654464 |

The wave starts and positions match H8; this is not a sample-offset problem.

## Mechanism

The integer PCM loop computes `kon=key&&!okey` and `active=key&&okey`.
The first pass latches the key and clears inactive histories/gain readbacks.
The next pass consumes the initial gain commands as an active voice.

Native startup previously completed as soon as mode bit5 latched. If a control
tick was pending at that instant, periodic EG control replaced the initial
`6cba,21ba` commands with `6cb4,2151` before they were consumed. Instead of the
H8 immediate rise, both gains ramped slowly from zero. The changing scheduling
phase explains intermittent behavior. Voice reuse/load can change that phase;
stale initial readback by itself was not the cause (it clears on the first pass).

`PCM_InstallVoice` now records its PCM cycle. `PCM_CompleteVoiceEnable` requires
the key latch plus two completed625-cycle PCM passes before releasing startup
to periodic control. This does not defer key-on, change MIDI timestamps, alter
ROM envelope values, force voice levels, or add a fitted per-instrument delay.
It retains the initial commands until the PCM has consumed them. Both old and
new gain histories can therefore follow the PCM's own reset behavior.

## Regression command

```
SC55_KICK_ALL=1 sc55-cpu-roles <v1.21-ROM-dir> --song-first-kick <55KTIZKE.MID>
```

This replays the first60 seconds with no track filtering or muting and asserts
matching onset gain for all13 kick attacks against H8. It does not assert whole
song audio identity or equal firmware dispatch latency. Optional diagnostic
`SC55_KICK_IDLE_FRAMES` varies the idle phase before Play; it is not a product
option. Without `SC55_KICK_ALL` the probe stops after the first kick.

The existing GATCHA55 part16/mute regression remains separate. ROMs and song
files remain external. Product changes introduce no allocation/logging in the
audio callback and no generated project edits.
