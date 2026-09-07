# CPU計測要約 — 2026-09-07

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

## 測定範囲

Logic本体が起動していなかったため、製品の `NukedSC55Emulator.cpp` とbackendを直接リンクした
一時ハーネスで測定。MIDI FIFO・音源・host-rateリサンプリングは製品コード。
AudioProcessor全体、ホスト、UI表示、2Xの負荷は含まない。

- macOS arm64、48kHz、128 samples/block、1インスタンス、SC-55 v1.21。
- 通常H8経路、既定のPCM simulation。native preview・ログ・PCM関連の環境変数overrideは解除。
- ROM読み込み・起動を除外。UART受信、PCM 24-slot設定、MCU 6000万cycles超を確認してから計測。
- 演奏: program 48、note 36〜59、velocity 100を保持。PCM voice mask `00ffffff`。
- Release相当 `-O3 -DNDEBUG`、Debug無最適化。両方にsymbols/frame pointersを付与。
- 下表は「計測スレッドのCPU秒 / 生成した音声秒 × 100」。Logicメーターの値ではない。

## 実測

| ビルド | 条件 | 音声秒 | CPU秒 | CPU/音声時間 |
|---|---|---:|---:|---:|
| Release | アイドル | 10 | 0.179483 | 1.795% |
| Release | 24音 | 20 | 0.922510 | 4.613% |
| Debug | アイドル | 10 | 1.095342 | 10.953% |
| Debug | 24音 | 10 | 4.401116 | 44.011% |

演奏peak=0.642068、アイドルは無音。underrun/MIDI dropなし。
サンプリング許可待ち中に完走した長いDebug計測も、1000音声秒に444.539601 CPU秒（44.454%）。
その後のprofileは新しい同条件のプロセスから採取し、採取後に終了した。

## 5秒サンプリングの内訳

値はサンプルされたCPUスタックの構成比。上表のCPU/音声時間比とは母数が違う。
呼び出し木のself countを祖先で分類し、以下の行同士が二重計上にならないよう集計。
ReleaseではH8 decode等がMCU_Stepにinlineされるため、単純な関数名比較をしない。

| 分類 | Debug 24音（4211標本） | Release 24音（3937） | Release idle（4270） |
|---|---:|---:|---:|
| PCM simulated voice renderer | 46.4% | 11.5% | 1.9% |
| PCMその他・エフェクト・PCM出力 | 6.6% | 11.8% | 10.9% |
| H8命令 | 27.8% | 次行に含む | 次行に含む |
| MCU制御・inline H8・UART | 6.1% | 59.8% | 70.6% |
| タイマー | 5.2% | 8.8% | 4.6% |
| 割り込み | 2.5% | 1.3% | 1.0% |
| adapter駆動・FIFO確認 | 4.7% | 5.3% | 6.8% |
| リサンプリング・adapter出力 | 0.6% | 1.4% | 3.9% |
| その他 | 0.1% | 0.1% | 0.3% |

Debugの主なhotspotは `pcm_sim.cpp::RenderFrameNeon`（wave address walk、差分decode、
補間、フィルタ/mix）。Releaseでは `mcu.cpp::MCU_Step` とH8 opcode・memory処理が大きい。
通常H8経路はアイドルでも約2000万emulated cycles/音声秒を進めていた。

## 結論と限界

通常経路のRelease改善ではH8/MCU処理の置換が優先候補。
ただしH8命令の中でEGが占める割合は分離していない。
UI/ログなしでDebug約44%が再現したことは、その基礎負荷がUI/ログだけの問題ではない証拠。
LCD競合が実際のLogicに与える影響を否定するものではない。

この測定で、Logicでの5%/1%目標達成、全音色の性能、native previewの音質・CPU優位性は保証できない。
製品コードの最適化・修正はこの診断では行っていない。

元の `RESULTS.md`、3本の `*.sample.txt`、ハーネスの `main.cpp` と `CMakeLists.txt` は
[ローカル証拠アーカイブ](evidence/README.md)内の `sc55-perf-uYIHTJ/` に保全してある。
