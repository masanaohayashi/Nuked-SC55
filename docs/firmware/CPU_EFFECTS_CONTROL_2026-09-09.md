# SC-55 v1.21: H8のエフェクト設定責務

## CPUとPCMの分担

CPUは設定要求を受け、変更種別を判別し、フェード・待機・係数/タップ設定を周期的に進める。
PCMは設定された係数とタップで毎サンプル演算する。H8の命令を外しても、
**音に影響する設定遷移とPCM側のエフェクト処理は残す必要がある。**

根拠: ROM00:5946〜674c、PCM_Write、pcm.cpp、pcm_effects.h、
`cpu-roles --effects`（H8最適化なし、割り込み補完なし）。製品変更はしていない。

## 要求の受理とキャッシュ

task 8 event0は5a2a、event1は5954。ここでは設定を比較して要求状態を作る。
周期event7の5af1で経過回数を回収し、604a（reverb）→6412（chorus）→ボイス更新。
従ってMIDI/SysExを受信した瞬間に設定がすべてPCMへ反映されるわけではない。

| 設定RAM | CPU内の作業値 | 作用 |
|---|---|---|
| 802b〜802f | cb51〜cb55 | reverb character、pre-LPF、level、time、delay feedback |
| 8030 | cb56（値>>1） | reverb pre-delay |
| 8033〜8034 | cb57〜cb58 | chorus pre-LPF、level |
| 8035 | cb59（値>>1） | chorus feedback |
| 8036〜8038 | cb5a〜cb5c | chorus delay、rate、depth |
| 8039 | cb5d（値>>1） | chorusからreverbへのsend |

reverbはcharacter変更、または進行中の一部条件で全切替を選ぶ。
同じcharacterでidleならcb70の変更bitを設定する差分更新（state12）。
time変更もcharacter>=6の場合には全切替へ進む。
chorusはdelay/rate/depth変更、または対応外の進行stateなら全切替。
idle/state10では他パラメータをcb71の変更bitで処理する。
新要求を単純に無視する/キューに全部積むモデルではない。

## reverb状態機械

cb6cは関数表6054のbyte offset。数値は連番の段階番号ではない。

| cb6c | 入口 | 仕事・次状態 |
|---:|---|---|
| 0 | 6411 | idle、周期本体から呼ばない |
| 2 | 6062 | cb5eの各byteを3ずつ0へ下げ、PCM bank30へ出力。0で4へ |
| 4 | 60a1 | bank28/29/30の設定を初期化、cb60=0、cb72=33、6へ |
| 6 | 6115 | cb72を1ずつ減らす。0で8へ |
| 8 | 6129 | ROM表からタップ/係数を設定し、LPF targetを用意。10へ |
| 10 | 625f | cb60のlow byteを2ずつtarget cb62へ上げ、終了で0へ |
| 12 | 628d | cb70の変更bitごとにLPF/level/time/feedback等を更新、全完了で0へ |

LPF変更は現在のlow byteを下げてからROM04:0088のtargetへ切替え、再び上げる。
level等の追従も増加2/減少3でtargetへclampする。全切替のROM設定表は04:0078。
33回待つ区間はtask 8周期呼出し回数で進む。ホストスレッドをsleepする処理ではない。

## chorus状態機械

cb6eは表641cのbyte offset。

| cb6e | 入口 | 仕事・次状態 |
|---:|---|---|
| 0 | 674c | idle |
| 2 | 6428 | cb64/65/66を3ずつ下げる。0でLPFを消し、cb73=33、4へ |
| 4 | 649f | cb73を1ずつ減らす。0で6へ |
| 6 | 64b3 | PCMのアドレス/変調設定とreadback同期、出力値とLPF targetを準備。8へ |
| 8 | 65b0 | cb68のlow byteを2ずつcb6aへ上げる。完了で0へ |
| 10 | 65de | cb71の差分要求を処理。全完了で0へ |

64dd以降にはdelay*6+3801、depth*10+10を使うアドレス計算、
depth*10*rate/8の計算がある。PCMのreadbackを待ちながら設定し、
単なるdelay bufferへの固定係数コピーではない。LPF表はROM04:0098。

## PCM側の受取先

bank28/29は主にdelay tap、bank30はreverb係数、bank31はchorus係数/変調状態。
pcm_effects.hはこれらを読み、16,384wordのdelayと作業値を保持する。
声ごとのsendはpcm_sim.cppがram2[slot][2]から読む。global FX設定と別の入力。

PCM側もbank29/31の変調用値を時間とともに更新する。
**2時点のram2差分をすべて「今回のSysExによるCPU書込」と解釈してはいけない。**
設定所有値とPCMの進行状態を分けてnativeインターフェースへ渡す必要がある。

## 実測

fresh boot後、下記4設定を順次送り各20M emulator cycles進めた。
各区間末尾のcb6c/cb6e=0をassertし成功。ログ `/tmp/sc55-cpu-effects.log`。

| 入力 | 観測したstateと呼出し回数 | 設定結果の例 |
|---|---|---|
| reverb level 40 01 33=17 | 12×16 | bank30[2]/[3]:4000→1100 |
| reverb character 40 01 31=6 | 2×6→4×1→6×33→8×1→10×33 | bank28/29のタップ・bank30係数変更 |
| chorus level 40 01 3a=17 | 10×16 | bank31[2]/[5]:4000→1100 |
| chorus delay 40 01 3c=27 | 2×6→4×33→6×1→8×33 | chorus設定の再構築 |

bootでも両方の全切替経路と33回待機を観測した。
すべてのcharacter・同時変更・処理中再要求の動的網羅、音声一致/CPU負荷の試験ではない。
係数演算のnative実装が完成したという報告でもない。

## 置換時に保つ契約

- 設定要求の反映はaudio-owned control stateで行う。UI timerへ移動しない。
- event受理と周期進行を分け、reverb→chorus→voiceの制御順序を保つ。
- dirty flagによる合流、全切替/差分更新の選択、フェードと待機を保つ。
- PCM同期はboundedな進行状態へ変換する。audio callback内の無期限busy waitにしない。
- ROMの係数/タップ表は既存の初回ROM取込の延長で取得し、H8の実行を前提にしない。

## Native implementation: request coalescing and drain

`src/backend/sc55_effects_control.h` now owns semantic reverb/chorus request
state. Raw configuration values are normalized once (pre-delay, feedback,
chorus-to-reverb send divide by two). Reverb requests during *any* non-idle
phase restart full setup; chorus permits differential phase10 coalescing.
Dirty bits and LPF transition bits preserve the firmware's ordering, including
time changes on reverb characters6/7 that choose full setup after accumulating
earlier differential changes.

`advanceDrain` implements reverb phases2/4/6 and chorus phases2/4: decrement
output components by3 with saturation, clear/setup the initial PCM registers,
and advance a33-call drain counter. Each call is bounded; PCM time must advance
between calls. The word writer is supplied by the serialized audio owner.
Returning false means idle or a phase outside this method, not completion.

Validation: `sc55-cpu-roles <v1.21 ROM directory> --native-effects-requests`
PASS, `/tmp/sc55-effects-requests.log`. The probe observes actual firmware
entry/return states during boot, settled GS changes and overlapping GS changes;
it does not replace CPU instructions or inject interrupts. Native predictions
match32 request completions (reverb full/diff9/5, chorus12/6) and497 periodic
drain completions. Drain checks cover control-state results and written
CPU-owned PCM coefficients. Mutable delay-address bank28/29 values are excluded
from post-step RAM comparison because PCM advances them independently.

This module is not yet connected to the native player: reverb phases8/10/12,
chorus6/8/10, coefficient-table import, GS mapping and reset/clock integration
remain. The player still disables effects; no full effect audio equivalence or
CPU improvement is claimed. Product project files were not regenerated.

## Native chorus setup and coefficient updates

Chorus phases6/8/10 are now implemented in `EffectsControl::Chorus`.
`advanceSetup` writes the delay span and modulation rate derived from delay,
depth and rate, preserves the three separate PCM readback gates, then installs
output levels and the LPF target. A mismatch returns to the audio owner with
explicit mask/position/modulation state; the owner must advance PCM and retry,
serializing later requests until setup completes. No busy loop is used.

`advanceCoefficients` handles full-setup LPF fade-in and differential LPF,
level, feedback and reverb-send updates. The LPF register is written before
advancing its internal value. Full-setup ascent is2, differential ascent3;
downward exact level hits clear their dirty bit on the next call, while upward
hits clear immediately. These ordering details were retained from the ROM.

`ImportEffectsTables` extracts the two eight-word LPF tables04:0088/0098 from
the exact SHA256-gated user ROMs at setup time; no recovered coefficient data
is embedded in source and no cache format is changed.

Validation PASS (`/tmp/sc55-effects-coeff.log`): previous32 request and497 drain
checks, plus254 chorus coefficient calls and6 completed chorus setup calls
agree with firmware control state and checked PCM coefficients. The setup
oracle predicts the ready-readback case; it does not claim cycle-exact native
timing or compare independently moving delay addresses. A separate native
fixture forces each readback gate to lag, checks bounded calls, address
calculations and withholding output until the last gate completes.

Remaining: reverb coefficient/setup phases8/10/12, full table import, GS/clock
and reset integration, whole-stream audio equivalence. Chorus is implemented
as a module but not enabled in the player yet; the ordinary product still
executes H8. No host build/install, resave, commit or push in this step.

## Native reverb setup and coefficient updates

Reverb phases8/10/12 now complete the module's periodic state branches.
`ImportEffectsTables` follows all eight pointers at04:0078 and imports each
26-word program (12 bank28 taps,9 bank29 taps,5 bank30 coefficients), alongside
the LPF tables. Import is still exact-ROM gated and setup-only.

`advanceSetup` installs those tables and level/pre-delay, derives decay from
time for characters0..5 or feedback for6/7 with the191 clamp, and overrides
the delay taps using112*time+22 (and56*time+22 for character7's left taps).
It then enters the LPF fade-in phase. Differential updates preserve register
write ordering, the distinct0x0A versus initial0xBA control byte, and the
live802B character gate rather than incorrectly using only cachedCB51.

Validation PASS (`/tmp/sc55-reverb.log`):96 request completions,1344 drain
calls,254 chorus coefficient calls,6 chorus setup completions,16 reverb setup
calls and1607 reverb coefficient calls. The input sequence covers all eight
reverb characters and LPFs, time/feedback/level/pre-delay endpoints and
overlapping requests. Comparison now includes the written delay taps and
extended bank30 coefficients, not only low coefficient registers. Only
PCM-mutated chorus mask/phase registers are excluded from final RAM comparison;
native PCM readback stalls remain covered by the earlier independent fixture.

Remaining work is integration and end-to-end validation: GS parameter mapping,
boot/default requests, periodic scheduling and readback resumption, reset
completion joining both effects controllers, voice sends, and actual audio
equivalence. The native player still disables effects until these are connected.
Normal mode has not switched away from H8; no performance claim is made here.

## Player integration: boot, periodic service, voice sends and reset

The ROM-backed native player optionally owns EffectsTables and initializes both
controllers from SystemDefaults. Product native-preview setup now imports and
passes those tables; research constructors without tables retain dry behavior.
Voice sends are no longer forced to zero when effects are present.

For each eligible control event, the player captures the clock event before
advancing reverb then chorus then voice control. If chorus readback is pending,
it returns to PCM advancement and resumes only chorus, preserving the captured
voice elapsed count. New timer events accrue separately. The engine accepts an
optional captured clock for that pass; other callers preserve existing behavior.
Reset drains voices, restores configuration, requests default effects, and
withholds subsequent MIDI until both controllers settle.

`native-player` PASS (5.16s): real PCM boot coefficients settle, a note with
CC91/93 sends leaves an audible tail after all24 voices reclaim, and GS reset
followed by Program Change/Note On resumes correctly. Final MCU PC/cycles remain
zero. The JUCE adapter diagnostic build also passes. This is not yet a waveform
equivalence or host/CPU test.

Correction to the previous endpoint-coverage statement: the earlier probe used
GS40 01 37, which is not pre-delay. The actual table entry is36. The corrected
probe passes (`/tmp/sc55-effects-predelay.log`):113 requests,1345 drain calls,
254 chorus coefficient calls,6 chorus setups,16 reverb setups and2070 reverb
coefficient calls. Thus pre-delay endpoints are now actually exercised.

Still missing: native GS global-effects parameter/macro dispatch and its full
ordering validation, end-to-end wet audio equivalence, and remaining non-FX H8
duties. The normal product path still uses H8; only the opt-in native preview
uses these connected effects. No resave, commit or push.

## GS parameter/macro dispatch connected

`EffectsSettings` owns raw global effect parameters and macro indices separately
from normalized DSP requests. It implements GS40 01 30..36 and38..3F with the
firmware's clamps. Both sets of eight macro records are imported from04:0010
(6bytes each) and04:0040 (7bytes each). Repeating the same macro restores its
fields even after manual edits. The player dispatches valid checked SysEx to
this state and requests reverb/chorus changes on the serialized audio path;
reset restores these raw fields from SystemDefaults as well.

Important continuation behavior found by the new oracle: after pre-delay36,
the next payload byte targets chorus macro38, not nonexistent37. The ROM
continues along the parameter table at04:0FBF, rather than incrementing the GS
address. Starting directly at37 remains unsupported and changes nothing.

`--native-effects-settings` PASS:83 real SysEx transactions compare all raw
reverb/chorus fields and macro indices to H8. Cases include every scalar,
clamping, every macro, repeated macro after manual edits, multi-field payloads,
the36→38 continuation, invalid starting address and missing data.
Log: `/tmp/sc55-effects-settings.log`.

Native-player PASS (5.38s): all eight pairs of macros settle on real PCM and
publish their levels, scalar level changes publish17/29, overlapping full
changes settle without unsupported events, and existing wet-tail/reset/MIDI
checks pass with zero H8 PC/cycles. The adapter diagnostic target builds.
No host or complete waveform-equivalence/CPU result is claimed. Remaining
normal-mode replacement, non-FX duties and end-to-end compatibility are still
part of the full H8 goal; it is not complete.
