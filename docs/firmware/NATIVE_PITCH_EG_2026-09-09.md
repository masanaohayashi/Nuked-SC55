# 通常経路のピッチEG段階遷移・補間 — 2026-09-09

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

## 追加した通常経路

- `TryDispatchPitchEnvelope`: 4fdbから段階判定・次段階への更新、
  3つの目標/rate選択、上昇下降判定、保持値の2つのpitchコピーへの反映。
  全12段階を扱い、補間5060、後段50cf、終了5367へ戻る。
- `TryAdvancePitchEnvelope`: 5060..50cf。elapsedと残余時間の16bit加算、
  rate積とphase更新、overshootの除算と持ち越し、上昇/下降の補間、
  2つの24bit pitchコピーの更新。
- 通常dispatchは4fdbに追加。補間へ進む場合は連続して実行し、
  両区間の命令数をdebtへ合成する。5060への独立した再入場はまだH8。

v1.21のハッシュ・ページ・割り込みマスク・trace禁止・正規voice条件を維持。
段階番号は0..22の偶数だけ受け入れ、それ以外は状態を変更せずH8へ戻す。
ROMテーブルとSRAMのみを扱い、確保・ロック・UI処理を追加しない。
12cycle刻みの周辺装置更新は維持し、RTSを省略しない。

## 検証

- 補間5,398ケース、段階遷移5,446ケースでレジスタ/SR/PC/SRAM/命令数がH8と一致。
- 全12段階とphaseの0/1/fffe/ffff、正負方向、rate=0、elapsedのwrap、
  phaseのovershoot、24bit上下端、ランダム入力を含む。
- 補間10種・段階遷移12種のガード拒否時に状態不変を確認。
- 起動、24音、controller/pressure、ポルタメント、新規Note、Note Offを通した
  379,676ステレオフレームがH8版とビット一致。
- 実再生で段階dispatch5,163回、うち補間99回。
  既存グライド/キャッシュ/変換/変調/LFO/level/cutoff/controller/TVAも照合。
- Mac Release Shared Code（arm64/x86_64、署名なし）ビルド成功。

```sh
cmake --build /tmp/sc55-firmware-oracle-build --target sc55-firmware-oracle -j 4
ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^native-tva$' -V
```

## 性能

製品adapter、macOS arm64 Release、48kHz/128samples、program48、24音保持、
120音声秒。直前のグライド追加済みバイナリを保存し、前/後/後/前で比較。
計測はビルドと並走せず、起動・ウォームアップを除外。

| 順序 | 前後 | スレッドCPU秒 |
|---|---|---:|
| 1 | 前 | 4.285555 |
| 2 | 後 | 4.304814 |
| 3 | 後 | 4.308065 |
| 4 | 前 | 4.318627 |

平均4.302091→4.306440秒で約0.10%増。前後の変動より小さく、
今回の追加で負荷が改善したとは言えない。全回underrun/MIDI drop=0、
peak=0.642068/RMS=0.121042。保持音では補間区間がほとんど動かない。
音色を跨いだ性能、アイドル、Logic、Windows/Linuxは未検証。

## 完全置換の残件

同日後続の[初期化・再入場の追加](NATIVE_PITCH_INIT_2026-09-09.md)で
以下の一部を実装。最新の初期化範囲と非マスク経路の制約は後続資料を参照。

本変更はH8完全置換の完了ではない。少なくとも次が通常経路に残る。

- ピッチEG初期化4f51..、再入場4f9e..、後段50cf..5175の一部。
- 他のEGの段階制御・初期化、MIDI/GS/SysEx処理、ボイス割当/解放の統合。
- 起動、タスクスケジューリング、ROMからのデータ取り込みと所有。
- パネル/LCDなどの制御と音源への影響を保持するUI分離。
- 他ROM版への対応。現在の通常経路の置換はv1.21専用。
- H8命令decodeなしで動く製品経路の成立と、機能・音声・タイミングの検証。

比較用のROM-freeヘルパーやnative previewが存在しても、上記が製品の
通常経路に接続され検証されるまでは「完全置換済み」と扱わない。
