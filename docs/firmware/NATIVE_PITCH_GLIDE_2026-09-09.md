# 通常経路のピッチ・グライド — 2026-09-09

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

## 実装

`TryAdvancePitchGlide` が v1.21 の5175..51e7を置換する。
24bitの符号付きincrementをelapsed×ROM rateで減衰させ、
ゼロへのクランプと24bit wrapを元の命令通りに処理して音程へ積算する。
後段51e7のPCMレート変換と補正キャッシュは前回までのC++実装へ進む。
これはピッチEG全体の置換ではなく、その後段のグライド・積算部分。

既存v1.21 ROMハッシュ、ページ、割り込みマスク、trace禁止、正規voiceに加え、
非ゼロincrementではrate sourceがSRAM内でindexが128未満であることを確認する。
不適合時は状態を変更せずH8へ戻す。ゼロincrementではsourceを参照しない。
全レジスタ・SR・メモリ・命令数を維持。debtで元の12cycle刻みの
周辺装置更新を残し、JUCEの音声スレッドに確保・ロック・UI操作を追加しない。
ROM・Projucer設定・生成プロジェクトは変更しない。

## 検証

- 6,040ケースで汎用レジスタ/SR/PC/SRAM/命令数がH8と一致。
  24bit境界、正負、全128rate、random入力、ゼロ時の不正source非参照を含む。
- 13種のガード拒否で状態が変わらないことを確認。
- 起動→24音→controller/pressure→ポルタメントONと新規Note→Note Offの
  379,676ステレオフレームがH8版とビット一致。
- 実再生で5,163回/47,954命令を置換。非ゼロincrementの62回も通過。
- 同じnative-tva内の既存置換テストも成功。
- Mac Release Shared Code（arm64/x86_64、署名なし）ビルド成功。

```sh
cmake --build /tmp/sc55-firmware-oracle-build --target sc55-firmware-oracle -j 4
ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^native-tva$' -V
```

## 性能

製品adapter、macOS arm64 Release、48kHz/128samples、program48、
24音保持120音声秒。前回キャッシュ追加済みバイナリを保存し前/後/後/前で測定。
起動・ウォームアップを除外し、計測時はビルドを並走させない。

| 順序 | 前後 | スレッドCPU秒 |
|---|---|---:|
| 1 | 前 | 4.363822 |
| 2 | 後 | 4.343632 |
| 3 | 後 | 4.349798 |
| 4 | 前 | 4.366687 |

平均4.365255→4.346715秒、今回追加分は約0.42%減と小幅。
peak=0.642068、RMS=0.121042、underrun/MIDI drop=0。
通常の24音保持では主にゼロincrementの短い枝なので、大幅改善ではない。
ポルタメントの性能、アイドル、Logicメーター、Windows/Linuxは未計測。
