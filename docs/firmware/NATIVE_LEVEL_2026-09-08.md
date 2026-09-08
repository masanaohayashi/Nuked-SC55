# 通常経路の音量合成置換とMK1 RAMアクセス — 2026-09-08

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

## 変更

`TryComposeLevelV121` で309b..312aの音量合成を通常再生へ接続した。
パート音量・expression・master・係数の合成、bias、2系統の変調、
二乗カーブ、ゼロ/飽和の各出口を処理する。
以前の309b補助実装には機種条件・出口状態・命令数の問題があるため、
単純にMK1で有効化する変更ではなく、v1.21用のレジスタ一致経路を追加した。
従来の他機種向け呼び出し条件は変更していない。

ROMハッシュ、ページ、割り込みマスク、voice/pointer/stack範囲を確認。
変調のBSRが残す310b/3117のスタック書き込みも再現し、外側のRTSはH8に残す。
stackは入力領域と重ならないSRAMまたは有効化された内蔵RAMに限定。
命令数は既存debtで消化するため、周辺装置の更新周期は飛ばさない。
今回の経路には共有の可変scratch、確保、ロック、ログを追加していない。

また、MCU_Read/Writeの既存fast pathがMK1を除外していた点を修正。
Slow実装を確認すると、8000..dfffのSRAMとRAMCR有効時fb80..ff7fへの
書き込みはMK1でも同一の配列アクセスだった。PCM/LCD/デバイス範囲は
従来どおりSlowへ渡す。以下の性能値は音量合成とRAM改善の両方を含む。

## 検証

- 4,096ケースで全汎用レジスタ/SR/復帰位置/RAM/SRAM/命令数をH8と比較。
  343ケースの変調境界値組み合わせ（-32768同士、cap境界、深さの符号など）、
  ランダム値、ゼロ、飽和、2種類のstack領域を含む。
- 不適格入力12種類はメモリを書き換えずfallback。
- MK1 SRAM全24,576アドレスのread/write、内蔵RAM全1,024アドレスのwriteを
  Slow実装と比較。変更したfast pathの範囲を確認。
- 通常起動→24音→controller/pressure変更→Note Offの347,676ステレオ
  フレームがH8経路とビット一致。音量合成4,083回、298,032命令を置換。
- 既存TVA/controller/cutoffの比較も通過。
- Mac Release Shared Code（arm64/x86_64）ビルド成功。

## 性能（前回のcutoff置換済み → 今回追加後）

製品adapter計測ハーネス・手順は
[controller計測](NATIVE_CONTROLLERS_2026-09-08.md)と同じ。
macOS arm64 Release、48kHz/128 samples、program48、24音保持、
起動・ウォームアップ除外。120音声秒の計測スレッドCPU時間、前/後/後/前の順。

| 順序 | 追加前後 | CPU秒 |
|---|---|---:|
| 1 | 前 | 4.749850 |
| 2 | 後 | 4.437022 |
| 3 | 後 | 4.422811 |
| 4 | 前 | 4.765638 |

平均4.757744 → 4.429917 CPU秒、**今回さらに6.89%削減**。
CPU/音声時間比は約3.965% → 3.692%。
全実行でpeak=0.642068、RMS=0.121042、underrun/MIDI dropなし。

60秒アイドルの同順序CPU秒は1.002527 / 0.999807 / 0.999193 / 1.007279。
平均約0.54%減に留まり、アイドルの明確な改善とは扱わない。
前回資料と絶対時間が違うため、今回の同時点の前後比較を使う。
過去の改善率を単純加算しない。Logic内のCPU値・Debug・全音色・
Windows/Linuxは今回未計測。

## 再実行

ROM指定は [TVAのconfigure手順](NATIVE_TVA_2026-09-08.md#再実行)と共通。

```sh
cmake --build /tmp/sc55-firmware-oracle-build --target sc55-firmware-oracle -j 4
ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^native-tva$' -V
```

既存テスト名でTVA/controller/cutoff/levelと通常再生の差分検証を実行する。
