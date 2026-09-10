# SC-55 v1.21: 設定転送・起動・パネル・実行順序

## 根拠と対象

mk1-v1.21のROM静的経路と `tools/firmware-oracle/cpu-roles` のH8実行を照合。
命令最適化は無効。製品コードを変更する調査ではない。
本書は [役割マップ](CPU_ROLE_MAP_2026-09-09.md) と
[通常SysEx/reset](SYSTEM_SYSEX_RESET_2026-09-09.md) の追加調査を引き継ぐ。

## SysEx受信と発音準備の独立性（2026-09-10追記）

通常のSysExもtask0の00:2563→04:0e8eで解釈される。
00:24b2のABFE/AC00比較はSysEx bufferの消費位置／終端であり、
task1の発音command queue AAF8/AAF9とは別。04:0ee9のbyte読出しで確認できる。
従って全SysExを発音commandの完了待ちにする根拠にはならない。

`--native-sysex-during-preparation`で最大発音中に32件のNote Onを送り、
master volume DT1→CC7を続けた。H8の8002変更時にはAAF8/AAF9=39/1cで、
発音commandは未処理。nativeは変更前全件待ち、変更後31件のqueued workが
残った状態で値を更新する。キュー容量／cursor単位／UART速度の一致は主張しない。
fragmented packet、不正checksum拒否、後続CC、次の正常packetもH8と比較した。

製品は停止不要なmaster/controller/FX/reserve/drum/displayの更新を受信側へ接続。
reset、RQ1は引き続きEOXで発音処理の順序を待つ。
EOXの固定長previewは待機時にreceiverを変更せず、次回同じEOXを再判定できる。
後続MIDIが未完了transactionを追い越すことはない。PCM演算／再利用ランプは変更なし。
通常TX待ち、bulk18要求62packet、reset後RQ1、shared rhythm42件の回帰もPASS。
GS part再設定は続く実装でVoiceCommandsへ接続した。04:1194→1197は
channel指定時の制御値resetとcommand16→10、04:1473..14aaはmono/poly bitの
変化時だけcommand1c/1eを投入する。Program Changeと同じ音色選択では
04:0a24の変更判定に従う。受信時の値と発音側の処理を混ぜない。
`--native-part-during-preparation`は未処理Noteが残るpart volume更新、
同一channel再指定・hold解除・後続expression保持をH8と比較してPASS。
capacity選択差、bulk/resetを含む全競合順序の一致はまだ未解決。

bulk48再設定も受信側へ接続した。04:1617..165aは全partを降順再選択し、
有効なmelodic toneが変わった場合だけ通常の音色commandを発行する。
nativeは設定のbank/program/noteFlagsをpreviewし、最大16件のProgramVoiceRequest
を確保してから設定全体を反映する。drum mapの再選択はlive状態で降順に行い、
上位partが共有map経由で下位partのprogramを変える挙動を保持する。
`--native-bulk-system`の48件Note→bulk volumeでは、H8のAAF8/AAF9=32/08、
nativeの残47件でvolume更新を確認した。既存8転送の全1,864byteとreset比較もPASS。
これは受信／発音の責務分離であり、PCM演算や仮想時間モデルは変更しない。

## model45: 表示データ

`04:7321–73a6`。通常の音源設定とは別のモデル識別子45。

| address | 受理するデータ | 保存・作用 |
|---|---|---|
| 10 00 00 | 1〜32文字 | 長さcead、文字ceb0〜。20h未満はspaceに補正し37ddへ |
| 10 01 00 | 64byteちょうど | ff00〜ff3fへコピー、cf34=012c |

不正addressはce5a=0b、不正lengthはce5a=0c（cf2f=78）。
実測でTESTの長さ4・文字列、64byteのドットデータ全一致を確認した。
これらは表示スナップショット側のデータ。ただしtask 7全体はresetも担当し、UI専用ではない。

## bulk48/49: 設定メモリの転送

48はニブル対をbyteに戻し、8000 + middle*64 + (low>>1)へ格納する。
範囲は通常設定8000〜8747。書込後の1617は全パートの音色・gain再反映へ接続する。
49はドラム設定の配置変換を伴い、基点8848、mapのstride48c。
block indexは0→2、1→3、2→0、3→1（それ以外はそのまま）へ変換する。
`48 00 04 02 05`で8002=25h、`49 02 00 03 09`で8848=39hを実測した。

これは通常DT1の設定テーブル経由とは別の書込経路。native側で片方だけ実装してはならない。
全境界値・全map組合せの動的網羅を意味しない。

### bulk48のnative接続（2026-09-10）

`BulkSystemData::write` はwireのoffsetとニブル対を復元し、
`NativeMelodicPlayer::storeBulkSystemByte` が既存の型付き設定へ直接反映する。
H8 RAMのコピーを実行時状態として保持しない。通常GSのscalar setterを経由して
clampしたりchannel変更時のresetを起こしたりせず、bulk経路の意味を保つ。

対象はmaster tune/volume/key/pan/portamento CC、16partのbank/program/routing、
音量・pan・key/velocity範囲・tone EG/LFO設定・scale tune・assigned CC、
6種類のmodulation感度列、partial reserve/priority、reverb/chorus設定。
受信完了後はROM1617に従い全partのprogram選択、level等の再反映、FX要求を行う。
expression等の演奏中の動的コントローラー状態は初期化しない。

不正開始offsetは拒否。有効開始位置から領域末尾へ達した場合はprefixを適用して終了。
奇数low addressは右shiftで切り捨て、空データ/奇数長の末尾はEE9のFFを使用する。
SysEx checksum/packet上限は既存receiverで処理する。

診断は `sc55-cpu-roles ROM_DIRECTORY --native-bulk-system`。
H8が1617へ入る直前に全0x748byteをwire decoderの期待結果と比較し、
処理後にnativeの型付き設定をH8と比較する。ROMへの直接パッチではなくMIDI入力を使う。
8転送でPASS: 発音中のmaster変更、1partを64+48byteに分割した転送、
空/奇数長/末尾切り詰め、reserve/priority、FX。
全16partの主要設定・6種の感度列・assigned CC・expression維持を比較した。
`--native-synth` もPASS、可変block長での一致とchecksum `3b54320560580fd3` は不変。
これは設定反映の比較であり、bulk変更直後のH8音声波形の全sample一致までは測っていない。
未使用・未モデル化のpadding byteはnative設定として保持していないため、
opaque byteを含めたRQ1/bulk読出しの完全round-tripを保証するものではない。
未解析の設定byteに機能が見つかった場合は名前付き状態として追加する。

## RQ1: CPUが応答を生成する経路

`04:0ec9`で40→17ce、41→192c、48→1c3f、49→1cab。
40/41は通常設定と共通の141レコードを7種の読出し関数（表1786）で直列化する。
1796でRoland DT1ヘッダ、16f5でTXリングa5f8へ追記しchecksumを累積、1723でchecksum/F7。
TX書込位置abfc、読出位置abfa。**診断が採取するのはこのリングで、ホストMIDI出力ではない。**

48は64復元byteずつ分割、送信drain後40tick待ってoffsetを40h進める。
49はsizeをmiddle*64 + (low>>1)に復元。通常要求128復元byte、特殊offset380hは12byte。
従って今回の49読出しsizeは00 02 00。00 00 02では正しい要求にならない。

### 現エミュレータのTX待ちと診断の区別

通常の `--extended` では4要求とも応答をリングに作るが、abfa=0のまま待つ。
待機PCは1882/19c1/1c76/1d1d、abfcは11/11/12/138。
DEV_SCRへの書込時に「既にSSR.TDRE=1でTIEを有効にした」割り込みが発火せず、
MCU_UpdateUART_TXもSSR bit7が既に1ならreturnするため。

`--extended-tx` は**診断だけ**でTX有効・TIE有効・TDRE=1の条件にUART_TX要求を補完する。
これにより4要求はすべてdrainし、40/41/48は11/11/12byte、49は138byte×2packetを生成。
5packetすべてaddress+data+checksumの和がmod128で0であることを確認した。
これは製品のTX修正でも、ホストからRQ1応答を取得できるという保証でもない。
補完なしの挙動を隠さず、補完ありをROMの応答生成ロジックの診断として区別する。

## fresh bootとRAMを残したresetは違う

Emulator::Initは新しいMCU/PCMを作る。Emulator::Reset/MCU_ResetはRAM全体を消さない。
SC-55のLoadNVRAM/SaveNVRAM経路はなく、該当保存処理はJV-880条件。
プラグインのinitialiseは新しいEmulatorを作る。ホストの保存状態復元とは別の概念。

`04:0746`は42c2の妥当性確認、cb75 bit0、cdfc等から既定値コピーの要否を判断する。
42c2はRAMcdcdの38byteをROM04:f380と比較する。
既定値コピー1e6aはROM03:ca00/ca08/ca48/cab8等から8000/8008/8048/80b8等へ。
fresh bootで1e6aを観測。TX待ちからRAM保持resetした場合はこれを通らず設定値も保持した。
任意の保存状態や故障RAMの全分岐を網羅した証拠ではない。

診断上の別注意: 現MCU_Resetはsleepを消さない。sleep中のCPUへResetだけを呼んだ
診断ではreset vector016cから進まなかった。fresh InitやROMの起動仕様と混同しない。
今回これらの製品動作は変更していない。

## パネルは入力、LCDは出力

MCU_ReadP1はP0で選択した行とbutton_pressedを読む。
task 3の04:29c9は3bankを走査し前回値と照合、2a69/2b07側にedge・repeat等の処理がある。
実際のボタン入力を2M cycles押下・2M cycles解放して確認:

- INST_R（bit4）: part1 program 80b9が00→01。
- LEVEL_R（bit21）: part1 volume 80c0が64→65。

パネル操作はMIDIと同じ音源状態へ入る。JUCE UIから直接audio-owned stateを書かず、
入力commandとして直列化する。LCD転送・文字・メーターは出力snapshotへ分離する。
全ボタン組合せや隠しモードまで確認したわけではない。

## カーネルの順序を意味として残す

通知0422はtaskのpending bitを立てる。通知引数はbit番号でありmaskではない。
待機0466〜0473はpending & waitMaskの**最下位bitから1個**消費して番号を返す。
同一bitの重複通知は合流する。MIDIのリング/イベントqueueは合流させてはならない。
042e〜0434はfdccとの優先度比較、より小さいtask IDへの通知はpreemptionを起こし得る。

従って同時にpendingならtask 8はevent0→1→7（設定要求→周期）、
task 1はevent0→1。これは「後から来た割り込みも常にこの順になる」という主張ではない。
task 8の周期本体5af1では経過回数を回収し、reverb→chorus→24ボイス更新の順に進む。

timerはnext deadline word fe12+2*n、period fe24+2*n、epoch fe36、timer wheel fe38。
周期通知bitと経過byteは別。256回でbyteが0へwrapしても通知は残る。
既存ControlTaskClockのpendingはraw tick数でなく**周期満了回数**。
H8実装は命令を一律12cyclesで進める箇所があり、トレースのcycleは現エミュレータ基準。
実機H8の全命令cycle精度やhost sample単位の完全一致を主張しない。

なお既存C++の「task4」はCAF4の**ボイス操作コード4**を指す場合がある。
本書のkernel task 4（LCD）と別物。名前の一致からスレッド所有権を判断しない。

## 残る互換性境界

### MIDI受信監視の追加確認（2026-09-10）

`--midi-watchdog` は通常のH8命令実行でUART受信と通信断を起こし、
`MidiReceiveState` の入力フィルター・監視状態を各処理の出口で比較する。
初回は39入力byte、4監視poll、1通信断で一致。
本体復帰テスト追加後は54入力byte、17回のFRT1処理出口（うち4回監視到達）で一致。
全byte値・全割込競合の網羅ではない。

- `00:05b3..05e8`: 全受信byteがactivityを立てる。F1〜F6は後続dataを
  捨てる状態へ移る。channel status/F0で解除。F7は通すが解除しない。
  F8〜FFは上位decoderへ通さず、FEだけが受信監視をarmする。
- `00:06e0..073a`: 未armならactivityを維持。arm後のpollでactivityがあれば
  消去して継続、なければdisarmして通信断を通知する。
  **最後の完成MIDIメッセージから固定300msを数える実装ではない。**
- 実行時FRT1 TCR=22、TCSR=51、OCRA=927c。監視到達間隔は今回
  9600324/9600192/4800192 emulator cycles。AC29分周とUART送信可能条件を通るため、
  この観測値をそのまま固定周期定数として採用してはいけない。
- timeout `00:06ec` はCE5A=10、CF2F=78と内部RXリングへのFE挿入を行う。
  外部FEはISRで除去されるため、この内部FEと意味が異なる。
  decoder `00:200f` → `04:08b8` を実行で確認した。
- `04:08b8` は全16partの0844（controller/key pressure等のリセット）と
  command18を投入し、RXリングとrunning statusを消去する。
  command18のtable entry `00:086a` は08ce。各partで最初に168dによる
  **全グループ停止・回収**を行い、続いて08ba（pedal解除等）へ進む。
  押鍵中の音も停止する。単純なPCM出力ゼロ化やGS全設定resetへの置換はしない。

続く実装で `MidiReceiveTimer` と製品scheduler、通信断イベントを接続した。
タイマーはFRT1の設定値から2400064 device cycles。AC29の分周、TX empty/ready gate、
timeout時にdivider clearを迂回する挙動をC++状態にする。
nativeの現構成にはTXキューがないためempty/readyをtrueで与える。
RQ1/TXを接続する際はその実状態で駆動する必要がある。
起動epochと命令実行遅延を含むH8とのsample単位の時刻一致は未保証。

通信断は受信キュー末尾の専用イベントであり、先行noteの開始待ちを追い越さない。
消費時に全part controller reset→全group停止→制御値再反映→RX/decoder破棄を行う。
PCMは既存の停止ランプを実行する。音色・volumeは維持する。
初回の本体比較ではcontrollerのみ一致し、押鍵中の音がnativeで1voice残った。
08ce先頭の168dの接続漏れが原因で、既存stopPartGroupsを接続して修正した。
修正後の同コマンドはPASS。全16partのexpression/voice countをH8と比較し、
押鍵中とpedal保持中の停止、program48/volume93の維持、復帰後の新規発音を確認した。
`--native-synth` もPASS（0/1/127/129/257 frame分割、音声checksum
`3b54320560580fd3` は接続前後で不変）。ホストCPU測定やXcode配布物の検証ではない。
通信断LCDエラー表示、TX busyとの統合、キュー満杯時のfirmware同等復帰は残る。
キューが満杯の場合は受理できたbyte分だけ受信状態をcommitする。
音声スレッド内で固定サイズの値を操作し、メモリ確保やUI呼出しは追加していない。

task 5/6は0542でeventを待ち、起動後の通常MIDI/設定/パネル試験では再実行を観測していない。
直接通知の検索だけで間接通知・全隠しモードの不存在を証明できないため、未使用とは断言しない。
全割り込みinterleaving、全パネル組合せ、全不正SysExは未網羅。
通常音源の責務を定義することと、ROMの全到達可能状態の証明を区別する。

## Native RQ1接続（2026-09-10）

`sc55_parameter_reply.h` はH8 RAMコピーではなく、既存のmaster/part/controller
設定から通常GS応答を生成する。NativeMelodicPlayerの実際のSysEx受信経路へ接続。
NativeSynthのaudio-owner用`popParameterReply`で応答を取り出せる。
対応範囲は40:00のtune/volume/key shift/pan/portamento CC、40:1pの全table record、
40:2pの全6系統×11 modulation設定、40:01の全record、41の両drum mapの全field。
40:3pと40:00:7fも下記追記の通り接続済み。
48/49は後述の明示的なbulk出力接続時に受理する。
system nameは既存C++状態に無かったため16byteの名前として所有し、通常DT1の長さ判定/
文字clamp、bulk48書込み、起動/GS reset復元を同じ値へ接続した。PCM処理は変更なし。

04:17b6はsize3byteの**加算**、19d9..1adfはrecord長の一致を要求する。
長さを通常のbase128整数として解釈したり、scalar読出しを連続recordへ拡張しない。
bit mask、fine tuneの2nibble、master tuneの4nibble、rhythm enable/mapを型で符号化する。
`--native-parameter-replies` は411件の全packetをH8がTX ringへ実際に書いたbyte列と比較。
part0/1/15、全read型、設定変更後、size上位byte指定を含む。
H8のTIE/TDRE ready IRQは既存extended-txと同じ**診断限定**補助を用いる。
同診断はPASS。8種類の不正sizeで両者が完全な応答を出さないこと、native出力の
16件保持/17件目破棄も確認。`--native-synth`はPASS、0/1/127/129/257 frame分割一致、
音声checksum `3b54320560580fd3` は不変。今回Xcode/Logicの再検証は行っていない。

リセット直後の応答遅延を発見し、修正した。修正前は変更済みFX状態から
reset送信後2M cycles、続くRQ1後2M cyclesの時点ではH8はdefault system nameを返信するが、
nativeは返信しなかった。`--native-reset-reply-timing` にこの再現条件を維持し、修正後PASS。
`--native-reset-reply-minimal` はboot→reset→RQ1のみでも修正前に同じ失敗を再現した。
観測したnative状態はreset phase=effects、free voices=24、FX未完了、reset完了回数0。
原因はリセットがFXランプ完了まで全MIDIを遮断する追加の待機段階を持っていたこと。
H8は04:12b8/12beでtask8へ通知し、04:379fでMIDI taskへ完了通知する。
この通知2回と復帰を実行中H8で確認。なお復帰時のFX phaseは常に非zeroとは限らず、
phaseの値だけを「非同期更新が通知された」証拠にはしない。

nativeでは既存のボイス停止/drainと設定復元を維持し、FX更新を要求した時点で
resetを完了させた。ランプは通常の制御周期で続く。固定delay追加・PCM変更は無し。
最小試験は修正後PASSし、FXが未完了でもMIDI再開、続くnoteのH8と同じvoice数、
さらに発音中の再resetで24voice解放後に応答することを確認した。
全411件の比較、8種類の不正size、queue境界、`--native-synth`もPASS。
全reset時刻のcycle一致や全FX状態での音声一致を証明したものではない。

応答は固定16packetに保持し、満杯なら新しい応答を破棄してcounterを増やす。
これはboundedな暫定出力境界であり、firmwareの送信待ち・RX順序・watchdogのTX busy
との統合を代替するものではない。ホストMIDI出力は未接続、生成project/format契約は未変更。
従ってRQ1全体の移植完了、ホスト送信成功、H8全制御移植完了とはしない。

## Bulk読出し転送状態（2026-09-10）

`sc55_bulk_reply.h` の `BulkReplyTransfer` に48/49読出しの転送処理を実装。
CPU命令・PCM・ホストAPIに依存しないaudio-ownerの状態で、最大138byteのpacketを
caller所有バッファへ作る。全設定を二重保持せず、chunk発行時に設定所有者から読む。

- 48: address=middle*64+(low>>1)、size先頭byteは0、長さは同じくmiddle*64+(low>>1)。
  正の長さ、開始<748h、終端<=748hを要求する（04:1b4d/1bb0）。
- 49: map0/1、開始middleのbit0=0、low=0。最初の4blockはxor2で行を対応させる。
  長さは128byte、offset380hの名前のみ12byte（04:1b65/1be4）。
- 最大64byteを2nibble化、独立checksum付きで出力（04:1af6）。
- 実際のTX完了通知まで次packetを出さない。単に送信queueへ積むことを完了扱いしない。
- TX完了後40kernel tick待つ。最終packetにも適用する（04:1c78/1d2b）。
  C++側はtick入力で進み、sleepは使わない。

`--native-bulk-replies` はPASS、18要求・62packetをH8の実際のTX ring出力と比較。
全748hのsystem領域、奇数wire address、両drum mapの全有効開始blockを含む。
H8のTX drain、TRAPAへの40tick引数を観測し、nativeの送信前advance無効・39/40tick境界・
重複完了通知拒否・busy request拒否を検証。6不正要求はnativeの受理判定のみ確認。
TX ready IRQ補助は以前と同じ診断限定。CPU-cycle単位の送信時刻一致を主張しない。

初回試験はH8のsource byteを利用したが、その後native設定所有者への読出しに置換した。
`--native-bulk-replies` は同じ18要求・62packetで再度PASS。source byteはsystemなら
`NativeMelodicPlayer::readSystemConfiguration`、drumなら既存`rhythmRecords()`のみから読む。
H8は比較対象のTX出力と境界観測であり、nativeの設定読出し元には使わない。

`readSystemConfiguration` は全748h（1,864byte）をlive master/part/controller/effectsから
構成する。発音モデルで未解釈の146byteだけ`UninterpretedSystemSettings`に保持し、
bulk48書込みと起動/GS resetへ接続した。既存の設定を常時コピーするRAM imageは追加していない。
未解釈とは「H8でも未使用」という意味ではない。意味を移植する際は対応する所有者へ移す。
`--native-bulk-system` は起動、8種の書込み後、GS reset後の全1,864byteをH8と比較してPASS。
変更した未解釈part field、以前捨てていたglobal40h、末尾747hの保持も含む。
`--native-synth` PASS、音声checksum `3b54320560580fd3` 不変。

**音源の要求受理へ接続済み、ホストMIDI出力は未接続。**
NativeMelodicPlayer/NativeSynthのaudio-owner API:
`setBulkOutputConnected`、`takeBulkReply`、`completeBulkReplyTransmission`。
送信先を明示的に接続した場合のみRQ1を受理し、接続中のtransactionは途中切断を拒否する。
デフォルトの出力未接続時はunsupportedとして後続MIDIを処理し、永久送信待ちにしない。
通常のparameter replyが残っていれば先に消費する。bulk packetのqueue投入と実送信完了を区別する。
40tick待ちは音声側の共通kernel tickで進行し、期限でMIDI処理を再開する。
PCM/周期制御は止めず、転送でMIDI処理を休止中はqueuedEventsだけで毎sample起床しない。
bulk packet送信中は既存watchdogのTX busy gateも適用する。

通常モードの転送開始/終了副作用を接続した。
開始04:1d6cでは04:1399で全part command12、04:13be/0844でcontroller初期化を要求し、
task1へ通知する。nativeは全part stop/reclaim→controller reset→control更新を行う。
04:4252/2773はUARTを無効化し、04:2715は既受信buffer/decoder/AC27をclearする。
nativeでも現在のqueue dispatchが戻ってから受信済みtailとdecoder/Active Sensingをclearする。
転送中の新着入力は終了時に再演奏しない。エミュレーターのMCU_PostUARTには一時保持されるが、
終了04:424b→2738が再開前に読捨てる。途中のUART bufferだけ見て「終了後に再生」とするのは誤り。
開始前に後続CC7、転送中にCC10、終了後に新たなCC7を送る比較で、前2つは不適用、最後だけ適用を確認。
`--native-bulk-replies` は実際のplayer.pushから開始し、音源が生成した全62packetをH8と比較してPASS。
発音中から開始して24voice解放、expression127復元、送信未完了時の足止め、終了後新入力再開も確認。
`--native-synth`/`--midi-watchdog` PASS、音声checksum不変。
CF02 bit0=1の別動作モード、受信再開の最終byte境界、通常RQ1の送信busy統合、
実ホスト出力・切断中断ポリシーは残る。ホストMIDI契約や生成projectは変更していない。

### パネル起点の一括転送の範囲訂正（2026-09-10）

上記の「別動作モード」は、通常の外部MIDI RQ1に任意のmodeを付ける機能ではない。
ROM2の04:713e、715f、717dはCF02 bit0をセットしてから共通開始1d6cを一度呼び、
一連の要求生成の後に28c2、424bを呼び、最後に同bitをクリアする。
1c4b/1cb7はそのbitで各要求の開始処理を、1c93/1d4bは終了時の受信再開を抑止する。
したがって、このbitは外側の一括送信処理が所有する。

713eの経路は70ceへ12hを渡し、70f2へ0、1を順に渡す。
70f2は709eの6byteレコードを8件走査し、要求のmapに引数の上位nibbleを加える。
70ce/70f2はcf7bへ要求を構成し、40tick待機後に7300で内部入力ring a6f8へ渡す。
7300は先に7262でこのringの消費を待つ。これは外部TXの完了待ちとは別であり、
「各転送の末尾にさらに固定40tick追加」と推測して実装してはいけない。
内部要求の消費と外部送信が重なり得る順序は実行比較が必要。

この確認はROMの命令・呼出し関係の静的確認であり、パネルの実操作からの到達や
全送信packetの動的比較ではない。C++側は通常RQ1の転送のみ接続済みのまま。
未対応責務を「パネル一括送信」としてTODOへ分離し、完了扱いにはしない。

#### 全設定送信の実ボタン照合とnative接続（同日追記）

`--panel-bulk-sequence`を追加。H8のPC/RAMや内部入力ringを書き換えず、
ALL→INSTRUMENT左右同時押し→ALL確定で713eへ到達する。
PART左右から切り替えた画面の同時押しは別範囲（selection2）なので混同しない。
実行命令hookで開始1回、要求17件、packet60件、終了1回を確認。
Step前PCでは割込からの復帰時に同じPCを再観測するため、診断hookはIRQ dispatch後の
実際の命令入口を使う。観測の重複を製品の再送仕様と解釈していない。

送信内容は全system1,864byte（30packet）、drum map0/1各15packet。
各mapの要求順はwire block00,02,04,06,08,0a,0c,0e（map1は+10）。
70e8/712aの40tick待機は17回。7300のreturnはその要求の最終TXと末尾待機の後に
観測され、次のproducer待機はその後に始まる。7318でtask7から解析関数へ直接
pjsrするためであり、別task0の処理を待つという当初の説明は訂正する。
線形逆アセンブルでは直前命令の末尾と誤認されていた03がpjsrの先頭。
実行命令hookで7318のopcode03とtask7を確認している。
単に送信packet末尾へ任意delayを足したものではない。
最終packetのTX完了219,674,004cycles、7300復帰220,474,296、
終了処理424b到達220,474,440、受信再開26ef到達220,506,960。
これは診断H8の時刻であり、nativeの絶対cycle一致を主張しない。

BulkReplyTransferが要求間待機・全シーケンス・packet送信完了待ちを所有する。
NativeSynth::requestAllSettingsDumpは明示出力接続と発音準備の完了を要求し、
受理後に一度だけ停止／controller reset／入力破棄を行う。転送中の値操作は
終了時に破棄する（H8の28c2）。通常外部RQ1の挙動にはこの破棄を適用しない。
NativeSynthの実push/render/transport経路から全60packetをH8と全バイト比較してPASS。
送信中にrenderしてもACKなしでは進まないこと、途中のCC7破棄、終了後CC7再開、
パネル値操作破棄も確認。固定サイズ状態のみで、PCM演算は変更していない。

全設定送信の音源側入口までの実装であり、製品UIの同時押し／確認表示、
部分送信715f/717d、ホストMIDI出力の接続はまだ残る。
診断ログ `/tmp/sc55-panel-bulk-sequence.log`。Xcode配布buildは今回実行していない。

#### 部分送信の統合とmap選択の実行差（同日追記）

715fはsystem先頭8byte、続く64byte、選択part各112byte、付随するdrum mapを送る。
717dはsystemの2要求を省く。7191は通常モードではNote Receiveの有効partを選び、
全体mute時は選択しない。solo表示（CDCC bit3）の追加選択規則は未接続。
BulkReplyTransferは固定長の送信範囲列と現在位置を所有し、全設定／部分設定で
同じpacket生成、ACK待機、要求間待機、開始／終了処理を使う。
NativeSynth::requestSettingsDump(scope)は対象partの番号列やRAM配置を呼出し元へ
要求せず、音源が持つNote Receive/muteとroutingから対象を決める。

両mapを有効にした試験で、7191はR5へ60hを集めるが、map0の70f2から戻った
71efではR5=0となり、map1は送信されなかった。解析関数を直接呼ぶ際の作業値変更が
残る既存H8の挙動であり、意図した仕様だとは断定しない。nativeは部分送信で
map0があればmap1を省く。713eは各mapを明示指定するので両方送るまま。
map1だけを使うケースでは正常にmap1の15packetを送ることも比較した。

| 実ボタン試験 | 内部要求 | packet |
|---|---:|---:|
| 全設定 | 17 | 60 |
| system＋有効parts（既定map0） | 26 | 49 |
| 有効parts（既定map0） | 24 | 47 |
| rhythm partのNote Receiveを無効化 | 15 | 30 |
| 全partsのNote Receiveを無効化 | 0 | 0 |
| map1のみ使用、parts送信 | 24 | 47 |
| 両map使用、system＋parts送信 | 26 | 49 |

実際のMIDI設定とパネル入力からH8の送信byteを採取し、NativeSynthの出力と全件一致。
空集合でも停止／reset／終了を行い、不要な送信待ちを作らない。
ACK前にrenderを進めても次packetを出さず、転送中のCCを破棄し、終了後は再開する。
通常外部RQ1の18要求62packet比較もPASS。今回も製品翻訳単位を含むheadless build。
製品UIの確認操作、solo表示規則、実ホストMIDI出力は未接続である。

## 再現

### 2026-09-10 追加: 通常RQ1送信待ちの接続

NativeSynth/NativeMelodicPlayerにaudio-owner用の
`setParameterOutputConnected` / `takeParameterReply` /
`completeParameterReplyTransmission`を接続した。
出力先を明示的に接続すると、返信を生成した時点で後続MIDIの消費を保留し、
実際の送信完了通知で再開する。packetを取り出しただけでは再開しない。
取得前の完了、二重取得・二重完了、待機中の切断は拒否。
通常RQ1中はRXを止めず、既存の有界受信queueに入力を保持する。
PCM・周期的音源制御は続け、後続MIDIがあるだけで毎sample起床しない。
watchdogには待機中のTX nonempty/busyを渡す。

未接続時は従来の非blockingな16packet capture queueとpopParameterReplyを維持する。
これはプラグインのホストMIDI出力を接続した意味ではない。
接続中はlegacy popを拒否し、明示完了の契約を迂回できなくした。
bulkとは通常RXを保持する点と40tickの送信後待ちがない点が異なる。

`--native-parameter-transport` PASS。実際のH8 RQ1をTX待ちに置き、同じ返信byte、
後続CC7/NoteOn保留、送信中に加えたCC10保持、完了後のCC/発音再開をnativeと比較。
H8 TX-ready IRQ補助は従来通り診断内だけ。
`--native-parameter-replies`463packet、`--native-bulk-replies`18要求62packet、
`--native-synth`もPASS。音声checksum3b54320560580fd3不変。
TX中のRX overflow、watchdogとの全位相競合、強制切断回復、実ホスト出力は未検証／未実装。

### 2026-09-10 追加: 制御ROM／波形ROM情報の応答

40:30..3f:00は03:d148の32byte識別情報を返信する。
40:30..3f:20はページ下位桁で選ぶ波形ROM領域の情報で、part音色情報ではない。
04:18c1..1929はbase=(page&15)<<20として20..27の8byte、space2個、
30..39の10byte、space2個、44の値が4なら文字4、それ以外なら文字8、space9個を出す。
00:5430はPCMの波形ROM読出しレジスタ経由のbyte読出しである。

制御ROM情報は検証済みROMからsetup時に32byteだけ所有データとしてimport。
波形情報は既存のROMアドレス解決をconstなPCM_ReadROMとして再利用する。
H8命令やPCMレジスタの書込み・ラッチ同期を製品へ追加していない。
ParameterReplyの固定容量を最大32byte応答＋header/checksumの42byteへ拡張。

通常パラメータと異なり、この2つのhandlerは要求sizeを読まず、指定長を検証しない。
アドレス3byteだけの要求もH8が返信するためnativeも受理する。
`--native-parameter-replies`は全16ページ、size0/1/32/127、size省略を含む
463件の全packetがH8 TXと一致。既存の8不正size・queue境界もPASS。
`--native-synth`はchecksum3b54320560580fd3不変、可変frame分割一致。
通常RQ1の既知レコード生成は接続済みだが、通常TX busyの統合・ホストMIDI出力は未了。
Release診断buildで製品翻訳単位をコンパイル。Xcode/Logicでの確認は未実施。

### 2026-09-10 追加: Resetレジスタの読出し

40:00:7fはd6e8のscalar record（保存位置8004）の読出しであり、
RQ1自体にresetの副作用はない。既存の設定所有者から応答するよう接続した。
起動時、bulk48で25hを書いた後、GS reset後を含む414件の全応答packetが
H8 TXと一致。既存8種の不正size・queue境界試験もPASS。
`--native-synth`は0/1/127/129/257分割で一致、checksum
`3b54320560580fd3`不変。Xcode/Logicの実行確認ではない。

### 次の責務: モデル45表示の製品接続（未実装）

現状の`DisplayData::write`はtext/bitmapを保存するだけ。
`NativeSynth::state`から`LcdCaptureBackend::captureNativeState`までに
その状態が渡されず、受信済み表示が製品LCDに反映されない。
単に文字をUIへ渡すだけで完了にせず、下記H8のライフサイクルを移植する。

- 04:37dd..38a0: 16文字以下は16桁へセンタリング（16文字ならそのまま）、
  CF32を300に設定。17文字以上は元の表示、区切り文字、受信文字を使う
  スクロール領域を組み、CEAFを有効にする。
- 04:38a1..391f: スクロールの表示窓と元表示の更新。CF30による歩進間隔は
  通常30、CDC8 bit3が立つ場合20。これはsample数ではない。
- 04:7389: bitmap受信時CF34=300。
- 00:7f20..7f80: FRT3比較割込、OCRAを2500進め、CE58の10分周後に
  CF32/CF34をzero飽和で減算する。UIフレーム数で置換しない。
- 04:39b4/39b8等: 操作モードに応じた取消経路がある。

以上は今回のROM静的再確認。表示の全状態遷移や実時間・LCD結果との
動的照合はまだ済んでいない。既存の受信バッファ一致試験はその代替ではない。

#### DisplayControlの実装と動的照合（同日追記）

`sc55_display.h`へ表示制御を実装した。短文の16桁センタリング、300tick保持、
長文の通常表示を両端へ置いたスクロール、途中の短文による置換、bitmapの
300tick保持と再受信による更新。時間と表示serviceは呼出し元が進めるため、
UIのpaintやsnapshot読出しに依存しない。ROMのPC/RAMを製品状態に持たない。

`--native-display-control`は実際のH8 MIDI受信とタイマー・表示serviceを観測し、
同じ意味のイベントをこのC++制御に与えて比較する。8件の文字受信
（1/3/4/15/16/32/3/17文字）、801回のscroll表示窓・offset・待ち時間、
2件のbitmapと3040回の保持timerが一致した。通常表示の復帰とbitmap期限切れも確認。
ログ `/tmp/sc55-display-test.log`、Release診断build成功。

これはイベント単位の表示制御の検証。製品NativeSynthのscheduler・snapshot・LCDへの
接続は次の作業であり、現時点ではまだ表示されない。高速scrollモード・全パネル取消・
resetによる取消、製品側イベント時刻、LCDピクセル配置はこの試験の保証範囲外。

#### 製品経路への接続（同日追記）

NativeMelodicPlayerがDisplayControlを所有し、受信時に更新する。
保持timerはFRT3の2500counts×prescaler4×2MCU cycles×10分周=200000cycles。
表示serviceは04:378bの20kernel tickを基準とする。描画やsnapshot読出しでは
時刻を進めない。アイドル時は表示用の追加起床なし。
H8の起動epoch・命令所要時間・taskの競合遅延は再現しておらず、
この接続をH8とのcycle単位の表示時刻一致とは主張しない。

NativeSynthのsnapshotに16桁の表示窓と64byte bitmap、有効状態を追加した。
既存の三重bufferでUIへ渡し、LcdCaptureBackendがinstrument行／CG bankへ反映する。
bitmapのbank選択と転送順は04:3065..3072/30b9の経路に従う。
GS resetの設定復元時に表示も取消す。全パネル操作の取消条件は未接続。
通常表示の名前表記は既存native表示を使い、H8の全表示モードの実装とはしない。

`--native-display-control`に実際のNativeSynth push/render/state試験を追加しPASS。
短文・bitmapのsnapshot到達、100回読んでも表示が進まないこと、エディターなしで
期限切れになることを確認。`--native-synth`もPASS、checksum
`3b54320560580fd3`不変、0/1/127/129/257frame分割一致。
Release診断targetは製品NukedSC55Emulator.cppもコンパイル済み。
Xcode配布build、Logic実画面、全LCDピクセルの比較は未実施。

#### 通常パネル操作での取消（同日追記）

NativeSynthのPART操作とINSTRUMENT操作は文字表示を取り消し、ALLは文字・bitmap
両方を取り消す。LEVEL/PAN/REVERB/CHORUS/KEY SHIFT/MIDI CH/MUTEでは保持する。
根拠は04:3a10..3a6aの通常画面経路。表示モードによる追加条件は別項目として残す。

`--native-display-control`でH8へ実際に18種のボタンを押下・解放し、各操作前に
文字とbitmapを再送。NativeSynthの同じ操作後の表示有効状態がH8と全件一致した。
さらにnativeのGS reset取消、長文表示中のzero/1/257frame分割での表示窓と音声一致がPASS。
`--native-synth`もPASS、既存checksum不変。通常の製品パネル取消は接続済みだが、
全隠し画面・全同時押しを検証した意味ではない。実ホスト画面の確認は引き続き未実施。

独立診断buildは役割マップ参照。`--extended` と `--extended-tx` を別々に実行する。
ログ: `/tmp/sc55-extended.log`、`/tmp/sc55-extended-tx.log`。
後者には表示・bulk書込一致およびRQ1 ring drainのassertionがあり実行成功。
checksumは採取packetを別途照合した。再現用probeにROMバイナリは同梱しない。
