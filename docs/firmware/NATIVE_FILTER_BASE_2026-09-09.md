# フィルター基準値の補正

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryAdjustFilterBase`で4662..46c3をC++化。voice+163の基準値に
part構造体+18のcontroller（中心64）を反映し、8bitの折り返しと
符号判定を再現する。増加側はvoice+162のbit2で無効化され、
有効時の増分上限は16。補正値を上位byteへ移してR5のEG値に加算し、
符号に応じた飽和後にvoice+34へ保存する。

続いてvoice+136の符号付きオフセットを反映。負側は減算borrowで0へ、
正側は加算overflowで32767へ飽和する。R5は補正後の値、voice+34は
オフセット適用前の値であり、両者を同じ値にまとめない。

v1.21、CP/DP/EP=0、mask7、traceなし、正規voice、SRAM上のpartを要求。
元のレジスタ・SR・命令数を維持し、既存audio経路に確保・ロック・UI処理を追加しない。
46c3のLFO準備手前で停止するため、PCMバス書き込みはこの置換に含まれない。

## 検証

- 26,624ケースでH8とPC/SR/レジスタ/SRAM/命令数が一致。
  controller全256値、bit2有無、正負境界・最大値と乱数を含む。
- Release oracle native-tvaで379,676ステレオフレームがビット一致。
- H8 fallback: 3,969,629→3,881,858（87,771命令減少）。
- Mac Release Shared Codeを署名なしでビルド成功。

CPU時間・Logic内負荷・他ROM/全音色は今回未検証。
LFO合成、後段設定、PCM出力には未置換部分が残っている。
