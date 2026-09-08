# 音量EGの遅延・復帰

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryAdvanceTvaDelay`で344b..346bの遅延カウンターを一括C++化。
voice+14と+16の加算がcarryしたとき、voice+0/+2/+4の3段階を2に設定する。
voice番号の範囲を確認し、AC42+番号のactivity byteをクリアする。
v1.21/正規voice/CP・DP・EP=0/traceなし/割り込みマスク7が条件。

元ASMは3453以降のMOV.W即値8bitを5byteと誤認し、以降の境界がずれていた。
ROMの実バイトとMCUデコーダでは3453/3457/345bが各4byteで、345fがcounter保存。
本実装は実デコーダと照合した（既存ASM全体の再生成はしていない）。

`TryStepTvaExit`は346b/3472のスタック加算、346dのマスク解除、3474の分岐、
3471/348b/36a7/36ad/36daのRTSを1命令単位で実行する。
マスク解除は既存ControlRegisterWriteとex_ignore=1を用い、debtを増やさない。
スタックアクセスは既存ヘルパーに委譲。RT確保・ロック・UI呼出しなし。

## 検証

- 遅延4,177ケースでPC/SR/レジスタ/SRAM/命令数が一致。
- 復帰9命令×384ケースで各命令後のPC/SR/レジスタ/SRAM/ex_ignoreが一致。
  マスク0..7、SR下位16通り、スタック加算の符号・wrap境界を含む。
  RTSのスタックは正常な偶数SRAMアドレス。異常スタック例外の専用検証は未実施。
- 起動・24音・controller・portamento・Note Offの379,676フレームがビット一致。
- Mac Release Shared Code（署名なし）ビルド成功。
- 遅延追加のみでは残存H8数は変わらず、復帰追加後4,503,178→4,497,966命令。
  ピッチ領域fallback=0を維持。

今回はCPU時間の比較を省略。ホスト内・Windows/Linux・全音色は未検証。
非マスク音量EG・ボイス解放の本体等は残り、完全置換は未完了。
