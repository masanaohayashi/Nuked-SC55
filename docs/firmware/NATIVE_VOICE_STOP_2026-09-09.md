# PCM停止指示とタスク通知

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryStepVoiceStop`で33c9..33deを1命令ずつC++実行する。
PCM channel選択E03E、E018への00B6書き込み、A1D4への番号保存、
割り込みマスク解除、通知引数設定、TRAPA 2要求を扱う。
PCM書き込み前にPCを命令末尾へ進める点もH8と同じにする。
debtを増やさず、各命令の12cycle境界と既存のトラップ処理を維持。
PCMアクセスはBR=E0、channel選択は0..23のみ許可する。
v1.21/CP・DP=0/traceなしを要求。通知引数でr0が変わるため正規voice判定は要求しない。
RT確保・ロック・UI呼出しなし。

## 検証

7命令×384ケースでPC/SR/レジスタ/SRAM/ex_ignore/TRAPA 2 pendingを比較。
PCM書き込みのcycle/CP/PC/address/valueもH8と一致し、書き込み数1/2を明示確認。
割り込みマスク0..7とSR下位16通りを含む。
起動・24音・controller・portamento・Note Offの379,676フレームがビット一致。
実再生fallbackは4,497,894→4,497,810（84命令減）、ピッチ領域fallback=0。
Mac Release Shared Code（署名なし）のビルド成功。

今回はCPU時間の前後比較は省略。ホスト内・Windows/Linux・全音色は未検証。
通知先タスク本体、復帰後のキュー処理、非マスクEGなどは残り、完全置換は未完了。
