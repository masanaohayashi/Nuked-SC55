# Nuked SC-55 — JUCE plugin fork

A fork of [nukeykt/Nuked-SC55](https://github.com/nukeykt/Nuked-SC55) that wraps the
emulator as an audio plugin, plus tooling for playing Standard MIDI Files through it.

**What this fork adds**

- **Plugin builds**: Standalone, AU, AUv3 and VST3 (macOS), in `Plugins/`.
- **Drop a `.mid` on the window and it plays.** The file is fed to the emulator in
  file order, so nothing sorts or de-duplicates same-tick controllers on the way in.
  That matters: RPN and NRPN are a state machine (a select, then a data entry), and a
  sequencer that reorders them silently loses settings such as pitch bend range.
- **`tools/sc55render`**: renders a `.mid` to WAV offline, song plus one bar, at
  roughly 5x real time.
- SDL is no longer required for the plugin build, and a number of fixes to the
  emulator core that only surface when it is initialised more than once per process.

Every difference from upstream, and why, is recorded in **[DEVIATIONS.md](DEVIATIONS.md)**
(written in Japanese).

**Building**

```
git clone --recurse-submodules https://github.com/masanaohayashi/Nuked-SC55.git
cd Nuked-SC55/Plugins
./build.sh              # builds the Projucer from the JUCE submodule, then the plugin
```

JUCE 9.0.0 is pinned as a submodule at `3rdparty/JUCE`. JUCE 9 modules can only be
saved by a matching Projucer, which is why `build.sh` builds it first.

Signing is ad-hoc by default, which is all a local build needs - including running the
AUv3 in a host. To sign with your own team for distribution, pass it in rather than
putting it in the project:

```
SC55_TEAM_ID=XXXXXXXXXX ./build.sh
```

**ROMs are not included and never will be.** Supply your own SC-55 ROM set; the
standalone app asks for the folder on first run.

**Licence**

Unchanged from upstream: the modified MAME licence in [LICENSE](LICENSE).
Redistributions **may not be sold, nor used in a commercial product or activity**.
This applies to everything here, including the plugin. Note also that JUCE itself is
AGPLv3 or commercial - see the JUCE licence for what that means for anything you
distribute.

---

# Nuked SC-55 (upstream README)
Roland SC-55 emulator, by nukeykt.

Supported models:
- SC-55mk2/SC-155mk2 (v1.01 firmware is confirmed to work)
- SC-55mk1 (v1.0/v1.21/v2.0 firmwares are confirmed to work)
- CM-300/SCC-1 (v1.10/v1.20 firmwares are confirmed to work)
- SCC-1A
- SC-55st (v1.01)
- JV-880 (v1.0.0/v1.0.1)
- SCB-55/RLP-3194
- RLP-3237
- SC-155

Special thanks:
- John McMaster: SC-55 PCM chip decap.
- org/ogamespec: deroute tool.
- SDL team.
- Wohlstand: linux/macos port.
- mattw.
- HardWareMan.
- giulioz: JV-880 support
- Cloudschatze.
- NikitaLita.
- Karmeck.

## License

Nuked SC-55 can be distributed and used under the original MAME license (see LICENSE file).
Non-commercial license was chosen to prevent making and selling SC-55 emulation boxes using (or around) this code, as well as preventing from using it in the commercial music production.

## Status

3 ICs are needed to be emulated:
- Roland PCM chip
- Hitachi H8/532 MCU
- Mitsubishi M37450M2 MCU

MCUs are emulated using info from their datasheets. PCM chip info is derived from analyzing decapped chip.

### PCM chip tracing progress

![pcm_tracing.jpg](pcm_tracing.jpg)

### MCU emulation progress

![mcu1.png](mcu1.png)

![mcu2.png](mcu2.png)


## Install

Put firmware image into the same folder as Nuked SC-55. Files should be names as such:

```
SC-55mk2/SC-155mk2 (v1.01):
R15199858 (H8/532 mcu) -> rom1.bin
R00233567 (H8/532 extra code) -> rom2.bin
R15199880 (M37450M2 mcu) -> rom_sm.bin
R15209359 (WAVE 16M) -> waverom1.bin
R15279813 (WAVE 8M) -> waverom2.bin

SC-55st (v1.01):
R15199858 (H8/532 mcu) -> rom1.bin
R00561413 (H8/532 extra code) -> rom2_st.bin
R15199880 (M37450M2 mcu) -> rom_sm.bin
R15209359 (WAVE 16M) -> waverom1.bin
R15279813 (WAVE 8M) -> waverom2.bin

SC-55 (v1.00):
R15199748 (H8/532 mcu) -> sc55_rom1.bin
R1544925800 (H8/532 extra code) -> sc55_rom2.bin
R15209276 (WAVE A) -> sc55_waverom1.bin
R15209277 (WAVE B) -> sc55_waverom2.bin
R15209281 (WAVE C) -> sc55_waverom3.bin

SC-55 (v1.21):
R15199778 (H8/532 mcu) -> sc55_rom1.bin
R15209363 (H8/532 extra code) -> sc55_rom2.bin
R15209276 (WAVE A) -> sc55_waverom1.bin
R15209277 (WAVE B) -> sc55_waverom2.bin
R15209281 (WAVE C) -> sc55_waverom3.bin

SC-55 (v2.0):
R15199799 (H8/532 mcu) -> sc55_rom1.bin
R15209387 (H8/532 extra code) -> sc55_rom2.bin
R15209276 (WAVE A) -> sc55_waverom1.bin
R15209277 (WAVE B) -> sc55_waverom2.bin
R15209281 (WAVE C) -> sc55_waverom3.bin

CM-300/SCC-1 (v1.10):
R15199774 (H8/532 mcu) -> cm300_rom1.bin
R15279809 (H8/532 extra code) -> cm300_rom2.bin
R15279806 (WAVE A) -> cm300_waverom1.bin
R15279807 (WAVE B) -> cm300_waverom2.bin
R15279808 (WAVE C) -> cm300_waverom3.bin

CM-300/SCC-1 (v1.20):
R15199774 (H8/532 mcu) -> cm300_rom1.bin
R15279812 (H8/532 extra code) -> cm300_rom2.bin
R15279806 (WAVE A) -> cm300_waverom1.bin
R15279807 (WAVE B) -> cm300_waverom2.bin
R15279808 (WAVE C) -> cm300_waverom3.bin

SCC-1A:
R00128523 (H8/532 mcu) -> cm300_rom1.bin
R00128567 (H8/532 extra code) -> cm300_rom2.bin
R15279806 (WAVE A) -> cm300_waverom1.bin
R15279807 (WAVE B) -> cm300_waverom2.bin
R15279808 (WAVE C) -> cm300_waverom3.bin

JV-880 (v1.0.0):
R15199810 (H8/532 mcu) -> jv880_rom1.bin
R15209386 (H8/532 extra code) -> jv880_rom2.bin
R15209312 (WAVE A) -> jv880_waverom1.bin
R15209313 (WAVE B) -> jv880_waverom2.bin
PCM Cards -> jv880_waverom_pcmcard.bin (optional)
Expansion PCBs -> jv880_waverom_expansion.bin (optional)

SCB-55/RLP-3194:
R15199827 (H8/532 mcu) -> scb55_rom1.bin
R15279828 (H8/532 extra code) -> scb55_rom2.bin
R15209359 (WAVE 16M) -> scb55_waverom1.bin
R15279813 (WAVE 8M) -> scb55_waverom2.bin

RLP-3237:
R15199827 (H8/532 mcu) -> rlp3237_rom1.bin
R15209486 (H8/532 extra code) -> rlp3237_rom2.bin
R15279824 (WAVE 16M) -> rlp3237_waverom1.bin

SC-155 (rev 1):
R15199799 (H8/532 mcu) -> sc155_rom1.bin
R15209361 (H8/532 extra code) -> sc155_rom2.bin
R15209276 (WAVE A) -> sc155_waverom1.bin
R15209277 (WAVE B) -> sc155_waverom2.bin
R15209281 (WAVE C) -> sc155_waverom3.bin

SC-155 (rev 2):
R15199799 (H8/532 mcu) -> sc155_rom1.bin
R15209400 (H8/532 extra code) -> sc155_rom2.bin
R15209276 (WAVE A) -> sc155_waverom1.bin
R15209277 (WAVE B) -> sc155_waverom2.bin
R15209281 (WAVE C) -> sc155_waverom3.bin

```

## Additional info

- Nuked SC-55 will listen to the specified MIDI IN port (default is port 0). Use `-p:<port_number>` command line argument to specify port number. To use it with the other applications use external MIDI pipe software (e.g. loopMIDI).

- SC-55mk2/SC-55mk1 buttons are mapped as such (currently hardcoded):

```
Q -> POWER
W -> INST ALL
E -> INST MUTE
R -> PART L
T -> PART R
Y -> INST L
U -> INST R
I -> KEY SHIFT L
O -> KEY SHIFT R
P -> LEVEL L
LEFTBRACKET  -> LEVEL R
A -> MIDI CH L
S -> MIDI CH R
D -> PAN L
F -> PAN R
G -> REVERB L
H -> REVERB R
J -> CHORUS L
K -> CHORUS R
LEFT -> PART L
RIGHT -> PART R
```

- JV-880 buttons are mapped as such (currently hardcoded):

```
P -> PREVIEW
LEFT -> CURSOR L
RIGHT -> CURSOR R
TAB -> DATA
Q -> TONE_SELECT
A -> PATCH_PERFORM
W -> EDIT
E -> SYSTEM
R -> RHYTHM
T -> UTILITY
S -> MUTE
D -> MONITOR
F -> COMPARE
G -> ENTER
COMMA -> ENCODER L
PERIOD -> ENCODER R
```

- `-mk2`, `-st`, `-mk1`, `-cm300`, `-jv880`, `-scb55`, `-rlp3237`, `-sc155` and `-sc155mk2` command line arguments can be used to specify rom set. If no model is specified emulator will try to autodetect rom set (based on file names). 

- Due to a bug in the SC-55mk2's firmware, some parameters don't reset properly on startup. Do GM, GS or MT-32 reset using buttons to fix this issue.

- SC-155 doesn't reset properly on startup (firmware bug?), use `Init All` option to workaround this issue.

- SC-155's additional buttons/sliders are not emulated at the moment. They will be emulated in the future after proper GUI is implemented.
