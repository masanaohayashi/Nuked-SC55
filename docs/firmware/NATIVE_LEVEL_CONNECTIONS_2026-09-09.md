# 音量LFOの接続と最終変換

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryStepLevelConnections`で30ff..312aと30eaの18命令を単命令C++化。
2組の引数はvoice[-122,+142,-96]と[-88,+150,-62]。
BSRは各2byte命令の戻りアドレスを既存スタックヘルパーで保存する。
最終変換はR4二乗の上位wordに0x208を掛け、上位word255を境に
バイト入れ替えまたはffffへ飽和する。30ea/3126/312aのRTSも扱う。

v1.21、CP/DP=0、正規voice、traceなしを要求し、mask/EPに依存しない。
debtを追加せず、各命令のレジスタ/SR/スタック状態を公開する。
音声スレッドを移動せず、確保・ロック・UI呼び出しを追加しない。

## 検証

- 18命令×384ケース＝6,912命令境界でH8とPC/SR/レジスタ/SRAMが一致。
  mask0..7、SR下位16通り、EP=1、正規voice24個を含む。
- Release oracleで379,676ステレオフレームがビット一致。
- H8 fallback: 3,574,541→3,569,004（5,537命令減少）。
- Mac Release Shared Codeを署名なしでビルド成功。

CPU時間・Logic内負荷は今回未測定。音量合成前段にはH8が残る。
