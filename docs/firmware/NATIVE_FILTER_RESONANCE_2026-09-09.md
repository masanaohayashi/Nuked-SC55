# フィルター後段のレゾナンス設定

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryAdjustFilterResonance`で46dd..473cをC++化。
まずLFO合成済みR5をvoice+34へ保存する。voice+103の基準値を
part構造体+19のcontrollerで調整し、voice+105の上限と比較する。
減算後の8bit符号判定、加算の折り返しも維持する。

目標が現在値voice+104と違う場合のみ現在値を1増減し、
voice+36を切り上げた上位byteで7714テーブルの制限値を引く。
加算が16bitからあふれるとindex=255。現在値が制限を超えたら制限値へ戻す。
目標と現在値が同じ場合はテーブルも保存も通らない。

v1.21、CP/DP/EP=0、mask7、traceなし、正規voice、SRAM内partを要求。
レジスタの上位byte、SR、命令数/debtを維持する。
既存audio経路で動作し、確保・ロック・I/O・UI呼び出しを追加しない。
473cから先は既存カットオフ変換へ戻り、PCM書き込み時刻を変更しない。

## 検証

- 26,624ケースでH8とPC/SR/レジスタ/SRAM/命令数が一致。
  controller全256値、現在値/上限/切り上げの境界、乱数を含む。
- Release oracleで379,676ステレオフレームがビット一致。
- H8 fallback: 3,721,792→3,644,347（77,445命令減少）。
- Mac Release Shared Codeを署名なしでビルド成功。

CPU時間・Logic内負荷は今回未測定。命令数減少をCPU改善率とはしない。
フィルター呼び出し接続部やPCM出力にはH8が残る。
