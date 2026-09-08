# ボイス更新のEG呼び出し区間

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryStepVoiceService`で32f0..3362を1命令ずつC++実行する。
5区間それぞれのマスク解除、4つのNOP、マスク再設定、voice状態14との比較、
終了分岐、3a7a/33f4/4443/4fdb/36dbへのBSR、および最後のマスク解除/RTSを扱う。
割り込みを処理できるNOP区間をまとめたり省略したりしない。
SR更新は既存ControlRegisterWriteとex_ignore、スタックは既存ヘルパーを利用。
各命令の状態・12cycle境界を保ち、debtは追加しない。
v1.21/CP・DP=0/正規voice/traceなしを要求。RT確保・ロック・UI呼出しなし。

## 検証

- 47命令×384ケースでPC/SR/レジスタ/SRAM/ex_ignoreがH8と一致。
  マスク0..7、SR下位16通り、状態13/14/15と符号境界を含む。
- 379,676ステレオフレームがビット一致。
- Mac Release Shared Code（署名なし）ビルド成功。
- 実再生H8 fallbackは4,497,810→4,301,519、196,291命令減少。
  ピッチ領域fallback=0を維持。
- Release oracle `SC55_TVA_PROFILE=1 ctest -R '^native-tva$' -V`。
- 5区間への拡張とリリース準備追加後はfallback 4,238,291。
  上記4,301,519は4区間の時点の記録。ピッチ領域0と音声一致を維持。

今回はCPU時間比較を行っていない。ホスト内・Windows/Linux・全音色は未検証。
呼び出し先には部分C++化とH8処理が混在する。ボイス更新全体や完全置換の完了ではない。
