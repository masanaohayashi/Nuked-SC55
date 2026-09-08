# 通常経路のLFO置換 — 2026-09-08

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

## 範囲

`TryAdvanceLfo` を通常MCU dispatchの3b26/3b2cへ接続。
2系統のLFOの遅延、attack、符号付き3深度の合成、rateテーブルと
modifierの飽和、サイン波の位相更新・テーブル補間をC++化した。
サイン波は3c30のRTSに戻る。他の6波形は周波数計算後にH8へ戻す。
特にランダム波形のPCM readbackは今回移動していない。

v1.21 ROMハッシュによる有効化、ページ、割り込みマスク、trace禁止、
24 voiceの正規アドレス、LFO block、rate/shapeの範囲でガードする。
SRAMだけを更新し、debtで元の命令数ぶん周辺装置を進める。
JUCEの音声処理スレッド構成を変えず、確保・ロック・ログは追加しない。

## 検証

- 4,096ケース：両entry、全7shape、遅延/attack境界値、ランダム深度・
  rate・modifier・位相。全汎用レジスタ、SR、PC、SRAM、命令数が一致。
- 不適格入力11種類は状態を変更せずfallback。
- 起動→24音→controller変更→Note Offの347,676ステレオフレームが
  H8版とビット一致。LFOの通常dispatch 8,188回、327,304命令を置換。
- TVA/controller/cutoff/levelの既存差分検証も通過。
- Mac Release Shared Code（arm64/x86_64）ビルド成功。

再実行は既存の `native-tva` CTestでLFO検証も行う。

## 性能

[前回](NATIVE_LEVEL_2026-09-08.md)と同じ製品adapterハーネス。
macOS arm64 Release、48kHz/128 samples、program48、24音保持、
120音声秒、起動・ウォームアップ除外。今回の直前版との前/後/後/前比較。

| 順序 | 追加前後 | スレッドCPU秒 |
|---|---|---:|
| 1 | 前 | 4.530251 |
| 2 | 後 | 4.455258 |
| 3 | 後 | 4.423571 |
| 4 | 前 | 4.515526 |

平均4.522889 → 4.439415 CPU秒、追加分で約1.85%削減。
peak=0.642068、RMS=0.121042、underrun/MIDI dropは全実行0。
効果は小幅。過去の改善率とは加算しない。
Logic内CPUメーター、全音色、Windows/Linuxは今回未計測。
