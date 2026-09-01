# オリジナル Nuked-SC55 からの変更点

JUCE プラグイン化（`Plugins/`）にあたって加えた変更の一覧。
「オリジナル」= このリポジトリの upstream、`src/` のスタンドアロン実行ファイル。

---

## 0. 全体方針

- `src/` への変更は **オリジナルの実行ファイルの挙動を変えない**ことを条件にした。
  SDL 依存部は `#ifndef NUKED_SC55_NO_MAIN` で囲んであり、プラグインビルドでのみ外れる。
- 挙動を変える判断はすべて `Plugins/Source/` 側に置いた。
- エミュレータのコア（`mcu`, `pcm`, `sm`, ROM 配列, サンプル sink）は**すべてグローバル変数**であり、
  実質シングルトン。これは変えていない。プラグイン側で1プロセス1インスタンスに制限した。

---

## 1. `src/`（コア）の変更

### 1.1 SDL 依存の分離

| ファイル | 変更 | オリジナルへの影響 |
|---|---|---|
| `mcu.cpp` | `#include "SDL.h"`、`sdl_audio`、`audio_callback`、`MCU_OpenAudio`/`MCU_CloseAudio`、`work_thread`、`MCU_Run`、`MCU_WorkThread_Lock/Unlock` を `#ifndef NUKED_SC55_NO_MAIN` で囲んだ | なし |
| `lcd.cpp` | ウィンドウ・レンダラ・キーマップ以降（`lcd_width`/`lcd_height` の直後から末尾まで）を同様に囲み、プラグイン用の no-op 実装を用意。`LCD_Enable` / `LCD_Write` / `LCD_QuitRequested` は SDL 非依存なのでそのまま生かしてある（将来エディタに LCD を出せる） | なし |
| `submcu.cpp` | 未使用の `#include "SDL_audio.h"` を削除 | なし |
| `mcu.h` / `mcu.cpp` | `SDL_atomic_t mcu_button_pressed` → `std::atomic<uint32_t>`。`SDL_AtomicGet/Set` → `load/store` | なし（等価） |
| `mcu.cpp` | `static _mutex *work_thread_lock` → `SDL_mutex` に復元（`_mutex=SDL_mutex` というプリプロセッサ定義でごまかしていたのをやめた） | 正常化 |

**理由**: AUv3 拡張はサンドボックス必須で、SDL の初期化が `dyld` のイニシャライザ内で `abort()` する。
また homebrew の SDL2 は arm64 のみで、universal ビルドの x86_64 スライスがリンクできない。
そして `/opt/homebrew/lib/libSDL2.dylib` にリンクした成果物は他のマシンで起動しない。

### 1.2 再初期化への対応 — `MCU_Init()` / `SM_Reset()`

オリジナルはプロセスで **1回しか初期化しない**ため、`mcu_t` / `sm` の外にあるファイルスコープ変数を
誰もリセットしない。プラグインはホストの `prepareToPlay` ごとに初期化し直すので、前回の値が残る。

`MCU_Init()` で追加初期化するようにしたもの:

```
ga_int[], ga_int_enable, ga_int_trigger, ga_lcd_counter,
ad_val[], ad_nibble, sw_pos, io_sd, adf_rd, ssr_rd, analog_end_time,
uart_rx_byte, uart_rx_delay, uart_tx_delay
```

`SM_Reset()` で追加初期化するようにしたもの:

```
sm_cts, sm_timer_cycles, sm_timer_prescaler, sm_timer_counter,
uart_rx_gotbyte, uart_rx_byte, uart_rx_delay
```

**最も重要なのは `uart_rx_delay` / `uart_tx_delay` / `sm_timer_cycles`**。
これらは**絶対サイクル値**を保持しているのに `mcu.cycles` は 0 に戻るため、
`if (mcu.cycles < uart_rx_delay) return;` により**前回の稼働時間ぶん MIDI 受信が停止する**。

実測（同一プロセスで3回初期化し、毎回ノートを送る）:

```
修正前: pass 1  AC=0.010585  発音
        pass 2  AC=0.000002  無音   ← MIDI が死んでいる
        pass 3  AC=0.000002  無音
修正後: pass 1/2/3 すべて AC≈0.0106  発音
```

オリジナルへの影響: **なし**（初期化が1回なら、これらは元からゼロ）。

### 1.3 追いつきループの上限 — `PCM_Update` / `TIMER_Clock` / `SM_Update`

```c
PCM_Update():  while (pcm.cycles < cycles)          // 625 サイクル/回
TIMER_Clock(): while (timer_cycles*2 < cycles)      // 2 サイクル/回
SM_Update():   while (sm.cycles < cycles * 5)
```

カウンタが不整合になると差分ぶんを全部回す。10分稼働後は `mcu.cycles ≈ 1.1e10` で、
`TIMER_Clock` は **55億回**ループする。結果はスレッドの事実上のハングで、そこから

- FIFO が枯れる → 「パツッ」という不連続 → 以降無音
- `release()` が `join()` で永久に待つ → プラグインを外す操作が固まる
- 新しいインスタンスを挿しても、古いスレッドがグローバルを握ったまま → 鳴らない
- ホストのプロセスを殺すしか復帰手段がない

`mcu.h` に `catch_up_limit = 1000000`（約40ms）を定義し、3箇所の先頭でクランプする。
正常時の呼び出しは12サイクル遅れなので**一度も発動しない**。

---

## 2. `Plugins/Source/` — 挙動の差分

### 2.1 MIDI 入力経路

オリジナルは RtMidi のコールバックが直接 `MCU_PostUART()` を呼ぶ。
プラグインはオーディオスレッドからエミュのグローバルを触れないため、間にロックフリー FIFO を挟む:

```
processBlock (audio thread) → midiFifo → emulationThread → MCU_PostUART()
```

`processBlock` は `MidiBuffer` の各メッセージを `getRawData()/getRawDataSize()` で丸ごと渡す。
**メッセージ種別によるフィルタは一切していない。**

### 2.2 ブートゲート（`drainMidi()`）

ファームが起動シーケンス中は UART を読まないため、送られたものが溜まり続け、
読み始めた瞬間に一括投入される。ノートオンとノートオフが同一時刻に届いて全部消える。

対策として、**ファームが最初の1バイトを読むまで**（= `uart_read_ptr` が動くまで）を
「ゲート閉」とし、その間の**非 SysEx メッセージを破棄**する。
一度開いたゲートは二度と閉じない。

- SysEx は**ゲートが閉じていても通す**（ホストは曲頭のセットアップバンクを起動中に投げてくる）
- 判定は**メッセージ単位**。`0xF0` で始まる列は `0xF7` まで一括で扱う。
  `0xF7` と `0xF8` 以上のリアルタイムバイトは新しいメッセージの開始とみなさない
- 唯一の例外は UART リング（8192バイト）が溢れる寸前（`uartRingHeadroom = 256`）

> **注意**: 一時期「バックログ64バイトで捨てる」実装を入れていたが、これは誤りだった。
> RPN は `CC101 → CC100 → CC6 → CC38` の4メッセージ列で、曲頭のセットアップ（16パート分の
> ボリューム・パン・プログラムチェンジ＝約140バイト）に埋まっていると途中で切られ、
> ピッチベンドレンジが黙って無効になる。上限は削除済み。

実測:

```
ブート中のノート          破棄（実機の電源投入中と同じ）
ブート後のノート          発音
ブート中の SysEx バンク   440バイト → 全到達
稼働中の SysEx バンク     660バイト → 全到達
140バイトのセットアップ   140/140 到達、RPN レンジ12 が適用（441.0 → 882.0 Hz）
```

### 2.3 起動時 GS リセット

オリジナルは `-gs` オプション指定時のみ `MIDI_Reset()` で送る（**既定は送らない**）。
プラグインは**常に送る**。ファームが UART 受信可能になった時点で、MIDI キューを経由せず
`MCU_PostUART()` に直接11バイト投入する（ゲートに食われないため）。

```
F0 41 10 42 12 40 00 7F 00 41 F7
```

**これは既定動作からの逸脱**。オプション化していない。

### 2.4 DC カット

PCM チップは DAC のバイアスとして定数を加算している（`pcm.cpp` の最終ミックス段、
`config_reg_3c & 0x30 == 0x30` のとき `orval |= 1 << 12`）。
`MCU_PostSample` の `>> 15` を通ると int16 で 512、すなわち 0.015625 の直流になる。

実機ではアナログ出力段のカップリングコンデンサで落ちるが、エミュレータには無い。
`render()` の出力段に **5Hz の1次ハイパス**を左右独立で入れた。係数は
`1 - 2π·5/fs` で、ホストのサンプルレートから `initialise()` で算出する。

```
無音時   DC=0.015625 AC=0.000000  →  DC=0.000000 AC=0.000000
発音時   レベル・音程とも変化なし
```

> 波形が「DC と 0 の間を往復する方形波」に見えたら、それは音源の音ではなく
> **FIFO の供給不足**。43Hz 程度の方形波として観測された。

### 2.5 アンダーラン時の処理

オリジナルの `audio_callback` は読んだ領域をゼロ埋めするので、供給が間に合わないと
ゼロが出る（= DC との段差で方形波になる）。プラグインは**直前のフレームを保持**する。
発生回数は `sourceUnderruns` に数え、デバッグログの `underruns=` に出る。

### 2.6 エミュレーションスレッドの QoS

優先度指定なしの `std::thread` は、アプリが放置されると macOS に QoS を絞られ、
供給が間に合わなくなる。macOS では `QOS_CLASS_USER_INTERACTIVE` を明示する。

### 2.7 1プロセス1インスタンス制限

コアがグローバル変数の塊なので、2個目のインスタンスが動くと2本のスレッドが
同じ CPU 状態を進め、ボイスがキーオンのまま残る等の破壊が起きる。
2個目の `initialise()` は失敗させ、無音のままにする（壊れた音を出さない）。

```
修正前: 2個目生成 → instance A 無音 / instance B AC=0.002587（MIDI 入力ゼロなのにノイズ）
修正後: 2個目 → "Another SC-55 instance is already running in this process"
        instance A は完全な無音を維持し、ノートを送れば正常に発音
```

あわせて、`release()` がグローバルの sink を**無条件に**消していたのを修正した
（所有していないインスタンスの `release()` が、動作中のインスタンスの出力を止めていた）。

### 2.8 ROM の探索先

AUv3 は App Sandbox 必須で、`~/Documents` も本物の `~/Library` も見えない。
`userApplicationDataDirectory` は拡張のコンテナを指す。探索先に以下を追加:

- バンドル内の `Resources` および `Contents/Resources`（同梱する場合）
- `userDocumentsDirectory` — サンドボックス内では**拡張のコンテナの Documents**

現状は後者にROMを置いてある:

```
~/Library/Containers/tokyo.studio-r.sc55.sc55AUv3/Data/Documents/
```

> ビルド後に `.appex` の中へ手でコピーしてはいけない（署名が壊れて拡張がロードされない）。
> 同梱するなら Projucer の `customXcodeResourceFolders` を使い、署名前にコピーさせる。

### 2.9 その他

- スタンドアロンは起動時に**利用可能な MIDI 入力を全て有効化**する。
  JUCE は保存済み設定を CoreMIDI の identifier で照合するが、Bluetooth 機器は
  再接続で identifier が変わり、設定上は有効に見えたまま無効になるため
- `publishDebugState()` は**デバッグログが有効なときだけ**呼ぶ。
  1命令ごとにアトミック書き込み12個を行っていた

---

## 2.10 ビルド（クローン直後から）

JUCE は `3rdparty/JUCE` にサブモジュールとして固定してある（**9.0.0**）。
JUCE 9 のモジュールは同じバージョンの Projucer でしか保存できないため、
Projucer 自体もサブモジュールからビルドする。

```
git clone --recurse-submodules <repo>
cd Nuked-SC55/Plugins
./build.sh              # Projucer をビルド → プロジェクト生成 → Standalone をビルド
./build.sh Debug        # Debug が欲しいとき
```

`Plugins/Builds/` と `Plugins/JuceLibraryCode/` は Projucer の生成物なので
コミットしていない。`build.sh` が毎回生成する。

`.jucer` の各モジュールは `useGlobalPath="0"` にしてある。`1` だと Projucer の
グローバル設定（各自のマシンの JUCE の場所）が優先され、他人の環境で壊れる。

ROM はリポジトリに含まれない。SC-55 v1.x の5ファイル
（`sc55_rom1.bin`, `sc55_rom2.bin`, `sc55_waverom1-3.bin`）を各自で用意する。

---

## 3. ビルド構成

| 項目 | 内容 |
|---|---|
| 製品名 | `.jucer` の `targetName` が `Nuked-SC55` になっていて製品名を上書きしていた。`SC-55` に変更（プロジェクト名・`JucePlugin_Name` は元から `SC-55`） |
| SDL | `.jucer` から `externalLibraries="SDL2"`、`/opt/homebrew` のヘッダ/ライブラリパス、`_mutex=SDL_mutex` の定義をすべて削除 |
| アーキテクチャ | **Projucer の既定のまま**（universal / x86_64 + arm64）。上書きはしていない |
| Xcode プロジェクト | `Builds/MacOSX` に `SC-55.xcodeproj` / `Nuked-SC55.xcodeproj` / `SC55.xcodeproj` の3つがあり、**同じ `build/` と同じ製品名を共有**している。emulator のソースが入っているのは **`SC-55.xcodeproj` だけ**。他の2つでビルドすると成果物を上書きする（未削除） |

**既知のはまりどころ**

- 製品名を変えた直後は、旧名のビルド中間物が残っていると archive が
  `invalid signature (code or signature have been modified)` で落ちる。
  Product → Clean Build Folder（⇧⌘K）で解消する
- `Builds/MacOSX/build/Release/` に前回 archive のシンボリックリンクが残っていると、
  Release ビルドが `unable to create directory 'SC-55.appex'` で落ちる。リンクを消せば通る
- デバッグログは `NUKED_SC55_DEBUG` 環境変数、または Debug ビルドで有効。出力先は stderr

---

## 4. オリジナルと「音」が変わる点

挙動差のうち、実際に出音・応答が変わるのは次の3つだけ:

1. **起動時 GS リセット** — 初期状態が変わる。オリジナルの既定は送らない（逸脱として最大）
2. **DC カット** — 直流が消える（可聴域は不変、ヘッドルームが 1.5% 戻る）
3. **ブート中の MIDI 破棄** — 起動直後（約3秒）に弾いた音が鳴らない。
   オリジナルは溜めて後から鳴らす。実機は鳴らさない

---

## 5. 未解決 / 確認済みの制限

- **パーシャルリザーブ（SysEx `40 01 10`）が効かない。**
  同じブロックのリバーブレベル（`40 01 33`）は効く（tail 0.0026 → 0.0057）ので、
  SysEx は届いてブロックも解釈されている。そのうえで2パートを競合させ
  `22:2` と `2:22` を送っても取り分は 50:50 のまま。ファーム側で未実装か、
  テスト条件が再現できていないかのいずれか
- **アフタータッチは音に影響しない。** バイトは届いている。GS の初期状態では
  デプスが全て 0 のため（実機と同じ）。SysEx か NRPN で設定すれば効く
- **起動直後 約3秒はノートを受け付けない。** 実機の電源投入直後と同じ。
  ホストのトランスポートを即スタートすると頭が欠ける
- **起動から約3秒の時点で一度だけ短い過渡音が出る。** DAC バイアスが 0 から
  立ち上がる段差を DC カットが過渡応答として通すため。一発のみ
- **VST3 / AU（.component）は未検証。** 動作確認は Standalone と AUv3 のみ
