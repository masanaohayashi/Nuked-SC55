# 音量EGのduration算出

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryComputeTvaDuration`で348c/34e2/3536から358eまでの3系統をC++化。
voice+46の参照先+20/+21/+22のcontroller、voice+79の基準値、6f12テーブル、
voice-34/-32とvoice-30/-28の2段倍率を使う。各乗算の8bitシフトと飽和を保持。
3536で現在値がゼロなら3472へ戻す。参照元範囲とテーブル添字は変更前に確認する。
正規v1.21 voice、CP/DP/EP=0、traceなし、割り込みマスク7が条件。
算出後はphase進行・補間・出力更新まで命令数を合成する。
新しいRT確保・ロック・UI呼出しなし。

## 検証

- 全128controller、基準値の境界、倍率の境界・ランダムの21,504ケースで
  PC/SR/全レジスタ/SRAM/命令数がH8と一致。
- 379,676ステレオフレームがビット一致。
- Mac Release Shared Code（署名なし）ビルド成功。
- 演奏中の残存H8は4,665,150→4,535,028、130,122命令削減。
- Release oracle `SC55_TVA_PROFILE=1 ctest -R '^native-tva$' -V`。

## 性能

macOS arm64 Release adapter、48kHz/128samples、24音保持120音声秒。
前/後/後/前CPU秒: 4.076506 / 4.080064 / 4.113871 / 4.144742。
平均4.110624→4.096968秒、約0.33%減。ただし測定変動内で、明確な改善は未確認。
全回peak=0.642068/RMS=0.121042、underrun/drop=0。
ホスト内・アイドル・Windows/Linux・全音色は未検証。

段階遷移・設定、durationが短い場合の後段、非マスク経路等は残る。
MIDI/GS・ボイス管理などを含む完全置換は未完了。
