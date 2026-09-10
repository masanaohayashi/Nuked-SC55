# ボイス補正係数の適用

TryStepVoicePatchApplyでSC-55 v1.21の4ada..4c50、154命令を単命令置換。
5ブロックは76バイト間隔で同じ演算を行うため、PCを正規化して実装を共有する。
実際のPC・分岐先は元の各ブロックの値を保ち、命令や割り込み境界をまとめない。

| 入力/上位byte | 下位word保存先 |
| --- | --- |
| voice[106] | voice[112] |
| voice[107] | voice[114] |
| voice[108] | voice[116] |
| voice[109] | voice[118] |
| voice[111] | voice[122] |

最初はR2の係数を用い、残り4ブロックはvoice[-56]から再ロードする。
入力byteの符号に応じて絶対値で79f2テーブルを参照し、R2と乗算。
下位word倍化と上位wordのADDX、byte転送とSWAPで中間16ビットを抽出する。
これをvoice[40]/[60]の基準値へ桁上げ・桁借り付きで加減算。
負側の上位byteが負なら上下位をゼロへ制限し、上表の保存先へ書く。

ADDXの累積Z、SUBXの通常Z、byteレジスタ上位保持を維持。
テーブルindexは0..255に限定し、それ以外はH8経路へ戻す。
RT経路に動的確保・ロック・UI処理は追加しない。
全体の置き換えは未完了。続く4c53以降は本ヘルパーの対象外。

## 検証

- 全154命令×384＝59,136境界でPC/SR/レジスタ/DP/EP/ex_ignore/SRAM/PCM状態一致。
- 379,676ステレオフレームがビット一致。
- 同じ再生のH8 fallbackは2,848,935→2,846,835。
- Mac Release Shared Codeビルド成功（署名なし）。

ログ: /tmp/sc55-voice-patch-apply-test.log、/tmp/sc55-voice-patch-apply-xcode.log。
今回のCPU改善量とLogicでの動作は未測定。
