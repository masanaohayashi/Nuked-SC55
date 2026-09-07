# 解析記録の保全

整理日: 2026-09-07。[解析索引](../../../FIRMWARE_RESEARCH_INDEX.md)から分野ごとの資料へ進める。

## 保全範囲

既存資料（FIRMWARE_STRUCTURE、DEVIATIONS、firmware-oracle README、voice-lifecycle）に明記された
`/tmp`参照を中心に、CSVに対応するログ、直近の結合・ビルド記録、CPU計測ソースとプロファイル、
スレッド監査ソースを選んで保全した。全一時ファイルや、資料中の全ての相対ファイル名を網羅したものではない。

- 246ファイル、圧縮前4,975,434,358 bytes。
- [manifest.json](manifest.json)に元パス、アーカイブ内パス、サイズ、SHA256を記録。
- ローカルアーカイブ: `.local/firmware-evidence/2026-09-07/research-records.tar.gz`（リポジトリルート基準）。
- 圧縮後545,358,292 bytes（約545 MB）。全246ファイルをアーカイブから読み出し、
  元ファイルのサイズ・SHA256と一致することを確認済み。
- アーカイブ全体のSHA256: `2e485f8d4398fa94ad4d317d742d965fcb8906847bddbc046145a14c277847e5`。
- ROM、逆アセンブル本体、ROM由来のバイナリデータ、実行ファイル、ビルドディレクトリは格納していない。
  除外対象と理由はmanifestの`notArchived`を参照する。
- 明示参照の調査範囲で、実在した結果ファイルの欠落は見つからなかった。
  存在しない`new-melodic.sdata`、`reference.csv`はコマンド例のプレースホルダー。

このアーカイブは**Git管理対象外のローカル保全**。cloneやpushでは別のマシンへ移らず、
ディスク障害へのバックアップにもならない。必要なら別途非公開の保管先へコピーすること。
ログにはROM由来の値やマシン固有のパスが含まれ得るため、リリース資産として公開しない。
Gitで共有する対象は、この説明・manifest・索引・[CPU計測要約](../CPU_PROFILE_2026-09-07.md)。

## 記録を読む

リポジトリルートで、manifestにある`member`を指定すると展開せずに読める。

```sh
tar -xOf .local/firmware-evidence/2026-09-07/research-records.tar.gz sc55-allocator-init-1.log
```

記録は過去の実行の証拠であり、現在のコードの再テスト結果ではない。
今回の作業では新しい解析・製品ビルド・テスト・CPU計測を行っていない。
