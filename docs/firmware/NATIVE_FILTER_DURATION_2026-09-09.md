# フィルターEGの時間計算

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryComputeFilterDuration`で44a3/44ff/4564から45b6までをC++化。
アタック・ディケイ・リリースのcontroller補正、6f12の時間テーブル、
2段階の倍率演算と飽和を再現する。アタックはvoice+162のbit4が
立っている場合だけcontroller補正を行う。

- 基準レート: voice+74。
- controller: voice+46が指す構造体の20/21/22。
- 第1倍率: attack/decayは-44、releaseは-42。
- 第2倍率: attackは-40、decay/releaseは-38。
- 8bit演算の折り返し、積の上位wordが255以上の場合の飽和を保持。

v1.21、CP/DP/EP=0、mask7、traceなし、正規voiceを要求。
補正を行う場合の参照先はSRAMに限定し、範囲外は状態変更前にH8へ戻す。
命令数/debt、SR、レジスタを保ち、既存audio経路に確保・ロック・UI処理を追加しない。

## 検証

- 21,504ケースでH8とPC/SR/レジスタ/SRAM/命令数が一致。
  controller 0..127、倍率のゼロ・符号境界・最大値、乱数値、attack補正無効を含む。
- Release oracle native-tvaで379,676ステレオフレームがビット一致。
- 再生中のH8 fallback: 4,195,814→4,090,892、104,922命令減少。
- Mac Release Shared Codeを署名なしでビルド成功。

CPU時間とLogic内負荷は今回未測定。命令数削減率をCPU削減率とはしない。
次の45b6以降の位相更新・補間処理はまだH8に残る。
