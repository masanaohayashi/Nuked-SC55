# 音量EGの保持・短時間更新

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TrySetTvaImmediate`で3477の保持、36c6の即時更新、36aeの短時間更新を追加。
36aeは6b06の指数テーブルを読み、出力値を設定して3666へ合流する。
`TryEncodeTvaTarget`を3666の継続入口に対応させ、既存r6の前回値とr3の指数を利用。
指数は0..10を許可。通常365d入口は従来通り指数7を設定する。
stage/phaseからの合成dispatchにも接続。正規v1.21 voice、CP/DP/EP=0、
traceなし、割り込みマスク7の条件を維持する。RT確保・ロック・UI呼出しなし。

## 検証

- 即時/保持/短時間更新6,912ケースでPC/SR/全レジスタ/SRAM/命令数がH8と一致。
  全256目標byteと0..8の短時間テーブル入力を含む。
- 3666継続の出力符号化4,352ケースで同じ状態・命令数を照合（指数0..10）。
- 起動・24音・controller・portamento・Note Offの379,676フレームがビット一致。
- この演奏テストの残存H8数は4,503,178で変わらない。今回追加したマスク条件の
  経路が実演奏で使用された証拠にはならず、単体一致と実経路到達は区別する。
- Release oracle `SC55_TVA_PROFILE=1 ctest -R '^native-tva$' -V`。
- Mac Release Shared Code（署名なし）ビルド成功。

今回はCPU時間の前後比較は行っていない。ホスト内・Windows/Linux・全音色も未検証。
遅延・終了や非マスク経路は残り、完全置換は未完了。
