# 現在の制御移植作業

2026-09-10。過去の実験TODOではなく、次の実装を選ぶための入口。
完了条件は `WAVESTATION_STRUCTURE_CHECK_2026-09-09.md` のまま変更しない。
次の機能選定は `NATIVE_CONTROL_ACCEPTANCE.md`。本書の古い未確認記述は履歴であり、
現在も未接続とみなす前に現状判定と製品ソースを確認する。

2026-09-10の最新ユーザー指示：隠し機能・拡張LCDメニューは後回し、音源制御を優先。
下記の過去記録で拡張パネルを次作業に挙げていても、この指示を優先する。
各作業単位の完了時にコミットする。pushは別途指示がある場合のみ。

2026-09-10実演奏フィードバック：演奏は概ね問題なし。GATCHA55後半のパートが
切れる件はユーザー指示で今は追わない。今回の対象は演奏終了後のメーター残留。

## 現在の製品構造

比較スイッチOFFのPCM方式を修正。以前はEmulatorコンストラクタが高速PCMを
既定有効にしており、「両方式で同じPCM」という以前の説明は誤りだった。
H8側の初期化でPCM_UseSimulation(false)／use_float_effects=falseを明示し、
SC55_SIM／SC55_FXSIMに関係なくH8命令実行＋従来整数PCM/エフェクトとする。
PCM出力周波数は方式確定後に取得。ON側の既定整数PCMと実験用指定は変更なし。
未発音voice・無音effectsの計算省略など共通最適化は残す（Fork当初への復元ではない）。
engine-switchでC++→H8→C++の発音確認、SIM/FXSIM両方1と両方0で
H8 stereo出力checksum3621512afd400fcfが一致。
ログ`/tmp/sc55-h8-pcm-test.log`と`/tmp/sc55-h8-pcm-test-reference.log`。
Release arm64 Standalone＋内蔵AUv3 BUILD SUCCEEDED
(`/tmp/sc55-h8-pcm-release.log`)。Logicでの比較試聴は未実施。

最適化toggleのROM対応判定を追加。ROM選択時の既存hash読込みで
`CanImportSoundData`を使い、C++音源と同じROM1/ROM2の対応条件を取得。
非対応／未選択はOFF表示・disabled、非対応ROMはH8経路で初期化する。
対応ROMへ戻すと再enableし、ユーザーの最適化ON/OFF希望は保持する。
processor側も非対応ROMでのON要求を拒否。判定はprocessBlockでは行わない。
headless `rom-optimization`でv1.21=available、mk2/SC-155 rev1=disabledを確認。
Release arm64 Standalone＋内蔵AUv3 BUILD SUCCEEDED
(`/tmp/sc55-rom-optimization-release.log`)。Logic上の操作確認は未実施。

設定の`toggleOptimization`を比較用のインスタンス単位切替へ接続。
ON=C++、OFF=H8。初期値のみ既存NUKED_SC55_USE_H8環境変数を参照し、
UI操作は環境変数を変更しない。今回の選択はprocessor寿命内だけ保持し、
host parameter／セッション保存項目は追加しない。editor再作成では選択を維持。
切替はmessage-threadで内蔵player停止・最大CPUリセット要求後、両2X音源を
再初期化。JUCE suspendProcessingで進行中callbackと同期し、重い初期化中は
callback lockを保持せずhostが無音を返せるようにする。失敗は旧方式へ復元を試み、
UIにエラー表示。音源状態の途中引継ぎではなく、曲の先頭から再比較する用途。
H8側は旧部分移植native_v121_enabledもfalseに固定。native cache生成はC++側だけ。
既存PCM DSPは共通のまま、元プロジェクト全体の過去版へ戻す機能ではない。
`engine-switch`試験で明示C++→H8→C++を同一adapterで再初期化し、
各方式のready／発音／H8 clock進行を確認。環境変数USE_H8=1でも明示選択が優先。
H8はfirmware起動完了を待ってNote Onを送る。最初の固定2秒待機では未起動だった。
ログ`/tmp/sc55-engine-switch-test.log`。Release arm64 Standalone＋内蔵AUv3
BUILD SUCCEEDED (`/tmp/sc55-engine-switch-release.log`)。Logicでのボタン操作、
失敗時復元とホスト処理中の切替は実行未確認。Resave／インストール／pushなし。

終了時にメーターが瞬時に消える挙動への追加修正：`NativeMeterDecay`を
message-threadのLCD描画に接続。上昇は即時、下降は1段40msでゼロまで減衰する。
これはUI表示用の時定数で、実機計測値ではない。steady_clockの経過時間を使い、
音声サンプルの進行や描画回数には依存しない。2Xは合算後に一度だけ適用。
PCM/EG/発音所有とraw snapshotは変更なし。ROM不要の`meter-decay`試験で
段階的消去・最終ゼロ・再発音・各パート独立・描画周期非依存を確認。
Release arm64 Standalone＋内蔵AUv3 BUILD SUCCEEDED
(`/tmp/sc55-meter-decay-release.log`)。Logicでの見た目は未確認。Resaveなし。

演奏終了後のメーター残留を修正：PCMのkey bit／gain値は発音ボイス返却後も残る。
`voiceLevels()`がPCM bitだけでactiveと判定していたため、発音0でも表示が残った。
実Note On/Offでvoices=0／meters=1／pcm_keys=800000を再現し、
返却済みallocationを表示snapshotから除外するとvoices=0／meters=0へ改善。
PCM bitは800000のまま保持し、音声生成やEG／ボイス返却を変更していない。
表示集計は従来通りUI側。発音中の表示維持と終了後の消去をnative-only試験へ追加。
修正前FAIL `/tmp/sc55-ended-meter-before.log`、修正後PASS `/tmp/sc55-ended-meter-after.log`。
既存音声checksum3b54320560580fd3も不変。Logic上の表示確認はユーザー確認待ち。
Release arm64 Standalone＋内蔵AUv3 BUILD SUCCEEDED (`/tmp/sc55-ended-meter-release.log`)。
Resave・登録・インストールなし。

2026-09-10 通常製品アダプターの負荷を分離計測：`adapter-load`は同じ既定PCMで
NativeSynth直接とNukedSC55Emulator::renderのFIFO／リサンプル／MIDI経路を比較する。
H8／代替PCM指定は拒否し、ROM setupは計測外。1秒warmup後、音声1秒を5回、
測定順を交互に実行。adapter48kHz/128frames、core32kHz/128frames。
無音0voiceとprogram80の持続24voiceを状態で確認する。演奏ベンチを減衰済みpianoにしない。

今回の中央値はidle core5.630ms／adapter6.400ms、24voice30.844ms／31.461ms
（いずれも音声1秒あたり）。idleの追加コストは0.770ms＝約0.077 percentage point。
範囲はidle core5.498〜5.727／adapter6.202〜6.430、
24voice core30.691〜30.963／adapter31.319〜31.556ms。
ログ `/tmp/sc55-adapter-load.log`。通常target build成功。
代替PCM指定の誤比較防止guard追加後の再実行も成功：idle5.744／6.490ms、
24voice31.959／33.098ms (`/tmp/sc55-adapter-load-final.log`)。
これはoffline平均であり、Logicの最大CPU2.6%を再現した結果ではない。
processBlock全体、GUI、2X、実時間threadの待ち・電源状態は未計測。
adapterを主要原因として変更する根拠は得られなかったため、製品コードは変更しない。
Xcode／Logic再確認なし。H8制御の未対応機能が減ったという主張でもない。

2026-09-10 H8／JUCEをリンクしない音源コアのbuildを追加：`tools/native-engine`の
`sc55-native-engine`は既存NativeSynthのC++20 headersとPCM／patch decoder／SHAだけを
公開するSTATIC target。別の音源実装やPCM差替え、製品の既定経路変更ではない。
setup側のcheck consumerが既存ROM loaderを使い、ROM読込み・波形decode・data importを
音声処理の前に実行する。ROM／cache／音声ファイルを生成・コミットしない。

Release arm64でconfigure/build/`native-without-h8`がPASS（0.37秒）。
実際の`NativeSynth::push/render/state`のうち今回実行したのはconstruct/push/render：
既存製品fixtureと同じNote On/Offと0/257frame renderでchecksum3b54320560580fd3、
nonzero16442を確認。バイナリのsymbolにMCU／Emulator／JUCEはなく、動的リンクは
libc++とlibSystemだけ。H8実装がリンクに存在しなくても通常制御とPCMで発音できる。
ログ `/tmp/sc55-native-only-{configure,build,test}.log`、再現手順はtool README。
macOS arm64のみ実行。Windows/Linuxや全制御機能、実曲全体の一致までは主張しない。
製品ソースは変更していないためXcode再build／Logic確認なし。

2026-09-10 55KTIZKEの残るpoly差とmono識別を確認：GS part3/4/8/12を
同じ60秒入力で比較。poly part3は入力325/native325、part4は219/219、part12は17/17。
part3の入力23.434917/key62はH8 slot12/group12へ23.448687秒に確定した後、
開始前の23.457937秒にpart7/key67へ置換される。
part12の3件も開始前に別partへ割当が移る：

| 入力秒／key | H8 slot/group | 対象の割当確認秒（status=0） | 他partへの変更確認秒／part/key |
|---|---|---|---|
| 41.971469 / 100 | 17 / 10 | 41.979062 | 41.984500 / 6 / 68 |
| 47.825117 / 95 | 21 / 17 | 47.832094 | 47.836156 / 3 / 66 |
| 48.800725 / 96 | 6 / 16 | 48.807469 | 48.812188 / 3 / 64 |

各対象keyの期間に新たなPCM開始なし。ログ `/tmp/sc55-ktizke-owner-part{3,12}.log`。
part4の入力59.999892/key81は60秒の終了境界に当たり、60.08秒へ延長すると
H8も60.003406秒に開始、part4は219/219になる (`/tmp/sc55-ktizke-end-part4.log`)。
前回のpart7も合わせ、polyの開始数差11件はH8の開始前再割当10件＋試験終了境界1件。
元の60秒の観測と既存regression assertionは変更しない。

mono part8はgroup keyの再利用ラベルが新しい要求keyと違う。例：入力24.166623/key71で
native開始24.172438のgroup keyは74だが、mono held keyは71、PCM pitchは312d。
H8開始24.179031はgroup key71、held key71、pitch312e。raw pitchの一致は要求しない。
開始時のheld keyと同音の直前入力を対応付けると、native112開始は112入力に各1回対応し、
H8との差は35.142213/key74、37.093429/key81、48.800725/key78の3件だけになる。
保持keyはPCMへの最終反映を証明する値ではないため、全ノートの音高／EG一致とは呼ばない。
この3件のH8側開始省略の原因までは未確定。従来のgroup key集計だけでnativeが
key71等を鳴らし損ねたと判定しない。ログ `/tmp/sc55-ktizke-mono-part8.log`。
診断target build成功。製品コード変更なし、Xcode再build／Logic確認なし。

2026-09-10 共通音源制御イベントをvoice ownerへ集約：`NativeVoiceEngine::updateControl`
がeventのelapsed保持、effects→voice passの順序、待ちからの再開と完了を所有する。
受信側の`effectPassClock_`を削除し、周期eventの内部状態を直接操作しない。
effects updaterは同一音声スレッド内の同期呼出しであり、UI callbackではない。
リセット時にも保持中eventを従来同様に残す。command優先、受信を起こす条件、
PCM key-latch／reuse待ち、共通周期、H8参照は変更しない。
新規H8機能の対応や負荷削減ではなく、共通制御の所有を音源に集約する実装変更。

製品と同じ入口の追加試験で、linked voiceの待ちを跨ぐeffects先行、
同一voice pass再開時のeffects非再実行、後続2周期の保持、effects無効を確認。
`voice-control-pcm`と`native-player`がPASS (`/tmp/sc55-periodic-owner-test.log`)。
通常製品targetでもchecksum3b54320560580fd3、0/1/127/129/257frame分割一致、
GATCHA55初期化後mute→part16第8音、55KTIZKEの13kickの先頭gainを維持。
ログ `/tmp/sc55-periodic-owner-{synth,part16,kick}.log`。
Release arm64 Standalone＋内蔵AUv3もBUILD SUCCEEDED
(`/tmp/sc55-periodic-owner-release.log`)。Resave・登録・インストールなし、Logic実操作は未確認。

2026-09-10 55KTIZKE GS part7の229/235差を発音前再割当に分類：
ミュートなし60秒の入力235 Note Onに対しnative PCM開始は235、H8は229。
同音キーの直前入力への対応付けだけでは判定せず、差6件を含む33.18〜38.60秒で
H8のslot/group/key/statusとPCM key/latchの変化をsample callbackから読み取り確認。
6件ともH8で対象keyに割り当てた後、PCM開始前に同じslot/groupを後続keyに再割当。
元keyの期間には新たなkey-onがなく、後続keyで初めてkey/latchの開始条件を満たす。
古いPCMが鳴り続けている期間のgroup key更新を「元keyが鳴った」と扱わない。

| MIDI入力秒／key | H8 slot/group | 元keyの割当確認秒（status=0） | 後続keyへの変更確認秒 | 後続keyのPCM開始秒 |
|---|---|---|---|---|
| 33.190997 / 52 | 5 / 8 | 33.203250 | 33.213094 / 67 | 33.223875 |
| 36.117821 / 50 | 3 / 11 | 36.123438 | 36.131469 / 66 | 36.143719 |
| 37.113754 / 59 | 4 / 4 | 37.116906 | 37.117344 / 66 | 37.128406 |
| 37.947086 / 55 | 4 / 17 | 37.952844 | 37.953656 / 66 | 37.962875 |
| 38.312939 / 55 | 17 / 9 | 38.321219 | 38.325844 / 66 | 38.331406 |
| 38.556841 / 55 | 20 / 15 | 38.564937 | 38.572031 / 66 | 38.574219 |

この6件はnative欠音ではなくH8側の発音前再利用。nativeは各要求でPCM開始している。
したがって開始数を229へ減らすためにH8の命令時間や人工待ちを製品へ戻さない。
ただし別のsurvivor、reserve判定、全曲の可聴一致まで証明したという意味ではない。
診断追加のみで製品コード変更なし。診断target build成功、全曲観測の総数とidentity集計も
以前のログと一致し、観測によるH8/native状態変更はしていない。
ログ `/tmp/sc55-ktizke-owner-part7-all.log`。再現はSC55_NATIVE_IO_AUDIT有効targetで
SC55_SONG_IDENTITIES=1、SC55_SONG_TRACE_PART=7、
SC55_SONG_TRACE_WINDOW=33.18,38.60を指定し`--song-allocation 55KTIZKE.MID`。
製品を変更していないためXcode再build／Logic確認なし。

2026-09-10 GATCHA55の87/84差をミュート境界に分類：`--song-part16`先頭18秒で
GS part4/9だけを記録する診断を追加。入力MIDI、PCM開始、先頭gainと実際のmute設定を比較。
part4のnative muteは3.722812秒時点で確認、H8 muteは3.799281秒時点で確認。
key79の3回目は入力3.713763秒で両方発音、key71/67は入力3.722821秒でH8だけ発音。
part9もnative muteは4.924344秒、H8は5.003969秒時点で確認し、その間の
key48（入力4.954703秒、H8開始4.957687秒）がH8だけ発音している。
これで既存identity差3件と87/84の全差が対応する。未識別開始は両方0。
設定観測は最大128frame間隔であり、上記を正確なボタン処理時刻とは扱わない。

物理ボタンscan経由とnative commandのUI受付差を、音源の欠音やcapacityバグと
取り違えない。音源に待ち時間を追加せず、製品コードは変更しない。
両診断ともpart16第8音の回帰条件はPASS。先頭音数の全曲一致／音声全体の証明ではない。
ログ `/tmp/sc55-identity-mute-part4.log`、`/tmp/sc55-identity-mute-part9.log`。
再現はSC55_NATIVE_IO_AUDIT有効のsc55-cpu-rolesで、SC55_SONG_IDENTITIES=1と
SC55_SONG_TRACE_PART=4または9を指定して同じ`--song-part16 GATCHA55.MID`を実行する。
今回は診断targetのみbuild。音声製品が変わっていないためXcode再buildは行わない。

2026-09-10 通常周期passの1voice計算を一括実行へ接続：`resumeControlWork`は
通常passでLFO／EG／filter／pitch／levelごとにphase dispatcherへ戻らず、
既存の`CalculateVoiceControl`を一回呼ぶ。共通tick、group選択、controller読出し、
paired LFO、PCM readback、終了通知、出力publishの順序は変更しない。
明示的phase／命令時間診断と、既に中断状態を持つLFO・計算の再開は従来経路を使う。
実PCMの再利用待ちやkey-latch待ちを削除する変更ではない。

状態所有の確認：`beginPreparedStart`は新しい準備状態をpendingStartとlifecycleへ保持し、
`pollPreparedStart`はPCMのreuse／key-latch完了後、全partialの継続状態を検証してから
`runtime.voices`へ確定する。待ち中の旧EG所有者と新しい準備状態は意味が違う。
`admissionLifecycle`、`importPendingVoiceOperations`、`exportControlChanges`のコピーを
単純な重複と判断して一つに潰さない。LFO配列もボイス間の共有元を参照する状態であり、
配列が複数あるという理由だけで新しい抽象層や参照viewを増やさない。

`voice-control-pcm`（全pass／phaseの入出力比較を含む）と`native-player`がPASS。
通常C++ targetのchecksum `3b54320560580fd3`、0/1/127/129/257frame分割一致、
GATCHA55第8音、55KTIZKEの13kickも維持。
ログ `/tmp/sc55-direct-periodic-{control-test,synth,part16,kick}.log`。
今回のheadless計測はidle5.472ms、24notes30.239ms／音声1秒。前回同条件の
5.462ms／30.237msと実質同程度で、大きな速度向上を主張する結果ではない。
Release arm64 Standalone＋内蔵AUv3もBUILD SUCCEEDED
(`/tmp/sc55-direct-periodic-release.log`)。Resave・登録・インストールなし、Logic実操作は未確認。

2026-09-10 発音commandの消費をボイス所有者へ集約：`serviceCommand`が
完了通知との優先順位、1commandの取出し、Note On/Off、pedal、source、release、
controller reset、program、mono/poly変更と、その後の発音準備を実行する。
受信側はcommandを取出さず、保留発音を直接生成・変更しない。
保留発音はprivate化。既存の命令単位fixtureだけが診断target限定の`admissionAudit`を使う。
portamento時間もsetter経由とし、製品受信側からmono状態を直接書き換えない。
capacity設定の所有は受信側に残し、消費したprogramのpart/toneを結果として返す。
その結果を、次のcommand／周期passへ進む前に反映する。新しい受信toneへ読み替えない。

commandと周期passの同時発生（backlog0/64）、release integration111比較、
準備中Program Change、native-playerがPASS。通常製品targetのchecksum
`3b54320560580fd3`と可変block一致、GATCHA55第8音、55KTIZKEの13kickも維持。
ログ `/tmp/sc55-command-dispatch-{order,integration,program,player,synth,part16,kick}.log`。
Release arm64 Standalone＋内蔵AUv3もBUILD SUCCEEDED
(`/tmp/sc55-command-dispatch-release.log`)。Resave・登録・インストールなし、Logic実操作は未確認。
この変更は発音管理の所有・interfaceの整理であり、新規対応機能やCPU改善の証明ではない。

2026-09-10 Note Onの準備・割当をボイスエンジンへ集約：受信側に残っていた
poly／high-note mapping、mono／portamento source再利用、rhythmの3経路を
`NativeVoiceEngine::serviceAdmission`へ移した。準備用PCM／EG入力、capacity確保後の再開、
音色変更によるreuse invalidation、発音確定時のpitch historyもボイス側で管理する。
受信側は発音要求を渡し、結果のfailed／unsupportedを扱う。
設定反映時の24voice入力更新も同じ所有者の`refreshControls`で行う。

`Configuration`は同一音声スレッド内の設定への読み取り専用viewで、呼出しを越えて保持しない。
受信時に決まったtoneは要求に保存、soft pedal／part設定／共有drum mapは発音準備時に読む。
無効kitへ変更された後でも、既に受理済みのdrum Note Onを取り消さない既存仕様を維持する。
共通制御周期、PCM開始・再利用待ち、mono戻り発音とdrum key-latch保護は変更していない。
新規H8機能やCPU改善の主張ではなく、意味単位の発音管理を一つの所有者へまとめる変更。
command取出しと一部のportamento値設定、起動初期化・診断にはまだ受信側からの状態参照が残る。

通常C++ targetでchecksum `3b54320560580fd3`、0/1/127/129/257frame分割一致、
GATCHA55初期化後のミュート→part16第8音、55KTIZKEの13kickのfirst-ms gainがPASS。
H8比較はrelease integration111件、shared rhythm42件がPASS。native-player試験もPASS。
ログ `/tmp/sc55-admission-owner-{synth,part16,kick,release-test,rhythm,player}.log`。
発音準備中Program Change（受理済みdrumと後続無効kitを含む）とGS part変更もH8比較PASS
(`/tmp/sc55-admission-owner-program-order.log`、`/tmp/sc55-admission-owner-part-order.log`)。
Release arm64 Standalone＋内蔵AUv3もBUILD SUCCEEDED
(`/tmp/sc55-admission-owner-release.log`)。Resave・署名登録・インストールなし。
Logicでの実演奏／CPUメーター確認は未実施。

2026-09-10 Note Off／ペダル／モード変更の発音管理をボイス所有側へ集約：
`NativeVoiceEngine`がpolyの解放snapshot、monoの保持キー判断と戻り発音要求、
hold/sostenuto/portamento、controller reset、mono/poly変更時の停止・キー初期化を扱う。
受信側はroutingのnote flagsと現在の選択toneを渡し、保持キーや解放snapshotを直接変更しない。
poly Note Offもペダル同様、更新候補のrelease snapshotを検証してから確定する。
monoの戻り発音は従来どおり現在の選択toneを使う。PCM readiness、共通更新周期は変更しない。
新しいH8対応機能や速度向上を主張する変更ではない。mono/poly/rhythmのNote On準備には
まだ受信側からallocator／runtime状態を操作する箇所があり、発音管理全体の集約は未完了。

通常製品targetで可変block（0/1/127/129/257frame）の一致と音声checksum
`3b54320560580fd3`を維持。GATCHA55初期化後のパートミュート→part16第8音、
55KTIZKEの13kickのfirst-ms gain、native-player（reset等を含む）試験がPASS。
ログ `/tmp/sc55-voice-command-owner-{synth,part16,kick,player}.log`。
Release arm64 Standalone＋内蔵AUv3のXcode buildも成功
(`/tmp/sc55-voice-command-owner-release.log`)。Resave、登録、インストールなし。
Logicでの実操作・CPUメーター確認は未実施。

2026-09-10 表示専用メーター集計の音声側残留を除去：従来の`NativeSynth::state()`は
16partごとに24slotを走査してLCD用のpeak stereo envelopeを集計していた。
音声側は`voiceLevels`に24slotの所属・active・左右gainを一度ずつコピーするだけに変更。
message-thread専用の`getNativeState`が取得後に`calculateDisplayLevels`を実行する。
2Xの左右音源のpart peak加算も従来どおりmessage thread。
immutable snapshotだけで計算でき、UIからlive synth／PCMを参照しない。
音色名・設定値等のsnapshotコピーまで削除したとは主張しない。

通常`sc55-native-product-check synth`で音声側の未集計とUI側の発音メーター復元を確認。
peak（voiceの和ではない）・65535上限・inactive除外・古いlevel消去もPASS。
音声checksum3b54320560580fd3、zero/1/127/129/257frame分割一致は不変。
adapterの44.1/48/96kHzと2X表示もPASS。
ログ `/tmp/sc55-meter-owner-synth.log`、`/tmp/sc55-meter-owner-adapter.log`。
通常targetの製品emulatorを含むbuild成功。最新Xcode／Logic確認とCPU差の計測は未実施。

2026-09-10 GS part受信の二重解析を除去：`receivePartSettings`は一度のtable解析で
設定候補とchannel reset／program／mono・rhythm変更の要求を作る。
voice command batchを確保できなければ候補を破棄してEOXを保持し、受理できた場合だけ
対象partとassigned CCを反映する。後続のunsupported／invalid recordより前に受理した
prefixは従来どおり反映。parser callbackからlive設定を変更する二度目の解析は削除。
dynamic controller resetとscalar設定は別の値を所有し、共有drum map更新は確定後だけ。
これは新規対応機能の発見ではなく、設定受信と音源操作の一回の確定へ整理する変更。

`native-player`がPASS、H8とのrelease integration111比較、shared rhythm42比較、
発音準備中のpart SysExと同一channel再設定時のrelease/resetがPASS。
ログ `/tmp/sc55-part-transaction-{test,release-test,rhythm,preparation}.log`。
CMakeの製品emulatorを含むtarget build成功。今回の変更後のXcode／Logic確認は未実施。

2026-09-10 リセットのボイス所有を集約：`NativeVoiceEngine::resetVoices`が
停止開始／drain待ち／PCM再利用確認／所有状態の再構築を所有する。
コントローラーのreset状態は要求の有無だけとし、停止開始時のdynamic MIDI resetと
完了時のGS/GM設定・ドラムmap・effect要求の復元を担当する。
物理slot走査、lifecycle判定、clock/key mask/pitch履歴・受信済みcommand/admissionの
退避復元をコントローラーから除去。`operationsPending`もボイス所有側で判定する。
実PCMのramp完了まで古いDSP ownerを保持する条件、後続MIDIを待たせる条件は維持。

`native-player`試験がPASS：EOX前のreset禁止、発音・sustain中reset、後続PC/noteの保持、
PCM時間の進行、連続GM reset、24slot使用中のresetを既存assertのまま確認。
`--native-panel-solo`の物理H8比較27件もPASS。通常設定の`sc55-native-product-check synth`
はchecksum3b54320560580fd3とzero/1/127/129/257frame分割一致を維持。
ログ：`/tmp/sc55-reset-owner-test.log`、`/tmp/sc55-reset-owner-solo.log`、
`/tmp/sc55-reset-owner-synth.log`。新しい音源機能やCPU改善を実証する変更ではない。
Release arm64 Standalone＋内蔵AUv3もBUILD SUCCEEDED
(`/tmp/sc55-reset-owner-release.log`)。Resave・登録・インストールなし。Logic実操作は未確認。

2026-09-10 ボイス停止責務を集約：MUTE/SOLOのpart停止要求と保留maskを
`NativeVoiceEngine`へ移動。コントローラーは`requestPartStops`を渡し、
`serviceRetirements`がphysical slot選択、activation待ち、停止再試行、停止後処理を所有する。
MIDI All Sound Offも同じ`stopSoundingParts`で対象を選ぶ。既存のPCM停止／再利用手順と
command/controlの実行順は維持。resetやadmissionの全内部状態を隠し終えたわけではない。
診断用・通常ランタイムのCMake targetをbuildし、以下を確認した。

- `--native-panel-solo`: H8の物理操作／MIDIとの27比較がPASS。
- `--native-panel-during-startup`: activation中の編集・キュー満杯後の回復がPASS。
- 通常`sc55-native-product-check synth`: checksum3b54320560580fd3を維持、
  zero/1/127/129/257frame分割一致。
- 通常`kick`の55KTIZKE先頭60秒: 13kickの最初1ms gainがH8と一致。

ログは`/tmp/sc55-retirement-{build,solo,startup,synth,kick}.log`。
この変更後のXcode再buildとLogic実操作は未実施。新機能の追加やCPU改善を示す変更ではなく、
ボイス所有者の外でphysical slotを解釈していた2箇所と停止完了loopを除去する構造変更。

2026-09-10 2Xパネル所有の修正：主音源だけでパート選択した後に2Xを有効にすると、
従来の二重ボタン解釈では主part4／副part2となることを再現した。
`pressFrontPanelButton(button, mirror)`はUI側で一度だけ解釈し、同じ対象付き
`SynthCommand`を両音源へ渡す。両キューの空きを確認してからUI状態を進める。
音声側は渡された対象をSOLOの発音対象にも適用し、UIコンポーネントは読まない。
`--native-panel-two-x`で選択・音量、PARTを挟まないSOLO、片側満杯時の全体拒否、
self-mirrorの二重適用防止がPASS。修正前はselected=3/1、修正後3/3。
`/tmp/sc55-two-x-panel-before.log`、`/tmp/sc55-two-x-panel-after.log`。
既存adapterの44.1/48/96kHzもPASS (`/tmp/sc55-two-x-panel-adapter.log`)。
これは操作対象の共有であり、2X切替時に全音色／MIDI設定を複製する変更ではない。
Release arm64 Standalone＋内蔵AUv3もBUILD SUCCEEDED
(`/tmp/sc55-two-x-panel-release.log`)。Resave／登録／インストールなし。
Logicでの実操作は未検証。

2026-09-10 通常ランタイムの実曲再確認：診断用timing macroを定義しない
`sc55-native-product-check`を追加。待機APIが存在したらstatic_assertでbuildを拒否。
既存試験を再利用し、判定条件は変更していない。

- `synth`: checksum3b54320560580fd3、zero/1/127/129/257分割一致。
  headless計測は音声1秒あたりidle5.503ms、24音30.310ms。
  Logicメーターや今回の変更による改善量ではない。
- GATCHA55：初期化後にパネルでparts1..15をmuteし、part16の8音目のsample startと
  gain128=99090432がH8と一致。key-on数87/84の差は残る。
- 55KTIZKE：先頭60秒、2114notes／1115full-capacity観測で13kickの最初1msのgain一致。
  key-on数2100/2114、silent windows1/1。全音声の完全一致とは扱わない。

ログ：`/tmp/sc55-product-normal-synth.log`、`/tmp/sc55-product-normal-gatcha.log`、
`/tmp/sc55-product-normal-kick.log`。これで通常側への診断状態除去後も、ユーザー報告の
2つの不具合を検出する既存条件が通ることを確認。Logic実操作は未検証。

- `sc55_synth.h`: 音源の構築、MIDI入力、render、解決済み音源コマンド適用、状態取得。
  製品のパネルボタン解釈は`NukedSC55Emulator::pressFrontPanelButton`のUI側。
  PART/ALL/SOLO/standbyの操作状態をそこで更新し、`SynthCommand`に対象partを
  固定して既存のbounded queueへ渡す。音声側はUI所有の選択状態を読まない。
  音量等の増減はMIDI直後の最新値へ適用するため音声側に残す。
  SOLOの発音対象と公開snapshotに必要なfocusの写しも音声側に保持する。
  旧selectPart/toggleAll等の診断用入口は残っているが製品queueからは呼ばない。
  fastScrollは製品の音声commandから除去した。UI側の設定を表示consumerが読む。
  音声側は表示cancelのevent記録をまだ行う。これは表示更新ではなくMIDI／設定変更と
  順序付ける記録であり、UI状態・ボタンの読出しはない。診断用fastScroll入口は残す。
  検証：`--native-adapter`の44.1/48/96kHz、MIDI後の相対編集、複数操作の
  対象part固定、standby中PART無視、SOLO連続toggle、再初期化がPASS
  (`/tmp/sc55-panel-command-test.log`)。Release arm64 Standalone＋内蔵AUv3
  build成功 (`/tmp/sc55-panel-command-release.log`)。Logic実動作は未検証。
  追加検証：UI所有fastScrollと従来の表示制御を比較、表示／音声独立性と
  adapterの3sample ratesで設定保持がPASS (`/tmp/sc55-ui-scroll-test.log`,
  `/tmp/sc55-ui-scroll-adapter.log`)。
  この追加変更を含むRelease arm64 Standalone＋内蔵AUv3もbuild成功
  (`/tmp/sc55-ui-scroll-release.log`)。Resave／登録／インストールはしていない。
- `sc55_native_player.h`: MIDIの意味解釈、設定、reset、発音要求、共通周期の制御。
- `sc55_system_settings.h`: マスター／名前／リザーブ・割当方針／エフェクトの設定値と
  システムページ受信規則を所有する。writeは値を反映してChangesを返す。
  reset・effect要求の実行は音源が引き受け、parserはPCMやボイスを直接操作しない。
- `sc55_rhythm_settings.h`: 2つの可変ドラムmapを所有する。preset復元、GS41、bulk49、
  NRPNを同じ所有者へ渡し、protocol recordと発音用lookupを常に一緒に更新する。
  発音・出力制御はmapとKeyOutputを読み、recordのbyte配置を解釈しない。
  パートごとの受信拒否／解決済みprogramと共有program latchはコントローラー側に残す。
- `sc55_voice_engine.h` / `sc55_voice_runtime.h`: ボイス所有、割当・停止・再利用、周期更新。
  開始準備／停止後処理の保留状態は`VoiceOperation`型で所有し、kernelのtask番号と
  区別する。PCM再利用判定には操作の有無だけを渡し、H8のCAF4 byteを渡さない。
- EG／LFO／pitch／filter／levelの計算は通常経路でC++を直接実行する。
  `serviceControl(...ControlSlice::pass)` が製品入口。H8命令時間を消費しない。
  H8命令数による途中結果保持／待機カウンタ／timedPhaseは通常ビルドから除外。
  `SC55_CONTROL_TIMING_ORACLE`を対象全体に定義する2つの比較ツールだけが持つ。
  通常のVoiceControlRuntimeにはPendingCalculation/PendingModulationもなく、
  amplitude/filter/pitch/outputのreference-workヘッダも読み込まない。
  実PCMの開始・再利用待ちと意味的なphase更新は変更していない。
  `voice-control-pcm`／`native-player`の比較ツール試験は両方PASS
  (`/tmp/sc55-control-oracle-test.log`)。これは新しい機能移植やCPU改善の測定ではなく、
  製品に診断用のH8待機経路を混入させないための構造変更。
  通常設定のRelease arm64 Standalone＋内蔵AUv3がBUILD SUCCEEDED
  (`/tmp/sc55-control-oracle-release.log`)。NukedSC55Emulator.dにも4種の
  parameter-work／modulation-calculationヘッダは含まれていない。
- 既存PCMは発振・filter・mix・rampとハードウェア側の開始／再利用状態を所有する。
  ここを残すことは未達成理由ではない。
- UIはパネルキューで操作を渡し、atomic snapshotを表示する。音源状態はUIが所有しない。
- `sc55_display_events.h`: 音声ownerは表示データの受信／解除／速度変更と時刻だけを
  固定容量履歴へ記録。`sc55_display.h`の`DisplayPresentation`はmessage-thread owner。
  表示timer、scroll、文字整形、expiryを公開snapshotから計算し、音声renderを呼ばない。

以上は構造の確認であり、全機能の互換性を証明したものではない。

## 次に閉じる責務（順序）

0. **表示処理のスレッド境界（ユーザー明示制約）**。Native経路から表示timerと
   scroll更新、および表示期限によるrender分割を削除済み。製品の表示処理入口は
   PluginEditorのtimer/refreshDisplay → copyLcdDisplay → captureNativeState。
   音声ownerはMIDI受信と音源操作に伴う表示event記録、snapshot公開だけを行う。
   soloに必要な選択partと音源設定の適用は引き続き音声owner。UI widgetは参照しない。
   表示ownerはevent時刻順に進め、paint回数には依存しない。新規音源instanceも識別。
   event履歴は64件。UIがそれ以上取りこぼした場合、最新text/bitmapを元の受信時刻で
   復元する。過去の失われたscroll countdown位相までは復元しない（resync回数を保持）。
   音声を待たせず、cancel済みの古い表示は復活させず、表示期限を開き直し時に延長しない。
   これはoverflow時の明示的な縮退であり、その場合のH8完全一致は主張しない。
   CMake製品emulator翻訳単位build成功、`--native-display-control` exit0
   (`/tmp/sc55-display-owner-test.log`)。8texts/801scroll/2bitmapのH8比較、
   18button cancel、遅いUI/再表示/同一text再受信/history overflow/音源交換、
   zero/1/257分割がPASS。24音、41114非ゼロframeで表示SysExあり/なしの音声が一致。
   `--native-synth`で並行snapshot交換とzero/1/127/129/257音声分割もPASS
   (`/tmp/sc55-display-owner-synth.log`)。Release/arm64 Standalone＋内蔵AUv3の
   Xcode BUILD SUCCEEDED (`/tmp/sc55-display-owner-release.log`)。
   Resave・インストールなし。Logic実行・GUI操作・負荷差は未検証。
   H8参照版のLCDコードは今回移植していない。
1. **受信設定の機能範囲**。`receivePartSettings`、`receiveController`、モデル42/45の
   受信処理から、H8が受理するのに製品で意味を持たない設定を特定する。
   `unsupportedEvents`は未対応機能の件数ではない。ROM表を持たない診断用構築、
   非対応モデル／アドレス、割当結果も同じカウンタに入るので区別する。
   次の作業はこの入口の棚卸しから、実際の不足を一つの設定責務として接続する。
2. **パネル／表示モード**。通常値変更・mute・soloは接続済み。
   通常モデル45文字／bitmapも接続済み。残りは別表示モード、長押し・隠し操作と
   高速scrollの到達条件。`DisplayControl::service`のfastScroll引数だけを無条件に
   有効にしない。H8の通常入力からの到達を根拠にモードを実装する。
3. **未分類のCPUサービス**。task5/6入口0542が待機後1fe2へ分岐することは既知。
   同じ入口を再解析するのではなく、通知元と到達する機能を特定する。
   到達不能／不要とも、新機能ともまだ断定しない。
   通常timerはbit7通知なのでmask01待機を解除できないことを確認済み。
   割込みから通知handlerへの直接jmp6箇所も宛先0/2/4で、5/6宛てはない。
   根拠はCPU_EVENT_LIFECYCLEの追加確認。残るのは別経路のbit0書込み／到達性であり、
   架空の周期タスクをC++へ追加しない。
4. **製品での確認**。上記接続後、既存のmono欠落・ドラムattack・reserveの回帰と
   実際のプラグイン操作を確認する。headless一致だけでLogicの音・CPU改善を宣言しない。

## 再開しない作業

### Rx Remoteの入力元を区別

Rx Remote（ROM2 file4c22のCDFD、表示名4d8f）はMIDI受信gateではない。
ROM1 00:7eb6の外部割込み処理がF106を読み、要因2の場合だけ7ecaでCDFDを確認。
許可時にCCCF bit0を消し、FFE8=7bを設定。00:7ef6から04:2c87へ進む処理は
FFE0..FFE7をCCD0/CCD8へ取り込み、値と6000hの比較でCCCEを構成する。
通常UART/MIDI入口やProgram ChangeのCDF6 gateとは別の外部入力経路。

現在のbackendではF106は`ga_int_trigger`を返し、`MCU_GA_SetGAInt`の通常呼出しは
line1（PCM）、line3/4（UART）、line5（別機種）だけ。line2を供給する入口は見つからない。
従ってH8の通常MIDI比較でこの機能が動くと仮定したり、MIDIをremote commandとして
解釈する処理をNativeへ追加してはいけない。外部remote入力の完全再現を検証済みとは
しないが、通常MIDIの未接続音源機能とは分離する。task5/6の用途を示す根拠にもならない。

- H8命令時間／同一サンプルのPCM書込み時刻／capacityの同一survivorを目的にした待ち追加。
  実際の欠音・attack欠け・reserve違反を示す場合だけ、関連する順序を調べる。
- MIDI出力、SysEx返信／dump送信UI。ユーザーが現時点の対象外と指定。
- サンプルごとのPCM互換同期の削減。ユーザー指示で後回し。

過去の診断と失敗結果は削除せず、未実装機能のリストと混同しない。
ソロ＋GS/GM resetの27比較は完了済み。同じ確認を繰り返す必要はない。

### MIDI送信除外を受信経路にも反映

送信無効でもRQ1のEOXがvoice transaction扱いで保留される抜けを修正。
3音のNote On直後にRQ1→DT1 volume87を送り1frame処理すると、修正前は
後続volumeが反映されず失敗した（`/tmp/sc55-rq1-disabled-before.log`）。
送信先も診断captureもないRQ1はEOXで消費し、発音準備の完了を待たない。
bulk読出しも送信先がなければ同じ方針。入力DT1/resetの処理は残す。
これはユーザー指定のno-output動作で、H8の送信待ち時間に合わせる試験ではない。
修正後、追加回帰と既存の明示接続TX比較がPASS
（`--native-parameter-transport`, `/tmp/sc55-rq1-disabled-after.log`）。
製品emulator翻訳単位を含むCMake build成功。今回のXcode/Logic検証は未実施。

## 受信設定の棚卸しで修正した抜け

### デバイス単位の受信設定（コア・UI・保存接続、一部H8比較は未完了）

#### Rx Inst Chgの未接続を修正

ROM2 04:0928..0934はCDF6が0ならMIDIによる音色変更を拒否する。
CDCC bit4を使う明示設定経路は別扱い。従来のNative receiveProgramにはこの
全体gateがなく、パート別受信maskしか確認していなかった。
`MidiInputSettings::receiveProgramChanges`を追加し、MIDI PC入口でのみgate。
パネル/GS DT1の直接音色指定は拒否しない。設定はPOWER右クリックの
Receive Program Changeから解決済みコマンドで渡し、保存・公開状態にも接続。
旧sessionとの互換性のため保存bit11は「無効」の意味とし、従来値は受信ON。

H8実ボタンでALL→PART左右同時押し→MUTE3回→INSTRUMENT左と操作し、
item3/CDF6=0を確認。melodic/drum PC拒否、DT1指定通過、再有効化後のPC受理が
NativeSynthと一致。512通りの保存形式と旧defaultもPASS
(`/tmp/sc55-program-gate-test.log`)。通常設定のRelease arm64 Standalone＋内蔵AUv3
がbuild成功 (`/tmp/sc55-program-gate-release.log`)。Logic操作は未検証。
製品adapterでUIコマンド→音声適用→受信拒否／再開→公開／保存bitも
44.1/48/96kHzでPASS (`/tmp/sc55-program-gate-adapter.log`)。

通常GSの141レコードとは別に、H8のac24/cdf8/cdf7/cdc8 bit9が
Device ID／exclusive受信／GM・GS Reset受信／checksum例外を所有する。
00:24d7はmanufacturer分岐より前にcdf8を確認するので、GMとmodel45も対象。
00:24f5はac24とdevice byteを照合。04:123dと1dacは共通のcdf7でGS/GM Resetをgate。
GS command値の保存はそれ以前の1121系なので、reset拒否はDT1全体の拒否ではない。
ROM2 file+4b76/4bc6/4bd6にはac24/cdf7/cdf8のパネル設定recordもある。
これらは通常GS bulk48設定の8000..8747には含まれない。

`MidiInputSettings`をNativeSynth/NativeMelodicPlayerの音声ownerへ接続した。
EOX previewと本受理の両方を同じ`decodeExclusive`へ渡し、reset受信禁止時は
不要なvoice-transaction barrierも要求しない。通常Note/CC、DT1の有効prefixは維持。
デフォルト設定は以前と同じ。UIからの変更は既存コマンドキューを経由する必要がある。
製品UI: ALL画面のMIDI CH左右でDevice IDを変更し、同じLCD欄にraw+1を表示。
H8のALL→MIDI CH操作でac24が16→17へ変わることを実測した。
Nativeの`applyMasterAdjustment(channel)`は未実装だったため接続。実H8ボタンと
Native公開panel操作で両端clamp（raw0..31）まで比較し、変更後の旧ID拒否／新ID受理も一致。
Reset/exclusive/checksumの3設定はPOWER右クリックの既存menuから変更できる。
UIは明示on/offを既存panel queueへ送り、音声ownerが設定を変更する。
menuのcheckは公開atomic flag、LCDのDevice IDはSynthState snapshotから読む。
POWER通常clickの設定画面は変更しない。H8参照modeではnative専用menuを無効化。
**未完了**: checksum変更の実H8パネル比較、
実GUI操作。Device ID以外の全パネル設定を再現できたとは扱わない。MIDI出力は追加しない。

Reset/exclusiveは物理入力による比較を完了。ROM2の27b0の組合せ表で
PART左右→event19を確認し、ALL画面→PART左右同時押しでpage2/mode1へ入った。
MUTEで項目を進め、item4（reset）とitem5（exclusive）をINSTRUMENT左右で変更。
H8のcdf7/cdf8が0/1へ変わることを実行で確認。RAM/PCは変更していない。
同じGS/GM/DT1/CCをNativeSynthへ送り、reset拒否、拒否中の先行設定commit、
exclusive拒否と通常CC通過、両設定を戻した後のGM resetを比較してPASS。
`--native-midi-input-settings`, `/tmp/sc55-input-gates-test.log`。
経路の観察は`--midi-input-panel-route`, `/tmp/sc55-input-panel-route.log`で再実行可能。
この比較では既存C++処理の修正は不要だった。製品コードは今回変更していない。

検証: CMake製品Emulator翻訳単位build成功。`--native-midi-input-settings` exit0
(`/tmp/sc55-midi-input-settings-test2.log`)。H8 boot既定値との比較と、NativeSynthの
fragmented MIDI入力で受信gate、普通のCC、reset付きprefix、device照合、checksum、
reset後のdevice設定保持を確認。非既定値でのH8動的比較ではない。
初回testはreset後masterを100と誤記し失敗したため、H8から読んだ既定値127を
期待値にした（part volumeの100とは別）。製品のreset処理はこの理由で変更していない。
`--native-synth`の並行snapshot交換とzero/1/127/129/257音声分割もPASS
(`/tmp/sc55-midi-input-settings-synth.log`)。この追加変更のXcode/Logicは未実行。

パネル接続後の検証: CMake製品翻訳単位buildと`--native-midi-input-settings` exit0
(`/tmp/sc55-midi-panel-final-test.log`)。H8 RAM/PCを変更せず物理button入力を使った。
増減・両端clamp、Device IDを変えたDT1受理、および以前の入力gate回帰がPASS。
Release/arm64 Standalone＋内蔵AUv3のXcode BUILD SUCCEEDED
(`/tmp/sc55-midi-input-ui-release.log`)。Resave・インストールなし。実GUI/Logicは未検証。

保存・復元: `nativeMidiInputV1` / `nativeMidiInputSecondaryV1`を既存state XMLへ追加。
Device IDと3フラグを1wordで保持し、各音源が反映済みの値を保存する。
入力キューの未実行UI操作まで保存する仕様ではない。旧stateでpropertyがない場合は
従来値（raw ID16、exclusive/reset許可、checksum検証）を使用。secondary未保存ならprimary。
未知bit、範囲外ID、負数、overflow、不正な数値表記は既定値へ戻す。
JUCE NamedValueSet::setFromXmlAttributesは属性を文字列へ復元するため、型判定で
整数だけを許可せず、decimal全体を検証する。APVTS既存IDやROMパス移行は変更しない。
`NativeMidiInputState`はrestore要求と音声ownerのpublishを単一atomicのbounded CASで
引き渡し、未処理restoreを古いpublishで上書きしない。ROM再初期化前後にも保持する。
UIはこの公開値を読む。音声callbackでValueTree/XML処理やUI読取りは行わない。
Native GS Resetボタンの宛先も現在のDevice IDへ変更（外来MIDIの判定は変更しない）。

`--native-midi-input-settings` exit0 (`/tmp/sc55-midi-state-final-test.log`)。
256通りのXML属性roundtrip、restore/publication順序、不正値、従来のH8 Device ID比較と
入力gateがPASS。これはcodec/handoffと音源入力の検証であり、Logicのsession再読込を
実行した証明ではない。設定を入れた直後、audioがまだ処理していないpanel commandとの
セッション復元競合は未検証。GS Resetボタンの実GUI操作も未実施。
GS Reset宛先変更を含む最終ソースのRelease/arm64 Standalone＋AUv3 BUILD SUCCEEDED
(`/tmp/sc55-midi-input-state-final-release.log`)。既定ID固定の旧ビルドを最終成果とは扱わない。
Resave・署名登録・インストールは実行していない。

### 未解釈設定の実読出しとGS命令の保存

`--configuration-readers`を追加し、H8の実際の設定readを命令入口と対応付けた。
製品へはコンパイルされず、診断targetでも`SC55_ORACLE_CONFIG_READS=ON`のときだけ
有効。CPU時間測定には使わない。診断側のreadbackは実行loop外なので記録しない。
初期146byteを対象にboot、Note On/Off、program48、全CC番号の127/0、GS resetを
流すと、8004を04:1138で読む1件だけを観測した
（`/tmp/sc55-configuration-readers.log`）。未観測145byteの未使用証明ではない。

8004はGS reset命令の保存値。04:1138で旧値と比較、113cで保存し、11b5以下は
zeroだけが123dのresetへ進む。非zeroでも値の保存は必要だった。
bulkで25hを書いた後にDT1 40:00:7f=16hを受信すると、H8は16h、nativeは25hのままで
不一致を再現（`/tmp/sc55-reset-command-before2.log`）。
MasterControls::resetCommandへ所有を移し、通常受信・bulk・boot/resetの復元を接続。
bulkや非zeroの保存だけではresetを要求しない。UninterpretedSystemSettingsは145byteへ。
既存の診断readbackで464packetがH8と一致し、非zeroでresetしないこともPASS
（`/tmp/sc55-reset-command-fixed.log`）。ホストMIDI出力は追加していない。
最後に観測hookが無効な通常の診断buildを再構築し、`--native-synth`の
zero/1/127/129/257frame分割一致を確認（`/tmp/sc55-config-owner-synth.log`）。
Xcode/Logicの再確認は未実施。

### ボイス操作の型とPCM再利用契約

`VoiceStopState::fieldCAF4`と`InstalledVoice::taskState`を、同じ
`VoiceOperation`（none/prepare/finishStop）型へ置換した。通常コードで整数を
代入できない。診断のROM read/writeと意図的な不正値fixtureだけで明示変換する。
PCM側は操作番号を解釈せず、別操作が保留中かというboolを受け取る。
priority、linked partial順序、停止後stage移行、開始key latch待ちは変更していない。
不正値を検出する既存assertionも削除していない。

製品emulatorを含むCMake build成功。`envelope-pcm`、`voice-control-pcm`、
`native-player`がPASS（`/tmp/sc55-voice-operation-tests.log`）。
`--native-synth`でもzero/1/127/129/257frame分割一致と非無音出力を確認
（`/tmp/sc55-voice-operation-synth.log`）。状態管理の型を明確にした変更であり、
性能改善を実証した変更ではない。Xcode/Logicの再確認は未実施。

### ドラム設定所有者の分離

NativeMelodicPlayerのrhythmRecords_/rhythmMaps_をRhythmSettingsへ移動した。
値の配置を変えたり旧stateへ毎回コピーするadapterは追加せず、所有する保存先そのものを
移した。GS/bulk/NRPNの更新とlookup更新を内部化し、name/KeyOutputを意味のある値で返す。
ROM presetは不変入力、共有mapは可変設定、パートのprogram/rejectionはルーティング状態と
して区別する。ボイスのadmission、PCM開始、EG処理は変更していない。

製品emulatorを含むCMake build成功。実MIDIでの共有map・拒否・program・mode切替の
H8比較42件がPASS（`/tmp/sc55-rhythm-owner-shared.log`）。
`native-player`と`native-rhythm-bulk`も4.72秒でPASS
（`/tmp/sc55-rhythm-owner-tests.log`）。NRPNのドラムlevel変更による実PCM出力、
共有mapへの参加後の編集保持、bulk受信を含む既存試験をそのまま使用した。
この分離後のXcode/Logic確認・CPU測定はまだ実施していない。

### 現在の製品コードによる実曲回帰（2026-09-10）

送信無効RQ1の修正を含む現在のソースで、ユーザーが報告した二つの症状を再確認した。

- `--song-part16 GATCHA55.MID`: 初期化後にH8の実ボタンでパート1〜15をmute。
  C++にも同じmute操作を渡し、18秒まで再生。パート16の8音目は両者とも
  sample start=cd940、先頭128frameのpeak gain=99090432でPASS。
  `/tmp/sc55-current-gatcha-regression.log`。
- `SC55_KICK_ALL=1 --song-first-kick 55KTIZKE.MID`: 他パートも鳴らして60秒再生。
  24voiceを使い切った観測点1115、Note On2114件。キック13回すべてで
  開始後32frameのgainがH8と一致しPASS。`/tmp/sc55-current-drum-regression.log`。

これらは既存の症状を確認する回帰で、波形全体／全Note On／全ドラムの保証ではない。
GATCHA55の総key-on数87/84、55KTIZKEの2100/2114など、全体の観測差は残る。
同一時刻の割当差を理由にH8命令時間を復活させず、機能欠落を示す場合に絞って調べる。
両試験とも製品MIDI decoder/controller/PCMを通るが、JUCEホストや実GUIは通らない。

同じソースでXcode Release/arm64のStandaloneと内蔵AUv3をbuildし、
ValidateEmbeddedBinaryを含めBUILD SUCCEEDEDを確認
（`/tmp/sc55-current-native-release.log`）。出力は
`/tmp/sc55-native-product-validation/Products/Release/SC-55.app`。
Resave、署名、登録、インストールは実行せず、Logic実行とCPU測定は未実施。

システムページ40:01:20からの連続scalar書込み。ROM表d726はstart-part20の次が
reverb macro30だが、製品は先頭だけを反映して後続を捨てていた。
実SysEx `{40,01,20,5,2,4,5,87}` に対しH8の802dは87、nativeは64で失敗を確認。
設定値のsuffixを既存EffectsSettingsへspanで渡し、同じeffect request経路へ接続した。
パケット再生成・heap確保・H8待ち時間追加はない。
`--native-panel-settings`は追加した受信経路と既存16part/ALL操作の42160比較がPASS。
失敗ログ `/tmp/sc55-system-range-test.log`、修正後 `/tmp/sc55-system-range-fixed.log`。
全SysExアドレス・全連続長の網羅ではなく、Xcode/Logicの再確認も未実施。

続いてmasterページの連続書込み途中のGS resetを修正。
`{40,00,7e,23,0}`でH8はresetするがnativeは末尾7fを未対応として捨てていた。
変更済みreverb音量がH8=64/native=87となる失敗を実MIDIで確認。
MasterControlsの解析結果にresetRequestedを追加し、通常のreset要求へ接続した。
受信前の発音処理完了待ち判定も同じ純粋なparserを使うため、先頭アドレスに
依存した副作用の見落としを避ける。前方の設定反映とreset要求を分けて扱う。
修正後`--native-panel-settings`の42305比較がPASS。scalarのprefix保持、
途中で終わるpacket、長さ不正のtune、単独resetは`sc55-sysex-test`でPASS。
ログ `/tmp/sc55-master-range-test.log`、`/tmp/sc55-master-range-fixed.log`。

これらを含むシステム設定をSystemSettingsへ集約。旧4フィールドへの参照は
所有先を切替え、受信dispatchから名前／reserve／effectの個別解析を除去した。
既存の製品MIDI/render/snapshotを通る42305比較はそのままPASS
(`/tmp/sc55-system-module-test.log`)。診断のための新しい転送層は追加していない。
Xcode Release/arm64 Standalone・内蔵AUv3のBUILD SUCCEEDEDを確認
(`/tmp/sc55-system-module-release.log`)。署名／インストール／Logic検証はしていない。

## 高速scrollの到達条件（実ボタンで確認）

`--panel-options`はH8 RAMを書き換えず、button_pressedのみで次を確認する。
起動page1/options0000 → POWER保持でpage0 → POWER+ALL+INSTRUMENT右で0008 →
release → POWER押下でpage1/options0008。再びPOWERでpage0へ入り、
POWER+MUTE+INSTRUMENT右でbit3をclear、再度オンにしても0000を保持する。
ログ `/tmp/sc55-panel-options.log`。set/clear後のpageとbitをassertしてexit0。
ROM table34f0のpage0先頭33e8、bit番号表33d0のbutton4=3、handler495dで
POWER確認後MUTEならclear／ALLならset。38c4がこのbitをscroll間隔20/30へ使う。
通常パネルクリックを高速scrollへ転用する仕様ではない。native側はまだ未接続。
必要なのはオフ状態の設定操作とオンへの復帰を持つパネル制御であり、
fastScrollを単に常時trueにする変更ではない。プラグインの既存POWERは設定画面を
開く操作なので、その動作を無断で電源操作へ置換しない。

### NativeSynthへの接続

後続確認で、H8はstandby移行時に発音を停止し、standby中のNote On/CC7を破棄。
復帰時にも破棄入力を再生せず、新規Note Onから受信再開することを確認した。
`NativeSynth::setStandby`を接続。受信queue/decoder/active-sensingをclearし、
先行admission完了後に既存controller reset/group stopを使う。PCMの停止rampは維持。
`setFastDisplayScroll`はstandby中だけ変更を受理し、復帰後のDisplayControlへ渡す。
`--panel-options`は実H8ボタン対NativeSynth操作でmode/bit/voice数/CC7が一致してexit0
(`/tmp/sc55-native-standby-options.log`)。音色program80、channel1、257frame分割。
同時のMUTE+INSTRUMENT押下はH8 scan順で取りこぼす場合があったため、
MUTEを保持してからINSTRUMENTを押す明確な物理手順にした。H8 RAMは変更していない。
他のstandby option、途中admission/複雑なペダル状態での切替、表示窓そのものの
高速scroll比較は未確認。GUIへの電源操作の追加はしていない。
製品emulator翻訳単位を含むCMake buildは成功、最新変更のXcode buildは未実施。

standbyの追加回帰: sustain有効＋32 Note Onを送り、0/1/8/127/257/4096frame後に
standbyへ移行する6ケースをNativeSynthの公開操作で確認。発音所有の解放、
standby中CC破棄、復帰後の新規発音とNote Off後の解放がPASS。
ログ `/tmp/sc55-standby-stress-final.log`。この6ケースは製品状態の回帰であり、
H8との同一サンプル比較ではない。sostenutoや全ペダル組合せを網羅していない。
初回の失敗はPCM enable maskを発音所有数と取り違えたtest側の誤りだった。
delay1でmask00ffffff/voices0を観測。PCMを余分にresetせず、所有数と復帰・releaseを
判定するよう修正した。音源動作の修正は不要。以前のマスク失敗ログも保存する。

### デスクトップGUIからの操作

POWERを通常クリックすると従来どおり設定画面を開く。右クリックでは
StandbyとFast display scrollのmenuを開く。後者はstandby中だけ選べる。
メニューはnative engineのready時だけ有効。既存FrontPanelキューへ明示on/offを送り、
音声ownerで反映する。H8参照モードに架空のボタンパルスは送らない。
standby中の通常パネル編集はqueue入口の消費側で破棄し、保留キューを埋めて
後続のstandbyOffを遮らない。UIはatomic状態からLEDとチェックマークを描画する。
非同期menu callbackはSafePointerでeditor破棄を扱う。APVTS IDや保存形式は変更なし。
設定のセッション保存・タッチでのメニュー操作は追加していない。
最終ソースでRelease/arm64 Standalone・内蔵AUv3のBUILD SUCCEEDEDを確認
(`/tmp/sc55-standby-menu-final-release.log`)。実際の右クリック操作、Logic、
メニュー表示中のeditor破棄は実行検証していない。Resave・インストールなし。
