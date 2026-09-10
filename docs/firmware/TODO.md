# H8制御のC++移植 — 残作業

最優先はH8が担当する制御機能の意味単位での移植。既存PCMのDSPを
作り直すことはゴールに含めない。判定の正解は既存H8の実行結果とする。

次の実装選定は[責務ごとの現状判定](NATIVE_CONTROL_ACCEPTANCE.md)を先に読む。
[現在の制御作業](CURRENT_CONTROL_WORK.md)は変更履歴と詳細根拠として参照する。
下記には過去の時間再現実験も残っている。未チェックの数を未実装機能数と見なさない。

## 後回し（2026-09-10、ユーザー指示）

- 隠し操作・拡張LCDメニューは後回し。音源制御のC++置換を先に進める。
  公開パネル操作・表示は引き続きmessage threadで扱う。

- MIDI出力（SysEx返信・設定一括送信）とそのUI接続は現状の対象外。
  既存の送信診断は保存するが、未完成の必須機能として追わない。
  SysEx受信による設定変更・GS reset・表示データ受信は対象内のまま。
  通常の出力未接続時はRQ1の返信を生成・蓄積しない。オフライン返信比較は
  `setParameterReplyCapture(true)`で明示的に有効化する。
  製品経路でRQ1を16回受信しても返信待ち・蓄積・overflowが起きず、続くDT1の
  音量変更が適用される試験を追加してPASS。既存の明示出力接続の比較も維持。
  ログ `/tmp/sc55-no-output-test.log`。この変更後のXcode再ビルドは未実施。

- [ ] NativeSynthのサンプルごとの互換PCM状態同期の削減。
  制御境界だけで同期する案は保留。実測した費用・必要性を確認してから再開する。
  H8制御の未実装機能より優先しない。既存PCMの演算とドラム開始保護を維持する。

## 制御機能の残り

作業単位が完了するごとにコミットする（2026-09-10、ユーザー指示）。
pushは別途指示がある場合だけ行う。

### 最新方針: 命令時間の再現は通常経路から外す

ユーザー確認により、H8エミュレーターとのサンプル一致そのものを完成条件にしない。
製品は共通周期の制御passをC++で実行し、命令数由来の計算待ちは使用しない。
PCMの発音開始・再利用待ち、EG初期化とドラムのkey-latch保護は維持する。
`timedPhase`は比較診断用に残す。以下の時間接続の記録は過去の実験結果。
ボイス選択差の既存テストは消さず、機能違反と単なる時刻差を区別して扱う。
未実装機能を優先し、細かな時刻差は実際の発音不具合の根拠がある場合に調べる。

この変更のheadless製品経路で再利用8回、他EG更新88回、0/1/127/257frameの
出力一致、reserve48発音と保護済みmono拒否、再利用中CC7／held release、
PCM通知受付を確認。コマンド順序は最初のreadback直後のstageを直接記録し、
一括passでも後の修正で見かけ上通らないようにした（backlog0/64ともPASS）。
音声処理から戻った時に命令時間待ちが残っていないことも検査する。
ログ `/tmp/sc55-semantic-pass-*.log`。Xcode／Logicでの音・CPU負荷は未確認。

### 2026-09-10: 第1／第2LFOの計算時間と乱数読取り位置を接続

以下は過去の計時実験の記録。通常passはEG・filter・pitch・levelのC++更新を
直接呼ぶように戻し、命令数計算と結果の一時コピーも通常経路から外した。
明示的なphase／timedPhase診断は結果保持・中断テストのために維持する。
直接更新と段階更新のPCM I/O比較、385toneの発音／停止／再利用を含む
`--native-voice-control-pcm-test`がPASS（`/tmp/sc55-direct-control-pcm.log`）。
製品経路のreserve48・再利用8回・PCM通知受付・可変block出力一致もPASS。
CPU負荷改善やLogic上の実演奏の確認結果ではない。

local LFO計算を`ModulationCalculation`で保持。共有元選択・解除・コピーは従来の
ownerに残し、ローカル計算を二重に実行しない。ランダム波形はE034のラッチ位置
まで進めてからPCM値を一度だけ読み、残りの計算時間後に確定する。その途中を
IRQ/MIDI受付の隙間にはしない。H8の実命令入口で10982計算と597読取り位置が一致。
H8で観測できた波形は0/1/3/4/5。全7波形のC++即時計算との一致も別に確認。
PCM385tone、release111、reserve48、startup8／他EG90、通知受付、block一致、
コマンド優先順、準備中MIDIがPASS。`/tmp/sc55-lfo-*.log`。
共有／解除などの周辺処理・controller・readback／publish・他CPUサービスの時間は
まだ未接続。625cycle単位への切上げも残り、全体の完成とはしない。

### 2026-09-10: 4つの計算本体を製品のデバイス時間へ接続

通常経路が`timedPhase`を使用。入力依存の命令数×12の計算本体時間をPCMの
実進行量で減らし、完了後に結果を確定する。途中はIRQ／MIDI等の制御を保留し、
PCMと次周期の時計は進める。周辺の未計時phaseへ固定sample待ちは追加しない。
全体は未完成。LFO・controller・readback／publish・他CPUサービス等はまだ未計時、
完了はPCMの625cycle単位へ切り上がる。通常capacity40の66/67差は残る。
PCM385tone、release111、reserve48、startup8／他EG101、通知受付、block一致、
コマンド優先順と4計算の完了直前／完了時テストがPASS。
`/tmp/sc55-timed-calculation-*.log`。実ホストとCPU改善は未確認。

### 2026-09-10: 周期計算の結果と公開を製品経路で分離

続く接続でruntime自身がslot・計算段階・結果・処理量を保持するようになった。
phase再開ではcalculateとcommitを分け、通常passも同じ段階を連続実行する。
計算後に入力ticksを変更しても再計算せず、独立snapshotと同じ結果を確定する。
途中結果がある間の発音準備開始を拒否し、宛先の再設定を保留する。
runtime snapshotは自分の結果を値で所有する。音声所有者を共有するpointerはない。
両target build、PCM385tone、release111、startup8／他EG88／block一致がPASS。
`/tmp/sc55-runtime-calculation-*.log`。段階化に伴い、1呼出し=1計算と仮定していた
試験はcalculate/commitの両方を進めるよう更新した。音声／停止の期待値は維持。
時間待ち、全体の実行時間配分、通常capacityの修正は未完。

`VoiceParameterCalculation` がEG・filter・pitch・levelの計算結果を型付きで所有する。
既存の`CalculateVoiceControlStage`もこの計算を一度だけ呼び、担当する状態だけを
commitする。全ボイスsnapshotの巻き戻し、入力参照の保持、二重計算はしない。
move元を空にし二重commitを拒否。停止stageになった宛先には古い結果を反映しない。
shared LFOとPCM操作は既存の継続ownerを維持し、この純粋計算には入れない。
通常経路はまだ即時commitで、デバイス時間の待機／実行時間モデルは未接続。
この変更をcapacity不一致の修正やCPU改善として扱わない。
両診断target build、4計算の公開保留／move／二重commit／停止保護、既存PCM385tone、
release111、startup8／他EG88更新／0・1・127・257frame一致がPASS。
`/tmp/sc55-parameter-*.log`。Xcode／Logic／CPU実測はしていない。

### 2026-09-10: PCM通知の受付と遅延処理を分離

NativeVoiceEngineが固定長のボイス別通知を保持し、再利用待ち中でもPCMの
単一IRQラッチをacknowledgeする。実際の境界処理は発音準備／再利用待ちが
終わってから、ボイス操作を優先してslot23から降順で実行する。
停止／波形再設定が消した通知は再実行しない。ROM根拠はCPU_SERVICESの
「PCM通知の受付と処理」。PCM演算・key-latch保護・製品時計は変更なし。
両診断targetのbuild、明示的な2デバイス通知の待機中受付、合流／降順／
古い通知破棄、既存PCM制御385tone、release111、reserve48／protected mono、
startup8／他EG88更新／0・1・127・257frame音声一致がPASS。
ログ `/tmp/sc55-boundary-reception*.log` と `/tmp/sc55-boundary-mailbox-pcm.log`。
自然なH8 MIDI入力では再利用待ちと通知受付の重なりはまだ観測できていない。
通常capacity40のボイス選択差の解決や、Logicでの改善を意味しない。

### 旧方針の診断記録: 割込み側も含む実行時間モデル（通常経路には追加しない）

最新切分け`end-clock-start`はH8の開始時刻でC++共通時計の回数を確定し、
H8完了時刻まで保持する。H8の経過回数は入力に使わず、48発音・保護による
拒否・発音復帰までPASS。対象windowは9pass／25周期。時刻replayはまだ残り、
通常製品や音声全体の合格ではない。`/tmp/sc55-elapsed-entry.log`。
重要な訂正: 製品にもeffects前の`effectPassClock_`とruntimeの`controlTicks_`が
既にあり、開始時回数と次回分は分離済み。時計や保持ownerを重複追加しない。
次は既存の意味単位の処理の開始・再開・完了をデバイス時間へ接続する作業。

追加切分け: PCM通知修正後の同一binaryで、同じH8周期完了時刻に対し
経過回数だけを実測値／毎回1へ変更。実測値では48件と拒否／復帰がPASS、
毎回1では40件目の66/67差が再現。対象windowは両方9pass・合流0、経過回数は
26対9。時刻を合わせるだけでなく、処理待ち中の周期回数の蓄積が必要。
回数だけを通常native時刻に与えれば十分、という証明ではない。
`/tmp/sc55-elapsed-{observed,unit}.log`。製品への回数強制やreplayは入れない。
続く`end-clock`はH8回数を使わず、C++の共通時計を音声frame数で進め、
H8完了機会で蓄積回数を回収する診断。対象windowは同じ9pass／26周期でも
40件目が不一致。過去windowと各passへの回数配分・初期位相は同一ではない。
合計回数だけで十分とはいえず、開始時の回数確定と、実行中に来た次の通知の
分離が必要。完了時回収を製品仕様にはしない。`/tmp/sc55-elapsed-clock.log`。

優先順修正後も通常capacity40は不一致。診断のみで周期完了時刻と経過回数を
H8に合わせると48件・リザーブ拒否・復帰まで一致するため、割当式を変更しない。
全周期区間を実命令入口で分類したところ、24ボイス開始146周では音源制御68.6%、
表示関連task4/7が15.2%、hardware IRQ10.4%、scheduler3.5%、その他2.3%。
未分類時間0。平均460327cyclesのうちtask8自身は315800cycles。
音源制御の計算量だけを時刻へ接続しても、割込み側を欠いたままでは完成しない。
既存の意味単位の処理を共通のdevice時間へ接続することを中心作業とし、
平均値の固定待ち、記録時刻replay、H8命令／kernel taskエミュレーションの復活はしない。
JUCE GUI処理をaudio callbackへ移す意味でもない。
詳細とコマンド・ログはNATIVE_CAPACITY_MODEの先頭。製品動作の修正ではなく、
今後の実装対象を確定する診断結果。通常capacityの合格は引き続き未達。

### 2026-09-10: 製品dispatcherのコマンド／周期順を接続

`serviceCommandWork` を周期effects／voice制御より前へ移した。
task1のコマンドリング（07e8–0850）がtask8（5af1）より優先されるため。
以前はNote Offと周期イベントが同時にreadyだと、旧状態のEGを1回進めてから
Note Offを処理していた。製品入口の回帰テストでは旧stage4、修正後stage12。
64コマンドの処理上限を越える入力でも同じ問題を確認し、処理可能なキューが
残る間は周期イベントをconsumeしないよう修正。待機中のadmission／PCM再利用／
トランザクションは区別し、他ボイスの周期更新を妨げない。
既存の固定長キューと回数上限は維持。推定待ち・H8時刻replay・PCM演算変更はなし。
これはready状態の優先順の接続であり、各操作の実行時間モデルの完成ではない。
検証ログ `/tmp/sc55-command-control-*.log`。製品TU buildと単独／64コマンド競合の
順序テストがPASS。startup8・他EG88更新・0/1/127/257block一致、準備中のMIDI／
hold release、release111、reserve48／protected monoもPASS。
単純な順序移動の段階でbulk/reset8転送1864byteもPASS。
capacity40のH8 key66／native key67差は最終コードでも残る（exit134）。
Xcode／Logicでの確認やCPU負荷の改善測定はしていない。


### 2026-09-10: 終了通知の優先順を訂正

以前の「終了mailboxをコマンド／MIDIより先に消費する」は誤り。
ROM 00:07c7–0850はevent0（コマンド）をevent1（終了通知）より先に選び、
コマンドリングを空になるまで処理してから待機へ戻る。MIDI解析は別のtask0。
NativeVoiceEngineはqueued command／admission／fanoutがある間の返却を保留し、
MIDI解析入口での暗黙の返却を削除した。周期制御中に新たに終了が発生する場合も、
返却を保留してtask1へ譲る。以前の「返却優先」を期待したテストをROM根拠で訂正。
この修正は全体の実行時間配分・capacity40不一致の解決を意味しない。
検証: `/tmp/sc55-command-event-order-*.log`。両診断targetのbuild、既存PCM制御と
キュー／継続admission／周期更新中の新規終了を含む順序テスト、H8 release111件、
reserve48件・protected mono、startup8件・他EG88更新・0/1/127/257block一致がPASS。
通常capacityは再実行でも40件目でH8 key66／native key67の差が残る（exit134）。
Xcode／Logicでの確認とCPU改善の測定はしていない。PCM演算・開始保護は変更なし。

- [ ] 共通タイマの位相と、発火後の処理待ちを分離してスケジューラへ接続。
  共通位相の所有は接続済み。`ControlTaskClock` が位相と制御イベントを分離し、
  activationの次tick・bulk送信間隔・周期イベントが同じ位相を使う。
  制御イベントのconsumeは位相をresetしない。観測イベントreplay中も位相は進む。
  activation専用の診断補正状態は削除した。初期位相は明示入力可能だが、
  H8 boot完了位相や処理待ち時間を推定して製品へ設定してはいない。
  `/tmp/sc55-common-phase-*.log`。startup8／他EG88、bulk/reset8転送、checksum維持。
  現在のcapacity40不一致は再現。対象92/39の受付をH8停止時刻へ合わせるだけでは
  slot7/8差は消えない。共通tickの位相も診断上で合わせると当該イベントは通過し、
  後続90/42の別のreadback差まで進む。H8発火202.906→poll206.842 frameであり、
  タイマ位相とdispatch遅延は別。固定待ち追加やready条件変更では対応しない。
  `/tmp/sc55-admission-timing-*.log`、詳細はNATIVE_CAPACITY_MODEの先頭。
  診断のみで、製品へ観測時刻再生・待ち時間は入れていない。
  後続90/42のframe167差はLFO失敗ではなく、slot16のkey-latch待ちによるdeferred。
  同時刻のH8 slot16はstage18、Nativeは次のkey-latch待ちへ先行している。
  診断がdeferredをfailedへ丸めていた点を修正し、元のreadback assertionは維持。
  `/tmp/sc55-readback-cause-owner.log`。ドラム頭欠けを防ぐkey-latch保護は外さず、
  後続発音の時間配分も含めて接続する必要がある。

- [x] 受付済みVoiceCommandとPendingAdmissionの所有をNativeVoiceEngineへ移管。
  元の受信key／tone、held-key return、source探索済み判断、同音回収済み状態を保持。
  fresh melodicの事前判定→一度だけ同音回収→再判定を `previewMelodicAdmission` に集約。
  mono/sourceのfresh分岐も同じ `retireAdmission` を使用。GS resetでは受付済みFIFOと
  admissionを明示的に保持し、以前と同じ順序で処理する。
  `/tmp/sc55-admission-progress-*.log`。release111、reserve48／protected mono、
  startup8／他EG88、bulk/reset8転送1864byteがPASS。全イベント時間配分は未完。

- [x] PCM開始待ちの再確認期限と波形境界イベント応答をNativeVoiceEngineへ移管。
  `serviceActivation` が再利用失敗時の次の共通kernel tickを保持する。
  プレイヤーは期限までPCMを進め、key-latch保護は従来どおりPCM passごとに確認。
  `handlePcmBoundary` が音程／停止処理とlifecycle・両LFO stageの反映を一括所有。
  EG計算中5段階の割込みテストもこの製品入口を通す。開始準備中は境界処理を保留する。
  後続修正で受付を分離し、再利用待ち中はacknowledgeしてボイス別通知を保持する。
  `/tmp/sc55-activation-owner-*.log`。開始待ち8件・他EG更新88回、音声block一致、
  bulk/reset8転送1864byteがPASS。時刻モデルを変更する作業ではなく、全時間配分は未完。

- [ ] 発音側の割当・停止・設定反映・DSP準備をスケジューラの継続処理へ接続。
  `resumeNotePreparation` をsample→DSP→PCM開始待ちまで一つの継続入口に接続。
  dispatch結果とpitch履歴もruntimeで保持し、両partialとDSP全体の完了時だけ返す。
  通常経路も同じ最大5段階を連続実行。途中再開の各段階で別発音／周期制御を保留し、
  初期入力を変更しても保持内容が変わらないこと、PCM書込み列の一括一致を確認。
  `/tmp/sc55-full-preparation-*.log`。build／PCM／release111／startup8・他EG88がPASS。
  製品時間配分は引き続き未完であり、固定遅延や音声演算変更は追加していない。
  EG/LFO/pitch準備も `NormalVoiceDspPreparation` へ分離しruntimeが所有。
  全record検証とcontroller設定→共有第1LFO→各ボイスのEG／第2LFO／pitchの順序を保持。
  継続元のDSPは値で保持し、live ownerの借用pointerを再開越しに残さない。
  `beginNormalPreparation`／`resumeNormalPreparation` が途中状態を予約し、完了後だけ
  PCM開始待ちへ渡す。通常経路も同じ処理を連続実行する。
  一括／段階PCM書込み列一致、入力破棄・二重開始防止と既存PCMテストがPASS。
  product TU build、release111、startup8／他EG88／block一致がPASS。
  `/tmp/sc55-dsp-preparation-*.log`。実行時間配分は未完で、発音時刻は変更していない。
  `MelodicSampleInstallation` を追加。サンプル／宛先を副作用なしで検証して
  固定長の準備内容を所有し、パーシャル順に停止・設定反映を再開できる。
  既存 `PrepareAndInstallMelodicSamples` もこの同じ処理を連続実行する。
  2partialが同じslotを使うケースの一括／段階実行でPCM書込み列とallocatorが一致。
  完了後の再呼出しは再書込みしない。slotを他の発音が回収しない予約は呼出側の責務。
  デフォルトの実行時刻はまだ変更していない。固定待ちや実測時刻の製品再生は追加なし。
  両target build、既存PCM制御と追加の段階反映、release111比較、startup-wakeがPASS。
  `/tmp/sc55-install-continuation-*.log`。
  続いて通常／mono再利用／rhythmをruntime所有の `NotePreparation` へ接続。
  選択・DSP入力・sample継続を固定長で保持し、部分反映後の再受付・回収・周期制御を保留。
  完了時はそのままDSP準備／PCM開始待ちへ渡す。通常経路も同じ段階を連続実行する。
  PCM再利用待ちでは従来どおり他ボイスのEGを動かす。入力寿命、二重実行防止、
  一括／段階のPCM書込み列一致を検証。`/tmp/sc55-owned-preparation-*.log`。
  スケジューラの実行時間配分はまだ未完であり、この項目全体は完了扱いにしない。

- [x] 発音準備履歴の所有と確定処理をNativeVoiceEngineへ集約。
  partごとの前回キー、参照キー、slotごとの前回pitchとdrum map/keyを所有。
  melodic／mono/source／rhythmの3つの履歴更新ループを共通の確定処理へ置換。
  deferred／不完全な準備で履歴を変更しない追加テスト、既存PCM制御、release111、
  bulk/reset8転送1864byte、startup-wakeがPASS。両診断target build成功。
  `/tmp/sc55-preparation-owner-*.log`。既存resetの保持／初期化の違いは維持。

- [x] モノ演奏状態と再始動／Note Off判断をNativeVoiceEngineへ集約。
  押下キー・現在音・velocity・portamento/sourceをエンジンが所有。
  プレイヤーからEG完了ラッチの直接確認とmonoグループ解放判断を削除。
  実MIDI/H8 release111比較、準備中のmode変更、追加のEG再始動／最高音戻り、
  既存PCM制御とstartup-wakeがPASS。`/tmp/sc55-mono-owner-*.log`。
  発音準備全体の所有権整理と制御時間配分は引き続き未完。

- [x] 同音回収・容量確保・グループ／パート停止をNativeVoiceEngineへ集約。
  プレイヤーによる直接の回収操作とEG状態コピーを削除。live EGと未消費の
  停止／準備要求の所有状態を共通の判断で選ぶ。追加所有状態テスト、既存PCM制御、
  reserve48発音・protected mono拒否・startup-wakeがPASS。両診断target build成功。
  `/tmp/sc55-admission-owner-*.log`。capacity40の66/67差は再実行でも残るため未解決。
  詳細は [NoteGroup所有権](NATIVE_NOTE_GROUP_STATE_2026-09-10.md)。

- [ ] ボイス終了通知と返却のスケジューリングを完成させる。
  EG終了／PCM停止確認からの通知をruntime内の単一mailboxへ分離。
  段階実行では次の再開でアロケータが返却し、通知直後には空き扱いにしない。
  一括実行は同じ通知を呼び出し内で消費する。pending中のスキャン再開は防止。
  `/tmp/sc55-return-phase-pcm.log` は通知前後の使用状態・一度だけ返却する追加テストと
  既存テストがPASS。product TU buildとstartup-wakeもPASS。全イベント優先順位／
  時間配分とMIDI・再割当てとのinterleaveは未検証。PCM待ち時間は変更していない。
  過去にMIDI受付入口 `NativeVoiceEngine::serviceMidi` からも終了mailboxを消費したが、
  task0／task1の混同だったため撤回した（上の訂正参照）。消費実装自体はruntimeに集約。
  `/tmp/sc55-completion-midi-test.log` は、MIDI受付時の空き数と、次の割当後に
  通知を二重消費しない追加検証を含みPASS。直接の発音APIとの競合や
  全体の時間配分は引き続き未完。通常のPCM演算・開始待ち時間は変更なし。
  `serviceVoiceCommand` の実行／再開入口からの無条件消費も訂正。
  現在はキュー／継続中のadmission／fanoutが空のときだけ通知を消費する。
  終了したslotを含む全24slotを再割当てしてから旧passを再開し、新しいownerが
  返却されない追加テストがPASS。`/tmp/sc55-command-completion-pcm.log`。
  product TU build、startup-wake、準備中MIDI／held releaseもPASS。
  これは通知順序の接続であり、全フェーズの時間配分を実装したものではない。

- [ ] 周期制御の継続状態を製品スケジューラへ接続する。
  `NativeVoiceEngine::resumeControl` を追加し、通常のserviceControlと
  停止／準備要求の受け渡し・制御結果の反映を共通化した。段階再開は時計を消費しない。
  診断のgroup再開もこの製品入口を通し、runtimeへの直接操作と別の状態コピーを削除。
  追加の途中停止・時計保持と既存PCMテスト、startup-wakeがPASS。両target build成功。
  `/tmp/sc55-control-owner-*.log`。group時刻replayの92/39 frame238差は依然残り、
  時間配分の実装完了とはしない。
  製品のpassを選択/controller、第1LFO、paired転送、readback、各計算、公開へ分割。
  第1LFOの共有解除は `FirstVoiceModulationUpdate` をruntime内で保持し、再開時の
  stage変更なら古いLFO更新をスキップする。一括実行は同じ段階を連続処理する。
  `/tmp/sc55-first-phase-detach.log` は既存PCM制御テストと途中停止の追加テストがPASS。
  product emulator TU buildと `--native-startup-wake` もPASS。待ち時間は未導入。
  第2LFOもruntime内の `VoiceModulationUpdate` を保持する形に接続。
  共有解除時だけ中断し、再開時のrelease/stopで旧LFOを進めない追加テストがPASS。
  `/tmp/sc55-second-phase-pcm.log` と `--native-startup-wake` がexit0、product TU build成功。
  第1/第2LFOとも通常経路は同じ段階を連続実行する。全イベント時間配分は未完。
  Xcode/Logicでは未確認。

- [ ] LFO全体の計算量と共有ボイスの制御順序を接続する。
  `modulation-control.h` で実MIDI入力からのLFOブロック全状態を照合。
  `/tmp/sc55-modulation-control.log` は6755回一致、sine3528回・sample/hold3227回。
  PCM乱数を読んだ461回は、実際の読出し値を外部入力として渡し、C++の読出し判断も一致。
  続いて `modulation-work.h` で入力依存計算量を追加。実音色のLFO設定から追加音色を選び
  MIDIで鳴らした `/tmp/sc55-modulation-shapes.log` は20539回の全状態・計算量が一致。
  波形0/1/3/4/5、乱数読出し804回、38種類の計算量。波形2/6はこの入力では未観測。
  共有処理全体と製品スケジュールは未完。計算量は診断用で、製品に待ちは入れていない。
  第2LFOの共有判定・状態コピー・共有解除時の全フォロワー付替えも追加照合。
  `/tmp/sc55-modulation-routing-shared.log` はlocal11435/shared58/detached2の状態・計算量が一致。
  実音色の共有設定を選び、同音を重ねてNote Offする入力。H8 RAMの書換えなし。
  解除後の割り込み区間／生存確認と、第1LFO共有全体は別途未完。
  `first-modulation-routing.h` で第1LFOのrouting/paired転送を追加照合。
  `/tmp/sc55-first-routing-chord.log` はlocal5238/paired573の全状態が一致（exit0）。
  第1LFOのshared/detachedは0件であり完了扱いにしない。音色選択はcommon[2]を参照。
  以前の波形カバレッジ探索がpartial[14]を参照していた点も修正した。
  初期化コピー3d1aをtask2で別観測するprobeを追加。
  `/tmp/sc55-lfo-initialization.log` はコピー6件の全状態が一致し、継続共有flagは全件0。
  周期側はlocal5238/paired573、shared/detached0。初期の状態コピーと
  周期的な共有元参照は別の挙動であり、初期コピーは上記範囲で接続・検証済み。
  38a2はflagをclear、3d68はsourceのflagをcopyする。共有modeの音色を重ねただけで
  周期共有が必ず発生すると仮定しない。全ROM経路で到達不能との証明ではない。

- [ ] 制御フェーズの入力依存計算量とイベント順序を製品へ接続する。
  出力制御全体（音量合成・ランプ・パン・送り量）の診断モデル
  `voice-output-work.h` を追加。実MIDI/GS入力3369回で全状態と計算量が一致、
  41種類の計算量、固定パン124回・ドラム係数150回・パン移動305回・送り移動218回。
  `/tmp/sc55-output-work-coverage.log` はexit0。capacity側7471回も一致するが
  既存の割り当てFAIL40は残る。LFOと別イベントの計算量／処理順は未完。
  部分的な待ち時間を製品へ入れて完了扱いにしない。PCM処理は変更していない。
  振幅EGも `amplitude-control-work.h` で全状態と計算量を照合。
  controller入力3377回（自然終了8回、release1330回）・35種類が一致。
  capacity側7471回・19種類も一致。発音待ち分岐はこの入力では未観測。
  `/tmp/sc55-amplitude-work-{variable,capacity}.log`。返却処理自体と発音待ち末尾の
  割り込み解除／外側へのreturnは別境界であり、この計算量に含めていない。

- [ ] ピッチ制御の全体照合を完了する。
  `pitch-control-work.h` にピッチEGからPCMピッチ値までの入力依存計算量モデルを追加。
  実MIDI入力の `--native-controller-work` はCC65/CC5と上下のNote On/Offを含む。
  2417回の状態・計算量比較が一致、うちポルタメント動作中520回、58種類の計算量。
  capacity側も7471回・26種類が一致。既存のボイス割り当てFAIL40は残る。
  ログ `/tmp/sc55-pitch-work-{variable,capacity}.log`。このモデルは診断用であり、
  製品の処理時間配分やCPU負荷は変更していない。未観測の分岐は完了扱いにしない。

- [ ] タスク5／6の起床条件と担当処理を確定する。
  初期化00:0261–02a0は入口表00:0778の各4byteを直接スタックへ設定する。
  5／6の入口00:0542はこの表で指定されており、「全slotへ仮入口を設定後、
  登録済みtaskだけ上書きする」とする根拠はない。
  起動後に実行されないという観測だけで未使用とは判定しない。
  通知元・待機解除条件を確認し、到達する制御責務があればC++側へ接続する。

- [x] ノート単位の状態をNoteGroupへ集約。
  [所有権と検証](NATIVE_NOTE_GROUP_STATE_2026-09-10.md)。7本のRAM由来配列を
  単一のノート状態へ置換し、Note Off可否・hold保持・同音回収markの操作も集約。
  割当・停止・リザーブ・音声の既存比較は維持。時間モデルの問題とは別の変更。
- [x] 物理ボイスの割当状態をVoiceAllocationへ集約。
  所属part/group、解放要求・公開command、空きリストをslot単位で所有する。
  初期化・再準備・返却で消す状態の違いと、PCM再利用待ちを維持。
  [構造と検証](NATIVE_NOTE_GROUP_STATE_2026-09-10.md)。

- [ ] ボイス再利用中の周期制御とMIDI処理順序のH8互換性。
  診断にselection→readback/hold→group公開を分ける `SC55_REPLAY_CAPACITY_PASSES=holds`
  を追加。`SC55_REPLAY_MIDI_INGRESS=1`併用でも92/39のframe216で
  H8はslot7、nativeはslot8を読み戻す。直前の選択frame196ではslot7のstageは6/6、
  slot8はH8停止準備18／native発音中2。単なる公開時刻差ではなく、再利用の
  stage18→発音への移行が選択より前に分岐している。次はこの移行とgain readinessを照合。
  `/tmp/sc55-control-selection-hold-ingress.log` は意図通り不一致で停止（exit134）。
  normal NativeSynth回帰はPASS。記録時刻は診断専用で、製品スケジュールは変更なし。
  readiness追跡で、H8停止29.626／gain零178.944／再発音208.243frameに対し
  native停止15／gain零160／再発音189と判明。零から成功pollまでの待ちは27／約28frame。
  再利用待ちの欠落ではなく、上流の割当・停止開始が早い。固定の追加待ちは入れない。
  `/tmp/sc55-reuse-readiness.log`。task8だけでなく発音側の意味単位の実行区間が必要。
  パネル転送統合後もcapacity admission40の66/67差を再確認。
  このfixtureで欠けていたEffectsTablesを製品と同じく渡すよう修正したが、差は残る。
  同時にreadyな発音／周期制御の優先順を診断用に逆転したところ、該当競合0回で
  同じFAIL。単純な優先順変更はこの再現の解決にならない。診断変更は削除済み。
  次はPCM時間が進む中での制御処理・公開時点。詳細はcapacity資料の冒頭参照。
  実行H8の7471更新で、readback後から出力公開までPCM振幅・ff00 commandが
  維持されたまま計150497 PCM frameが進むことを確認。平均停止区間は
  12589 emulated cycles（全停止時間の下限）。制御group全体を瞬時に移すだけでなく、
  readback／holdと公開を別時点として扱う必要がある。固定待ち時間は追加していない。
  読み戻し・計算・公開を保持する段階実行を製品のVoiceControlRuntimeへ実装。
  通常経路も同じ実装を使い、ペアの両計算後に両出力を反映する順序を維持する。
  PCMを途中で進めるholdテストと、一括／段階実行のI/O比較を追加した。
  通常経路はまだ待ちなしで段階を完走する。時間配分・途中のPCMイベント／発音要求の
  順序は未解決で、capacityのFAIL40は残る。これを修正完了とは扱わない。
  H8はLFO／振幅EG／フィルタEG／ピッチ／音量の間で割り込みを許可し、停止を再確認。
  capacity実行でも振幅EG入口で停止して後続を省略する1例を観測した。
  C++計算も5段階へ分割し、各入口でPCM終端の停止を上書きしないよう修正。
  新しい段階実行の停止テストは修正前FAIL、全5入口で修正後PASS。
  時間配分／イベント配送はまだ未接続で、通常レンダリングは引き続き待ちなし。
  上のH8途中停止はtask1のslot7停止→再割当だった。全taskの実入口を観測して確定。
  NativeVoiceEngineのタイマー付き制御にも1段階実行を接続し、途中の停止／新part・key
  設定とtask2要求が制御側へ渡ること、後発の周期イベントを消費しないことを検証済み。
  計算中の発音管理を全面禁止する方式ではなく、この引継ぎを使う時間モデルが必要。
  5計算本体と段階間を実入口で分離計測。capacityでは計算59962212 cycles、
  段階間27744108 cycles（うち制御自身3586272）。計算本体のwall=ownで、
  他task/IRQは段階間に入っていた。各gap自身は96 cycles、wallは最大135540。
  可変controllerでも同じ分離を確認。計算時間とイベント処理時間を混ぜずに扱う。
  最大だったフィルタEG／出力の全計算を、入力値から計算量を求める診断モデルにした。
  capacity7471回と、CC72／Note Offを追加した1713回で状態・出力・計算量が一致。
  拡張fixtureはrelease338回／終了段階22の1回を含む73種類の計算量を検証。
  他4計算とイベント実行の時間モデルは未完で、製品の待ち時間にはまだ使用しない。
  MIDI解釈と発音準備の分離を開始。
  `--native-midi-during-preparation`で最大発音から再利用待ちに入り、
  実MIDIのCC7を送る比較を追加。修正前H8=1/native=0、修正後は両者とも
  待機終了前に音量反映。controller更新を独立関数へ移し、PCM準備待ちから切り離した。
  H8のRAMや待機時間は変更していない。単に全イベントの待機を外すのではなく、
  Note On/Offを`VoiceCommands`へ接続。MIDI入力のfan-out・受信gate・velocity補正と、
  発音側の割当／mono／rhythm／停止を分離した。PendingAdmissionも生MIDIではなく
  受信済み要求を所有し、別のNote Offパートmask/event状態は削除した。
  待機中に次のNote On→CC7を送る比較も修正前FAIL／修正後PASS。
  キューは完全なfan-outをまとめて公開し、容量不足なら無変更で再試行する。
  受信時snapshot・FIFO・wrap・部分公開の禁止と、後続ノートの完了を確認。
  hold/sostenuto/portamento・設定可能なportamento source・All Notes Off/All Sound Offも
  型付き要求として同じFIFOへ接続。受信側ではrouting/receive bitsを確定し、
  発音側で先行ノートとの順序を保って適用。MIDI→ノート→hold→CC7の再利用待ち比較と、
  All Notes Offをholdで保護してhold-offで全24voiceを返却する比較がPASS。
  Program Changeも受信時の選択とProgramVoiceRequestへ分離済み。
  04:0a24/0a2e/0a3cに従い、有効toneが変わった時だけ対象part/toneを保持して投入。
  capacity mode・reuse無効化・pitch履歴は00:08da側の責務として順序付きで適用する。
  drum PCは04:0940..0ab3と同様、受信側で共有mapを更新する。
  受信済みdrum Noteを後続の無効kitで再拒否する判定も除去（00:0c3cはlive mapを参照）。
  新規`--native-program-during-preparation`は修正前FAIL／修正後PASS。
  再利用待ち中のNote→PC→CC7、確定tone/capacity mode、受信済みsnareと後続無効kitを比較。
  H8 drumのMIDI keyはA1B6で確認。C8FCは変換後keyなのでMIDI key比較に使わない。
  MIDI mono/polyも受信側mode bitとPartModeRequestに分離済み。
  00:2837/285eで受信値を変更し、094b/095d相当の停止・held-key初期化はFIFOで実行。
  同じMIDI modeの再送も停止要求を出す。GSの同値を無視する既存契約とは分ける。
  通常message（Note/CC/Program/Pressure/Bend）の入口をreceivePerformanceMessageへ統合。
  CC whitelistの二重dispatchを削除し、CC5や割当controller、無処理CCも準備待ちから分離。
  続けてSysExの受信と停止不要な設定更新も発音準備から分離。
  master/controller/FX/reserve/drum map/model45を受信側で適用する。
  GS/GM reset、RQ1送信はEOXで順序待ちを維持。
  完成packetの固定長previewで判定し、deferred時はreceiver/入力を無変更にする。
  `--native-sysex-during-preparation`は最大発音中の32件連続Note→master DT1で、
  H8が発音command未処理でも設定を更新することを確認。nativeは修正前全件待ち、
  修正後は31件のqueued workが残る間に更新。fragment/checksum拒否/後続CCも比較。
  最初の単一reuse試験ではH8側にpacketが間に合わず、待機仕様の証拠にはしなかった。
  接続後のparameter transport、bulk18要求/62packet、reset後を含むRQ1 463packet、
  shared rhythm42件、native-player9,034,172frames、全partの13kickアタック比較はPASS。
  全SysExの競合順序や全曲の音声一致を確認した意味ではない。
  GS part設定も受信／発音の分離へ接続。既存PartSettings decoderで副作用を
  preflightし、最大3件のcommandを全件確保してから設定値を更新する。
  channel変更は04:1194..11a0のcontroller reset→All Notes Offの順序を保持。
  音色変更は有効toneの変更時だけProgramVoiceRequest、mono/polyはbit変更時だけ
  PartModeRequestを投入。drum map変更の設定側責務は維持し、同期mono実装は削除。
  不正な後続recordが先行recordをrollbackしない既存仕様も同じdecoderで保持する。
  `--native-part-during-preparation`でH8/nativeとも発音要求が残る間のpart volume更新、
  同一channel再指定によるhold解除／全voice返却と後続expression維持を確認。
  release111件、shared rhythm42件、RQ1 463packet、パネル42,015設定比較もPASS。
  bulk48再設定もProgramVoiceRequestへ接続。04:1617..165aの降順part再選択を
  受信側で行い、有効toneの変更分だけ発音側へ投入する。全件の容量確保前には
  設定値を変更しない。共有drum mapによる後続partのprogram変更はlive値を使用。
  `--native-bulk-system`で8転送の全1,864byte／reset比較と、48件Note直後の
  bulk master更新を確認。H8 command cursor=32/08、native残47件で受信反映PASS。
  PCMの同期削減・演算変更は行っていない。この追加はheadless buildで確認し、
  下記の既存Xcode製品ビルドにはまだ含まない。
  受信回復の順序待ちは維持し、未処理transactionを後続入力が追い越さない。
  新規mode比較は、準備中の切替・後続CC7・mono→poly発音・同値mode再送の停止を確認。
  monoは物理partial数ではなく1つのnote groupで検証。H8/nativeのgroup/keyと空き数を比較する。
  modeは準備を停止するため、H8の通常再利用完了573eを必須条件にはしない。
  即時の設定更新と後続の発音処理を分ける必要があり、丸ごと遅延実行しない。
  CC121は04:0844のexpression/soft/selector/contribution/pressure初期化と、
  00:08baのportamento/source/hold/retained-key解除を分離済み。
  前者はMIDI受信側、後者はControllerResetRequestとして発音側で実行する。
  全対象part分のキュー容量を確保するまでは制御値も変更しない。
  再利用待ち中のNote On→CC121→expression→hold→volume比較は修正前FAIL／修正後PASS。
  後続expressionが即時反映され、遅延したresetに上書きされないこともH8と一致。
  GS/GMの全体resetに必要な待機・入力保留はこれとは別であり維持している。
  これらも含めたtask0/task1の完全な分離は残る。未処理設定を勝手に飛ばさない。
  Note Onの00:214a/2156/2164–2189では、受信時のvelocity補正・key range・
  選択tone wordを確定してpart/key/velocityと共に投入する。
  生のMIDIを第二キューに移すだけでは後続設定変更を誤って参照するため不十分。
  最新のnote要求経路でrelease111件、パネル準備待ち／backpressure、
  55KTIZKE全partの13kickアタック比較がPASS。hostでの聴感は未確認。
  pedal/part解放統合後もrelease111件・source競合768件・13kick比較がPASS。
  native-player-testの9,034,172 PCM framesもH8実行なしでPASS。
  release111件、source-controller768件、再利用8回／他EG更新88件、
  zero/1/127/257frame分割音声一致、55KTIZKE全partの13kickアタック比較はPASS。
  この差とcapacity admission40の因果関係はまだ未確定。
  再利用失敗後の約1ms周期待機を接続済み（00:5710–573c、task2周期1）。
  毎sampleの再利用判定をやめ、共通時計の次tickで再開する。
  Key-latch待機・2PCM-passのアタック保護は変更なし。
  新規`--native-startup-wake`は修正前FAIL、修正後8回の再利用と
  最大発音中のzero/1/127/257frame分割音声一致がPASS。
  release111件・reserve48件もPASS。ただし通常capacityのadmission40の差は残る。
  続けて製品の周期制御を再開可能passへ接続。再利用待ちの間に他のEGを進める。
  待機slotを除外し、連動先待ちでは進捗とelapsedを保持。反映は今回変更したslotのみ。
  新規回帰は修正前「全EG停止」でFAIL、修正後88件の他ボイス更新を確認。
  55KTIZKE全part60秒の13kickアタックもH8と一致。別スレッドは導入していない。
  未処理の物理停止についても、停止stage18/20を周期更新側へ公開して全pass延期を除去。
  単独／連動の両ケースで走査・途中退出・後続の停止要求消費を検証済み。
  未処理の準備要求も既存DSPがある枠は公開済みstageで更新可否を判断する。
  H8の116b/1173→5c22/5c38に従い、controller参照は新しいinstalled part/keyを使用。
  継続／再発音の両ケースで要求保持と制御更新を確認。旧identityの別コピーは作らない。
  既存DSPがない枠の準備延期と、H8の実行時間差はまだ残る。
  [詳細と再現](NATIVE_CAPACITY_MODE_2026-09-10.md)。通常の
  `--native-capacity-stealing` はadmission 40の選択差が残る。
  診断限定のgroup replayではadmission 34で新規ボイスの更新対象に差が残る。
  H8は準備待ちスロットを走査時にスキップし、準備完了後に同じpassで再訪しない。
  予約先の除外をresumable経路へ反映し、限定テストは通ったが、差全体は未解決。
  実際の聴感への影響は未確認。製品の周期を診断トレースに合わせる変更はしていない。
  任意の遅延や録画済みH8スケジュールを製品に埋め込んで解決扱いにしない。
  コントローラー計算の入力由来所要時間は可変入力1,223回／9種の時間でH8と一致。
  フィルター出力4662..47faも入力値由来のモデルで値／指令／実行数を比較済み。
  capacity経路7,471回・18種、可変controller経路1,223回・40種が一致。
  補間／符号化473cは別にも照合し、処理量の誤差が相殺されないよう確認する。
  これは診断モデルであり、製品の待ち時間やPCM演算は変更していない。
  既存の純粋演算から計算可能。ただしEG等を含む全体の時間モデルは未完成で、
  この部分だけの待ち時間を製品へ追加してはいない。

- [x] GSドラムモード切替の設定・発音への反映。
  [原因とH8根拠](NATIVE_RHYTHM_MODE_TRANSITION_2026-09-10.md)。
  無効な共有kit番号からmapを再指定した際の発音禁止解除漏れと、
  RPN移調を勝手にゼロへ戻す処理を修正。両mapを実MIDIで比較し、
  共有mapの編集保持・同一mode再指定・離脱/再加入・後続発音を確認。
  発音中のmelodic→map1→map2→melodicも、pedalなし／hold／sostenutoの
  各条件でNote Offと最終返却まで比較。計42件のgroup比較がPASS。

- [x] 発音開始中のパネル値変更でnativeエンジンが停止する問題。
  Note On直後の1sample render後にMIDI CHを変更すると、変更処理からの
  release要求がstartupPendingに衝突し、修正前はaccepted=0 / failed=1。
  値変更を音声スレッド所有の固定長64件キューで保持し、発音準備完了後に適用する。
  対象partは受付時に保存。満杯時は製品側の入力キューに残して再試行する。
  `--native-panel-during-startup`で停止回避・対象保持・満杯・復帰を確認。
  `--native-panel-settings`の42,015件、`--native-display-control`、
  `--native-synth`の可変block/checksum試験もPASS。実ホスト確認は未実施。

- [ ] 高音域の特殊発音（key125..127）のNote Off後のvoice/group差。
  `--native-high-admission`で再現。program24/key125を4回発音した後、
  Note Offから2M cyclesでH8は2group、nativeは1group。H8は09f7→155a→1633で
  正しくrelease要求を処理しており、Note Off自体を無視する仕様ではない。
  Note On時のgroup release flagsはH8の0bd1に従い1からFFへ修正。
  このflag修正だけでは残るgroup差は解決しない。原因未確定。
  特殊発音の新規マッピングcaseはprogram0/key125（なし）、24/key125（あり）まで。
  失敗で停止するためkey126/127はまだこの統合試験を完走していない。
  追加切分け: SC55_HIGH_ATTACKS=1/2では4case（125のあり/なし、126/127のなし）がPASS、
  3/4で差が出る。4回のNote On後はgroup列・slot・同音mark・previous linkが一致。
  Note Offは両者ともgroup1/slot22だけにrelease要求を出す。
  nativeのgroup0/slot21はstatus0/request0のまま、その後先に終了する。
  Note Offが別groupを選ぶ仮説は棄却。次は自然終了のEG/PCM境界を比較する。
  H8の空group観測だけで恒久保持の仕様と解釈しない。1cbd→1d08は空groupを返す。
  終了種別はサンプル終端。slot21は両者ともstage4→14、release=0/task=0。
  H8は00:296aでNote Off入力後1,861,092cycles、nativeは1,817,500cycles。
  差43,592cycles（約2.18ms）。nativeのstage22/返却は1,980,625cyclesで、
  固定2Mcyclesの比較境界をまたぐ。開始時刻・終端通知処理のどちらが差を作るかは未確定。
  一時probeは削除、ログ`/tmp/sc55-high-end.log`。再現試験のFAILは維持。

- [x] 通常ポリ発音の同音回収を接続。
  [原因・検証](NATIVE_REPEATED_NOTE_ADMISSION_2026-09-10.md)。
  MIDI入力から停止系CCとpedal、GS割当3modeをH8と比較済み。
  CC84 source不在の新規発音にも接続。音色変更後のsource無効化では、
  待機からの復帰後に別groupを再検索せず、新規発音の判断を保持する。

- [x] 通常RQ1の40:00:7f読出し。起動・bulk変更・GS reset後を含め
  414件のpacketをH8と比較済み。読出しでresetしないことも確認。
- [x] 通常RQ1の40:3p読出し。制御ROM識別・波形ROM情報を接続し、
  全16ページとサイズ省略を含む463packetのH8比較がPASS。
- [x] 通常RQ1の明示的な送信待ち／完了を音源のMIDI処理へ接続。
  送信中の入力保持、完了後のCC・発音再開をH8比較済み。
  watchdogのTX busy入力にも接続。実ホスト出力は引き続き未接続。
- 対象外（ユーザー指示）: パネル起点の一括設定送信と転送終了時の境界。
  CF02 bit0は通常RQ1の別設定ではなく、04:713e/715f/717dが所有する
  複数転送の外側の状態。停止・controller reset・受信禁止を最初に一度行い、
  全転送の終了後だけ受信を再開する。各RQ1に通常の開始/終了処理を繰り返さない。
  内部入力経路の送信待ちも含めた責務として未対応。単なるmodeフラグ追加はしない。
  [根拠と未確認範囲](CPU_SERVICES_AND_ORDER_2026-09-09.md#パネル起点の一括転送の範囲訂正2026-09-10)。
  追加実装: ALL表示→INSTRUMENT左右同時押し→ALL確定の実ボタン経路を確認。
  開始1回、内部要求17件、TX60packet、終了1回。各producerは前要求の処理完了後に
  40tick待機を開始することを実行命令hookで確認した（Step前PCの重複を除外）。
  7318はtask7から解析関数04:0e8eへの直接pjsr。別taskへ投入するという説明は訂正。
  BulkReplyTransferが全設定シーケンスを所有し、NativeSynth::requestAllSettingsDump
  から開始可能。通常のRQ1と同じ送信packet生成器を使い、全体で一度だけ停止／
  controller reset／RX破棄を行う。内部要求の間にRXを再開しない。
  終了時は28c2に合わせ、転送中に蓄積したパネル値操作を破棄する。
  `--panel-bulk-sequence`で実H8と全60packet一致、実送信完了待ち、途中MIDI破棄、
  終了後受信再開、パネル操作破棄がPASS。UIボタンからの製品接続、部分送信
  （715f/717d）、ホストMIDI出力は残る。この責務全体を完了とはしない。
  続く実装で715f/717dも同じ転送ownerへ統合。NativeSynth::requestSettingsDump
  は送信範囲だけ受け取り、Note Receive/muteから対象part、routingから付随mapを決定。
  system付き49packet、parts47、melodicのみ30、空集合0、map1のみ47を実ボタンと比較。
  両mapを使う部分送信はH8でmap0後に選択が消えてmap1を送らず、49packetになる。
  71efのR5=0を実測し、この挙動も保持。全設定送信は従来通り両mapを送る。
  完了待ち／途中入力破棄／終了後再開も各caseで確認。通常RQ1の18要求62packetも維持。
  残りは製品UIからの操作・確認表示、solo表示時の選択、ホストMIDI出力接続。
- [ ] 未解釈設定の機能、パネルの未対応操作・隠しモード。
  ソロの音源制御を接続。通常画面ALL+MUTEの物理入力でCDCC bit3が切り替わる。
  開始時に他partを停止し、選択part変更時も停止対象を更新する。
  Note Receive/global muteの設定値を保存したまま、solo中だけ受信判定を上書きし、
  解除時には元のmuteに従い停止する。solo中の通常MUTE操作はH8同様に無視する。
  NativeSynth::toggleSoloと既存パネルキューのsoloコマンドまで接続済み。
  `--native-panel-solo`で実H8の物理ボタンとMIDIを比較し18件PASS。
  ALL soloによる全part受信、単partへの復帰、global muteの復帰も含む。
  `/tmp/sc55-solo-test.log`。音色はprogram80、入力は3channel。
  全音色・全演奏条件やLogicのGUI操作を網羅した結果ではない。
  GUIはALL+MUTEの同時押し、およびdesktop用Shift+MUTEを既存キューへ接続。
  同時押し後の単独ALL/MUTEクリックは抑止し、実押下とButtonの描画用flashを区別する。
  nativeのsolo状態をatomic snapshotでUIへ公開しMUTE点滅とtooltipに表示する。
  この点滅はGUIの表示でありH8のLED周期再現ではない。H8参照モードは従来LED表示。
  実機タッチ／Logic上のGUI操作は未確認。
  GS resetを単part solo中、GM resetをALL solo中に送り、後続Note Onとsolo解除を
  含めH8比較27件PASS（`/tmp/sc55-solo-reset-test.log`）。H8もsoloとglobal muteを
  保持するため、C++側でリセット時にこれらを初期化する変更は不要だった。
  接続後のXcode Release/arm64 Standaloneと内蔵AUv3はBUILD SUCCEEDED
  (`/tmp/sc55-native-solo-ui-release.log`)。Resave・署名・インストールはしていない。
  通常設定操作は16partとALLをH8の実ボタン入力と比較済み。
  ドラムINSTRUMENTの単純な番号加減算を修正し、有効kitだけを前後選択する。
  全kit往復・両端を含め比較済み。MIDI PCのfallback規則は変更しない。
  音色名の所有をinstrumentNameへ統一。ドラムの固定RHYTHM表示を廃止し、
  live map名をsnapshotとscroll復帰行に使用する。H8のLCD用文字列と比較し、
  全kit・GS名変更・MIDI fallback・拒否番号を確認済み。
  隠しモード・同時押し・長押しはこの確認に含まない。
- [ ] モデル45表示のライフサイクルを製品へ接続する（次のまとまった責務）。
  通常の文字・bitmapはNativeSynth::stateからLCDまで接続済み。
  文字の短文保持／長文スクロール、bitmap保持、取消・resetをまとめて扱う。
  描画はUI、表示時間・内容の所有は音源側。H8の根拠は下記資料に追記。
  DisplayControlは実装済み、8文字メッセージ・801scroll更新・2bitmap・
  3040timer更新をH8と照合済み。製品MIDI/render/snapshotの配信・期限切れも確認。
  通常画面の18ボタンによる取消・維持はH8比較済み、GS reset取消と
  表示中のzero/1/257frame分割も確認済み。
  残りは別表示モードの操作、高速scrollモード、ホスト画面とLCDピクセルの確認。

各項目の根拠と現在の対応範囲は
[CPUサービス](CPU_SERVICES_AND_ORDER_2026-09-09.md)を参照。
ホストMIDI出力接続は別の契約変更であり、内部制御の完了と混同しない。

## 製品ビルド確認（2026-09-10）

最新確認: 通常passの命令時間待ちを外し、EG／filter／pitch／levelを直接更新する
現状で、Xcode Release / arm64 Standaloneの `BUILD SUCCEEDED` を確認。
内蔵AUv3のリンク・app内へのコピー・ValidateEmbeddedBinaryも成功。
出力 `/tmp/sc55-native-product-validation/Products/Release/SC-55.app`、
ログ `/tmp/sc55-native-direct-release.log`。
Resaveなし、署名・Launch Services登録・インストールなし。
ソースの既定選択はC++のまま。Logicの認識、実演奏、CPU負荷は未確認。

今後の機能作業はパネル操作の未接続部分（同時押し／確認／solo選択）、
表示の特殊モード、到達条件未確定の制御責務を対象とする。
高音域の2ms程度の終了時刻差とcapacity40の生存key差は既存診断として残すが、
その一致だけを目的とする命令時間モデルの追加は再開しない。

最終Xcode確認: NoteGroup／VoiceAllocation集約、再利用の周期待機、
待機中の他ボイス制御、pass再開、未処理停止stageの公開、
既存DSPへの準備要求中の周期更新、再利用待ちから独立したMIDI controller更新、
受信済みNote On/Off・pedal・source・part解放の型付きコマンドキューと
発音管理の分離、CC121とProgram Change、MIDI mode、通常message入口統合、
SysExとGS partの受信／発音処理分離までのソースで
Release / arm64 Standalone schemeのビルド成功。内蔵AUv3も対象。
`/tmp/sc55-native-product-validation-current.log` がこの確認のログ。
署名無効・登録無効、Resaveなし。Logicでの起動・実演奏は未確認。

同じソースはCMake診断targetでも製品のNukedSC55Emulator.cppを含めてコンパイル済み。
成果物: `/tmp/sc55-native-product-validation/Products/Release/SC-55.app`。
その後のIRQ計測フックは診断target限定のcompile definitionであり、製品では無効。

同日途中の変更でXcodeのStandalone schemeをRelease / arm64でビルドし、
内蔵AUv3を含め `BUILD SUCCEEDED`。Resave・インストールはしていない。
既存出力先のディレクトリ作成に失敗したため、検証出力だけ
`/tmp/sc55-native-product-validation/Products/Release` に変更した。
署名は無効化したビルド確認であり、Logicでの認識・実演奏確認は未実施。
ログ: `/tmp/sc55-native-product-validation.log`。
このビルド後のパネル操作キュー等の変更は、上記のheadless試験で確認したもので、
この製品ビルド結果には含まない。

同日再確認: パネル操作キューとGSドラムモード修正を含む現状で、同じ
Release / arm64 Standalone schemeを再ビルドし `BUILD SUCCEEDED`。
AUv3のリンク・app内への埋込み検証も完了。環境変数なしでNativeSynthを
選ぶ製品経路は維持。Resave・登録・インストール・Logic起動はしていない。
ログ: `/tmp/sc55-native-product-validation-current.log`。
生成物: `/tmp/sc55-native-product-validation/Products/Release/SC-55.app`。
