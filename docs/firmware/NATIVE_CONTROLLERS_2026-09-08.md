# 通常経路のコントローラ計算置換 — 2026-09-08

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

## 優先順位と実装

通常起動→program48/24音→controller変更→releaseを実行し、H8のPC別命令数を集計。
`00:5c20..5ff4` は852,433 / 6,666,668命令（12.79%）。
256-byte単位の上位は4700（5.62%）、3100（5.18%）、3600（4.52%）だが、
連続したコントローラ計算全体はそれらの区画より大きい。命令割合はCPU時間割合ではない。

この連続区間を `TryPrepareControllers` として通常のMCU実行に接続した。
全11出力（ピッチ・EGオフセット、音量バイアス、2組のLFO rate/depth）を計算する。
既存 `PrepareVoiceControllers` の算術を小さな共有関数 `ScaleVoiceController` に抽出し、
比較用native playerとも共有。通常再生にpreview設定は不要。

- MK1/v1.21の既存ROM全体ハッシュ判定を共有（フラグ名を `native_v121_enabled` に変更）。
- PC/ページ/割り込みマスク/trace、slot・part・key・voiceアドレスを確認。不適格ならH8を継続。
- firmwareと同じ順序でSRAMに書き込み、scratch領域、全汎用レジスタ、SR、命令数を保持。
- `5ff4` のRTSはH8のまま。置換命令分の周辺装置クロックも既存のdebtで1ステップずつ進める。
- 新規の音声スレッド処理に確保・ロック・ログ・環境変数参照を加えていない。
  ROMロード時の `SC55_NONATIVE` でTVAと今回の置換を無効化できる。

## 正しさ

既存 `native-tva` テストに、今回のcontroller比較を追加。

- 1,024入力の全レジスタ/SR/SRAM/命令数が実ROMのH8と一致。
  通常MIDI範囲、ゼロ、負値、飽和、16-bit加算のwrapを含む。
- fallbackガードでSRAMを変更しないことを確認。
- Note On/Off、CC1/7/16/17、channel pressure、poly pressureを通常UART経路で投入。
  347,676ステレオPCMフレームがビット一致。4,094呼び出しで847,458命令を置換。
- 既存TVAの4,390ケースも通過。
- Mac Release Shared Code（arm64/x86_64）をビルド。生成プロジェクトは変更していない。

## 性能（前回commitのTVA置換あり → 今回のcontroller置換追加）

既存の製品adapter計測ハーネス `sc55-perf-uYIHTJ` を再ビルドして測定。
ソース・測定方式は [CPU計測資料](CPU_PROFILE_2026-09-07.md) とそのローカル証拠アーカイブを参照。
macOS arm64 Release、48kHz/128 samples、program48、note36〜59、velocity100、
PCM voice mask `00ffffff`。起動とウォームアップを除き、計測スレッドのCPU時間を採取。
今回のH8-PC集計コードはこのハーネスには入っていない。

| 順序 | 120秒の24音演奏 | CPU秒 |
|---|---|---:|
| 1 | 変更前 | 7.668521 |
| 2 | 変更後 | 7.369028 |
| 3 | 変更後 | 7.364051 |
| 4 | 変更前 | 7.653681 |

平均7.661101 → 7.366540 CPU秒、**3.84%削減**。
CPU/音声時間比は6.384% → 6.139%（約0.245ポイント減）。
全実行でpeak=0.642068、RMS=0.121042、underrun/MIDI dropなし。
60秒アイドルは1.503635 → 1.503812 CPU秒で改善なし。
過去9/7の絶対値とは測定時の環境が違うため、今回の前後比較を使う。

Logicメーターの改善率ではない。ホスト・UI・全音色/GS・Windows/Linuxは今回未検証。
処理周期、MIDIタイミングや精度を落として得た削減ではなく、命令解釈を置換した効果。
完全C++化や最終CPU目標の達成ではない。

## 再実行

[TVAのROM指定・configure手順](NATIVE_TVA_2026-09-08.md#再実行)と同じビルドを使う。

```sh
cmake --build /tmp/sc55-firmware-oracle-build --target sc55-firmware-oracle -j 4
ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^native-tva$' -V
SC55_TVA_PROFILE=1 ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^native-tva$' -V
```

最後のコマンドは調査用の命令分布。instrumentationありの時間を性能比較に使わない。
ROMや生成済みバイナリはコミットしない。
