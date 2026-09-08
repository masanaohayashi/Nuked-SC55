# 累積変更のRelease CPU時間比較

TVA段階処理追加後に残していたRelease計測バイナリと、フィルター補間の
非マスク経路追加後の現状を比較した。間にTVA保持/復帰、ボイス処理、
フィルター処理を追加しており、今回の補間単体の効果ではない。

製品のNukedSC55Emulatorとbackendを使うローカル計測器。
48kHz/128samples、program48、24音、120秒相当をレンダリング。
起動・ウォームアップを除外しCLOCK_THREAD_CPUTIME_IDで測定。
ビルド/テスト終了後、旧→新→新→旧の順で逐次実行した。

|版|CPU時間（秒）|
|---|---:|
|旧1|4.080224|
|新1|4.023253|
|新2|4.053665|
|旧2|4.139771|

平均は旧4.109998秒、新4.038459秒、約1.74%短縮。
小幅であり、大幅改善や統計的に確定した性能差とは言えない。
全実行でpeak=.642068、RMS=.121042、H8cyclesDelta=2400053748、
underruns/dropped=0。Logic/GUI/プラグインラッパーを含む測定ではない。
この回はアイドル負荷を測定していない。

## ローカル証拠

- ログ: `/tmp/sc55-filter-series-perf.log`
- 旧: `/tmp/sc55-perf-uYIHTJ/sc55-perf-before-filter-series`
  SHA256 `c0b117b7df21ba939ff288b5591054de2e0f69617c6e1fdd0695e084158286a8`
- 新: `/tmp/sc55-perf-uYIHTJ/release/sc55-perf`
  SHA256 `f2c46bfaf5989fb1b62d4c517362c51a6e2e991f25636b1f191a5df3d1c374e5`
- コマンド: 各バイナリへROMディレクトリ、`chord 120`を渡す。

バイナリはローカル証拠で、ROMやバイナリをリポジトリへ追加していない。
