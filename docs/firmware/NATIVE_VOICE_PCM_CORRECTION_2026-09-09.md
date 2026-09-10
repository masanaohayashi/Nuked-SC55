# PCM由来値によるボイス追加補正

TryStepVoicePcmCorrectionでSC-55 v1.21の4c53..4cb7、38命令を単命令置換。

voice[-60]に退避済みの符号付きPCM由来値を、パッチbyte17で乗算。
128加算・SWAP・下位byteの10倍を経てvoice[106]/[112]へ加減算する。
負側では上位byteが負なら上下位をゼロへ制限する。
結果をvoice[106]/[112]、[45]/[70]、[44]/[68]へ保存。

続いてvoice[106]と[107]を比較し、等しい場合のみ[112]と[114]を比較する。
比較結果のCによってvoice[-3]を0または2に設定し、ANDCで割り込みマスクを解除。
CMPはレジスタを変えず、MOV即値によるNZV更新とC保持も維持する。

単命令ごとのPC、NZVC、byte上位保持、ADDXの累積Z、SUBXの通常Z、
ANDCによるex_ignoreを保存。RT経路の動的確保・ロック・UI呼び出しは追加しない。
パッチ参照はEP=0のROM/SRAMおよびEP=1..4のROMに限定し、範囲外はH8へ戻す。

この段階では4cbb以降は未置換。全体のC++化は継続中。

## 検証

- 全38命令×384＝14,592境界でPC/SR/レジスタ/DP/EP/ex_ignore/SRAM/PCM状態一致。
- 379,676ステレオフレームがビット一致。
- 同じ再生のH8 fallbackは2,846,835→2,846,160。
- Mac Release Shared Codeビルド成功（署名なし）。

ログ: /tmp/sc55-voice-pcm-correction-test.log、/tmp/sc55-voice-pcm-correction-xcode.log。
今回のCPU改善量とLogicでの動作は未測定。
