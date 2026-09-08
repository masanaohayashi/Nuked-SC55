# 通常H8経路のTVA部分置換 — 2026-09-08

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

## 実装範囲

SC-55 v1.21 の内部ROM `00:36ee..3734` を
`mcu_native::TryAdvanceTva` に置換。音量ランプ加算・飽和、レベルとの
乗算、前回値との差分、PCM向けTVAコマンド生成まで。
通常の `MCU_Step` から呼ぶため、native preview の有効化は不要。
MIDI受付、EGの段階遷移、直前の音量合成 `309b`、後続のpan/effect、
PCMへの実書き込みは引き続きH8。完全C++化ではない。

ROMロード時にMK1機種と内部ROM全体のSHA-256を照合する。
`SC55_NONATIVE=1` を起動前に指定すればこの置換を無効化できる。
異なるROM、ページ、非SRAMのvoiceアドレス、割り込み非マスク状態、
trace状態は元の命令実行へ戻す。既存の309b用フックは変更していない。

音声処理中のROMハッシュ計算、環境変数参照、メモリ確保は追加しない。
元の命令数分は `native_debt` として毎ステップ消化し、PCM・タイマー・
UARTのクロックをまとめて飛ばさない。この処理は `SC55_BULK` に従わない。
リセット時にはdebtを消去する。

## 検証

- 実ROMのH8命令と、置換直後3734で全汎用レジスタ・SR・SRAM・命令数を比較。
  境界値294ケース＋固定seedランダム4096ケース、一致。
- 不適格な実行状態、改変ROM拒否、ROM再ロード、debtのリセットを検証。
- 通常起動後、program 48、note 36〜59/velocity 100、CC7変更、Note Off。
  347,676ステレオPCMフレームがビット一致。通常のMCU経路で4,085回置換。
  テストは音声が無音のみでないことも確認する。
- macOS Release Shared Codeのビルド成功（arm64/x86_64）。
  Logic内、Windows/Linuxビルド、全音色/GSの網羅検証は今回行っていない。

Releaseの単発headless計測では、起動後の演奏〜release処理が
H8 0.259223秒、置換あり0.257737秒（約0.57%減）。
短時間のwall-clock計測で、測定順・ホスト負荷・テスト用観測も含む。
**誤差を超える改善やLogic CPUメーターの低下を立証した値ではない。**
大きな改善には、残る音量合成・EG/LFO等のより広い置換が必要。

## 再実行

ROMは配布・コミットしない。ローカルのv1.21 ROMディレクトリを指定する。

```sh
cmake -S tools/firmware-oracle -B /tmp/sc55-firmware-oracle-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSC55_NATIVE_TVA_TEST_DIRECTORY="/path/to/SC-55 v1.21"
cmake --build /tmp/sc55-firmware-oracle-build --target sc55-firmware-oracle -j 4
ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^native-tva$' -V
```
