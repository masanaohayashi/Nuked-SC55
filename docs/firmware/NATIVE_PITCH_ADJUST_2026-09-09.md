# ピッチ後段補正・割り込み可能な段階設定 — 2026-09-09

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

## 実装

- `TryAdjustEnvelopePitch`: 50cf..510a。符号付きcontroller寄与を加え、
  元の24bit wrapと0..127000の飽和を再現する。
- `TryApplyPitchTuning`: 5124..5175。global tuning、part tuningの順に補正。
  負の補正はwrap後のbit23を判定し、正の補正は24bit wrapだけ行う。
- 両区間の一括実行は既存のROMハッシュ・ページ・interrupt mask・trace・
  正規voiceのガードを維持し、元の命令数をdebtに残す。
- `TryStepPitchStage`: 4f9e..505eの段階設定をC++の単命令経路で実行。
  マスクされていない再入場・段階遷移でも、命令間で割り込みを受けられる。
  ROM命令のfetch/decodeはしないが、各境界のレジスタ/SR/SRAMとPCを更新。
  間接テーブルは既存ROMデータを使い、traceや不正段階ではH8へ戻す。
- 新規確保・ロック・UI呼び出しなし。JUCEのスレッド・生成プロジェクトは変更しない。

## 検証

- controller補正とtuning各7,585ケースでレジスタ/SR/PC/SRAM/命令数が一致。
  127000前後、24bit上下端、符号境界、全24voice/16part、ランダム入力を含む。
- 単命令の通常段階と再入場各5,446ケースを、H8の各命令の直後と比較。
  EP=1、SR下位全16組合せ、全12段階、目標値/rateコピーを含む。
- 拒否ガードで状態不変を確認。既存のnative-tva内の比較も成功。
- Mac Release Shared Code（arm64/x86_64、署名なし）ビルド成功。
- 実再生でcontroller補正/tuningは各5,163回。
  起動→24音→controller/pressure→portamento→新規Note→Note Offの
  379,676ステレオフレームがビット一致。

```sh
cmake --build /tmp/sc55-firmware-oracle-build --target sc55-firmware-oracle -j 4
ctest --test-dir /tmp/sc55-firmware-oracle-build -R '^native-tva$' -V
```

### 到達性の訂正

前回の初期化調査では4f51がSR=0009/EP=1で通り、一括処理のガードに
拒否されることを確認した。一方4f9eの再入場については、今回別に入口の
到達回数を数えると0だった。「再入場もガードのため0回」とは断定できない。
再入場のC++処理は単体の全境界比較で検証しているが、このMIDI演奏で
再入場を通した証拠ではない。初期化の実再生25回と混同しない。

## 性能

macOS arm64 Releaseの製品adapter、48kHz/128samples、program48の24音保持、
120音声秒。直前の初期化追加済みバイナリと前/後/後/前で比較。
起動・ウォームアップを除外し、計測とビルドは並走させない。

| 順序 | 前後 | スレッドCPU秒 |
|---|---|---:|
| 1 | 前 | 4.181294 |
| 2 | 後 | 4.227736 |
| 3 | 後 | 4.231793 |
| 4 | 前 | 4.175254 |

平均4.178274→4.229765秒、約1.23%増。速度改善ではなく、小幅な悪化。
全回peak=0.642068、RMS=0.121042、underrun/MIDI drop=0。
単命令経路の判定追加も含む変更全体の値で、原因の内訳は分離していない。
完全置換への機能実装として進めたが、性能面は未達として残す。
この値はLogicのメーターではない。アイドル・他音色・他OSは未計測。

## 残件

非マスク時の補間5060..50cf、補正・変調・変換、初期化内のcall/returnや
時間復元はまだH8に戻る。単命令のC++化だけでは周期ごとのMCU駆動コストを
除去できず、最終的には割り込み/周辺装置の観測を保持する制御実行方式が必要。
他EG・ボイス管理・MIDI/GS・起動・パネル等も未完了。
完全置換のゴールは達成していない。
