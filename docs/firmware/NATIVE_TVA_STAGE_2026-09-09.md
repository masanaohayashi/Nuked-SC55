# 音量EGの段階遷移・設定

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryDispatchTvaStage`で33f4..3449の段階判定、phase終端での次段階選択、
3組のrate/曲線/目標値設定、duration処理先選択をC++化。
テーブル6ac8/6ae0/6af8を参照し、許可した段階・分岐先以外は状態不変で拒否。
v1.21、正規voice、CP/DP/EP=0、traceなし、割り込みマスク7の条件を維持。
duration算出へ進む場合はphase・補間・出力符号化まで命令数を合成する。
344b/3477/3472へ進む処理はまだH8側で続ける。RT確保・ロック・UI呼出しなし。

## 検証

- 4,224ケースでPC/SR/全レジスタ/SRAM/命令数がH8と一致。
  段階0..12の通常/終端、14..22の終端、設定byteのランダム値を含む。
- 379,676ステレオフレームがビット一致。
- Mac Release Shared Code（署名なし）ビルド成功。
- 実再生fallbackは4,535,028→4,503,178、31,850命令減少。
  ピッチ領域fallback=0を維持。
- Release oracle `SC55_TVA_PROFILE=1 ctest -R '^native-tva$' -V`。

## 性能

macOS arm64 Release adapter、48kHz/128samples、24音保持120音声秒。
前/後/後/前CPU秒: 4.193172 / 4.161491 / 4.171109 / 4.168715。
平均4.180944→4.166300秒、約0.35%減。測定変動内で明確な改善とは扱わない。
全回peak=0.642068/RMS=0.121042、underrun/drop=0。
後の2回目にmaxBlock=9.2271msの壁時計外れ値あり。ホストのリアルタイム安全性の
証明ではなく、今回の性能比較はスレッドCPU時間を用いる。
ホスト内・アイドル・Windows/Linux・全音色は未検証。

短いdurationの後段、保持/遅延/終了、割り込み可能な音量EGは残る。
他のMIDI/GS・ボイス管理等も含め、完全置換は未完了。
