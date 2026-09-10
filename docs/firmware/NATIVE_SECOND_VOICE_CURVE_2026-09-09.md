# 第2ボイス補正カーブ

TryStepSecondVoiceCurveでSC-55 v1.21の4d26..4d87、39命令を単命令置換。
パッチbyte31を倍化してページ3のdd62テーブルを参照し、c8fc[R1]を加えた
間接アドレスからbyteを読み出す。パッチbyte33と64との差で符号を選択し、
128中心の補正、679aの係数との乗算・倍化・SWAP、67c6参照を経て
voice[-52]へ保存する。ゼロ差等の迂回経路は256を保存。

前段と異なり、テーブルindexはR2を使い、byte31の下位4bitへの制限はない。
4d33/4d3fは4バイトのLDC.W。実バイトから確認した正しい後続境界は
4d37/4d3b/4d3d/4d3f/4d43/4d45/4d48。

dd62は偶数0..510、679aは0..255、67c6は偶数0..510のindexを対象とする。
パッチ参照はEP=0のROM/SRAMおよびEP=1..4のROM。範囲外は既存H8へ戻す。
NZVC、byte上位保持、LDCのex_ignore、命令ごとの割り込み境界を維持。
RT経路の動的確保・ロック・UI呼び出しは追加しない。

全体の置き換えは未完了。次の4d8c以降はこのヘルパーの対象外。

## 検証

- 全39命令×384＝14,976境界でPC/SR/レジスタ/DP/EP/ex_ignore/SRAM一致。
- 379,676ステレオフレームがビット一致。
- 同じ再生のH8 fallbackは2,845,760→2,845,385。
- Mac Release Shared Codeビルド成功（署名なし）。

ログ: /tmp/sc55-second-voice-curve-test.log、/tmp/sc55-second-voice-curve-xcode.log。
今回のCPU改善量とLogicでの動作は未測定。
