# 終了ボイスのリンク解除

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryUnlinkFinishedVoice`で3393..33c9の管理テーブル更新をC++化。
AC42のactivityクリア、voice状態22、CAC4/CADCの関連ボイス選択・両側リンク解除。
第1リンク優先、第2リンクfallback、リンクなしの3経路を扱う。
対象indexと選択partnerは24voice以内（または未接続255）でなければ状態不変で拒否。
正規v1.21 voice、CP/DP/EP=0、traceなし、割り込みマスク7を要求。
PCMレジスタ操作が始まる33c9で止め、6/10/13命令の時間を保持する。
RT確保・ロック・UI呼出しなし。

既存ASMは3397のMOV.W即値8bitの長さを誤認していたため、実ROMとデコーダで
339b/33a0/33a2/33a7/33a9/33ad/33afの境界を確認した。

## 検証

24対象×25第1リンク×25第2リンク=15,000ケースで、自己リンク・同一partner・
未接続を含めPC/SR/全レジスタ/SRAM/命令数がH8と一致。
起動・24音・controller・portamento・Note Offの379,676フレームもビット一致。
Release oracle `SC55_TVA_PROFILE=1 ctest -R '^native-tva$' -V`。
実再生H8 fallbackは4,497,966→4,497,894（72命令減）。
Mac Release Shared Code（署名なし）のビルド成功。

今回のCPU時間比較、ホスト内・Windows/Linux・全音色は未検証。
PCM停止指示・タスク通知・非マスク経路などは残り、完全置換は未完了。
