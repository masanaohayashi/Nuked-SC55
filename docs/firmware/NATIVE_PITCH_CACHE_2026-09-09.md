# 通常経路のピッチ補正キャッシュ — 2026-09-09

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

## 実装

`mcu_native::TryCorrectPitch` が v1.21 の 527c..5367 を置換する。
source byte が変化した場合だけ、24bit referenceから81000を引き、
ROMテーブル・オクターブシフト・除算で補正値を更新する。
referenceだけの変更ではキャッシュを無効化しない、元の挙動を維持。
DIVXUの16bit商オーバーフロー時のレジスタ保持と符号付き飽和、
補正値を加えた最終PCMレートの上下限も再現する。

既存51e7の変換に連続して接続し、毎命令のswitchに判定項目を増やさない。
両区間の命令数を加算してdebtに保持し、12cycles刻みの周辺装置更新を維持。
RTSはH8に残す。補正区間はnoinlineとして命令ループの肥大化を避ける。
単独で527cへ入る経路は引き続きH8。

v1.21の既存ROMハッシュ、ページ、割り込みマスク、trace禁止、24voiceの
正規アドレスに加え、source参照がSRAM内であることを副作用前に確認する。
不適合なら補正区間はH8へ戻す。メモリ確保・ロック・UI呼び出しは追加しない。
JUCEのスレッド構成、生成プロジェクト、ROM、リリース成果物は変更しない。

## 一致検証

- 8,228ケースで全汎用レジスタ・SR・PC・SRAM・命令数をH8実行と比較。
- 全256 source値、hit/miss、24bit境界、極端なシフト、商の飽和、
  最終加算の境界、2,048ランダム入力を含む。
- 12種のガード拒否でレジスタ・SRAM・PC・SR・debtが変化しないことを確認。
- 既存native-tva内のTVA/controller/cutoff/level/LFO/pitch比較も成功。
- 起動→24音→controller/pressure→Note Offの347,676ステレオフレームがビット一致。
- 補正は4,083回接続され、追加で42,034命令を置換。
  ピッチ変換＋補正の合計は209,046命令。
- Mac Release Shared Code（arm64/x86_64、署名なし）のビルド成功。

```sh
cmake --build /tmp/sc55-firmware-oracle-build --target sc55-firmware-oracle -j 4
ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^native-tva$' -V
```

## 性能・限界

製品adapterのReleaseハーネス、macOS arm64、48kHz/128samples、
program48、24音保持、120音声秒。起動・ウォームアップを除外。
変更前実行ファイルを保存し、前/後/後/前で比較。計測中はビルドを並走させない。

| 順序 | 前後 | スレッドCPU秒 |
|---|---|---:|
| 1 | 前 | 4.366991 |
| 2 | 後 | 4.362726 |
| 3 | 後 | 4.364479 |
| 4 | 前 | 4.362907 |

平均4.364949→4.363603秒。約0.03%差で、実質横ばい。
peak=0.642068、RMS=0.121042、underrun/MIDI dropは全回0。
キャッシュhitは約10命令なので、区間の長さほど通常演奏の削減量は大きくない。
当初の単独switch項目追加では小幅悪化したため採用しなかった。
CPU時間の大幅削減を達成した変更とは扱わない。

Logicメーター、アイドル負荷、全音色、Windows/Linuxは今回未計測。
テストのsmoke wall timeはビルドと並走し得るので性能根拠にしない。
完全なH8除去、上流のピッチEG/ボイス制御の通常経路への統合は未完了。
