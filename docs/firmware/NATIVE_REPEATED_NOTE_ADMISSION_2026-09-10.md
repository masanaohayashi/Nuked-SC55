# 通常ポリ発音の同音回収を接続

## 原因と変更

通常ポリ発音のNativeMelodicPlayer::startPendingは、velocity/key gateの後に
直接AllocateMelodicNoteへ進み、H8の00:17b8の同音整理を通っていなかった。
既存RetireRepeatedNoteはドラムのRhythmGroupAdmissionだけが呼んでいた。

実MIDIで弦音色48、NoteOn60を2回、NoteOn64を入力した時点で、H8は
古い60のgroupFieldA288 bit2を立てるがnativeは立てていなかった。
これは周期差ではなく、呼出しが存在しない接続欠落。

通常ポリにもvelocity/key gate通過後・capacity確保前の同音整理を接続した。
GSのpart assign mode0は同音を置換、1は同音グループをmarkして再重複時に回収、
2はこの整理をしない。既存H8比較済みのRetireRepeatedNoteを使用する。
処理済み状態はpending MIDIの寿命に属し、capacity/startup待ちで再入しても
markや回収を再実行しない。PCMの演算・開始保護・制御周期は変更していない。

## 検証

`sc55-cpu-roles ROM_DIRECTORY --native-release-integration`:

- 実際のH8とnativeに同じMIDIを入力し、partのgroupをkey/status/hold/typeへ
  正規化して比較。H8のRAM書換えや命令実行の代替はしない。
- CC120/121/123/124/125/126/127 × pedalなし/hold/sostenuto。
- 重複鍵を含む発音、停止command、pedal解除とNoteOff後を比較。
- assign mode0/1/2で同音を6回連続発音し、markと実回収を比較。
- 81比較PASS。物理slot・EG位相・全sampleのH8一致を示す試験ではない。
- `--native-synth`: checksum `3b54320560580fd3` とzero/1/127/129/257frame
  分割の一致は維持。
- `SC55_KICK_ALL=1 --song-first-kick 55KTIZKE.MID`: 13kickの最初の1msの
  gain比較PASS。key-onはH8=2100/native=2114、allocation差の観測も残る。
  これは曲全体の互換性PASSではなく、ドラム開始保護の限定回帰確認。

既存のcapacity survivor/timing差は別件であり、この修正で解決済みとはしない。

## CC84の新規発音と無効化後の継続

追加の実MIDI比較で、CC84のsource groupが見つからない新規発音にも同じ
retirementの未接続があった。assign mode0で2回同音を入力すると、H8は1group、
nativeは2groupを保持していた。retirePendingMelodicへ処理を共通化し、
通常polyとsourceなしの新規発音が、同じpending-note単位の処理を通る。

さらに音色変更によるreuse invalidationが立ったsource選択では、H8の
00:0d65..0d73は一致groupを停止してinvalid bitを消し、0dccの新規発音へ進む。
nativeは停止後returnし、次のserviceで再検索して別の古い同音groupを再利用した。
pendingSourceFreshでこの新規発音の判断を保持する。新しいMIDI NoteOnでは解除し、
通常のsource reuseを妨げない。録画済みのH8時刻や任意の遅延は使用しない。

追加fixtureはassign mode0/1/2でsource不在の同音4回、source一致かつinvalid、
その次のinvalidでないsource reuseを通す。H8のPCとinvalid bitは診断だけで観測する。
停止系と合わせ99group比較PASS。通常の音声checksumと可変block分割も維持。

## 発音継続状態の所有

分離していたpending MIDI、part番号、戻り音フラグ、回収済みフラグ、
sourceの新規発音判断を`PendingAdmission`に統合した。
イベントの完了/棄却でoptionalを破棄すると継続状態も一緒に破棄される。
MIDI NoteOnとheld-key returnの生成箇所は、以前のフラグを個別にクリアする
順序へ依存しない。CPUのPC/stackを模した状態や、新しいPCM実装ではない。

製品と同じMIDI/render経路で、portamento有無の60→64→67→off67→off64→off60を
追加し、H8のグループ状態と現在のmono keyを比較。停止系・CC84と合わせ111比較PASS。
この所有権整理は特殊音の約2.18msの終端時刻差を直す変更ではない。
