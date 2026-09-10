# CPUの仕事と、C++置換の実際の残り

> 本文は意味単位実装を開始する前の調査時点の記録です。現在の実装状況は
> [実装・検証記録](NATIVE_SYSTEM_IMPLEMENTATION_2026-09-09.md) の追記を参照してください。
> 特にmono・drum・FX・reset・PCM IRQを「未実装」とする以下の記述は現状ではありません。
> 2026-09-10現在、製品の既定はC++制御＋既存PCMです。H8は
> `NUKED_SC55_USE_H8=1` の明示指定時のみです。以下の「既定はH8」も過去の記録です。
> 全置換は未完了。現在の完成条件は
> [WAVESTATION構成と受入条件](WAVESTATION_STRUCTURE_CHECK_2026-09-09.md) を参照してください。

## 結論

CPUは音声サンプルのフィルター/ミックスそのものよりも、16パート・24ボイスの
状態管理、制御演算、PCMへの設定、入出力サービスを実行している。
WAVESTATION型へ進むには、その仕事を状態と関数としてつなぎ、H8のPC・スタック・
タスク切替・命令時間の消化を通常レンダーから外す必要がある。
現時点は**解析済みの部品が多いが、製品の通常レンダーはまだH8を使う**状態。

命令置換件数、ROMのbyte数、演奏中に実行した命令数は、この完成度の指標にならない。
以下は実装を読んだ接続状況であり、性能改善率や全機能の音声一致の保証ではない。

## 責務ごとの現在地

| CPUの仕事 | 根拠/既存C++ | 未完成の接続 |
|---|---|---|
| MIDI byte受理・decode・時刻順queue | sc55_midi.h / sc55_midi_queue.h | 通常製品のH8入口を代替する統合 |
| channel→part振分け・receive gate | sc55_note_fanout.h / sc55_note_dispatch.h | GS設定/パネルmodeからroutingを供給 |
| bank・音色・ドラムmap・velocity展開 | sc55_preset.h / sc55_rhythm.h / sc55_note_dispatch.h | 全設定の所有者と音色変更の再反映 |
| ボイス確保・奪取・排他停止 | sc55_voice_allocator.h / sc55_voice_lifecycle.h / sc55_rhythm_admission.h | melodic限定previewから全分岐へ接続 |
| NoteOff・hold・sostenuto・EG終了返却 | sc55_voice_engine.h / sc55_note_dispatch.h | monoを含む全MIDI処理、PCM完了時の統合 |
| mono保持鍵・戻り音の選択 | MonoHeldKeys / PrepareMonoReplacementVelocity / prepareMonoReuse | current-key更新→再準備→commit/retriggerを一本にする |
| sample選択・PCMアドレス・発音開始 | sc55_note_setup.h / sc55_sample_install.h / sc55_voice_runtime.h | special sample・reuseと全設定の入力供給 |
| EG・LFO・pitch・TVF・TVA周期制御 | sc55_envelope_runner.h / sc55_pitch.h / sc55_filter.h / sc55_level.h | 全入力の所有者、再入経路、設定反映順序 |
| global reverb/chorus設定 | CPU_EFFECTS_CONTROL資料、ROM5946〜674c | nativeの要求/遷移/係数設定は未実装 |
| GS/GM reset・SysEx・bulk/RQ1 | SYSTEM_SYSEX_RESET / CPU_SERVICES_AND_ORDER資料 | native設定所有者・応答・reset状態機械は未実装 |
| パネル・表示・LCD | CPU_SERVICES_AND_ORDER / CPU_ROLE_MAP資料 | 入力commandと表示snapshotへ分離 |
| kernel・timer・PCM IRQ | CPU_SERVICES_AND_ORDER / sc55_control_clock.h | 命令スケジュールでなく音源イベントとして統合 |

## monoの意味はどこまで分かっているか

既存のlive probeと今回のソース/ROM照合から:

- 保持鍵はpartごとの128bit bitmap。重複NoteOnを回数で数えない。
- 現在鳴らす鍵のNoteOffでだけ戻り音/停止を選ぶ。戻り音は最高鍵で、最後に押した鍵ではない。
- 戻り音のvelocityはその鍵を押したときのvelocityではなく、現在の準備velocity。
  既存実測60/40→64/80→67/110→releaseで、60へ戻る準備も110を使う。
- 戻り音なし/velocity展開で候補なしは先頭groupのreleaseへ進む。通常polyと同じ探索ではない。
- 0aed系は既存voiceを再利用する計画を作る。11d0で準備、1d55で公開、task2へ通知する。

これらの部品はあるが、PartNoteState::receiveNoteOffはmono枝を拒否し、
NativeMelodicPlayerもmonoを完成機能として扱っていない。
portamentoの演算部品とprevious voice pitch状態はあるが、note再利用時にゼロ初期化せず
どの状態を引き継ぐかを通常のmono発音経路へ接続する作業が残る。
「monoは未解析」と一括りにしない。同様に、演算部品があるだけで対応済みと言わない。

## 製品で今どの経路が動くか

NukedSC55Emulator.cppは通常Emulatorを作り、H8を動かす。
`NUKED_SC55_NATIVE_PREVIEW=1` の場合だけNativeMelodicPlayerを作る。
previewはcapital-bank melodic限定。ドラム・variation・SysEx・mono・FX setupを除外し、
CC123も完全なhold semanticsでなく限定panicになっている。
したがってpreviewの存在を「H8完全置換済み」と説明してはいけない。

製品のmcu_native.hはH8のPC/レジスタ/復帰先を維持する部分置換であり、
意味単位のnative engineとは別の層。ここだけ増やしてもkernelやnative_debtのStepは残る。

## 次の実装を何で判定するか

通常発音の縦方向の接続を単位にする。先に設定/resetの所有者を定め、解析済みの
MIDI→part→発音管理→周期制御→PCMをつなぐ。次にmono/drum/FX/表示を同じ所有権へ接続する。
新たな命令置換を増やすことを完了条件にしない。

検証点は受信結果・voice割当/返却・PCM設定と音声、同一条件のRelease CPU時間。
未対応入力を黙って落とさず、互換性未達を明示する。
audio callbackは単一の可変状態所有者、UIはcommandを渡しsnapshotを読む。
初回ROM取込・cache生成・ファイルI/Oは音声処理の外。

## 解析上の限界を残す

通常の音源サービスの役割と主経路の接続は上記資料へ整理した。
task5/6の全起動条件、隠しパネルmode、全不正入力、全割り込み競合は未網羅。
これらを「全ROMを完全理解した」と置き換えない。
既存数千caseの部品試験も、製品全体のnative音声互換性を証明するものではない。
