# H8制御移植の現状判定

2026-09-10。現在のソースと保存済み試験結果の照合。
完了条件は `WAVESTATION_STRUCTURE_CHECK_2026-09-09.md` を変更しない。
本書は新たな全試験の実行結果ではない。過去資料の未チェック項目を作業件数に数えない。

## 製品につながっている責務

| 責務 | 現在の製品実装 | 確認の範囲／残る限界 |
|---|---|---|
| 起動・音源所有 | `NukedSC55Emulator::initialise` → `NativeSynth` | 環境変数なしはv1.21 C++。H8構築は`NUKED_SC55_USE_H8=1`の分岐だけ。他ROMのC++対応ではない |
| MIDI受信・パート振分け | `NativeMelodicPlayer::push/receive`、`receiveNote/receiveController/receiveVoiceController` | 直接設定更新と発音commandは別。GS part受信を発音準備中にも反映する試験が通る |
| 任意CCによるportamento source | `receiveVoiceController`と`PortamentoSourceRequest` | 最新コードで既存768ケースを実行しH8一致。全128CC、値0/48/127、melodic/rhythmの2経路。固定CCとの優先順位と主要scalar副作用を比較 |
| 16種の発音管理command | `VoiceCommands`と`NativeVoiceEngine::serviceCommand`、receive recovery | NoteOn/Off、hold/sostenuto/portamento、source、notes/sound off、reset、program、mono/polyを接続。消費・保留発音はvoice ownerが管理。下記の111比較はこれらの全入力空間を網羅しない |
| melodic/rhythm/monoの割当 | `NativeVoiceEngine::serviceAdmission`、`VoiceControlRuntime` | 3経路の準備・割当・再開をvoice ownerへ集約。変更後もrelease integration111比較、shared rhythm42比較がPASS。単なる関数単体ではなくMIDIを流した比較 |
| stealing・partial reserve | `ensureCapacity`と`VoiceCapacityPolicy` | 実曲の満杯状態まで通るが、H8とのsurvivor／key-on数の差は残る。同一サンプル時刻の一致を目的に待ちを追加しない |
| PCM完了・停止・再利用 | `receivePcmBoundary/servicePcmBoundaries/serviceActivation/serviceRetirements` | 既存PCMを使用。mono欠落とdrum attackの回帰条件を保持。単なるactive bitで所有・発音・解放を同一視しない |
| GS/GM reset | `resetVoices`＋controller側の設定復元 | EOX、sustain、24slot使用、連続reset、後続MIDI保持をnative-player試験で確認 |
| 共通周期のEG/LFO/pitch/filter/level | `ControlTaskClock` → `serviceControl(...pass)` | 1tick=20032 cycles、voice period=8tick。通常側はC++演算を直接実行。H8命令時間の再現は診断targetだけ |
| effect制御 | `SystemSettings`、`EffectsControl`、`serviceEffects` | パラメータ要求／PCM readback・rampとサンプル演算を分離。既存PCM DSPの置換は要求外 |
| GS設定・bulk受信 | `PartSettings`、`SystemSettings`、`RhythmSettings`、bulk decoder | 型付き設定へ反映。設定保持の比較は、その全組合せの音声一致の証明ではない |
| device受信設定 | `MidiInputSettings`、`NativeMidiInputState` | Device ID、Rx Inst/GS Reset/SysExとchecksum設定を接続。物理H8比較、512 codec/handoff比較あり。checksum切替の物理操作比較は未確認 |
| 公開パネル操作 | UI `pressFrontPanelButton` → resolved `SynthCommand` | PART/ALL/MUTE/SOLO/値変更/設定項目。2X対象の共有を修正。UI状態をprocessBlockから読まない |
| 通常表示・model45 | message-thread `copyLcdDisplay/captureNativeState`、`DisplayPresentation` | 文字・bitmap・scroll・メーターをUIで処理。音声側はsound/event snapshotを公開。メーター集計もUIへ移動済み |

## 今も完了を主張できないもの

1. **拡張パネルの機能対応**。通常play画面と公開メニューはあるが、H8の別表示モード、
   長押し／複数押下／隠し操作を全部置き換えた実装ではない。
   2026-09-10のユーザー回答により、隠し操作・拡張LCDメニューは後回し。
   次の作業は音源制御を優先し、これらのUI実装を先に進めない。
   後回しは実装済みという意味ではない。再開時も音声側へボタンscanやLCD更新を戻さない。
   ユーザーが除外したMIDI送信・dump UIは、この不足に数えない。
2. **未分類経路**。task5/6は0542で待機、通常timer／既知IRQ通知からは起きない。
   既知入口を繰り返し読むだけでは進捗にならない。別の通知元の証拠が得られた時に追う。
   Rx Remoteは外部入力経路であり通常MIDIに読み替えない。
3. **実曲の差の意味**。GATCHA55 part16の第8音は回帰条件を通るがkey-on数87/84。
   55KTIZKE先頭60秒は13kickの頭のgainが一致するがkey-on数2100/2114。
   この数だけで欠音とも許容差とも断定しない。再調査時は音楽的なノート要求と
   PCMの再キーオンを区別し、実際の欠音／attack／reserve違反に結び付ける。
   追加の識別別観測：GATCHA55の差3件はGS part4のkey67/71とpart9のkey48で、
   part16ではない。55KTIZKEのdrum part0は853/853、mono part2は195/195。
   part2は総数が同じでも起動時の所属key分布が異なる。part8もmonoで109/112。
   polyの差はpart3=324/325、4=218/219、7=229/235、12=14/17。
   未識別PCM起動は両方0。これは「全MIDIノートが正しい」の証明ではないが、
   以前の総数差をdrum／part16欠落の根拠にするのは誤り。
   次に実曲差を追う際はこのpart/modeの区別を使う。ログは
   `/tmp/sc55-song-identities-kick.log`、`/tmp/sc55-song-identities-gatcha.log`。
   追加のmono保持キー比較：nativeのqueued workが0、H8のRXとcommand ringが空、
   H8の24slotのpending operationが0の観測点でA070[part]とcurrentMonoKeyを比較。
   part2=15228回、part8=14867回、part11=14699回、全て不一致0
   (`/tmp/sc55-song-mono-identities.log`)。途中の全ノートの可聴発音やEG波形の証明ではない。
   PCM起動数差だけをmono保持キーのバグと呼んで修正を入れない。
4. **未解釈設定145byte**。bulk保持はあるが、観測されなかったことは未使用証明ではない。
   paddingらしい配置だけで機能なしと断定せず、実読出し・設定表・入力経路で判断する。
5. **製品確認**。最新のGS受信整理・メーター移動後のCMake製品翻訳単位と通常headless
   targetに加え、Release arm64 Standalone＋内蔵AUv3もBUILD SUCCEEDED
   (`/tmp/sc55-current-control-release.log`)。Logic実操作、実ホストのCPU負荷は未確認。
   署名登録やインストールはしていない。

## この判定で実在を確認したログ

- `/tmp/sc55-meter-owner-synth.log`: 通常C++、checksum3b54320560580fd3、可変block一致。
- `/tmp/sc55-meter-owner-adapter.log`: 44.1/48/96kHz、2X表示を含むadapter試験。
- `/tmp/sc55-part-transaction-release-test.log`: release integration111比較。
- `/tmp/sc55-part-transaction-rhythm.log`: shared rhythm42比較。
- `/tmp/sc55-part-transaction-preparation.log`: 準備中GS part・同一channel再設定。
- `/tmp/sc55-current-source-controller.log`: 既存768ケースの最新実行。重複試験を新設していない。
- `/tmp/sc55-reset-owner-test.log`: native-playerのresetを含む試験。
- `/tmp/sc55-program-gate-test.log`: 受信設定のH8比較と保存handoff。
- `/tmp/sc55-control-oracle-test.log`: 制御演算の診断比較。最新全変更後の再実行ではない。
- `/tmp/sc55-product-normal-gatcha.log`、`/tmp/sc55-product-normal-kick.log`: 実曲回帰。
  最新全変更後の再実行ではない。

## 再開しない作業

MIDI出力、独立PCM rendererへの昇格、サンプルごとの互換同期削減、命令時間の擬似待ち。
関数移動・命名整理だけを「H8の未実装機能が減った」と報告しない。
