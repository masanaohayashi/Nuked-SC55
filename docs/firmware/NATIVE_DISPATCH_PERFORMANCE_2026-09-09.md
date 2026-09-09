# 単命令置換のディスパッチ費用

ボイス/LFOからフィルター初期化まで追加した現状を、以前の318c..3210までの
Release測定器と比較した。最初の比較では24音で8.85%、idleで7.43%遅かった。
単命令置換の範囲判定が対象外のH8フォールバックにも積み上がっていた。

MCU_Stepの既存switchのdefault入口で、CP!=0またはPCが312d..5c1d外なら
判定列を省略する。さらに3985..4440の変調/フィルター初期化の判定列を、
その領域内だけに限定した。各helperのガード、実行命令、周辺回路更新は変更しない。
default内の全対象範囲は外側条件に含まれ、既存の明示caseは条件の対象外。

## 測定

既存NukedSC55Emulator/backendのRelease測定器。48kHz/128samples、120秒相当。
起動・ウォームアップを除外したCLOCK_THREAD_CPUTIME_ID。
各条件で旧→新→新→旧を逐次実行し、測定中はビルド/テストを並走させない。
SC55_NONATIVE、NUKED_SC55_NATIVE_PREVIEW、SC55_FXSIMはunset。

|比較|条件|旧1|新1|新2|旧2|新旧平均比|
|---|---|---:|---:|---:|---:|---:|
|判定改善前|24音|4.047623|4.347557|4.590439|4.163588|+8.85%|
|判定改善前|idle|2.158532|2.317089|2.303886|2.142877|+7.43%|
|判定改善後|24音|4.122320|4.003833|3.992417|4.114373|-2.92%|
|判定改善後|idle|2.153656|2.077596|2.040588|2.127238|-3.80%|

単位はCPU秒。旧版に対する累積変更全体の比較で、個々の置換の効果ではない。
各2回の小標本であり、大幅改善や全パッチでの改善を示すものではない。
Logic/UI/ラッパーは含まない。oneCore値とLogicのメーターは直接比較しない。
全実行でcyclesDelta=2400053748、underruns/dropped=0。
24音のpeak=.642068/rms=.121042、idleは0（集計値はビット一致の証明ではない）。

## 証拠

- 旧バイナリ: `/tmp/sc55-perf-uYIHTJ/sc55-perf-before-voice-lfo-setup`
  SHA256 `720a4c52f28bb0e26f962281f9a6e21a1acc4570773da55459ee6b479b059ea5`
- 改善前バイナリSHA256: `051bf2ddfbfb533e6cbb551be64ebfc78be0985f2ffcc909e1b19a0523dbe94a`
- 改善後: `/tmp/sc55-perf-uYIHTJ/release/sc55-perf`
  SHA256 `8e79bce710653f297ccf13437d18d32f39c07d30352a034fbd72535d1a90aa66`
- 初回ログ: `/tmp/sc55-voice-lfo-setup-perf.log`
- 外側条件のみの中間比較: `/tmp/sc55-dispatch-envelope-perf.log`
- 最終ログ: `/tmp/sc55-dispatch-group-perf.log`

## 正しさの確認

- native-tva成功、379,676ステレオフレームがビット一致。
- H8 fallbackは2,852,488で判定改善前と同じ。
- Mac Release Shared Codeビルド成功（署名なし）。
- ログ: `/tmp/sc55-dispatch-group-oracle-test.log`、`/tmp/sc55-dispatch-group-xcode.log`。

引き続き未置換の経路が多い。命令数削減を性能改善と同一視せず、
まとまった追加の後に同条件のCPU時間を確認する。
