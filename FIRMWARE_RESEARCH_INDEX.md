# SC-55 v1.21 解析・C++化の参照索引

更新: 2026-09-09。既存資料・実装・残存ログを整理した索引。
**完全置換の完了報告ではない。全資料の矛盾を精査した確定仕様書でもない。**

## 最初に読むこと

- 通常起動は現在もH8経路。製品のC++化は未完了。
- 2026-09-09: 割り込み可能なピッチ補間と呼び出し・復帰を追加。
  [命令境界と音声の比較](docs/firmware/NATIVE_PITCH_CONNECTIONS_2026-09-09.md)。
- 2026-09-09: ピッチ後段のcontroller/global/part補正と、割り込み可能な段階設定を追加。
  [状態・音声一致、性能上の未改善と残件](docs/firmware/NATIVE_PITCH_ADJUST_2026-09-09.md)。
- 2026-09-09: ピッチEG初期化に割り込み可能なC++単命令経路を追加。
  [初期化・再入場の検証と残件](docs/firmware/NATIVE_PITCH_INIT_2026-09-09.md)。
- 2026-09-09: ピッチEGの段階遷移・補間・時間持ち越しを通常経路へ接続。
  [実装範囲と完全置換に残る処理](docs/firmware/NATIVE_PITCH_EG_2026-09-09.md)。
- 2026-09-09: ピッチ・グライドの減衰と24bit音程積算を通常経路へ接続。
  [ポルタメントを含む音声一致・性能](docs/firmware/NATIVE_PITCH_GLIDE_2026-09-09.md)。
- 2026-09-09: ピッチ補正キャッシュ更新・最終PCMレート合成を通常経路へ接続。
  [音声一致・追加分の性能はほぼ横ばい](docs/firmware/NATIVE_PITCH_CACHE_2026-09-09.md)。
- 2026-09-08: ピッチ変調と音程差→PCMレート変換を通常経路へ接続。
  [LFO追加後からさらに演奏CPU時間3.42%減・検証範囲](docs/firmware/NATIVE_PITCH_2026-09-08.md)。
- 2026-09-08: 音量合成・2系統の変調を通常経路へ接続し、MK1のRAMアクセスを改善。
  [cutoff置換からさらに演奏CPU時間6.89%減・検証範囲](docs/firmware/NATIVE_LEVEL_2026-09-08.md)。
  LFOの遅延・attack・rate・サイン波も通常経路へ接続済み。
  [追加分の演奏CPU時間1.85%減・検証範囲](docs/firmware/NATIVE_LFO_2026-09-08.md)。
- 2026-09-08: 通常経路のカットオフ補間・符号化をC++化し、置換先判定を整理。
  [controller置換からさらに演奏CPU時間5.81%減・検証範囲](docs/firmware/NATIVE_CUTOFF_2026-09-08.md)。
- 2026-09-08: 通常経路のcontroller計算11出力をC++化。
  [前回実装から演奏CPU時間3.84%減・検証範囲](docs/firmware/NATIVE_CONTROLLERS_2026-09-08.md)。
- 2026-09-08: 通常経路のTVAランプ・出力値生成を部分的にC++化。
  [実装範囲・音声一致検証・性能上の限界](docs/firmware/NATIVE_TVA_2026-09-08.md)。
- `NUKED_SC55_NATIVE_PREVIEW=1` で有効になる比較用経路は、通常音色の限定的な連続発音に対応する。
  ドラム・バリエーション・GS/GM SysEx・エフェクト設定・パネル/LCD実行は未対応。
- 「関数単位でH8と一致」「H8を実行したままの回帰トレースが一致」「H8なしで発音できる」
  「製品として完全互換」は別の証拠。相互に読み替えない。
- 文書中の「all tests pass」「未実装」「次に実装する」は、その段落を書いた時点の履歴。
  日付が同じでも追記順が前後する。ファイル末尾や行数だけで新旧を決めない。
- 比較モードにもROMセット読み込みが残る。**control ROMなしで製品が起動・完全動作することは未検証・未達成。**

## 証拠の扱い

| 表記 | 意味 |
|---|---|
| 関数検証済み | 記載された入力範囲・ROM版でH8との比較記録がある。全入力・全機種の保証ではない |
| 結合検証済み | 記載された組み合わせで状態遷移・PCM出力等を確認。GS全体の互換性とは別 |
| 部分解析 | 一部の表・分岐・経路は判明しているが、残りを含む仕様は未確定 |
| 未完了 | 未解析、実装不足、接続不足、または製品としての検証不足が残る |

下表の状態は既存の記録に基づく。今回の再検証結果ではない。

## 分野別の入口

| 分野 | 現時点の到達点／残件 | 実装 | 詳細資料・検証コード |
|---|---|---|---|
| パッチ配置 | v1.21のname-first配置、bank1=224件・bank2=162件を検証。他版へ一般化しない | [sc55_patch.cpp](src/backend/sc55_patch.cpp)、[ヘッダー](src/backend/sc55_patch.h) | [Bank2 extraction](tools/firmware-oracle/README.md#bank2-data-only-patch-extraction)、[patch-layout-test](tools/firmware-oracle/patch-layout-test.cpp) |
| データ抽出・所有 | MD15に386パッチ・参照サンプル・計算表を格納。GS初期状態や全エフェクト設定は含まない | [SoundData](src/backend/sc55_sound_data.h)、[import](src/backend/sc55_sound_data_import.h)、[cache](Plugins/Source/NativeSoundDataCache.h) | [MD15](tools/firmware-oracle/README.md#md15-import-and-layout)、[cache test](tools/firmware-oracle/sound-data-cache-test.cpp) |
| MIDIバイト列・通常音色 | デコード、running status、チャンネル値、bank MSB=0の128プログラム対応。全MIDI/GSではない | [MIDI](src/backend/sc55_midi.h)、[channel](src/backend/sc55_channel.h)、[preset](src/backend/sc55_preset.h)、[queue](src/backend/sc55_midi_queue.h) | [front end](tools/firmware-oracle/README.md#native-midi-front-end)、[decoder test](tools/firmware-oracle/midi-decoder-test.cpp) |
| 受信・ベロシティ・パート | 受信ゲート、キー範囲、速度補正、ペダル、コントローラ寄与に比較記録。GSの初期設定一式は不足 | [dispatch](src/backend/sc55_note_dispatch.h)、[controllers](src/backend/sc55_note_start.h)、[fanout](src/backend/sc55_note_fanout.h) | [receive gate](tools/firmware-oracle/README.md#note-receive-gate)、[part owner](tools/firmware-oracle/README.md#native-part-note-state-owner) |
| キー・サンプル選択 | ピッチ追従、スケール、ゾーン、descriptor、通常サンプル設定を検証。特殊サンプル経路は別扱い | [setup](src/backend/sc55_note_setup.h)、[bank](src/backend/sc55_sample_bank.h)、[install](src/backend/sc55_sample_install.h)、[voice setup](src/backend/sc55_voice_setup.h) | [sample preparation](tools/firmware-oracle/README.md#composed-partial-sample-preparation-and-voice-binding)、[install test](tools/firmware-oracle/sample-install-test.h) |
| 第1EG・振幅 | 初期値、キースケール、時間、短時間分岐、ステージ、release、PCM同期に関数・結合検証 | [envelope](src/backend/sc55_envelope.h)、[runner](src/backend/sc55_envelope_runner.h)、[setup](src/backend/sc55_envelope_setup.h)、[PCM](src/backend/sc55_envelope_pcm.h) | [initialization](tools/firmware-oracle/README.md#native-envelope-initialization)、[runner](tools/firmware-oracle/README.md#persistent-first-envelope-runner)、[oracle](tools/firmware-oracle/envelope-runner-oracle.h) |
| 第2EG・フィルタ制御 | 区間更新から出力・PCM・起動への結合が進んでいる。初期の「区間補間のみ」という段落だけを現状としない | [envelope](src/backend/sc55_envelope.h)、[second tables](src/backend/sc55_second_envelope_tables.h)、[filter](src/backend/sc55_filter.h)、[voice control](src/backend/sc55_voice_control.h) | [second envelope](tools/firmware-oracle/README.md#second-envelope-interpolation)、[activation](tools/firmware-oracle/README.md#second-envelope-activation-handoff)、[PCM integration](tools/firmware-oracle/README.md#composed-voice-control-with-the-real-pcm-pipeline) |
| ピッチEG・グライド | 計算・状態更新・通常発音との結合を検証。モノ/ポルタメント全体のGS運用は未完了 | [pitch](src/backend/sc55_pitch.h)、[voice prepare](src/backend/sc55_voice_prepare.h) | [pitch lifecycle](tools/firmware-oracle/voice-lifecycle.md#persistent-composed-part-pitch-preparation)、[pitch start](tools/firmware-oracle/README.md#native-normal-pitch-start-composition) |
| LFO・音量・パン・送り | 各演算、共有、初期化、コントローラ接続に比較記録。送り量の計算とエフェクト本体は別 | [LFO](src/backend/sc55_lfo.h)、[modulation tables](src/backend/sc55_modulation_tables.h)、[level](src/backend/sc55_level.h)、[tables](src/backend/sc55_tables.h) | [LFO connection](tools/firmware-oracle/README.md#patch-lfo-connection-to-real-pcm)、[送り順訂正](tools/firmware-oracle/README.md#correction-channel-to-voice-effect-send-byte-order) |
| ボイス管理・再利用 | グループ、割当、回収、優先候補、強制停止、key latch、再利用待ちを検証。全モードの運用方針は未完了 | [allocator](src/backend/sc55_voice_allocator.h)、[links](src/backend/sc55_voice_links.h)、[lifecycle](src/backend/sc55_voice_lifecycle.h) | [専用資料](tools/firmware-oracle/voice-lifecycle.md)、[allocator oracle](tools/firmware-oracle/voice-allocator-oracle.h)、[allocation test](tools/firmware-oracle/melodic-allocation-test.h) |
| ドラム | tone map参照、ベロシティ、排他停止、同音再処理、admission、初期キー、bank2のPCM発音に記録。製品のドラムセット選択・設定接続は未完了 | [rhythm](src/backend/sc55_rhythm.h)、[admission](src/backend/sc55_rhythm_admission.h)、[sample install](src/backend/sc55_sample_install.h) | [rhythm velocity](tools/firmware-oracle/README.md#mapped-rhythm-velocity-expansion)、[admission](tools/firmware-oracle/README.md#owned-rhythm-group-admission)、[bank2 PCM](tools/firmware-oracle/README.md#first-native-bank2-drum-pcm-startup)、[velocity oracle](tools/firmware-oracle/rhythm-velocity-oracle.h) |
| モノ音 | 既存voice選択・速度補正・release・held keysは部分解析/実装。mono再発音の全経路は未完了 | [dispatch](src/backend/sc55_note_dispatch.h)、[note start](src/backend/sc55_note_start.h) | [mono selection](tools/firmware-oracle/README.md#existing-mono-voice-selection)、[live probe](tools/firmware-oracle/README.md#live-mono-preparation-probe) |
| EG/LFOの周期・開始待ち | device cycle基準の周期と保留状態を実装。H8タスクの実際の遅延・同時進行を完全再現したものではない | [clock](src/backend/sc55_control_clock.h)、[runtime](src/backend/sc55_voice_runtime.h)、[engine](src/backend/sc55_voice_engine.h) | [clock evidence](tools/firmware-oracle/README.md#recovered-voice-task-clock)、[startup ownership](tools/firmware-oracle/README.md#native-prepared-start-ownership)、[clock probe](tools/firmware-oracle/control-clock-probe.h) |
| 製品への接続 | 比較モードのみMIDI→発音→EG→解放をC++実行。通常モードはH8。失敗時は比較モードの出力を停止 | [native player](src/backend/sc55_native_player.h)、[adapter](Plugins/Source/NukedSC55Emulator.cpp)、[processor](Plugins/Source/PluginProcessor.cpp) | [preview](tools/firmware-oracle/README.md#native-melodic-player-preview)、[player test](tools/firmware-oracle/native-player-test.h)、[voice/PCM test](tools/firmware-oracle/voice-control-pcm-test.h) |
| GS/NRPN・エフェクト・UI | 一部の宛先・変換・既存PCM処理は分かっている。nativeの初期状態、全設定反映、パネル/LCDの置換は未完了 | [既存PCM](src/backend/pcm.cpp)、[既存LCD](src/backend/lcd.cpp) | [構造資料](FIRMWARE_STRUCTURE.md)の4q〜4t。旧patch配置に依存する解釈は冒頭訂正の対象 |

## 優先して読む訂正・更新

1. **パッチ配置**：`FIRMWARE_STRUCTURE.md`冒頭の訂正が旧配置推定に優先する。
   名前は+0、共通部は+0x0c、partialは+0x20/+0x7c、record長は0xd8。
   ROM2 bank1は0x10000、bank2は0x20000。旧フィールド説明を一括して確定扱いしない。
2. **bank2は「162個すべてドラム」ではない**：補助的な通常音色も含む。
   MD15の224+162件と、ドラムセット/キーの選択マップを混同しない。
3. **CC91/CC93送り順**：[訂正節](tools/firmware-oracle/README.md#correction-channel-to-voice-effect-send-byte-order)を優先。
   part+0x0eはchorus/CC93、+0x0fはreverb/CC91。effects wordはreverbが上位byte。
   一部構造体の歴史的なフィールド名は逆のままなので、名前だけで再接続しない。
4. **停止ステージと再利用待ち**：[停止ステージ訂正](tools/firmware-oracle/voice-lifecycle.md#correction-stopped-stages-do-not-poll-for-silence)と
   [別のreuse gate](tools/firmware-oracle/voice-lifecycle.md#reuse-readiness-gate-not-the-stopped-stage-dispatcher)を両方読む。
   強制停止ステージの通常EG処理に、独自の「無音まで待つ」を足さない。
5. **古い「未実装」リスト**：oracle READMEの Remaining work、voice-lifecycle冒頭、
   FIRMWARE_STRUCTUREの「5. ネイティブ化にとっての意味」は履歴。現在の残件は本索引と実装を合わせて確認する。
   allocator、MIDI ingress、第2EG、通常発音結合などは後続で実装が進んでいる。
6. **制御周期**：通常voice taskの名目周期は160256 device cycles
   （20MHz換算8.0128ms）。hostのprocessBlockごとにEGを1回進める設計ではない。
   [clock evidence](tools/firmware-oracle/README.md#recovered-voice-task-clock)には実H8の遅延との差も記載。
7. **CPU測定の母数**：旧資料のH8命令/タスク比率とホストCPU比率は別物。
   最新の[CPU測定要約](docs/firmware/CPU_PROFILE_2026-09-07.md)を参照する。
8. **ログの条件**：`DEVIATIONS.md`の「Debugビルドで有効」という旧説明は現状と一致しない。
   現在の[SC55Debug.h](Plugins/Source/SC55Debug.h)は環境変数で有効化し、Releaseからも除去されない。

## 次の実装に残っていること

「一切未解析」ではなく、断片の解析済み範囲と未接続部分が混在する。

- GS初期状態、パート設定、ドラムセット・variation/bank選択を、出所と更新ルール付きで所有する。
- SysEx/NRPN/チャンネルモード、mono/portamento、特殊サンプルなどの未対応分岐を埋める。
- 既存のドラムadmission・PCM準備を、実際のセット/キー設定から製品のMIDI経路へ接続する。
- エフェクト送りだけでなく、エフェクト設定・初期化と処理本体の対応を完成させる。
- パネル/LCD/保存状態まで含め、通常起動をC++側へ置換する。比較モードの中立値をGS仕様と称さない。
- control ROMなしの起動、全対応モード、release tail、状態復元、LogicでのCPU目標を確認する。

## スレッド監査で判明した未修正事項

2026-09-07の限定検証。実際のLogicでの発生頻度やCPU寄与を測ったものではない。

- 通常H8経路の`LCD_Write`はUIのLCD snapshotと共有するmutexで待つ可能性がある。
- UIのGM/GSボタンとaudio callbackが同じ単一producer想定のMIDI FIFOへ書き込む。
  書き込み位置の読み出しが重なる実行順序では、2回成功扱いでも1byteが失われる。
- `NUKED_SC55_DEBUG=1`ではReleaseでもstderrへ出力・flushする。未設定なら出力しない。
- 検証用ソースはローカル証拠アーカイブ内の`sc55-thread-audit.cpp`。
  現時点で製品側の修正はしていない。

## 証拠・再開手順

1. 本索引で対象分野と訂正先を確認する。
2. 該当する実装ヘッダーとoracle/testを読む。検証済み入力範囲と未対応条件を確認する。
3. [保全記録](docs/firmware/evidence/README.md)で元ログとハッシュを特定する。
4. 再検証が必要な場合だけ、[CMake定義](tools/firmware-oracle/CMakeLists.txt)と該当CLIモードを使う。
   過去の「全テスト通過」を現在の変更の保証に流用しない。

原資料は削除・再構成していない。今回の索引だけで全解析内容の完全性を保証するものではない。
以後は解析の追加時に、この索引の状態・詳細へのリンク・訂正先も更新する。
