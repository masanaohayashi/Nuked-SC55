# 音量EGの線形・曲線補間

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryInterpolateTva`で35db..365dを一括C++計算する。
voice+96/+97の始点/終点、r3のphase、voice-8の曲線選択を用いて補間する。
曲線は6d10テーブルの隣接値補間と16bit乗算の切り捨てを維持。
上昇/下降/同値を扱い、全レジスタと最後のADD/SUBのフラグを保持する。
14/16/29/30命令の時間をnative_debtで消化し、365dの出力更新へ戻る。
v1.21、CP/DP/EP=0、正規voice、traceなし、割り込みマスク7が条件。
割り込み許可中の本区間はまだH8。新しい確保・ロック・UI呼出しなし。

## 検証

- 全256テーブル区間と小数境界、正逆方向・同値・ランダム入力の7,168ケースで
  PC/SR/全レジスタ/SRAM/命令数がH8と一致。
- 379,676ステレオフレームがビット一致。
- Mac Release Shared Code（署名なし）ビルド成功。
- 実再生の残存H8が5,143,894→4,998,480命令、145,414命令減少。
  ピッチ領域のfallback=0も維持。
- `SC55_TVA_PROFILE=1 ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^native-tva$' -V`

## 性能

macOS arm64 Release製品adapter、48kHz/128samples、program48の24音保持120音声秒。
前/後/後/前のCPU秒は4.109319 / 4.096973 / 4.140944 / 4.147749。
平均4.128534→4.118959秒、約0.23%減。ただし測定内変動より小さく、
明確な負荷改善は確認できない。全回peak=0.642068/RMS=0.121042、underrun/drop=0。
ホスト内・アイドル・Windows/Linux・全音色は未検証。

次は365d以降の出力値更新と、358e以降のphase/rate進行を優先する。
他のEG・MIDI/GS・ボイス管理等は残っており、完全置換は未完了。
