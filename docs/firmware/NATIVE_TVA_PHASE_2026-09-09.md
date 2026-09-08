# 音量EGのphase/rate進行と後段統合

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryAdvanceTvaPhase`で358e..35db/365dを一括C++化。
durationからのrate算出、elapsed+残余時間の16bit wrap、phase積算、
上限到達時の余剰時間持ち越し、目標値への到達を扱う。
duration=0/1..8ではそれぞれ36c6/36aeへ戻し、特殊な後段はH8に残す。
正規v1.21 voice、CP/DP/EP=0、traceなし、割り込みマスク7の条件を維持。
補間と出力符号化へ進む場合は命令数を合成し、周辺装置の時間を保つ。
確保・ロック・UI呼出しなし。

## 検証

- 9,664ケースでPC/SR/全レジスタ/SRAM/命令数がH8と一致。
  durationの0/8/9境界、phase終端、elapsed/residual wrapを含む。
- 379,676ステレオフレームがビット一致。
- Mac Release Shared Code（署名なし）ビルド成功。
- 実再生H8 fallbackは4,763,697→4,665,150、98,547命令減少。
  ピッチ領域fallback=0を維持。
- Release oracle `SC55_TVA_PROFILE=1 ctest -R '^native-tva$' -V`。

## 性能

macOS arm64 Release adapter、48kHz/128samples、24音保持120音声秒。
比較元は出力符号化追加前の`sc55-perf-before-tva-target`。
したがって出力符号化と今回のphase統合の合計を比較しており、今回単独の効果ではない。
前/後/後/前CPU秒: 4.089797 / 4.139515 / 4.136933 / 4.153314。
平均4.121556→4.138224秒、約0.40%増で改善は未確認。
全回peak=0.642068/RMS=0.121042、underrun/drop=0。
ホスト内・アイドル・Windows/Linux・全音色は未検証。

音量EGの段階選択・duration算出・短いdurationの後段、割り込み可能な経路は残る。
他のMIDI/GS/ボイス管理等も含め、完全置換は未完了。
